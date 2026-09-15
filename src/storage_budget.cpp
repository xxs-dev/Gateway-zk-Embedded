#include "edge_gateway/storage_budget.hpp"

#include <limits>
#include <stdexcept>
#include <sys/stat.h>

#ifndef _WIN32
#include <sys/statvfs.h>
#endif

namespace edge_gateway {
namespace {
bool add(std::uint64_t& total, std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
    total += value;
    return true;
}

std::uint64_t fileSizeIfPresent(const std::string& path) {
    struct stat info {};
    if (stat(path.c_str(), &info) != 0) return 0;
    if (!S_ISREG(info.st_mode) || info.st_size < 0) {
        throw std::runtime_error("storage budget sidecar is not a regular file: " + path);
    }
    return static_cast<std::uint64_t>(info.st_size);
}

std::uint64_t databaseFootprint(const std::string& path) {
    std::uint64_t total = 0;
    for (const auto* suffix : {"", "-wal", "-shm", "-journal"}) {
        const auto size = fileSizeIfPresent(path + suffix);
        if (!add(total, size)) throw std::overflow_error("storage budget footprint overflow");
    }
    return total;
}

std::uint64_t filesystemAvailable(const std::string& path) {
#ifdef _WIN32
    (void)path;
    return std::numeric_limits<std::uint64_t>::max();
#else
    struct statvfs info {};
    if (statvfs(path.c_str(), &info) != 0) {
        // A test double or a not-yet-created database has no file to stat;
        // inspect its parent, which is the filesystem that will receive it.
        const auto separator = path.find_last_of('/');
        const auto parent = separator == std::string::npos ? std::string(".") :
            (separator == 0 ? std::string("/") : path.substr(0, separator));
        if (statvfs(parent.c_str(), &info) != 0) {
            throw std::runtime_error("cannot inspect storage filesystem: " + path);
        }
    }
    const auto unit = static_cast<std::uint64_t>(info.f_frsize ? info.f_frsize : info.f_bsize);
    const auto blocks = static_cast<std::uint64_t>(info.f_bavail);
    if (unit == 0 || blocks > std::numeric_limits<std::uint64_t>::max() / unit) {
        throw std::overflow_error("storage filesystem available bytes overflow");
    }
    return blocks * unit;
#endif
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
    // availableBytes is measured before this write. Reject an oversized write
    // even when the configured reserve is zero, then apply the reserve to the
    // space left after the write without overflowing availableBytes + incoming.
    if (incomingBytes > availableBytes) {
        result.allowed = false;
        result.reason = "insufficient-available-bytes";
        return result;
    }
    if (availableBytes - incomingBytes < budget.minFreeBytes) {
        result.allowed = false;
        result.reason = "min-free-bytes";
    }
    return result;
}

StorageAdmission checkStorageWriteAdmission(
    StorageBudget budget, const std::string& databasePath,
    std::uint64_t incomingBytes, StorageSpaceProbe availableBytesProbe) {
    StorageUsage usage;
    usage.historyBytes = databaseFootprint(databasePath);
    const auto available = availableBytesProbe ? availableBytesProbe(databasePath) : filesystemAvailable(databasePath);
    return checkStorageAdmission(budget, usage, incomingBytes, available);
}

std::uint64_t estimateStorageWriteBytes(std::size_t itemCount) {
    constexpr std::uint64_t kConservativeBytesPerItem = 4096;
    if (itemCount > std::numeric_limits<std::uint64_t>::max() / kConservativeBytesPerItem) {
        throw std::overflow_error("storage write estimate overflow");
    }
    return static_cast<std::uint64_t>(itemCount) * kConservativeBytesPerItem;
}
} // namespace edge_gateway
