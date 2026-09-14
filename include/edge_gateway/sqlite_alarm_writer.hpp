#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

class SqliteAlarmWriter {
public:
    using MaintenanceClock = std::chrono::steady_clock;
    explicit SqliteAlarmWriter(std::string dbPath, std::string libraryPath = "",
        int retentionDays = PointHistoryConfig{}.retentionDays);
    ~SqliteAlarmWriter();

    SqliteAlarmWriter(const SqliteAlarmWriter&) = delete;
    SqliteAlarmWriter& operator=(const SqliteAlarmWriter&) = delete;

    void writeEvents(const std::vector<AlarmEvent>& events);
    void cleanupExpiredEvents(std::int64_t nowMs,
        MaintenanceClock::time_point now = MaintenanceClock::now());

private:
    void loadLibrary();
    void openDatabase();
    void ensureSchema();
    void closeDatabase();
    void unloadLibrary();

    std::string dbPath_;
    std::string libraryPath_;
    int retentionDays_;
    MaintenanceClock::time_point nextCleanup_{};
    void* libraryHandle_ = nullptr;
    void* databaseHandle_ = nullptr;
};

}  // namespace edge_gateway
