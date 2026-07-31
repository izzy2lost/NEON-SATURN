#pragma once

#include <string>

namespace neonsaturn::android {

struct BootstrapConfig {
    std::string iplPath;
    std::string cdbPath;
    std::string discPath;
    std::string dataRoot;
    std::string gameControllerDbPath;
    std::string aspectRatio = "4:3";
    std::string textureFilter = "nearest";
    /// 0 = off (plain SDL_Renderer blit), 1 = 6x xBRZ shader (needs the GL presenter).
    int upscaleFilter = 0;
    bool deinterlace = false;
    bool transparentMeshes = false;
    bool rewindEnabled = false;
};

void SetBootstrapConfig(BootstrapConfig config);
BootstrapConfig GetBootstrapConfig();

} // namespace neonsaturn::android
