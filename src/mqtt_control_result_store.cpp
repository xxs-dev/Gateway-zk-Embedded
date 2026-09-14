#include "edge_gateway/mqtt_control_result_store.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <utility>

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
using sqlite3_busy_timeout_fn = int (*)(sqlite3*, int);
using sqlite3_bind_int_fn = int (*)(sqlite3_stmt*, int, int);
using sqlite3_bind_int64_fn = int (*)(sqlite3_stmt*, int, long long);
using sqlite3_bind_double_fn = int (*)(sqlite3_stmt*, int, double);
using sqlite3_bind_text_fn = int (*)(sqlite3_stmt*, int, const char*, int, void (*)(void*));
using sqlite3_step_fn = int (*)(sqlite3_stmt*);
using sqlite3_finalize_fn = int (*)(sqlite3_stmt*);
using sqlite3_errmsg_fn = const char* (*)(sqlite3*);
using sqlite3_free_fn = void (*)(void*);
using sqlite3_changes_fn = int (*)(sqlite3*);
using sqlite3_column_int64_fn = long long (*)(sqlite3_stmt*, int);
using sqlite3_column_int_fn = int (*)(sqlite3_stmt*, int);
using sqlite3_column_double_fn = double (*)(sqlite3_stmt*, int);
using sqlite3_column_text_fn = const unsigned char* (*)(sqlite3_stmt*, int);

constexpr int kSqliteOk = 0;
constexpr int kSqliteBusy = 5;
constexpr int kSqliteLocked = 6;
constexpr int kSqliteRow = 100;
constexpr int kSqliteDone = 101;
constexpr int kSqliteOpenReadWrite = 0x00000002;
constexpr int kSqliteOpenCreate = 0x00000004;
constexpr int kSqliteBusyTimeoutMs = 25;
constexpr int kSqliteRetryAttempts = 3;
constexpr std::int64_t kCleanupIntervalMs = 60LL * 60LL * 1000LL;
constexpr std::int64_t kDayMs = 24LL * 60LL * 60LL * 1000LL;

sqlite3_open_v2_fn g_open = nullptr;
sqlite3_close_v2_fn g_close = nullptr;
sqlite3_exec_fn g_exec = nullptr;
sqlite3_prepare_v2_fn g_prepare = nullptr;
sqlite3_busy_timeout_fn g_busy_timeout = nullptr;
sqlite3_bind_int_fn g_bind_int = nullptr;
sqlite3_bind_int64_fn g_bind_int64 = nullptr;
sqlite3_bind_double_fn g_bind_double = nullptr;
sqlite3_bind_text_fn g_bind_text = nullptr;
sqlite3_step_fn g_step = nullptr;
sqlite3_finalize_fn g_finalize = nullptr;
sqlite3_errmsg_fn g_errmsg = nullptr;
sqlite3_free_fn g_free = nullptr;
sqlite3_changes_fn g_changes = nullptr;
sqlite3_column_int64_fn g_column_int64 = nullptr;
sqlite3_column_int_fn g_column_int = nullptr;
sqlite3_column_double_fn g_column_double = nullptr;
sqlite3_column_text_fn g_column_text = nullptr;
std::once_flag g_sqlite_library_once;
void* g_sqlite_library_handle = nullptr;
std::string g_sqlite_library_request;

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

void closeDynamicLibrary(void* handle) {
    if (handle == nullptr) return;
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

std::string sqliteError(sqlite3* db) {
    return db && g_errmsg ? g_errmsg(db) : "sqlite error";
}

void execOrThrow(sqlite3* db, const char* sql) {
    std::string lastMessage;
    for (int attempt = 0; attempt < kSqliteRetryAttempts; ++attempt) {
        char* errorMessage = nullptr;
        const auto rc = g_exec(db, sql, nullptr, nullptr, &errorMessage);
        if (rc == kSqliteOk) {
            if (errorMessage != nullptr && g_free != nullptr) g_free(errorMessage);
            return;
        }
        lastMessage = errorMessage != nullptr ? errorMessage : sqliteError(db);
        if (errorMessage != nullptr && g_free != nullptr) g_free(errorMessage);
        if (rc != kSqliteBusy && rc != kSqliteLocked) {
            throw std::runtime_error(lastMessage);
        }
    }
    throw std::runtime_error(lastMessage.empty() ? "sqlite busy timeout" : lastMessage);
}

int prepareWithRetry(sqlite3* db, const char* sql, sqlite3_stmt** stmt) {
    for (int attempt = 0; attempt < kSqliteRetryAttempts; ++attempt) {
        const auto rc = g_prepare(db, sql, -1, stmt, nullptr);
        if (rc == kSqliteOk || (rc != kSqliteBusy && rc != kSqliteLocked)) return rc;
    }
    return kSqliteBusy;
}

int stepWithRetry(sqlite3_stmt* stmt) {
    for (int attempt = 0; attempt < kSqliteRetryAttempts; ++attempt) {
        const auto rc = g_step(stmt);
        if (rc == kSqliteDone || rc == kSqliteRow || (rc != kSqliteBusy && rc != kSqliteLocked)) {
            return rc;
        }
    }
    return kSqliteBusy;
}

std::string columnText(sqlite3_stmt* stmt, int column) {
    const auto* value = g_column_text(stmt, column);
    return value == nullptr ? std::string() : reinterpret_cast<const char*>(value);
}

void bindTextOrThrow(sqlite3_stmt* stmt, int index, const std::string& value, sqlite3* db) {
    if (g_bind_text(stmt, index, value.c_str(), -1, nullptr) != kSqliteOk) {
        throw std::runtime_error(sqliteError(db));
    }
}

class StatementGuard {
public:
    explicit StatementGuard(sqlite3_stmt* statement = nullptr) : statement_(statement) {}
    ~StatementGuard() {
        if (statement_ != nullptr) g_finalize(statement_);
    }
    sqlite3_stmt** out() { return &statement_; }
    sqlite3_stmt* get() const { return statement_; }
    void finalize() {
        if (statement_ != nullptr) {
            g_finalize(statement_);
            statement_ = nullptr;
        }
    }

private:
    sqlite3_stmt* statement_ = nullptr;
};

bool tableHasColumn(sqlite3* db, const char* table, const char* column) {
    const std::string sql = std::string("PRAGMA table_info(") + table + ");";
    StatementGuard statement;
    if (prepareWithRetry(db, sql.c_str(), statement.out()) != kSqliteOk) {
        throw std::runtime_error(sqliteError(db));
    }
    while (true) {
        const auto rc = stepWithRetry(statement.get());
        if (rc == kSqliteDone) return false;
        if (rc != kSqliteRow) throw std::runtime_error(sqliteError(db));
        if (columnText(statement.get(), 1) == column) return true;
    }
}

}  // namespace

MqttControlResultStore::MqttControlResultStore(
    std::string dbPath,
    std::string libraryPath,
    int deliveredRetentionDays,
    std::size_t maxDeliveredRecords
)
    : dbPath_(std::move(dbPath)),
      libraryPath_(std::move(libraryPath)),
      deliveredRetentionDays_(deliveredRetentionDays),
      maxDeliveredRecords_(maxDeliveredRecords) {
    if (dbPath_.empty()) {
        throw std::invalid_argument("mqtt control result store path must not be empty");
    }
    if (deliveredRetentionDays_ < 1 || deliveredRetentionDays_ > 3650) {
        throw std::invalid_argument("mqtt control result retention days must be 1..3650");
    }
    if (maxDeliveredRecords_ < 1 || maxDeliveredRecords_ > 1000000) {
        throw std::invalid_argument("mqtt control result delivered record limit must be 1..1000000");
    }
    loadLibrary();
    try {
        openDatabase();
        ensureSchema();
    } catch (...) {
        closeDatabase();
        unloadLibrary();
        throw;
    }
}

MqttControlResultStore::~MqttControlResultStore() {
    closeDatabase();
    unloadLibrary();
}

std::string MqttControlResultStore::defaultPathForOwnershipFile(const std::string& ownershipFile) {
    const std::string suffix = ".json";
    if (ownershipFile.size() >= suffix.size() &&
        ownershipFile.compare(ownershipFile.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return ownershipFile.substr(0, ownershipFile.size() - suffix.size()) + ".control-results.db";
    }
    return ownershipFile + ".control-results.db";
}

MqttControlReserveStatus MqttControlResultStore::reservePending(
    const MqttControlResultRecord& record
) {
    if (record.id.empty() || record.fingerprint.empty() || record.generation == 0 ||
        record.acceptedAtMs <= 0 || record.deadlineMs < record.acceptedAtMs || record.routes.empty()) {
        throw std::invalid_argument("invalid pending mqtt control result");
    }
    auto* db = static_cast<sqlite3*>(databaseHandle_);
    const auto existing = findInternal(record.id);
    if (existing) {
        return existing->fingerprint == record.fingerprint
            ? MqttControlReserveStatus::Existing
            : MqttControlReserveStatus::FingerprintMismatch;
    }

    execOrThrow(db, "BEGIN IMMEDIATE;");
    try {
        StatementGuard insert;
        const char* sql =
            "INSERT INTO mqtt_control_result("
            "command_id,fingerprint,command_type,target_kw,generation,accepted_at,deadline_at,"
            "submitted,final_payload,delivered,delivered_at) VALUES(?,?,?,?,?,?,?,0,'',0,0);";
        if (prepareWithRetry(db, sql, insert.out()) != kSqliteOk) {
            throw std::runtime_error(sqliteError(db));
        }
        bindTextOrThrow(insert.get(), 1, record.id, db);
        bindTextOrThrow(insert.get(), 2, record.fingerprint, db);
        if (g_bind_int(insert.get(), 3, record.type) != kSqliteOk ||
            g_bind_double(insert.get(), 4, record.targetKw) != kSqliteOk ||
            g_bind_int64(insert.get(), 5, static_cast<long long>(record.generation)) != kSqliteOk ||
            g_bind_int64(insert.get(), 6, static_cast<long long>(record.acceptedAtMs)) != kSqliteOk ||
            g_bind_int64(insert.get(), 7, static_cast<long long>(record.deadlineMs)) != kSqliteOk ||
            stepWithRetry(insert.get()) != kSqliteDone) {
            throw std::runtime_error(sqliteError(db));
        }

        const char* routeSql =
            "INSERT INTO mqtt_control_result_route(command_id,point_index,shared_memory_name) "
            "VALUES(?,?,?);";
        for (const auto& route : record.routes) {
            StatementGuard routeInsert;
            if (prepareWithRetry(db, routeSql, routeInsert.out()) != kSqliteOk) {
                throw std::runtime_error(sqliteError(db));
            }
            bindTextOrThrow(routeInsert.get(), 1, record.id, db);
            if (g_bind_int64(routeInsert.get(), 2, static_cast<long long>(route.index)) != kSqliteOk) {
                throw std::runtime_error(sqliteError(db));
            }
            bindTextOrThrow(routeInsert.get(), 3, route.sharedMemoryName, db);
            if (stepWithRetry(routeInsert.get()) != kSqliteDone) {
                throw std::runtime_error(sqliteError(db));
            }
        }
        execOrThrow(db, "COMMIT;");
    } catch (...) {
        try { execOrThrow(db, "ROLLBACK;"); } catch (...) {}
        const auto concurrent = findInternal(record.id);
        if (concurrent) {
            return concurrent->fingerprint == record.fingerprint
                ? MqttControlReserveStatus::Existing
                : MqttControlReserveStatus::FingerprintMismatch;
        }
        throw;
    }
    return MqttControlReserveStatus::Inserted;
}

Optional<MqttControlResultRecord> MqttControlResultStore::find(const std::string& id) const {
    if (id.empty()) return NullOpt;
    return findInternal(id);
}

Optional<MqttControlResultRecord> MqttControlResultStore::findInternal(const std::string& id) const {
    auto* db = static_cast<sqlite3*>(databaseHandle_);
    StatementGuard statement;
    const char* sql =
        "SELECT fingerprint,command_type,target_kw,generation,accepted_at,deadline_at,"
        "submitted,final_payload,delivered FROM mqtt_control_result WHERE command_id=?;";
    if (prepareWithRetry(db, sql, statement.out()) != kSqliteOk) {
        throw std::runtime_error(sqliteError(db));
    }
    bindTextOrThrow(statement.get(), 1, id, db);
    const auto rc = stepWithRetry(statement.get());
    if (rc == kSqliteDone) return NullOpt;
    if (rc != kSqliteRow) throw std::runtime_error(sqliteError(db));

    MqttControlResultRecord record;
    record.id = id;
    record.fingerprint = columnText(statement.get(), 0);
    record.type = g_column_int(statement.get(), 1);
    record.targetKw = g_column_double(statement.get(), 2);
    record.generation = static_cast<std::uint32_t>(g_column_int64(statement.get(), 3));
    record.acceptedAtMs = static_cast<std::int64_t>(g_column_int64(statement.get(), 4));
    record.deadlineMs = static_cast<std::int64_t>(g_column_int64(statement.get(), 5));
    record.submitted = g_column_int(statement.get(), 6) != 0;
    record.finalPayload = columnText(statement.get(), 7);
    record.delivered = g_column_int(statement.get(), 8) != 0;
    statement.finalize();
    record.routes = loadRoutes(id);
    return record;
}

bool MqttControlResultStore::markSubmitted(
    const std::string& id,
    const std::string& fingerprint
) {
    if (id.empty() || fingerprint.empty()) return false;
    auto* db = static_cast<sqlite3*>(databaseHandle_);
    StatementGuard statement;
    const char* sql =
        "UPDATE mqtt_control_result SET submitted=1 "
        "WHERE command_id=? AND fingerprint=? AND final_payload='';";
    if (prepareWithRetry(db, sql, statement.out()) != kSqliteOk) return false;
    bindTextOrThrow(statement.get(), 1, id, db);
    bindTextOrThrow(statement.get(), 2, fingerprint, db);
    if (stepWithRetry(statement.get()) != kSqliteDone) return false;
    if (g_changes(db) == 1) return true;
    const auto current = findInternal(id);
    return current && current->fingerprint == fingerprint && current->submitted &&
        current->finalPayload.empty();
}

std::vector<PointStoreRoute> MqttControlResultStore::loadRoutes(const std::string& id) const {
    auto* db = static_cast<sqlite3*>(databaseHandle_);
    StatementGuard statement;
    const char* sql =
        "SELECT point_index,shared_memory_name FROM mqtt_control_result_route "
        "WHERE command_id=? ORDER BY point_index;";
    if (prepareWithRetry(db, sql, statement.out()) != kSqliteOk) {
        throw std::runtime_error(sqliteError(db));
    }
    bindTextOrThrow(statement.get(), 1, id, db);
    std::vector<PointStoreRoute> routes;
    while (true) {
        const auto rc = stepWithRetry(statement.get());
        if (rc == kSqliteDone) break;
        if (rc != kSqliteRow) throw std::runtime_error(sqliteError(db));
        PointStoreRoute route;
        route.index = static_cast<std::uint32_t>(g_column_int64(statement.get(), 0));
        route.sharedMemoryName = columnText(statement.get(), 1);
        route.writable = true;
        routes.push_back(std::move(route));
    }
    return routes;
}

std::vector<MqttControlResultRecord> MqttControlResultStore::loadPending() const {
    auto* db = static_cast<sqlite3*>(databaseHandle_);
    StatementGuard statement;
    if (prepareWithRetry(
            db,
            "SELECT command_id FROM mqtt_control_result WHERE final_payload='' ORDER BY accepted_at;",
            statement.out()) != kSqliteOk) {
        throw std::runtime_error(sqliteError(db));
    }
    std::vector<std::string> ids;
    while (true) {
        const auto rc = stepWithRetry(statement.get());
        if (rc == kSqliteDone) break;
        if (rc != kSqliteRow) throw std::runtime_error(sqliteError(db));
        ids.push_back(columnText(statement.get(), 0));
    }
    statement.finalize();
    std::vector<MqttControlResultRecord> records;
    records.reserve(ids.size());
    for (const auto& id : ids) {
        const auto record = findInternal(id);
        if (record) records.push_back(*record);
    }
    return records;
}

std::vector<MqttControlResultRecord> MqttControlResultStore::loadUndelivered(
    std::size_t limit
) const {
    auto* db = static_cast<sqlite3*>(databaseHandle_);
    StatementGuard statement;
    const char* sql =
        "SELECT command_id FROM mqtt_control_result "
        "WHERE final_payload<>'' AND delivered=0 ORDER BY accepted_at LIMIT ?;";
    if (prepareWithRetry(db, sql, statement.out()) != kSqliteOk) {
        throw std::runtime_error(sqliteError(db));
    }
    if (g_bind_int64(statement.get(), 1, static_cast<long long>(std::max<std::size_t>(1, limit))) != kSqliteOk) {
        throw std::runtime_error(sqliteError(db));
    }
    std::vector<std::string> ids;
    while (true) {
        const auto rc = stepWithRetry(statement.get());
        if (rc == kSqliteDone) break;
        if (rc != kSqliteRow) throw std::runtime_error(sqliteError(db));
        ids.push_back(columnText(statement.get(), 0));
    }
    statement.finalize();
    std::vector<MqttControlResultRecord> records;
    records.reserve(ids.size());
    for (const auto& id : ids) {
        const auto record = findInternal(id);
        if (record) records.push_back(*record);
    }
    return records;
}

bool MqttControlResultStore::storeFinalPayload(
    const std::string& id,
    const std::string& fingerprint,
    const std::string& payload
) {
    if (id.empty() || fingerprint.empty() || payload.empty()) return false;
    const auto current = findInternal(id);
    if (!current || current->fingerprint != fingerprint) return false;
    if (!current->finalPayload.empty()) return current->finalPayload == payload;

    auto* db = static_cast<sqlite3*>(databaseHandle_);
    StatementGuard statement;
    const char* sql =
        "UPDATE mqtt_control_result SET final_payload=?,delivered=0,delivered_at=0 "
        "WHERE command_id=? AND fingerprint=? AND final_payload='';";
    if (prepareWithRetry(db, sql, statement.out()) != kSqliteOk) return false;
    bindTextOrThrow(statement.get(), 1, payload, db);
    bindTextOrThrow(statement.get(), 2, id, db);
    bindTextOrThrow(statement.get(), 3, fingerprint, db);
    if (stepWithRetry(statement.get()) != kSqliteDone) return false;
    return g_changes(db) == 1;
}

bool MqttControlResultStore::markDelivered(
    const std::string& id,
    const std::string& fingerprint,
    std::int64_t deliveredAtMs
) {
    auto* db = static_cast<sqlite3*>(databaseHandle_);
    StatementGuard statement;
    const char* sql =
        "UPDATE mqtt_control_result SET delivered=1,delivered_at=? "
        "WHERE command_id=? AND fingerprint=? AND final_payload<>'';";
    if (prepareWithRetry(db, sql, statement.out()) != kSqliteOk) return false;
    if (g_bind_int64(statement.get(), 1, static_cast<long long>(deliveredAtMs)) != kSqliteOk) return false;
    bindTextOrThrow(statement.get(), 2, id, db);
    bindTextOrThrow(statement.get(), 3, fingerprint, db);
    if (stepWithRetry(statement.get()) != kSqliteDone) return false;
    return g_changes(db) == 1;
}

bool MqttControlResultStore::discardPending(
    const std::string& id,
    const std::string& fingerprint
) {
    const auto current = findInternal(id);
    if (!current || current->fingerprint != fingerprint || !current->finalPayload.empty()) return false;
    auto* db = static_cast<sqlite3*>(databaseHandle_);
    execOrThrow(db, "BEGIN IMMEDIATE;");
    try {
        StatementGuard routes;
        if (prepareWithRetry(
                db,
                "DELETE FROM mqtt_control_result_route WHERE command_id=?;",
                routes.out()) != kSqliteOk) {
            throw std::runtime_error(sqliteError(db));
        }
        bindTextOrThrow(routes.get(), 1, id, db);
        if (stepWithRetry(routes.get()) != kSqliteDone) throw std::runtime_error(sqliteError(db));
        routes.finalize();

        StatementGuard record;
        if (prepareWithRetry(
                db,
                "DELETE FROM mqtt_control_result WHERE command_id=? AND fingerprint=? AND final_payload='';",
                record.out()) != kSqliteOk) {
            throw std::runtime_error(sqliteError(db));
        }
        bindTextOrThrow(record.get(), 1, id, db);
        bindTextOrThrow(record.get(), 2, fingerprint, db);
        if (stepWithRetry(record.get()) != kSqliteDone) throw std::runtime_error(sqliteError(db));
        const bool removed = g_changes(db) == 1;
        execOrThrow(db, "COMMIT;");
        return removed;
    } catch (...) {
        try { execOrThrow(db, "ROLLBACK;"); } catch (...) {}
        throw;
    }
}

void MqttControlResultStore::cleanupIfDue(std::int64_t nowMs) {
    if (nowMs <= 0 ||
        (lastCleanupMs_ > 0 && nowMs - lastCleanupMs_ < kCleanupIntervalMs)) {
        return;
    }
    auto* db = static_cast<sqlite3*>(databaseHandle_);
    execOrThrow(db, "BEGIN IMMEDIATE;");
    try {
        StatementGuard expired;
        const char* expiredSql =
            "DELETE FROM mqtt_control_result WHERE delivered=1 AND delivered_at>0 "
            "AND delivered_at<?;";
        if (prepareWithRetry(db, expiredSql, expired.out()) != kSqliteOk) {
            throw std::runtime_error(sqliteError(db));
        }
        const auto cutoff = nowMs - static_cast<std::int64_t>(deliveredRetentionDays_) * kDayMs;
        if (g_bind_int64(expired.get(), 1, static_cast<long long>(cutoff)) != kSqliteOk ||
            stepWithRetry(expired.get()) != kSqliteDone) {
            throw std::runtime_error(sqliteError(db));
        }
        expired.finalize();

        const auto countSql =
            "DELETE FROM mqtt_control_result WHERE command_id IN ("
            "SELECT command_id FROM mqtt_control_result WHERE delivered=1 "
            "ORDER BY delivered_at DESC,accepted_at DESC LIMIT -1 OFFSET " +
            std::to_string(maxDeliveredRecords_) + ");";
        execOrThrow(db, countSql.c_str());
        execOrThrow(
            db,
            "DELETE FROM mqtt_control_result_route WHERE NOT EXISTS ("
            "SELECT 1 FROM mqtt_control_result r "
            "WHERE r.command_id=mqtt_control_result_route.command_id);"
        );
        execOrThrow(db, "COMMIT;");
        lastCleanupMs_ = nowMs;
    } catch (...) {
        try { execOrThrow(db, "ROLLBACK;"); } catch (...) {}
        throw;
    }
}

void MqttControlResultStore::loadLibrary() {
    std::call_once(g_sqlite_library_once, [this]() {
        void* handle = nullptr;
#ifdef _WIN32
        handle = !libraryPath_.empty() ? LoadLibraryA(libraryPath_.c_str()) : nullptr;
        if (handle == nullptr) handle = LoadLibraryA("sqlite3.dll");
#else
        handle = !libraryPath_.empty()
            ? dlopen(libraryPath_.c_str(), RTLD_NOW | RTLD_LOCAL)
            : nullptr;
        if (handle == nullptr) handle = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) handle = dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
#endif
        if (handle == nullptr) throw std::runtime_error("failed to load sqlite3 library");

        try {
            g_open = reinterpret_cast<sqlite3_open_v2_fn>(loadSymbol(handle, "sqlite3_open_v2"));
            g_close = reinterpret_cast<sqlite3_close_v2_fn>(loadSymbol(handle, "sqlite3_close_v2"));
            g_exec = reinterpret_cast<sqlite3_exec_fn>(loadSymbol(handle, "sqlite3_exec"));
            g_prepare = reinterpret_cast<sqlite3_prepare_v2_fn>(loadSymbol(handle, "sqlite3_prepare_v2"));
            g_busy_timeout = reinterpret_cast<sqlite3_busy_timeout_fn>(loadSymbol(handle, "sqlite3_busy_timeout"));
            g_bind_int = reinterpret_cast<sqlite3_bind_int_fn>(loadSymbol(handle, "sqlite3_bind_int"));
            g_bind_int64 = reinterpret_cast<sqlite3_bind_int64_fn>(loadSymbol(handle, "sqlite3_bind_int64"));
            g_bind_double = reinterpret_cast<sqlite3_bind_double_fn>(loadSymbol(handle, "sqlite3_bind_double"));
            g_bind_text = reinterpret_cast<sqlite3_bind_text_fn>(loadSymbol(handle, "sqlite3_bind_text"));
            g_step = reinterpret_cast<sqlite3_step_fn>(loadSymbol(handle, "sqlite3_step"));
            g_finalize = reinterpret_cast<sqlite3_finalize_fn>(loadSymbol(handle, "sqlite3_finalize"));
            g_errmsg = reinterpret_cast<sqlite3_errmsg_fn>(loadSymbol(handle, "sqlite3_errmsg"));
            g_free = reinterpret_cast<sqlite3_free_fn>(loadSymbol(handle, "sqlite3_free"));
            g_changes = reinterpret_cast<sqlite3_changes_fn>(loadSymbol(handle, "sqlite3_changes"));
            g_column_int64 = reinterpret_cast<sqlite3_column_int64_fn>(loadSymbol(handle, "sqlite3_column_int64"));
            g_column_int = reinterpret_cast<sqlite3_column_int_fn>(loadSymbol(handle, "sqlite3_column_int"));
            g_column_double = reinterpret_cast<sqlite3_column_double_fn>(loadSymbol(handle, "sqlite3_column_double"));
            g_column_text = reinterpret_cast<sqlite3_column_text_fn>(loadSymbol(handle, "sqlite3_column_text"));
        } catch (...) {
            closeDynamicLibrary(handle);
            throw;
        }
        g_sqlite_library_request = libraryPath_;
        g_sqlite_library_handle = handle;
    });

    if (!libraryPath_.empty() && libraryPath_ != g_sqlite_library_request) {
        throw std::invalid_argument(
            "mqtt control result store cannot switch sqlite libraries within one process"
        );
    }
    libraryHandle_ = g_sqlite_library_handle;
}

void MqttControlResultStore::openDatabase() {
    sqlite3* db = nullptr;
    if (g_open(dbPath_.c_str(), &db, kSqliteOpenReadWrite | kSqliteOpenCreate, nullptr) != kSqliteOk) {
        throw std::runtime_error("failed to open mqtt control result database");
    }
    if (g_busy_timeout != nullptr && g_busy_timeout(db, kSqliteBusyTimeoutMs) != kSqliteOk) {
        g_close(db);
        throw std::runtime_error("failed to configure mqtt control result sqlite timeout");
    }
    databaseHandle_ = db;
}

void MqttControlResultStore::ensureSchema() {
    auto* db = static_cast<sqlite3*>(databaseHandle_);
    execOrThrow(db, "PRAGMA journal_mode=DELETE;");
    execOrThrow(db, "PRAGMA synchronous=FULL;");
    execOrThrow(
        db,
        "CREATE TABLE IF NOT EXISTS mqtt_control_result("
        "command_id TEXT PRIMARY KEY,"
        "fingerprint TEXT NOT NULL,"
        "command_type INTEGER NOT NULL,"
        "target_kw REAL NOT NULL,"
        "generation INTEGER NOT NULL,"
        "accepted_at INTEGER NOT NULL,"
        "deadline_at INTEGER NOT NULL,"
        "submitted INTEGER NOT NULL DEFAULT 0,"
        "final_payload TEXT NOT NULL DEFAULT '',"
        "delivered INTEGER NOT NULL DEFAULT 0,"
        "delivered_at INTEGER NOT NULL DEFAULT 0"
        ");"
    );
    if (!tableHasColumn(db, "mqtt_control_result", "submitted")) {
        execOrThrow(
            db,
            "ALTER TABLE mqtt_control_result ADD COLUMN submitted INTEGER NOT NULL DEFAULT 0;"
        );
    }
    execOrThrow(
        db,
        "CREATE TABLE IF NOT EXISTS mqtt_control_result_route("
        "command_id TEXT NOT NULL,"
        "point_index INTEGER NOT NULL,"
        "shared_memory_name TEXT NOT NULL,"
        "PRIMARY KEY(command_id,point_index)"
        ");"
    );
    execOrThrow(
        db,
        "CREATE INDEX IF NOT EXISTS idx_mqtt_control_result_pending "
        "ON mqtt_control_result(final_payload,delivered,accepted_at);"
    );
}

void MqttControlResultStore::closeDatabase() {
    if (databaseHandle_ != nullptr) {
        g_close(static_cast<sqlite3*>(databaseHandle_));
        databaseHandle_ = nullptr;
    }
}

void MqttControlResultStore::unloadLibrary() {
    // SQLite entry points are process-wide and immutable after first use. Keep
    // the module loaded so one store cannot invalidate another store's calls.
    libraryHandle_ = nullptr;
}

}  // namespace edge_gateway
