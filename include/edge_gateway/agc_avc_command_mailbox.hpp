#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

struct AgcAvcCommandMailboxRuntime {
    bool configured = false;
    bool enabled = false;
    bool shadowMode = true;
    bool submitWrites = false;
    std::uint32_t sequenceIndex = 0;
    std::vector<std::uint32_t> commandIndexes;

    bool contains(std::uint32_t index) const;
    bool allowsSource(const std::string& source) const;
};

AgcAvcCommandMailboxRuntime appendSiblingAgcAvcRuntime(
    const std::string& primaryAppConfigPath,
    const DeviceIdentity& identity,
    std::vector<DeviceConfig>& deviceConfigs,
    std::vector<std::string>& sharedMemoryNames
);

}  // namespace edge_gateway
