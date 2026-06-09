#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#ifndef _WIN32
#include <sys/prctl.h>
#endif

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/dio_collector.hpp"
#include "edge_gateway/dio_command_executor.hpp"
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
    auto config = ConfigLoader::loadFromFile(configPath, identity);
    config.mqttDriver = appConfig.mqttDriver;
    if (config.protocol.type != "local_dio") {
        throw std::invalid_argument("DioDriver requires protocol.type=local_dio");
    }

    setProcessName("dio-" + sanitizeProcessToken(basenameOf(configPath)));

    auto gpioPort = std::make_shared<SysfsGpioPort>(config.protocol.gpioBasePath);
    MemoryPointStore store(config.memoryStore);
    GatewayDaemon daemon(
        config,
        store,
        [gpioPort](const DeviceConfig& runtimeConfig, MemoryPointStore& runtimeStore) {
            return std::unique_ptr<ICollector>(
                new DioCollector(runtimeConfig, runtimeStore, gpioPort)
            );
        },
        [gpioPort](const DeviceConfig& runtimeConfig, MemoryPointStore& runtimeStore) {
            return std::unique_ptr<ICommandExecutor>(
                new DioCommandExecutor(runtimeConfig, runtimeStore, gpioPort)
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
              << " mqtt=disabled"
              << " mode=local_dio"
              << std::endl;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    daemon.stop();
    std::cout << "dio driver stopped" << std::endl;
    return 0;
}
