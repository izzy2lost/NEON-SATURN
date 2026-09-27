#include "vdp1_defs.hlsli"
#include "vdp1_common_params.hlsli"
#include "vdp1_write_params.hlsli"

#include "util/bit_ops.hlsli"

struct FBRAMWriteParams {
    uint writeCount;
};

#ifdef __spirv__
// Vulkan receives these parameters as push constants rather than root constants. The C++ side pads
// the common parameters to 16 bytes like a cbuffer would, hence the explicit offset.
struct RenderParamsPC {
    CommonRenderParams commonParams;
    [[vk::offset(16)]] FBRAMWriteParams writeParams;
};
[[vk::push_constant]] RenderParamsPC g_renderParamsPC;
    #define g_commonParams g_renderParamsPC.commonParams
    #define g_writeParams g_renderParamsPC.writeParams
#else
cbuffer CommonRenderParamsBuffer : register(b0) {
    CommonRenderParams g_commonParams;
    FBRAMWriteParams g_writeParams;
}
#endif

StructuredBuffer<FBRAMWrite> g_fbramWrites : register(t1);

RWByteAddressBuffer g_fbramOut : register(u1);

// ---------------------------------------------------------------------------------------------------------------------
// Parameters

static const bool doubleDensity = BitTest(g_commonParams.displayParams, 3);
static const uint drawFB = BitExtract(g_commonParams.displayParams, 7, 1);
static const uint drawFBOffset = drawFB * kVDP1FBSize;

static const bool deinterlace = BitTest(g_commonParams.enhancements, 0);
static const bool transparentMeshes = BitTest(g_commonParams.enhancements, 1);
static const uint kScale = DecodeResolutionScale(g_commonParams.enhancements);

// Framebuffer geometry, used to map native addresses into scaled framebuffers
static const uint2 fbSize = uint2(
    512u << BitExtract(g_commonParams.displayParams, 0, 1),
    256u << BitExtract(g_commonParams.displayParams, 1, 1)
);
static const uint pixelSize = BitTest(g_commonParams.displayParams, 2) ? 1u : 2u;
static const uint kScaledFBSize = kVDP1FBSize * kScale * kScale;

// Writes a byte into a scaled framebuffer. Neighboring threads may write other bytes of the same word.
void StoreByteAtomic(uint address, uint value) {
    const uint shift = (address & 3u) * 8u;
    uint dummy;
    g_fbramOut.InterlockedAnd(address & ~3u, ~(0xFFu << shift), dummy);
    g_fbramOut.InterlockedOr(address & ~3u, value << shift, dummy);
}

// Clears a byte in a scaled framebuffer.
void ClearByteAtomic(uint address) {
    uint dummy;
    g_fbramOut.InterlockedAnd(address & ~3u, ~(0xFFu << ((address & 3u) * 8u)), dummy);
}

// Replicates a CPU write into every pixel of the corresponding blocks of the scaled framebuffers.
void WriteScaled(FBRAMWrite write) {
    const uint nativeLineSize = fbSize.x * pixelSize;
    const uint scaledLineSize = nativeLineSize * kScale;
    for (uint i = 0; i < 4; ++i) {
        if (BitExtract(write.andMask, i * 8u, 8) != 0) {
            // Byte not written
            continue;
        }
        const uint nativeByte = write.address + i;
        const uint fbLine = nativeByte / nativeLineSize;
        const uint lineOffset = ScaleFBLineByteOffset(nativeByte % nativeLineSize, pixelSize, kScale);
        const uint value = BitExtract(write.orMask, i * 8u, 8);
        for (uint dy = 0; dy < kScale; ++dy) {
            for (uint dx = 0; dx < kScale; ++dx) {
                const uint address =
                    drawFB * kScaledFBSize + (fbLine * kScale + dy) * scaledLineSize + lineOffset + dx * pixelSize;
                StoreByteAtomic(address, value);
                if (transparentMeshes) {
                    ClearByteAtomic(address + kScaledFBSize * 4);
                }
                if (deinterlace && doubleDensity) {
                    StoreByteAtomic(address + kScaledFBSize * 2, value);
                    if (transparentMeshes) {
                        ClearByteAtomic(address + kScaledFBSize * 6);
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Entrypoint

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const uint index = id.x;
    if (index >= g_writeParams.writeCount) {
        return;
    }

    const FBRAMWrite write = g_fbramWrites[index];
    if (kScale > 1) {
        WriteScaled(write);
        return;
    }

    // The address is relative to the start of the current CPU-visible framebuffer
    const uint address = write.address + drawFBOffset;
    uint value = g_fbramOut.Load(address);
    value &= write.andMask;
    value |= write.orMask;

    // Write updated value to FBRAM.
    // Clear corresponding pixels in the mesh buffers.
    // Replicate to the deinterlace buffers.
    g_fbramOut.Store(address, value);
    if (transparentMeshes) {
        uint meshValue = g_fbramOut.Load(address + kVDP1FBRAMSize * 2);
        meshValue &= write.andMask;
        g_fbramOut.Store(address + kVDP1FBRAMSize * 2, meshValue);
    }
    if (deinterlace && doubleDensity) {
        g_fbramOut.Store(address + kVDP1FBRAMSize, value);
        if (transparentMeshes) {
            uint meshValue = g_fbramOut.Load(address + kVDP1FBRAMSize * 3);
            meshValue &= write.andMask;
            g_fbramOut.Store(address + kVDP1FBRAMSize * 3, meshValue);
        }
    }
}
