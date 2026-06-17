#include <chrono>
#include <algorithm>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#ifndef _WIN32
#include <dirent.h>
#include <sys/prctl.h>
#endif

#include "edge_gateway/builtin_mqtt_driver_publisher.hpp"
#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/point_store_router.hpp"
#include "edge_gateway/system_monitor_direct_maintenance.hpp"
#include "edge_gateway/system_monitor_service.hpp"

namespace {

volatile std::sig_atomic_t g_running = 1;

void handleSignal(int) {
    g_running = 0;
}

std::string basenameOf(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string dirnameOf(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
}

void addUniquePath(std::vector<std::string>& paths, const std::string& path) {
    if (path.empty()) {
        return;
    }
    if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
        paths.push_back(path);
    }
}

std::vector<std::string> discoverSiblingAppConfigFiles(const std::string& appConfigPath) {
    std::vector<std::string> files;
#ifndef _WIN32
    const auto dir = dirnameOf(appConfigPath);
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
#else
    (void)appConfigPath;
#endif
    return files;
}

std::string sanitizeProcessToken(std::string value) {
    for (auto& ch : value) {
        if (ch == '/' || ch == '\\' || ch == '.' || ch == '-' || ch == ' ') {
            ch = '_';
        }
    }
    return value;
}

void setProcessName(const std::string& name) {
#ifndef _WIN32
    prctl(PR_SET_NAME, name.substr(0, 15).c_str(), 0, 0, 0);
#else
    (void)name;
#endif
}

class StdoutMqttDriverPublisher : public edge_gateway::IMqttDriverPublisher {
public:
    void publishFullSnapshot(const std::string&, const std::vector<edge_gateway::StoredPointValue>&, const std::string&) override {}
    void publishAlarm(const std::string&, std::uint32_t, const edge_gateway::StoredPointValue&, const std::string&, bool) override {}
    void publishOnDemand(const std::string&, const std::vector<edge_gateway::StoredPointValue>&, const std::string&) override {}
    void publishChangeEvent(const std::string&, const edge_gateway::StoredPointValue&) override {}
    void publishCommandReply(const std::string&, const edge_gateway::MqttCommandReply&) override {}
    void publishOtaReply(const std::string&, const edge_gateway::OtaReply&) override {}
    void publishOtaStatus(const std::string&, const edge_gateway::OtaStatus&) override {}
    void publishJsonMessage(const std::string& topic, const std::string& payload) override {
        std::cout << "system-monitor topic=" << topic << " payload=" << payload << std::endl;
    }
    std::vector<edge_gateway::MqttIncomingMessage> pollIncoming(int) override { return {}; }
};

}  // namespace

int main(int argc, char* argv[]) {
    using namespace edge_gateway;

    std::string appConfigPath = "config/runtime/apps/mqtt-service.json";
    bool directMaintenanceDisabled = false;
    bool once = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--app-config" && i + 1 < argc) {
            appConfigPath = argv[++i];
        } else if (arg == "--no-direct-maintenance") {
            directMaintenanceDisabled = true;
        } else if (arg == "--once") {
            once = true;
        }
    }

    auto appConfig = ConfigLoader::loadAppConfigFromFile(appConfigPath);
    setProcessName("modbus-sysm-" + sanitizeProcessToken(basenameOf(appConfigPath)));

    DeviceIdentity identity;
    if (!appConfig.identityConfigFile.empty()) {
        identity = ConfigLoader::loadDeviceIdentityFromFile(appConfig.identityConfigFile);
    }
    const auto deviceConfigs = ConfigLoader::loadMany(appConfig.deviceConfigFiles, identity);
    std::vector<std::string> sharedMemoryNames = appConfig.mqttDriver.sharedMemoryNames;
    if (sharedMemoryNames.empty()) {
        sharedMemoryNames.push_back(appConfig.mqttDriver.sharedMemoryName);
    }
    std::unordered_set<std::string> seen(sharedMemoryNames.begin(), sharedMemoryNames.end());
    for (const auto& config : deviceConfigs) {
        const auto& name = config.memoryStore.sharedMemoryName;
        if (!name.empty() && seen.insert(name).second) {
            sharedMemoryNames.push_back(name);
        }
    }
    if (!appConfig.cameraService.sharedMemoryName.empty() &&
        seen.insert(appConfig.cameraService.sharedMemoryName).second) {
        sharedMemoryNames.push_back(appConfig.cameraService.sharedMemoryName);
    }
    edge_gateway::PointStoreRouter router;
    std::vector<std::unique_ptr<edge_gateway::MemoryPointStore>> stores;
    stores.reserve(sharedMemoryNames.size());
    for (const auto& name : sharedMemoryNames) {
        stores.emplace_back(new edge_gateway::MemoryPointStore(name));
        router.addStore(name, *stores.back());
    }
    router.addRoutesFromDeviceConfigs(deviceConfigs, appConfig.mqttDriver.sharedMemoryName);
    const auto machineCode = !identity.machineCode.empty()
        ? identity.machineCode
        : (deviceConfigs.empty() ? std::string() : deviceConfigs.front().machineCode);
    router.addRoutesFromCameraServiceConfig(appConfig.cameraService, machineCode);

    if (!machineCode.empty()) {
        appConfig.mqtt.topicMachineCode = machineCode;
        appConfig.mqtt.clientId = machineCode;
    }
    if (!appConfig.mqtt.clientId.empty()) {
        appConfig.mqtt.clientId += "_system_monitor";
    } else {
        appConfig.mqtt.clientId = "system-monitor";
    }
    std::vector<std::string> configFiles;
    if (!appConfig.identityConfigFile.empty()) {
        addUniquePath(configFiles, appConfig.identityConfigFile);
    }
    addUniquePath(configFiles, appConfigPath);
    for (const auto& appFile : discoverSiblingAppConfigFiles(appConfigPath)) {
        addUniquePath(configFiles, appFile);
    }
    for (const auto& file : appConfig.deviceConfigFiles) {
        addUniquePath(configFiles, file);
    }
    std::shared_ptr<IMqttDriverPublisher> publisher;
    if (appConfig.mqtt.enabled) {
        publisher = std::make_shared<BuiltinMqttDriverPublisher>(appConfig.mqtt);
    } else {
        publisher = std::make_shared<StdoutMqttDriverPublisher>();
    }
    SystemMonitorService service(appConfig.systemMonitor, appConfig.mqtt, publisher, machineCode, configFiles, &router);

    if (once) {
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        service.runOnce(nowMs);
        return 0;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    service.start();
    std::thread directMaintenanceThread;
    auto directMaintenanceConfig = appConfig.systemMonitor.directMaintenance;
    if (directMaintenanceDisabled) {
        directMaintenanceConfig.enabled = false;
    }
    if (directMaintenanceConfig.enabled) {
        directMaintenanceThread = std::thread([directMaintenanceConfig]() {
            edge_gateway::system_monitor_direct_maintenance::runFromConfig(directMaintenanceConfig);
        });
    }
    std::cout << "system monitor started"
              << " appConfig=" << appConfigPath
              << " broker=" << (appConfig.mqtt.enabled ? appConfig.mqtt.broker : "disabled")
              << " directMaintenance=" << (directMaintenanceConfig.enabled ? "enabled" : "disabled")
              << std::endl;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    service.stop();
    if (directMaintenanceThread.joinable()) {
        edge_gateway::system_monitor_direct_maintenance::requestStop();
        directMaintenanceThread.join();
    }
    std::cout << "system monitor stopped" << std::endl;
    return 0;
}
