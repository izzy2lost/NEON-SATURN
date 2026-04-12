#pragma once

#include <string>

namespace neonsaturn::android {

struct BootstrapConfig {
    std::string iplPath;
    std::string cdbPath;
    std::string discPath;
    std::string dataRoot;
};

void SetBootstrapConfig(BootstrapConfig config);
BootstrapConfig GetBootstrapConfig();

} // namespace neonsaturn::android

