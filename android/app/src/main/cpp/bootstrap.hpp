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
    int resolutionScale = 1;
    bool deinterlace = false;
    bool transparentMeshes = false;
    bool rewindEnabled = false;
};

void SetBootstrapConfig(BootstrapConfig config);
BootstrapConfig GetBootstrapConfig();

} // namespace neonsaturn::android
