#pragma once

#include <cstdint>
#include <string>

namespace edge_gateway {

// Linux only. Caller must stop ALL participants and disable automatic restarts.
// Throws on refusal or IO failure; never unlinks the segment or overwrites a backup.
std::uint32_t migrateOfflinePointStore(
    const std::string& segmentName, const std::string& backupPath, bool offlineConfirmed);

}  // namespace edge_gateway
