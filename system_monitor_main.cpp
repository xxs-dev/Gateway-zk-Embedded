#include <chrono>
#include <csignal>
#include <cstdlib>
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
#include "edge_gateway/system_monitor_direct_maintenance.hpp"
#include "edge_gateway/system_monitor_points.hpp"
#include "edge_gateway/system_monitor_runtime_discovery.hpp"
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

void setDefaultSharedMutexTimeout() {
#ifndef _WIN32
    if (std::getenv("GATEWAY_SHARED_MUTEX_LOCK_TIMEOUT_SEC") == nullptr) {
        setenv("GATEWAY_SHARED_MUTEX_LOCK_TIMEOUT_SEC", "2", 0);
    }
#endif
}

void addStoreIfAvailable(
    const std::string& name,
    edge_gateway::PointStoreRouter& router,
    std::vector<std::unique_ptr<edge_gateway::MemoryPointStore>>& stores
) {
    try {
        stores.emplace_back(new edge_gateway::MemoryPointStore(name));
        router.addStore(name, *stores.back());
    } catch (const std::exception& ex) {
        std::cerr << "system monitor skipped shared memory "
                  << name
                  << " during startup: "
                  << ex.what()
                  << std::endl;
    }
}

class StdoutMqttDriverPublisher : public edge_gateway::IMqttDriverPublisher {
public:
    void publishFullSnapshot(const std::string&, const std::vector<edge_gateway::StoredPointValue>&, const std::string&) override {}
    void publishAlarm(const std::string&, std::uint32_t, const edge_gateway::StoredPointValue&, const std::string&, bool) override {}
    void publishOnDemand(const std::string&, const std::vector<edge_gateway::StoredPointValue>&, const std::string&) override {}
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
    std::string systemMonitorSharedMemoryName = system_monitor_points::kSharedMemoryName;
    bool directMaintenanceDisabled = false;
    bool once = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--app-config" && i + 1 < argc) {
            appConfigPath = argv[++i];
        } else if (arg == "--system-monitor-shared-memory" && i + 1 < argc) {
            systemMonitorSharedMemoryName = argv[++i];
        } else if (arg == "--no-direct-maintenance") {
            directMaintenanceDisabled = true;
        } else if (arg == "--once") {
            once = true;
        }
    }

    auto appConfig = ConfigLoader::loadAppConfigFromFile(appConfigPath);
    setProcessName("modbus-sysm-" + sanitizeProcessToken(basenameOf(appConfigPath)));
    setDefaultSharedMutexTimeout();

    DeviceIdentity identity;
    if (!appConfig.identityConfigFile.empty()) {
        identity = ConfigLoader::loadDeviceIdentityFromFile(appConfig.identityConfigFile);
    }
    const auto runtimeDependencies = discoverSystemMonitorRuntimeDependencies(appConfigPath, appConfig);
    for (const auto& warning : runtimeDependencies.warnings) {
        std::cerr << "system monitor skipped sibling app config " << warning << std::endl;
    }
    auto deviceConfigs = ConfigLoader::loadMany(appConfig.deviceConfigFiles, identity);
    std::unordered_set<std::string> primaryDeviceFiles(
        appConfig.deviceConfigFiles.begin(),
        appConfig.deviceConfigFiles.end()
    );
    for (const auto& file : runtimeDependencies.deviceConfigFiles) {
        if (primaryDeviceFiles.find(file) != primaryDeviceFiles.end()) {
            continue;
        }
        try {
            deviceConfigs.push_back(ConfigLoader::loadFromFile(file, identity));
        } catch (const std::exception& ex) {
            std::cerr << "system monitor skipped sibling device config "
                      << file << ": " << ex.what() << std::endl;
        }
    }
    std::vector<std::string> sharedMemoryNames = runtimeDependencies.sharedMemoryNames;
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
    router.setPowerControlOwnershipFile(appConfig.mqttDriver.powerControlOwnershipFile, "system-monitor");
    std::vector<std::unique_ptr<edge_gateway::MemoryPointStore>> stores;
    stores.reserve(sharedMemoryNames.size());
    for (const auto& name : sharedMemoryNames) {
        addStoreIfAvailable(name, router, stores);
    }
    router.addRoutesFromDeviceConfigs(deviceConfigs, appConfig.mqttDriver.sharedMemoryName);
    const auto machineCode = !identity.machineCode.empty()
        ? identity.machineCode
        : (deviceConfigs.empty() ? std::string() : deviceConfigs.front().machineCode);
    for (const auto& cameraService : runtimeDependencies.cameraServices) {
        router.addRoutesFromCameraServiceConfig(cameraService, machineCode);
    }
    stores.emplace_back(new MemoryPointStore(systemMonitorSharedMemoryName));
    router.addStore(systemMonitorSharedMemoryName, *stores.back());
    system_monitor_points::registerStorePoints(*stores.back(), machineCode);
    system_monitor_points::addRoutes(router, machineCode, systemMonitorSharedMemoryName);

    if (!machineCode.empty()) {
        appConfig.mqtt.topicMachineCode = machineCode;
        appConfig.mqtt.clientId = machineCode;
    }
    if (!appConfig.mqtt.clientId.empty()) {
        appConfig.mqtt.clientId += "_system_monitor";
    } else {
        appConfig.mqtt.clientId = "system-monitor";
    }
    const auto& configFiles = runtimeDependencies.configFiles;
    std::shared_ptr<IMqttDriverPublisher> publisher;
    if (appConfig.mqtt.enabled) {
        publisher = std::make_shared<BuiltinMqttDriverPublisher>(appConfig.mqtt);
    } else {
        publisher = std::make_shared<StdoutMqttDriverPublisher>();
    }
    auto monitorConfig = appConfig.systemMonitor;
    monitorConfig.scadaUpperComputerSafety.priorityControlLeaseFile =
        appConfig.mqttDriver.priorityControlLeaseFile;
    monitorConfig.scadaUpperComputerSafety.priorityControlLeaseTtlMs =
        appConfig.mqttDriver.priorityControlLeaseTtlMs;
    SystemMonitorService service(
        std::move(monitorConfig),
        appConfig.mqtt,
        publisher,
        machineCode,
        configFiles,
        &router
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
    std::thread directMaintenanceThread;
    auto directMaintenanceConfig = appConfig.systemMonitor.directMaintenance;
    if (directMaintenanceDisabled) {
        directMaintenanceConfig.enabled = false;
    }
    if (directMaintenanceConfig.enabled) {
        directMaintenanceThread = std::thread([directMaintenanceConfig]() {
            edge_gateway::system_monitor_direct_maintenance::runFromConfig(directMaintenanceConfig);
        });
    }
    std::cout << "system monitor started"
              << " appConfig=" << appConfigPath
              << " broker=" << (appConfig.mqtt.enabled ? appConfig.mqtt.broker : "disabled")
              << " directMaintenance=" << (directMaintenanceConfig.enabled ? "enabled" : "disabled")
              << std::endl;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    service.stop();
    if (directMaintenanceThread.joinable()) {
        edge_gateway::system_monitor_direct_maintenance::requestStop();
        directMaintenanceThread.join();
    }
    std::cout << "system monitor stopped" << std::endl;
    return 0;
}
