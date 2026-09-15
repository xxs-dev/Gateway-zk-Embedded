#include "edge_gateway/sqlite_alarm_writer.hpp"
#include "edge_gateway/sqlite_sample_writer.hpp"
#include "edge_gateway/storage_budget.hpp"

#include <dlfcn.h>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
struct sqlite3;

class SqliteApi {
public:
    SqliteApi() {
        library_ = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!library_) library_ = dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
        require(library_ != nullptr, "SQLite unavailable");
        open = load<int (*)(const char*, sqlite3**)>("sqlite3_open");
        close = load<int (*)(sqlite3*)>("sqlite3_close");
        exec = load<int (*)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**)>("sqlite3_exec");
        free = load<void (*)(void*)>("sqlite3_free");
    }
    ~SqliteApi() { if (library_) dlclose(library_); }
    int (*open)(const char*, sqlite3**) = nullptr;
    int (*close)(sqlite3*) = nullptr;
    int (*exec)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**) = nullptr;
    void (*free)(void*) = nullptr;
private:
    template <typename T> T load(const char* name) {
        auto* symbol = dlsym(library_, name);
        require(symbol != nullptr, std::string("missing SQLite symbol: ") + name);
        return reinterpret_cast<T>(symbol);
    }
    void* library_ = nullptr;
    static void require(bool ok, const std::string& message) {
        if (!ok) throw std::runtime_error(message);
    }
};

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

int rows(SqliteApi& api, const std::string& path, const char* table) {
    sqlite3* db = nullptr;
    require(api.open(path.c_str(), &db) == 0, "cannot open SQLite verification database");
    int count = -1;
    std::string query = std::string("SELECT COUNT(*) FROM ") + table + ";";
    char* error = nullptr;
    require(api.exec(db, query.c_str(), [](void* out, int count, char** values, char**) {
        if (count > 0 && values[0]) *static_cast<int*>(out) = std::stoi(values[0]);
        return 0;
    }, &count, &error) == 0, error ? error : "SQLite count failed");
    if (error) api.free(error);
    api.close(db);
    return count;
}

edge_gateway::AlarmEvent alarm() {
    edge_gateway::AlarmEvent event;
    event.eventId = "guard-alarm";
    event.index = 1;
    event.ts = 1000;
    event.alarmType = "high";
    event.machineCode = "machine";
    event.meterCode = "meter";
    event.pointCode = "point";
    return event;
}
}

int main() {
    const auto base = std::string("sqlite_history_storage_guard_") + std::to_string(getpid());
    const auto samplePath = base + "_samples.db";
    const auto alarmPath = base + "_alarms.db";
    std::uint64_t available = 0;
    const auto probe = [&available](const std::string&) { return available; };
    try {
        SqliteApi api;
        {
            edge_gateway::SqliteSampleWriter writer(samplePath, "", 30, 1024, probe);
            available = 4095;
            bool rejected = false;
            try { writer.writeSamples({{1, 1.0, 1000, 1}}); } catch (const std::exception&) { rejected = true; }
            require(rejected && rows(api, samplePath, "point_samples") == 0, "low space must reject before sample write");

            available = 5120; // incoming estimate 4096 + exact 1024 reserve
            writer.writeSamples({{1, 1.0, 1000, 1}});
            require(rows(api, samplePath, "point_samples") == 1, "sample write did not recover");

            available = 5119;
            rejected = false;
            try { writer.writeSamples({{2, 2.0, 2000, 2}}); } catch (const std::exception&) { rejected = true; }
            require(rejected && rows(api, samplePath, "point_samples") == 1, "rejection deleted existing samples");
        }
        {
            edge_gateway::SqliteAlarmWriter writer(alarmPath, "", 30, 1024, probe);
            available = 4095;
            bool rejected = false;
            try { writer.writeEvents({alarm()}); } catch (const std::exception&) { rejected = true; }
            require(rejected && rows(api, alarmPath, "alarm_events") == 0, "low space must reject before alarm write");
            available = 5120;
            writer.writeEvents({alarm()});
            require(rows(api, alarmPath, "alarm_events") == 1, "alarm write did not recover");
        }

        // Confirm the same observation includes SQLite sidecars, not only the main DB.
        const auto beforeSidecar = edge_gateway::checkStorageWriteAdmission(
            {0, 0}, samplePath, 0, [](const std::string&) { return std::numeric_limits<std::uint64_t>::max(); });
        {
            std::ofstream sidecar(samplePath + "-wal", std::ios::binary);
            sidecar << std::string(1234, 'x');
        }
        const auto afterSidecar = edge_gateway::checkStorageWriteAdmission(
            {0, 0}, samplePath, 0, [](const std::string&) { return std::numeric_limits<std::uint64_t>::max(); });
        require(afterSidecar.usedBytes == beforeSidecar.usedBytes + 1234,
            "SQLite sidecar footprint delta was not exact");

        // Exercise the real filesystem probe for an existing database and a
        // not-yet-created database whose parent already exists.
        const auto actual = edge_gateway::checkStorageWriteAdmission({0, 0}, samplePath, 0);
        require(actual.allowed, "real statvfs path should be available");
        const auto missingPath = base + "_missing.db";
        const auto missing = edge_gateway::checkStorageWriteAdmission({0, 0}, missingPath, 0);
        require(missing.allowed && missing.usedBytes == 0,
            "missing database with an existing parent should use parent filesystem");

#ifndef _WIN32
        const auto blockerPath = base + "_not_a_directory";
        {
            std::ofstream blocker(blockerPath, std::ios::binary);
            blocker << 'x';
        }
        bool notDirectoryRejected = false;
        try {
            (void)edge_gateway::checkStorageWriteAdmission({0, 0}, blockerPath + "/db", 0);
        } catch (const std::exception&) {
            notDirectoryRejected = true;
        }
        require(notDirectoryRejected, "ENOTDIR must fail closed instead of falling back");
        std::remove(blockerPath.c_str());
#endif

        for (const auto& path : {samplePath, alarmPath}) {
            for (const auto* suffix : {"", "-wal", "-shm", "-journal"}) std::remove((path + suffix).c_str());
        }
        std::remove(missingPath.c_str());
        std::cout << "sqlite history storage guard: 11 assertions PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
