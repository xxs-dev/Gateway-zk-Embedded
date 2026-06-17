#include <chrono>
#include <csignal>
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#ifndef _WIN32
#include <sys/prctl.h>
#endif

#include "edge_gateway/common/command_executor_interface.hpp"
#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/gateway_daemon.hpp"
#include "edge_gateway/iec_client.hpp"
#include "edge_gateway/iec_collector.hpp"
#include "edge_gateway/iec_command_executor.hpp"
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

bool isTcpIecMode(const edge_gateway::DeviceConfig& config) {
    const auto type = config.protocol.type;
    return type == "iec104" || type == "iec103_tcp" ||
        (type == "iec103" && config.protocol.iec.transportMode == "tcp");
}

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

}  // namespace

int main(int argc, char* argv[]) {
    using namespace edge_gateway;

    std::string configPath = "config/runtime/devices/device_iec104_example.json";
    std::string appConfigPath = "config/runtime/apps/mqtt-service.json";
    bool useMock = false;
    bool once = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--mock") {
            useMock = true;
        } else if (arg == "--once") {
            once = true;
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
    if (config.protocol.type != "iec104" && config.protocol.type != "iec101" &&
        config.protocol.type != "iec103" && config.protocol.type != "iec103_tcp" &&
        config.protocol.type != "iec103_serial") {
        throw std::invalid_argument("IecDriver requires protocol.type=iec104, iec101, iec103, iec103_tcp or iec103_serial");
    }

    const auto tcpMode = isTcpIecMode(config);
    auto processToken = tcpMode
        ? (config.protocol.type + "_" + std::to_string(config.protocol.tcp.port))
        : basenameOf(config.protocol.transport.serialPort);
    if (processToken.empty()) {
        processToken = basenameOf(configPath);
    }
    setProcessName("iec-" + sanitizeProcessToken(processToken));

    std::shared_ptr<IecClient> iecClient;
    if (tcpMode) {
        if (useMock) {
            throw std::invalid_argument("--mock is not supported for IEC TCP");
        }
        iecClient = std::make_shared<IecTcpClient>(config.protocol.type, config.protocol.tcp, config.protocol.iec);
    } else {
        SerialPortOptions serialOptions;
        serialOptions.device = config.protocol.transport.serialPort;
        serialOptions.baudRate = config.protocol.transport.baudRate;
        serialOptions.dataBits = config.protocol.transport.dataBits;
        serialOptions.stopBits = config.protocol.transport.stopBits;
        serialOptions.parity = config.protocol.transport.parity;
        serialOptions.timeoutMs = config.protocol.transport.timeoutMs;
        serialOptions.frameIntervalMs = config.protocol.transport.frameIntervalMs >= 0
            ? config.protocol.transport.frameIntervalMs
            : std::max(0, config.collect.defaultIntervalMs);
        serialOptions.readRetryCount = std::max(0, config.protocol.transport.readRetryCount);

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
        iecClient = std::make_shared<IecSerialClient>(config.protocol.type, serialPort, serialOptions, config.protocol.iec);
    }

    if (!config.memoryStore.sharedMemoryName.empty()) {
        MemoryPointStore::cleanupOrphanedSegment(config.memoryStore.sharedMemoryName);
    }
    MemoryPointStore store(config.memoryStore);
    GatewayDaemon daemon(
        config,
        store,
        [iecClient](const DeviceConfig& runtimeConfig, MemoryPointStore& runtimeStore) {
            return std::unique_ptr<ICollector>(
                new IecCollector(runtimeConfig, runtimeStore, iecClient)
            );
        },
        [iecClient](const DeviceConfig& runtimeConfig, MemoryPointStore& runtimeStore) {
            return std::unique_ptr<ICommandExecutor>(
                new IecCommandExecutor(runtimeConfig, runtimeStore, iecClient)
            );
        },
        nullptr,
        nullptr,
        appConfig.systemMonitor.realtimeMeterLeaseFile
    );

    if (once) {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        daemon.collectOnce(now);
        daemon.flushPersistentOnce();
        return 0;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    daemon.start();
    const auto runtimeMeterCount = config.meters.empty() ? 1 : config.meters.size();
    std::cout << "iec driver started"
              << " config=" << configPath
              << " appConfig=" << appConfigPath
              << " protocol=" << config.protocol.type
              << " transportMode=" << (tcpMode ? "tcp" : "serial")
              << " meters=" << runtimeMeterCount
              << " sharedMemory=" << config.memoryStore.sharedMemoryName
              << " sqlite=" << config.memoryStore.sqlitePath
              << " mqtt=disabled"
              << std::endl;

    std::int64_t lastClockSyncMs = 0;
    while (g_running) {
        if (config.protocol.type == "iec104" && config.protocol.iec.clockSyncIntervalSec > 0) {
            const auto now = currentTimeMs();
            const auto intervalMs = static_cast<std::int64_t>(config.protocol.iec.clockSyncIntervalSec) * 1000;
            if (lastClockSyncMs == 0 || now - lastClockSyncMs >= intervalMs) {
                try {
                    iecClient->synchronizeClock(now);
                    lastClockSyncMs = now;
                } catch (const std::exception& ex) {
                    std::cerr << "IEC104 clock sync failed: " << ex.what() << std::endl;
                    lastClockSyncMs = now;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    daemon.stop();
    std::cout << "iec driver stopped" << std::endl;
    return 0;
}
