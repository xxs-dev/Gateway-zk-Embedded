#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#ifndef _WIN32
#include <sys/prctl.h>
#endif

#include "edge_gateway/builtin_mqtt_driver_publisher.hpp"
#include "edge_gateway/agc_avc_command_mailbox.hpp"
#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/ems_cluster_points.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/mqtt_forwarder_service.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace {

volatile std::sig_atomic_t g_running = 1;

void handleSignal(int) {
    g_running = 0;
}

std::string basenameOf(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
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

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

}  // namespace

int main(int argc, char* argv[]) {
    using namespace edge_gateway;

    std::string appConfigPath = "config/runtime/apps/mqtt-service.json";
    bool once = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--app-config" && i + 1 < argc) {
            appConfigPath = argv[++i];
        } else if (arg == "--once") {
            once = true;
        }
    }

    auto appConfig = ConfigLoader::loadAppConfigFromFile(appConfigPath);
    setProcessName("mqtt-fwd-" + sanitizeProcessToken(basenameOf(appConfigPath)));
    if (!appConfig.mqttForward.enabled) {
        std::cout << "mqtt forwarder disabled appConfig=" << appConfigPath << std::endl;
        return 0;
    }

    DeviceIdentity identity;
    if (!appConfig.identityConfigFile.empty()) {
        identity = ConfigLoader::loadDeviceIdentityFromFile(appConfig.identityConfigFile);
    }
    auto deviceConfigs = ConfigLoader::loadMany(appConfig.deviceConfigFiles, identity);
    std::vector<std::string> agcAvcSharedMemoryNames;
    (void)appendSiblingAgcAvcRuntime(
        appConfigPath,
        identity,
        deviceConfigs,
        agcAvcSharedMemoryNames
    );
    std::string topicMachineCode = identity.machineCode;
    for (const auto& config : deviceConfigs) {
        if (config.machineCode.empty()) {
            continue;
        }
        if (topicMachineCode.empty()) {
            topicMachineCode = config.machineCode;
        } else if (topicMachineCode != config.machineCode) {
            throw std::invalid_argument("mqtt forwarder requires a single machineCode across device configs");
        }
    }

    std::vector<std::string> sharedMemoryNames = appConfig.mqttDriver.sharedMemoryNames;
    if (sharedMemoryNames.empty() && !appConfig.mqttDriver.sharedMemoryName.empty()) {
        sharedMemoryNames.push_back(appConfig.mqttDriver.sharedMemoryName);
    }
    std::unordered_set<std::string> seenSharedMemoryNames(sharedMemoryNames.begin(), sharedMemoryNames.end());
    for (const auto& name : agcAvcSharedMemoryNames) {
        if (!name.empty() && seenSharedMemoryNames.insert(name).second) {
            sharedMemoryNames.push_back(name);
        }
    }
    for (const auto& config : deviceConfigs) {
        const auto& name = config.memoryStore.sharedMemoryName;
        if (!name.empty() && seenSharedMemoryNames.insert(name).second) {
            sharedMemoryNames.push_back(name);
        }
    }
    if (!appConfig.cameraService.sharedMemoryName.empty() &&
        seenSharedMemoryNames.insert(appConfig.cameraService.sharedMemoryName).second) {
        sharedMemoryNames.push_back(appConfig.cameraService.sharedMemoryName);
    }
    if (appConfig.emsCluster.enabled && !appConfig.emsCluster.virtualSharedMemoryName.empty() &&
        seenSharedMemoryNames.insert(appConfig.emsCluster.virtualSharedMemoryName).second) {
        sharedMemoryNames.push_back(appConfig.emsCluster.virtualSharedMemoryName);
    }
    if (sharedMemoryNames.empty()) {
        throw std::invalid_argument("mqtt forwarder requires at least one PointStore shared memory name");
    }

    PointStoreRouter router;
    std::vector<std::unique_ptr<MemoryPointStore>> stores;
    stores.reserve(sharedMemoryNames.size());
    for (const auto& name : sharedMemoryNames) {
        stores.emplace_back(new MemoryPointStore(name, MemoryStoreOpenMode::OpenExisting));
        router.addStore(name, *stores.back());
    }
    router.addRoutesFromDeviceConfigs(deviceConfigs, appConfig.mqttDriver.sharedMemoryName);
    router.addRoutesFromCameraServiceConfig(appConfig.cameraService, topicMachineCode);
    addEmsClusterPointRoutes(router, appConfig.emsCluster, topicMachineCode);
    for (const auto& camera : appConfig.cameraService.cameras) {
        if (!camera.enabled) {
            continue;
        }
        const auto addCameraStatusIndex = [&](std::uint32_t index) {
            if (index != 0) {
                appConfig.mqttDriver.fullUploadIndexes.push_back(index);
            }
        };
        addCameraStatusIndex(camera.statusPointIndexes.online);
        addCameraStatusIndex(camera.statusPointIndexes.fps);
        addCameraStatusIndex(camera.statusPointIndexes.bitrateKbps);
        addCameraStatusIndex(camera.statusPointIndexes.errorCode);
    }

    const auto txConfig = MqttForwarderService::makeTxOnlyMqttConfig(appConfig.mqttForward, topicMachineCode);
    auto publisher = std::make_shared<BuiltinMqttDriverPublisher>(txConfig, MqttPublisherMode::TxOnly);
    MqttForwarderService service(
        appConfig.mqttForward,
        appConfig.mqttDriver,
        router,
        publisher,
        "/opt/modbus-gateway/run/mqtt-forwarder-health.json"
    );

    if (once) {
        service.runOnce(nowMs());
        return 0;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    service.start();
    std::cout << "mqtt forwarder started"
              << " appConfig=" << appConfigPath
              << " shmCount=" << sharedMemoryNames.size()
              << " broker=" << appConfig.mqttForward.broker
              << " topic=" << appConfig.mqttForward.fullTelemetryTopic
              << " clientId=" << txConfig.clientId
              << " txOnly=1"
              << std::endl;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    service.stop();
    std::cout << "mqtt forwarder stopped" << std::endl;
    return 0;
}
