#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace edge_gateway {

// Linux only. Caller must stop ALL participants and disable automatic restarts.
// Throws on refusal or IO failure; never unlinks the segment or overwrites a backup.
std::uint32_t migrateOfflinePointStore(
    const std::string& segmentName, const std::string& backupPath, bool offlineConfirmed);

struct OfflineMigrationOptions {
    bool checkOnly = false;
    bool deduplicateLatest = false;
};

struct LatestDuplicateGroup {
    std::uint32_t index = 0;
    std::uint32_t winnerSlot = 0;
    std::vector<std::uint32_t> removedSlots;
};

// Slot numbers are zero-based; no sample values are returned.
struct OfflineMigrationResult {
    std::uint32_t oldVersion = 0;
    std::uint32_t occupiedBefore = 0;
    std::uint32_t occupiedAfter = 0;
    std::uint32_t removedCount = 0;
    std::vector<LatestDuplicateGroup> duplicateGroups;
};

// Copies v10 into an exclusively created v11 segment. Never modifies the source.
OfflineMigrationResult copyOfflinePointStoreV10ToV11(
    const std::string& sourceName, const std::string& targetName,
    const std::string& backupPath, bool offlineConfirmed);

OfflineMigrationResult migrateOfflinePointStore(
    const std::string& segmentName, const std::string& backupPath, bool offlineConfirmed,
    const OfflineMigrationOptions& options);

}  // namespace edge_gateway
