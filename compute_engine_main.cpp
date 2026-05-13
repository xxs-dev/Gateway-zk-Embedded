#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <sys/prctl.h>
#endif

#include "edge_gateway/compute_engine_service.hpp"
#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/legacy_ems_point_catalog.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/point_store_router.hpp"

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

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

void addUnique(std::vector<std::string>& values, std::unordered_set<std::string>& seen, const std::string& value) {
    if (!value.empty() && seen.insert(value).second) {
        values.push_back(value);
    }
}

std::string outputPointCode(const edge_gateway::ComputeRuleConfig& rule, const edge_gateway::ComputeOutputConfig& output) {
    if (!output.name.empty()) {
        return output.name;
    }
    if (!rule.ruleCode.empty()) {
        return rule.ruleCode + "_out_" + std::to_string(output.index);
    }
    return "compute_out_" + std::to_string(output.index);
}

bool latestOutputMode(const std::string& mode) {
    return mode.empty() || mode == "latestOnly" || mode == "both";
}

std::string legacyPointCode(const edge_gateway::LegacyEmsPoint& point) {
    if (!point.name.empty()) {
        return point.name;
    }
    if (!point.desc.empty()) {
        return point.desc;
    }
    return "legacy_var_" + std::to_string(point.index);
}

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
    setProcessName("gateway-compute-" + sanitizeProcessToken(basenameOf(appConfigPath)));

    DeviceIdentity identity;
    if (!appConfig.identityConfigFile.empty()) {
        identity = ConfigLoader::loadDeviceIdentityFromFile(appConfig.identityConfigFile);
    }
    const auto deviceConfigs = ConfigLoader::loadMany(appConfig.deviceConfigFiles, identity);

    std::string machineCode = identity.machineCode;
    for (const auto& config : deviceConfigs) {
        if (config.machineCode.empty()) {
            continue;
        }
        if (machineCode.empty()) {
            machineCode = config.machineCode;
        } else if (machineCode != config.machineCode) {
            throw std::invalid_argument("compute engine requires a single machineCode across device configs");
        }
    }

    std::vector<std::string> sharedMemoryNames;
    std::unordered_set<std::string> seenSharedMemoryNames;
    for (const auto& name : appConfig.computeEngine.sharedMemoryNames) {
        addUnique(sharedMemoryNames, seenSharedMemoryNames, name);
    }
    for (const auto& name : appConfig.mqttDriver.sharedMemoryNames) {
        addUnique(sharedMemoryNames, seenSharedMemoryNames, name);
    }
    addUnique(sharedMemoryNames, seenSharedMemoryNames, appConfig.mqttDriver.sharedMemoryName);
    for (const auto& config : deviceConfigs) {
        addUnique(sharedMemoryNames, seenSharedMemoryNames, config.memoryStore.sharedMemoryName);
    }
    addUnique(sharedMemoryNames, seenSharedMemoryNames, appConfig.cameraService.sharedMemoryName);
    for (const auto& rule : appConfig.computeEngine.rules) {
        for (const auto& output : rule.outputs) {
            if (!output.sharedMemoryName.empty()) {
                addUnique(sharedMemoryNames, seenSharedMemoryNames, output.sharedMemoryName);
            }
        }
    }
    addUnique(sharedMemoryNames, seenSharedMemoryNames, appConfig.computeEngine.outputDefaultSharedMemoryName);
    if (sharedMemoryNames.empty()) {
        sharedMemoryNames.push_back("gateway_point_store");
    }

    PointStoreRouter router;
    std::vector<std::unique_ptr<MemoryPointStore>> stores;
    stores.reserve(sharedMemoryNames.size());
    for (const auto& name : sharedMemoryNames) {
        stores.emplace_back(new MemoryPointStore(name));
        router.addStore(name, *stores.back());
    }
    router.addRoutesFromDeviceConfigs(deviceConfigs, appConfig.mqttDriver.sharedMemoryName);
    router.addRoutesFromCameraServiceConfig(appConfig.cameraService, machineCode);

    for (const auto& rule : appConfig.computeEngine.rules) {
        if (rule.script.type != "legacyEms") {
            continue;
        }
        const auto catalog = LegacyEmsPointCatalog::loadFromFiles(
            rule.script.legacyGlListFile,
            rule.script.legacyVarListFile,
            rule.script.legacyEncoding.empty() ? std::string("gbk") : rule.script.legacyEncoding
        );
        for (const auto& point : catalog.points()) {
            if (point.index == 0 || router.routeByIndex(point.index)) {
                continue;
            }
            PointStoreRoute route;
            route.index = point.index;
            route.machineCode = machineCode;
            route.meterCode = point.source == LegacyEmsPointSource::Global ? "LEGACY_GL" : "LEGACY_VAR";
            route.pointCode = legacyPointCode(point);
            route.interfaceCode = "legacy-ems";
            route.interfaceType = "compute";
            route.sharedMemoryName = appConfig.computeEngine.outputDefaultSharedMemoryName.empty()
                ? std::string("gateway_point_store")
                : appConfig.computeEngine.outputDefaultSharedMemoryName;
            route.writable = false;
            route.reportOnChange = true;
            route.isStore = false;
            route.persistIntervalSec = 60;
            router.addRoute(route);
        }
    }

    for (const auto& rule : appConfig.computeEngine.rules) {
        for (const auto& output : rule.outputs) {
            if (output.index == 0 || !latestOutputMode(output.mode) || router.routeByIndex(output.index)) {
                continue;
            }
            PointStoreRoute route;
            route.index = output.index;
            route.machineCode = machineCode;
            route.meterCode = "COMPUTE";
            route.pointCode = outputPointCode(rule, output);
            route.interfaceCode = "compute";
            route.interfaceType = "compute";
            route.sharedMemoryName = output.sharedMemoryName.empty()
                ? appConfig.computeEngine.outputDefaultSharedMemoryName
                : output.sharedMemoryName;
            if (route.sharedMemoryName.empty()) {
                route.sharedMemoryName = appConfig.mqttDriver.sharedMemoryName.empty()
                    ? std::string("gateway_point_store")
                    : appConfig.mqttDriver.sharedMemoryName;
            }
            route.writable = false;
            router.addRoute(route);
        }
    }

    if (!appConfig.computeEngine.enabled) {
        std::cout << "compute engine disabled appConfig=" << appConfigPath << std::endl;
        return 0;
    }

    ComputeEngineService service(appConfig.computeEngine, router);
    if (once) {
        service.runOnce(nowMs());
        return 0;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    service.start();
    std::cout << "compute engine started"
              << " appConfig=" << appConfigPath
              << " rules=" << appConfig.computeEngine.rules.size()
              << " shmCount=" << sharedMemoryNames.size()
              << std::endl;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    service.stop();
    std::cout << "compute engine stopped" << std::endl;
    return 0;
}
