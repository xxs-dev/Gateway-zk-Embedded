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

#include "edge_gateway/can_driver_service.hpp"
#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/memory_point_store.hpp"

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

    std::string configPath = "config/runtime/devices/device_can0.json";
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
    if (config.protocol.type != "can_socketcan" && config.protocol.type != "can") {
        throw std::invalid_argument("CanDriver requires protocol.type=can_socketcan");
    }

    auto processToken = config.protocol.can.interfaceName.empty()
        ? basenameOf(configPath)
        : config.protocol.can.interfaceName;
    setProcessName("can-" + sanitizeProcessToken(processToken));

    MemoryPointStore store(config.memoryStore);
    CanDriverService service(config, store, nullptr);

    if (once) {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        service.processWritebackOnce(now);
        service.flushPersistentOnce();
        return 0;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    service.start();
    const auto runtimeMeterCount = config.meters.empty() ? 1 : config.meters.size();
    std::cout << "can driver started"
              << " config=" << configPath
              << " appConfig=" << appConfigPath
              << " meters=" << runtimeMeterCount
              << " interface=" << config.protocol.can.interfaceName
              << " bitrate=" << config.protocol.can.bitrate
              << " sharedMemory=" << config.memoryStore.sharedMemoryName
              << " sqlite=" << config.memoryStore.sqlitePath
              << " mqtt=disabled"
              << std::endl;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    service.stop();
    std::cout << "can driver stopped" << std::endl;
    return 0;
}
