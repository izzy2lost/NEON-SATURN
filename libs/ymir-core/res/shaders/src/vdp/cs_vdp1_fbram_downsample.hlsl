#include "vdp1_defs.hlsli"
#include "vdp1_common_params.hlsli"

#include "util/bit_ops.hlsli"

// Converts the scaled VDP1 framebuffers into native FBRAM contents by sampling the top-left pixel of every block.
// Used with internal resolution scaling to provide the CPU (and save states) with native framebuffer data.

#ifdef __spirv__
// Vulkan receives these parameters as push constants rather than root constants.
struct RenderParamsPC {
    CommonRenderParams commonParams;
};
[[vk::push_constant]] RenderParamsPC g_renderParamsPC;
    #define g_commonParams g_renderParamsPC.commonParams
#else
cbuffer CommonRenderParamsBuffer : register(b0) {
    CommonRenderParams g_commonParams;
}
#endif

ByteAddressBuffer g_scaledFBRAM : register(t1);

RWByteAddressBuffer g_nativeFBRAMOut : register(u1);

// ---------------------------------------------------------------------------------------------------------------------
// Parameters

static const uint2 fbSize = uint2(
    512u << BitExtract(g_commonParams.displayParams, 0, 1),
    256u << BitExtract(g_commonParams.displayParams, 1, 1)
);
static const uint pixelSize = BitTest(g_commonParams.displayParams, 2) ? 1u : 2u;
static const uint kScale = DecodeResolutionScale(g_commonParams.enhancements);
static const uint kScaledFBSize = kVDP1FBSize * kScale * kScale;

// ---------------------------------------------------------------------------------------------------------------------
// Entrypoint

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const uint nativeAddress = id.x * 4u;
    if (nativeAddress >= kVDP1FBRAMSize) {
        return;
    }

    const uint nativeLineSize = fbSize.x * pixelSize;
    const uint scaledLineSize = nativeLineSize * kScale;

    uint value = 0;
    for (uint i = 0; i < 4; ++i) {
        const uint nativeByte = nativeAddress + i;
        const uint fb = nativeByte / kVDP1FBSize;
        const uint fbByte = nativeByte % kVDP1FBSize;
        const uint fbLine = fbByte / nativeLineSize;
        const uint scaledAddress = fb * kScaledFBSize + fbLine * kScale * scaledLineSize +
                                   ScaleFBLineByteOffset(fbByte % nativeLineSize, pixelSize, kScale);
        const uint byte = BitExtract(g_scaledFBRAM.Load(scaledAddress & ~3u), (scaledAddress & 3u) * 8u, 8);
        value |= byte << (i * 8u);
    }
    g_nativeFBRAMOut.Store(nativeAddress, value);
}
