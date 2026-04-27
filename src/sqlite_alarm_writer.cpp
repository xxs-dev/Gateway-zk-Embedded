#include "edge_gateway/sqlite_alarm_writer.hpp"

#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace edge_gateway {

namespace {

struct sqlite3;
struct sqlite3_stmt;

using sqlite3_open_v2_fn = int (*)(const char*, sqlite3**, int, const char*);
using sqlite3_close_v2_fn = int (*)(sqlite3*);
using sqlite3_exec_fn = int (*)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
using sqlite3_prepare_v2_fn = int (*)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
using sqlite3_bind_int_fn = int (*)(sqlite3_stmt*, int, int);
using sqlite3_bind_int64_fn = int (*)(sqlite3_stmt*, int, long long);
using sqlite3_bind_double_fn = int (*)(sqlite3_stmt*, int, double);
using sqlite3_bind_text_fn = int (*)(sqlite3_stmt*, int, const char*, int, void (*)(void*));
using sqlite3_step_fn = int (*)(sqlite3_stmt*);
using sqlite3_finalize_fn = int (*)(sqlite3_stmt*);
using sqlite3_errmsg_fn = const char* (*)(sqlite3*);
using sqlite3_free_fn = void (*)(void*);

constexpr int kSqliteOk = 0;
constexpr int kSqliteDone = 101;
constexpr int kSqliteOpenReadWrite = 0x00000002;
constexpr int kSqliteOpenCreate = 0x00000004;

sqlite3_open_v2_fn g_sqlite3_open_v2 = nullptr;
sqlite3_close_v2_fn g_sqlite3_close_v2 = nullptr;
sqlite3_exec_fn g_sqlite3_exec = nullptr;
sqlite3_prepare_v2_fn g_sqlite3_prepare_v2 = nullptr;
sqlite3_bind_int_fn g_sqlite3_bind_int = nullptr;
sqlite3_bind_int64_fn g_sqlite3_bind_int64 = nullptr;
sqlite3_bind_double_fn g_sqlite3_bind_double = nullptr;
sqlite3_bind_text_fn g_sqlite3_bind_text = nullptr;
sqlite3_step_fn g_sqlite3_step = nullptr;
sqlite3_finalize_fn g_sqlite3_finalize = nullptr;
sqlite3_errmsg_fn g_sqlite3_errmsg = nullptr;
sqlite3_free_fn g_sqlite3_free = nullptr;

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

std::string sqliteError(sqlite3* db) {
    if (db == nullptr || g_sqlite3_errmsg == nullptr) {
        return "sqlite error";
    }
    return g_sqlite3_errmsg(db);
}

void execOrThrow(sqlite3* db, const char* sql) {
    char* errorMessage = nullptr;
    const auto rc = g_sqlite3_exec(db, sql, nullptr, nullptr, &errorMessage);
    if (rc != kSqliteOk) {
        std::string message = errorMessage != nullptr ? errorMessage : sqliteError(db);
        if (errorMessage != nullptr && g_sqlite3_free != nullptr) {
            g_sqlite3_free(errorMessage);
        }
        throw std::runtime_error(message);
    }
}

}  // namespace

SqliteAlarmWriter::SqliteAlarmWriter(std::string dbPath, std::string libraryPath)
    : dbPath_(std::move(dbPath)), libraryPath_(std::move(libraryPath)) {
    loadLibrary();
    openDatabase();
    ensureSchema();
}

SqliteAlarmWriter::~SqliteAlarmWriter() {
    closeDatabase();
    unloadLibrary();
}

void SqliteAlarmWriter::writeEvents(const std::vector<AlarmEvent>& events) {
    if (events.empty()) {
        return;
    }

    auto* db = static_cast<sqlite3*>(databaseHandle_);
    execOrThrow(db, "BEGIN IMMEDIATE TRANSACTION;");

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO alarm_events(point_index, ts, alarm_type, active, threshold, value, quality, stale, persist_value, gateway_code, device_code, point_code) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    if (g_sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != kSqliteOk) {
        execOrThrow(db, "ROLLBACK;");
        throw std::runtime_error(sqliteError(db));
    }

    try {
        for (const auto& event : events) {
            if (g_sqlite3_bind_int(stmt, 1, static_cast<int>(event.index)) != kSqliteOk ||
                g_sqlite3_bind_int64(stmt, 2, static_cast<long long>(event.ts)) != kSqliteOk ||
                g_sqlite3_bind_text(stmt, 3, event.alarmType.c_str(), -1, nullptr) != kSqliteOk ||
                g_sqlite3_bind_int(stmt, 4, event.active ? 1 : 0) != kSqliteOk ||
                g_sqlite3_bind_double(stmt, 5, event.threshold) != kSqliteOk ||
                g_sqlite3_bind_double(stmt, 6, event.value) != kSqliteOk ||
                g_sqlite3_bind_int(stmt, 7, event.quality) != kSqliteOk ||
                g_sqlite3_bind_int(stmt, 8, event.stale ? 1 : 0) != kSqliteOk ||
                g_sqlite3_bind_text(stmt, 9, event.persistValue.c_str(), -1, nullptr) != kSqliteOk ||
                g_sqlite3_bind_text(stmt, 10, event.machineCode.c_str(), -1, nullptr) != kSqliteOk ||
                g_sqlite3_bind_text(stmt, 11, event.meterCode.c_str(), -1, nullptr) != kSqliteOk ||
                g_sqlite3_bind_text(stmt, 12, event.pointCode.c_str(), -1, nullptr) != kSqliteOk) {
                throw std::runtime_error(sqliteError(db));
            }
            if (g_sqlite3_step(stmt) != kSqliteDone) {
                throw std::runtime_error(sqliteError(db));
            }
            g_sqlite3_finalize(stmt);
            stmt = nullptr;
            if (g_sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != kSqliteOk) {
                throw std::runtime_error(sqliteError(db));
            }
        }
        g_sqlite3_finalize(stmt);
        execOrThrow(db, "COMMIT;");
    } catch (...) {
        if (stmt != nullptr) {
            g_sqlite3_finalize(stmt);
        }
        execOrThrow(db, "ROLLBACK;");
        throw;
    }
}

void SqliteAlarmWriter::loadLibrary() {
    if (libraryHandle_ != nullptr) {
        return;
    }

#ifdef _WIN32
    if (!libraryPath_.empty()) {
        libraryHandle_ = LoadLibraryA(libraryPath_.c_str());
    }
    if (libraryHandle_ == nullptr) {
        libraryHandle_ = LoadLibraryA("sqlite3.dll");
    }
#else
    if (!libraryPath_.empty()) {
        libraryHandle_ = dlopen(libraryPath_.c_str(), RTLD_NOW | RTLD_LOCAL);
    }
    if (libraryHandle_ == nullptr) {
        libraryHandle_ = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
    }
    if (libraryHandle_ == nullptr) {
        libraryHandle_ = dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
    }
#endif
    if (libraryHandle_ == nullptr) {
        throw std::runtime_error("failed to load sqlite3 library");
    }

    g_sqlite3_open_v2 = reinterpret_cast<sqlite3_open_v2_fn>(loadSymbol(libraryHandle_, "sqlite3_open_v2"));
    g_sqlite3_close_v2 = reinterpret_cast<sqlite3_close_v2_fn>(loadSymbol(libraryHandle_, "sqlite3_close_v2"));
    g_sqlite3_exec = reinterpret_cast<sqlite3_exec_fn>(loadSymbol(libraryHandle_, "sqlite3_exec"));
    g_sqlite3_prepare_v2 = reinterpret_cast<sqlite3_prepare_v2_fn>(loadSymbol(libraryHandle_, "sqlite3_prepare_v2"));
    g_sqlite3_bind_int = reinterpret_cast<sqlite3_bind_int_fn>(loadSymbol(libraryHandle_, "sqlite3_bind_int"));
    g_sqlite3_bind_int64 = reinterpret_cast<sqlite3_bind_int64_fn>(loadSymbol(libraryHandle_, "sqlite3_bind_int64"));
    g_sqlite3_bind_double = reinterpret_cast<sqlite3_bind_double_fn>(loadSymbol(libraryHandle_, "sqlite3_bind_double"));
    g_sqlite3_bind_text = reinterpret_cast<sqlite3_bind_text_fn>(loadSymbol(libraryHandle_, "sqlite3_bind_text"));
    g_sqlite3_step = reinterpret_cast<sqlite3_step_fn>(loadSymbol(libraryHandle_, "sqlite3_step"));
    g_sqlite3_finalize = reinterpret_cast<sqlite3_finalize_fn>(loadSymbol(libraryHandle_, "sqlite3_finalize"));
    g_sqlite3_errmsg = reinterpret_cast<sqlite3_errmsg_fn>(loadSymbol(libraryHandle_, "sqlite3_errmsg"));
    g_sqlite3_free = reinterpret_cast<sqlite3_free_fn>(loadSymbol(libraryHandle_, "sqlite3_free"));
}

void SqliteAlarmWriter::openDatabase() {
    sqlite3* db = nullptr;
    const auto rc = g_sqlite3_open_v2(
        dbPath_.c_str(),
        &db,
        kSqliteOpenReadWrite | kSqliteOpenCreate,
        nullptr
    );
    if (rc != kSqliteOk) {
        throw std::runtime_error("failed to open sqlite database");
    }
    databaseHandle_ = db;
}

void SqliteAlarmWriter::ensureSchema() {
    auto* db = static_cast<sqlite3*>(databaseHandle_);
    execOrThrow(
        db,
        "CREATE TABLE IF NOT EXISTS alarm_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "point_index INTEGER NOT NULL,"
        "ts INTEGER NOT NULL,"
        "alarm_type TEXT NOT NULL,"
        "active INTEGER NOT NULL,"
        "threshold REAL NOT NULL,"
        "value REAL NOT NULL,"
        "quality INTEGER NOT NULL,"
        "stale INTEGER NOT NULL,"
        "persist_value TEXT NOT NULL,"
        "gateway_code TEXT NOT NULL,"
        "device_code TEXT NOT NULL,"
        "point_code TEXT NOT NULL"
        ");"
    );
}

void SqliteAlarmWriter::closeDatabase() {
    if (databaseHandle_ != nullptr) {
        g_sqlite3_close_v2(static_cast<sqlite3*>(databaseHandle_));
        databaseHandle_ = nullptr;
    }
}

void SqliteAlarmWriter::unloadLibrary() {
    if (libraryHandle_ == nullptr) {
        return;
    }
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(libraryHandle_));
#else
    dlclose(libraryHandle_);
#endif
    libraryHandle_ = nullptr;
}

}  // namespace edge_gateway
