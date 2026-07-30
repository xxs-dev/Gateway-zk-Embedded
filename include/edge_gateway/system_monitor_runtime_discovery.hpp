#pragma once

#include <string>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

struct SystemMonitorRuntimeDependencies {
    std::vector<std::string> configFiles;
    std::vector<std::string> deviceConfigFiles;
    std::vector<std::string> sharedMemoryNames;
    std::vector<CameraServiceConfig> cameraServices;
    std::vector<std::string> warnings;
};

SystemMonitorRuntimeDependencies discoverSystemMonitorRuntimeDependencies(
    const std::string& primaryAppConfigPath,
    const AppConfig& primaryAppConfig
);

}  // namespace edge_gateway
