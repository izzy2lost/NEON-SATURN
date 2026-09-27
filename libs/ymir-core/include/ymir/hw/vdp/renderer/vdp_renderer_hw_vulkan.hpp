#pragma once

/**
@file
@brief VDP1 and VDP2 renderer implementation using Vulkan compute shaders.

A port of the Direct3D 12 renderer running the same HLSL shaders compiled to SPIR-V. The renderer creates its own
headless Vulkan device and delivers finished frames to the CPU through `VulkanRendererCallbacks::FrameReady`.

Requires Vulkan 1.1 with the `shaderInt64`, `shaderStorageImageExtendedFormats` and `scalarBlockLayout` features.
*/

#include <ymir/hw/vdp/renderer/vdp_renderer_hw_base.hpp>

#include "vdp_renderer_hw_vulkan_callbacks.hpp"

#include <ymir/util/result.hpp>

#include <memory>
#include <string>

namespace ymir::vdp {

/// @brief VDP renderer implementation using Vulkan.
class VulkanVDPRenderer : public HardwareVDPRendererBase {
    VulkanVDPRenderer(VDPState &state, const config::VDP2DebugRender &vdp2DebugRenderOptions,
                      const config::VDP2AccessPatternsConfig &vdp2AccessPatternsConfig, uint32 resolutionScale);

    util::VoidResult<> Initialize();

public:
    ~VulkanVDPRenderer();

    /// @brief Maximum supported internal resolution scale.
    static constexpr uint32 kMaxResolutionScale = 4;

    /// @brief Creates a Vulkan renderer.
    /// @param[in] resolutionScale the internal resolution scale, from 1 (native) to `kMaxResolutionScale`. Every
    /// native pixel is rendered as a block of `resolutionScale` x `resolutionScale` pixels.
    static util::ObjectResult<VulkanVDPRenderer> Create(VDPState &state,
                                                        const config::VDP2DebugRender &vdp2DebugRenderOptions,
                                                        const config::VDP2AccessPatternsConfig &vdp2AccessPatternsConfig,
                                                        uint32 resolutionScale = 1);

    // -------------------------------------------------------------------------
    // Configuration

    VulkanRendererCallbacks HwCallbacks;

    /// @brief Retrieves the name of the GPU used by this renderer.
    /// @return the name of the Vulkan physical device
    std::string GetDeviceName() const;

    /// @brief Retrieves the internal resolution scale this renderer was created with.
    /// @return the internal resolution scale
    uint32 GetResolutionScale() const;

protected:
    void UpdateEnhancements() override;

public:
    // -------------------------------------------------------------------------
    // Basics

    bool IsValid() const override;

    void Reset(bool hard) override;

    // -------------------------------------------------------------------------
    // Save states

    void PreSaveStateSync() override;
    void PostLoadStateSync() override;

    void SaveState(savestate::VDPSaveState::VDPRendererSaveState &state) override;
    bool ValidateState(const savestate::VDPSaveState::VDPRendererSaveState &state) const override;
    void LoadState(const savestate::VDPSaveState::VDPRendererSaveState &state) override;

    // -------------------------------------------------------------------------
    // VDP1 memory and register writes

    void VDP1WriteVRAM(uint32 address, uint8 value) override;
    void VDP1WriteVRAM(uint32 address, uint16 value) override;
    void VDP1SyncFB() override;
    void VDP1DebugSyncFB() override;
    void VDP1WriteFB(uint32 address, uint8 value) override;
    void VDP1WriteFB(uint32 address, uint16 value) override;
    void VDP1WriteReg(uint32 address, uint16 value) override;

    // -------------------------------------------------------------------------
    // VDP2 memory and register writes

    void VDP2WriteVRAM(uint32 address, uint8 value) override;
    void VDP2WriteVRAM(uint32 address, uint16 value) override;
    void VDP2WriteCRAM(uint32 address, uint8 value) override;
    void VDP2WriteCRAM(uint32 address, uint16 value) override;
    void VDP2WriteReg(uint32 address, uint16 value) override;

    // -------------------------------------------------------------------------
    // Debugger

    void UpdateEnabledLayers() override;

    // -------------------------------------------------------------------------
    // Utilities

    void DumpExtraVDP1Framebuffers(std::ostream &out) const override;

    // -------------------------------------------------------------------------
    // Rendering process

    void VDP1EraseFramebuffer(uint64 cycles) override;
    void VDP1SwapFramebuffer() override;
    void VDP1BeginFrame() override;
    void VDP1ExecuteCommand(uint32 cmdAddress, VDP1Command::Control control) override;
    void VDP1EndFrame() override;

    void VDP2SetResolution(uint32 h, uint32 v, bool exclusive) override;
    void VDP2SetField(bool odd) override;
    void VDP2LatchTVMD() override;
    void VDP2BeginFrame() override;
    void VDP2RenderLine(uint32 y) override;
    void VDP2EndFrame() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ymir::vdp
