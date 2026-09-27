#pragma once

#include <ymir/util/callback.hpp>

#include <ymir/core/types.hpp>

namespace ymir::vdp {

/// @brief Type of callback invoked when the Vulkan VDP renderer has a finished frame available on the CPU.
///
/// The framebuffer contains `width` by `height` tightly packed pixels in R8G8B8A8 byte order (XBGR8888 when read as
/// little-endian 32-bit words), matching the output of the software renderer. The dimensions include the internal
/// resolution scale. The pointer is only valid for the duration of the call.
///
/// Frames are delivered one frame late to avoid stalling the emulator thread on the GPU.
///
/// @param[in] fb pointer to the framebuffer data
/// @param[in] width the width of the framebuffer
/// @param[in] height the height of the framebuffer
using CBVulkanFrameReady = util::OptionalCallback<void(uint32 *fb, uint32 width, uint32 height)>;

/// @brief Callbacks specific to the Vulkan VDP renderer.
struct VulkanRendererCallbacks {
    CBVulkanFrameReady FrameReady;
};

} // namespace ymir::vdp
