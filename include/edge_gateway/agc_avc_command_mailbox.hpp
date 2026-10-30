#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

// Who may commit AGC/AVC dispatch commands into the shared command mailbox. Authorization comes from
// the ingress that received the request (MQTT command topic = platform, authenticated direct
// maintenance = maintenance) and the server-side agcAvc.commandMailbox policy; a request's "source"
// text is audit only. Returns an empty string when allowed, else the refusal reason.
std::string agcAvcMailboxRefusal(
    bool enabled,
    bool shadowMode,
    bool submitWrites,
    const AgcAvcCommandMailboxPolicy& policy,
    LatestValueOrigin ingress
);

// Consumer side: a mailbox value counts only if it was committed by an ingress the policy authorizes
// in the current mode (shadow-test values never drive submitWrites).
bool agcAvcMailboxAccepts(const AgcAvcConfig& config, LatestValueOrigin origin);

const char* latestValueOriginName(LatestValueOrigin origin);

struct AgcAvcCommandMailboxRuntime {
    bool configured = false;
    bool enabled = false;
    bool shadowMode = true;
    bool submitWrites = false;
    AgcAvcCommandMailboxPolicy policy;
    std::uint32_t sequenceIndex = 0;
    std::vector<std::uint32_t> commandIndexes;

    bool contains(std::uint32_t index) const;
    // Empty when `ingress` may commit mailbox commands now (see agcAvcMailboxRefusal).
    std::string refusal(LatestValueOrigin ingress) const;
};

AgcAvcCommandMailboxRuntime appendSiblingAgcAvcRuntime(
    const std::string& primaryAppConfigPath,
    const DeviceIdentity& identity,
    std::vector<DeviceConfig>& deviceConfigs,
    std::vector<std::string>& sharedMemoryNames
);

}  // namespace edge_gateway
