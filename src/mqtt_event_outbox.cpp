#include "edge_gateway/mqtt_event_outbox.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
using sqlite3_bind_text_fn = int (*)(sqlite3_stmt*, int, const char*, int, void (*)(void*));
using sqlite3_step_fn = int (*)(sqlite3_stmt*);
using sqlite3_reset_fn = int (*)(sqlite3_stmt*);
using sqlite3_clear_bindings_fn = int (*)(sqlite3_stmt*);
using sqlite3_finalize_fn = int (*)(sqlite3_stmt*);
using sqlite3_errmsg_fn = const char* (*)(sqlite3*);
using sqlite3_free_fn = void (*)(void*);
using sqlite3_last_insert_rowid_fn = long long (*)(sqlite3*);
using sqlite3_column_int64_fn = long long (*)(sqlite3_stmt*, int);
using sqlite3_column_int_fn = int (*)(sqlite3_stmt*, int);
using sqlite3_column_text_fn = const unsigned char* (*)(sqlite3_stmt*, int);

constexpr int kSqliteOk = 0;
constexpr int kSqliteBusy = 5;
constexpr int kSqliteIoErr = 10;
constexpr int kSqliteRow = 100;
constexpr int kSqliteDone = 101;
constexpr int kSqliteOpenReadWrite = 0x00000002;
constexpr int kSqliteOpenCreate = 0x00000004;

sqlite3_open_v2_fn g_open = nullptr;
sqlite3_close_v2_fn g_close = nullptr;
sqlite3_exec_fn g_exec = nullptr;
sqlite3_prepare_v2_fn g_prepare = nullptr;
sqlite3_busy_timeout_fn g_busy_timeout = nullptr;
sqlite3_bind_int_fn g_bind_int = nullptr;
sqlite3_bind_int64_fn g_bind_int64 = nullptr;
sqlite3_bind_text_fn g_bind_text = nullptr;
sqlite3_step_fn g_step = nullptr;
sqlite3_reset_fn g_reset = nullptr;
sqlite3_clear_bindings_fn g_clear_bindings = nullptr;
sqlite3_finalize_fn g_finalize = nullptr;
sqlite3_errmsg_fn g_errmsg = nullptr;
sqlite3_free_fn g_free = nullptr;
sqlite3_last_insert_rowid_fn g_last_insert_rowid = nullptr;
sqlite3_column_int64_fn g_column_int64 = nullptr;
sqlite3_column_int_fn g_column_int = nullptr;
sqlite3_column_text_fn g_column_text = nullptr;

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

void execOrThrow(sqlite3* db, const char* sql) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        char* errorMessage = nullptr;
        const auto rc = g_exec(db, sql, nullptr, nullptr, &errorMessage);
        if (rc == kSqliteOk) {
            return;
        }
        std::string message = errorMessage != nullptr ? errorMessage : sqliteError(db);
        if (errorMessage != nullptr && g_free != nullptr) {
            g_free(errorMessage);
        }
        if (rc == kSqliteBusy || rc == kSqliteIoErr) {
            continue;
        }
        throw std::runtime_error(message);
    }
    throw std::runtime_error("sqlite busy timeout");
}

int prepareWithRetry(sqlite3* db, const char* sql, sqlite3_stmt** stmt) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        const auto rc = g_prepare(db, sql, -1, stmt, nullptr);
        if (rc == kSqliteOk) {
            return rc;
        }
        if (rc != kSqliteBusy && rc != kSqliteIoErr) {
            return rc;
        }
    }
    return kSqliteBusy;
}

int stepWithRetry(sqlite3_stmt* stmt) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        const auto rc = g_step(stmt);
        if (rc == kSqliteDone || rc == kSqliteRow) {
            return rc;
        }
        if (rc != kSqliteBusy && rc != kSqliteIoErr) {
            return rc;
        }
    }
    return kSqliteBusy;
}

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::string columnText(sqlite3_stmt* stmt, int index) {
    const auto* text = g_column_text(stmt, index);
    return text == nullptr ? std::string() : reinterpret_cast<const char*>(text);
}

}  // namespace

MqttEventOutbox::MqttEventOutbox(
    std::string dbPath,
    std::string libraryPath,
    int retentionMonths,
    int cleanupIntervalHours,
    std::size_t replayBatchSize,
    std::size_t maxDiskBytes
) : dbPath_(std::move(dbPath)),
    libraryPath_(std::move(libraryPath)),
    retentionMonths_(retentionMonths <= 0 ? 12 : retentionMonths),
    cleanupIntervalHours_(cleanupIntervalHours <= 0 ? 24 : cleanupIntervalHours),
    replayBatchSize_(replayBatchSize == 0 ? 100 : replayBatchSize),
    maxDiskBytes_(maxDiskBytes) {
    loadLibrary();
    openDatabase();
    ensureSchema();
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
    std::vector<std::int64_t> ids;
    if (events.empty()) {
        return ids;
    }

    auto* db = static_cast<sqlite3*>(databaseHandle_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO mqtt_event_outbox(event_type, topic, payload, event_ts, event_month, created_at, sent) "
        "VALUES(?, ?, ?, ?, ?, ?, 0);";

    ids.reserve(events.size());
    try {
        execOrThrow(db, "BEGIN IMMEDIATE;");
        enforceDiskLimit();
        if (prepareWithRetry(db, sql, &stmt) != kSqliteOk) {
            throw std::runtime_error(sqliteError(db));
        }

        const auto createdAt = currentTimeMs();
        for (const auto& event : events) {
            if (event.topic.empty()) {
                continue;
            }

            const auto ts = event.eventTs > 0 ? event.eventTs : createdAt;
            const auto month = eventMonth(ts);
            if (g_bind_text(stmt, 1, event.eventType.c_str(), -1, nullptr) != kSqliteOk ||
                g_bind_text(stmt, 2, event.topic.c_str(), -1, nullptr) != kSqliteOk ||
                g_bind_text(stmt, 3, event.payload.c_str(), -1, nullptr) != kSqliteOk ||
                g_bind_int64(stmt, 4, static_cast<long long>(ts)) != kSqliteOk ||
                g_bind_text(stmt, 5, month.c_str(), -1, nullptr) != kSqliteOk ||
                g_bind_int64(stmt, 6, static_cast<long long>(createdAt)) != kSqliteOk ||
                stepWithRetry(stmt) != kSqliteDone) {
                throw std::runtime_error(sqliteError(db));
            }

            ids.push_back(static_cast<std::int64_t>(g_last_insert_rowid(db)));
            if (g_reset(stmt) != kSqliteOk || g_clear_bindings(stmt) != kSqliteOk) {
                throw std::runtime_error(sqliteError(db));
            }
        }

        g_finalize(stmt);
        stmt = nullptr;
        enforceDiskLimit();
        execOrThrow(db, "COMMIT;");
    } catch (...) {
        if (stmt != nullptr) {
            g_finalize(stmt);
        }
        try {
            execOrThrow(db, "ROLLBACK;");
        } catch (...) {
        }
        throw;
    }
    return ids;
}

void MqttEventOutbox::markSent(std::int64_t id, std::int64_t sentAt) {
    auto* db = static_cast<sqlite3*>(databaseHandle_);
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

    auto* db = static_cast<sqlite3*>(databaseHandle_);
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
        try {
            execOrThrow(db, "ROLLBACK;");
        } catch (...) {
        }
        throw;
    }
}

std::size_t MqttEventOutbox::pendingCount() {
    auto* db = static_cast<sqlite3*>(databaseHandle_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT COUNT(*) FROM mqtt_event_outbox WHERE sent=0;";
    if (prepareWithRetry(db, sql, &stmt) != kSqliteOk) {
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
    return replayWithStats(0, send).count;
}

std::size_t MqttEventOutbox::replay(
    std::size_t maxBytes,
    const std::function<void(const std::string&, const std::string&)>& send
) {
    return replayWithStats(maxBytes, send).count;
}

MqttEventOutbox::ReplayStats MqttEventOutbox::replayWithStats(
    const std::function<void(const std::string&, const std::string&)>& send
) {
    return replayWithStats(0, send);
}

MqttEventOutbox::ReplayStats MqttEventOutbox::replayWithStats(
    std::size_t maxBytes,
    const std::function<void(const std::string&, const std::string&)>& send
) {
    struct Row {
        std::int64_t id = 0;
        std::string eventType;
        std::string topic;
        std::string payload;
    };
    std::vector<Row> rows;
    auto* db = static_cast<sqlite3*>(databaseHandle_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, event_type, topic, payload FROM mqtt_event_outbox "
        "WHERE sent=0 "
        "ORDER BY CASE event_type WHEN 'alarm' THEN 0 WHEN 'change' THEN 1 ELSE 2 END, event_ts ASC, id ASC LIMIT ?;";
    if (prepareWithRetry(db, sql, &stmt) != kSqliteOk) {
        throw std::runtime_error(sqliteError(db));
    }
    g_bind_int(stmt, 1, static_cast<int>(replayBatchSize_));
    while (stepWithRetry(stmt) == kSqliteRow) {
        rows.push_back(Row{
            static_cast<std::int64_t>(g_column_int64(stmt, 0)),
            columnText(stmt, 1),
            columnText(stmt, 2),
            columnText(stmt, 3)
        });
    }
    g_finalize(stmt);
    if (rows.empty()) {
        return ReplayStats();
    }

    std::vector<std::int64_t> sentIds;
    sentIds.reserve(rows.size());
    ReplayStats stats;
    for (const auto& row : rows) {
        const std::size_t rowBytes = row.topic.size() + row.payload.size();
        if (maxBytes > 0 && !sentIds.empty() && stats.bytes + rowBytes > maxBytes) {
            break;
        }
        send(row.topic, row.payload);
        sentIds.push_back(row.id);
        stats.count += 1;
        stats.bytes += rowBytes;
        if (row.eventType == "alarm") {
            stats.alarmCount += 1;
        } else if (row.eventType == "change") {
            stats.changeCount += 1;
        } else {
            stats.otherCount += 1;
        }
    }
    if (sentIds.empty()) {
        return ReplayStats();
    }
    markSentBatch(sentIds, currentTimeMs());
    return stats;
}

void MqttEventOutbox::cleanupIfDue(std::int64_t nowMs) {
    if (lastCleanupMs_ > 0 &&
        nowMs - lastCleanupMs_ < static_cast<std::int64_t>(cleanupIntervalHours_) * 60 * 60 * 1000) {
        return;
    }
    lastCleanupMs_ = nowMs;
    const auto beforeMonth = cleanupBeforeMonth(nowMs);
    auto* db = static_cast<sqlite3*>(databaseHandle_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM mqtt_event_outbox WHERE sent=1 AND event_month < ?;";
    if (g_prepare(db, sql, -1, &stmt, nullptr) != kSqliteOk) {
        return;
    }
    g_bind_text(stmt, 1, beforeMonth.c_str(), -1, nullptr);
    g_step(stmt);
    g_finalize(stmt);
}

void MqttEventOutbox::loadLibrary() {
    if (libraryHandle_ != nullptr) {
        return;
    }
#ifdef _WIN32
    libraryHandle_ = !libraryPath_.empty() ? LoadLibraryA(libraryPath_.c_str()) : nullptr;
    if (libraryHandle_ == nullptr) libraryHandle_ = LoadLibraryA("sqlite3.dll");
#else
    libraryHandle_ = !libraryPath_.empty() ? dlopen(libraryPath_.c_str(), RTLD_NOW | RTLD_LOCAL) : nullptr;
    if (libraryHandle_ == nullptr) libraryHandle_ = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (libraryHandle_ == nullptr) libraryHandle_ = dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
#endif
    if (libraryHandle_ == nullptr) {
        throw std::runtime_error("failed to load sqlite3 library");
    }
    g_open = reinterpret_cast<sqlite3_open_v2_fn>(loadSymbol(libraryHandle_, "sqlite3_open_v2"));
    g_close = reinterpret_cast<sqlite3_close_v2_fn>(loadSymbol(libraryHandle_, "sqlite3_close_v2"));
    g_exec = reinterpret_cast<sqlite3_exec_fn>(loadSymbol(libraryHandle_, "sqlite3_exec"));
    g_prepare = reinterpret_cast<sqlite3_prepare_v2_fn>(loadSymbol(libraryHandle_, "sqlite3_prepare_v2"));
    g_busy_timeout = reinterpret_cast<sqlite3_busy_timeout_fn>(loadSymbol(libraryHandle_, "sqlite3_busy_timeout"));
    g_bind_int = reinterpret_cast<sqlite3_bind_int_fn>(loadSymbol(libraryHandle_, "sqlite3_bind_int"));
    g_bind_int64 = reinterpret_cast<sqlite3_bind_int64_fn>(loadSymbol(libraryHandle_, "sqlite3_bind_int64"));
    g_bind_text = reinterpret_cast<sqlite3_bind_text_fn>(loadSymbol(libraryHandle_, "sqlite3_bind_text"));
    g_step = reinterpret_cast<sqlite3_step_fn>(loadSymbol(libraryHandle_, "sqlite3_step"));
    g_reset = reinterpret_cast<sqlite3_reset_fn>(loadSymbol(libraryHandle_, "sqlite3_reset"));
    g_clear_bindings = reinterpret_cast<sqlite3_clear_bindings_fn>(loadSymbol(libraryHandle_, "sqlite3_clear_bindings"));
    g_finalize = reinterpret_cast<sqlite3_finalize_fn>(loadSymbol(libraryHandle_, "sqlite3_finalize"));
    g_errmsg = reinterpret_cast<sqlite3_errmsg_fn>(loadSymbol(libraryHandle_, "sqlite3_errmsg"));
    g_free = reinterpret_cast<sqlite3_free_fn>(loadSymbol(libraryHandle_, "sqlite3_free"));
    g_last_insert_rowid = reinterpret_cast<sqlite3_last_insert_rowid_fn>(loadSymbol(libraryHandle_, "sqlite3_last_insert_rowid"));
    g_column_int64 = reinterpret_cast<sqlite3_column_int64_fn>(loadSymbol(libraryHandle_, "sqlite3_column_int64"));
    g_column_int = reinterpret_cast<sqlite3_column_int_fn>(loadSymbol(libraryHandle_, "sqlite3_column_int"));
    g_column_text = reinterpret_cast<sqlite3_column_text_fn>(loadSymbol(libraryHandle_, "sqlite3_column_text"));
}

void MqttEventOutbox::openDatabase() {
    sqlite3* db = nullptr;
    const auto rc = g_open(dbPath_.c_str(), &db, kSqliteOpenReadWrite | kSqliteOpenCreate, nullptr);
    if (rc != kSqliteOk) {
        throw std::runtime_error("failed to open mqtt event outbox database");
    }
    if (g_busy_timeout != nullptr) {
        g_busy_timeout(db, 5000);
    }
    databaseHandle_ = db;
}

void MqttEventOutbox::ensureSchema() {
    auto* db = static_cast<sqlite3*>(databaseHandle_);
    execOrThrow(db, "PRAGMA journal_mode=DELETE;");
    execOrThrow(db, "PRAGMA synchronous=NORMAL;");
    execOrThrow(
        db,
        "CREATE TABLE IF NOT EXISTS mqtt_event_outbox ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
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
    execOrThrow(db, "CREATE INDEX IF NOT EXISTS idx_mqtt_event_outbox_pending ON mqtt_event_outbox(sent, event_ts, id);");
    execOrThrow(db, "CREATE INDEX IF NOT EXISTS idx_mqtt_event_outbox_cleanup ON mqtt_event_outbox(sent, event_month);");
}

std::size_t MqttEventOutbox::pendingBytes() {
    auto* db = static_cast<sqlite3*>(databaseHandle_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT COALESCE(SUM(length(topic) + length(payload)), 0) FROM mqtt_event_outbox WHERE sent=0;";
    if (prepareWithRetry(db, sql, &stmt) != kSqliteOk) {
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

std::size_t MqttEventOutbox::prunePendingRows(std::size_t targetBytes) {
    auto* db = static_cast<sqlite3*>(databaseHandle_);
    const auto currentBytes = pendingBytes();
    if (currentBytes <= targetBytes) {
        return 0;
    }

    const auto bytesToRemove = currentBytes - targetBytes;
    std::vector<std::int64_t> ids;
    std::size_t selectedBytes = 0;
    sqlite3_stmt* stmt = nullptr;
    const char* selectSql =
        "SELECT id, length(topic) + length(payload) FROM mqtt_event_outbox WHERE sent=0 "
        "ORDER BY CASE event_type WHEN 'alarm' THEN 1 ELSE 0 END, event_ts ASC, id ASC;";
    if (prepareWithRetry(db, selectSql, &stmt) != kSqliteOk) {
        throw std::runtime_error(sqliteError(db));
    }
    while (stepWithRetry(stmt) == kSqliteRow) {
        ids.push_back(static_cast<std::int64_t>(g_column_int64(stmt, 0)));
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
        const char* deleteSql = "DELETE FROM mqtt_event_outbox WHERE id=?;";
        if (prepareWithRetry(db, deleteSql, &stmt) != kSqliteOk) {
            throw std::runtime_error(sqliteError(db));
        }
        if (g_bind_int64(stmt, 1, static_cast<long long>(id)) != kSqliteOk ||
            stepWithRetry(stmt) != kSqliteDone) {
            g_finalize(stmt);
            throw std::runtime_error(sqliteError(db));
        }
        g_finalize(stmt);
        ++removed;
    }
    return removed;
}

void MqttEventOutbox::enforceDiskLimit() {
    if (maxDiskBytes_ == 0) {
        return;
    }
    const auto targetBytes = maxDiskBytes_ > 4096 ? maxDiskBytes_ - 4096 : maxDiskBytes_;
    (void)prunePendingRows(targetBytes);
}

void MqttEventOutbox::closeDatabase() {
    if (databaseHandle_ != nullptr) {
        g_close(static_cast<sqlite3*>(databaseHandle_));
        databaseHandle_ = nullptr;
    }
}

void MqttEventOutbox::unloadLibrary() {
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

std::string MqttEventOutbox::cleanupBeforeMonth(std::int64_t nowMs) const {
    const std::time_t sec = static_cast<std::time_t>(nowMs / 1000);
    std::tm tm {};
#ifdef _WIN32
    gmtime_s(&tm, &sec);
#else
    gmtime_r(&sec, &tm);
#endif
    int year = tm.tm_year + 1900;
    int month = tm.tm_mon + 1 - retentionMonths_;
    while (month <= 0) {
        month += 12;
        --year;
    }
    std::ostringstream out;
    out << year << "-";
    if (month < 10) out << "0";
    out << month;
    return out.str();
}

}  // namespace edge_gateway
