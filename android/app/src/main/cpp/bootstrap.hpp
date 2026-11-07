#pragma once

#include <string>

namespace neonsaturn::android {

struct BootstrapConfig {
    std::string iplPath;
    /// Every imported IPL ROM; the one matching the disc's region replaces iplPath once the disc is in.
    std::string iplDir;
    std::string cdbPath;
    std::string discPath;
    std::string dataRoot;
    std::string gameControllerDbPath;
    std::string aspectRatio = "4:3";
    std::string textureFilter = "nearest";
    /// 0 = off (plain SDL_Renderer blit), 1 = 6x xBRZ shader (needs the GL presenter).
    int upscaleFilter = 0;
    /// 0 = software renderer; 1-4 = Vulkan renderer at that internal resolution scale.
    int resolutionScale = 0;
    bool deinterlace = false;
    bool transparentMeshes = false;
    bool rewindEnabled = false;
    /// Expansion cartridge: "auto" (whatever the game database recommends), "none", "dram8" (1 MiB) or "dram32" (4 MiB).
    std::string cartridge = "auto";
    /// RetroAchievements account; both empty when achievements are off or the player is signed out.
    std::string cheevosUser;
    std::string cheevosToken;
};

void SetBootstrapConfig(BootstrapConfig config);
BootstrapConfig GetBootstrapConfig();

} // namespace neonsaturn::android
