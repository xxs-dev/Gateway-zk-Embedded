#include "edge_gateway/storage_budget.hpp"

#include <limits>

namespace edge_gateway {
namespace {
bool add(std::uint64_t& total, std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
    total += value;
    return true;
}
}

StorageAdmission checkStorageAdmission(
    StorageBudget budget, StorageUsage usage, std::uint64_t incomingBytes,
    std::uint64_t availableBytes) {
    StorageAdmission result;
    if (!add(result.usedBytes, usage.historyBytes) ||
        !add(result.usedBytes, usage.controlLedgerBytes) ||
        !add(result.usedBytes, usage.logsBytes) ||
        !add(result.usedBytes, usage.otaBytes) ||
        !add(result.usedBytes, incomingBytes)) {
        result.allowed = false;
        result.reason = "size-overflow";
        return result;
    }
    if (budget.maxBytes != 0 && result.usedBytes > budget.maxBytes) {
        result.allowed = false;
        result.reason = "max-bytes";
        return result;
    }
    if (budget.minFreeBytes != 0 && availableBytes < budget.minFreeBytes) {
        result.allowed = false;
        result.reason = "min-free-bytes";
    }
    return result;
}
} // namespace edge_gateway
