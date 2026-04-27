#pragma once

#include <string>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

class SqliteAlarmWriter {
public:
    explicit SqliteAlarmWriter(std::string dbPath, std::string libraryPath = "");
    ~SqliteAlarmWriter();

    SqliteAlarmWriter(const SqliteAlarmWriter&) = delete;
    SqliteAlarmWriter& operator=(const SqliteAlarmWriter&) = delete;

    void writeEvents(const std::vector<AlarmEvent>& events);

private:
    void loadLibrary();
    void openDatabase();
    void ensureSchema();
    void closeDatabase();
    void unloadLibrary();

    std::string dbPath_;
    std::string libraryPath_;
    void* libraryHandle_ = nullptr;
    void* databaseHandle_ = nullptr;
};

}  // namespace edge_gateway
