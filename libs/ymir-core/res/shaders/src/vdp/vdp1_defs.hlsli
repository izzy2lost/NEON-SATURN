#ifndef YMIR_VDP_VDP1_DEFS_HLSLI
#define YMIR_VDP_VDP1_DEFS_HLSLI

// Size of VDP1 VRAM
static const uint kVDP1VRAMSize = 512 * 1024;

// Size of a single VDP1 framebuffer
static const uint kVDP1FBSize = 256 * 1024;
// Size of the entire VDP1 FBRAM
static const uint kVDP1FBRAMSize = kVDP1FBSize * 2;

// CMDPMOD bits 0..1
static const uint kColorBlendModeReplace = 0;
static const uint kColorBlendModeShadow = 1;
static const uint kColorBlendModeHalfLuminance = 2;
static const uint kColorBlendModeHalfTransparency = 3;

// Internal resolution scaling.
//
// With an internal resolution scale S > 1, each framebuffer holds S*S times as many pixels: every native pixel becomes
// an SxS block. Framebuffer lines keep their native byte layout (pixel size and order), only scaled. The scale is
// passed in bits 2-4 of the enhancements parameter as S - 1.

// Retrieves the internal resolution scale from the enhancements parameter.
uint DecodeResolutionScale(uint enhancements) {
    return ((enhancements >> 2u) & 7u) + 1u;
}

// Maps a byte offset within a native framebuffer line to the byte offset of the top-left of its block in a scaled
// framebuffer line.
uint ScaleFBLineByteOffset(uint byteOffset, uint pixelSize, uint scale) {
    const uint pixel = byteOffset / pixelSize;
    return pixel * scale * pixelSize + (byteOffset % pixelSize);
}

// Maps a byte offset within a scaled framebuffer line to the byte offset within a native framebuffer line.
uint UnscaleFBLineByteOffset(uint byteOffset, uint pixelSize, uint scale) {
    const uint pixel = byteOffset / pixelSize;
    return (pixel / scale) * pixelSize + (byteOffset % pixelSize);
}

struct OITFragment {
    uint data; // packed sprite data + sequence number
    uint next; // pointer to next fragment; 0xFFFFFFFF = end of list
};

#endif
