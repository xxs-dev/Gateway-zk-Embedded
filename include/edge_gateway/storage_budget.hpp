#pragma once

#include <cstdint>

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

StorageAdmission checkStorageAdmission(
    StorageBudget budget, StorageUsage usage, std::uint64_t incomingBytes,
    std::uint64_t availableBytes);

} // namespace edge_gateway
