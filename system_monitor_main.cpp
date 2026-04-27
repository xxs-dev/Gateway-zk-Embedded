#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
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
#include "edge_gateway/point_store_router.hpp"
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
    void publishFullSnapshot(const std::string&, const std::vector<edge_gateway::StoredPointValue>&) override {}
    void publishAlarm(const std::string&, std::uint32_t, const edge_gateway::StoredPointValue&, const std::string&, bool) override {}
    void publishOnDemand(const std::string&, const std::vector<edge_gateway::StoredPointValue>&) override {}
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
    setProcessName("modbus-sysm-" + sanitizeProcessToken(basenameOf(appConfigPath)));
    if (!appConfig.mqtt.clientId.empty()) {
        appConfig.mqtt.clientId += "_system_monitor";
    } else {
        appConfig.mqtt.clientId = "system-monitor";
    }

    const auto deviceConfigs = ConfigLoader::loadMany(appConfig.deviceConfigFiles);
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
    edge_gateway::PointStoreRouter router;
    std::vector<std::unique_ptr<edge_gateway::MemoryPointStore>> stores;
    stores.reserve(sharedMemoryNames.size());
    for (const auto& name : sharedMemoryNames) {
        stores.emplace_back(new edge_gateway::MemoryPointStore(name));
        router.addStore(name, *stores.back());
    }
    router.addRoutesFromDeviceConfigs(deviceConfigs, appConfig.mqttDriver.sharedMemoryName);

    const auto machineCode = deviceConfigs.empty() ? std::string() : deviceConfigs.front().machineCode;
    if (!machineCode.empty()) {
        appConfig.mqtt.topicMachineCode = machineCode;
    }
    std::vector<std::string> configFiles;
    configFiles.push_back(appConfigPath);
    for (const auto& file : appConfig.deviceConfigFiles) {
        configFiles.push_back(file);
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
    std::cout << "system monitor started"
              << " appConfig=" << appConfigPath
              << " broker=" << (appConfig.mqtt.enabled ? appConfig.mqtt.broker : "disabled")
              << std::endl;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    service.stop();
    std::cout << "system monitor stopped" << std::endl;
    return 0;
}
