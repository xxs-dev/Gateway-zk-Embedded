#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>

namespace edge_gateway {

// A side-effect-free admission check shared by storage owners. Zero limits disable
// the corresponding check; callers must provide logical bytes including sidecars.
struct StorageBudget {
    std::uint64_t maxBytes = 0;
    std::uint64_t minFreeBytes = 0;
};

struct StorageUsage {
    std::uint64_t historyBytes = 0;
    std::uint64_t controlLedgerBytes = 0;
    std::uint64_t logsBytes = 0;
    std::uint64_t otaBytes = 0;
};

struct StorageAdmission {
    bool allowed = true;
    std::uint64_t usedBytes = 0;
    const char* reason = "ok";
};

using StorageSpaceProbe = std::function<std::uint64_t(const std::string& path)>;

StorageAdmission checkStorageAdmission(
    StorageBudget budget, StorageUsage usage, std::uint64_t incomingBytes,
    std::uint64_t availableBytes);

// Observe one SQLite database and its sidecars before a write. The available
// byte value is measured before the write; a probe is injectable for tests.
StorageAdmission checkStorageWriteAdmission(
    StorageBudget budget, const std::string& databasePath,
    std::uint64_t incomingBytes, StorageSpaceProbe availableBytesProbe = {});

std::uint64_t estimateStorageWriteBytes(std::size_t itemCount);

} // namespace edge_gateway
