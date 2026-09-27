#ifndef YMIR_UTIL_SPIRV_HLSLI
#define YMIR_UTIL_SPIRV_HLSLI

// Helpers for the SPIR-V (Vulkan) build of the shaders. DXC defines __spirv__ when compiling with -spirv.
//
// Storage images must declare the exact format of the image view bound to them: DXC otherwise infers a 32-bit
// format from the element type (uint4 -> Rgba32ui, float4 -> Rgba32f), which does not match the 8/16-bit textures
// the renderer creates. Declaring an unknown format instead would require shaderStorageImageReadWithoutFormat and
// shaderStorageImageWriteWithoutFormat, which many mobile GPUs lack.
#ifdef __spirv__
    #define SPIRV_IMAGE_FORMAT(fmt) [[vk::image_format(fmt)]]
#else
    #define SPIRV_IMAGE_FORMAT(fmt)
#endif

#endif
