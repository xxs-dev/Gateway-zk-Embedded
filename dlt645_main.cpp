#include <iostream>
#include <chrono>
#include <csignal>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/prctl.h>
#endif

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/dlt645_client.hpp"
#include "edge_gateway/gateway_daemon.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/mock_serial_port.hpp"
#ifndef _WIN32
#include "edge_gateway/posix_serial_port.hpp"
#endif

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

    std::string configPath = "config/runtime/devices/device_dlt645_multi_meter_1_2.json";
    std::string appConfigPath = "config/runtime/apps/mqtt-service.json";
    bool useMock = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--mock") {
            useMock = true;
        } else if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "--app-config" && i + 1 < argc) {
            appConfigPath = argv[++i];
        }
    }

    const auto config = ConfigLoader::loadFromFile(configPath);
    if (config.protocol.type != "dlt645_2007") {
        throw std::invalid_argument("Dlt645Driver requires protocol.type=dlt645_2007");
    }
    const auto appConfig = ConfigLoader::loadAppConfigFromFile(appConfigPath);

    auto processToken = basenameOf(config.protocol.transport.serialPort);
    if (processToken.empty()) {
        processToken = basenameOf(configPath);
    }
    setProcessName("dlt645-" + sanitizeProcessToken(processToken));

    std::shared_ptr<IMqttPublisher> mqttPublisher;
    if (appConfig.mqtt.enabled) {
        mqttPublisher = std::make_shared<DemoMqttPublisher>(appConfig.mqtt);
    }

    SerialPortOptions serialOptions;
    serialOptions.device = config.protocol.transport.serialPort;
    serialOptions.baudRate = config.protocol.transport.baudRate;
    serialOptions.dataBits = config.protocol.transport.dataBits;
    serialOptions.stopBits = config.protocol.transport.stopBits;
    serialOptions.parity = config.protocol.transport.parity;
    serialOptions.timeoutMs = config.protocol.transport.timeoutMs;

    std::shared_ptr<ISerialPort> serialPort;
#ifdef _WIN32
    useMock = true;
#endif
    if (useMock) {
        serialPort = std::make_shared<MockSerialPort>();
    }
#ifndef _WIN32
    else {
        serialPort = std::make_shared<PosixSerialPort>(serialOptions);
    }
#endif

    auto dlt645Client = std::make_shared<Dlt645Client>(serialPort, serialOptions);
    MemoryPointStore store(config.memoryStore);
    GatewayDaemon daemon(config, store, nullptr, dlt645Client, mqttPublisher);
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    daemon.start();
    const auto runtimeMeterCount = config.meters.empty() ? 1 : config.meters.size();
    std::cout << "dlt645 driver started"
              << " config=" << configPath
              << " appConfig=" << appConfigPath
              << " meters=" << runtimeMeterCount
              << " sharedMemory=" << config.memoryStore.sharedMemoryName
              << " sqlite=" << config.memoryStore.sqlitePath
              << " mqtt=" << (appConfig.mqtt.enabled ? appConfig.mqtt.broker : "disabled")
              << " mode=" << (useMock ? "mock" : "dlt645_2007")
              << std::endl;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    daemon.stop();
    std::cout << "dlt645 driver stopped" << std::endl;
    return 0;
}
