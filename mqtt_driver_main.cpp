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
#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/mqtt_event_outbox.hpp"
#include "edge_gateway/ota_service.hpp"
#include "edge_gateway/mqtt_driver_service.hpp"
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

std::string scopedTopic(const std::string& topic, const std::string& machineCode) {
    if (topic.empty() || machineCode.empty()) {
        return topic;
    }
    const std::string suffix = "/" + machineCode;
    if (topic.size() >= suffix.size() &&
        topic.compare(topic.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return topic;
    }
    return topic + suffix;
}

void setProcessName(const std::string& name) {
#ifndef _WIN32
    prctl(PR_SET_NAME, name.substr(0, 15).c_str(), 0, 0, 0);
#else
    (void)name;
#endif
}

std::unique_ptr<edge_gateway::MqttEventOutbox> createEventOutboxWithRetry(const edge_gateway::MqttConfig& config) {
    std::string lastError = "unknown";
    for (int attempt = 1; attempt <= 10; ++attempt) {
        try {
            return std::unique_ptr<edge_gateway::MqttEventOutbox>(new edge_gateway::MqttEventOutbox(
                config.eventOutboxSqlitePath,
                config.eventOutboxSqliteLibraryPath,
                config.eventOutboxRetentionMonths,
                config.eventOutboxCleanupIntervalHours,
                config.eventOutboxReplayBatchSize,
                config.eventOutboxMaxDiskBytes
            ));
        } catch (const std::exception& ex) {
            lastError = ex.what();
            std::cerr << "mqtt driver outbox open retry"
                      << " attempt=" << attempt
                      << " error=" << lastError
                      << std::endl;
            if (attempt < 10) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    }
    throw std::runtime_error(std::string("failed to open mqtt event outbox after retries: ") + lastError);
}

class StdoutMqttDriverPublisher : public edge_gateway::IMqttDriverPublisher {
public:
    explicit StdoutMqttDriverPublisher(edge_gateway::MqttConfig config) : config_(std::move(config)) {
    }

    void publishFullSnapshot(
        const std::string& topic,
        const std::vector<edge_gateway::StoredPointValue>& values,
        const std::string& valueFormat
    ) override {
        std::cout << "mqtt full version=" << config_.protocolVersion
                  << " topic=" << scopedTopic(topic, config_.topicMachineCode)
                  << " format=" << valueFormat
                  << " count=" << values.size()
                  << std::endl;
    }

    void publishAlarm(
        const std::string& topic,
        std::uint32_t index,
        const edge_gateway::StoredPointValue& value,
        const std::string& alarmType,
        bool active
    ) override {
        std::cout << "mqtt alarm version=" << config_.protocolVersion
                  << " topic=" << scopedTopic(topic, config_.topicMachineCode)
                  << " index=" << index
                  << " type=" << alarmType
                  << " active=" << active
                  << " value=" << value.value
                  << std::endl;
    }

    void publishOnDemand(
        const std::string& topic,
        const std::vector<edge_gateway::StoredPointValue>& values,
        const std::string& valueFormat
    ) override {
        std::cout << "mqtt demand version=" << config_.protocolVersion
                  << " topic=" << scopedTopic(topic, config_.topicMachineCode)
                  << " format=" << valueFormat
                  << " count=" << values.size()
                  << std::endl;
    }

    void publishChangeEvent(
        const std::string& topic,
        const edge_gateway::StoredPointValue& value
    ) override {
        std::cout << "mqtt change version=" << config_.protocolVersion
                  << " topic=" << scopedTopic(topic, config_.topicMachineCode)
                  << " index=" << value.index
                  << " value=" << value.value
                  << " quality=" << value.quality
                  << std::endl;
    }

    void publishCommandReply(
        const std::string& topic,
        const edge_gateway::MqttCommandReply& reply
    ) override {
        std::cout << "mqtt command-reply version=" << config_.protocolVersion
                  << " topic=" << scopedTopic(topic, config_.topicMachineCode)
                  << " cmdId=" << reply.cmdId
                  << " success=" << reply.success
                  << " message=" << reply.message
                  << std::endl;
    }

    void publishOtaReply(
        const std::string& topic,
        const edge_gateway::OtaReply& reply
    ) override {
        std::cout << "mqtt ota-reply version=" << config_.protocolVersion
                  << " topic=" << scopedTopic(topic, config_.topicMachineCode)
                  << " jobId=" << reply.jobId
                  << " accepted=" << reply.accepted
                  << " message=" << reply.message
                  << std::endl;
    }

    void publishOtaStatus(
        const std::string& topic,
        const edge_gateway::OtaStatus& status
    ) override {
        std::cout << "mqtt ota-status version=" << config_.protocolVersion
                  << " topic=" << scopedTopic(topic, config_.topicMachineCode)
                  << " jobId=" << status.jobId
                  << " stage=" << status.stage
                  << " progress=" << status.progress
                  << " message=" << status.message
                  << std::endl;
    }

    void publishJsonMessage(
        const std::string& topic,
        const std::string& payload
    ) override {
        std::cout << "mqtt json version=" << config_.protocolVersion
                  << " topic=" << scopedTopic(topic, config_.topicMachineCode)
                  << " payload=" << payload
                  << std::endl;
    }

    std::vector<edge_gateway::MqttIncomingMessage> pollIncoming(int) override {
        return {};
    }

private:
    edge_gateway::MqttConfig config_;
};

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

}  // namespace

int main(int argc, char* argv[]) {
    using namespace edge_gateway;

    std::string appConfigPath = "config/runtime/apps/mqtt-service.json";
    std::vector<std::uint32_t> pullIndexes;
    bool once = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--app-config" && i + 1 < argc) {
            appConfigPath = argv[++i];
        } else if (arg == "--once") {
            once = true;
        } else if (arg == "--index" && i + 1 < argc) {
            pullIndexes.push_back(static_cast<std::uint32_t>(std::stoul(argv[++i])));
        }
    }

    auto appConfig = ConfigLoader::loadAppConfigFromFile(appConfigPath);
    setProcessName("modbus-mqtt-" + sanitizeProcessToken(basenameOf(appConfigPath)));
    if (appConfig.mqtt.protocolVersion != "mqtt3" && appConfig.mqtt.protocolVersion != "mqtt5") {
        throw std::invalid_argument("mqtt.protocolVersion must be mqtt3 or mqtt5");
    }
    DeviceIdentity identity;
    if (!appConfig.identityConfigFile.empty()) {
        identity = ConfigLoader::loadDeviceIdentityFromFile(appConfig.identityConfigFile);
    }
    const auto deviceConfigs = ConfigLoader::loadMany(appConfig.deviceConfigFiles, identity);
    std::string topicMachineCode = identity.machineCode;
    for (const auto& config : deviceConfigs) {
        if (config.machineCode.empty()) {
            continue;
        }
        if (topicMachineCode.empty()) {
            topicMachineCode = config.machineCode;
        } else if (topicMachineCode != config.machineCode) {
            throw std::invalid_argument("mqtt driver requires a single machineCode across device configs");
        }
    }
    appConfig.mqtt.topicMachineCode = topicMachineCode;
    if (!topicMachineCode.empty()) {
        appConfig.mqtt.clientId = topicMachineCode;
    }
    std::vector<std::string> sharedMemoryNames = appConfig.mqttDriver.sharedMemoryNames;
    if (sharedMemoryNames.empty()) {
        sharedMemoryNames.push_back(appConfig.mqttDriver.sharedMemoryName);
    }
    std::unordered_set<std::string> seenSharedMemoryNames(sharedMemoryNames.begin(), sharedMemoryNames.end());
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
    PointStoreRouter router;
    std::vector<std::unique_ptr<MemoryPointStore>> stores;
    stores.reserve(sharedMemoryNames.size());
    for (const auto& name : sharedMemoryNames) {
        stores.emplace_back(new MemoryPointStore(name));
        router.addStore(name, *stores.back());
    }
    router.addRoutesFromDeviceConfigs(deviceConfigs, appConfig.mqttDriver.sharedMemoryName);
    router.addRoutesFromCameraServiceConfig(appConfig.cameraService, topicMachineCode);
    std::shared_ptr<IMqttDriverPublisher> publisher;
    if (appConfig.mqtt.enabled) {
        publisher = std::make_shared<BuiltinMqttDriverPublisher>(appConfig.mqtt);
    } else {
        publisher = std::make_shared<StdoutMqttDriverPublisher>(appConfig.mqtt);
    }
    std::unique_ptr<OtaService> otaService;
    if (appConfig.ota.enabled) {
        otaService.reset(new OtaService(appConfig.ota));
    }
    std::unique_ptr<MqttEventOutbox> eventOutbox;
    if (appConfig.eventEngine.publishMode == "mqtt_driver_outbox") {
        eventOutbox = createEventOutboxWithRetry(appConfig.mqtt);
    }
    MqttDriverService service(
        appConfig.mqtt,
        appConfig.mqttDriver,
        deviceConfigs,
        router,
        publisher,
        std::move(eventOutbox),
        std::move(otaService)
    );

    if (once) {
        service.publishOnDemandNow(pullIndexes, nowMs());
        return 0;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    service.start();
    std::cout << "mqtt driver started"
              << " appConfig=" << appConfigPath
              << " shmCount=" << sharedMemoryNames.size()
              << " primaryShm=" << appConfig.mqttDriver.sharedMemoryName
              << " version=" << appConfig.mqtt.protocolVersion
              << " deviceConfigs=" << deviceConfigs.size()
              << " broker=" << (appConfig.mqtt.enabled ? appConfig.mqtt.broker : "disabled")
              << std::endl;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    service.stop();
    std::cout << "mqtt driver stopped" << std::endl;
    return 0;
}
