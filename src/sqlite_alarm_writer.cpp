#include "edge_gateway/sqlite_alarm_writer.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

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
using sqlite3_reset_fn = int (*)(sqlite3_stmt*);
using sqlite3_clear_bindings_fn = int (*)(sqlite3_stmt*);
using sqlite3_finalize_fn = int (*)(sqlite3_stmt*);
using sqlite3_errmsg_fn = const char* (*)(sqlite3*);
using sqlite3_free_fn = void (*)(void*);
using sqlite3_busy_timeout_fn = int (*)(sqlite3*, int);
using sqlite3_changes_fn = int (*)(sqlite3*);

constexpr int kSqliteOk = 0;
constexpr int kSqliteBusy = 5;
constexpr int kSqliteLocked = 6;
constexpr int kSqliteDone = 101;
constexpr int kSqliteOpenReadWrite = 0x00000002;
constexpr int kSqliteOpenCreate = 0x00000004;
constexpr int kSqliteBusyTimeoutMs = 25;
constexpr int kSqliteRetryAttempts = 3;
constexpr int kSqliteRetryBackoffMs = 10;

sqlite3_open_v2_fn g_sqlite3_open_v2 = nullptr;
sqlite3_close_v2_fn g_sqlite3_close_v2 = nullptr;
sqlite3_exec_fn g_sqlite3_exec = nullptr;
sqlite3_prepare_v2_fn g_sqlite3_prepare_v2 = nullptr;
sqlite3_bind_int_fn g_sqlite3_bind_int = nullptr;
sqlite3_bind_int64_fn g_sqlite3_bind_int64 = nullptr;
sqlite3_bind_double_fn g_sqlite3_bind_double = nullptr;
sqlite3_bind_text_fn g_sqlite3_bind_text = nullptr;
sqlite3_step_fn g_sqlite3_step = nullptr;
sqlite3_reset_fn g_sqlite3_reset = nullptr;
sqlite3_clear_bindings_fn g_sqlite3_clear_bindings = nullptr;
sqlite3_finalize_fn g_sqlite3_finalize = nullptr;
sqlite3_errmsg_fn g_sqlite3_errmsg = nullptr;
sqlite3_free_fn g_sqlite3_free = nullptr;
sqlite3_busy_timeout_fn g_sqlite3_busy_timeout = nullptr;
sqlite3_changes_fn g_sqlite3_changes = nullptr;

class StatementGuard {
public:
    StatementGuard() = default;

    ~StatementGuard() {
        finalize();
    }

    sqlite3_stmt** output() {
        return &statement_;
    }

    sqlite3_stmt* get() const {
        return statement_;
    }

    void finalize() noexcept {
        if (statement_ != nullptr) {
            g_sqlite3_finalize(statement_);
            statement_ = nullptr;
        }
    }

    StatementGuard(const StatementGuard&) = delete;
    StatementGuard& operator=(const StatementGuard&) = delete;

private:
    sqlite3_stmt* statement_ = nullptr;
};

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
    for (int attempt = 0; attempt < kSqliteRetryAttempts; ++attempt) {
        char* errorMessage = nullptr;
        const auto rc = g_sqlite3_exec(db, sql, nullptr, nullptr, &errorMessage);
        if (rc == kSqliteOk) {
            if (errorMessage != nullptr && g_sqlite3_free != nullptr) {
                g_sqlite3_free(errorMessage);
            }
            return;
        }
        std::string message = errorMessage != nullptr ? errorMessage : sqliteError(db);
        if (errorMessage != nullptr && g_sqlite3_free != nullptr) {
            g_sqlite3_free(errorMessage);
        }
        if ((rc == kSqliteBusy || rc == kSqliteLocked) &&
            attempt + 1 < kSqliteRetryAttempts) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kSqliteRetryBackoffMs * (attempt + 1))
            );
            continue;
        }
        throw std::runtime_error(message);
    }
}

void stepDoneOrThrow(sqlite3* db, sqlite3_stmt* stmt) {
    for (int attempt = 0; attempt < kSqliteRetryAttempts; ++attempt) {
        const auto rc = g_sqlite3_step(stmt);
        if (rc == kSqliteDone) {
            return;
        }
        if ((rc == kSqliteBusy || rc == kSqliteLocked) &&
            attempt + 1 < kSqliteRetryAttempts) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kSqliteRetryBackoffMs * (attempt + 1))
            );
            continue;
        }
        throw std::runtime_error(sqliteError(db));
    }
}

void rollbackNoThrow(sqlite3* db) noexcept {
    char* errorMessage = nullptr;
    g_sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, &errorMessage);
    if (errorMessage != nullptr && g_sqlite3_free != nullptr) {
        g_sqlite3_free(errorMessage);
    }
}

struct ColumnLookup {
    const char* name = nullptr;
    bool found = false;
};

int findColumn(void* context, int columnCount, char** values, char**) {
    auto* lookup = static_cast<ColumnLookup*>(context);
    if (lookup != nullptr && columnCount > 1 && values[1] != nullptr &&
        std::string(values[1]) == lookup->name) {
        lookup->found = true;
    }
    return 0;
}

bool tableHasColumn(sqlite3* db, const char* table, const char* column) {
    ColumnLookup lookup{column, false};
    const auto sql = std::string("PRAGMA table_info(") + table + ");";
    char* errorMessage = nullptr;
    const auto rc = g_sqlite3_exec(db, sql.c_str(), findColumn, &lookup, &errorMessage);
    if (rc != kSqliteOk) {
        const std::string message = errorMessage != nullptr ? errorMessage : sqliteError(db);
        if (errorMessage != nullptr && g_sqlite3_free != nullptr) {
            g_sqlite3_free(errorMessage);
        }
        throw std::runtime_error(message);
    }
    if (errorMessage != nullptr && g_sqlite3_free != nullptr) {
        g_sqlite3_free(errorMessage);
    }
    return lookup.found;
}

}  // namespace

SqliteAlarmWriter::SqliteAlarmWriter(std::string dbPath, std::string libraryPath, int retentionDays)
    : dbPath_(std::move(dbPath)), libraryPath_(std::move(libraryPath)),
      retentionDays_(std::min(std::max(retentionDays, 1), 3650)) {
    try {
        loadLibrary();
        openDatabase();
        ensureSchema();
    } catch (...) {
        closeDatabase();
        unloadLibrary();
        throw;
    }
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

    StatementGuard stmt;
    const char* sql =
        "INSERT OR IGNORE INTO alarm_events(event_id, point_index, ts, alarm_type, active, threshold, value, quality, stale, persist_value, gateway_code, device_code, point_code) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    try {
        if (g_sqlite3_prepare_v2(db, sql, -1, stmt.output(), nullptr) != kSqliteOk) {
            throw std::runtime_error(sqliteError(db));
        }
        for (const auto& event : events) {
            if (g_sqlite3_bind_text(stmt.get(), 1, event.eventId.c_str(), -1, nullptr) != kSqliteOk ||
                g_sqlite3_bind_int(stmt.get(), 2, static_cast<int>(event.index)) != kSqliteOk ||
                g_sqlite3_bind_int64(stmt.get(), 3, static_cast<long long>(event.ts)) != kSqliteOk ||
                g_sqlite3_bind_text(stmt.get(), 4, event.alarmType.c_str(), -1, nullptr) != kSqliteOk ||
                g_sqlite3_bind_int(stmt.get(), 5, event.active ? 1 : 0) != kSqliteOk ||
                g_sqlite3_bind_double(stmt.get(), 6, event.threshold) != kSqliteOk ||
                g_sqlite3_bind_double(stmt.get(), 7, event.value) != kSqliteOk ||
                g_sqlite3_bind_int(stmt.get(), 8, event.quality) != kSqliteOk ||
                g_sqlite3_bind_int(stmt.get(), 9, event.stale ? 1 : 0) != kSqliteOk ||
                g_sqlite3_bind_text(stmt.get(), 10, event.persistValue.c_str(), -1, nullptr) != kSqliteOk ||
                g_sqlite3_bind_text(stmt.get(), 11, event.machineCode.c_str(), -1, nullptr) != kSqliteOk ||
                g_sqlite3_bind_text(stmt.get(), 12, event.meterCode.c_str(), -1, nullptr) != kSqliteOk ||
                g_sqlite3_bind_text(stmt.get(), 13, event.pointCode.c_str(), -1, nullptr) != kSqliteOk) {
                throw std::runtime_error(sqliteError(db));
            }
            stepDoneOrThrow(db, stmt.get());
            if (g_sqlite3_reset(stmt.get()) != kSqliteOk ||
                g_sqlite3_clear_bindings(stmt.get()) != kSqliteOk) {
                throw std::runtime_error(sqliteError(db));
            }
        }
        stmt.finalize();
        execOrThrow(db, "COMMIT;");
    } catch (...) {
        stmt.finalize();
        rollbackNoThrow(db);
        throw;
    }
}

void SqliteAlarmWriter::cleanupExpiredEvents(std::int64_t nowMs, MaintenanceClock::time_point now) {
    if (now < nextCleanup_) return;
    const auto retentionMs = static_cast<std::int64_t>(retentionDays_) * 86400000;
    const auto cutoff = nowMs < std::numeric_limits<std::int64_t>::min() + retentionMs
        ? std::numeric_limits<std::int64_t>::min() : nowMs - retentionMs;
    auto* db = static_cast<sqlite3*>(databaseHandle_);
    StatementGuard stmt;
    const char* sql = "DELETE FROM alarm_events WHERE id IN ("
        "SELECT id FROM alarm_events INDEXED BY idx_alarm_events_ts_id "
        "WHERE ts < ? ORDER BY ts,id LIMIT 512);";
    try {
        if (g_sqlite3_prepare_v2(db, sql, -1, stmt.output(), nullptr) != kSqliteOk ||
            g_sqlite3_bind_int64(stmt.get(), 1, cutoff) != kSqliteOk ||
            g_sqlite3_step(stmt.get()) != kSqliteDone) {
            throw std::runtime_error(sqliteError(db));
        }
    } catch (...) {
        stmt.finalize();
        nextCleanup_ = std::max(now, MaintenanceClock::now()) + std::chrono::seconds(5);
        throw;
    }
    const auto delay = std::chrono::seconds(g_sqlite3_changes(db) == 512 ? 1 : 60);
    stmt.finalize();
    nextCleanup_ = std::max(now, MaintenanceClock::now()) + delay;
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
    g_sqlite3_reset = reinterpret_cast<sqlite3_reset_fn>(loadSymbol(libraryHandle_, "sqlite3_reset"));
    g_sqlite3_clear_bindings = reinterpret_cast<sqlite3_clear_bindings_fn>(loadSymbol(libraryHandle_, "sqlite3_clear_bindings"));
    g_sqlite3_finalize = reinterpret_cast<sqlite3_finalize_fn>(loadSymbol(libraryHandle_, "sqlite3_finalize"));
    g_sqlite3_errmsg = reinterpret_cast<sqlite3_errmsg_fn>(loadSymbol(libraryHandle_, "sqlite3_errmsg"));
    g_sqlite3_free = reinterpret_cast<sqlite3_free_fn>(loadSymbol(libraryHandle_, "sqlite3_free"));
    g_sqlite3_busy_timeout = reinterpret_cast<sqlite3_busy_timeout_fn>(loadSymbol(libraryHandle_, "sqlite3_busy_timeout"));
    g_sqlite3_changes = reinterpret_cast<sqlite3_changes_fn>(loadSymbol(libraryHandle_, "sqlite3_changes"));
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
        if (db != nullptr) {
            g_sqlite3_close_v2(db);
        }
        throw std::runtime_error("failed to open sqlite database");
    }
    if (g_sqlite3_busy_timeout(db, kSqliteBusyTimeoutMs) != kSqliteOk) {
        g_sqlite3_close_v2(db);
        throw std::runtime_error("failed to configure sqlite busy timeout");
    }
    databaseHandle_ = db;
}

void SqliteAlarmWriter::ensureSchema() {
    auto* db = static_cast<sqlite3*>(databaseHandle_);
    execOrThrow(db, "PRAGMA journal_mode=WAL;");
    execOrThrow(db, "PRAGMA synchronous=NORMAL;");
    execOrThrow(
        db,
        "CREATE TABLE IF NOT EXISTS alarm_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "event_id TEXT,"
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
    if (!tableHasColumn(db, "alarm_events", "event_id")) {
        execOrThrow(db, "ALTER TABLE alarm_events ADD COLUMN event_id TEXT;");
    }
    execOrThrow(
        db,
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_alarm_events_event_id "
        "ON alarm_events(event_id) WHERE event_id IS NOT NULL AND event_id <> '';"
    );
    execOrThrow(db, "CREATE INDEX IF NOT EXISTS idx_alarm_events_ts_id ON alarm_events(ts,id);");
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
