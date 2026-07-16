#include <algorithm>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#ifndef _WIN32
#include <sys/prctl.h>
#endif

#include "edge_gateway/agc_avc_service.hpp"
#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace {

volatile std::sig_atomic_t g_running = 1;

void handleSignal(int) {
    g_running = 0;
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

void setProcessName() {
#ifndef _WIN32
    prctl(PR_SET_NAME, "gateway-agcavc", 0, 0, 0);
#endif
}

}  // namespace

int main(int argc, char* argv[]) {
    using namespace edge_gateway;

    std::string appConfigPath = "config/runtime/apps/agc-avc-service.json";
    bool once = false;
    bool validateOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--app-config" && i + 1 < argc) {
            appConfigPath = argv[++i];
        } else if (arg == "--once") {
            once = true;
        } else if (arg == "--validate") {
            validateOnly = true;
        }
    }

    try {
        setProcessName();
        const auto appConfig = ConfigLoader::loadAppConfigFromFile(appConfigPath);
        if (appConfig.runtimeMode != "agc_avc") {
            throw std::invalid_argument("AgcAvcController requires runtimeMode=agc_avc");
        }
        DeviceIdentity identity;
        if (!appConfig.identityConfigFile.empty()) {
            identity = ConfigLoader::loadDeviceIdentityFromFile(appConfig.identityConfigFile);
        }
        const auto deviceConfigs = ConfigLoader::loadMany(appConfig.deviceConfigFiles, identity);

        std::vector<std::string> sharedMemoryNames;
        std::unordered_set<std::string> seen;
        for (const auto& name : appConfig.agcAvc.sharedMemoryNames) {
            addUnique(sharedMemoryNames, seen, name);
        }
        for (const auto& config : deviceConfigs) {
            addUnique(sharedMemoryNames, seen, config.memoryStore.sharedMemoryName);
        }
        addUnique(sharedMemoryNames, seen, appConfig.agcAvc.outputSharedMemoryName);
        if (sharedMemoryNames.empty()) {
            sharedMemoryNames.push_back("gateway_point_store");
        }

        PointStoreRouter router;
        router.setPowerControlOwnershipFile(appConfig.agcAvc.ownership.leaseFile, "agc-avc");
        std::vector<std::unique_ptr<MemoryPointStore>> stores;
        stores.reserve(sharedMemoryNames.size());
        for (const auto& name : sharedMemoryNames) {
            stores.emplace_back(new MemoryPointStore(name));
            router.addStore(name, *stores.back());
        }
        router.addRoutesFromDeviceConfigs(deviceConfigs, "gateway_point_store");

        PointStoreAgcAvcBus pointBus(router);
        AgcAvcService service(appConfig.agcAvc, pointBus);
        const auto issues = service.validate();
        bool hasErrors = false;
        for (const auto& issue : issues) {
            std::cerr << "[agc-avc] " << issue.severity << " " << issue.path << ": " << issue.message << std::endl;
            hasErrors = hasErrors || issue.severity == "error";
        }
        if (hasErrors) {
            return 2;
        }
        if (validateOnly) {
            std::cout << "[agc-avc] validation passed" << std::endl;
            return 0;
        }
        if (!appConfig.agcAvc.enabled) {
            std::cout << "[agc-avc] module disabled" << std::endl;
            return 0;
        }

        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);
        auto previousState = AgcAvcRuntimeState::Disabled;
        do {
            const auto output = service.tick(nowMs());
            if (output.state != previousState) {
                std::cout << "[agc-avc] state=" << agcAvcRuntimeStateName(output.state)
                          << " reason=" << output.reason << std::endl;
                previousState = output.state;
            }
            if (once) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(std::max(50, appConfig.agcAvc.cycleMs)));
        } while (g_running != 0);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[agc-avc] fatal: " << ex.what() << std::endl;
        return 1;
    }
}
