#include <chrono>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "edge_gateway/sqlite_sample_writer.hpp"

namespace {

struct sqlite3;
struct sqlite3_stmt;

using sqlite3_open_v2_fn = int (*)(const char*, sqlite3**, int, const char*);
using sqlite3_close_v2_fn = int (*)(sqlite3*);
using sqlite3_prepare_v2_fn = int (*)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
using sqlite3_step_fn = int (*)(sqlite3_stmt*);
using sqlite3_finalize_fn = int (*)(sqlite3_stmt*);
using sqlite3_column_int64_fn = long long (*)(sqlite3_stmt*, int);
using sqlite3_errmsg_fn = const char* (*)(sqlite3*);

constexpr int kSqliteOk = 0;
constexpr int kSqliteRow = 100;
constexpr int kSqliteOpenReadOnly = 0x00000001;
constexpr std::int64_t kMillisecondsPerDay = 24LL * 60 * 60 * 1000;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void* loadSymbol(void* handle, const char* name) {
#ifdef _WIN32
    auto* symbol = reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    auto* symbol = dlsym(handle, name);
#endif
    if (symbol == nullptr) {
        throw std::runtime_error(std::string("failed to load sqlite symbol: ") + name);
    }
    return symbol;
}

class SqliteCounter {
public:
    explicit SqliteCounter(const std::string& path) {
#ifdef _WIN32
        libraryHandle_ = LoadLibraryA("sqlite3.dll");
#else
        libraryHandle_ = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
        if (libraryHandle_ == nullptr) {
            libraryHandle_ = dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
        }
#endif
        if (libraryHandle_ == nullptr) {
            throw std::runtime_error("failed to load sqlite3 library");
        }
        open_ = reinterpret_cast<sqlite3_open_v2_fn>(loadSymbol(libraryHandle_, "sqlite3_open_v2"));
        close_ = reinterpret_cast<sqlite3_close_v2_fn>(loadSymbol(libraryHandle_, "sqlite3_close_v2"));
        prepare_ = reinterpret_cast<sqlite3_prepare_v2_fn>(loadSymbol(libraryHandle_, "sqlite3_prepare_v2"));
        step_ = reinterpret_cast<sqlite3_step_fn>(loadSymbol(libraryHandle_, "sqlite3_step"));
        finalize_ = reinterpret_cast<sqlite3_finalize_fn>(loadSymbol(libraryHandle_, "sqlite3_finalize"));
        columnInt64_ = reinterpret_cast<sqlite3_column_int64_fn>(
            loadSymbol(libraryHandle_, "sqlite3_column_int64")
        );
        error_ = reinterpret_cast<sqlite3_errmsg_fn>(loadSymbol(libraryHandle_, "sqlite3_errmsg"));
        if (open_(path.c_str(), &db_, kSqliteOpenReadOnly, nullptr) != kSqliteOk) {
            throw std::runtime_error("failed to open sqlite test database");
        }
    }

    ~SqliteCounter() {
        if (db_ != nullptr) {
            close_(db_);
        }
        if (libraryHandle_ != nullptr) {
#ifdef _WIN32
            FreeLibrary(static_cast<HMODULE>(libraryHandle_));
#else
            dlclose(libraryHandle_);
#endif
        }
    }

    SqliteCounter(const SqliteCounter&) = delete;
    SqliteCounter& operator=(const SqliteCounter&) = delete;

    std::int64_t count(const std::string& where = std::string()) const {
        const auto sql = std::string("SELECT COUNT(*) FROM point_samples") +
            (where.empty() ? std::string() : " WHERE " + where) + ";";
        sqlite3_stmt* stmt = nullptr;
        if (prepare_(db_, sql.c_str(), -1, &stmt, nullptr) != kSqliteOk) {
            throw std::runtime_error(error_(db_));
        }
        if (step_(stmt) != kSqliteRow) {
            finalize_(stmt);
            throw std::runtime_error(error_(db_));
        }
        const auto result = static_cast<std::int64_t>(columnInt64_(stmt, 0));
        finalize_(stmt);
        return result;
    }

private:
    void* libraryHandle_ = nullptr;
    sqlite3* db_ = nullptr;
    sqlite3_open_v2_fn open_ = nullptr;
    sqlite3_close_v2_fn close_ = nullptr;
    sqlite3_prepare_v2_fn prepare_ = nullptr;
    sqlite3_step_fn step_ = nullptr;
    sqlite3_finalize_fn finalize_ = nullptr;
    sqlite3_column_int64_fn columnInt64_ = nullptr;
    sqlite3_errmsg_fn error_ = nullptr;
};

std::string temporaryDatabasePath() {
    const auto suffix = std::chrono::high_resolution_clock::now().time_since_epoch().count();
#ifdef _WIN32
    return "sqlite_sample_writer_retention_" + std::to_string(suffix) + ".db";
#else
    return "/tmp/sqlite_sample_writer_retention_" + std::to_string(suffix) + ".db";
#endif
}

void removeDatabase(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-journal").c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

void verifyGlobalRetentionCleanup() {
    using edge_gateway::PersistentPointSample;
    using edge_gateway::SqliteSampleWriter;

    const auto path = temporaryDatabasePath();
    removeDatabase(path);
    const std::int64_t referenceTimeMs = 1800000000000LL;
    const std::int64_t cutoff = referenceTimeMs - 30 * kMillisecondsPerDay;
    const std::vector<PersistentPointSample> samples = {
        {101, 1.0, cutoff - 1000},
        {202, 2.0, cutoff - 1},
        {303, 3.0, cutoff},
        {404, 4.0, cutoff + 1},
        {505, 5.0, referenceTimeMs}
    };

    {
        SqliteSampleWriter writer(path, "", 3650);
        writer.writeSamples(samples, referenceTimeMs);
    }
    require(SqliteCounter(path).count() == 5, "retention fixture was not written");

    {
        SqliteSampleWriter writer(path, "", 30);
        writer.writeSamples({}, referenceTimeMs);
        writer.writeSamples({}, referenceTimeMs);

        SqliteCounter counter(path);
        require(counter.count() == 3, "expired samples were not removed globally");
        require(
            counter.count("ts < " + std::to_string(cutoff)) == 0,
            "expired samples remain after cleanup"
        );
        require(
            counter.count("ts = " + std::to_string(cutoff)) == 1,
            "retention cutoff must be inclusive"
        );
        require(
            counter.count("point_index IN (101, 202)") == 0,
            "cleanup must apply to every persisted point index"
        );

        writer.writeSamples({}, referenceTimeMs + kMillisecondsPerDay);
    }

    {
        SqliteCounter finalCounter(path);
        require(finalCounter.count() == 1, "daily cleanup did not advance the retention cutoff");
        require(
            finalCounter.count("point_index = 505") == 1,
            "current history sample should remain after cleanup"
        );
    }
    removeDatabase(path);
}

}  // namespace

int main() {
    try {
        verifyGlobalRetentionCleanup();
        std::cout << "sqlite_sample_writer_retention_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "sqlite_sample_writer_retention_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
