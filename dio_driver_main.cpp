#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <sys/prctl.h>
#endif

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/gateway_daemon.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/sysfs_gpio_port.hpp"

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

class DemoMqttPublisher : public edge_gateway::IMqttPublisher {
public:
    explicit DemoMqttPublisher(edge_gateway::MqttConfig config) : config_(std::move(config)) {
    }

    void publishTelemetry(
        const std::string& machineCode,
        const std::vector<edge_gateway::PointValue>& values
    ) override {
        std::cout << "publish telemetry broker=" << config_.broker
                  << " topic=" << config_.telemetryTopic
                  << " machine=" << machineCode
                  << " count=" << values.size() << std::endl;
    }

    void publishCommandResult(const edge_gateway::CommandResult& result) override {
        std::cout << "publish command result broker=" << config_.broker
                  << " topic=" << config_.commandReplyTopic
                  << " index=" << result.index
                  << " point=" << result.pointCode
                  << " success=" << result.success << std::endl;
    }

    void publishStatusMessage(
        const std::string& machineCode,
        const std::string& payload
    ) override {
        std::cout << "publish status broker=" << config_.broker
                  << " topic=" << config_.statusTopic
                  << " machine=" << machineCode
                  << " payload=" << payload
                  << std::endl;
    }

private:
    edge_gateway::MqttConfig config_;
};

}  // namespace

int main(int argc, char* argv[]) {
    using namespace edge_gateway;

    std::string configPath = "config/runtime/devices/device_dio.json";
    std::string appConfigPath = "config/runtime/apps/mqtt-service.json";
    bool once = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "--app-config" && i + 1 < argc) {
            appConfigPath = argv[++i];
        } else if (arg == "--once") {
            once = true;
        }
    }

    const auto appConfig = ConfigLoader::loadAppConfigFromFile(appConfigPath);
    DeviceIdentity identity;
    if (!appConfig.identityConfigFile.empty()) {
        identity = ConfigLoader::loadDeviceIdentityFromFile(appConfig.identityConfigFile);
    }
    const auto config = ConfigLoader::loadFromFile(configPath, identity);
    if (config.protocol.type != "local_dio") {
        throw std::invalid_argument("DioDriver requires protocol.type=local_dio");
    }

    setProcessName("dio-" + sanitizeProcessToken(basenameOf(configPath)));

    std::shared_ptr<IMqttPublisher> mqttPublisher;
    if (appConfig.mqtt.enabled) {
        mqttPublisher = std::make_shared<DemoMqttPublisher>(appConfig.mqtt);
    }
    auto gpioPort = std::make_shared<SysfsGpioPort>(config.protocol.gpioBasePath);
    MemoryPointStore store(config.memoryStore);
    GatewayDaemon daemon(config, store, nullptr, nullptr, mqttPublisher, gpioPort);

    if (once) {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        daemon.collectOnce(now);
        daemon.processWritebackOnce(now);
        daemon.flushPersistentOnce();
        return 0;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    daemon.start();
    const auto runtimeMeterCount = config.meters.empty() ? 1 : config.meters.size();
    std::cout << "dio driver started"
              << " config=" << configPath
              << " appConfig=" << appConfigPath
              << " meters=" << runtimeMeterCount
              << " sharedMemory=" << config.memoryStore.sharedMemoryName
              << " sqlite=" << config.memoryStore.sqlitePath
              << " mqtt=" << (appConfig.mqtt.enabled ? appConfig.mqtt.broker : "disabled")
              << " mode=local_dio"
              << std::endl;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    daemon.stop();
    std::cout << "dio driver stopped" << std::endl;
    return 0;
}
