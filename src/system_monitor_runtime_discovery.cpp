#include "edge_gateway/system_monitor_runtime_discovery.hpp"

#include <algorithm>
#ifndef _WIN32
#include <dirent.h>
#endif

#include "edge_gateway/config_loader.hpp"

namespace edge_gateway {

namespace {

std::string dirnameOf(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
}

void addUnique(std::vector<std::string>& values, const std::string& value) {
    if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

void addSharedMemoryNames(
    SystemMonitorRuntimeDependencies& result,
    const std::vector<std::string>& names
) {
    for (const auto& name : names) {
        addUnique(result.sharedMemoryNames, name);
    }
}

std::vector<std::string> discoverSiblingAppConfigFiles(const std::string& primaryAppConfigPath) {
    std::vector<std::string> files;
#ifndef _WIN32
    const auto dir = dirnameOf(primaryAppConfigPath);
    DIR* handle = opendir(dir.c_str());
    if (handle == nullptr) {
        return files;
    }
    while (dirent* entry = readdir(handle)) {
        const std::string name = entry->d_name;
        if (name.size() < 6 || name.substr(name.size() - 5) != ".json") {
            continue;
        }
        files.push_back(dir + "/" + name);
    }
    closedir(handle);
    std::sort(files.begin(), files.end());
#else
    (void)primaryAppConfigPath;
#endif
    return files;
}

void mergeAppDependencies(
    SystemMonitorRuntimeDependencies& result,
    const std::string& appConfigPath,
    const AppConfig& appConfig,
    bool primary
) {
    addUnique(result.configFiles, appConfigPath);
    addUnique(result.configFiles, appConfig.identityConfigFile);
    for (const auto& file : appConfig.deviceConfigFiles) {
        addUnique(result.deviceConfigFiles, file);
        addUnique(result.configFiles, file);
    }

    if (primary || appConfig.mqttDriver.enabled) {
        addSharedMemoryNames(result, appConfig.mqttDriver.sharedMemoryNames);
        if (appConfig.mqttDriver.sharedMemoryNames.empty()) {
            addUnique(result.sharedMemoryNames, appConfig.mqttDriver.sharedMemoryName);
        }
    }
    if (appConfig.computeEngine.enabled) {
        addSharedMemoryNames(result, appConfig.computeEngine.sharedMemoryNames);
        addUnique(result.sharedMemoryNames, appConfig.computeEngine.outputDefaultSharedMemoryName);
    }
    if (appConfig.localDisplay.enabled) {
        addSharedMemoryNames(result, appConfig.localDisplay.sharedMemoryNames);
    }
    if (appConfig.agcAvc.enabled) {
        addSharedMemoryNames(result, appConfig.agcAvc.sharedMemoryNames);
        addUnique(result.sharedMemoryNames, appConfig.agcAvc.outputSharedMemoryName);
    }
    if (appConfig.cameraService.enabled) {
        addUnique(result.sharedMemoryNames, appConfig.cameraService.sharedMemoryName);
        result.cameraServices.push_back(appConfig.cameraService);
    }
}

}  // namespace

SystemMonitorRuntimeDependencies discoverSystemMonitorRuntimeDependencies(
    const std::string& primaryAppConfigPath,
    const AppConfig& primaryAppConfig
) {
    SystemMonitorRuntimeDependencies result;
    mergeAppDependencies(result, primaryAppConfigPath, primaryAppConfig, true);

    for (const auto& appFile : discoverSiblingAppConfigFiles(primaryAppConfigPath)) {
        if (appFile == primaryAppConfigPath) {
            continue;
        }
        try {
            const auto siblingApp = ConfigLoader::loadAppConfigFromFile(appFile);
            mergeAppDependencies(result, appFile, siblingApp, false);
        } catch (const std::exception& ex) {
            result.warnings.push_back(appFile + ": " + ex.what());
        }
    }
    return result;
}

}  // namespace edge_gateway
