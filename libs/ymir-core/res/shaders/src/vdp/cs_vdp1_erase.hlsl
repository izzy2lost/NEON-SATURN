#include "vdp1_defs.hlsli"
#include "vdp1_common_params.hlsli"
#include "vdp1_erase_params.hlsli"

#include "util/bit_ops.hlsli"

#ifdef __spirv__
// Vulkan receives these parameters as push constants rather than root constants. The C++ side pads
// the common parameters to 16 bytes like a cbuffer would, hence the explicit offset.
struct RenderParamsPC {
    CommonRenderParams commonParams;
    [[vk::offset(16)]] EraseParams eraseParams;
};
[[vk::push_constant]] RenderParamsPC g_renderParamsPC;
    #define g_commonParams g_renderParamsPC.commonParams
    #define g_eraseParams g_renderParamsPC.eraseParams
#else
cbuffer CommonRenderParamsBuffer : register(b0) {
    CommonRenderParams g_commonParams;
    EraseParams g_eraseParams;
}
#endif

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

static const bool vblankErase = BitTest(g_eraseParams.vblank, 0);
static const uint vblankEraseMaxY = BitExtract(g_eraseParams.vblank, 1, 9);
static const uint vblankEraseMaxX = BitExtract(g_eraseParams.vblank, 10, 10);
static const uint addressShift = BitExtract(g_eraseParams.erase, 16, 1) + 8u;
static const uint eraseScaleV = BitExtract(g_eraseParams.coords, 31, 1);
static const uint eraseX1 = BitExtract(g_eraseParams.coords, 0, 6) << 3u;
static const uint eraseY1 = BitExtract(g_eraseParams.coords, 6, 9) << eraseScaleV;
static const uint eraseX3 = BitExtract(g_eraseParams.coords, 15, 7) << 3u;
static const uint eraseY3 = BitExtract(g_eraseParams.coords, 22, 9) << eraseScaleV;

// ---------------------------------------------------------------------------------------------------------------------
// Entrypoint

// Erases a scaled framebuffer. Each thread handles one 32-bit word of the scaled framebuffer, mapping every byte back
// to its native address to apply the exact same bounds as the native erase process below.
void EraseScaled(uint wordIndex) {
    const uint scaledLineSize = fbSize.x * pixelSize * kScale;
    const uint nativeLineSize = fbSize.x * pixelSize;
    const uint scaledAddress = wordIndex * 4u;
    if (scaledAddress >= kScaledFBSize) {
        return;
    }

    const uint writeValue = BitExtract(g_eraseParams.erase, 0, 16);
    uint eraseMask = 0;
    uint eraseValue = 0;
    for (uint i = 0; i < 4; ++i) {
        const uint scaledByte = scaledAddress + i;
        const uint nativeLine = (scaledByte / scaledLineSize) / kScale;
        const uint nativeByte =
            nativeLine * nativeLineSize + UnscaleFBLineByteOffset(scaledByte % scaledLineSize, pixelSize, kScale);

        // Erase coordinates address 16-bit words
        const uint word = nativeByte >> 1u;
        const uint x = word & ((1u << addressShift) - 1u);
        const uint y = word >> addressShift;

        // The native process writes 32-bit pairs of words starting at the (even) X1 coordinate, so X3 + 1 is included
        if (x < eraseX1 || x > eraseX3 + 1 || y < eraseY1 || y > eraseY3 + 1) {
            continue;
        }
        if (vblankErase && (y > vblankEraseMaxY || (y == vblankEraseMaxY && x > vblankEraseMaxX))) {
            continue;
        }
        eraseMask |= 0xFFu << (i * 8u);
        eraseValue |= ((writeValue >> ((nativeByte & 1u) * 8u)) & 0xFFu) << (i * 8u);
    }
    if (eraseMask == 0) {
        return;
    }

    const uint address = drawFB * kScaledFBSize + scaledAddress;
    const uint value = (g_fbramOut.Load(address) & ~eraseMask) | eraseValue;
    g_fbramOut.Store(address, value);
    if (transparentMeshes) {
        g_fbramOut.Store(address + kScaledFBSize * 4, g_fbramOut.Load(address + kScaledFBSize * 4) & ~eraseMask);
    }
    if (deinterlace && doubleDensity) {
        g_fbramOut.Store(address + kScaledFBSize * 2, value);
        if (transparentMeshes) {
            g_fbramOut.Store(address + kScaledFBSize * 6, g_fbramOut.Load(address + kScaledFBSize * 6) & ~eraseMask);
        }
    }
}

[numthreads(32, 32, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 groupID : SV_GroupID, uint groupIndex : SV_GroupIndex) {
    if (kScale > 1) {
        // Dispatched as a linear list of 1024-word groups covering the entire scaled framebuffer
        EraseScaled(groupID.x * 1024u + groupIndex);
        return;
    }

    // VDP1 erase process writes 16-bit words, but this shader works on 32-bit units to avoid interlocked writes.
    const uint2 pos = uint2(id.x * 2 + eraseX1, id.y + eraseY1);

    // Bail out if out of range
    if (pos.x >= eraseX3 + 1 || pos.y > eraseY3 + 1) {
        return;
    }
    // Bail out if pixel exceeds VBlank erase cycle limit
    bool partialWrite = false;
    if (vblankErase) {
        if (pos.y > vblankEraseMaxY) {
            return;
        }
        if (pos.y == vblankEraseMaxY && pos.x > vblankEraseMaxX) {
            return;
        }
        // A partial write occurs if the last VBlank erase occurs at an odd X coordinate.
        // This means we have to write only the first 16-bit word and leave the second word untouched.
        partialWrite = pos.y == vblankEraseMaxY && pos.x + 1 > vblankEraseMaxX;
    }

    const uint address = drawFBOffset + ((pos.y << addressShift) + pos.x) * 2;
    const uint writeValue = BitExtract(g_eraseParams.erase, 0, 16);
    uint value;
    if (partialWrite) {
        value = g_fbramOut.Load(address);
        value &= ~0xFFFF;
        value |= writeValue;
    } else {
        value = (writeValue << 16u) | writeValue;

    }
    g_fbramOut.Store(address, value);
    if (transparentMeshes) {
        g_fbramOut.Store(address + kVDP1FBRAMSize * 2, 0);
    }
    if (deinterlace && doubleDensity) {
        g_fbramOut.Store(address + kVDP1FBRAMSize, value);
        if (transparentMeshes) {
            g_fbramOut.Store(address + kVDP1FBRAMSize * 3, 0);
        }
    }
}
