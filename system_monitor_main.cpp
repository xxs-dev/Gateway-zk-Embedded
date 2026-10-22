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
#include <unistd.h>
#endif

#include "edge_gateway/builtin_mqtt_driver_publisher.hpp"
#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/maintenance_v2.hpp"
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

void printUsage() {
    std::cout <<
        "Usage: SystemMonitor [--app-config FILE] [options]\n"
        "  --app-config FILE                 app config (default config/runtime/apps/mqtt-service.json)\n"
        "  --no-direct-maintenance           do not start the direct maintenance channels\n"
        "  --once                            collect once and exit\n"
        "  --set-maintenance-password        read a new maintenance password from stdin and store its credential\n"
        "  --init-maintenance-tls            create the maintenance v2 device certificate if missing and print fingerprint=...\n"
        "  --maintenance-v2-dir DIR          v2 material directory (default /opt/modbus-gateway/config/maintenance-v2)\n"
        "  --maintenance-v2-state-dir DIR    v2 state directory (default /opt/modbus-gateway/data/maintenance-v2)\n"
        "  --maintenance-v2-services-cmd CMD services adapter for tests (CMD list | state UNIT | restart)\n"
        "  --maintenance-v2-launcher MODE    transaction helper launcher: auto | systemd-run | fork\n"
        "  --maintenance-v2-tx ID --maintenance-v2-tx-mode MODE   (internal) run a config transaction helper\n";
}

std::string absolutePath(const std::string& path) {
#ifndef _WIN32
    if (!path.empty() && path[0] != '/') {
        char cwd[4096];
        if (::getcwd(cwd, sizeof(cwd)) != nullptr) {
            return std::string(cwd) + "/" + path;
        }
    }
#endif
    return path;
}

}  // namespace

int runSystemMonitor(int argc, char* argv[]) {
    using namespace edge_gateway;

    std::string appConfigPath = "config/runtime/apps/mqtt-service.json";
    bool directMaintenanceDisabled = false;
    bool once = false;
    bool setMaintenancePassword = false;
    bool initMaintenanceTls = false;
    std::string transactionId;
    std::string transactionMode = "apply";
    edge_gateway::maintenance_v2::Options v2Options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--app-config" && i + 1 < argc) {
            appConfigPath = argv[++i];
        } else if (arg == "--no-direct-maintenance") {
            directMaintenanceDisabled = true;
        } else if (arg == "--once") {
            once = true;
        } else if (arg == "--set-maintenance-password") {
            setMaintenancePassword = true;
        } else if (arg == "--init-maintenance-tls") {
            initMaintenanceTls = true;
        } else if (arg == "--maintenance-v2-dir" && i + 1 < argc) {
            v2Options.materialDirectory = absolutePath(argv[++i]);
        } else if (arg == "--maintenance-v2-state-dir" && i + 1 < argc) {
            v2Options.stateDirectory = absolutePath(argv[++i]);
        } else if (arg == "--maintenance-v2-services-cmd" && i + 1 < argc) {
            v2Options.servicesCommand = absolutePath(argv[++i]);
        } else if (arg == "--maintenance-v2-launcher" && i + 1 < argc) {
            v2Options.helperLauncher = argv[++i];
        } else if (arg == "--maintenance-v2-tx" && i + 1 < argc) {
            transactionId = argv[++i];
        } else if (arg == "--maintenance-v2-tx-mode" && i + 1 < argc) {
            transactionMode = argv[++i];
        }
    }

    auto appConfig = ConfigLoader::loadAppConfigFromFile(appConfigPath);
    v2Options.helperCommand = {
        "/proc/self/exe",
        "--app-config", absolutePath(appConfigPath),
        "--maintenance-v2-dir", v2Options.materialDirectory,
        "--maintenance-v2-state-dir", v2Options.stateDirectory,
        "--maintenance-v2-launcher", v2Options.helperLauncher
    };
#ifndef _WIN32
    {
        char self[4096];
        const auto length = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (length > 0) {
            self[length] = '\0';
            v2Options.helperCommand[0] = self;
        }
    }
#endif
    if (!v2Options.servicesCommand.empty()) {
        v2Options.helperCommand.push_back("--maintenance-v2-services-cmd");
        v2Options.helperCommand.push_back(v2Options.servicesCommand);
    }
    if (initMaintenanceTls) {
        return edge_gateway::maintenance_v2::initTls(
            appConfig.systemMonitor.directMaintenance, v2Options, std::cout, std::cerr);
    }
    if (!transactionId.empty()) {
        return edge_gateway::maintenance_v2::runTransactionHelper(
            appConfig.systemMonitor.directMaintenance, v2Options, transactionId, transactionMode);
    }
    if (setMaintenancePassword) {
        // Provisioning mode: the password is read from stdin (never from argv/shell history).
        std::string password;
        std::getline(std::cin, password);
        if (!password.empty() && password.back() == '\r') {
            password.pop_back();
        }
        std::string error;
        if (!system_monitor_direct_maintenance::provisionCredential(
                appConfig.systemMonitor.directMaintenance,
                password,
                &error)) {
            std::cerr << "maintenance password provisioning failed: " << error << std::endl;
            return 1;
        }
        std::cout << "maintenance credential written to "
                  << appConfig.systemMonitor.directMaintenance.credentialFile << std::endl;
        return 0;
    }
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
    const auto systemMonitorSharedMemoryName = system_monitor_points::sharedMemoryName();
    stores.emplace_back(new MemoryPointStore(systemMonitorSharedMemoryName));
    router.addStore(systemMonitorSharedMemoryName, *stores.back());
    system_monitor_points::registerStorePoints(*stores.back(), machineCode);
    system_monitor_points::addRoutes(router, machineCode);

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
        publisher = std::make_shared<BuiltinMqttDriverPublisher>(
            appConfig.mqtt,
            MqttPublisherMode::Bidirectional,
            MqttEventOutboxOwnership::External
        );
    } else {
        publisher = std::make_shared<StdoutMqttDriverPublisher>();
    }
    auto monitorConfig = appConfig.systemMonitor;
    // Never AT-probe a serial port that belongs to a configured field interface.
    for (const auto& config : deviceConfigs) {
        if (!config.protocol.transport.serialPort.empty()) {
            monitorConfig.cellular.excludedDevices.push_back(config.protocol.transport.serialPort);
        }
    }
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
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

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
    std::thread maintenanceV2Thread;
    if (!directMaintenanceDisabled && directMaintenanceConfig.v2.enabled) {
        maintenanceV2Thread = std::thread([directMaintenanceConfig, v2Options]() {
            edge_gateway::maintenance_v2::run(directMaintenanceConfig, v2Options);
        });
    }
    std::cout << "system monitor started"
              << " appConfig=" << appConfigPath
              << " broker=" << (appConfig.mqtt.enabled ? appConfig.mqtt.broker : "disabled")
              << " directMaintenance=" << (directMaintenanceConfig.enabled ? "enabled" : "disabled")
              << " maintenanceV2=" << (!directMaintenanceDisabled && directMaintenanceConfig.v2.enabled ? "enabled" : "disabled")
              << std::endl;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    service.stop();
    if (directMaintenanceThread.joinable()) {
        edge_gateway::system_monitor_direct_maintenance::requestStop();
        directMaintenanceThread.join();
    }
    if (maintenanceV2Thread.joinable()) {
        edge_gateway::maintenance_v2::requestStop();
        maintenanceV2Thread.join();
    }
    std::cout << "system monitor stopped" << std::endl;
    return 0;
}

int main(int argc, char* argv[]) {
    try {
        return runSystemMonitor(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << "system monitor failed: " << ex.what() << std::endl;
        return 1;
    }
}
