#include "edge_gateway/agc_avc_command_mailbox.hpp"

#include <algorithm>
#include <fstream>

#include "edge_gateway/config_loader.hpp"

namespace edge_gateway {

namespace {

std::string siblingPath(const std::string& path, const std::string& name) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? name : path.substr(0, pos + 1) + name;
}

bool fileExists(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    return static_cast<bool>(input);
}

void addUnique(std::vector<std::string>& values, const std::string& value) {
    if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

void addIndex(std::vector<std::uint32_t>& indexes, const AgcAvcPointRefConfig& ref) {
    if (ref.index != 0 && std::find(indexes.begin(), indexes.end(), ref.index) == indexes.end()) {
        indexes.push_back(ref.index);
    }
}

}  // namespace

std::string agcAvcMailboxRefusal(
    bool enabled,
    bool shadowMode,
    bool submitWrites,
    const AgcAvcCommandMailboxPolicy& policy,
    LatestValueOrigin ingress
) {
    if (!enabled) {
        return "AGC/AVC is disabled (agcAvc.enabled=false)";
    }
    switch (ingress) {
        case LatestValueOrigin::PlatformDispatch:
            return policy.platformDispatch
                ? std::string()
                : "platform dispatch into the AGC/AVC mailbox is not authorized "
                  "(agcAvc.commandMailbox.platformDispatch=false)";
        case LatestValueOrigin::MaintenanceShadowTest:
            if (!policy.maintenanceShadowTest) {
                return "maintenance shadow-test injection is not authorized "
                       "(agcAvc.commandMailbox.maintenanceShadowTest=false)";
            }
            if (!shadowMode || submitWrites) {
                return "maintenance shadow-test injection requires agcAvc.shadowMode=true and submitWrites=false";
            }
            return std::string();
        case LatestValueOrigin::Ordinary:
            break;
    }
    return "the AGC/AVC mailbox accepts only its authorized ingresses";
}

bool agcAvcMailboxAccepts(const AgcAvcConfig& config, LatestValueOrigin origin) {
    return origin != LatestValueOrigin::Ordinary &&
        agcAvcMailboxRefusal(config.enabled, config.shadowMode, config.submitWrites, config.commandMailbox, origin).empty();
}

const char* latestValueOriginName(LatestValueOrigin origin) {
    switch (origin) {
        case LatestValueOrigin::PlatformDispatch:
            return "platform-dispatch";
        case LatestValueOrigin::MaintenanceShadowTest:
            return "maintenance-shadow-test";
        case LatestValueOrigin::Ordinary:
            break;
    }
    return "ordinary";
}

bool AgcAvcCommandMailboxRuntime::contains(std::uint32_t index) const {
    return configured && std::find(commandIndexes.begin(), commandIndexes.end(), index) != commandIndexes.end();
}

std::string AgcAvcCommandMailboxRuntime::refusal(LatestValueOrigin ingress) const {
    if (!configured) {
        return "AGC/AVC command mailbox is not configured";
    }
    return agcAvcMailboxRefusal(enabled, shadowMode, submitWrites, policy, ingress);
}

AgcAvcCommandMailboxRuntime appendSiblingAgcAvcRuntime(
    const std::string& primaryAppConfigPath,
    const DeviceIdentity& identity,
    std::vector<DeviceConfig>& deviceConfigs,
    std::vector<std::string>& sharedMemoryNames
) {
    AgcAvcCommandMailboxRuntime runtime;
    const auto path = siblingPath(primaryAppConfigPath, "agc-avc-service.json");
    if (!fileExists(path)) {
        return runtime;
    }

    AppConfig app;
    try {
        app = ConfigLoader::loadAppConfigFromFile(path);
    } catch (...) {
        return runtime;
    }
    if (app.runtimeMode != "agc_avc") {
        return runtime;
    }
    runtime.configured = true;
    runtime.enabled = app.agcAvc.enabled;
    runtime.shadowMode = app.agcAvc.shadowMode;
    runtime.submitWrites = app.agcAvc.submitWrites;
    runtime.policy = app.agcAvc.commandMailbox;
    runtime.sequenceIndex = app.agcAvc.agc.commandSequence.index;
    addIndex(runtime.commandIndexes, app.agcAvc.agc.target);
    addIndex(runtime.commandIndexes, app.agcAvc.agc.commandSequence);
    addIndex(runtime.commandIndexes, app.agcAvc.agc.commandTimestamp);
    addIndex(runtime.commandIndexes, app.agcAvc.avc.mode);
    addIndex(runtime.commandIndexes, app.agcAvc.avc.targetQ);
    addIndex(runtime.commandIndexes, app.agcAvc.avc.targetVoltage);
    addIndex(runtime.commandIndexes, app.agcAvc.avc.targetPowerFactor);

    auto mailboxDevices = ConfigLoader::loadMany(app.deviceConfigFiles, identity);
    mailboxDevices.erase(
        std::remove_if(
            mailboxDevices.begin(),
            mailboxDevices.end(),
            [](const DeviceConfig& config) { return config.protocol.type != "agc_avc_virtual"; }
        ),
        mailboxDevices.end()
    );
    deviceConfigs.insert(deviceConfigs.end(), mailboxDevices.begin(), mailboxDevices.end());
    for (const auto& device : mailboxDevices) {
        addUnique(sharedMemoryNames, device.memoryStore.sharedMemoryName);
    }
    addUnique(sharedMemoryNames, app.agcAvc.outputSharedMemoryName);
    for (const auto& name : app.agcAvc.sharedMemoryNames) {
        addUnique(sharedMemoryNames, name);
    }
    return runtime;
}

}  // namespace edge_gateway
