#include <iostream>
#include <chrono>
#include <csignal>
#include <fstream>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/prctl.h>
#endif

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/timing_policy.hpp"
#include "edge_gateway/dlt645_collector.hpp"
#include "edge_gateway/dlt645_client.hpp"
#include "edge_gateway/dlt645_command_executor.hpp"
#include "edge_gateway/gateway_daemon.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/dlt645_mock_serial_port.hpp"
#include "edge_gateway/dlt645_time_sync.hpp"
#include "edge_gateway/dlt645_version.hpp"
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

    std::string configPath = "config/runtime/devices/device_dlt645_multi_meter_1_2.json";
    std::string appConfigPath = "config/runtime/apps/mqtt-service.json";
    bool useMock = false;
    // Diagnostic: --read-address [--address-pattern AAAAAAAAAAAA] prints the address of the single
    // meter answering the wildcard on the configured bus and exits (stop the driver service first).
    bool readAddress = false;
    std::string addressPattern = "AAAAAAAAAAAA";
    // Diagnostic: --time-sync-status prints the gateway clock state and the last broadcast time
    // sync attempt of the running driver (status file); it does not touch the bus.
    bool timeSyncStatus = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--mock") {
            useMock = true;
        } else if (arg == "--read-address") {
            readAddress = true;
        } else if (arg == "--time-sync-status") {
            timeSyncStatus = true;
        } else if (arg == "--address-pattern" && i + 1 < argc) {
            addressPattern = argv[++i];
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
    TimingPolicyResolver::apply(config, &appConfig.timingPolicy);
    config.memoryStore.historyRetentionDays = appConfig.pointHistory.retentionDays;
    config.mqttDriver = appConfig.mqttDriver;
    config.emsCluster = appConfig.emsCluster;
    if (!isDlt645Protocol(config.protocol.type)) {
        throw std::invalid_argument("Dlt645Driver requires protocol.type dlt645_2007 or dlt645_1997");
    }
    const auto version = dlt645VersionOf(config.protocol.type);
    const auto timeSyncStatusFile = dlt645TimeSyncStatusPath(configPath);
    if (timeSyncStatus) {
        const auto wallNowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto clock = dlt645GatewayClockState(wallNowMs);
        std::cout << "timeSync.enabled=" << (config.protocol.dlt645.timeSync.enabled ? "true" : "false")
                  << " intervalMs=" << config.protocol.dlt645.timeSync.intervalMs << std::endl;
        std::cout << "clock.trusted=" << (clock.trusted ? "true" : "false")
                  << " reason=" << (clock.reason.empty() ? "-" : clock.reason)
                  << " kernelRead=" << (clock.kernelRead ? "true" : "false")
                  << " kernelState=" << clock.kernelState
                  << " kernelStatus=0x" << std::hex << clock.kernelStatus << std::dec
                  << " maxErrorUs=" << clock.maxErrorUs
                  << " estErrorUs=" << clock.estErrorUs
                  << " message=" << clock.message << std::endl;
        std::ifstream last(timeSyncStatusFile.c_str());
        std::string line;
        if (last && std::getline(last, line)) {
            std::cout << "lastAttempt=" << line << std::endl;
        } else {
            std::cout << "lastAttempt=none (" << timeSyncStatusFile
                      << " missing: the driver has not run with this configuration since boot)" << std::endl;
        }
        return 0;
    }

    auto processToken = basenameOf(config.protocol.transport.serialPort);
    if (processToken.empty()) {
        processToken = basenameOf(configPath);
    }
    setProcessName("dlt645-" + sanitizeProcessToken(processToken));

    SerialPortOptions serialOptions;
    serialOptions.device = config.protocol.transport.serialPort;
    serialOptions.baudRate = config.protocol.transport.baudRate;
    serialOptions.dataBits = config.protocol.transport.dataBits;
    serialOptions.stopBits = config.protocol.transport.stopBits;
    serialOptions.parity = config.protocol.transport.parity;
    serialOptions.timeoutMs = config.protocol.transport.timeoutMs;
    serialOptions.frameIntervalMs = std::max(0, config.protocol.transport.frameIntervalMs);
    serialOptions.wakeupBytes = config.protocol.transport.wakeupBytes;
    serialOptions.localEcho = config.protocol.transport.localEcho;
    serialOptions.rs485 = config.protocol.transport.rs485;

    std::shared_ptr<ISerialPort> serialPort;
#ifdef _WIN32
    useMock = true;
#endif
    if (useMock) {
        std::vector<std::string> mockAddresses;
        for (const auto& meter : config.meters) {
            if (meter.enabled) mockAddresses.push_back(meter.address);
        }
        serialPort = std::make_shared<Dlt645MockSerialPort>(serialOptions.localEcho, mockAddresses);
    }
#ifndef _WIN32
    else {
        serialPort = std::make_shared<PosixSerialPort>(serialOptions);
    }
#endif

    auto dlt645Client = std::make_shared<Dlt645Client>(serialPort, serialOptions, version);
    if (readAddress) {
        try {
            const auto address = dlt645Client->queryAddress(addressPattern);
            std::cout << "address=" << address << std::endl;
            return 0;
        } catch (const std::exception& ex) {
            std::cerr << "read address failed: " << ex.what() << std::endl;
            return 1;
        }
    }
    MemoryPointStore store(config.memoryStore);
    GatewayDaemon daemon(
        config,
        store,
        [dlt645Client](const DeviceConfig& runtimeConfig, MemoryPointStore& runtimeStore) {
            return std::unique_ptr<ICollector>(
                new Dlt645Collector(runtimeConfig, runtimeStore, dlt645Client)
            );
        },
        [dlt645Client](const DeviceConfig& runtimeConfig, MemoryPointStore& runtimeStore) {
            return std::unique_ptr<ICommandExecutor>(
                new Dlt645CommandExecutor(runtimeConfig, runtimeStore, dlt645Client)
            );
        },
        nullptr,
        nullptr,
        appConfig.systemMonitor.realtimeMeterLeaseFile
    );
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
#ifdef SIGPIPE
    // A peer resetting a TCP connection must fail the send, not kill the driver.
    std::signal(SIGPIPE, SIG_IGN);
#endif

    daemon.start();
    const auto runtimeMeterCount = config.meters.empty() ? 1 : config.meters.size();
    std::cout << "dlt645 driver started"
              << " config=" << configPath
              << " appConfig=" << appConfigPath
              << " meters=" << runtimeMeterCount
              << " sharedMemory=" << config.memoryStore.sharedMemoryName
              << " sqlite=" << config.memoryStore.sqlitePath
              << " mqtt=disabled"
              << " mode=" << (useMock ? "mock" : config.protocol.type)
              << " timeSync=" << (config.protocol.dlt645.timeSync.enabled ? "enabled" : "disabled")
              << std::endl;

    // Broadcast time sync shares the bus client (transaction lock + quiet time) with collection.
    // Every attempt (and "disabled") is written to the status file read by --time-sync-status.
    Dlt645TimeSync timeSync(config.protocol.dlt645.timeSync, dlt645Client, useMock ? std::string() : timeSyncStatusFile);
    while (g_running) {
        const auto steadyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        timeSync.tick(steadyMs);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    daemon.stop();
    std::cout << "dlt645 driver stopped" << std::endl;
    return 0;
}
