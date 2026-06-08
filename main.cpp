#include <iostream>
#include <chrono>
#include <csignal>
#include <memory>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/prctl.h>
#endif

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/dlt645_standard_points_loader.hpp"
#include "edge_gateway/dlt645_client.hpp"
#include "edge_gateway/gateway_daemon.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/mock_serial_port.hpp"
#include "edge_gateway/modbus_rtu_client.hpp"
#include "edge_gateway/modbus_tcp_client.hpp"
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

}  // namespace

int main(int argc, char* argv[]) {
    using namespace edge_gateway;

    std::string configPath = "config/runtime/devices/device_slave_ttySP1.json";
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

    const auto appConfig = ConfigLoader::loadAppConfigFromFile(appConfigPath);
    DeviceIdentity identity;
    if (!appConfig.identityConfigFile.empty()) {
        identity = ConfigLoader::loadDeviceIdentityFromFile(appConfig.identityConfigFile);
    }
    auto config = ConfigLoader::loadFromFile(configPath, identity);
    config.mqttDriver = appConfig.mqttDriver;
    std::string processToken;
    if (config.protocol.type == "modbus_tcp") {
        processToken = "tcp" + std::to_string(config.protocol.tcp.port);
    } else {
        processToken = basenameOf(config.protocol.transport.serialPort);
    }
    if (processToken.empty()) {
        processToken = basenameOf(configPath);
    }
    setProcessName("modbus-" + sanitizeProcessToken(processToken));
    std::shared_ptr<IModbusClient> modbusClient;
    std::shared_ptr<Dlt645Client> dlt645Client;
    const auto frameIntervalMs = config.protocol.transport.frameIntervalMs >= 0
        ? config.protocol.transport.frameIntervalMs
        : std::max(0, config.collect.defaultIntervalMs);
    if (config.protocol.type == "modbus_tcp") {
        if (useMock) {
            throw std::invalid_argument("--mock is not supported for modbus_tcp");
        }
        modbusClient = std::make_shared<ModbusTcpClient>(
            config.protocol.tcp,
            config.collect.maxRequestRegisters
        );
    } else if (config.protocol.type == "dlt645_2007") {
        SerialPortOptions serialOptions;
        serialOptions.device = config.protocol.transport.serialPort;
        serialOptions.baudRate = config.protocol.transport.baudRate;
        serialOptions.dataBits = config.protocol.transport.dataBits;
        serialOptions.stopBits = config.protocol.transport.stopBits;
        serialOptions.parity = config.protocol.transport.parity;
        serialOptions.timeoutMs = config.protocol.transport.timeoutMs;
        serialOptions.maxRequestRegisters = config.collect.maxRequestRegisters;
        serialOptions.frameIntervalMs = frameIntervalMs;
        serialOptions.readRetryCount = std::max(0, config.protocol.transport.readRetryCount);

        std::shared_ptr<ISerialPort> serialPort;
#ifdef _WIN32
        useMock = true;
#endif
        if (useMock) {
            auto mock = std::make_shared<MockSerialPort>();
            serialPort = mock;
        }
#ifndef _WIN32
        else {
            serialPort = std::make_shared<PosixSerialPort>(serialOptions);
        }
#endif
        dlt645Client = std::make_shared<Dlt645Client>(serialPort, serialOptions);
    } else {
        SerialPortOptions serialOptions;
        serialOptions.device = config.protocol.transport.serialPort;
        serialOptions.baudRate = config.protocol.transport.baudRate;
        serialOptions.dataBits = config.protocol.transport.dataBits;
        serialOptions.stopBits = config.protocol.transport.stopBits;
        serialOptions.parity = config.protocol.transport.parity;
        serialOptions.timeoutMs = config.protocol.transport.timeoutMs;
        serialOptions.maxRequestRegisters = config.collect.maxRequestRegisters;
        serialOptions.frameIntervalMs = frameIntervalMs;
        serialOptions.readRetryCount = std::max(0, config.protocol.transport.readRetryCount);

        std::shared_ptr<ISerialPort> serialPort;
#ifdef _WIN32
        useMock = true;
#endif
        if (useMock) {
            auto mock = std::make_shared<MockSerialPort>();
            mock->setRegister(0, 2205);
            mock->setRegister(120, 1);
            serialPort = mock;
        }
#ifndef _WIN32
        else {
            serialPort = std::make_shared<PosixSerialPort>(serialOptions);
        }
#endif
        modbusClient = std::make_shared<ModbusRtuClient>(serialPort, serialOptions);
    }
    if (!config.memoryStore.sharedMemoryName.empty()) {
        MemoryPointStore::cleanupOrphanedSegment(config.memoryStore.sharedMemoryName);
    }
    MemoryPointStore store(config.memoryStore);

    GatewayDaemon daemon(
        config,
        store,
        modbusClient,
        dlt645Client,
        nullptr,
        nullptr,
        appConfig.systemMonitor.realtimeMeterLeaseFile
    );
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    daemon.start();
    const auto runtimeMeterCount = config.meters.empty() ? 1 : config.meters.size();
    std::cout << "gateway daemon started"
              << " config=" << configPath
              << " appConfig=" << appConfigPath
              << " meters=" << runtimeMeterCount
              << " sharedMemory=" << config.memoryStore.sharedMemoryName
              << " sqlite=" << config.memoryStore.sqlitePath
              << " mqtt=disabled"
              << " mode=" << (useMock ? "mock" : config.protocol.type)
              << std::endl;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    daemon.stop();
    std::cout << "gateway daemon stopped" << std::endl;
}
