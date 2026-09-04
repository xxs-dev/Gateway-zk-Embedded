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
#include "edge_gateway/mqtt_driver_service.hpp"
#include "edge_gateway/mqtt_forwarder_service.hpp"
#include "edge_gateway/point_store_router.hpp"
#include "edge_gateway/timing_policy.hpp"

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

std::string sanitizeFileToken(std::string value) {
    for (auto& ch : value) {
        if (ch == '/' || ch == '\\' || ch == '.' || ch == ' ') {
            ch = '_';
        }
    }
    return value;
}

std::string scopedWorkerPath(std::string path, const std::string& appConfigPath) {
    auto instance = basenameOf(appConfigPath);
    const auto extension = instance.rfind(".json");
    if (extension != std::string::npos && extension + 5 == instance.size()) {
        instance.erase(extension);
    }
    instance = sanitizeFileToken(instance);
    const std::string marker = "{instance}";
    std::size_t pos = 0;
    while ((pos = path.find(marker, pos)) != std::string::npos) {
        path.replace(pos, marker.size(), instance);
        pos += instance.size();
    }
    return path;
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
    TimingPolicyResolver::applyAppServices(appConfig);
    appConfig.mqttDriver.fullUploadWorker.healthFile = scopedWorkerPath(
        appConfig.mqttDriver.fullUploadWorker.healthFile,
        appConfigPath
    );
    appConfig.mqttDriver.fullUploadWorker.publishLockFile = scopedWorkerPath(
        appConfig.mqttDriver.fullUploadWorker.publishLockFile,
        appConfigPath
    );
    setProcessName("mqtt-fwd-" + sanitizeProcessToken(basenameOf(appConfigPath)));
    const bool primaryFullEnabled = appConfig.mqtt.enabled &&
        appConfig.mqttDriver.enabled &&
        appConfig.mqttDriver.fullUploadIntervalMs > 0 &&
        appConfig.mqttDriver.fullUploadWorker.mode == "isolated";
    const bool thirdPartyEnabled = appConfig.mqttForward.enabled;
    if (!primaryFullEnabled && !thirdPartyEnabled) {
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
    if (appConfig.cameraService.enabled &&
        !appConfig.cameraService.sharedMemoryName.empty() &&
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

    std::vector<std::shared_ptr<IMqttDriverPublisher>> publishers;
    std::vector<std::unique_ptr<MqttForwarderService>> services;
    std::string primaryFullClientId;
    std::string primaryFullTopic;

    if (primaryFullEnabled) {
        MqttForwardConfig primaryForward;
        primaryForward.enabled = true;
        primaryForward.fullTelemetryTopic = appConfig.mqtt.fullTelemetryTopic.empty()
            ? appConfig.mqtt.telemetryTopic
            : appConfig.mqtt.fullTelemetryTopic;
        primaryForward.fullTelemetryTopicMachineScoped =
            appConfig.mqtt.fullTelemetryTopicMachineScoped;
        primaryForward.pointIndexes = MqttDriverService::resolveFullUploadIndexes(
            appConfig.mqttDriver,
            deviceConfigs,
            router
        );
        primaryForward.payloadFormat = appConfig.mqttDriver.fullUploadJsonFormat;
        primaryForward.qos = appConfig.mqtt.qos;
        primaryForward.intervalMs = appConfig.mqttDriver.fullUploadIntervalMs;
        primaryForward.retryMinMs = appConfig.mqttDriver.fullUploadWorker.retryMinMs;
        primaryForward.retryMaxMs = appConfig.mqttDriver.fullUploadWorker.retryMaxMs;
        primaryForward.healthHeartbeatMs =
            appConfig.mqttDriver.fullUploadWorker.healthHeartbeatMs;
        primaryForward.healthLeaseTtlMs =
            appConfig.mqttDriver.fullUploadWorker.failoverTimeoutMs;
        primaryForward.failOnStoreError = false;
        primaryForward.primaryFullUpload = true;
        primaryForward.publishOnStart = appConfig.mqttDriver.publishFullOnStart;
        primaryForward.primaryMachineCode = topicMachineCode;
        primaryForward.publishLockFile =
            appConfig.mqttDriver.fullUploadWorker.publishLockFile;

        const auto primaryMqttConfig = MqttForwarderService::makePrimaryFullMqttConfig(
            appConfig.mqtt,
            appConfig.mqttDriver.fullUploadWorker,
            topicMachineCode
        );
        primaryFullClientId = primaryMqttConfig.clientId;
        primaryForward.primaryClientId = primaryMqttConfig.clientId;
        primaryFullTopic = primaryForward.fullTelemetryTopic;
        auto publisher = std::make_shared<BuiltinMqttDriverPublisher>(
            primaryMqttConfig,
            MqttPublisherMode::TxOnly
        );
        publishers.push_back(publisher);
        services.emplace_back(new MqttForwarderService(
            primaryForward,
            router,
            publisher,
            appConfig.mqttDriver.fullUploadWorker.healthFile
        ));
        std::cout << "primary full forwarder configured"
                  << " broker=" << primaryMqttConfig.broker
                  << " topic=" << primaryForward.fullTelemetryTopic
                  << " clientId=" << primaryMqttConfig.clientId
                  << " pointCount=" << primaryForward.pointIndexes.size()
                  << std::endl;
    }

    if (thirdPartyEnabled) {
        const auto mqttConfig = MqttForwarderService::makeMqttConfig(
            appConfig.mqttForward,
            topicMachineCode
        );
        if (primaryFullEnabled && mqttConfig.broker == appConfig.mqtt.broker &&
            mqttConfig.clientId == primaryFullClientId) {
            throw std::invalid_argument(
                "primary full and third-party MQTT forwarders must use different clientId values"
            );
        }
        if (primaryFullEnabled && mqttConfig.broker == appConfig.mqtt.broker &&
            appConfig.mqttForward.fullTelemetryTopicMachineScoped ==
                appConfig.mqtt.fullTelemetryTopicMachineScoped &&
            appConfig.mqttForward.fullTelemetryTopic == primaryFullTopic) {
            throw std::invalid_argument(
                "primary full and third-party MQTT forwarders must not publish the same topic"
            );
        }
        const auto publisherMode = appConfig.mqttForward.control.enabled
            ? MqttPublisherMode::Bidirectional
            : MqttPublisherMode::TxOnly;
        auto publisher = std::make_shared<BuiltinMqttDriverPublisher>(mqttConfig, publisherMode);
        publishers.push_back(publisher);
        services.emplace_back(new MqttForwarderService(
            appConfig.mqttForward,
            router,
            publisher,
            "/opt/modbus-gateway/run/mqtt-forwarder-health.json"
        ));
        std::cout << "third-party mqtt forwarder configured"
                  << " broker=" << appConfig.mqttForward.broker
                  << " topic=" << appConfig.mqttForward.fullTelemetryTopic
                  << " clientId=" << mqttConfig.clientId
                  << " thirdPartyControl=" << (appConfig.mqttForward.control.enabled ? 1 : 0)
                  << std::endl;
    }

    if (once) {
        for (const auto& service : services) {
            service->runOnce(nowMs());
        }
        return 0;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    for (const auto& service : services) {
        service->start();
    }
    std::cout << "mqtt forwarder started"
              << " appConfig=" << appConfigPath
              << " shmCount=" << sharedMemoryNames.size()
              << " workerCount=" << services.size()
              << " primaryFull=" << (primaryFullEnabled ? 1 : 0)
              << " thirdParty=" << (thirdPartyEnabled ? 1 : 0)
              << std::endl;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    for (auto it = services.rbegin(); it != services.rend(); ++it) {
        (*it)->stop();
    }
    std::cout << "mqtt forwarder stopped" << std::endl;
    return 0;
}
