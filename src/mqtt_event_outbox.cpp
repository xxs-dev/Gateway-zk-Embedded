#include "edge_gateway/mqtt_event_outbox.hpp"
#include "edge_gateway/sqlite_error.hpp"
#include "edge_gateway/event_store.hpp"
#include "edge_gateway/event_history_projection.hpp"
#include "edge_gateway/event_store_clock.hpp"
#include "edge_gateway/json_value.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <cmath>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
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
using sqlite3_reset_fn = int (*)(sqlite3_stmt*);
using sqlite3_clear_bindings_fn = int (*)(sqlite3_stmt*);
using sqlite3_finalize_fn = int (*)(sqlite3_stmt*);
using sqlite3_errmsg_fn = const char* (*)(sqlite3*);
using sqlite3_free_fn = void (*)(void*);
using sqlite3_last_insert_rowid_fn = long long (*)(sqlite3*);
using sqlite3_changes_fn = int (*)(sqlite3*);
using sqlite3_column_int64_fn = long long (*)(sqlite3_stmt*, int);
using sqlite3_column_int_fn = int (*)(sqlite3_stmt*, int);
using sqlite3_column_double_fn = double (*)(sqlite3_stmt*, int);
using sqlite3_column_text_fn = const unsigned char* (*)(sqlite3_stmt*, int);
using sqlite3_extended_errcode_fn = int (*)(sqlite3*);
using sqlite3_extended_result_codes_fn = int (*)(sqlite3*, int);
using sqlite3_get_autocommit_fn = int (*)(sqlite3*);
using sqlite3_db_handle_fn = sqlite3* (*)(sqlite3_stmt*);

constexpr int kSqliteOk = 0;
constexpr int kSqliteBusy = 5;
constexpr int kSqliteBusySnapshot = 517;
constexpr int kSqliteConstraintTrigger = 1811;
constexpr int kSqliteRow = 100;
constexpr int kSqliteDone = 101;
constexpr int kSqliteOpenReadWrite = 0x00000002;
constexpr int kSqliteOpenReadOnly = 0x00000001;
constexpr int kSqliteOpenPrivateCache = 0x00040000;
constexpr int kSqliteOpenCreate = 0x00000004;
constexpr int kSqliteBusyTimeoutMs = 25;
constexpr int kSqliteSchemaBusyTimeoutMs = 15 * 60 * 1000;
constexpr int kSqliteStatsMigrationBusyTimeoutMs = 5000;
constexpr int kSqliteRetryAttempts = 4;
constexpr int kEnqueueBusyTimeoutMs = 100;
constexpr int kEnqueueTransactionRetryAttempts = 8;
constexpr int kReplayBusyTimeoutMs = 250;
constexpr int kReplayTransactionRetryAttempts = 8;
constexpr std::uint64_t kStatsMigrationTargetBatchBytes = 8ULL * 1024ULL * 1024ULL;
constexpr int kStatsMigrationMinBatchRows = 256;
constexpr int kStatsMigrationMaxBatchRows = 32768;
constexpr int kStatsMigrationPauseMs = 2;
constexpr std::int64_t kReplayClaimLeaseMs = 60 * 1000;
constexpr std::size_t kReplayClaimBatchSize = 16;
constexpr std::size_t kReplayPublishBatchSize = 8;
constexpr int kReplayFairnessPauseMs = 5;

class TargetCapacityError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

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
sqlite3_reset_fn g_reset = nullptr;
sqlite3_clear_bindings_fn g_clear_bindings = nullptr;
sqlite3_finalize_fn g_finalize = nullptr;
sqlite3_errmsg_fn g_errmsg = nullptr;
sqlite3_free_fn g_free = nullptr;
sqlite3_last_insert_rowid_fn g_last_insert_rowid = nullptr;
sqlite3_changes_fn g_changes = nullptr;
sqlite3_column_int64_fn g_column_int64 = nullptr;
sqlite3_column_int_fn g_column_int = nullptr;
sqlite3_column_double_fn g_column_double = nullptr;
sqlite3_column_text_fn g_column_text = nullptr;
sqlite3_extended_errcode_fn g_extended_errcode = nullptr;
sqlite3_extended_result_codes_fn g_extended_result_codes = nullptr;
sqlite3_get_autocommit_fn g_get_autocommit = nullptr;
sqlite3_db_handle_fn g_db_handle = nullptr;
std::mutex g_library_mutex;
void* g_library_handle = nullptr;
std::string g_library_path;

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
    return db && g_errmsg ? g_errmsg(db) : "sqlite error";
}

SqliteError sqliteFailure(sqlite3* db, int rc, const char* operation,
    const std::string& message = std::string()) {
    const auto extended = db && g_extended_errcode ? g_extended_errcode(db) : rc;
    const auto effective = (extended & 255) == (rc & 255) ? extended : rc;
    return SqliteError(effective, operation,
        db && g_get_autocommit && g_get_autocommit(db) == 0,
        message.empty() ? sqliteError(db) : message);
}

bool retryStatement(sqlite3* db, int rc) {
    if ((rc & 255) != kSqliteBusy) return false;
    // A stale read snapshot requires a new transaction, never the same statement.
    return rc != kSqliteBusySnapshot &&
        (!db || g_extended_errcode(db) != kSqliteBusySnapshot);
}

const char* sqlOperation(const char* sql) {
    if (std::strncmp(sql, "COMMIT", 6) == 0) return "commit";
    if (std::strncmp(sql, "BEGIN", 5) == 0) return "begin";
    if (std::strncmp(sql, "ROLLBACK TO", 11) == 0) return "rollback.savepoint";
    if (std::strncmp(sql, "ROLLBACK", 8) == 0) return "rollback";
    if (std::strncmp(sql, "SAVEPOINT", 9) == 0) return "savepoint";
    if (std::strncmp(sql, "RELEASE", 7) == 0) return "release.savepoint";
    return "exec";
}

void waitBeforeBusyRetry(int attempt) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto baseMs = static_cast<unsigned int>(2U << std::min(attempt, 5));
    const auto jitterMs = static_cast<unsigned int>(
        sequence.fetch_add(1, std::memory_order_relaxed) % (baseMs + 1U)
    );
    std::this_thread::sleep_for(std::chrono::milliseconds(baseMs + jitterMs));
}

void waitBeforeEnqueueRetry(int attempt) {
    static std::atomic<std::uint64_t> sequence{0};
    static const int delaysMs[] = {5, 10, 20, 40, 50, 50, 50};
    const auto delayIndex = std::min(
        attempt,
        static_cast<int>(sizeof(delaysMs) / sizeof(delaysMs[0])) - 1
    );
    const auto baseMs = static_cast<unsigned int>(delaysMs[delayIndex]);
    const auto jitterMs = static_cast<unsigned int>(
        sequence.fetch_add(1, std::memory_order_relaxed) % (baseMs / 2U + 1U)
    );
    std::this_thread::sleep_for(std::chrono::milliseconds(baseMs + jitterMs));
}

template <typename Operation>
void retryReplayTransaction(const char*, Operation operation) {
    for (int attempt = 0; attempt < kReplayTransactionRetryAttempts; ++attempt) {
        try {
            operation();
            return;
        } catch (const SqliteError& ex) {
            if (!ex.isBusy() || attempt + 1 >= kReplayTransactionRetryAttempts) {
                throw;
            }
            waitBeforeEnqueueRetry(attempt);
        }
    }
}

void execOrThrow(sqlite3* db, const char* sql) {
    for (int attempt = 0; attempt < kSqliteRetryAttempts; ++attempt) {
        char* errorMessage = nullptr;
        const auto rc = g_exec(db, sql, nullptr, nullptr, &errorMessage);
        if (rc == kSqliteOk) {
            if (errorMessage != nullptr && g_free != nullptr) {
                g_free(errorMessage);
            }
            return;
        }
        const auto failure = sqliteFailure(db, rc, sqlOperation(sql),
            errorMessage != nullptr ? errorMessage : sqliteError(db));
        if (errorMessage != nullptr && g_free != nullptr) {
            g_free(errorMessage);
        }
        if (retryStatement(db, rc) && attempt + 1 < kSqliteRetryAttempts) {
            waitBeforeBusyRetry(attempt);
            continue;
        }
        throw failure;
    }
}

void execOnceOrThrow(sqlite3* db, const char* sql) {
    char* errorMessage = nullptr;
    const auto rc = g_exec(db, sql, nullptr, nullptr, &errorMessage);
    if (rc == kSqliteOk) {
        if (errorMessage != nullptr && g_free != nullptr) {
            g_free(errorMessage);
        }
        return;
    }
    const auto failure = sqliteFailure(db, rc, sqlOperation(sql),
        errorMessage != nullptr ? errorMessage : sqliteError(db));
    if (errorMessage != nullptr && g_free != nullptr) {
        g_free(errorMessage);
    }
    throw failure;
}

void setBusyTimeoutOrThrow(sqlite3* db, int timeoutMs) {
    const auto rc = g_busy_timeout(db, timeoutMs);
    if (rc != kSqliteOk) throw sqliteFailure(db, rc, "configure.busy_timeout");
}

class BusyTimeoutScope {
public:
    BusyTimeoutScope(sqlite3* db, int scopedTimeoutMs, int restoreTimeoutMs, bool* poisoned)
        : db_(db), restoreTimeoutMs_(restoreTimeoutMs), poisoned_(poisoned) {
        setBusyTimeoutOrThrow(db_, scopedTimeoutMs);
    }

    ~BusyTimeoutScope() {
        restore();
    }

    void restore() noexcept {
        if (db_ && g_busy_timeout(db_, restoreTimeoutMs_) != kSqliteOk) *poisoned_ = true;
        db_ = nullptr;
    }

private:
    sqlite3* db_;
    int restoreTimeoutMs_;
    bool* poisoned_;
};

int prepareWithRetry(sqlite3* db, const char* sql, sqlite3_stmt** stmt) {
    for (int attempt = 0; attempt < kSqliteRetryAttempts; ++attempt) {
        const auto rc = g_prepare(db, sql, -1, stmt, nullptr);
        if (rc == kSqliteOk) {
            return rc;
        }
        if (!retryStatement(db, rc) || attempt + 1 >= kSqliteRetryAttempts) {
            return rc;
        }
        if (*stmt != nullptr) {
            g_finalize(*stmt);
            *stmt = nullptr;
        }
        waitBeforeBusyRetry(attempt);
    }
    return kSqliteBusy;
}

int stepWithRetry(sqlite3_stmt* stmt) {
    for (int attempt = 0; attempt < kSqliteRetryAttempts; ++attempt) {
        const auto rc = g_step(stmt);
        if (rc == kSqliteDone || rc == kSqliteRow) {
            return rc;
        }
        if (!retryStatement(g_db_handle(stmt), rc) || attempt + 1 >= kSqliteRetryAttempts) {
            return rc;
        }
        waitBeforeBusyRetry(attempt);
    }
    return kSqliteBusy;
}

void throwSqliteResult(sqlite3* db, int rc, const char* operation = "statement") {
    throw sqliteFailure(db, rc, operation);
}

bool isOptionalTargetFailure(const std::exception_ptr& failure) {
    try {
        std::rethrow_exception(failure);
    } catch (const TargetCapacityError&) {
        return true;
    } catch (const SqliteError& ex) {
        return ex.extendedCode() == kSqliteConstraintTrigger &&
            ex.operation() == "enqueue.event.insert" && ex.transactionActive();
    } catch (...) {
        return false;
    }
}

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::string newClaimToken() {
    static std::atomic<std::uint64_t> sequence{0};
#ifdef _WIN32
    const auto processId = static_cast<unsigned long long>(GetCurrentProcessId());
#else
    const auto processId = static_cast<unsigned long long>(getpid());
#endif
    return std::to_string(processId) + ":" + std::to_string(currentTimeMs()) + ":" +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

std::string columnText(sqlite3_stmt* stmt, int index) {
    const auto* text = g_column_text(stmt, index);
    return text == nullptr ? std::string() : reinterpret_cast<const char*>(text);
}

std::string normalizedTargetId(const std::string& targetId) {
    return targetId.empty() ? "main" : targetId;
}

std::vector<std::string> nonEmptyTypes(const std::vector<std::string>& types) {
    std::vector<std::string> result;
    result.reserve(types.size());
    for (const auto& type : types) {
        if (!type.empty() && std::find(result.begin(), result.end(), type) == result.end()) {
            result.push_back(type);
        }
    }
    return result;
}

void appendPlaceholders(std::ostringstream& sql, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0) {
            sql << ",";
        }
        sql << "?";
    }
}

struct NormalizedEventTypeFilter {
    std::vector<std::string> include;
    std::vector<std::string> exclude;
};

int replayEventTypePriority(const std::string& eventType) {
    if (eventType == "alarm") {
        return 0;
    }
    if (eventType == "change") {
        return 1;
    }
    return 2;
}

NormalizedEventTypeFilter normalizeEventTypeFilter(
    const MqttEventOutbox::EventTypeFilter& filter
) {
    return NormalizedEventTypeFilter{
        nonEmptyTypes(filter.include),
        nonEmptyTypes(filter.exclude)
    };
}

void appendEventTypeFilterSql(
    std::ostringstream& sql,
    const NormalizedEventTypeFilter& filter
) {
    if (!filter.include.empty()) {
        sql << " AND event_type IN (";
        appendPlaceholders(sql, filter.include.size());
        sql << ")";
    }
    if (!filter.exclude.empty()) {
        sql << " AND event_type NOT IN (";
        appendPlaceholders(sql, filter.exclude.size());
        sql << ")";
    }
}

int bindEventTypeFilter(
    sqlite3_stmt* stmt,
    int bindIndex,
    const NormalizedEventTypeFilter& filter
) {
    for (const auto& type : filter.include) {
        if (g_bind_text(stmt, bindIndex++, type.c_str(), -1, nullptr) != kSqliteOk) {
            return -1;
        }
    }
    for (const auto& type : filter.exclude) {
        if (g_bind_text(stmt, bindIndex++, type.c_str(), -1, nullptr) != kSqliteOk) {
            return -1;
        }
    }
    return bindIndex;
}

bool tableHasColumn(sqlite3* db, const char* table, const char* column) {
    std::string sql = std::string("PRAGMA table_info(") + table + ");";
    sqlite3_stmt* stmt = nullptr;
    if (prepareWithRetry(db, sql.c_str(), &stmt) != kSqliteOk) {
        throw std::runtime_error(sqliteError(db));
    }
    bool found = false;
    for (;;) {
        const auto rc = stepWithRetry(stmt);
        if (rc == kSqliteDone) {
            break;
        }
        if (rc != kSqliteRow) {
            g_finalize(stmt);
            throw std::runtime_error(sqliteError(db));
        }
        if (columnText(stmt, 1) == column) {
            found = true;
            break;
        }
    }
    g_finalize(stmt);
    return found;
}

bool tableHasIndex(sqlite3* db, const char* table, const char* index) {
    std::string sql = std::string("PRAGMA index_list(") + table + ");";
    sqlite3_stmt* stmt = nullptr;
    if (prepareWithRetry(db, sql.c_str(), &stmt) != kSqliteOk) {
        throw std::runtime_error(sqliteError(db));
    }
    bool found = false;
    for (;;) {
        const auto rc = stepWithRetry(stmt);
        if (rc == kSqliteDone) {
            break;
        }
        if (rc != kSqliteRow) {
            g_finalize(stmt);
            throw std::runtime_error(sqliteError(db));
        }
        if (columnText(stmt, 1) == index) {
            found = true;
            break;
        }
    }
    g_finalize(stmt);
    return found;
}

}  // namespace

MqttEventOutbox::MqttEventOutbox(
    std::string dbPath,
    std::string libraryPath,
    int retentionMonths,
    int cleanupIntervalHours,
    std::size_t replayBatchSize,
    std::size_t maxDiskBytes,
    StorageProfile storageProfile,
    AccessMode accessMode,
    int retentionDays
) : dbPath_(std::move(dbPath)),
    libraryPath_(std::move(libraryPath)),
    retentionDays_(std::clamp(retentionDays, 1, 3650)),
    cleanupIntervalHours_(cleanupIntervalHours <= 0 ? 24 : cleanupIntervalHours),
    replayBatchSize_(replayBatchSize == 0 ? 100 : replayBatchSize),
    maxDiskBytes_(maxDiskBytes),
    storageProfile_(storageProfile),
    accessMode_(accessMode) {
    (void)retentionMonths; // Legacy constructor compatibility; global days supersede calendar months.
    try {
        loadLibrary();
        openDatabase();
        if (accessMode_ == AccessMode::ReadWrite) {
            ensureSchema();
            resumeStatsMigration();
        } else {
            execOnceOrThrow(static_cast<sqlite3*>(checkedDatabase()), "PRAGMA query_only=ON;");
        }
    } catch (...) {
        closeDatabase();
        unloadLibrary();
        throw;
    }
}

MqttEventOutbox::~MqttEventOutbox() {
    closeDatabase();
    unloadLibrary();
}

std::int64_t MqttEventOutbox::enqueue(
    const std::string& eventType,
    const std::string& topic,
    const std::string& payload,
    std::int64_t eventTs
) {
    std::vector<EventMessage> events;
    EventMessage event;
    event.eventType = eventType;
    event.topic = topic;
    event.payload = payload;
    event.eventTs = eventTs;
    events.push_back(event);
    const auto ids = enqueueBatch(events);
    return ids.empty() ? 0 : ids.front();
}

std::vector<std::int64_t> MqttEventOutbox::enqueueBatch(const std::vector<EventMessage>& events) {
    return enqueueBatchAndStates(events, {}, false).ids;
}

std::vector<std::int64_t> MqttEventOutbox::enqueueBatchWithStates(
    const std::vector<EventMessage>& events,
    const std::vector<EventState>& states
) {
    return enqueueBatchAndStates(events, states, false).ids;
}

MqttEventOutbox::FanoutEnqueueResult MqttEventOutbox::enqueueFanoutWithStates(
    const std::vector<EventMessage>& events,
    const std::vector<EventState>& states
) {
    return enqueueBatchAndStates(events, states, true);
}

MqttEventOutbox::FanoutEnqueueResult MqttEventOutbox::enqueueBatchAndStates(
    const std::vector<EventMessage>& events,
    const std::vector<EventState>& states,
    bool isolateOptionalTargets,
    bool manageTransaction
) {
    if (events.empty() && states.empty()) {
        return FanoutEnqueueResult();
    }

    auto* db = static_cast<sqlite3*>(checkedDatabase());
    const char* eventSql =
        "INSERT OR IGNORE INTO mqtt_event_outbox("
        "event_id, target_id, event_type, topic, payload, event_ts, event_month, created_at, sent) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, 0);";
    const char* stateSql =
        "INSERT OR REPLACE INTO mqtt_event_state("
        "state_key, event_type, point_index, alarm_type, active, value, quality, source_ts, lifecycle, updated_at) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    struct TargetGroup {
        std::string targetId;
        std::vector<const EventMessage*> events;
    };
    std::vector<TargetGroup> groups;
    for (const auto& event : events) {
        if (event.topic.empty()) {
            continue;
        }
        const auto targetId = normalizedTargetId(event.targetId);
        auto group = std::find_if(groups.begin(), groups.end(), [&](const TargetGroup& candidate) {
            return candidate.targetId == targetId;
        });
        if (group == groups.end()) {
            groups.push_back(TargetGroup{targetId, {}});
            group = groups.end() - 1;
        }
        group->events.push_back(&event);
    }

    const auto enqueueOnce = [&]() {
        (void)checkedDatabase();
        FanoutEnqueueResult result;
        result.ids.reserve(events.size());
        sqlite3_stmt* eventStmt = nullptr;
        sqlite3_stmt* stateStmt = nullptr;
        BusyTimeoutScope enqueueTimeout(db, kEnqueueBusyTimeoutMs, kSqliteBusyTimeoutMs, &databasePoisoned_);
        try {
            if (manageTransaction) execOnceOrThrow(db, "BEGIN IMMEDIATE;");
            else if (g_get_autocommit(db)) throw std::logic_error("event store outer transaction missing");
        if (!groups.empty()) {
            const auto prepareRc = prepareWithRetry(db, eventSql, &eventStmt);
            if (prepareRc != kSqliteOk) {
                throwSqliteResult(db, prepareRc);
            }
        }

        const auto createdAt = currentTimeMs();
        for (std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
            const auto& group = groups[groupIndex];
            const auto savepoint = std::string("mqtt_target_") + std::to_string(groupIndex);
            execOrThrow(db, ("SAVEPOINT " + savepoint + ";").c_str());
            std::vector<std::int64_t> groupIds;
            groupIds.reserve(group.events.size());
            try {
                for (const auto* event : group.events) {
                    const auto ts = event->eventTs > 0 ? event->eventTs : createdAt;
                    const auto month = eventMonth(ts);
                    if (g_bind_text(eventStmt, 1, event->eventId.c_str(), -1, nullptr) != kSqliteOk ||
                        g_bind_text(eventStmt, 2, group.targetId.c_str(), -1, nullptr) != kSqliteOk ||
                        g_bind_text(eventStmt, 3, event->eventType.c_str(), -1, nullptr) != kSqliteOk ||
                        g_bind_text(eventStmt, 4, event->topic.c_str(), -1, nullptr) != kSqliteOk ||
                        g_bind_text(eventStmt, 5, event->payload.c_str(), -1, nullptr) != kSqliteOk ||
                        g_bind_int64(eventStmt, 6, static_cast<long long>(ts)) != kSqliteOk ||
                        g_bind_text(eventStmt, 7, month.c_str(), -1, nullptr) != kSqliteOk ||
                        g_bind_int64(eventStmt, 8, static_cast<long long>(createdAt)) != kSqliteOk) {
                        throw std::runtime_error(sqliteError(db));
                    }
                    const auto stepRc = stepWithRetry(eventStmt);
                    if (stepRc != kSqliteDone) {
                        throwSqliteResult(db, stepRc, "enqueue.event.insert");
                    }
                    if (g_changes(db) > 0) {
                        groupIds.push_back(static_cast<std::int64_t>(g_last_insert_rowid(db)));
                    }
                    if (g_reset(eventStmt) != kSqliteOk || g_clear_bindings(eventStmt) != kSqliteOk) {
                        throw std::runtime_error(sqliteError(db));
                    }
                }
                if (!groupIds.empty()) {
                    enforceDiskLimit(groupIds, group.targetId);
                }
                execOrThrow(db, ("RELEASE " + savepoint + ";").c_str());
                result.ids.insert(result.ids.end(), groupIds.begin(), groupIds.end());
            } catch (...) {
                const auto failure = std::current_exception();
                const bool transactionActive = g_get_autocommit(db) == 0;
                (void)g_reset(eventStmt);
                (void)g_clear_bindings(eventStmt);
                if (!transactionActive || !isolateOptionalTargets || group.targetId == "main" ||
                    !isOptionalTargetFailure(failure)) {
                    std::rethrow_exception(failure);
                }
                try {
                    execOnceOrThrow(db, ("ROLLBACK TO " + savepoint + ";").c_str());
                    execOnceOrThrow(db, ("RELEASE " + savepoint + ";").c_str());
                } catch (...) {
                    // A cleanup failure aborts the outer transaction, preserving its first cause.
                    std::rethrow_exception(failure);
                }
                result.failedTargetIds.push_back(group.targetId);
            }
        }

        if (eventStmt != nullptr) {
            g_finalize(eventStmt);
            eventStmt = nullptr;
        }

        if (!states.empty()) {
            const auto prepareRc = prepareWithRetry(db, stateSql, &stateStmt);
            if (prepareRc != kSqliteOk) {
                throwSqliteResult(db, prepareRc);
            }
        }
        for (const auto& state : states) {
            if (state.stateKey.empty()) {
                throw std::runtime_error("mqtt event state key must not be empty");
            }
            if (g_bind_text(stateStmt, 1, state.stateKey.c_str(), -1, nullptr) != kSqliteOk ||
                g_bind_text(stateStmt, 2, state.eventType.c_str(), -1, nullptr) != kSqliteOk ||
                g_bind_int64(stateStmt, 3, static_cast<long long>(state.index)) != kSqliteOk ||
                g_bind_text(stateStmt, 4, state.alarmType.c_str(), -1, nullptr) != kSqliteOk ||
                g_bind_int(stateStmt, 5, state.active ? 1 : 0) != kSqliteOk ||
                g_bind_double(stateStmt, 6, state.value) != kSqliteOk ||
                g_bind_int(stateStmt, 7, state.quality) != kSqliteOk ||
                g_bind_int64(stateStmt, 8, static_cast<long long>(state.sourceTs)) != kSqliteOk ||
                g_bind_text(stateStmt, 9, state.lifecycle.c_str(), -1, nullptr) != kSqliteOk ||
                g_bind_int64(stateStmt, 10, static_cast<long long>(createdAt)) != kSqliteOk) {
                throw std::runtime_error(sqliteError(db));
            }
            const auto stepRc = stepWithRetry(stateStmt);
            if (stepRc != kSqliteDone) {
                throwSqliteResult(db, stepRc);
            }
            if (g_reset(stateStmt) != kSqliteOk || g_clear_bindings(stateStmt) != kSqliteOk) {
                throw std::runtime_error(sqliteError(db));
            }
        }
        if (stateStmt != nullptr) {
            g_finalize(stateStmt);
            stateStmt = nullptr;
        }

            if (manageTransaction) execOrThrow(db, "COMMIT;");
            enqueueTimeout.restore();
        } catch (...) {
            if (eventStmt != nullptr) {
                g_finalize(eventStmt);
            }
            if (stateStmt != nullptr) {
                g_finalize(stateStmt);
            }
            rollbackAfterFailure();
            throw;
        }
        return result;
    };

    for (int attempt = 0; attempt < kEnqueueTransactionRetryAttempts; ++attempt) {
        try {
            return enqueueOnce();
        } catch (const SqliteError& ex) {
            if (!manageTransaction || !ex.isBusy() || databasePoisoned_ || attempt + 1 >= kEnqueueTransactionRetryAttempts) {
                throw;
            }
            waitBeforeEnqueueRetry(attempt);
        }
    }
    throw std::runtime_error("mqtt event enqueue retry loop exited unexpectedly");
}

std::vector<MqttEventOutbox::EventState> MqttEventOutbox::loadStates() {
    std::vector<EventState> states;
    auto* db = static_cast<sqlite3*>(checkedDatabase());
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT state_key, event_type, point_index, alarm_type, active, value, quality, source_ts, lifecycle "
        "FROM mqtt_event_state ORDER BY state_key;";
    if (prepareWithRetry(db, sql, &stmt) != kSqliteOk) {
        throw std::runtime_error(sqliteError(db));
    }
    for (;;) {
        const auto rc = stepWithRetry(stmt);
        if (rc == kSqliteDone) {
            break;
        }
        if (rc != kSqliteRow) {
            g_finalize(stmt);
            throw std::runtime_error(sqliteError(db));
        }
        EventState state;
        state.stateKey = columnText(stmt, 0);
        state.eventType = columnText(stmt, 1);
        state.index = static_cast<std::uint32_t>(g_column_int64(stmt, 2));
        state.alarmType = columnText(stmt, 3);
        state.active = g_column_int(stmt, 4) != 0;
        state.value = g_column_double(stmt, 5);
        state.quality = g_column_int(stmt, 6);
        state.sourceTs = static_cast<std::int64_t>(g_column_int64(stmt, 7));
        state.lifecycle = columnText(stmt, 8);
        states.push_back(std::move(state));
    }
    g_finalize(stmt);
    return states;
}

void MqttEventOutbox::markSent(std::int64_t id, std::int64_t sentAt) {
    auto* db = static_cast<sqlite3*>(checkedDatabase());
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE mqtt_event_outbox SET sent=1, sent_at=? WHERE id=?;";
    if (prepareWithRetry(db, sql, &stmt) != kSqliteOk) {
        throw std::runtime_error(sqliteError(db));
    }
    if (g_bind_int64(stmt, 1, static_cast<long long>(sentAt)) != kSqliteOk ||
        g_bind_int64(stmt, 2, static_cast<long long>(id)) != kSqliteOk ||
        stepWithRetry(stmt) != kSqliteDone) {
        g_finalize(stmt);
        throw std::runtime_error(sqliteError(db));
    }
    g_finalize(stmt);
}

void MqttEventOutbox::markSentBatch(const std::vector<std::int64_t>& ids, std::int64_t sentAt) {
    if (ids.empty()) {
        return;
    }

    auto* db = static_cast<sqlite3*>(checkedDatabase());
    std::ostringstream sql;
    sql << "UPDATE mqtt_event_outbox SET sent=1, sent_at=" << sentAt << " WHERE id IN (";
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) {
            sql << ",";
        }
        sql << ids[i];
    }
    sql << ");";

    try {
        execOrThrow(db, "BEGIN IMMEDIATE;");
        execOrThrow(db, sql.str().c_str());
        execOrThrow(db, "COMMIT;");
    } catch (...) {
        rollbackAfterFailure();
        throw;
    }
}

std::size_t MqttEventOutbox::pendingCount() {
    return pendingCount("main", EventTypeFilter());
}

std::size_t MqttEventOutbox::pendingCount(const std::string& targetId) {
    return pendingCount(targetId, EventTypeFilter());
}

std::size_t MqttEventOutbox::pendingCount(
    const std::string& targetId,
    const EventTypeFilter& eventTypes
) {
    auto* db = static_cast<sqlite3*>(checkedDatabase());
    sqlite3_stmt* stmt = nullptr;
    const auto normalizedFilter = normalizeEventTypeFilter(eventTypes);
    std::ostringstream sql;
    sql << "SELECT COALESCE(SUM(pending_count),0) "
           "FROM mqtt_event_outbox_stats WHERE target_id=?";
    appendEventTypeFilterSql(sql, normalizedFilter);
    sql << ";";
    if (prepareWithRetry(db, sql.str().c_str(), &stmt) != kSqliteOk) {
        throw std::runtime_error(sqliteError(db));
    }
    const auto normalizedTarget = normalizedTargetId(targetId);
    if (g_bind_text(stmt, 1, normalizedTarget.c_str(), -1, nullptr) != kSqliteOk ||
        bindEventTypeFilter(stmt, 2, normalizedFilter) < 0) {
        g_finalize(stmt);
        throw std::runtime_error(sqliteError(db));
    }
    std::size_t count = 0;
    const auto rc = stepWithRetry(stmt);
    if (rc == kSqliteRow) {
        count = static_cast<std::size_t>(g_column_int64(stmt, 0));
    } else if (rc != kSqliteDone) {
        g_finalize(stmt);
        throw std::runtime_error(sqliteError(db));
    }
    g_finalize(stmt);
    return count;
}

std::size_t MqttEventOutbox::replay(const std::function<void(const std::string&, const std::string&)>& send) {
    return replayWithStats("main", EventTypeFilter(), 0, send).count;
}

std::size_t MqttEventOutbox::replay(
    const std::string& targetId,
    const std::function<void(const std::string&, const std::string&)>& send
) {
    return replayWithStats(targetId, EventTypeFilter(), 0, send).count;
}

std::size_t MqttEventOutbox::replay(
    const std::string& targetId,
    const EventTypeFilter& eventTypes,
    const std::function<void(const std::string&, const std::string&)>& send
) {
    return replayWithStats(targetId, eventTypes, 0, send).count;
}

std::size_t MqttEventOutbox::replay(
    std::size_t maxBytes,
    const std::function<void(const std::string&, const std::string&)>& send
) {
    return replayWithStats("main", EventTypeFilter(), maxBytes, send).count;
}

std::size_t MqttEventOutbox::replay(
    const std::string& targetId,
    std::size_t maxBytes,
    const std::function<void(const std::string&, const std::string&)>& send
) {
    return replayWithStats(targetId, EventTypeFilter(), maxBytes, send).count;
}

std::size_t MqttEventOutbox::replay(
    const std::string& targetId,
    const EventTypeFilter& eventTypes,
    std::size_t maxBytes,
    const std::function<void(const std::string&, const std::string&)>& send
) {
    return replayWithStats(targetId, eventTypes, maxBytes, send).count;
}

MqttEventOutbox::ReplayStats MqttEventOutbox::replayWithStats(
    const std::function<void(const std::string&, const std::string&)>& send
) {
    return replayWithStats("main", EventTypeFilter(), 0, send);
}

MqttEventOutbox::ReplayStats MqttEventOutbox::replayWithStats(
    const std::string& targetId,
    const std::function<void(const std::string&, const std::string&)>& send
) {
    return replayWithStats(targetId, EventTypeFilter(), 0, send);
}

MqttEventOutbox::ReplayStats MqttEventOutbox::replayWithStats(
    const std::string& targetId,
    const EventTypeFilter& eventTypes,
    const std::function<void(const std::string&, const std::string&)>& send
) {
    return replayWithStats(targetId, eventTypes, 0, send);
}

MqttEventOutbox::ReplayStats MqttEventOutbox::replayWithStats(
    std::size_t maxBytes,
    const std::function<void(const std::string&, const std::string&)>& send
) {
    return replayWithStats("main", EventTypeFilter(), maxBytes, send);
}

MqttEventOutbox::ReplayStats MqttEventOutbox::replayWithStats(
    const std::string& targetId,
    std::size_t maxBytes,
    const std::function<void(const std::string&, const std::string&)>& send
) {
    return replayWithStats(targetId, EventTypeFilter(), maxBytes, send);
}

MqttEventOutbox::ReplayStats MqttEventOutbox::replayWithStats(
    const std::string& targetId,
    const EventTypeFilter& eventTypes,
    std::size_t maxBytes,
    const std::function<void(const std::string&, const std::string&)>& send
) {
    return replayWithStats(targetId, eventTypes, maxBytes, 0, send);
}

MqttEventOutbox::ReplayStats MqttEventOutbox::replayWithStats(
    const std::string& targetId,
    const EventTypeFilter& eventTypes,
    std::size_t maxBytes,
    std::size_t maxCount,
    const std::function<void(const std::string&, const std::string&)>& send
) {
    return replayWithStatsInternal(
        targetId,
        eventTypes,
        maxBytes,
        maxCount,
        send,
        std::function<void(const std::vector<ReplayMessage>&)>()
    );
}

MqttEventOutbox::ReplayStats MqttEventOutbox::replayBatchWithStats(
    const std::string& targetId,
    const EventTypeFilter& eventTypes,
    std::size_t maxBytes,
    std::size_t maxCount,
    const std::function<void(const std::vector<ReplayMessage>&)>& send
) {
    return replayWithStatsInternal(
        targetId,
        eventTypes,
        maxBytes,
        maxCount,
        std::function<void(const std::string&, const std::string&)>(),
        send
    );
}

MqttEventOutbox::ReplayStats MqttEventOutbox::replayWithStatsInternal(
    const std::string& targetId,
    const EventTypeFilter& eventTypes,
    std::size_t maxBytes,
    std::size_t maxCount,
    const std::function<void(const std::string&, const std::string&)>& send,
    const std::function<void(const std::vector<ReplayMessage>&)>& batchSend
) {
    struct Row {
        std::int64_t id = 0;
        std::int64_t eventTs = 0;
        std::string eventType;
        std::string topic;
        std::string payload;
    };
    auto* db = static_cast<sqlite3*>(checkedDatabase());
    const auto normalizedFilter = normalizeEventTypeFilter(eventTypes);
    const auto normalizedTarget = normalizedTargetId(targetId);
    const auto replayLimit = maxCount == 0
        ? replayBatchSize_
        : std::min(replayBatchSize_, maxCount);
    ReplayStats stats;
    while (stats.count < replayLimit) {
        std::vector<Row> rows;
        const auto claimToken = newClaimToken();
        const auto claimLimit = std::min(kReplayClaimBatchSize, replayLimit - stats.count);
        std::size_t claimedBytes = 0;
        const auto claimedAt = currentTimeMs();
        retryReplayTransaction("mqtt event replay claim remained busy", [&]() {
            (void)checkedDatabase();
            rows.clear();
            claimedBytes = 0;
            sqlite3_stmt* typeStmt = nullptr;
            sqlite3_stmt* selectStmt = nullptr;
            sqlite3_stmt* claimStmt = nullptr;
            BusyTimeoutScope claimBusyTimeout(db, kReplayBusyTimeoutMs, kSqliteBusyTimeoutMs, &databasePoisoned_);
            try {
                execOnceOrThrow(db, "BEGIN IMMEDIATE;");

                std::ostringstream typeSql;
                typeSql << "SELECT event_type FROM mqtt_event_outbox_stats "
                           "WHERE target_id=? AND pending_count>0";
                appendEventTypeFilterSql(typeSql, normalizedFilter);
                typeSql << " ORDER BY event_type;";
                const auto typePrepareRc = prepareWithRetry(db, typeSql.str().c_str(), &typeStmt);
                if (typePrepareRc != kSqliteOk) {
                    throwSqliteResult(db, typePrepareRc);
                }
                auto bindIndex = 1;
                if (g_bind_text(typeStmt, bindIndex++, normalizedTarget.c_str(), -1, nullptr) != kSqliteOk) {
                    throw std::runtime_error(sqliteError(db));
                }
                bindIndex = bindEventTypeFilter(typeStmt, bindIndex, normalizedFilter);
                if (bindIndex < 0) {
                    throw std::runtime_error(sqliteError(db));
                }
                std::vector<std::string> pendingTypes;
                for (;;) {
                    const auto typeRc = stepWithRetry(typeStmt);
                    if (typeRc == kSqliteDone) {
                        break;
                    }
                    if (typeRc != kSqliteRow) {
                        throwSqliteResult(db, typeRc);
                    }
                    pendingTypes.push_back(columnText(typeStmt, 0));
                }
                g_finalize(typeStmt);
                typeStmt = nullptr;

                const char* candidateSql =
                    "SELECT id,event_type,topic,payload,event_ts FROM mqtt_event_outbox "
                    "WHERE target_id=? AND sent=0 AND event_type=? "
                    "AND (claim_until IS NULL OR claim_until<=?) "
                    "ORDER BY event_ts ASC,id ASC LIMIT ?;";
                for (const auto& eventType : pendingTypes) {
                    const auto candidatePrepareRc = prepareWithRetry(db, candidateSql, &selectStmt);
                    if (candidatePrepareRc != kSqliteOk) {
                        throwSqliteResult(db, candidatePrepareRc);
                    }
                    if (g_bind_text(selectStmt, 1, normalizedTarget.c_str(), -1, nullptr) != kSqliteOk ||
                        g_bind_text(selectStmt, 2, eventType.c_str(), -1, nullptr) != kSqliteOk ||
                        g_bind_int64(selectStmt, 3, static_cast<long long>(claimedAt)) != kSqliteOk ||
                        g_bind_int64(selectStmt, 4, static_cast<long long>(claimLimit)) != kSqliteOk) {
                        throw std::runtime_error(sqliteError(db));
                    }
                    for (;;) {
                        const auto candidateRc = stepWithRetry(selectStmt);
                        if (candidateRc == kSqliteDone) {
                            break;
                        }
                        if (candidateRc != kSqliteRow) {
                            throwSqliteResult(db, candidateRc);
                        }
                        Row row;
                        row.id = static_cast<std::int64_t>(g_column_int64(selectStmt, 0));
                        row.eventType = columnText(selectStmt, 1);
                        row.topic = columnText(selectStmt, 2);
                        row.payload = columnText(selectStmt, 3);
                        row.eventTs = static_cast<std::int64_t>(g_column_int64(selectStmt, 4));
                        rows.push_back(std::move(row));
                    }
                    g_finalize(selectStmt);
                    selectStmt = nullptr;
                }

                std::sort(rows.begin(), rows.end(), [](const Row& left, const Row& right) {
                    const auto leftPriority = replayEventTypePriority(left.eventType);
                    const auto rightPriority = replayEventTypePriority(right.eventType);
                    if (leftPriority != rightPriority) {
                        return leftPriority < rightPriority;
                    }
                    if (left.eventTs != right.eventTs) {
                        return left.eventTs < right.eventTs;
                    }
                    return left.id < right.id;
                });
                if (rows.size() > claimLimit) {
                    rows.resize(claimLimit);
                }
                std::size_t keepCount = 0;
                for (; keepCount < rows.size(); ++keepCount) {
                    const auto rowBytes = rows[keepCount].topic.size() + rows[keepCount].payload.size();
                    if (maxBytes > 0 && stats.count + keepCount > 0 &&
                        stats.bytes + claimedBytes + rowBytes > maxBytes) {
                        break;
                    }
                    claimedBytes += rowBytes;
                }
                rows.resize(keepCount);

                if (!rows.empty()) {
                    std::ostringstream claimSql;
                    claimSql << "UPDATE mqtt_event_outbox SET claim_token=?,claim_until=? "
                                "WHERE target_id=? AND sent=0 "
                                "AND (claim_until IS NULL OR claim_until<=?) AND id IN (";
                    appendPlaceholders(claimSql, rows.size());
                    claimSql << ");";
                    const auto claimPrepareRc = prepareWithRetry(db, claimSql.str().c_str(), &claimStmt);
                    if (claimPrepareRc != kSqliteOk) {
                        throwSqliteResult(db, claimPrepareRc);
                    }
                    if (g_bind_text(claimStmt, 1, claimToken.c_str(), -1, nullptr) != kSqliteOk ||
                        g_bind_int64(claimStmt, 2,
                            static_cast<long long>(claimedAt + kReplayClaimLeaseMs)) != kSqliteOk ||
                        g_bind_text(claimStmt, 3, normalizedTarget.c_str(), -1, nullptr) != kSqliteOk ||
                        g_bind_int64(claimStmt, 4, static_cast<long long>(claimedAt)) != kSqliteOk) {
                        throw std::runtime_error(sqliteError(db));
                    }
                    for (std::size_t index = 0; index < rows.size(); ++index) {
                        if (g_bind_int64(claimStmt, static_cast<int>(index + 5),
                                static_cast<long long>(rows[index].id)) != kSqliteOk) {
                            throw std::runtime_error(sqliteError(db));
                        }
                    }
                    const auto claimRc = stepWithRetry(claimStmt);
                    if (claimRc != kSqliteDone) {
                        throwSqliteResult(db, claimRc);
                    }
                    if (g_changes(db) != static_cast<int>(rows.size())) {
                        throw std::runtime_error("mqtt event replay batch lost candidates before claim");
                    }
                    g_finalize(claimStmt);
                    claimStmt = nullptr;
                }
                execOnceOrThrow(db, "COMMIT;");
                claimBusyTimeout.restore();
            } catch (...) {
                if (typeStmt != nullptr) {
                    g_finalize(typeStmt);
                }
                if (selectStmt != nullptr) {
                    g_finalize(selectStmt);
                }
                if (claimStmt != nullptr) {
                    g_finalize(claimStmt);
                }
                rollbackAfterFailure();
                throw;
            }
        });
        if (rows.empty()) {
            break;
        }

        std::size_t sentCount = 0;
        std::exception_ptr sendError;
        if (batchSend) {
            while (sentCount < rows.size()) {
                const auto publishCount = std::min(
                    kReplayPublishBatchSize,
                    rows.size() - sentCount
                );
                std::vector<ReplayMessage> messages;
                messages.reserve(publishCount);
                for (std::size_t index = 0; index < publishCount; ++index) {
                    const auto& row = rows[sentCount + index];
                    messages.push_back(ReplayMessage{row.topic, row.payload});
                }
                try {
                    batchSend(messages);
                    sentCount += publishCount;
                } catch (...) {
                    sendError = std::current_exception();
                    break;
                }
            }
        } else {
            for (; sentCount < rows.size(); ++sentCount) {
                try {
                    send(rows[sentCount].topic, rows[sentCount].payload);
                } catch (...) {
                    sendError = std::current_exception();
                    break;
                }
            }
        }

        retryReplayTransaction("mqtt event replay acknowledgement remained busy", [&]() {
            (void)checkedDatabase();
            sqlite3_stmt* finishStmt = nullptr;
            BusyTimeoutScope finishBusyTimeout(db, kReplayBusyTimeoutMs, kSqliteBusyTimeoutMs, &databasePoisoned_);
            try {
                execOnceOrThrow(db, "BEGIN IMMEDIATE;");
                if (sentCount > 0) {
                    std::ostringstream acknowledgeSql;
                    acknowledgeSql << "UPDATE mqtt_event_outbox SET sent=1,sent_at=?,"
                                      "claim_token=NULL,claim_until=NULL "
                                      "WHERE sent=0 AND claim_token=? AND id IN (";
                    appendPlaceholders(acknowledgeSql, sentCount);
                    acknowledgeSql << ");";
                    const auto acknowledgePrepareRc = prepareWithRetry(
                        db, acknowledgeSql.str().c_str(), &finishStmt
                    );
                    if (acknowledgePrepareRc != kSqliteOk) {
                        throwSqliteResult(db, acknowledgePrepareRc);
                    }
                    if (g_bind_int64(finishStmt, 1,
                            static_cast<long long>(currentTimeMs())) != kSqliteOk ||
                        g_bind_text(finishStmt, 2, claimToken.c_str(), -1, nullptr) != kSqliteOk) {
                        throw std::runtime_error(sqliteError(db));
                    }
                    for (std::size_t index = 0; index < sentCount; ++index) {
                        if (g_bind_int64(finishStmt, static_cast<int>(index + 3),
                                static_cast<long long>(rows[index].id)) != kSqliteOk) {
                            throw std::runtime_error(sqliteError(db));
                        }
                    }
                    const auto acknowledgeRc = stepWithRetry(finishStmt);
                    if (acknowledgeRc != kSqliteDone) {
                        throwSqliteResult(db, acknowledgeRc);
                    }
                    if (g_changes(db) != static_cast<int>(sentCount)) {
                        throw std::runtime_error(
                            "mqtt event replay batch lost claims before acknowledgement"
                        );
                    }
                    g_finalize(finishStmt);
                    finishStmt = nullptr;
                }
                if (sentCount < rows.size()) {
                    const char* releaseSql =
                        "UPDATE mqtt_event_outbox SET claim_token=NULL,claim_until=NULL "
                        "WHERE sent=0 AND claim_token=?;";
                    const auto releasePrepareRc = prepareWithRetry(db, releaseSql, &finishStmt);
                    if (releasePrepareRc != kSqliteOk) {
                        throwSqliteResult(db, releasePrepareRc);
                    }
                    if (g_bind_text(finishStmt, 1, claimToken.c_str(), -1, nullptr) != kSqliteOk) {
                        throw std::runtime_error(sqliteError(db));
                    }
                    const auto releaseRc = stepWithRetry(finishStmt);
                    if (releaseRc != kSqliteDone) {
                        throwSqliteResult(db, releaseRc);
                    }
                    if (g_changes(db) != static_cast<int>(rows.size() - sentCount)) {
                        throw std::runtime_error("mqtt event replay batch lost claims before release");
                    }
                    g_finalize(finishStmt);
                    finishStmt = nullptr;
                }
                execOnceOrThrow(db, "COMMIT;");
                finishBusyTimeout.restore();
            } catch (...) {
                if (finishStmt != nullptr) {
                    g_finalize(finishStmt);
                }
                rollbackAfterFailure();
                throw;
            }
        });

        for (std::size_t index = 0; index < sentCount; ++index) {
            const auto& row = rows[index];
            stats.count += 1;
            stats.bytes += row.topic.size() + row.payload.size();
            if (row.eventType == "alarm") {
                stats.alarmCount += 1;
            } else if (row.eventType == "change") {
                stats.changeCount += 1;
            } else {
                stats.otherCount += 1;
            }
        }
        if (sendError) {
            std::rethrow_exception(sendError);
        }
        if (stats.count < replayLimit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kReplayFairnessPauseMs));
        }
    }
    return stats;
}

void MqttEventOutbox::markClaimSent(
    std::int64_t id,
    std::int64_t sentAt,
    const std::string& targetId,
    const EventTypeFilter& eventTypes,
    const std::string& claimToken
) {
    auto* db = static_cast<sqlite3*>(checkedDatabase());
    const auto normalizedFilter = normalizeEventTypeFilter(eventTypes);
    std::ostringstream sql;
    sql << "UPDATE mqtt_event_outbox SET sent=1, sent_at=?, claim_token=NULL, claim_until=NULL "
           "WHERE sent=0 AND id=? AND claim_token=? AND target_id=?";
    appendEventTypeFilterSql(sql, normalizedFilter);
    sql << ";";

    sqlite3_stmt* stmt = nullptr;
    if (prepareWithRetry(db, sql.str().c_str(), &stmt) != kSqliteOk) {
        throw std::runtime_error(sqliteError(db));
    }
    auto bindIndex = 1;
    if (g_bind_int64(stmt, bindIndex++, static_cast<long long>(sentAt)) != kSqliteOk) {
        g_finalize(stmt);
        throw std::runtime_error(sqliteError(db));
    }
    if (g_bind_int64(stmt, bindIndex++, static_cast<long long>(id)) != kSqliteOk ||
        g_bind_text(stmt, bindIndex++, claimToken.c_str(), -1, nullptr) != kSqliteOk) {
        g_finalize(stmt);
        throw std::runtime_error(sqliteError(db));
    }
    const auto normalizedTarget = normalizedTargetId(targetId);
    if (g_bind_text(stmt, bindIndex++, normalizedTarget.c_str(), -1, nullptr) != kSqliteOk ||
        bindEventTypeFilter(stmt, bindIndex, normalizedFilter) < 0 ||
        stepWithRetry(stmt) != kSqliteDone) {
        g_finalize(stmt);
        throw std::runtime_error(sqliteError(db));
    }
    g_finalize(stmt);
    if (g_changes(db) != 1) {
        throw std::runtime_error("mqtt event replay claim was lost before acknowledgement");
    }
}

void MqttEventOutbox::releaseClaim(std::int64_t id, const std::string& claimToken) {
    auto* db = static_cast<sqlite3*>(checkedDatabase());
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE mqtt_event_outbox SET claim_token=NULL, claim_until=NULL "
        "WHERE id=? AND sent=0 AND claim_token=?;";
    if (prepareWithRetry(db, sql, &stmt) != kSqliteOk ||
        g_bind_int64(stmt, 1, static_cast<long long>(id)) != kSqliteOk ||
        g_bind_text(stmt, 2, claimToken.c_str(), -1, nullptr) != kSqliteOk ||
        stepWithRetry(stmt) != kSqliteDone) {
        if (stmt != nullptr) {
            g_finalize(stmt);
        }
        throw std::runtime_error(sqliteError(db));
    }
    g_finalize(stmt);
}

void MqttEventOutbox::cleanupIfDue(std::int64_t nowMs, MaintenanceClock::time_point now) {
    if (accessMode_ != AccessMode::ReadWrite || now < nextCleanup_) return;
    const auto retentionMs = static_cast<std::int64_t>(retentionDays_) * 86400000;
    const auto cutoff = nowMs < std::numeric_limits<std::int64_t>::min() + retentionMs
        ? std::numeric_limits<std::int64_t>::min() : nowMs - retentionMs;
    auto* db = static_cast<sqlite3*>(checkedDatabase());
    sqlite3_stmt* stmt = nullptr;
    // IPC rows are also immutable dedup identities. Do not reclaim them with Legacy expiry.
    const char* identitySql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name='event_store_identity';";
    const char* sql = "DELETE FROM mqtt_event_outbox WHERE id IN ("
        "SELECT id FROM mqtt_event_outbox INDEXED BY idx_mqtt_event_outbox_expiry_days "
        "WHERE sent=1 AND event_ts < ? "
        "ORDER BY event_ts,id LIMIT 512);";
    try {
        if (g_prepare(db, identitySql, -1, &stmt, nullptr) != kSqliteOk) {
            throw std::runtime_error(sqliteError(db));
        }
        const auto identity = g_step(stmt);
        if (identity != kSqliteRow && identity != kSqliteDone) {
            throw std::runtime_error(sqliteError(db));
        }
        g_finalize(stmt);
        stmt = nullptr;
        if (identity == kSqliteRow) {
            nextCleanup_ = std::max(now, MaintenanceClock::now()) + std::chrono::hours(cleanupIntervalHours_);
            return;
        }
        if (g_prepare(db, sql, -1, &stmt, nullptr) != kSqliteOk ||
            g_bind_int64(stmt, 1, cutoff) != kSqliteOk || g_step(stmt) != kSqliteDone) {
            throw std::runtime_error(sqliteError(db));
        }
    } catch (...) {
        if (stmt) g_finalize(stmt);
        nextCleanup_ = std::max(now, MaintenanceClock::now()) + std::chrono::seconds(5);
        throw;
    }
    const auto delay = g_changes(db) == 512 ? std::chrono::seconds(1)
        : std::chrono::duration_cast<std::chrono::seconds>(std::chrono::hours(cleanupIntervalHours_));
    g_finalize(stmt);
    nextCleanup_ = std::max(now, MaintenanceClock::now()) + delay;
}

void MqttEventOutbox::loadLibrary() {
    std::lock_guard<std::mutex> lock(g_library_mutex);
    if (libraryHandle_ != nullptr) return;
    if (g_library_handle && libraryPath_.empty()) {
        libraryHandle_ = g_library_handle;
        return;
    }
    void* candidate = nullptr;
#ifdef _WIN32
    candidate = LoadLibraryA(libraryPath_.empty() ? "sqlite3.dll" : libraryPath_.c_str());
#else
    candidate = dlopen(libraryPath_.empty() ? "libsqlite3.so.0" : libraryPath_.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!candidate && libraryPath_.empty()) candidate = dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
#endif
    if (!candidate) {
        throw std::runtime_error(libraryPath_.empty() ? "failed to load system sqlite3 library" :
            "failed to load explicit sqlite3 library (fallback disabled): " + libraryPath_);
    }
    const auto releaseCandidate = [candidate] {
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(candidate));
#else
        dlclose(candidate);
#endif
    };
    if (g_library_handle) {
        const bool sameLibrary = candidate == g_library_handle;
        releaseCandidate();
        if (!sameLibrary) throw std::runtime_error("cannot mix SQLite libraries in one Outbox process");
        libraryHandle_ = g_library_handle;
        return;
    }

    // Resolve everything before publishing the process-wide, lifetime-pinned API.
#define OUTBOX_SQLITE_SYMBOLS(X) \
    X(open, sqlite3_open_v2) X(close, sqlite3_close_v2) X(exec, sqlite3_exec) \
    X(prepare, sqlite3_prepare_v2) X(busy_timeout, sqlite3_busy_timeout) \
    X(bind_int, sqlite3_bind_int) X(bind_int64, sqlite3_bind_int64) \
    X(bind_double, sqlite3_bind_double) X(bind_text, sqlite3_bind_text) \
    X(step, sqlite3_step) X(reset, sqlite3_reset) X(clear_bindings, sqlite3_clear_bindings) \
    X(finalize, sqlite3_finalize) X(errmsg, sqlite3_errmsg) X(free, sqlite3_free) \
    X(last_insert_rowid, sqlite3_last_insert_rowid) X(changes, sqlite3_changes) \
    X(column_int64, sqlite3_column_int64) X(column_int, sqlite3_column_int) \
    X(column_double, sqlite3_column_double) X(column_text, sqlite3_column_text) \
    X(extended_errcode, sqlite3_extended_errcode) X(extended_result_codes, sqlite3_extended_result_codes) \
    X(get_autocommit, sqlite3_get_autocommit) X(db_handle, sqlite3_db_handle)
    try {
#define RESOLVE(member, symbol) const auto member = reinterpret_cast<decltype(g_##member)>(loadSymbol(candidate, #symbol));
        OUTBOX_SQLITE_SYMBOLS(RESOLVE)
#undef RESOLVE
        std::string resolvedPath = libraryPath_;
#ifdef _WIN32
        std::vector<char> path(32768);
        const auto length = GetModuleFileNameA(static_cast<HMODULE>(candidate), path.data(),
            static_cast<DWORD>(path.size()));
        if (length > 0 && length < path.size()) resolvedPath.assign(path.data(), length);
#else
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(open), &info) && info.dli_fname) resolvedPath = info.dli_fname;
#endif
        g_library_path = std::move(resolvedPath);
#define PUBLISH(member, symbol) g_##member = member;
        OUTBOX_SQLITE_SYMBOLS(PUBLISH)
#undef PUBLISH
        g_library_handle = candidate;
        libraryHandle_ = candidate;
    } catch (...) {
        releaseCandidate();
        throw;
    }
#undef OUTBOX_SQLITE_SYMBOLS
}

void MqttEventOutbox::openDatabase() {
    sqlite3* db = nullptr;
    const auto flags = accessMode_ == AccessMode::ReadOnly ? kSqliteOpenReadOnly :
        kSqliteOpenReadWrite | kSqliteOpenCreate;
    const auto rc = g_open(dbPath_.c_str(), &db, flags | kSqliteOpenPrivateCache, nullptr);
    if (rc != kSqliteOk) {
        const auto failure = sqliteFailure(db, rc, "open");
        if (db) g_close(db);
        throw failure;
    }
    try {
        const auto extendedRc = g_extended_result_codes(db, 1);
        if (extendedRc != kSqliteOk) throwSqliteResult(db, extendedRc, "configure.extended_codes");
        setBusyTimeoutOrThrow(db, kSqliteBusyTimeoutMs);
    } catch (...) {
        g_close(db);
        throw;
    }
    databaseHandle_ = db;
    databasePoisoned_ = false;
}

void MqttEventOutbox::ensureSchema() {
    auto* db = static_cast<sqlite3*>(checkedDatabase());
    BusyTimeoutScope schemaTimeout(db, kSqliteSchemaBusyTimeoutMs, kSqliteBusyTimeoutMs, &databasePoisoned_);
    try {
        const bool wal = storageProfile_ == StorageProfile::WalNormal ||
            storageProfile_ == StorageProfile::WalFull;
        const bool full = storageProfile_ == StorageProfile::DeleteFull ||
            storageProfile_ == StorageProfile::WalFull;
        execOnceOrThrow(db, wal ? "PRAGMA journal_mode=WAL;" : "PRAGMA journal_mode=DELETE;");
        execOnceOrThrow(db, full ? "PRAGMA synchronous=FULL;" : "PRAGMA synchronous=NORMAL;");
        const auto actual = storageSettings();
        if (actual.journalMode != (wal ? "wal" : "delete") || actual.synchronous != (full ? 2 : 1)) {
            throw std::runtime_error("mqtt event outbox storage profile was not applied");
        }
        execOnceOrThrow(db, "BEGIN IMMEDIATE;");
        execOnceOrThrow(
            db,
            "CREATE TABLE IF NOT EXISTS mqtt_event_outbox ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "event_id TEXT,"
            "target_id TEXT NOT NULL DEFAULT 'main',"
            "claim_token TEXT,"
            "claim_until INTEGER,"
            "event_type TEXT NOT NULL,"
            "topic TEXT NOT NULL,"
            "payload TEXT NOT NULL,"
            "event_ts INTEGER NOT NULL,"
            "event_month TEXT NOT NULL,"
            "created_at INTEGER NOT NULL,"
            "sent INTEGER NOT NULL DEFAULT 0,"
            "sent_at INTEGER,"
            "retry_count INTEGER NOT NULL DEFAULT 0,"
            "last_error TEXT"
            ");"
        );
        if (!tableHasColumn(db, "mqtt_event_outbox", "event_id")) {
            execOnceOrThrow(db, "ALTER TABLE mqtt_event_outbox ADD COLUMN event_id TEXT;");
        }
        const bool targetIdWasAdded = !tableHasColumn(db, "mqtt_event_outbox", "target_id");
        if (targetIdWasAdded) {
            execOnceOrThrow(
                db,
                "ALTER TABLE mqtt_event_outbox ADD COLUMN target_id TEXT NOT NULL DEFAULT 'main';"
            );
        }
        if (!tableHasColumn(db, "mqtt_event_outbox", "claim_token")) {
            execOnceOrThrow(db, "ALTER TABLE mqtt_event_outbox ADD COLUMN claim_token TEXT;");
        }
        if (!tableHasColumn(db, "mqtt_event_outbox", "claim_until")) {
            execOnceOrThrow(db, "ALTER TABLE mqtt_event_outbox ADD COLUMN claim_until INTEGER;");
        }
        const bool targetMigrationComplete = tableHasIndex(
            db, "mqtt_event_outbox", "idx_mqtt_event_outbox_event_target"
        );
        if (!targetIdWasAdded && !targetMigrationComplete) {
            execOnceOrThrow(
                db,
                "UPDATE mqtt_event_outbox SET target_id='main' "
                "WHERE target_id IS NULL OR target_id='';"
            );
        }
        execOnceOrThrow(
            db,
            "CREATE UNIQUE INDEX IF NOT EXISTS idx_mqtt_event_outbox_event_target "
            "ON mqtt_event_outbox(event_id, target_id) "
            "WHERE event_id IS NOT NULL AND event_id <> '';"
        );
        execOnceOrThrow(
            db,
            "CREATE INDEX IF NOT EXISTS idx_mqtt_event_outbox_pending "
            "ON mqtt_event_outbox(sent, event_ts, id);"
        );
        execOnceOrThrow(
            db,
            "CREATE INDEX IF NOT EXISTS idx_mqtt_event_outbox_target_pending "
            "ON mqtt_event_outbox(target_id, sent, event_type, event_ts, id);"
        );
        execOnceOrThrow(
            db,
            "CREATE INDEX IF NOT EXISTS idx_mqtt_event_outbox_claim "
            "ON mqtt_event_outbox(target_id, sent, claim_until, event_type, event_ts, id);"
        );
        execOnceOrThrow(
            db,
            "CREATE INDEX IF NOT EXISTS idx_mqtt_event_outbox_cleanup "
            "ON mqtt_event_outbox(sent, event_month);"
        );
        execOnceOrThrow(db, "CREATE INDEX IF NOT EXISTS idx_mqtt_event_outbox_expiry_days "
            "ON mqtt_event_outbox(sent,event_ts,id);");
        execOnceOrThrow(
            db,
            "CREATE TABLE IF NOT EXISTS mqtt_event_state ("
            "state_key TEXT PRIMARY KEY,"
            "event_type TEXT NOT NULL,"
            "point_index INTEGER NOT NULL,"
            "alarm_type TEXT NOT NULL,"
            "active INTEGER NOT NULL,"
            "value REAL NOT NULL,"
            "quality INTEGER NOT NULL,"
            "source_ts INTEGER NOT NULL,"
            "lifecycle TEXT NOT NULL,"
            "updated_at INTEGER NOT NULL"
            ");"
        );
        execOnceOrThrow(
            db,
            "CREATE TABLE IF NOT EXISTS mqtt_event_outbox_stats ("
            "target_id TEXT NOT NULL,"
            "event_type TEXT NOT NULL,"
            "pending_count INTEGER NOT NULL DEFAULT 0 CHECK (pending_count >= 0),"
            "pending_bytes INTEGER NOT NULL DEFAULT 0 CHECK (pending_bytes >= 0),"
            "updated_at INTEGER NOT NULL,"
            "PRIMARY KEY(target_id, event_type)"
            ") WITHOUT ROWID;"
        );
        execOnceOrThrow(
            db,
            "CREATE TABLE IF NOT EXISTS mqtt_event_outbox_meta ("
            "key TEXT PRIMARY KEY,"
            "value TEXT NOT NULL"
            ");"
        );
        execOnceOrThrow(
            db,
            "INSERT OR IGNORE INTO mqtt_event_outbox_meta(key,value) "
            "SELECT 'stats_migrate_cap', CAST(COALESCE(MAX(id),0) AS TEXT) "
            "FROM mqtt_event_outbox;"
        );
        execOnceOrThrow(
            db,
            "INSERT OR IGNORE INTO mqtt_event_outbox_meta(key,value) "
            "VALUES('stats_migrate_hw','0');"
        );
        execOnceOrThrow(
            db,
            "INSERT OR IGNORE INTO mqtt_event_outbox_meta(key,value) "
            "SELECT 'stats_migrate_done', "
            "CASE WHEN CAST(value AS INTEGER)=0 THEN '1' ELSE '0' END "
            "FROM mqtt_event_outbox_meta WHERE key='stats_migrate_cap';"
        );

        execOnceOrThrow(db, "DROP TRIGGER IF EXISTS mqtt_event_outbox_stats_insert;");
        execOnceOrThrow(db, "DROP TRIGGER IF EXISTS mqtt_event_outbox_stats_sent;");
        execOnceOrThrow(db, "DROP TRIGGER IF EXISTS mqtt_event_outbox_stats_delete;");
        execOnceOrThrow(
            db,
            "CREATE TRIGGER mqtt_event_outbox_stats_insert "
            "AFTER INSERT ON mqtt_event_outbox "
            "WHEN NEW.sent=0 AND ("
            "COALESCE((SELECT value FROM mqtt_event_outbox_meta "
            "WHERE key='stats_migrate_done'),'0')='1' OR "
            "NEW.id>COALESCE(CAST((SELECT value FROM mqtt_event_outbox_meta "
            "WHERE key='stats_migrate_cap') AS INTEGER),0)) "
            "BEGIN "
            "INSERT OR IGNORE INTO mqtt_event_outbox_stats("
            "target_id,event_type,pending_count,pending_bytes,updated_at) "
            "VALUES(NEW.target_id,NEW.event_type,0,0,NEW.created_at); "
            "UPDATE mqtt_event_outbox_stats SET "
            "pending_count=pending_count+1,"
            "pending_bytes=pending_bytes+length(NEW.topic)+length(NEW.payload),"
            "updated_at=NEW.created_at "
            "WHERE target_id=NEW.target_id AND event_type=NEW.event_type; "
            "END;"
        );
        execOnceOrThrow(
            db,
            "CREATE TRIGGER mqtt_event_outbox_stats_sent "
            "AFTER UPDATE OF sent ON mqtt_event_outbox "
            "WHEN OLD.sent=0 AND NEW.sent=1 AND ("
            "COALESCE((SELECT value FROM mqtt_event_outbox_meta "
            "WHERE key='stats_migrate_done'),'0')='1' OR "
            "NEW.id>COALESCE(CAST((SELECT value FROM mqtt_event_outbox_meta "
            "WHERE key='stats_migrate_cap') AS INTEGER),0) OR "
            "NEW.id<=COALESCE(CAST((SELECT value FROM mqtt_event_outbox_meta "
            "WHERE key='stats_migrate_hw') AS INTEGER),0)) "
            "BEGIN "
            "UPDATE mqtt_event_outbox_stats SET "
            "pending_count=pending_count-1,"
            "pending_bytes=pending_bytes-length(OLD.topic)-length(OLD.payload),"
            "updated_at=COALESCE(NEW.sent_at,NEW.created_at) "
            "WHERE target_id=OLD.target_id AND event_type=OLD.event_type; "
            "END;"
        );
        execOnceOrThrow(
            db,
            "CREATE TRIGGER mqtt_event_outbox_stats_delete "
            "AFTER DELETE ON mqtt_event_outbox "
            "WHEN OLD.sent=0 AND ("
            "COALESCE((SELECT value FROM mqtt_event_outbox_meta "
            "WHERE key='stats_migrate_done'),'0')='1' OR "
            "OLD.id>COALESCE(CAST((SELECT value FROM mqtt_event_outbox_meta "
            "WHERE key='stats_migrate_cap') AS INTEGER),0) OR "
            "OLD.id<=COALESCE(CAST((SELECT value FROM mqtt_event_outbox_meta "
            "WHERE key='stats_migrate_hw') AS INTEGER),0)) "
            "BEGIN "
            "UPDATE mqtt_event_outbox_stats SET "
            "pending_count=pending_count-1,"
            "pending_bytes=pending_bytes-length(OLD.topic)-length(OLD.payload),"
            "updated_at=OLD.created_at "
            "WHERE target_id=OLD.target_id AND event_type=OLD.event_type; "
            "END;"
        );
        execOnceOrThrow(db, "COMMIT;");
    } catch (...) {
        rollbackAfterFailure();
        throw;
    }
    schemaTimeout.restore();
}

MqttEventOutbox::StorageSettings MqttEventOutbox::storageSettings() const {
    auto* db = static_cast<sqlite3*>(checkedDatabase());
    const auto scalar = [db](const char* sql) {
        sqlite3_stmt* stmt = nullptr;
        const auto prepared = prepareWithRetry(db, sql, &stmt);
        if (prepared != kSqliteOk) {
            if (stmt != nullptr) g_finalize(stmt);
            throwSqliteResult(db, prepared);
        }
        const auto rc = stepWithRetry(stmt);
        if (rc != kSqliteRow) {
            g_finalize(stmt);
            throwSqliteResult(db, rc);
        }
        const auto value = columnText(stmt, 0);
        g_finalize(stmt);
        return value;
    };
    StorageSettings settings;
    settings.sqliteLibraryPath = g_library_path;
    settings.sqliteVersion = scalar("SELECT sqlite_version();");
    settings.journalMode = scalar("PRAGMA journal_mode;");
    settings.synchronous = std::stoi(scalar("PRAGMA synchronous;"));
    settings.busyTimeoutMs = std::stoi(scalar("PRAGMA busy_timeout;"));
    settings.walAutoCheckpointPages = std::stoi(scalar("PRAGMA wal_autocheckpoint;"));
    return settings;
}

void MqttEventOutbox::resumeStatsMigration() {
    struct Aggregate {
        std::string targetId;
        std::string eventType;
        std::int64_t count = 0;
        std::int64_t bytes = 0;
    };

    auto* db = static_cast<sqlite3*>(checkedDatabase());
    std::uint64_t databaseBytes = 0;
    std::ifstream databaseFile(dbPath_, std::ios::binary | std::ios::ate);
    if (databaseFile) {
        const auto end = databaseFile.tellg();
        if (end > 0) {
            databaseBytes = static_cast<std::uint64_t>(end);
        }
    }
    int migrationBatchRows = 4096;
    bool migrationBatchRowsEstimated = false;
    BusyTimeoutScope migrationTimeout(
        db,
        kSqliteStatsMigrationBusyTimeoutMs,
        kSqliteBusyTimeoutMs,
        &databasePoisoned_
    );
    bool complete = false;
    while (!complete) {
        sqlite3_stmt* metaStmt = nullptr;
        sqlite3_stmt* upperStmt = nullptr;
        sqlite3_stmt* aggregateStmt = nullptr;
        sqlite3_stmt* insertStmt = nullptr;
        sqlite3_stmt* updateStmt = nullptr;
        try {
            execOrThrow(db, "BEGIN IMMEDIATE;");

            const char* metaSql =
                "SELECT "
                "COALESCE(MAX(CASE WHEN key='stats_migrate_cap' THEN CAST(value AS INTEGER) END),0),"
                "COALESCE(MAX(CASE WHEN key='stats_migrate_hw' THEN CAST(value AS INTEGER) END),0),"
                "COALESCE(MAX(CASE WHEN key='stats_migrate_done' THEN CAST(value AS INTEGER) END),0) "
                "FROM mqtt_event_outbox_meta;";
            if (prepareWithRetry(db, metaSql, &metaStmt) != kSqliteOk ||
                stepWithRetry(metaStmt) != kSqliteRow) {
                throw std::runtime_error(sqliteError(db));
            }
            const auto cap = static_cast<std::int64_t>(g_column_int64(metaStmt, 0));
            const auto highWater = static_cast<std::int64_t>(g_column_int64(metaStmt, 1));
            complete = g_column_int64(metaStmt, 2) != 0;
            g_finalize(metaStmt);
            metaStmt = nullptr;
            if (complete) {
                execOrThrow(db, "COMMIT;");
                break;
            }
            if (!migrationBatchRowsEstimated && cap > 0 && databaseBytes > 0) {
                const auto estimatedRowBytes = std::max<std::uint64_t>(
                    1,
                    databaseBytes / static_cast<std::uint64_t>(cap)
                );
                const auto estimatedRows = kStatsMigrationTargetBatchBytes / estimatedRowBytes;
                migrationBatchRows = static_cast<int>(std::max<std::uint64_t>(
                    kStatsMigrationMinBatchRows,
                    std::min<std::uint64_t>(kStatsMigrationMaxBatchRows, estimatedRows)
                ));
                migrationBatchRowsEstimated = true;
            }

            const char* upperSql =
                "SELECT COALESCE(MAX(id),0) FROM ("
                "SELECT id FROM mqtt_event_outbox WHERE id>? AND id<=? "
                "ORDER BY id LIMIT ?);";
            if (prepareWithRetry(db, upperSql, &upperStmt) != kSqliteOk ||
                g_bind_int64(upperStmt, 1, static_cast<long long>(highWater)) != kSqliteOk ||
                g_bind_int64(upperStmt, 2, static_cast<long long>(cap)) != kSqliteOk ||
                g_bind_int(upperStmt, 3, migrationBatchRows) != kSqliteOk ||
                stepWithRetry(upperStmt) != kSqliteRow) {
                throw std::runtime_error(sqliteError(db));
            }
            auto upper = static_cast<std::int64_t>(g_column_int64(upperStmt, 0));
            g_finalize(upperStmt);
            upperStmt = nullptr;
            if (upper == 0) {
                upper = cap;
            }

            std::vector<Aggregate> aggregates;
            const char* aggregateSql =
                "SELECT target_id,event_type,COUNT(*),"
                "COALESCE(SUM(length(topic)+length(payload)),0) "
                "FROM mqtt_event_outbox WHERE id>? AND id<=? AND sent=0 "
                "GROUP BY target_id,event_type;";
            if (prepareWithRetry(db, aggregateSql, &aggregateStmt) != kSqliteOk ||
                g_bind_int64(aggregateStmt, 1, static_cast<long long>(highWater)) != kSqliteOk ||
                g_bind_int64(aggregateStmt, 2, static_cast<long long>(upper)) != kSqliteOk) {
                throw std::runtime_error(sqliteError(db));
            }
            for (;;) {
                const auto rc = stepWithRetry(aggregateStmt);
                if (rc == kSqliteDone) {
                    break;
                }
                if (rc != kSqliteRow) {
                    throw std::runtime_error(sqliteError(db));
                }
                aggregates.push_back(Aggregate{
                    columnText(aggregateStmt, 0),
                    columnText(aggregateStmt, 1),
                    static_cast<std::int64_t>(g_column_int64(aggregateStmt, 2)),
                    static_cast<std::int64_t>(g_column_int64(aggregateStmt, 3))
                });
            }
            g_finalize(aggregateStmt);
            aggregateStmt = nullptr;

            const auto updatedAt = currentTimeMs();
            const char* insertSql =
                "INSERT OR IGNORE INTO mqtt_event_outbox_stats("
                "target_id,event_type,pending_count,pending_bytes,updated_at) "
                "VALUES(?,?,0,0,?);";
            const char* updateSql =
                "UPDATE mqtt_event_outbox_stats SET "
                "pending_count=pending_count+?,pending_bytes=pending_bytes+?,updated_at=? "
                "WHERE target_id=? AND event_type=?;";
            if (!aggregates.empty() &&
                (prepareWithRetry(db, insertSql, &insertStmt) != kSqliteOk ||
                 prepareWithRetry(db, updateSql, &updateStmt) != kSqliteOk)) {
                throw std::runtime_error(sqliteError(db));
            }
            for (const auto& aggregate : aggregates) {
                if (g_bind_text(insertStmt, 1, aggregate.targetId.c_str(), -1, nullptr) != kSqliteOk ||
                    g_bind_text(insertStmt, 2, aggregate.eventType.c_str(), -1, nullptr) != kSqliteOk ||
                    g_bind_int64(insertStmt, 3, static_cast<long long>(updatedAt)) != kSqliteOk ||
                    stepWithRetry(insertStmt) != kSqliteDone ||
                    g_reset(insertStmt) != kSqliteOk ||
                    g_clear_bindings(insertStmt) != kSqliteOk) {
                    throw std::runtime_error(sqliteError(db));
                }
                if (g_bind_int64(updateStmt, 1, static_cast<long long>(aggregate.count)) != kSqliteOk ||
                    g_bind_int64(updateStmt, 2, static_cast<long long>(aggregate.bytes)) != kSqliteOk ||
                    g_bind_int64(updateStmt, 3, static_cast<long long>(updatedAt)) != kSqliteOk ||
                    g_bind_text(updateStmt, 4, aggregate.targetId.c_str(), -1, nullptr) != kSqliteOk ||
                    g_bind_text(updateStmt, 5, aggregate.eventType.c_str(), -1, nullptr) != kSqliteOk ||
                    stepWithRetry(updateStmt) != kSqliteDone ||
                    g_reset(updateStmt) != kSqliteOk ||
                    g_clear_bindings(updateStmt) != kSqliteOk) {
                    throw std::runtime_error(sqliteError(db));
                }
            }
            if (insertStmt != nullptr) {
                g_finalize(insertStmt);
                insertStmt = nullptr;
            }
            if (updateStmt != nullptr) {
                g_finalize(updateStmt);
                updateStmt = nullptr;
            }

            std::ostringstream advanceSql;
            advanceSql << "UPDATE mqtt_event_outbox_meta SET value='" << upper
                       << "' WHERE key='stats_migrate_hw';";
            execOrThrow(db, advanceSql.str().c_str());
            complete = upper >= cap;
            if (complete) {
                execOrThrow(
                    db,
                    "UPDATE mqtt_event_outbox_meta SET value='1' "
                    "WHERE key='stats_migrate_done';"
                );
            }
            execOrThrow(db, "COMMIT;");
        } catch (...) {
            if (metaStmt != nullptr) g_finalize(metaStmt);
            if (upperStmt != nullptr) g_finalize(upperStmt);
            if (aggregateStmt != nullptr) g_finalize(aggregateStmt);
            if (insertStmt != nullptr) g_finalize(insertStmt);
            if (updateStmt != nullptr) g_finalize(updateStmt);
            rollbackAfterFailure();
            throw;
        }
        if (!complete) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kStatsMigrationPauseMs));
        }
    }
    migrationTimeout.restore();
}

std::size_t MqttEventOutbox::pendingBytes(const std::string& targetId) {
    auto* db = static_cast<sqlite3*>(checkedDatabase());
    sqlite3_stmt* stmt = nullptr;
    const char* sql = targetId.empty()
        ? "SELECT COALESCE(SUM(pending_bytes),0) FROM mqtt_event_outbox_stats;"
        : "SELECT COALESCE(SUM(pending_bytes),0) "
          "FROM mqtt_event_outbox_stats WHERE target_id=?;";
    if (prepareWithRetry(db, sql, &stmt) != kSqliteOk) {
        throw std::runtime_error(sqliteError(db));
    }
    if (!targetId.empty() &&
        g_bind_text(stmt, 1, targetId.c_str(), -1, nullptr) != kSqliteOk) {
        g_finalize(stmt);
        throw std::runtime_error(sqliteError(db));
    }
    std::size_t bytes = 0;
    const auto rc = stepWithRetry(stmt);
    if (rc == kSqliteRow) {
        bytes = static_cast<std::size_t>(std::max<long long>(0, g_column_int64(stmt, 0)));
    } else if (rc != kSqliteDone) {
        g_finalize(stmt);
        throw std::runtime_error(sqliteError(db));
    }
    g_finalize(stmt);
    return bytes;
}

std::size_t MqttEventOutbox::prunePendingRows(
    const std::string& targetId,
    std::size_t targetBytes,
    const std::vector<std::int64_t>& protectedIds
) {
    auto* db = static_cast<sqlite3*>(checkedDatabase());
    const auto currentBytes = pendingBytes(targetId);
    if (currentBytes <= targetBytes) {
        return 0;
    }

    const auto bytesToRemove = currentBytes - targetBytes;
    std::vector<std::int64_t> ids;
    std::size_t selectedBytes = 0;
    sqlite3_stmt* stmt = nullptr;
    const char* selectSql = targetId.empty()
        ? "SELECT id, length(topic) + length(payload) FROM mqtt_event_outbox "
          "WHERE sent=0 AND event_type='change' AND (claim_until IS NULL OR claim_until<=?) "
          "ORDER BY event_ts ASC, id ASC;"
        : "SELECT id, length(topic) + length(payload) FROM mqtt_event_outbox "
          "WHERE sent=0 AND target_id=? AND event_type='change' "
          "AND (claim_until IS NULL OR claim_until<=?) "
          "ORDER BY event_ts ASC, id ASC;";
    if (prepareWithRetry(db, selectSql, &stmt) != kSqliteOk) {
        throw std::runtime_error(sqliteError(db));
    }
    const auto nowMs = currentTimeMs();
    int bindIndex = 1;
    if (!targetId.empty() &&
        g_bind_text(stmt, bindIndex++, targetId.c_str(), -1, nullptr) != kSqliteOk) {
        g_finalize(stmt);
        throw std::runtime_error(sqliteError(db));
    }
    if (g_bind_int64(stmt, bindIndex, static_cast<long long>(nowMs)) != kSqliteOk) {
        g_finalize(stmt);
        throw std::runtime_error(sqliteError(db));
    }
    for (;;) {
        const auto rc = stepWithRetry(stmt);
        if (rc == kSqliteDone) {
            break;
        }
        if (rc != kSqliteRow) {
            g_finalize(stmt);
            throw std::runtime_error(sqliteError(db));
        }
        const auto id = static_cast<std::int64_t>(g_column_int64(stmt, 0));
        if (std::find(protectedIds.begin(), protectedIds.end(), id) != protectedIds.end()) {
            continue;
        }
        ids.push_back(id);
        selectedBytes += static_cast<std::size_t>(std::max<long long>(0, g_column_int64(stmt, 1)));
        if (selectedBytes >= bytesToRemove) {
            break;
        }
    }
    g_finalize(stmt);
    if (ids.empty()) {
        return 0;
    }

    std::size_t removed = 0;
    for (const auto id : ids) {
        stmt = nullptr;
        const char* deleteSql =
            "DELETE FROM mqtt_event_outbox WHERE id=? AND sent=0 "
            "AND (claim_until IS NULL OR claim_until<=?);";
        if (prepareWithRetry(db, deleteSql, &stmt) != kSqliteOk) {
            throw std::runtime_error(sqliteError(db));
        }
        if (g_bind_int64(stmt, 1, static_cast<long long>(id)) != kSqliteOk ||
            g_bind_int64(stmt, 2, static_cast<long long>(nowMs)) != kSqliteOk ||
            stepWithRetry(stmt) != kSqliteDone) {
            g_finalize(stmt);
            throw std::runtime_error(sqliteError(db));
        }
        g_finalize(stmt);
        ++removed;
    }
    return removed;
}

void MqttEventOutbox::enforceDiskLimit(
    const std::vector<std::int64_t>& protectedIds,
    const std::string& targetId
) {
    if (maxDiskBytes_ == 0) {
        return;
    }
    const auto targetBytes = maxDiskBytes_ > 4096 ? maxDiskBytes_ - 4096 : maxDiskBytes_;
    (void)prunePendingRows(targetId, targetBytes, protectedIds);
    if (pendingBytes(targetId) > targetBytes) {
        throw TargetCapacityError(
            "mqtt event outbox disk limit exceeded" +
            (targetId.empty() ? std::string() : std::string(" for target ") + targetId) +
            "; pending alarm events are protected"
        );
    }
}

void MqttEventOutbox::closeDatabase() {
    if (databaseHandle_ != nullptr) {
        g_close(static_cast<sqlite3*>(databaseHandle_));
        databaseHandle_ = nullptr;
    }
}

void MqttEventOutbox::unloadLibrary() {
    // One library reference intentionally lives until process exit; other instances
    // must never call function pointers from an unloaded/replaced module.
    libraryHandle_ = nullptr;
}

void* MqttEventOutbox::checkedDatabase() const {
    if (!databaseHandle_ || databasePoisoned_) {
        throw std::runtime_error("mqtt event outbox connection quarantined; recreate Outbox before retry");
    }
    return databaseHandle_;
}

void MqttEventOutbox::rollbackAfterFailure() noexcept {
    auto* db = static_cast<sqlite3*>(databaseHandle_);
    if (!db || g_get_autocommit(db) != 0) return;
    char* error = nullptr;
    const auto rc = g_exec(db, "ROLLBACK;", nullptr, nullptr, &error);
    if (error) g_free(error);
    if (rc != kSqliteOk || g_get_autocommit(db) == 0) databasePoisoned_ = true;
}

std::string MqttEventOutbox::eventMonth(std::int64_t eventTs) const {
    const std::time_t sec = static_cast<std::time_t>(eventTs / 1000);
    std::tm tm {};
#ifdef _WIN32
    gmtime_s(&tm, &sec);
#else
    gmtime_r(&sec, &tm);
#endif
    std::ostringstream out;
    out << (tm.tm_year + 1900) << "-";
    if (tm.tm_mon + 1 < 10) out << "0";
    out << (tm.tm_mon + 1);
    return out.str();
}

namespace {

class StoreStatement {
public:
    StoreStatement(sqlite3* db, const char* sql) : db_(db) {
        const int rc = g_prepare(db, sql, -1, &statement_, nullptr);
        if (rc != kSqliteOk) {
            const auto error = sqliteFailure(db, rc, "store.prepare");
            if (statement_) g_finalize(statement_);
            throw error;
        }
    }
    ~StoreStatement() { g_finalize(statement_); }
    StoreStatement(const StoreStatement&) = delete;
    StoreStatement& operator=(const StoreStatement&) = delete;
    void text(int index, const std::string& value) {
        // Own the bound bytes; callers may bind temporary strings or shorter-lived locals.
        const auto transient = reinterpret_cast<void (*)(void*)>(-1);
        const int rc = g_bind_text(statement_, index, value.data(), static_cast<int>(value.size()), transient);
        if (rc != kSqliteOk) throw sqliteFailure(db_, rc, "store.bind");
    }
    void integer(int index, std::int64_t value) {
        const int rc = g_bind_int64(statement_, index, value);
        if (rc != kSqliteOk) throw sqliteFailure(db_, rc, "store.bind");
    }
    void number(int index, double value) {
        const int rc = g_bind_double(statement_, index, value);
        if (rc != kSqliteOk) throw sqliteFailure(db_, rc, "store.bind");
    }
    bool row() {
        const int rc = g_step(statement_);
        if (rc == kSqliteRow) return true;
        if (rc == kSqliteDone) return false;
        throw sqliteFailure(db_, rc, "store.step");
    }
    void done() {
        if (row()) throw std::logic_error("unexpected event store query result");
    }
    void finish() {
        auto* statement = statement_;
        statement_ = nullptr;
        const int rc = g_finalize(statement);
        if (rc != kSqliteOk) throw sqliteFailure(db_, rc, "store.finalize");
    }
    std::string text(int index) const { return columnText(statement_, index); }
    std::int64_t integer(int index) const { return g_column_int64(statement_, index); }
    double number(int index) const { return g_column_double(statement_, index); }

private:
    sqlite3* db_;
    sqlite3_stmt* statement_ = nullptr;
};

bool storeIdentifier(const std::string& value) {
    return !value.empty() && value.size() <= 96 && std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_' || ch == ':';
    });
}

EventStoreProducer readProducer(sqlite3* db, const std::string& id, bool withRequest = false) {
    StoreStatement stmt(db, withRequest ?
        "SELECT session_id,epoch,sequence,request,receipt FROM event_store_producer WHERE producer_id=?;" :
        "SELECT session_id,epoch,sequence,'',receipt FROM event_store_producer WHERE producer_id=?;");
    stmt.text(1, id);
    EventStoreProducer result;
    result.producerId = id;
    if (stmt.row()) {
        result.sessionId = stmt.text(0);
        result.epoch = stmt.integer(1);
        result.sequence = stmt.integer(2);
        result.request = stmt.text(3);
        result.receipt = stmt.text(4);
    }
    return result;
}

void boundedStoreText(const std::string& value, std::size_t max, bool required) {
    if ((required && value.empty()) || value.size() > max || value.find('\0') != std::string::npos) {
        throw std::invalid_argument("event store text is empty, oversized or contains NUL");
    }
}

std::string localIdentity(const EventStoreLocalEvent& local) {
    std::string result;
    const auto add = [&](const std::string& value) { result += std::to_string(value.size()) + ':' + value; };
    const auto real = [&](double value) {
        // SQLite normalizes signed zero. Reject nonfinite values before this function.
        if (value == 0) value = 0;
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        add(std::to_string(bits));
    };
    const auto& e = local.event;
    add(local.kind); add(e.eventId); add(std::to_string(e.index)); add(std::to_string(e.ts));
    add(e.alarmType); add(e.active ? "1" : "0"); real(e.threshold); real(e.value);
    add(std::to_string(e.quality)); add(e.stale ? "1" : "0"); add(e.persistValue);
    add(e.machineCode); add(e.meterCode); add(e.pointCode);
    add(std::to_string(local.stateVersion)); add(local.configGeneration);
    return result;
}

constexpr const char* journalColumns = "id,kind,event_id,point_index,ts,alarm_type,active,threshold,value,"
    "quality,stale,persist_value,gateway_code,device_code,point_code,state_version,config_generation";

EventStoreJournalRow readJournalRow(StoreStatement& row) {
    EventStoreJournalRow result;
    result.id = row.integer(0);
    auto& l = result.local;
    auto& e = l.event;
    l.kind = row.text(1); e.eventId = row.text(2); e.index = static_cast<std::uint32_t>(row.integer(3));
    e.ts = row.integer(4); e.alarmType = row.text(5); e.active = row.integer(6) != 0;
    e.threshold = row.number(7); e.value = row.number(8); e.quality = static_cast<int>(row.integer(9));
    e.stale = row.integer(10) != 0; e.persistValue = row.text(11); e.machineCode = row.text(12);
    e.meterCode = row.text(13); e.pointCode = row.text(14);
    l.stateVersion = row.integer(15); l.configGeneration = row.text(16);
    return result;
}

void bindAlarm(StoreStatement& row, const AlarmEvent& e, int start = 1) {
    row.text(start++, e.eventId); row.integer(start++, e.index); row.integer(start++, e.ts);
    row.text(start++, e.alarmType); row.integer(start++, e.active); row.number(start++, e.threshold);
    row.number(start++, e.value); row.integer(start++, e.quality); row.integer(start++, e.stale);
    row.text(start++, e.persistValue); row.text(start++, e.machineCode); row.text(start++, e.meterCode);
    row.text(start, e.pointCode);
}

std::string appendIdentity(const EventStoreAppend& request) {
    std::string identity;
    const auto add = [&](const std::string& value) { identity += std::to_string(value.size()) + ':' + value; };
    add(request.request);
    add(request.producerId);
    add(std::to_string(request.epoch));
    add(std::to_string(request.sequence));
    add(std::to_string(request.events.size()));
    for (const auto& event : request.events) {
        add(event.eventId); add(event.targetId); add(event.eventType); add(event.topic); add(event.payload);
        add(std::to_string(event.eventTs));
    }
    add(std::to_string(request.states.size()));
    for (const auto& item : request.states) {
        const auto& state = item.state;
        add(state.stateKey); add(state.eventType); add(std::to_string(state.index)); add(state.alarmType);
        add(state.active ? "1" : "0");
        std::uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(state.value), "unexpected double size");
        std::memcpy(&bits, &state.value, sizeof(bits));
        add(std::to_string(bits)); add(std::to_string(state.quality)); add(std::to_string(state.sourceTs));
        add(state.lifecycle); add(std::to_string(item.version));
    }
    // Preserve receipts generated by pre-journal versions for requests without local rows.
    if (!request.localEvents.empty()) {
        add("localEvents-v1"); add(std::to_string(request.localEvents.size()));
        for (const auto& local : request.localEvents) add(localIdentity(local));
    }
    return identity;
}

}  // namespace

EventPendingStats MqttEventOutbox::readPendingStats(const EventStatsScope& requestedScope) {
    const auto scope = normalizeEventStatsScope(requestedScope);
    switch (storageProfile_) {
    case StorageProfile::DeleteNormal:
    case StorageProfile::DeleteFull:
    case StorageProfile::WalNormal:
    case StorageProfile::WalFull:
        break;
    default:
        throw std::invalid_argument("invalid stats storage profile");
    }
    auto* db = static_cast<sqlite3*>(checkedDatabase());
    std::ostringstream sql;
    // LEFT JOIN preserves the readiness marker even for an explicitly empty scope.
    sql << "SELECT COALESCE(SUM(s.pending_count),0),COALESCE(SUM(s.pending_bytes),0),"
           "COALESCE(MAX(m.value),'') FROM mqtt_event_outbox_meta AS m "
           "LEFT JOIN mqtt_event_outbox_stats AS s ON s.target_id=?";
    if (scope.selection == EventStatsSelection::Only) {
        if (scope.include.empty()) sql << " AND 0";
        else {
            sql << " AND s.event_type IN (";
            appendPlaceholders(sql, scope.include.size());
            sql << ")";
        }
    }
    if (!scope.exclude.empty()) {
        sql << " AND s.event_type NOT IN (";
        appendPlaceholders(sql, scope.exclude.size());
        sql << ")";
    }
    sql << " WHERE m.key='stats_migrate_done';";
    StoreStatement statement(db, sql.str().c_str());
    int index = 1;
    statement.text(index++, scope.targetId);
    for (const auto& type : scope.include) statement.text(index++, type);
    for (const auto& type : scope.exclude) statement.text(index++, type);
    if (!statement.row()) throw std::runtime_error("missing stats aggregate row");
    if (statement.text(2) != "1") throw std::runtime_error("outbox stats migration is incomplete");
    EventPendingStats result{statement.integer(0), statement.integer(1)};
    if (result.pendingCount < 0 || result.pendingTextUnits < 0 ||
        (result.pendingCount == 0 && result.pendingTextUnits != 0) ||
        statement.text(0) != std::to_string(result.pendingCount) ||
        statement.text(1) != std::to_string(result.pendingTextUnits)) {
        throw std::runtime_error("invalid outbox stats counters");
    }
    statement.done();
    statement.finish();
    return result;
}

EventStoreDatabase::EventStoreDatabase(MqttEventOutbox& outbox, EventStoreIdentity identity,
    std::vector<std::string> producers, bool readOnly, std::vector<EventStoreSenderConfig> senders,
    std::function<EventStoreLeaseTime()> leaseClock)
    : outbox_(outbox), identity_(std::move(identity)), producers_(std::move(producers)), readOnly_(readOnly),
      senders_(std::move(senders)), leaseClock_(leaseClock ? std::move(leaseClock) : readEventStoreLeaseTime) {
    if (!storeIdentifier(identity_.storeId) || !storeIdentifier(identity_.configGeneration) ||
        producers_.empty() || producers_.size() > 64) throw std::invalid_argument("invalid event store identity/registry");
    std::set<std::string> unique;
    for (const auto& producer : producers_) {
        if (!storeIdentifier(producer) || !unique.insert(producer).second) {
            throw std::invalid_argument("invalid/duplicate event store producer");
        }
    }
    unique.clear();
    if (senders_.size() > 64) throw std::invalid_argument("too many event store senders");
    for (const auto& sender : senders_) {
        if (!storeIdentifier(sender.senderId) || !storeIdentifier(sender.targetId) ||
            !unique.insert(sender.senderId).second || sender.eventTypes.empty() || sender.eventTypes.size() > 16) {
            throw std::invalid_argument("invalid event store sender scope");
        }
        std::set<std::string> types;
        for (const auto& type : sender.eventTypes) {
            if (!storeIdentifier(type) || !types.insert(type).second) throw std::invalid_argument("invalid sender event types");
        }
    }
    if (readOnly_ != (outbox_.accessMode_ == MqttEventOutbox::AccessMode::ReadOnly)) {
        throw std::invalid_argument("event store connection access mismatch");
    }
    auto* db = static_cast<sqlite3*>(outbox_.checkedDatabase());
    try {
        if (!readOnly_) {
            execOnceOrThrow(db, "BEGIN IMMEDIATE;");
            execOnceOrThrow(db, "CREATE TABLE IF NOT EXISTS event_store_identity("
                "id INTEGER PRIMARY KEY CHECK(id=1),store_id TEXT NOT NULL,config_generation TEXT NOT NULL,"
                "schema_version INTEGER NOT NULL);");
        }
        bool existing = false;
        {
            StoreStatement identityRow(db, "SELECT store_id,config_generation,schema_version FROM event_store_identity WHERE id=1;");
            if (identityRow.row()) {
                existing = true;
                if (identityRow.text(0) != identity_.storeId || identityRow.text(1) != identity_.configGeneration ||
                    identityRow.integer(2) != 1) throw std::runtime_error("event store identity/generation/schema mismatch");
            }
        }
        if (!existing) {
            if (readOnly_) throw std::runtime_error("event store is not initialized");
            StoreStatement oldRows(db, "SELECT EXISTS(SELECT 1 FROM mqtt_event_outbox) OR EXISTS(SELECT 1 FROM mqtt_event_state);");
            if (!oldRows.row() || oldRows.integer(0)) {
                throw std::runtime_error("legacy event store requires an explicit migration; laboratory runtime refuses adoption");
            }
            StoreStatement insert(db, "INSERT INTO event_store_identity VALUES(1,?,?,1);");
            insert.text(1, identity_.storeId);
            insert.text(2, identity_.configGeneration);
            insert.done();
        }
        if (!readOnly_) {
            execOnceOrThrow(db, "CREATE TABLE IF NOT EXISTS event_journal_meta("
                "id INTEGER PRIMARY KEY CHECK(id=1),schema_version INTEGER NOT NULL CHECK(schema_version=1),"
                "journal_generation TEXT NOT NULL);");
            execOnceOrThrow(db, "INSERT OR IGNORE INTO event_journal_meta VALUES(1,1,lower(hex(randomblob(16))));");
            execOnceOrThrow(db, "CREATE TABLE IF NOT EXISTS event_local_journal("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,kind TEXT NOT NULL CHECK(kind IN ('alarm','change')),"
                "event_id TEXT NOT NULL UNIQUE CHECK(length(event_id)>0),point_index INTEGER NOT NULL,"
                "ts INTEGER NOT NULL,alarm_type TEXT NOT NULL,active INTEGER NOT NULL,threshold REAL NOT NULL,"
                "value REAL NOT NULL,quality INTEGER NOT NULL,stale INTEGER NOT NULL,persist_value TEXT NOT NULL,"
                "gateway_code TEXT NOT NULL,device_code TEXT NOT NULL,point_code TEXT NOT NULL,"
                "state_version INTEGER NOT NULL,config_generation TEXT NOT NULL);");
            execOnceOrThrow(db, "CREATE TRIGGER IF NOT EXISTS event_local_journal_immutable "
                "BEFORE UPDATE ON event_local_journal BEGIN SELECT RAISE(ABORT,'immutable journal'); END;");
            execOnceOrThrow(db, "CREATE INDEX IF NOT EXISTS idx_event_local_journal_kind_ts_id "
                "ON event_local_journal(kind,ts,id);");
            execOnceOrThrow(db, "CREATE TABLE IF NOT EXISTS event_history_projection_cursor("
                "id INTEGER PRIMARY KEY CHECK(id=1),projection_id TEXT NOT NULL CHECK(projection_id='alarm-history-v1'),"
                "projected_through INTEGER NOT NULL CHECK(projected_through>=0),"
                "cleaned_through INTEGER NOT NULL CHECK(cleaned_through>=0));");
            execOnceOrThrow(db, "INSERT OR IGNORE INTO event_history_projection_cursor VALUES(1,'alarm-history-v1',0,0);");
            execOnceOrThrow(db, "CREATE TABLE IF NOT EXISTS event_store_producer("
                "producer_id TEXT PRIMARY KEY,session_id TEXT NOT NULL,epoch INTEGER NOT NULL CHECK(epoch>0),"
                "sequence INTEGER NOT NULL CHECK(sequence>=0),request TEXT NOT NULL,receipt TEXT NOT NULL);");
            execOnceOrThrow(db, "CREATE TABLE IF NOT EXISTS event_store_state_version("
                "state_key TEXT PRIMARY KEY,producer_id TEXT NOT NULL,version INTEGER NOT NULL CHECK(version>0));");
            execOnceOrThrow(db, "CREATE INDEX IF NOT EXISTS event_store_state_owner ON event_store_state_version(producer_id,state_key);");
            execOnceOrThrow(db, "CREATE INDEX IF NOT EXISTS idx_event_store_delivery_order "
                "ON mqtt_event_outbox(target_id,event_type,sent,id);");
            execOnceOrThrow(db, "CREATE TABLE IF NOT EXISTS event_store_sender_registry("
                "sender_id TEXT PRIMARY KEY,scope TEXT NOT NULL);");
            execOnceOrThrow(db, "CREATE TABLE IF NOT EXISTS event_store_sender("
                "sender_id TEXT PRIMARY KEY,session_id TEXT NOT NULL,epoch INTEGER NOT NULL CHECK(epoch>0),"
                "sequence INTEGER NOT NULL CHECK(sequence>=0),request TEXT NOT NULL,receipt TEXT NOT NULL,last_op TEXT NOT NULL);");
            execOnceOrThrow(db, "CREATE TABLE IF NOT EXISTS event_store_claim_batch("
                "sender_id TEXT PRIMARY KEY,epoch INTEGER NOT NULL,claim_token TEXT NOT NULL UNIQUE,"
                "boot_id TEXT NOT NULL,lease_until INTEGER NOT NULL);");
            execOnceOrThrow(db, "CREATE TABLE IF NOT EXISTS event_store_claim_item("
                "sender_id TEXT NOT NULL,row_id INTEGER NOT NULL UNIQUE,event_id TEXT NOT NULL,"
                "target_id TEXT NOT NULL,state TEXT NOT NULL,PRIMARY KEY(sender_id,row_id));");
            {
                StoreStatement foreign(db, "SELECT 1 FROM mqtt_event_outbox o "
                    "LEFT JOIN event_store_claim_item i ON i.row_id=o.id "
                    "LEFT JOIN event_store_claim_batch b ON b.sender_id=i.sender_id "
                    "WHERE o.sent=0 AND o.claim_token IS NOT NULL AND "
                    "(b.claim_token IS NULL OR o.claim_token!=b.claim_token OR i.state!='active' "
                    "OR o.event_id!=i.event_id OR o.target_id!=i.target_id) LIMIT 1;");
                if (foreign.row()) throw std::runtime_error("foreign or inconsistent claim; explicit migration required");
            }
            for (const auto& sender : senders_) {
                auto types = sender.eventTypes;
                std::sort(types.begin(), types.end());
                std::string scope = sender.targetId + "\n";
                for (const auto& type : types) scope += type + "\n";
                StoreStatement row(db, "SELECT scope FROM event_store_sender_registry WHERE sender_id=?;");
                row.text(1, sender.senderId);
                if (row.row()) {
                    if (row.text(0) != scope) throw std::invalid_argument("sender scope changed; explicit migration required");
                } else {
                    StoreStatement add(db, "INSERT INTO event_store_sender_registry VALUES(?,?);");
                    add.text(1, sender.senderId); add.text(2, scope); add.done();
                }
            }
            {
                StoreStatement count(db, "SELECT count(*) FROM event_store_sender_registry;");
                if (!count.row() || count.integer(0) > 64) throw std::invalid_argument("persistent sender registry exceeds 64");
            }
            execOnceOrThrow(db, "COMMIT;");
        }
    } catch (...) {
        if (!readOnly_) outbox_.rollbackAfterFailure();
        throw;
    }
}

std::vector<EventStoreJournalRow> EventStoreDatabase::readJournal(std::int64_t afterId, std::size_t limit) {
    if (afterId < 0 || limit == 0 || limit > 64) throw std::invalid_argument("journal page must be 1..64 with nonnegative cursor");
    auto* db = static_cast<sqlite3*>(outbox_.checkedDatabase());
    const auto sql = std::string("SELECT ") + journalColumns +
        " FROM event_local_journal WHERE id>? ORDER BY id LIMIT ?;";
    StoreStatement row(db, sql.c_str());
    row.integer(1, afterId); row.integer(2, static_cast<std::int64_t>(limit));
    std::vector<EventStoreJournalRow> result;
    while (row.row()) result.push_back(readJournalRow(row));
    return result;
}

std::string EventStoreDatabase::journalGeneration() {
    StoreStatement row(static_cast<sqlite3*>(outbox_.checkedDatabase()),
        "SELECT schema_version,journal_generation FROM event_journal_meta WHERE id=1;");
    if (!row.row() || row.integer(0) != 1 || row.text(1).empty()) throw std::runtime_error("invalid journal metadata");
    return row.text(1);
}

const std::string& EventStoreDatabase::journalDatabasePath() const { return outbox_.dbPath_; }

EventStoreProjectionCursor EventStoreDatabase::projectionCursor() {
    StoreStatement row(static_cast<sqlite3*>(outbox_.checkedDatabase()),
        "SELECT m.journal_generation,c.projected_through,c.cleaned_through,"
        "COALESCE((SELECT MAX(id) FROM event_local_journal),0) "
        "FROM event_journal_meta m JOIN event_history_projection_cursor c ON c.id=m.id "
        "WHERE m.id=1 AND m.schema_version=1 AND c.projection_id='alarm-history-v1';");
    if (!row.row() || row.text(0).empty() || row.integer(1) < row.integer(2) || row.integer(2) < 0)
        throw std::runtime_error("invalid source projection cursor");
    return {row.text(0), row.integer(1), row.integer(2), row.integer(3)};
}

void EventStoreDatabase::commitProjectionCursor(const std::string& generation,
    std::int64_t expected, std::int64_t through) {
    if (readOnly_) throw std::logic_error("read-only connection cannot commit projection cursor");
    if (expected < 0 || through < 0 || (through > expected && through - expected > 64))
        throw std::invalid_argument("projection cursor advance exceeds bounded batch");
    auto* db = static_cast<sqlite3*>(outbox_.checkedDatabase());
    writeTransaction([&] {
        const auto prior = projectionCursor();
        if (prior.journalGeneration != generation || through < prior.cleanedThrough || through > prior.journalThrough)
            throw EventStoreConflict("PROJECTION_CURSOR_CONFLICT", "projection generation/range mismatch");
        if (prior.projectedThrough == through) return EventStoreProducer{};
        if (prior.projectedThrough != expected)
            throw EventStoreConflict("PROJECTION_CURSOR_CONFLICT", "projection expected cursor changed");
        StoreStatement update(db, "UPDATE event_history_projection_cursor SET projected_through=? WHERE id=1;");
        update.integer(1, through); update.done();
        return EventStoreProducer{};
    });
}

#ifndef _WIN32
namespace {
std::string projectionRealPath(const std::string& path) {
    char* resolved = ::realpath(path.c_str(), nullptr);
    if (!resolved) throw std::runtime_error("projection physical path cannot be resolved");
    std::string result(resolved);
    std::free(resolved);
    return result;
}

void verifyProjectionFile(int fd, const std::string& requested, const std::string& canonical) {
    struct stat held{}, named{}, resolved{};
    if (fd < 0 || fstat(fd, &held) || lstat(requested.c_str(), &named) || lstat(canonical.c_str(), &resolved) ||
        !S_ISREG(held.st_mode) || !S_ISREG(named.st_mode) || !S_ISREG(resolved.st_mode) ||
        held.st_nlink != 1 || named.st_nlink != 1 || resolved.st_nlink != 1 ||
        held.st_dev != named.st_dev || held.st_ino != named.st_ino ||
        held.st_dev != resolved.st_dev || held.st_ino != resolved.st_ino ||
        projectionRealPath(requested) != canonical) {
        throw std::runtime_error("projection physical file replaced, aliased or not regular");
    }
}
} // namespace
#endif

// Projection SQL deliberately shares the process-pinned SQLite API above. Opening a
// separate loader here could silently mix the system and application-private engines.
EventHistoryProjection::EventHistoryProjection(EventStoreDatabase& source, const std::string& path)
    : storeId_(source.identity_.storeId), generation_(source.journalGeneration()) {
    if (path.empty() || path.front() != '/' || path.find('\0') != std::string::npos) throw std::invalid_argument("invalid absolute history path");
#ifdef _WIN32
    throw std::runtime_error("history projection requires POSIX physical-file ownership lock");
#else
    requestedPath_ = path;
    sourcePath_ = source.journalDatabasePath();
    // Resolve only the parent before open: resolving the final component would hide a symlink.
    const auto slash = path.find_last_of('/');
    canonicalPath_ = projectionRealPath(slash == 0 ? "/" : path.substr(0, slash));
    if (canonicalPath_ != "/") canonicalPath_ += '/';
    canonicalPath_ += path.substr(slash + 1);
    lockFd_ = ::open(canonicalPath_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lockFd_ < 0) throw std::runtime_error("cannot open history ownership file");
    try {
        canonicalPath_ = projectionRealPath(canonicalPath_);
        canonicalSourcePath_ = projectionRealPath(sourcePath_);
        sourceFd_ = ::open(sourcePath_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        verifyProjectionFile(lockFd_, requestedPath_, canonicalPath_);
        verifyProjectionFile(sourceFd_, sourcePath_, canonicalSourcePath_);
        struct stat history{}, origin{};
        if (fstat(lockFd_, &history) || fstat(sourceFd_, &origin) ||
            !S_ISREG(history.st_mode) || (history.st_dev == origin.st_dev && history.st_ino == origin.st_ino)) {
            throw std::runtime_error("history must be a distinct regular physical database");
        }
        if (flock(lockFd_, LOCK_EX | LOCK_NB)) throw std::runtime_error("history projection already owned");
        sqlite3* db = nullptr;
        const int rc = g_open(canonicalPath_.c_str(), &db, kSqliteOpenReadWrite | kSqliteOpenPrivateCache, nullptr);
        database_ = db;
        if (rc != kSqliteOk) throw sqliteFailure(db, rc, "projection.open");
        checkPhysicalFiles();
        g_extended_result_codes(db, 1);
        g_busy_timeout(db, 25);
        execOnceOrThrow(db, "PRAGMA synchronous=FULL;");
        {
            StoreStatement sync(db, "PRAGMA synchronous;");
            if (!sync.row() || sync.integer(0) != 2) throw std::runtime_error("history FULL unavailable");
        }
        {
            StoreStatement mode(db, "PRAGMA journal_mode;");
            if (!mode.row() || (mode.text(0) != "delete" && mode.text(0) != "wal" &&
                mode.text(0) != "truncate" && mode.text(0) != "persist")) {
                throw std::runtime_error("history journal mode is not durable");
            }
        }
        execOnceOrThrow(db, "BEGIN IMMEDIATE;");
        execOnceOrThrow(db, "CREATE TABLE IF NOT EXISTS alarm_events("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,event_id TEXT,point_index INTEGER NOT NULL,ts INTEGER NOT NULL,"
            "alarm_type TEXT NOT NULL,active INTEGER NOT NULL,threshold REAL NOT NULL,value REAL NOT NULL,"
            "quality INTEGER NOT NULL,stale INTEGER NOT NULL,persist_value TEXT NOT NULL,"
            "gateway_code TEXT NOT NULL,device_code TEXT NOT NULL,point_code TEXT NOT NULL);");
        // Old pre-event_id schemas require explicit offline migration, never silent adoption.
        execOnceOrThrow(db, "CREATE UNIQUE INDEX IF NOT EXISTS idx_alarm_events_event_id "
            "ON alarm_events(event_id) WHERE event_id IS NOT NULL AND event_id<>'';");
        execOnceOrThrow(db, "CREATE INDEX IF NOT EXISTS idx_alarm_events_ts_id ON alarm_events(ts,id);");
        execOnceOrThrow(db, "CREATE TABLE IF NOT EXISTS alarm_projection_meta("
            "id INTEGER PRIMARY KEY CHECK(id=1),projection_id TEXT NOT NULL,store_id TEXT NOT NULL,"
            "journal_generation TEXT NOT NULL,last_contiguous_journal_id INTEGER NOT NULL CHECK(last_contiguous_journal_id>=0));");
        StoreStatement insert(db, "INSERT OR IGNORE INTO alarm_projection_meta VALUES(1,'alarm-history-v1',?,?,0);");
        insert.text(1, storeId_); insert.text(2, generation_); insert.done(); insert.finish();
        checkMetadata();
        execOnceOrThrow(db, "COMMIT;");
        checkPhysicalFiles();
    } catch (...) {
        if (database_) { g_close(static_cast<sqlite3*>(database_)); database_ = nullptr; }
        ::close(lockFd_); lockFd_ = -1;
        if (sourceFd_ >= 0) { ::close(sourceFd_); sourceFd_ = -1; }
        throw;
    }
#endif
}

EventHistoryProjection::~EventHistoryProjection() {
    if (database_) g_close(static_cast<sqlite3*>(database_));
#ifndef _WIN32
    if (lockFd_ >= 0) ::close(lockFd_);
    if (sourceFd_ >= 0) ::close(sourceFd_);
#endif
}

void EventHistoryProjection::checkPhysicalFiles() {
#ifndef _WIN32
    verifyProjectionFile(lockFd_, requestedPath_, canonicalPath_);
    verifyProjectionFile(sourceFd_, sourcePath_, canonicalSourcePath_);
#endif
}

void EventHistoryProjection::checkIdentity(EventStoreDatabase& source) {
    checkPhysicalFiles();
#ifndef _WIN32
    verifyProjectionFile(sourceFd_, source.journalDatabasePath(), canonicalSourcePath_);
#endif
    checkMetadata();
    if (source.identity_.storeId != storeId_ || source.journalGeneration() != generation_)
        throw std::runtime_error("history projection identity mismatch");
}

void EventHistoryProjection::checkMetadata() {
    (void)checkedWatermark();
}

std::int64_t EventHistoryProjection::checkedWatermark() {
    checkPhysicalFiles();
    if (!database_) throw std::runtime_error("projection connection is closed; reopen and reconcile");
    StoreStatement row(static_cast<sqlite3*>(database_),
        "SELECT projection_id,store_id,journal_generation,last_contiguous_journal_id FROM alarm_projection_meta WHERE id=1;");
    if (!row.row() || row.text(0) != "alarm-history-v1" || row.text(1) != storeId_ || row.text(2) != generation_) {
        throw std::runtime_error("history projection identity mismatch");
    }
    const auto through = row.integer(3);
    if (through < 0) throw std::runtime_error("missing/invalid history watermark");
    checkPhysicalFiles();
    return through;
}

std::int64_t EventHistoryProjection::watermark() {
    checkPhysicalFiles();
    if (!database_) throw std::runtime_error("projection connection is closed; reopen and reconcile");
    StoreStatement row(static_cast<sqlite3*>(database_),
        "SELECT last_contiguous_journal_id FROM alarm_projection_meta WHERE id=1;");
    if (!row.row() || row.integer(0) < 0) throw std::runtime_error("missing/invalid history watermark");
    checkPhysicalFiles();
    return row.integer(0);
}

void EventHistoryProjection::verify(const EventStoreJournalRow& journal) {
    checkPhysicalFiles();
    verifyContent(journal);
}

void EventHistoryProjection::verifyContent(const EventStoreJournalRow& journal) {
    if (journal.local.kind == "change") return;
    if (journal.local.kind != "alarm") throw std::runtime_error("unknown journal kind");
    StoreStatement row(static_cast<sqlite3*>(database_), "SELECT 0,'alarm',event_id,point_index,ts,alarm_type,"
        "active,threshold,value,quality,stale,persist_value,gateway_code,device_code,point_code,0,'' "
        "FROM alarm_events WHERE event_id=?;");
    row.text(1, journal.local.event.eventId);
    if (!row.row()) throw EventStoreConflict("PROJECTION_MISSING", "history row is absent");
    auto actual = readJournalRow(row).local;
    actual.stateVersion = journal.local.stateVersion;
    actual.configGeneration = journal.local.configGeneration;
    if (localIdentity(actual) != localIdentity(journal.local)) {
        throw EventStoreConflict("PROJECTION_CONFLICT", "same eventId has different historical content");
    }
    if (row.row()) throw EventStoreConflict("PROJECTION_CONFLICT", "duplicate history identity");
}

std::int64_t EventHistoryProjection::replay(EventStoreDatabase& reader, std::int64_t afterId, std::size_t limit) {
    checkIdentity(reader);
    return projectRows(reader.readJournal(afterId, limit), afterId);
}

std::int64_t EventHistoryProjection::auditRows(const std::vector<EventStoreJournalRow>& rows, std::int64_t afterId) {
    checkMetadata();
    if (afterId < 0 || rows.size() > 64) throw std::invalid_argument("invalid projection audit page");
    auto verified = afterId;
    for (const auto& row : rows) {
        if (verified == std::numeric_limits<std::int64_t>::max() || row.id != verified + 1)
            throw std::runtime_error("journal gap during projection audit");
        try { verifyContent(row); }
        catch (const EventStoreConflict& error) {
            if (error.code() != "PROJECTION_MISSING") throw;
            checkPhysicalFiles();
            return verified;
        }
        verified = row.id;
    }
    checkPhysicalFiles();
    return verified;
}

std::int64_t EventHistoryProjection::projectRows(const std::vector<EventStoreJournalRow>& rows, std::int64_t afterId) {
    if (rows.size() > 64) throw std::invalid_argument("projection page exceeds 64");
    const auto committed = checkedWatermark();
    if (afterId < 0 || afterId > committed) throw std::invalid_argument("projection cannot skip unprocessed journal rows");
    if (rows.empty()) return afterId;
    // New journal ids are gap-free: INSERT (not INSERT OR IGNORE) allocates ids only for new events.
    auto expected = afterId;
    for (const auto& row : rows) {
        if (expected == std::numeric_limits<std::int64_t>::max() || row.id != ++expected) {
            throw std::runtime_error("journal gap: restore valid backup before projecting");
        }
    }
    auto* db = static_cast<sqlite3*>(database_);
    try {
        execOnceOrThrow(db, "BEGIN IMMEDIATE;");
        for (const auto& row : rows) {
            if (row.local.kind == "alarm") {
                StoreStatement insert(db, "INSERT OR IGNORE INTO alarm_events(event_id,point_index,ts,alarm_type,"
                    "active,threshold,value,quality,stale,persist_value,gateway_code,device_code,point_code) "
                    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?);");
                bindAlarm(insert, row.local.event); insert.done(); insert.finish();
            }
            verifyContent(row);
        }
        StoreStatement advance(db, "UPDATE alarm_projection_meta SET last_contiguous_journal_id=? WHERE id=1;");
        advance.integer(1, std::max(committed, rows.back().id)); advance.done(); advance.finish();
        checkPhysicalFiles();
        execOnceOrThrow(db, "COMMIT;");
        checkPhysicalFiles();
        return rows.back().id;
    } catch (...) {
        // Preserve the original FULL/IOERR/COMMIT error. A failed rollback poisons this connection.
        if (!g_get_autocommit(db) && g_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr) != kSqliteOk) {
            g_close(db); database_ = nullptr;
        }
        throw;
    }
}

std::int64_t EventStoreDatabase::reconcileProjection(EventHistoryProjection& projection) {
    if (readOnly_) throw std::logic_error("read-only connection cannot reconcile projection cursor");
    projection.checkIdentity(*this);
    const auto historyThrough = projection.watermark();
    // Audit retained copies in bounded read pages before trusting metadata after a
    // restart/backup restore. No source write transaction is held during this scan.
    std::int64_t verified = 0;
    while (verified < historyThrough) {
        const auto rows = readJournal(verified, 64);
        if (rows.empty()) break;
        bool missing = false;
        for (const auto& row : rows) {
            if (row.id > historyThrough) break;
            if (row.id != verified + 1) throw std::runtime_error("journal gap during recovery audit");
            try { projection.verify(row); }
            catch (const EventStoreConflict& error) {
                if (error.code() != "PROJECTION_MISSING") throw;
                missing = true;
                break;
            }
            verified = row.id;
        }
        if (missing) break;
    }
    std::int64_t recovered = 0;
    auto* db = static_cast<sqlite3*>(outbox_.checkedDatabase());
    writeTransaction([&] {
        StoreStatement row(db, "SELECT projected_through,cleaned_through FROM event_history_projection_cursor WHERE id=1;");
        if (!row.row()) throw std::runtime_error("missing source projection cursor");
        recovered = std::min(row.integer(0), std::min(historyThrough, verified));
        if (recovered < row.integer(1)) throw std::runtime_error("history backup predates cleaned journal; unrecoverable without backup");
        StoreStatement high(db, "SELECT COALESCE(MAX(id),0) FROM event_local_journal;");
        if (!high.row() || historyThrough > high.integer(0)) throw std::runtime_error("history is ahead of source journal; backup mismatch");
        StoreStatement update(db, "UPDATE event_history_projection_cursor SET projected_through=? WHERE id=1;");
        update.integer(1, recovered); update.done();
        return EventStoreProducer{};
    });
    return recovered;
}

void EventStoreDatabase::confirmProjection(EventHistoryProjection& projection) {
    if (readOnly_) throw std::logic_error("read-only connection cannot confirm projection");
    projection.checkIdentity(*this);
    const auto through = projection.watermark();
    const auto cursor = projectionCursor();
    auto last = cursor.projectedThrough;
    if (last > through) throw std::runtime_error("projection rolled back; reconcile before confirmation");
    // Finish source reads and history verification before acquiring a source write lock.
    const auto rows = readJournal(last, 64);
    for (const auto& row : rows) {
        if (row.id > through) break;
        if (row.id != last + 1) throw std::runtime_error("journal gap during confirmation");
        projection.verify(row);
        last = row.id;
    }
    projection.checkPhysicalFiles();
    commitProjectionCursor(cursor.journalGeneration, cursor.projectedThrough, last);
}

void EventStoreDatabase::checkProducer(const std::string& id) const {
    if (std::find(producers_.begin(), producers_.end(), id) == producers_.end()) {
        throw std::invalid_argument("event store producer is not registered in configuration");
    }
}

EventStoreProducer EventStoreDatabase::writeTransaction(const std::function<EventStoreProducer()>& operation) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    for (int attempt = 0;; ++attempt) {
        auto* db = static_cast<sqlite3*>(outbox_.checkedDatabase());
        try {
            execOnceOrThrow(db, "BEGIN IMMEDIATE;");
            auto result = operation();
            execOnceOrThrow(db, "COMMIT;");
            return result;
        } catch (const SqliteError& ex) {
            outbox_.rollbackAfterFailure();
            if (!ex.isBusy() || outbox_.databasePoisoned_ || attempt >= 3 ||
                std::chrono::steady_clock::now() >= deadline) throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } catch (const std::invalid_argument& ex) {
            outbox_.rollbackAfterFailure();
            if (outbox_.databasePoisoned_) {
                throw std::runtime_error(std::string(ex.what()) + "; rollback unconfirmed, reconcile receipt");
            }
            throw;
        } catch (...) { outbox_.rollbackAfterFailure(); throw; }
    }
}

EventStoreProducer EventStoreDatabase::registerProducer(const std::string& id,
    const std::string& session, std::int64_t expectedEpoch) {
    checkProducer(id);
    if (readOnly_) throw std::logic_error("event store read-only connection cannot register");
    if (!storeIdentifier(session) || expectedEpoch < 0) throw std::invalid_argument("invalid producer registration");
    auto* db = static_cast<sqlite3*>(outbox_.checkedDatabase());
    return writeTransaction([&] {
        auto prior = readProducer(db, id);
        if (prior.sessionId == session) {
            if (expectedEpoch != prior.epoch - 1) throw EventStoreConflict("REGISTER_CONFLICT", "retry must keep original expected epoch");
            return prior;
        }
        if (prior.epoch != expectedEpoch || prior.epoch == std::numeric_limits<std::int64_t>::max()) {
            throw EventStoreConflict("STALE_EPOCH", "producer registration must reconcile current epoch");
        }
        EventStoreProducer next;
        next.producerId = id;
        next.sessionId = session;
        next.epoch = prior.epoch + 1;
        {
            StoreStatement write(db, "INSERT OR REPLACE INTO event_store_producer VALUES(?,?,?,0,'','');");
            write.text(1, id);
            write.text(2, session);
            write.integer(3, next.epoch);
            write.done();
        }
        return next;
    });
}

EventStoreProducer EventStoreDatabase::receipt(const std::string& id) {
    checkProducer(id);
    return readProducer(static_cast<sqlite3*>(outbox_.checkedDatabase()), id);
}

void EventStoreDatabase::setCapacityLimits(EventStoreCapacityLimits limits) {
    if (!limits.historyPath.empty() && (limits.historyPath.front() != '/' ||
        limits.historyPath.find('\0') != std::string::npos))
        throw std::invalid_argument("capacity historyPath must be absolute");
#ifdef _WIN32
    if (limits.minFreeBytes || limits.maxStoreBytes)
        throw std::invalid_argument("EventStore capacity admission requires POSIX filesystem statistics");
#endif
    capacityLimits_ = std::move(limits);
}

EventStoreCapacityStatus EventStoreDatabase::capacityStatus() const {
#ifdef _WIN32
    throw std::runtime_error("EventStore capacity inspection requires POSIX filesystem statistics");
#else
    EventStoreCapacityStatus status;
    status.availableBytes = std::numeric_limits<std::uint64_t>::max();
    std::vector<std::string> paths{outbox_.dbPath_};
    if (!capacityLimits_.historyPath.empty()) paths.push_back(capacityLimits_.historyPath);
    std::set<std::pair<dev_t, ino_t>> seen;
    for (const auto& path : paths) {
        struct statvfs space{};
        if (statvfs(path.c_str(), &space) != 0)
            throw EventStoreConflict("CAPACITY_REJECTED", "cannot inspect database filesystem space");
        const auto unit = static_cast<std::uint64_t>(space.f_frsize ? space.f_frsize : space.f_bsize);
        if (unit == 0) throw EventStoreConflict("CAPACITY_REJECTED", "invalid filesystem block size");
        const auto blocks = static_cast<std::uint64_t>(space.f_bavail);
        const auto available = blocks > std::numeric_limits<std::uint64_t>::max() / unit ?
            std::numeric_limits<std::uint64_t>::max() : blocks * unit;
        status.availableBytes = std::min(status.availableBytes, available);
        for (const auto* suffix : {"", "-wal", "-shm", "-journal"}) {
            struct stat file{};
            const auto filename = path + suffix;
            if (lstat(filename.c_str(), &file) != 0) {
                // A sidecar can disappear at a concurrent checkpoint. The database itself cannot.
                if (*suffix && errno == ENOENT) continue;
                throw EventStoreConflict("CAPACITY_REJECTED", "cannot inspect database footprint");
            }
            if (!S_ISREG(file.st_mode) || file.st_size < 0 || file.st_nlink != 1)
                throw EventStoreConflict("CAPACITY_REJECTED", "database footprint contains a nonregular/aliased file");
            if (!seen.emplace(file.st_dev, file.st_ino).second) continue;
            const auto size = static_cast<std::uint64_t>(file.st_size);
            status.storeBytes = size > std::numeric_limits<std::uint64_t>::max() - status.storeBytes ?
                std::numeric_limits<std::uint64_t>::max() : status.storeBytes + size;
        }
    }
    if (capacityLimits_.minFreeBytes && status.availableBytes <= capacityLimits_.minFreeBytes) {
        status.blocked = true;
        status.reason = "minFreeBytes reached: available=" + std::to_string(status.availableBytes) +
            " required_above=" + std::to_string(capacityLimits_.minFreeBytes);
    } else if (capacityLimits_.maxStoreBytes && status.storeBytes >= capacityLimits_.maxStoreBytes) {
        status.blocked = true;
        status.reason = "maxStoreBytes reached: observed=" + std::to_string(status.storeBytes) +
            " limit=" + std::to_string(capacityLimits_.maxStoreBytes);
    }
    return status;
#endif
}

EventStoreProducer EventStoreDatabase::append(const EventStoreAppend& request) {
    checkProducer(request.producerId);
    if (readOnly_) throw std::logic_error("event store read-only connection cannot append");
    if (request.epoch <= 0 || request.sequence <= 0 || request.events.size() > 64 || request.states.size() > 64 ||
        request.localEvents.size() > 64 ||
        (request.events.empty() && request.states.empty() && request.localEvents.empty())) throw std::invalid_argument("invalid event store append shape");
    boundedStoreText(request.request, 256 * 1024, true);
    std::size_t bytes = 0;
    std::set<std::string> localIds;
    for (const auto& local : request.localEvents) {
        const auto& e = local.event;
        boundedStoreText(e.eventId, 256, true);
        boundedStoreText(e.alarmType, 256, local.kind == "alarm");
        boundedStoreText(e.persistValue, 16384, false);
        boundedStoreText(e.machineCode, 256, false);
        boundedStoreText(e.meterCode, 256, false);
        boundedStoreText(e.pointCode, 256, false);
        boundedStoreText(local.configGeneration, 96, true);
        if ((local.kind != "alarm" && local.kind != "change") || e.ts <= 0 ||
            local.stateVersion < 0 || !std::isfinite(e.threshold) || !std::isfinite(e.value) ||
            !localIds.insert(e.eventId).second || local.configGeneration != identity_.configGeneration) {
            throw std::invalid_argument("invalid local event kind/value/version/generation/identity");
        }
        bytes += localIdentity(local).size();
    }
    std::set<std::pair<std::string, std::string>> eventKeys;
    for (const auto& event : request.events) {
        boundedStoreText(event.eventId, 256, true);
        boundedStoreText(event.targetId, 96, true);
        boundedStoreText(event.eventType, 96, true);
        boundedStoreText(event.topic, 4096, true);
        boundedStoreText(event.payload, 256 * 1024, false);
        if (event.eventTs <= 0 || !eventKeys.emplace(event.eventId, event.targetId).second) {
            throw std::invalid_argument("invalid event timestamp or duplicate event/target in append");
        }
        bytes += event.eventId.size() + event.targetId.size() + event.eventType.size() + event.topic.size() + event.payload.size();
    }
    std::set<std::string> stateKeys;
    for (const auto& item : request.states) {
        const auto& state = item.state;
        boundedStoreText(state.stateKey, 256, true);
        boundedStoreText(state.eventType, 96, true);
        boundedStoreText(state.alarmType, 256, false);
        boundedStoreText(state.lifecycle, 4096, false);
        if (!stateKeys.insert(state.stateKey).second || item.version < 0 ||
            item.version == std::numeric_limits<std::int64_t>::max() || !std::isfinite(state.value)) {
            throw std::invalid_argument("invalid state version/value or duplicate state in append");
        }
        bytes += state.stateKey.size() + state.eventType.size() + state.alarmType.size() + state.lifecycle.size() + 128;
    }
    if (bytes > 256 * 1024) throw std::invalid_argument("event store append data exceeds byte limit");
    const auto immutableIdentity = appendIdentity(request);
    auto* db = static_cast<sqlite3*>(outbox_.checkedDatabase());
    return writeTransaction([&] {
        auto producer = readProducer(db, request.producerId, true);
        if (producer.epoch != request.epoch) throw EventStoreConflict("STALE_EPOCH", "append producer was replaced");
        if (producer.sequence == request.sequence) {
            if (producer.request != immutableIdentity) throw EventStoreConflict("REQUEST_CONFLICT", "same sequence with different bytes/data");
            return producer;
        }
        if (producer.sequence == std::numeric_limits<std::int64_t>::max() || request.sequence != producer.sequence + 1) {
            throw EventStoreConflict("STALE_OR_GAPPED_SEQUENCE", "reconcile last committed receipt");
        }
        // Check only new mutations, after exact receipt replay. Never invoke Legacy
        // pruning to satisfy this gate; ACK, reads and projection recovery bypass it.
        if (capacityLimits_.minFreeBytes || capacityLimits_.maxStoreBytes) {
            const auto capacity = capacityStatus();
            if (capacity.blocked) throw EventStoreConflict("CAPACITY_REJECTED", capacity.reason);
        }
        std::vector<std::int64_t> journalIds;
        for (const auto& local : request.localEvents) {
            const auto sql = std::string("SELECT ") + journalColumns + " FROM event_local_journal WHERE event_id=?;";
            StoreStatement prior(db, sql.c_str());
            prior.text(1, local.event.eventId);
            if (prior.row()) {
                auto row = readJournalRow(prior);
                if (localIdentity(row.local) != localIdentity(local)) {
                    throw EventStoreConflict("JOURNAL_CONFLICT", "same eventId with different local content");
                }
                journalIds.push_back(row.id);
            } else {
                StoreStatement insert(db, "INSERT INTO event_local_journal(kind,event_id,point_index,ts,alarm_type,"
                    "active,threshold,value,quality,stale,persist_value,gateway_code,device_code,point_code,"
                    "state_version,config_generation) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
                insert.text(1, local.kind); bindAlarm(insert, local.event, 2);
                insert.integer(15, local.stateVersion); insert.text(16, local.configGeneration); insert.done();
                journalIds.push_back(g_last_insert_rowid(db));
            }
        }
        for (const auto& item : request.states) {
            StoreStatement row(db, "SELECT producer_id,version FROM event_store_state_version WHERE state_key=?;");
            row.text(1, item.state.stateKey);
            const bool found = row.row();
            if ((found && (row.text(0) != request.producerId || row.integer(1) != item.version)) ||
                (!found && item.version != 0)) throw std::invalid_argument("STATE_CONFLICT: wrong owner or expected version");
        }
        const bool managementOnly = request.states.empty() && request.localEvents.empty() &&
            std::all_of(request.events.begin(), request.events.end(), [](const MqttEventOutbox::EventMessage& event) {
                return event.eventType == "ota_status";
            });
        std::vector<MqttEventOutbox::EventMessage> newEvents;
        for (const auto& event : request.events) {
            StoreStatement row(db, "SELECT event_type,topic,payload,event_ts FROM mqtt_event_outbox WHERE event_id=? AND target_id=?;");
            row.text(1, event.eventId);
            row.text(2, event.targetId);
            if (row.row()) {
                if (!managementOnly || row.text(0) != event.eventType || row.text(1) != event.topic ||
                    row.text(2) != event.payload || row.integer(3) != event.eventTs) {
                    throw std::invalid_argument("EVENT_CONFLICT: event identity reused by a new request");
                }
                // A management retry must not touch sent state or an active claim.
            } else {
                newEvents.push_back(event);
            }
        }
        std::vector<MqttEventOutbox::EventState> states;
        for (const auto& item : request.states) states.push_back(item.state);
        auto result = outbox_.enqueueBatchAndStates(newEvents, states, false, false);
        (void)outbox_.checkedDatabase();
        if (managementOnly) {
            result.ids.clear();
            for (const auto& event : request.events) {
                StoreStatement row(db, "SELECT id FROM mqtt_event_outbox WHERE event_id=? AND target_id=?;");
                row.text(1, event.eventId);
                row.text(2, event.targetId);
                if (!row.row()) throw std::logic_error("management event missing inside append transaction");
                result.ids.push_back(row.integer(0));
            }
        }
        for (const auto& item : request.states) {
            StoreStatement write(db, "INSERT OR REPLACE INTO event_store_state_version VALUES(?,?,?);");
            write.text(1, item.state.stateKey);
            write.text(2, request.producerId);
            write.integer(3, item.version + 1);
            write.done();
        }
        std::ostringstream receipt;
        receipt << "{\"committed\":true,\"sequence\":\"" << request.sequence << "\",\"ids\":[";
        for (std::size_t i = 0; i < result.ids.size(); ++i) {
            if (i) receipt << ',';
            receipt << '\"' << result.ids[i] << '\"';
        }
        receipt << "]";
        if (!journalIds.empty()) {
            receipt << ",\"journalIds\":[";
            for (std::size_t i = 0; i < journalIds.size(); ++i) {
                if (i) receipt << ',';
                receipt << '\"' << journalIds[i] << '\"';
            }
            receipt << ']';
        }
        receipt << "}";
        producer.sequence = request.sequence;
        producer.request = immutableIdentity;
        producer.receipt = receipt.str();
        {
            StoreStatement write(db, "UPDATE event_store_producer SET sequence=?,request=?,receipt=? WHERE producer_id=? AND epoch=?;");
            write.integer(1, producer.sequence);
            write.text(2, producer.request);
            write.text(3, producer.receipt);
            write.text(4, producer.producerId);
            write.integer(5, producer.epoch);
            write.done();
            if (g_changes(db) != 1) throw std::logic_error("event store producer disappeared inside transaction");
        }
        return producer;
    });
}

std::vector<EventStoreState> EventStoreDatabase::states(const std::string& id,
    const std::string& afterKey, std::size_t limit) {
    checkProducer(id);
    if (limit == 0 || limit > 64) throw std::invalid_argument("event store state page limit must be 1..64");
    boundedStoreText(afterKey, 256, false);
    auto* db = static_cast<sqlite3*>(outbox_.checkedDatabase());
    const auto query = [&] {
        StoreStatement row(db, "SELECT s.state_key,s.event_type,s.point_index,s.alarm_type,s.active,s.value,"
            "s.quality,s.source_ts,s.lifecycle,v.version FROM event_store_state_version v "
            "JOIN mqtt_event_state s ON s.state_key=v.state_key "
            "WHERE v.producer_id=? AND v.state_key>? ORDER BY v.state_key LIMIT ?;");
        row.text(1, id);
        row.text(2, afterKey);
        row.integer(3, static_cast<std::int64_t>(limit));
        std::vector<EventStoreState> result;
        while (row.row()) {
            EventStoreState item;
            auto& state = item.state;
            state.stateKey = row.text(0);
            state.eventType = row.text(1);
            state.index = static_cast<std::uint32_t>(row.integer(2));
            state.alarmType = row.text(3);
            state.active = row.integer(4) != 0;
            state.value = row.number(5);
            state.quality = static_cast<int>(row.integer(6));
            state.sourceTs = row.integer(7);
            state.lifecycle = row.text(8);
            item.version = row.integer(9);
            result.push_back(std::move(item));
        }
        return result;
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    for (int attempt = 0;; ++attempt) {
        try { return query(); }
        catch (const SqliteError& ex) {
            // Recreate the statement and snapshot; never return a partial page.
            if (!ex.isBusy() || attempt >= 3 || std::chrono::steady_clock::now() >= deadline) throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

namespace {
std::string storeQuote(const std::string& value) {
    static const char* hex = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char ch : value) {
        if (ch == '"' || ch == '\\') { result += '\\'; result += static_cast<char>(ch); }
        else if (ch < 32) { result += "\\u00"; result += hex[ch >> 4]; result += hex[ch & 15]; }
        else result += static_cast<char>(ch);
    }
    return result + '"';
}

struct DeliverySender {
    std::string id, session, request, receipt, operation;
    std::int64_t epoch = 0, sequence = 0;
};

DeliverySender readSender(sqlite3* db, const std::string& id) {
    StoreStatement row(db, "SELECT session_id,epoch,sequence,request,receipt,last_op FROM event_store_sender WHERE sender_id=?;");
    row.text(1, id);
    DeliverySender result;
    result.id = id;
    if (row.row()) {
        result.session = row.text(0); result.epoch = row.integer(1); result.sequence = row.integer(2);
        result.request = row.text(3); result.receipt = row.text(4); result.operation = row.text(5);
    }
    return result;
}

struct DeliveryBatch {
    std::string token, boot;
    std::int64_t epoch = 0, until = 0;
};

DeliveryBatch readBatch(sqlite3* db, const std::string& sender) {
    StoreStatement row(db, "SELECT epoch,claim_token,boot_id,lease_until FROM event_store_claim_batch WHERE sender_id=?;");
    row.text(1, sender);
    DeliveryBatch result;
    if (row.row()) {
        result.epoch = row.integer(0); result.token = row.text(1); result.boot = row.text(2); result.until = row.integer(3);
    }
    return result;
}

EventStoreLeaseTime checkedLeaseTime(const std::function<EventStoreLeaseTime()>& clock) {
    const auto time = clock();
    if (time.milliseconds < 0 || time.milliseconds > std::numeric_limits<std::int64_t>::max() - 30000 ||
        time.bootId.size() != 36 || !storeIdentifier(time.bootId)) {
        throw std::runtime_error("invalid event store lease clock");
    }
    return time;
}

bool liveBatch(const DeliveryBatch& batch, const DeliverySender& sender, const EventStoreLeaseTime& now) {
    return !batch.token.empty() && batch.epoch == sender.epoch && batch.boot == now.bootId && batch.until > now.milliseconds;
}

std::string deliveryJson(const DeliverySender& sender, const std::string& status, const std::string& extra = {}) {
    return "{\"ok\":true,\"senderId\":" + storeQuote(sender.id) + ",\"sessionId\":" + storeQuote(sender.session) +
        ",\"epoch\":\"" + std::to_string(sender.epoch) + "\",\"sequence\":\"" + std::to_string(sender.sequence) +
        "\",\"status\":" + storeQuote(status) + extra + "}";
}

std::string effectiveDeliveryReceipt(sqlite3* db, const DeliverySender& sender, const EventStoreLeaseTime& now) {
    if (sender.receipt.empty()) return deliveryJson(sender, sender.epoch ? "REGISTERED" : "UNREGISTERED");
    if (sender.operation == "ClaimBatch") {
        const auto receipt = json::JsonParser(sender.receipt, 16, 4096).parse();
        const auto* status = receipt.find("status");
        if (!status) throw std::runtime_error("invalid persisted claim receipt");
        if (status->asString() == "CLAIMED") {
            const auto batch = readBatch(db, sender.id);
            const auto* token = receipt.find("claimToken");
            if (!token || batch.token != token->asString()) return deliveryJson(sender, "REVOKED", ",\"messages\":[]");
            if (!liveBatch(batch, sender, now)) return deliveryJson(sender, "EXPIRED", ",\"messages\":[]");
            const auto* messages = receipt.find("messages");
            if (!messages || messages->asArray().values.empty()) throw std::runtime_error("empty persisted claim receipt");
            StoreStatement rows(db, "SELECT count(*) FROM event_store_claim_item i JOIN mqtt_event_outbox o "
                "ON o.id=i.row_id AND o.event_id=i.event_id AND o.target_id=i.target_id "
                "WHERE i.sender_id=? AND i.state='active' AND o.sent=0 AND o.claim_token=?;");
            rows.text(1, sender.id); rows.text(2, batch.token);
            if (!rows.row() || rows.integer(0) != static_cast<std::int64_t>(messages->asArray().values.size())) {
                return deliveryJson(sender, "REVOKED", ",\"messages\":[]");
            }
        }
    }
    return sender.receipt;
}

void clearSenderBatch(sqlite3* db, const std::string& sender) {
    const auto batch = readBatch(db, sender);
    if (!batch.token.empty()) {
        StoreStatement clear(db, "UPDATE mqtt_event_outbox SET claim_token=NULL,claim_until=NULL WHERE claim_token=? AND sent=0;");
        clear.text(1, batch.token); clear.done();
    }
    StoreStatement items(db, "DELETE FROM event_store_claim_item WHERE sender_id=?;");
    items.text(1, sender); items.done();
    StoreStatement metadata(db, "DELETE FROM event_store_claim_batch WHERE sender_id=?;");
    metadata.text(1, sender); metadata.done();
}

std::string deliveryIdentity(const EventStoreDeliveryRequest& request) {
    std::string result;
    const auto add = [&](const std::string& text) { result += std::to_string(text.size()) + ':' + text; };
    add(request.request); add(request.operation); add(request.senderId);
    add(std::to_string(request.epoch)); add(std::to_string(request.sequence));
    if (request.operation == "ClaimBatch") {
        add(std::to_string(request.limit)); add(std::to_string(request.maxBytes)); add(std::to_string(request.leaseMs));
    } else {
        add(request.claimToken); add(std::to_string(request.items.size()));
        for (const auto& item : request.items) { add(std::to_string(item.id)); add(item.eventId); }
    }
    return result;
}
} // namespace

const EventStoreSenderConfig& EventStoreDatabase::checkSender(const std::string& senderId) const {
    const auto found = std::find_if(senders_.begin(), senders_.end(), [&](const EventStoreSenderConfig& sender) {
        return sender.senderId == senderId;
    });
    if (found == senders_.end()) throw std::invalid_argument("sender is not configured");
    return *found;
}

std::string EventStoreDatabase::registerSender(const std::string& id, const std::string& session,
    std::int64_t expectedEpoch) {
    (void)checkSender(id);
    if (readOnly_) throw std::logic_error("read-only connection cannot register sender");
    if (!storeIdentifier(session) || expectedEpoch < 0) throw std::invalid_argument("invalid sender registration");
    auto* db = static_cast<sqlite3*>(outbox_.checkedDatabase());
    return writeTransaction([&] {
        auto prior = readSender(db, id);
        EventStoreProducer result;
        if (prior.session == session) {
            if (prior.epoch - 1 != expectedEpoch) throw EventStoreConflict("REGISTER_CONFLICT", "keep original expected epoch");
            result.receipt = effectiveDeliveryReceipt(db, prior, checkedLeaseTime(leaseClock_));
            return result;
        }
        if (prior.epoch != expectedEpoch || prior.epoch == std::numeric_limits<std::int64_t>::max()) {
            throw EventStoreConflict("STALE_EPOCH", "sender registration CAS failed");
        }
        clearSenderBatch(db, id);
        prior.session = session; ++prior.epoch; prior.sequence = 0;
        {
            StoreStatement row(db, "INSERT OR REPLACE INTO event_store_sender VALUES(?,?,?,0,'','','');");
            row.text(1, id); row.text(2, session); row.integer(3, prior.epoch); row.done();
        }
        result.receipt = deliveryJson(prior, "REGISTERED");
        return result;
    }).receipt;
}

std::string EventStoreDatabase::deliveryReceipt(const std::string& id) {
    (void)checkSender(id);
    if (readOnly_) throw std::logic_error("sender receipt requires the ordered writer connection");
    auto* db = static_cast<sqlite3*>(outbox_.checkedDatabase());
    return effectiveDeliveryReceipt(db, readSender(db, id), checkedLeaseTime(leaseClock_));
}

void EventStoreDatabase::setDeliveryResponseLimit(std::size_t maxBytes) {
    if (maxBytes < 4096 || maxBytes > 256 * 1024) throw std::invalid_argument("invalid delivery response limit");
    auto* db = static_cast<sqlite3*>(outbox_.checkedDatabase());
    StoreStatement rows(db, "SELECT COALESCE(MAX(length(CAST(receipt AS BLOB))),0) FROM event_store_sender;");
    if (!rows.row() || rows.integer(0) > static_cast<std::int64_t>(maxBytes)) {
        throw std::invalid_argument("persisted delivery receipt exceeds configured response limit");
    }
    deliveryResponseLimit_ = maxBytes;
}

std::string EventStoreDatabase::deliver(const EventStoreDeliveryRequest& request) {
    const auto& scope = checkSender(request.senderId);
    if (readOnly_) throw std::logic_error("read-only connection cannot deliver");
    if (request.epoch < 1 || request.sequence < 1) throw std::invalid_argument("invalid sender epoch/sequence");
    boundedStoreText(request.request, 256 * 1024, true);
    const bool claim = request.operation == "ClaimBatch";
    const bool ack = request.operation == "AckBatch";
    if (!claim && !ack && request.operation != "ReleaseBatch") throw std::invalid_argument("invalid delivery operation");
    if (claim) {
        if (request.limit < 1 || request.limit > 16 || request.maxBytes < 1 || request.maxBytes > 32768 ||
            request.leaseMs < 100 || request.leaseMs > 30000 || !request.items.empty() || !request.claimToken.empty()) {
            throw std::invalid_argument("invalid claim limits");
        }
    } else {
        boundedStoreText(request.claimToken, 256, true);
        if (request.items.empty() || request.items.size() > 16) throw std::invalid_argument("invalid finish item count");
        std::set<std::int64_t> ids;
        for (const auto& item : request.items) {
            boundedStoreText(item.eventId, 256, true);
            if (item.id < 1 || !ids.insert(item.id).second) throw std::invalid_argument("invalid/duplicate finish row id");
        }
    }
    const auto identity = deliveryIdentity(request);
    auto* db = static_cast<sqlite3*>(outbox_.checkedDatabase());
    return writeTransaction([&] {
        const auto now = checkedLeaseTime(leaseClock_);
        auto sender = readSender(db, request.senderId);
        if (sender.epoch != request.epoch) throw EventStoreConflict("STALE_EPOCH", "sender was replaced");
        EventStoreProducer result;
        if (sender.sequence == request.sequence) {
            if (sender.request != identity) throw EventStoreConflict("REQUEST_CONFLICT", "delivery sequence reused with different content");
            result.receipt = effectiveDeliveryReceipt(db, sender, now);
            return result;
        }
        if (sender.sequence == std::numeric_limits<std::int64_t>::max() || request.sequence != sender.sequence + 1) {
            throw EventStoreConflict("STALE_OR_GAPPED_SEQUENCE", "reconcile sender receipt");
        }
        auto batch = readBatch(db, sender.id);
        sender.sequence = request.sequence;
        if (claim) {
            if (liveBatch(batch, sender, now)) {
                StoreStatement active(db, "SELECT 1 FROM event_store_claim_item WHERE sender_id=? AND state='active' LIMIT 1;");
                active.text(1, sender.id);
                if (active.row()) throw EventStoreConflict("BATCH_IN_PROGRESS", "finish existing batch before new claim");
            }
            clearSenderBatch(db, sender.id);
            struct Candidate {
                std::int64_t id, ts, bytes;
                std::string eventId, type, topic, payload;
            };
            std::vector<Candidate> candidates;
            for (const auto& type : scope.eventTypes) {
                StoreStatement row(db, "SELECT o.id,o.event_id,o.event_type,o.event_ts,"
                    "length(CAST(o.topic AS BLOB))+length(CAST(o.payload AS BLOB)) "
                    "FROM mqtt_event_outbox o LEFT JOIN event_store_claim_item i ON i.row_id=o.id "
                    "LEFT JOIN event_store_claim_batch b ON b.sender_id=i.sender_id "
                    "WHERE o.target_id=? AND o.event_type=? AND o.sent=0 "
                    "AND (i.row_id IS NULL OR i.state!='active' OR b.boot_id!=? OR b.lease_until<=?) "
                    "AND (o.claim_token IS NULL OR o.claim_token=b.claim_token) "
                    "ORDER BY o.id LIMIT ?;");
                row.text(1, scope.targetId); row.text(2, type); row.text(3, now.bootId);
                row.integer(4, now.milliseconds); row.integer(5, static_cast<std::int64_t>(request.limit));
                while (row.row()) candidates.push_back({row.integer(0), row.integer(3), row.integer(4), row.text(1), row.text(2), {}, {}});
            }
            std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
                const auto left = replayEventTypePriority(a.type), right = replayEventTypePriority(b.type);
                if (left != right) return left < right;
                // Preserve submission order within each priority, including after wall-clock rollback.
                return a.id < b.id;
            });
            std::size_t bytes = 0, count = 0;
            for (; count < candidates.size() && count < request.limit; ++count) {
                const auto size = candidates[count].bytes;
                if (size < 0) throw std::runtime_error("invalid event byte length");
                if (static_cast<std::uint64_t>(size) > request.maxBytes - bytes) {
                    if (count == 0) throw std::invalid_argument("ROW_TOO_LARGE: head event exceeds claim byte budget");
                    break;
                }
                bytes += static_cast<std::size_t>(size);
            }
            candidates.resize(count);
            // Only materialize payloads after the aggregate byte budget is checked.
            for (auto& candidate : candidates) {
                StoreStatement row(db, "SELECT topic,payload FROM mqtt_event_outbox WHERE id=?;");
                row.integer(1, candidate.id);
                if (!row.row()) throw std::runtime_error("claim candidate disappeared inside transaction");
                candidate.topic = row.text(0); candidate.payload = row.text(1);
                if (candidate.topic.size() + candidate.payload.size() != static_cast<std::size_t>(candidate.bytes)) {
                    throw std::runtime_error("claim candidate byte length changed");
                }
            }
            if (candidates.empty()) result.receipt = deliveryJson(sender, "EMPTY", ",\"messages\":[]");
            else {
                batch.token = std::to_string(sender.id.size()) + ':' + sender.id + ':' + std::to_string(sender.epoch) + ':' + std::to_string(sender.sequence);
                batch.boot = now.bootId; batch.until = now.milliseconds + request.leaseMs; batch.epoch = sender.epoch;
                {
                    StoreStatement add(db, "INSERT INTO event_store_claim_batch VALUES(?,?,?,?,?);");
                    add.text(1, sender.id); add.integer(2, sender.epoch); add.text(3, batch.token);
                    add.text(4, batch.boot); add.integer(5, batch.until); add.done();
                }
                std::string messages;
                for (const auto& row : candidates) {
                    {
                        StoreStatement forget(db, "DELETE FROM event_store_claim_item WHERE row_id=?;");
                        forget.integer(1, row.id); forget.done();
                        StoreStatement item(db, "INSERT INTO event_store_claim_item VALUES(?,?,?,?,'active');");
                        item.text(1, sender.id); item.integer(2, row.id); item.text(3, row.eventId); item.text(4, scope.targetId); item.done();
                        StoreStatement mark(db, "UPDATE mqtt_event_outbox SET claim_token=?,claim_until=? WHERE id=? AND sent=0;");
                        mark.text(1, batch.token); mark.integer(2, std::numeric_limits<std::int64_t>::max()); mark.integer(3, row.id); mark.done();
                        if (g_changes(db) != 1) throw std::runtime_error("claim row changed inside writer transaction");
                    }
                    if (!messages.empty()) messages += ',';
                    messages += "{\"id\":\"" + std::to_string(row.id) + "\",\"eventId\":" + storeQuote(row.eventId) +
                        ",\"eventType\":" + storeQuote(row.type) + ",\"topic\":" + storeQuote(row.topic) +
                        ",\"payload\":" + storeQuote(row.payload) + ",\"eventTs\":\"" + std::to_string(row.ts) + "\"}";
                }
                result.receipt = deliveryJson(sender, "CLAIMED", ",\"claimToken\":" + storeQuote(batch.token) +
                    ",\"bootId\":" + storeQuote(batch.boot) + ",\"leaseUntilMs\":\"" + std::to_string(batch.until) +
                    "\",\"messages\":[" + messages + "]");
            }
        } else {
            std::string statuses;
            for (const auto& item : request.items) {
                std::string status;
                if (batch.token != request.claimToken || batch.epoch != sender.epoch) status = "REVOKED";
                else {
                    StoreStatement row(db, "SELECT i.state,o.sent,o.claim_token FROM event_store_claim_item i "
                        "JOIN mqtt_event_outbox o ON o.id=i.row_id WHERE i.sender_id=? AND i.row_id=? "
                        "AND i.event_id=? AND i.target_id=? AND o.event_id=i.event_id AND o.target_id=i.target_id;");
                    row.text(1, sender.id); row.integer(2, item.id); row.text(3, item.eventId); row.text(4, scope.targetId);
                    if (!row.row()) status = "NOT_CLAIMED";
                    else if (row.text(0) == "acked") status = "ALREADY_ACKED";
                    else if (row.text(0) == "released") status = "ALREADY_RELEASED";
                    else if (!liveBatch(batch, sender, now)) status = "EXPIRED";
                    else if (row.integer(1) != 0 || row.text(2) != batch.token) status = "REVOKED";
                    else status = "APPLIED";
                }
                if (status == "APPLIED") {
                    StoreStatement mark(db, ack ?
                        "UPDATE mqtt_event_outbox SET sent=1,sent_at=?,claim_token=NULL,claim_until=NULL WHERE id=? AND event_id=? AND target_id=? AND claim_token=? AND sent=0;" :
                        "UPDATE mqtt_event_outbox SET claim_token=NULL,claim_until=NULL WHERE ? >= 0 AND id=? AND event_id=? AND target_id=? AND claim_token=? AND sent=0;");
                    mark.integer(1, ack ? currentTimeMs() : 1); mark.integer(2, item.id); mark.text(3, item.eventId);
                    mark.text(4, scope.targetId); mark.text(5, batch.token); mark.done();
                    if (g_changes(db) != 1) throw std::runtime_error("finish row changed inside writer transaction");
                    StoreStatement done(db, "UPDATE event_store_claim_item SET state=? WHERE sender_id=? AND row_id=?;");
                    const std::string completedState = ack ? "acked" : "released";
                    done.text(1, completedState); done.text(2, sender.id); done.integer(3, item.id); done.done();
                }
                if (!statuses.empty()) statuses += ',';
                statuses += "{\"id\":\"" + std::to_string(item.id) + "\",\"eventId\":" + storeQuote(item.eventId) + ",\"status\":" + storeQuote(status) + "}";
            }
            result.receipt = deliveryJson(sender, "FINISHED", ",\"results\":[" + statuses + "]");
        }
        if (result.receipt.size() > deliveryResponseLimit_) throw std::invalid_argument("delivery receipt exceeds frame bound");
        {
            StoreStatement save(db, "UPDATE event_store_sender SET sequence=?,request=?,receipt=?,last_op=? WHERE sender_id=? AND epoch=?;");
            save.integer(1, sender.sequence); save.text(2, identity); save.text(3, result.receipt); save.text(4, request.operation);
            save.text(5, sender.id); save.integer(6, sender.epoch); save.done();
            if (g_changes(db) != 1) throw std::runtime_error("sender disappeared inside writer transaction");
        }
        return result;
    }).receipt;
}

}  // namespace edge_gateway
