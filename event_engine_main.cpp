#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <sys/prctl.h>
#endif

#include "edge_gateway/builtin_mqtt_driver_publisher.hpp"
#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/event_engine_service.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/mqtt_event_outbox.hpp"
#include "edge_gateway/point_store_router.hpp"
#include "edge_gateway/sqlite_alarm_writer.hpp"

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
            std::cerr << "event engine outbox open retry"
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

    void publishFullSnapshot(const std::string& topic, const std::vector<edge_gateway::StoredPointValue>& values, const std::string&) override {
        std::cout << "event full topic=" << topic << " count=" << values.size() << std::endl;
    }

    void publishAlarm(
        const std::string& topic,
        std::uint32_t index,
        const edge_gateway::StoredPointValue& value,
        const std::string& alarmType,
        bool active
    ) override {
        std::cout << "event alarm topic=" << topic
                  << " index=" << index
                  << " type=" << alarmType
                  << " active=" << active
                  << " value=" << value.value
                  << std::endl;
    }

    void publishOnDemand(const std::string& topic, const std::vector<edge_gateway::StoredPointValue>& values, const std::string&) override {
        std::cout << "event demand topic=" << topic << " count=" << values.size() << std::endl;
    }

    void publishChangeEvent(const std::string& topic, const edge_gateway::StoredPointValue& value) override {
        std::cout << "event change topic=" << topic
                  << " index=" << value.index
                  << " value=" << value.value
                  << std::endl;
    }

    void publishCommandReply(const std::string&, const edge_gateway::MqttCommandReply&) override {
    }

    void publishOtaReply(const std::string&, const edge_gateway::OtaReply&) override {
    }

    void publishOtaStatus(const std::string&, const edge_gateway::OtaStatus&) override {
    }

    void publishJsonMessage(const std::string& topic, const std::string& payload) override {
        std::cout << "event json topic=" << topic << " payload=" << payload << std::endl;
    }

    std::vector<edge_gateway::MqttIncomingMessage> pollIncoming(int) override {
        return {};
    }

private:
    edge_gateway::MqttConfig config_;
};

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
    setProcessName("modbus-event-" + sanitizeProcessToken(basenameOf(appConfigPath)));

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
            throw std::invalid_argument("event engine requires a single machineCode across device configs");
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

    PointStoreRouter router;
    std::vector<std::unique_ptr<MemoryPointStore>> ownedStores;
    std::vector<MemoryPointStore*> stores;
    ownedStores.reserve(sharedMemoryNames.size());
    stores.reserve(sharedMemoryNames.size());
    for (const auto& name : sharedMemoryNames) {
        ownedStores.emplace_back(new MemoryPointStore(name));
        stores.push_back(ownedStores.back().get());
        router.addStore(name, *ownedStores.back());
    }
    router.addRoutesFromDeviceConfigs(deviceConfigs, appConfig.mqttDriver.sharedMemoryName);
    router.addRoutesFromCameraServiceConfig(appConfig.cameraService, topicMachineCode);

    MqttConfig eventMqtt = appConfig.mqtt;
    if (!appConfig.eventEngine.mqttClientIdSuffix.empty()) {
        eventMqtt.clientId += "_" + appConfig.eventEngine.mqttClientIdSuffix;
    }

    std::shared_ptr<IMqttDriverPublisher> publisher;
    if (eventMqtt.enabled) {
        publisher = std::make_shared<BuiltinMqttDriverPublisher>(eventMqtt);
    } else {
        publisher = std::make_shared<StdoutMqttDriverPublisher>(eventMqtt);
    }

    std::unique_ptr<SqliteAlarmWriter> alarmWriter;
    if (appConfig.alarmStore.enabled) {
        alarmWriter.reset(new SqliteAlarmWriter(
            appConfig.alarmStore.sqlitePath,
            appConfig.alarmStore.sqliteLibraryPath
        ));
    }
    std::unique_ptr<MqttEventOutbox> eventOutbox;
    if (appConfig.eventEngine.publishMode == "mqtt_driver_outbox") {
        eventOutbox = createEventOutboxWithRetry(eventMqtt);
    }

    EventEngineService service(
        appConfig.eventEngine,
        eventMqtt,
        deviceConfigs,
        router,
        stores,
        publisher,
        std::move(eventOutbox),
        std::move(alarmWriter)
    );

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
    std::cout << "event engine started"
              << " appConfig=" << appConfigPath
              << " shmCount=" << sharedMemoryNames.size()
              << " broker=" << (eventMqtt.enabled ? eventMqtt.broker : "disabled")
              << " publishMode=" << appConfig.eventEngine.publishMode
              << std::endl;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    service.stop();
    std::cout << "event engine stopped" << std::endl;
    return 0;
}
