#include "edge_gateway/sqlite_sample_writer.hpp"

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
using sqlite3_step_fn = int (*)(sqlite3_stmt*);
using sqlite3_reset_fn = int (*)(sqlite3_stmt*);
using sqlite3_clear_bindings_fn = int (*)(sqlite3_stmt*);
using sqlite3_finalize_fn = int (*)(sqlite3_stmt*);
using sqlite3_errmsg_fn = const char* (*)(sqlite3*);
using sqlite3_free_fn = void (*)(void*);
using sqlite3_busy_timeout_fn = int (*)(sqlite3*, int);

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
sqlite3_step_fn g_sqlite3_step = nullptr;
sqlite3_reset_fn g_sqlite3_reset = nullptr;
sqlite3_clear_bindings_fn g_sqlite3_clear_bindings = nullptr;
sqlite3_finalize_fn g_sqlite3_finalize = nullptr;
sqlite3_errmsg_fn g_sqlite3_errmsg = nullptr;
sqlite3_free_fn g_sqlite3_free = nullptr;
sqlite3_busy_timeout_fn g_sqlite3_busy_timeout = nullptr;

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

void rollbackNoThrow(sqlite3* db) noexcept {
    char* errorMessage = nullptr;
    g_sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, &errorMessage);
    if (errorMessage != nullptr && g_sqlite3_free != nullptr) {
        g_sqlite3_free(errorMessage);
    }
}

}  // namespace

SqliteSampleWriter::SqliteSampleWriter(std::string dbPath, std::string libraryPath)
    : dbPath_(std::move(dbPath)), libraryPath_(std::move(libraryPath)) {
    if (dbPath_.empty()) {
        return;
    }
    enabled_ = true;
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

SqliteSampleWriter::~SqliteSampleWriter() {
    closeDatabase();
    unloadLibrary();
}

void SqliteSampleWriter::writeSamples(const std::vector<PersistentPointSample>& samples) {
    if (!enabled_ || samples.empty()) {
        return;
    }

    auto* db = static_cast<sqlite3*>(databaseHandle_);
    execOrThrow(db, "BEGIN IMMEDIATE TRANSACTION;");

    StatementGuard stmt;
    const char* sql =
        "INSERT OR REPLACE INTO point_samples(point_index, ts, value) VALUES(?, ?, ?);";

    try {
        if (g_sqlite3_prepare_v2(db, sql, -1, stmt.output(), nullptr) != kSqliteOk) {
            throw std::runtime_error(sqliteError(db));
        }
        for (const auto& sample : samples) {
            if (g_sqlite3_bind_int(stmt.get(), 1, static_cast<int>(sample.index)) != kSqliteOk ||
                g_sqlite3_bind_int64(stmt.get(), 2, static_cast<long long>(sample.ts)) != kSqliteOk ||
                g_sqlite3_bind_double(stmt.get(), 3, sample.value) != kSqliteOk) {
                throw std::runtime_error(sqliteError(db));
            }
            if (g_sqlite3_step(stmt.get()) != kSqliteDone) {
                throw std::runtime_error(sqliteError(db));
            }
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

void SqliteSampleWriter::loadLibrary() {
    if (!enabled_) {
        return;
    }
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
    g_sqlite3_step = reinterpret_cast<sqlite3_step_fn>(loadSymbol(libraryHandle_, "sqlite3_step"));
    g_sqlite3_reset = reinterpret_cast<sqlite3_reset_fn>(loadSymbol(libraryHandle_, "sqlite3_reset"));
    g_sqlite3_clear_bindings = reinterpret_cast<sqlite3_clear_bindings_fn>(loadSymbol(libraryHandle_, "sqlite3_clear_bindings"));
    g_sqlite3_finalize = reinterpret_cast<sqlite3_finalize_fn>(loadSymbol(libraryHandle_, "sqlite3_finalize"));
    g_sqlite3_errmsg = reinterpret_cast<sqlite3_errmsg_fn>(loadSymbol(libraryHandle_, "sqlite3_errmsg"));
    g_sqlite3_free = reinterpret_cast<sqlite3_free_fn>(loadSymbol(libraryHandle_, "sqlite3_free"));
    g_sqlite3_busy_timeout = reinterpret_cast<sqlite3_busy_timeout_fn>(loadSymbol(libraryHandle_, "sqlite3_busy_timeout"));
}

void SqliteSampleWriter::openDatabase() {
    if (!enabled_) {
        return;
    }
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
    if (g_sqlite3_busy_timeout(db, 5000) != kSqliteOk) {
        g_sqlite3_close_v2(db);
        throw std::runtime_error("failed to configure sqlite busy timeout");
    }
    databaseHandle_ = db;
}

void SqliteSampleWriter::ensureSchema() {
    if (!enabled_) {
        return;
    }
    auto* db = static_cast<sqlite3*>(databaseHandle_);
    execOrThrow(db, "PRAGMA journal_mode=WAL;");
    execOrThrow(db, "PRAGMA synchronous=NORMAL;");
    execOrThrow(
        db,
        "CREATE TABLE IF NOT EXISTS point_samples ("
        "point_index INTEGER NOT NULL,"
        "ts INTEGER NOT NULL,"
        "value REAL NOT NULL,"
        "PRIMARY KEY(point_index, ts)"
        ");"
    );
}

void SqliteSampleWriter::closeDatabase() {
    if (databaseHandle_ != nullptr) {
        g_sqlite3_close_v2(static_cast<sqlite3*>(databaseHandle_));
        databaseHandle_ = nullptr;
    }
}

void SqliteSampleWriter::unloadLibrary() {
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
