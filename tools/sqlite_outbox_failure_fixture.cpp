#include <atomic>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#include <dlfcn.h>

struct sqlite3;
struct sqlite3_stmt;

namespace {
void* realLibrary() {
    static void* handle = [] {
        const char* configured = std::getenv("GATEWAY_SQLITE_FIXTURE_REAL_LIBRARY");
        void* result = dlopen(configured && *configured ? configured : "libsqlite3.so.0",
            RTLD_NOW | RTLD_LOCAL);
        if (!result) throw std::runtime_error("cannot load real SQLite for Outbox fixture");
        return result;
    }();
    return handle;
}

template <typename Function>
Function real(const char* name) {
    auto* result = dlsym(realLibrary(), name);
    if (!result) throw std::runtime_error(std::string("missing real SQLite symbol: ") + name);
    return reinterpret_cast<Function>(result);
}

thread_local sqlite3* failedDb = nullptr;
thread_local int errorCode = 0;
thread_local int injectedCode = 0;
thread_local int failuresLeft = 0;
thread_local bool automaticRollback = false;
thread_local int eventAttempts = 0;
thread_local int rollbackToAttempts = 0;
thread_local int beginAttempts = 0;
thread_local bool failRestore = false;
thread_local bool failRollback = false;
thread_local int commitCode = 0;
thread_local bool commitRollback = false;
std::atomic<int> openHandles{0};

void clearError() { failedDb = nullptr; errorCode = 0; }

const char* errorText(int code) {
    switch (code & 255) {
    case 13: return "injected full";
    case 10: return "injected ioerr";
    case 6: return "injected locked";
    case 5: return "injected busy snapshot";
    case 19: return "injected constraint";
    default: return "injected sqlite failure";
    }
}
}

// The fixture delegates real schema/transactions; only a selected event step fails.
#define PROXY(name, result, parameters, arguments) \
    extern "C" result name parameters { \
        static auto fn = real<result (*) parameters>(#name); \
        return fn arguments; \
    }

PROXY(sqlite3_prepare_v2, int,
    (sqlite3* db, const char* sql, int n, sqlite3_stmt** s, const char** t), (db, sql, n, s, t))
PROXY(sqlite3_extended_result_codes, int, (sqlite3* db, int enabled), (db, enabled))
PROXY(sqlite3_get_autocommit, int, (sqlite3* db), (db))
PROXY(sqlite3_bind_int, int, (sqlite3_stmt* s, int i, int v), (s, i, v))
PROXY(sqlite3_bind_int64, int, (sqlite3_stmt* s, int i, long long v), (s, i, v))
PROXY(sqlite3_bind_double, int, (sqlite3_stmt* s, int i, double v), (s, i, v))
PROXY(sqlite3_bind_text, int,
    (sqlite3_stmt* s, int i, const char* v, int n, void (*d)(void*)), (s, i, v, n, d))
PROXY(sqlite3_clear_bindings, int, (sqlite3_stmt* s), (s))
PROXY(sqlite3_last_insert_rowid, long long, (sqlite3* db), (db))
PROXY(sqlite3_changes, int, (sqlite3* db), (db))
PROXY(sqlite3_column_int64, long long, (sqlite3_stmt* s, int i), (s, i))
PROXY(sqlite3_column_int, int, (sqlite3_stmt* s, int i), (s, i))
PROXY(sqlite3_column_double, double, (sqlite3_stmt* s, int i), (s, i))
PROXY(sqlite3_column_text, const unsigned char*, (sqlite3_stmt* s, int i), (s, i))
PROXY(sqlite3_free, void, (void* p), (p))
PROXY(sqlite3_libversion, const char*, (), ())
PROXY(sqlite3_db_handle, sqlite3*, (sqlite3_stmt* s), (s))

extern "C" void outbox_fault_reset() {
    clearError();
    injectedCode = 0;
    failuresLeft = 0;
    automaticRollback = false;
    eventAttempts = 0;
    rollbackToAttempts = 0;
    beginAttempts = 0;
    failRestore = false;
    failRollback = false;
    commitCode = 0;
    commitRollback = false;
}

extern "C" void outbox_fault_event(int code, int rollback, int count) {
    injectedCode = code;
    automaticRollback = rollback != 0;
    failuresLeft = count;
}

extern "C" int outbox_fault_event_attempts() { return eventAttempts; }
extern "C" int outbox_fault_rollback_to_attempts() { return rollbackToAttempts; }
extern "C" int outbox_fault_begin_attempts() { return beginAttempts; }
extern "C" void outbox_fault_restore_failure() { failRestore = true; }
extern "C" void outbox_fault_rollback_failure() { failRollback = true; }
extern "C" void outbox_fault_commit(int code, int rollback) { commitCode = code; commitRollback = rollback != 0; }
extern "C" int outbox_fault_open_handles() { return openHandles.load(); }

extern "C" int sqlite3_open_v2(const char* path, sqlite3** db, int flags, const char* vfs) {
    static auto fn = real<int (*)(const char*, sqlite3**, int, const char*)>("sqlite3_open_v2");
    const int rc = fn(path, db, flags, vfs);
    if (*db) ++openHandles;
    return rc;
}

extern "C" int sqlite3_close_v2(sqlite3* db) {
    static auto fn = real<int (*)(sqlite3*)>("sqlite3_close_v2");
    const int rc = fn(db);
    if (db && rc == 0) --openHandles;
    return rc;
}

extern "C" int sqlite3_busy_timeout(sqlite3* db, int ms) {
    if (ms == 25 && failRestore) { failRestore = false; return 7; }
    static auto fn = real<int (*)(sqlite3*, int)>("sqlite3_busy_timeout");
    return fn(db, ms);
}

extern "C" int sqlite3_exec(sqlite3* db, const char* sql,
    int (*callback)(void*, int, char**, char**), void* context, char** error) {
    static auto fn = real<int (*)(sqlite3*, const char*,
        int (*)(void*, int, char**, char**), void*, char**)>("sqlite3_exec");
    if (std::strncmp(sql, "ROLLBACK TO ", 12) == 0) ++rollbackToAttempts;
    if (std::strncmp(sql, "BEGIN", 5) == 0) ++beginAttempts;
    if (std::strcmp(sql, "COMMIT;") == 0 && commitCode != 0) {
        const int code = commitCode;
        commitCode = 0;
        if (commitRollback) fn(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        failedDb = db;
        errorCode = code;
        if (error) *error = nullptr;
        return code;
    }
    if (std::strcmp(sql, "ROLLBACK;") == 0 && failRollback) {
        failRollback = false;
        failedDb = db;
        errorCode = 1034;
        if (error) *error = nullptr;
        return errorCode;
    }
    clearError();
    return fn(db, sql, callback, context, error);
}

extern "C" int sqlite3_step(sqlite3_stmt* statement) {
    static auto sql = real<const char* (*)(sqlite3_stmt*)>("sqlite3_sql");
    static auto dbHandle = real<sqlite3* (*)(sqlite3_stmt*)>("sqlite3_db_handle");
    static auto step = real<int (*)(sqlite3_stmt*)>("sqlite3_step");
    const char* query = sql(statement);
    constexpr char insertPrefix[] = "INSERT OR IGNORE INTO mqtt_event_outbox(";
    if (query && std::strncmp(query, insertPrefix, sizeof(insertPrefix) - 1) == 0) {
        ++eventAttempts;
        if (failuresLeft != 0) {
            if (failuresLeft > 0) --failuresLeft;
            auto* db = dbHandle(statement);
            if (automaticRollback) sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
            failedDb = db;
            errorCode = injectedCode;
            return injectedCode;
        }
    }
    clearError();
    return step(statement);
}

extern "C" int sqlite3_reset(sqlite3_stmt* statement) {
    clearError();
    static auto fn = real<int (*)(sqlite3_stmt*)>("sqlite3_reset");
    return fn(statement);
}

extern "C" int sqlite3_finalize(sqlite3_stmt* statement) {
    clearError();
    static auto fn = real<int (*)(sqlite3_stmt*)>("sqlite3_finalize");
    return fn(statement);
}

extern "C" int sqlite3_extended_errcode(sqlite3* db) {
    static auto fn = real<int (*)(sqlite3*)>("sqlite3_extended_errcode");
    return db == failedDb ? errorCode : fn(db);
}

extern "C" int sqlite3_errcode(sqlite3* db) {
    static auto fn = real<int (*)(sqlite3*)>("sqlite3_errcode");
    return db == failedDb ? (errorCode & 255) : fn(db);
}

extern "C" const char* sqlite3_errmsg(sqlite3* db) {
    static auto fn = real<const char* (*)(sqlite3*)>("sqlite3_errmsg");
    return db == failedDb ? errorText(errorCode) : fn(db);
}
