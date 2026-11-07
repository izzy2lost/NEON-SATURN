#include "vdp1_defs.hlsli"
#include "vdp1_common_params.hlsli"

#include "util/bit_ops.hlsli"
#include "util/data_ops.hlsli"

// Shader specialization macros:
// - POLYSPEC_TRANSPARENT_MESH:
//     0 = disabled (checkerboard)
//     1 = enabled (50% alpha); processes both normal and mesh pixels simultaneously
// - POLYSPEC_MERGE_MODE:
//     0 = Copy (Replace, Half-Luminance)
//     1 = Right-shift (Shadow)
//     2 = OIT (Half-Transparency)
//
// Implementation notes:
// - Works on 32-bit units at a time
// - The Z ID selects the field to draw for deinterlaced rendering (0=main, 1=alternate)

#define POLYSPEC_SHADING_MODE_COPY  0
#define POLYSPEC_SHADING_MODE_SHIFT 1
#define POLYSPEC_SHADING_MODE_OIT   2

// Modify these to adjust IntelliSense highlighting
#ifdef __INTELLISENSE__
#define POLYSPEC_TRANSPARENT_MESH 1
#define POLYSPEC_MERGE_MODE       0
#endif

// TODO: figure out how shadow and transparent pixels on the transparent mesh layer should be rendered
// - also broken on the software renderer

#ifdef __spirv__
// Vulkan receives these parameters as push constants rather than root constants.
struct RenderParamsPC {
    CommonRenderParams commonParams;
};
[[vk::push_constant]] RenderParamsPC g_renderParamsPC;
    #define g_commonParams g_renderParamsPC.commonParams
#else
cbuffer RenderParamsBuffer : register(b0) {
    CommonRenderParams g_commonParams;
}
#endif

#if POLYSPEC_MERGE_MODE == POLYSPEC_SHADING_MODE_OIT
StructuredBuffer<OITFragment> g_fragments : register(t1);
RWByteAddressBuffer g_fbramOut : register(u1);
RWBuffer<uint> g_listHeads : register(u2);
#else
RWByteAddressBuffer g_fbramOut : register(u1);
RWBuffer<uint> g_internalSpriteOut : register(u2);
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Parameters

static const uint kScale = DecodeResolutionScale(g_commonParams.enhancements);

// Framebuffer dimensions, scaled by the internal resolution
static const uint2 fbSize = uint2(
    512u << BitExtract(g_commonParams.displayParams, 0, 1),
    256u << BitExtract(g_commonParams.displayParams, 1, 1)
) * kScale;
static const bool pixel8Bits = BitTest(g_commonParams.displayParams, 2);
static const uint drawFB = BitExtract(g_commonParams.displayParams, 7, 1);

static const uint kScaledFBSize = kVDP1FBSize * kScale * kScale;
static const uint kScaledFBRAMSize = kScaledFBSize * 2;
static const uint fbOffset = drawFB * kScaledFBSize;

// ---------------------------------------------------------------------------------------------------------------------
// Mergers

#if POLYSPEC_MERGE_MODE == POLYSPEC_SHADING_MODE_COPY
// ----------------------------------------------------------------------------
// Copy (Replace, Half-Luminance)

void Merge8(uint2 pos, uint field) {
    const uint2 inPos = uint2(pos.x * 4, pos.y);
    const uint inOffset = inPos.x + inPos.y * fbSize.x + field * fbSize.x * fbSize.y;

    // Read internal outputs
    const uint out0 = g_internalSpriteOut[inOffset + 0];
    const uint out1 = g_internalSpriteOut[inOffset + 1];
    const uint out2 = g_internalSpriteOut[inOffset + 2];
    const uint out3 = g_internalSpriteOut[inOffset + 3];

    // Get sequence numbers
    const uint seqNum0 = BitExtract(out0, 16, 16);
    const uint seqNum1 = BitExtract(out1, 16, 16);
    const uint seqNum2 = BitExtract(out2, 16, 16);
    const uint seqNum3 = BitExtract(out3, 16, 16);
    bool hasPixel0 = seqNum0 != 0;
    bool hasPixel1 = seqNum1 != 0;
    bool hasPixel2 = seqNum2 != 0;
    bool hasPixel3 = seqNum3 != 0;
#if POLYSPEC_TRANSPARENT_MESH
    const uint inMeshOffset = inOffset + fbSize.x * fbSize.y * 2;
    const uint meshOut0 = g_internalSpriteOut[inMeshOffset + 0];
    const uint meshOut1 = g_internalSpriteOut[inMeshOffset + 1];
    const uint meshOut2 = g_internalSpriteOut[inMeshOffset + 2];
    const uint meshOut3 = g_internalSpriteOut[inMeshOffset + 3];
    const uint meshSeqNum0 = BitExtract(meshOut0, 16, 16);
    const uint meshSeqNum1 = BitExtract(meshOut1, 16, 16);
    const uint meshSeqNum2 = BitExtract(meshOut2, 16, 16);
    const uint meshSeqNum3 = BitExtract(meshOut3, 16, 16);
    hasPixel0 = hasPixel0 || meshSeqNum0 != 0;
    hasPixel1 = hasPixel1 || meshSeqNum1 != 0;
    hasPixel2 = hasPixel2 || meshSeqNum2 != 0;
    hasPixel3 = hasPixel3 || meshSeqNum3 != 0;
#endif
    if (!hasPixel0 && !hasPixel1 && !hasPixel2 && !hasPixel3) {
        // Nothing written to these pixels
        return;
    }

    // Clear internal outputs
    g_internalSpriteOut[inOffset + 0] = 0;
    g_internalSpriteOut[inOffset + 1] = 0;
    g_internalSpriteOut[inOffset + 2] = 0;
    g_internalSpriteOut[inOffset + 3] = 0;
#if POLYSPEC_TRANSPARENT_MESH
    g_internalSpriteOut[inMeshOffset + 0] = 0;
    g_internalSpriteOut[inMeshOffset + 1] = 0;
    g_internalSpriteOut[inMeshOffset + 2] = 0;
    g_internalSpriteOut[inMeshOffset + 3] = 0;
#endif

    const uint outOffset = inPos.x + inPos.y * fbSize.x + field * kScaledFBRAMSize;
    uint fbramValue = g_fbramOut.Load(outOffset + fbOffset);
    if (seqNum0 != 0) {
        fbramValue &= ~0xFFu;
        fbramValue |= BitExtract(out0, 0, 8);
    }
    if (seqNum1 != 0) {
        fbramValue &= ~0xFF00u;
        fbramValue |= BitExtract(out1, 0, 8) << 8u;
    }
    if (seqNum2 != 0) {
        fbramValue &= ~0xFF0000u;
        fbramValue |= BitExtract(out2, 0, 8) << 16u;
    }
    if (seqNum3 != 0) {
        fbramValue &= ~0xFF000000u;
        fbramValue |= BitExtract(out3, 0, 8) << 24u;
    }
    g_fbramOut.Store(outOffset + fbOffset, fbramValue);
#if POLYSPEC_TRANSPARENT_MESH
    const uint outMeshOffset = outOffset + kScaledFBRAMSize * 2;
    fbramValue = g_fbramOut.Load(outMeshOffset + fbOffset);
    if (hasPixel0) {
        fbramValue &= ~0xFFu;
        if (meshSeqNum0 > seqNum0) {
            fbramValue |= BitExtract(meshOut0, 0, 8);
        }
    }
    if (hasPixel1) {
        fbramValue &= ~0xFF00u;
        if (meshSeqNum1 > seqNum1) {
            fbramValue |= BitExtract(meshOut1, 0, 8) << 8u;
        }
    }
    if (hasPixel2) {
        fbramValue &= ~0xFF0000u;
        if (meshSeqNum2 > seqNum2) {
            fbramValue |= BitExtract(meshOut2, 0, 8) << 16u;
        }
    }
    if (hasPixel3) {
        fbramValue &= ~0xFF000000u;
        if (meshSeqNum3 > seqNum3) {
            fbramValue |= BitExtract(meshOut3, 0, 8) << 24u;
        }
    }
    g_fbramOut.Store(outMeshOffset + fbOffset, fbramValue);
#endif
}

void Merge16(uint2 pos, uint field) {
    const uint2 inPos = uint2(pos.x * 2, pos.y);
    const uint inOffset = inPos.x + inPos.y * fbSize.x + field * fbSize.x * fbSize.y;

    // Read internal outputs
    const uint out0 = g_internalSpriteOut[inOffset + 0];
    const uint out1 = g_internalSpriteOut[inOffset + 1];

    // Get sequence numbers
    const uint seqNum0 = BitExtract(out0, 16, 16);
    const uint seqNum1 = BitExtract(out1, 16, 16);
    bool hasPixel0 = seqNum0 != 0;
    bool hasPixel1 = seqNum1 != 0;
#if POLYSPEC_TRANSPARENT_MESH
    const uint inMeshOffset = inOffset + fbSize.x * fbSize.y * 2;
    const uint meshOut0 = g_internalSpriteOut[inMeshOffset + 0];
    const uint meshOut1 = g_internalSpriteOut[inMeshOffset + 1];
    const uint meshSeqNum0 = BitExtract(meshOut0, 16, 16);
    const uint meshSeqNum1 = BitExtract(meshOut1, 16, 16);
    hasPixel0 = hasPixel0 || meshSeqNum0 != 0;
    hasPixel1 = hasPixel1 || meshSeqNum1 != 0;
#endif
    if (!hasPixel0 && !hasPixel1) {
        // Nothing written to these pixels
        return;
    }

    // Clear internal outputs
    g_internalSpriteOut[inOffset + 0] = 0;
    g_internalSpriteOut[inOffset + 1] = 0;
#if POLYSPEC_TRANSPARENT_MESH
    g_internalSpriteOut[inMeshOffset + 0] = 0;
    g_internalSpriteOut[inMeshOffset + 1] = 0;
#endif

    const uint outOffset = (inPos.x + inPos.y * fbSize.x) * 2 + field * kScaledFBRAMSize;
    uint fbramValue = g_fbramOut.Load(outOffset + fbOffset);
    if (seqNum0 != 0) {
        fbramValue &= ~0xFFFFu;
        fbramValue |= ByteSwap16(out0);
    }
    if (seqNum1 != 0) {
        fbramValue &= ~0xFFFF0000u;
        fbramValue |= ByteSwap16(out1) << 16u;
    }
    g_fbramOut.Store(outOffset + fbOffset, fbramValue);
#if POLYSPEC_TRANSPARENT_MESH
    const uint outMeshOffset = outOffset + kScaledFBRAMSize * 2;
    fbramValue = g_fbramOut.Load(outMeshOffset + fbOffset);
    if (hasPixel0) {
        fbramValue &= ~0xFFFFu;
        if (meshSeqNum0 > seqNum0) {
            fbramValue |= ByteSwap16(meshOut0);
        }
    }
    if (hasPixel1) {
        fbramValue &= ~0xFFFF0000u;
        if (meshSeqNum1 > seqNum1) {
            fbramValue |= ByteSwap16(meshOut1) << 16u;
        }
    }
    g_fbramOut.Store(outMeshOffset + fbOffset, fbramValue);
#endif
}

#elif POLYSPEC_MERGE_MODE == POLYSPEC_SHADING_MODE_SHIFT
// ----------------------------------------------------------------------------
// Right-shift (Shadow)

void Merge16Single(uint2 pos, uint field) {
    const uint2 inPos = uint2(pos.x * 2, pos.y);
    const uint inOffset = inPos.x + inPos.y * fbSize.x + field * fbSize.x * fbSize.y;

    // Read and clear internal outputs
    const uint shift0 = min(g_internalSpriteOut[inOffset + 0], 5);
    const uint shift1 = min(g_internalSpriteOut[inOffset + 1], 5);
    if (shift0 == 0 && shift1 == 0) {
        // Nothing written to these pixels
        return;
    }
    g_internalSpriteOut[inOffset + 0] = 0;
    g_internalSpriteOut[inOffset + 1] = 0;

    const uint outOffset = (inPos.x + inPos.y * fbSize.x) * 2 + field * kScaledFBRAMSize;
    uint fbramValue = g_fbramOut.Load(outOffset + fbOffset);
    if (shift0 != 0) {
        uint4 color = Uint16ToColor555(ByteSwap16(fbramValue));
        if (color.a != 0u) {
            color.rgb >>= shift0;
            fbramValue &= ~0xFFFFu;
            fbramValue |= ByteSwap16(Color555ToUint16(color));
        }
    }
    if (shift1 != 0) {
        uint4 color = Uint16ToColor555(ByteSwap16(fbramValue >> 16u));
        if (color.a != 0u) {
            color.rgb >>= shift1;
            fbramValue &= ~0xFFFF0000u;
            fbramValue |= ByteSwap16(Color555ToUint16(color)) << 16u;
        }
    }
    g_fbramOut.Store(outOffset + fbOffset, fbramValue);
}

void Merge8(uint2 pos, uint field) {
    // Shadow does not apply to 8-bit mode.
}

void Merge16(uint2 pos, uint field) {
#if POLYSPEC_TRANSPARENT_MESH
    // Like the software renderer: shadow always reads the normal layer. A normal shadow pixel removes the mesh pixel
    // beneath it; a mesh shadow pixel puts the normal layer's pixel, shadowed once, on the mesh layer. Counts carry no
    // drawing order, so when both kinds hit a pixel in one batch the mesh one is taken as the last.
    const uint2 inPos = uint2(pos.x * 2, pos.y);
    const uint inMeshOffset = inPos.x + inPos.y * fbSize.x + (field + 2) * fbSize.x * fbSize.y;
    const uint inOffset = inPos.x + inPos.y * fbSize.x + field * fbSize.x * fbSize.y;
    const uint2 normalCount = uint2(g_internalSpriteOut[inOffset + 0], g_internalSpriteOut[inOffset + 1]);
    const uint2 meshCount = uint2(g_internalSpriteOut[inMeshOffset + 0], g_internalSpriteOut[inMeshOffset + 1]);
    g_internalSpriteOut[inMeshOffset + 0] = 0;
    g_internalSpriteOut[inMeshOffset + 1] = 0;
#endif

    Merge16Single(pos, field);

#if POLYSPEC_TRANSPARENT_MESH
    if (any(normalCount != 0) || any(meshCount != 0)) {
        const uint outOffset = (inPos.x + inPos.y * fbSize.x) * 2 + field * kScaledFBRAMSize;
        const uint normalValue = g_fbramOut.Load(outOffset + fbOffset);
        const uint meshAddress = outOffset + kScaledFBRAMSize * 2 + fbOffset;
        uint meshValue = g_fbramOut.Load(meshAddress);
        for (uint i = 0; i < 2; ++i) {
            const uint shiftBits = i * 16u;
            if (meshCount[i] != 0) {
                uint4 color = Uint16ToColor555(ByteSwap16(normalValue >> shiftBits));
                if (color.a != 0u) {
                    color.rgb >>= 1u;
                }
                meshValue &= ~(0xFFFFu << shiftBits);
                meshValue |= ByteSwap16(Color555ToUint16(color)) << shiftBits;
            } else if (normalCount[i] != 0) {
                meshValue &= ~(0xFFFFu << shiftBits);
            }
        }
        g_fbramOut.Store(meshAddress, meshValue);
    }
#endif
}

#elif POLYSPEC_MERGE_MODE == POLYSPEC_SHADING_MODE_OIT
// ----------------------------------------------------------------------------
// OIT (Half-Transparency)

uint HalfTransparentBlend(uint baseColor, uint listHead) {
    // Early exit if nothing was written to the pixel
    if (listHead == 0xFFFFFFFF) {
        return baseColor;
    }

    // Collect fragments
    OITFragment frags[32];
    uint count = 0;
    uint curr = listHead;
    while (curr != 0xFFFFFFFF && count < 32) {
        frags[count++] = g_fragments[curr];
        curr = frags[count - 1].next;
    }

    if (curr != 0xFFFFFFFF) {
        // More fragments than fit: heavily overdrawn pixels (folded quads redraw the same pixels many times). Every
        // blend counts, the oldest ones included, so walk the list in drawing order instead, picking the next
        // (sequence number, node) pair each time.
        uint4 color = Uint16ToColor555(baseColor);
        uint lastSeq = 0;
        uint lastNode = 0;
        bool first = true;
        while (true) {
            uint bestSeq = 0xFFFFFFFF;
            uint bestNode = 0xFFFFFFFF;
            uint bestData = 0;
            for (uint node = listHead; node != 0xFFFFFFFF; node = g_fragments[node].next) {
                const uint seq = BitExtract(g_fragments[node].data, 16, 16);
                const bool after = first || seq > lastSeq || (seq == lastSeq && node > lastNode);
                const bool better = seq < bestSeq || (seq == bestSeq && node < bestNode);
                if (after && better) {
                    bestSeq = seq;
                    bestNode = node;
                    bestData = g_fragments[node].data;
                }
            }
            if (bestNode == 0xFFFFFFFF) {
                break;
            }
            const uint4 fragColor = Uint16ToColor555(bestData);
            if (color.a != 0) {
                color.rgb = (color.rgb + fragColor.rgb) >> 1u;
            } else {
                color = fragColor;
            }
            lastSeq = bestSeq;
            lastNode = bestNode;
            first = false;
        }
        return Color555ToUint16(color);
    }

    // Sort by sequence number in ascending order (oldest to latest) so that fragments are blended in drawing order,
    // each one on top of the result of the previous ones
    for (uint i = 1; i < count; ++i) {
        OITFragment key = frags[i];
        int j = i - 1;
        while (j >= 0 && BitExtract(frags[j].data, 16, 16) > BitExtract(key.data, 16, 16)) {
            frags[j + 1] = frags[j];
            j--;
        }
        frags[j + 1] = key;
    }

    // Blend colors
    uint4 finalColor = Uint16ToColor555(baseColor);
    for (uint k = 0; k < count; ++k) {
        uint4 fragColor = Uint16ToColor555(frags[k].data);
        if (finalColor.a != 0) {
            finalColor.rgb = (finalColor.rgb + fragColor.rgb) >> 1u;
        } else {
            finalColor = fragColor;
        }
    }

    return Color555ToUint16(finalColor);
}

void Merge16Single(uint2 pos, uint field) {
    const uint2 inPos = uint2(pos.x * 2, pos.y);
    const uint inOffset = inPos.x + inPos.y * fbSize.x + field * fbSize.x * fbSize.y;

    const uint2 heads = uint2(
        g_listHeads[inOffset + 0],
        g_listHeads[inOffset + 1]
    );

    // Early exit if nothing was written to either pixel
    if (all(heads == 0xFFFFFFFF)) {
        return;
    }

    // Clear heads
    g_listHeads[inOffset + 0] = 0xFFFFFFFF;
    g_listHeads[inOffset + 1] = 0xFFFFFFFF;

    // Get base FBRAM value
    const uint fbramAddress = (inPos.x + inPos.y * fbSize.x) * 2 + fbOffset + field * kScaledFBRAMSize;
    uint fbramValue = g_fbramOut.Load(fbramAddress);

    // Modify
    fbramValue =
        (ByteSwap16(HalfTransparentBlend(ByteSwap16(fbramValue >> 16u), heads[1])) << 16u) |
         ByteSwap16(HalfTransparentBlend(ByteSwap16(fbramValue), heads[0]));

    // Write back
    g_fbramOut.Store(fbramAddress, fbramValue);
}

void Merge8(uint2 pos, uint field) {
    // Half-Transparency does not apply to 8-bit mode.
}

#if POLYSPEC_TRANSPARENT_MESH
// Latest sequence number in a pixel's fragment list, or 0 if empty
uint LatestSequence(uint listHead) {
    uint latest = 0;
    for (uint node = listHead; node != 0xFFFFFFFF; node = g_fragments[node].next) {
        latest = max(latest, BitExtract(g_fragments[node].data, 16, 16));
    }
    return latest;
}
#endif

void Merge16(uint2 pos, uint field) {
#if POLYSPEC_TRANSPARENT_MESH
    // A normal pixel drawn after a mesh pixel removes it from the mesh layer, as in the software renderer (and the copy
    // merger). Find which was drawn last before the lists are consumed.
    const uint2 inPos = uint2(pos.x * 2, pos.y);
    const uint inOffset = inPos.x + inPos.y * fbSize.x + field * fbSize.x * fbSize.y;
    const uint inMeshOffset = inPos.x + inPos.y * fbSize.x + (field + 2) * fbSize.x * fbSize.y;
    const uint2 latestNormal = uint2(LatestSequence(g_listHeads[inOffset + 0]), LatestSequence(g_listHeads[inOffset + 1]));
    const uint2 latestMesh =
        uint2(LatestSequence(g_listHeads[inMeshOffset + 0]), LatestSequence(g_listHeads[inMeshOffset + 1]));
#endif

    Merge16Single(pos, field);
#if POLYSPEC_TRANSPARENT_MESH
    Merge16Single(pos, field + 2);

    if (any(latestNormal > latestMesh)) {
        const uint meshAddress = (inPos.x + inPos.y * fbSize.x) * 2 + fbOffset + (field + 2) * kScaledFBRAMSize;
        uint meshValue = g_fbramOut.Load(meshAddress);
        if (latestNormal.x > latestMesh.x) {
            meshValue &= ~0xFFFFu;
        }
        if (latestNormal.y > latestMesh.y) {
            meshValue &= ~0xFFFF0000u;
        }
        g_fbramOut.Store(meshAddress, meshValue);
    }
#endif
}

#endif

// ---------------------------------------------------------------------------------------------------------------------
// Entrypoint

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (pixel8Bits) {
        Merge8(id.xy, id.z);
    } else {
        Merge16(id.xy, id.z);
    }
}
