#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "edge_gateway/models.hpp"
#include "edge_gateway/storage_budget.hpp"

namespace edge_gateway {

class SqliteSampleWriter {
public:
    using MaintenanceClock = std::chrono::steady_clock;
    static constexpr int kCleanupBatchSize = 512;
    static constexpr std::uint64_t kDefaultMinFreeBytes = 256ULL * 1024ULL * 1024ULL;

    explicit SqliteSampleWriter(std::string dbPath, std::string libraryPath = "", int retentionDays = 30,
        std::uint64_t minFreeBytes = kDefaultMinFreeBytes, StorageSpaceProbe availableBytesProbe = {});
    ~SqliteSampleWriter();

    SqliteSampleWriter(const SqliteSampleWriter&) = delete;
    SqliteSampleWriter& operator=(const SqliteSampleWriter&) = delete;

    void writeSamples(const std::vector<PersistentPointSample>& samples);

    // Background only, serialized with writeSamples by the caller. Failures never undo a write/ACK.
    void cleanupExpiredSamples(std::int64_t nowMs, MaintenanceClock::time_point now = MaintenanceClock::now());

private:
    void loadLibrary();
    void openDatabase();
    void ensureSchema();
    void closeDatabase();
    void unloadLibrary();

    std::string dbPath_;
    std::string libraryPath_;
    int retentionDays_;
    StorageBudget storageBudget_;
    StorageSpaceProbe availableBytesProbe_;
    MaintenanceClock::time_point nextCleanup_ = MaintenanceClock::time_point::min();
    bool enabled_ = false;
    void* libraryHandle_ = nullptr;
    void* databaseHandle_ = nullptr;
};

}  // namespace edge_gateway
