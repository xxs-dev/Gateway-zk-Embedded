#include "edge_gateway/mqtt_event_outbox.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

struct sqlite3;
struct sqlite3_stmt;

using sqlite3_open_v2_fn = int (*)(const char*, sqlite3**, int, const char*);
using sqlite3_close_v2_fn = int (*)(sqlite3*);
using sqlite3_exec_fn = int (*)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
using sqlite3_free_fn = void (*)(void*);
using sqlite3_prepare_v2_fn = int (*)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
using sqlite3_step_fn = int (*)(sqlite3_stmt*);
using sqlite3_finalize_fn = int (*)(sqlite3_stmt*);
using sqlite3_column_int64_fn = long long (*)(sqlite3_stmt*, int);

constexpr int kSqliteOk = 0;
constexpr int kSqliteRow = 100;
constexpr int kSqliteOpenReadWrite = 0x00000002;
constexpr int kSqliteOpenCreate = 0x00000004;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class LegacyDatabaseBuilder {
public:
    LegacyDatabaseBuilder(const std::string& path, const std::string& libraryPath) {
#ifdef _WIN32
        handle_ = !libraryPath.empty() ? LoadLibraryA(libraryPath.c_str()) : nullptr;
        if (handle_ == nullptr) {
            handle_ = LoadLibraryA("sqlite3.dll");
        }
#else
        handle_ = !libraryPath.empty() ? dlopen(libraryPath.c_str(), RTLD_NOW | RTLD_LOCAL) : nullptr;
        if (handle_ == nullptr) {
            handle_ = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
        }
        if (handle_ == nullptr) {
            handle_ = dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
        }
#endif
        if (handle_ == nullptr) {
            throw std::runtime_error("failed to load sqlite3 library for legacy schema test");
        }
        open_ = reinterpret_cast<sqlite3_open_v2_fn>(loadSymbol("sqlite3_open_v2"));
        close_ = reinterpret_cast<sqlite3_close_v2_fn>(loadSymbol("sqlite3_close_v2"));
        exec_ = reinterpret_cast<sqlite3_exec_fn>(loadSymbol("sqlite3_exec"));
        free_ = reinterpret_cast<sqlite3_free_fn>(loadSymbol("sqlite3_free"));
        prepare_ = reinterpret_cast<sqlite3_prepare_v2_fn>(loadSymbol("sqlite3_prepare_v2"));
        step_ = reinterpret_cast<sqlite3_step_fn>(loadSymbol("sqlite3_step"));
        finalize_ = reinterpret_cast<sqlite3_finalize_fn>(loadSymbol("sqlite3_finalize"));
        columnInt64_ = reinterpret_cast<sqlite3_column_int64_fn>(loadSymbol("sqlite3_column_int64"));
        if (open_(path.c_str(), &db_, kSqliteOpenReadWrite | kSqliteOpenCreate, nullptr) != kSqliteOk) {
            throw std::runtime_error("failed to create legacy sqlite database");
        }
    }

    ~LegacyDatabaseBuilder() {
        if (db_ != nullptr) {
            close_(db_);
        }
#ifdef _WIN32
        if (handle_ != nullptr) {
            FreeLibrary(static_cast<HMODULE>(handle_));
        }
#else
        if (handle_ != nullptr) {
            dlclose(handle_);
        }
#endif
    }

    void exec(const char* sql) {
        char* error = nullptr;
        const auto rc = exec_(db_, sql, nullptr, nullptr, &error);
        if (rc == kSqliteOk) {
            return;
        }
        const std::string message = error == nullptr ? "legacy sqlite exec failed" : error;
        if (error != nullptr) {
            free_(error);
        }
        throw std::runtime_error(message);
    }

    std::int64_t scalarInt64(const char* sql) {
        sqlite3_stmt* stmt = nullptr;
        if (prepare_(db_, sql, -1, &stmt, nullptr) != kSqliteOk || stmt == nullptr) {
            throw std::runtime_error("failed to prepare sqlite scalar query");
        }
        const auto rc = step_(stmt);
        if (rc != kSqliteRow) {
            finalize_(stmt);
            throw std::runtime_error("sqlite scalar query returned no row");
        }
        const auto value = static_cast<std::int64_t>(columnInt64_(stmt, 0));
        finalize_(stmt);
        return value;
    }

private:
    void* loadSymbol(const char* name) {
#ifdef _WIN32
        auto* symbol = reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
        auto* symbol = dlsym(handle_, name);
#endif
        if (symbol == nullptr) {
            throw std::runtime_error(std::string("missing sqlite symbol: ") + name);
        }
        return symbol;
    }

    void* handle_ = nullptr;
    sqlite3* db_ = nullptr;
    sqlite3_open_v2_fn open_ = nullptr;
    sqlite3_close_v2_fn close_ = nullptr;
    sqlite3_exec_fn exec_ = nullptr;
    sqlite3_free_fn free_ = nullptr;
    sqlite3_prepare_v2_fn prepare_ = nullptr;
    sqlite3_step_fn step_ = nullptr;
    sqlite3_finalize_fn finalize_ = nullptr;
    sqlite3_column_int64_fn columnInt64_ = nullptr;
};

std::string testDatabasePath(const std::string& name) {
#ifdef _WIN32
    const auto processId = static_cast<unsigned long long>(GetCurrentProcessId());
#else
    const auto processId = static_cast<unsigned long long>(getpid());
#endif
    const auto nonce = static_cast<unsigned long long>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()
    );
    const char* configuredTemp = std::getenv(
#ifdef _WIN32
        "TEMP"
#else
        "TMPDIR"
#endif
    );
    std::string tempDirectory = configuredTemp == nullptr || *configuredTemp == '\0'
        ? std::string("/tmp")
        : std::string(configuredTemp);
    if (!tempDirectory.empty() && tempDirectory.back() != '/' && tempDirectory.back() != '\\') {
#ifdef _WIN32
        tempDirectory.push_back('\\');
#else
        tempDirectory.push_back('/');
#endif
    }
    return tempDirectory + "gateway_mqtt_outbox_" + name + "_" +
        std::to_string(processId) + "_" + std::to_string(nonce) + ".db";
}

void removeDatabase(const std::string& path) {
    (void)std::remove(path.c_str());
    (void)std::remove((path + "-shm").c_str());
    (void)std::remove((path + "-wal").c_str());
    (void)std::remove((path + "-journal").c_str());
}

std::unique_ptr<edge_gateway::MqttEventOutbox> makeOutbox(
    const std::string& path,
    const std::string& libraryPath,
    std::size_t maxDiskBytes = 0,
    std::size_t replayBatchSize = 100
) {
    return std::unique_ptr<edge_gateway::MqttEventOutbox>(new edge_gateway::MqttEventOutbox(
        path, libraryPath, 12, 24, replayBatchSize, maxDiskBytes
    ));
}

edge_gateway::MqttEventOutbox::EventMessage event(
    std::string eventId,
    std::string targetId,
    std::string eventType,
    std::string topic,
    std::string payload,
    std::int64_t eventTs
) {
    edge_gateway::MqttEventOutbox::EventMessage result;
    result.eventId = std::move(eventId);
    result.targetId = std::move(targetId);
    result.eventType = std::move(eventType);
    result.topic = std::move(topic);
    result.payload = std::move(payload);
    result.eventTs = eventTs;
    return result;
}

edge_gateway::MqttEventOutbox::EventTypeFilter included(std::vector<std::string> types) {
    edge_gateway::MqttEventOutbox::EventTypeFilter filter;
    filter.include = std::move(types);
    return filter;
}

edge_gateway::MqttEventOutbox::EventTypeFilter excluded(std::vector<std::string> types) {
    edge_gateway::MqttEventOutbox::EventTypeFilter filter;
    filter.exclude = std::move(types);
    return filter;
}

void testStorageProfiles(const std::string& libraryPath) {
    using Outbox = edge_gateway::MqttEventOutbox;
    const auto legacyPath = testDatabasePath("default_profile");
    {
        auto box = makeOutbox(legacyPath, libraryPath);
        const auto settings = box->storageSettings();
        require(settings.journalMode == "delete" && settings.synchronous == 1,
            "default storage profile changed");
    }
    removeDatabase(legacyPath);
    for (const auto policy : {Outbox::StorageProfile::DeleteNormal, Outbox::StorageProfile::DeleteFull,
                             Outbox::StorageProfile::WalNormal, Outbox::StorageProfile::WalFull}) {
        const auto path = testDatabasePath("explicit_profile");
        const bool wal = policy == Outbox::StorageProfile::WalNormal || policy == Outbox::StorageProfile::WalFull;
        const bool full = policy == Outbox::StorageProfile::DeleteFull || policy == Outbox::StorageProfile::WalFull;
        {
            Outbox box(path, libraryPath, 12, 24, 100, 0, policy);
            const auto settings = box.storageSettings();
            require(settings.journalMode == (wal ? "wal" : "delete") && settings.synchronous == (full ? 2 : 1),
                "explicit storage profile not applied to actual connection");
            require(!settings.sqliteVersion.empty() && settings.busyTimeoutMs == 25,
                "invalid live storage diagnostics");
            require(box.enqueue("alarm", "test", "payload", 1000) > 0, "profile enqueue failed");
        }
        {
            Outbox box(path, libraryPath, 12, 24, 100, 0, policy);
            require(box.pendingCount() == 1, "profile reopen lost event");
            require(box.replay([](const std::string&, const std::string&) {}) == 1,
                "profile replay failed");
            require(box.pendingCount() == 0, "profile ACK failed");
        }
        removeDatabase(path);
    }
}

void testLegacySchemaMigration(const std::string& libraryPath) {
    const auto path = testDatabasePath("legacy");
    removeDatabase(path);
    {
        LegacyDatabaseBuilder legacy(path, libraryPath);
        legacy.exec(
            "CREATE TABLE mqtt_event_outbox ("
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
            "last_error TEXT);"
        );
        legacy.exec(
            "INSERT INTO mqtt_event_outbox("
            "event_type,topic,payload,event_ts,event_month,created_at,sent) "
            "VALUES('change','legacy/topic','legacy-payload',1000,'1970-01',1000,0);"
        );
    }

    {
        auto outbox = makeOutbox(path, libraryPath);
        require(outbox->pendingCount() == 1, "legacy row was not migrated to the main target");
        require(outbox->pendingCount("third") == 0, "legacy row leaked into another target");

        edge_gateway::MqttEventOutbox::EventMessage legacyAggregate{
            "ota_status", "aggregate/topic", "aggregate-payload", 1500
        };
        require(legacyAggregate.targetId == "main",
            "legacy EventMessage aggregate did not retain the main target default");
        require(outbox->enqueueBatch({legacyAggregate}).size() == 1,
            "legacy EventMessage aggregate call was not accepted");

        const auto ids = outbox->enqueueBatch({
            event("migrated-third", "third", "change", "third/topic", "third-payload", 2000)
        });
        require(ids.size() == 1, "migrated schema did not accept event_id and target_id");

        std::vector<std::string> replayed;
        require(outbox->replay([&](const std::string& topic, const std::string&) {
            replayed.push_back(topic);
        }) == 2, "legacy API did not replay all migrated/default main rows");
        require(replayed.size() == 2 && replayed.front() == "legacy/topic" &&
                replayed.back() == "aggregate/topic",
            "legacy API replayed a non-main target");
        require(outbox->pendingCount() == 0, "legacy main row was not acknowledged");
        require(outbox->pendingCount("third") == 1, "main replay acknowledged the third target");
    }
    removeDatabase(path);
}

void testCompletedMigrationDoesNotRewriteRows(const std::string& libraryPath) {
    const auto path = testDatabasePath("migration_idempotent");
    removeDatabase(path);
    {
        auto outbox = makeOutbox(path, libraryPath);
        require(outbox->enqueueBatch({
            event("stable-row", "main", "change", "stable/topic", "stable", 1000)
        }).size() == 1, "failed to seed completed migration database");
    }
    {
        LegacyDatabaseBuilder database(path, libraryPath);
        database.exec(
            "CREATE TRIGGER reject_redundant_target_migration "
            "BEFORE UPDATE OF target_id ON mqtt_event_outbox "
            "BEGIN SELECT RAISE(ABORT, 'unexpected target migration'); END;"
        );
    }
    {
        auto outbox = makeOutbox(path, libraryPath);
        require(outbox->pendingCount("main") == 1,
            "idempotent schema startup did not preserve the existing row");
    }
    removeDatabase(path);
}

void testConcurrentSchemaStartupWaitsForLock(const std::string& libraryPath) {
    const auto path = testDatabasePath("schema_lock");
    removeDatabase(path);
    {
        auto seeded = makeOutbox(path, libraryPath);
    }
    {
        LegacyDatabaseBuilder blocker(path, libraryPath);
        blocker.exec("BEGIN EXCLUSIVE;");
        auto opening = std::async(std::launch::async, [&]() {
            try {
                auto outbox = makeOutbox(path, libraryPath);
                return true;
            } catch (const std::exception&) {
                return false;
            }
        });
        require(opening.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout,
            "schema startup failed fast instead of waiting for the migration lock");
        blocker.exec("ROLLBACK;");
        require(opening.wait_for(std::chrono::seconds(2)) == std::future_status::ready && opening.get(),
            "schema startup did not recover after the migration lock was released");
    }
    removeDatabase(path);
}

void testTargetIsolationAndIdempotency(const std::string& libraryPath) {
    const auto path = testDatabasePath("targets");
    removeDatabase(path);
    {
        auto outbox = makeOutbox(path, libraryPath);
        const auto ids = outbox->enqueueBatch({
            event("fanout-1", "main", "change", "main/topic", "main", 1000),
            event("fanout-1", "third", "change", "third/topic", "third", 1000),
            event("fanout-1", "main", "change", "duplicate/topic", "duplicate", 1000)
        });
        require(ids.size() == 2, "fan-out did not ignore only the duplicate event target");
        require(outbox->pendingCount() == 1, "main target pending count is incorrect");
        require(outbox->pendingCount("third") == 1, "third target pending count is incorrect");

        require(outbox->enqueueBatch({
            event("fanout-1", "main", "change", "again/topic", "again", 2000)
        }).empty(), "duplicate eventId in one target was not idempotent");

        std::vector<std::string> mainTopics;
        require(outbox->replay("main", [&](const std::string& topic, const std::string&) {
            mainTopics.push_back(topic);
        }) == 1, "main target replay count is incorrect");
        require(mainTopics.size() == 1 && mainTopics.front() == "main/topic",
            "main target replay selected the wrong row");
        require(outbox->pendingCount() == 0, "main target was not acknowledged");
        require(outbox->pendingCount("third") == 1, "main acknowledgement consumed third target");

        std::vector<std::string> thirdTopics;
        require(outbox->replay("third", [&](const std::string& topic, const std::string&) {
            thirdTopics.push_back(topic);
        }) == 1, "third target replay count is incorrect");
        require(thirdTopics.size() == 1 && thirdTopics.front() == "third/topic",
            "third target replay selected the wrong row");
    }
    removeDatabase(path);
}

void testFanoutAndStateTransaction(const std::string& libraryPath) {
    const auto path = testDatabasePath("states");
    removeDatabase(path);
    {
        auto outbox = makeOutbox(path, libraryPath);
        edge_gateway::MqttEventOutbox::EventState state;
        state.stateKey = "alarm:4201:over-temperature";
        state.eventType = "alarm";
        state.index = 4201;
        state.alarmType = "over-temperature";
        state.active = true;
        state.value = 73.25;
        state.quality = 1;
        state.sourceTs = 123456;
        state.lifecycle = "raised";

        const auto ids = outbox->enqueueBatchWithStates({
            event("alarm-4201-raised", "main", "alarm", "main/alarm", "alarm-main", 123456),
            event("alarm-4201-raised", "third", "alarm", "third/alarm", "alarm-third", 123456)
        }, {state});
        require(ids.size() == 2, "fan-out rows were not committed with event state");
        const auto states = outbox->loadStates();
        require(states.size() == 1, "persisted event state was not loaded");
        require(states.front().stateKey == state.stateKey &&
                states.front().eventType == state.eventType &&
                states.front().index == state.index &&
                states.front().alarmType == state.alarmType &&
                states.front().active == state.active &&
                std::abs(states.front().value - state.value) < 0.0001 &&
                states.front().quality == state.quality &&
                states.front().sourceTs == state.sourceTs &&
                states.front().lifecycle == state.lifecycle,
            "persisted event state fields changed during round-trip");

        edge_gateway::MqttEventOutbox::EventState invalidState;
        bool threw = false;
        try {
            outbox->enqueueBatchWithStates({
                event("must-rollback", "main", "change", "rollback/topic", "rollback", 200000)
            }, {invalidState});
        } catch (const std::exception&) {
            threw = true;
        }
        require(threw, "invalid state did not fail the event/state transaction");
        require(outbox->pendingCount() == 1, "failed state update did not roll back its event row");
        require(outbox->pendingCount("third") == 1, "failed transaction changed another target");
        require(outbox->loadStates().size() == 1, "failed transaction changed persisted state");
    }
    removeDatabase(path);
}

void testEventTypeConsumerIsolation(const std::string& libraryPath) {
    const auto path = testDatabasePath("types");
    removeDatabase(path);
    {
        auto outbox = makeOutbox(path, libraryPath);
        outbox->enqueueBatch({
            event("business-alarm", "main", "alarm", "event/alarm", "alarm", 1000),
            event("business-change", "main", "change", "event/change", "change", 2000),
            event("management-ota", "main", "ota_status", "event/ota", "ota", 3000)
        });
        const auto business = included({"alarm", "change"});
        const auto management = excluded({"alarm", "change"});
        require(outbox->pendingCount("main", business) == 2,
            "business event filter pending count is incorrect");
        require(outbox->pendingCount("main", management) == 1,
            "management event filter pending count is incorrect");

        std::vector<std::string> businessTopics;
        const auto businessStats = outbox->replayWithStats(
            "main",
            business,
            [&](const std::string& topic, const std::string&) { businessTopics.push_back(topic); }
        );
        require(businessStats.count == 2 && businessStats.alarmCount == 1 &&
                businessStats.changeCount == 1 && businessStats.otherCount == 0,
            "business replay statistics are incorrect");
        require(businessTopics.size() == 2 && businessTopics.front() == "event/alarm" &&
                businessTopics.back() == "event/change",
            "business consumer replayed a management event");
        require(outbox->pendingCount("main", business) == 0,
            "business consumer did not acknowledge its rows");
        require(outbox->pendingCount("main", management) == 1 && outbox->pendingCount() == 1,
            "business consumer acknowledged a management row");

        std::vector<std::string> managementTopics;
        const auto managementStats = outbox->replayWithStats(
            "main",
            management,
            [&](const std::string& topic, const std::string&) { managementTopics.push_back(topic); }
        );
        require(managementStats.count == 1 && managementStats.otherCount == 1,
            "management replay statistics are incorrect");
        require(managementTopics.size() == 1 && managementTopics.front() == "event/ota",
            "management consumer replayed a business event");
        require(outbox->pendingCount() == 0, "management row was not acknowledged");
    }
    removeDatabase(path);
}

void testReplayKeepsPriorityAcrossIndexedEventTypes(const std::string& libraryPath) {
    const auto path = testDatabasePath("indexed_type_priority");
    removeDatabase(path);
    {
        auto outbox = makeOutbox(path, libraryPath, 0, 1);
        require(outbox->enqueueBatch({
            event("other-later", "main", "ota_status", "event/other-later", "other", 3000),
            event("change-oldest", "main", "change", "event/change", "change", 1000),
            event("alarm-newest", "main", "alarm", "event/alarm", "alarm", 4000),
            event("other-earlier", "main", "command_reply", "event/other-earlier", "other", 2000),
            event("third-alarm", "third", "alarm", "third/alarm", "third", 500)
        }).size() == 5, "failed to seed indexed replay priority rows");

        std::vector<std::string> topics;
        const auto replayOne = [&]() {
            require(outbox->replay("main", [&](const std::string& topic, const std::string&) {
                topics.push_back(topic);
            }) == 1, "indexed replay did not consume one row");
        };
        replayOne();
        replayOne();
        replayOne();
        replayOne();
        require(topics == std::vector<std::string>({
            "event/alarm", "event/change", "event/other-earlier", "event/other-later"
        }), "indexed replay changed alarm/change/other ordering");
        require(outbox->pendingCount("main") == 0 && outbox->pendingCount("third") == 1,
            "indexed replay crossed target boundaries");
    }
    removeDatabase(path);
}

void testReplayBatchAcknowledgesPrefixAndReleasesRemainder(const std::string& libraryPath) {
    const auto path = testDatabasePath("batch_claim_finish");
    removeDatabase(path);
    {
        auto outbox = makeOutbox(path, libraryPath, 0, 4);
        require(outbox->enqueueBatch({
            event("batch-1", "main", "change", "event/1", "one", 1000),
            event("batch-2", "main", "change", "event/2", "two", 2000),
            event("batch-3", "main", "change", "event/3", "three", 3000),
            event("batch-4", "main", "change", "event/4", "four", 4000)
        }).size() == 4, "failed to seed replay batch rows");

        int sendCalls = 0;
        bool failed = false;
        try {
            outbox->replay("main", [&](const std::string&, const std::string&) {
                ++sendCalls;
                if (sendCalls == 1) {
                    LegacyDatabaseBuilder database(path, libraryPath);
                    require(database.scalarInt64(
                        "SELECT COUNT(*) FROM mqtt_event_outbox "
                        "WHERE sent=0 AND claim_token IS NOT NULL;"
                    ) == 4, "replay did not claim its bounded batch in one transaction");
                }
                if (sendCalls == 3) {
                    throw std::runtime_error("injected batch send failure");
                }
            });
        } catch (const std::exception& ex) {
            failed = std::string(ex.what()) == "injected batch send failure";
        }
        require(failed, "replay batch did not surface the send failure");
        require(outbox->pendingCount("main") == 2,
            "replay batch did not acknowledge only the successful prefix");
        {
            LegacyDatabaseBuilder database(path, libraryPath);
            require(database.scalarInt64(
                "SELECT COUNT(*) FROM mqtt_event_outbox "
                "WHERE sent=0 AND claim_token IS NOT NULL;"
            ) == 0, "replay batch left failed or unsent rows claimed");
        }

        std::vector<std::string> retriedTopics;
        require(outbox->replay("main", [&](const std::string& topic, const std::string&) {
            retriedTopics.push_back(topic);
        }) == 2, "replay batch did not retry the failed suffix");
        require(retriedTopics == std::vector<std::string>({"event/3", "event/4"}),
            "replay batch retried acknowledged rows or reordered its suffix");
        require(outbox->pendingCount("main") == 0,
            "replay batch did not acknowledge the retried suffix");
    }
    removeDatabase(path);
}

void testReplayPublisherBatchFailureReleasesWholeClaim(const std::string& libraryPath) {
    const auto path = testDatabasePath("publisher_batch_failure");
    removeDatabase(path);
    {
        auto outbox = makeOutbox(path, libraryPath, 0, 4);
        require(outbox->enqueueBatch({
            event("publish-batch-1", "main", "change", "event/1", "one", 1000),
            event("publish-batch-2", "main", "change", "event/2", "two", 2000),
            event("publish-batch-3", "main", "change", "event/3", "three", 3000),
            event("publish-batch-4", "main", "change", "event/4", "four", 4000)
        }).size() == 4, "failed to seed publisher batch failure rows");

        bool failed = false;
        try {
            outbox->replayBatchWithStats(
                "main",
                included({"alarm", "change"}),
                0,
                0,
                [&](const std::vector<edge_gateway::MqttEventOutbox::ReplayMessage>& messages) {
                    require(messages.size() == 4, "publisher did not receive the whole claim batch");
                    throw std::runtime_error("injected publisher batch failure");
                }
            );
        } catch (const std::exception& ex) {
            failed = std::string(ex.what()) == "injected publisher batch failure";
        }
        require(failed, "publisher batch failure was not surfaced");
        require(outbox->pendingCount("main") == 4,
            "publisher batch failure acknowledged an uncertain delivery");
        {
            LegacyDatabaseBuilder database(path, libraryPath);
            require(database.scalarInt64(
                "SELECT COUNT(*) FROM mqtt_event_outbox "
                "WHERE sent=0 AND claim_token IS NOT NULL;"
            ) == 0, "publisher batch failure leaked its claim");
        }

        std::vector<std::string> retriedTopics;
        const auto stats = outbox->replayBatchWithStats(
            "main",
            included({"alarm", "change"}),
            0,
            0,
            [&](const std::vector<edge_gateway::MqttEventOutbox::ReplayMessage>& messages) {
                for (const auto& message : messages) {
                    retriedTopics.push_back(message.topic);
                }
            }
        );
        require(stats.count == 4 && retriedTopics == std::vector<std::string>({
            "event/1", "event/2", "event/3", "event/4"
        }), "publisher batch retry lost or reordered events");
        require(outbox->pendingCount("main") == 0,
            "publisher batch retry did not acknowledge all events");
    }
    removeDatabase(path);
}

void testReplayPublisherSecondSubBatchAcknowledgesPrefix(const std::string& libraryPath) {
    const auto path = testDatabasePath("publisher_second_sub_batch_failure");
    removeDatabase(path);
    {
        auto outbox = makeOutbox(path, libraryPath, 0, 16);
        std::vector<edge_gateway::MqttEventOutbox::EventMessage> events;
        for (int index = 1; index <= 16; ++index) {
            events.push_back(event(
                "publish-window-" + std::to_string(index),
                "main",
                "change",
                "event/" + std::to_string(index),
                "payload-" + std::to_string(index),
                1000 + index
            ));
        }
        require(outbox->enqueueBatch(events).size() == events.size(),
            "failed to seed the sixteen-row replay claim");

        int publishCalls = 0;
        bool failed = false;
        try {
            outbox->replayBatchWithStats(
                "main",
                included({"alarm", "change"}),
                0,
                0,
                [&](const std::vector<edge_gateway::MqttEventOutbox::ReplayMessage>& messages) {
                    ++publishCalls;
                    require(messages.size() == 8,
                        "outbox did not split a sixteen-row claim into eight-row publish windows");
                    if (publishCalls == 2) {
                        throw std::runtime_error("injected second publish window failure");
                    }
                }
            );
        } catch (const std::exception& ex) {
            failed = std::string(ex.what()) == "injected second publish window failure";
        }
        require(failed && publishCalls == 2,
            "second publish window failure was not surfaced");
        require(outbox->pendingCount("main") == 8,
            "successfully acknowledged publish prefix was replayed or failed suffix was lost");
        {
            LegacyDatabaseBuilder database(path, libraryPath);
            require(database.scalarInt64(
                "SELECT COUNT(*) FROM mqtt_event_outbox "
                "WHERE sent=0 AND claim_token IS NOT NULL;"
            ) == 0, "second publish window failure leaked claimed rows");
        }

        std::vector<std::string> retriedTopics;
        const auto stats = outbox->replayBatchWithStats(
            "main",
            included({"alarm", "change"}),
            0,
            0,
            [&](const std::vector<edge_gateway::MqttEventOutbox::ReplayMessage>& messages) {
                for (const auto& message : messages) {
                    retriedTopics.push_back(message.topic);
                }
            }
        );
        std::vector<std::string> expectedTopics;
        for (int index = 9; index <= 16; ++index) {
            expectedTopics.push_back("event/" + std::to_string(index));
        }
        require(stats.count == 8 && retriedTopics == expectedTopics,
            "retry after second publish window failure did not preserve only the failed suffix");
        require(outbox->pendingCount("main") == 0,
            "failed publish suffix was not acknowledged on retry");
    }
    removeDatabase(path);
}

void testReplayByteLimitClaimsOnlySendablePrefix(const std::string& libraryPath) {
    const auto path = testDatabasePath("claim_byte_limit");
    removeDatabase(path);
    {
        auto outbox = makeOutbox(path, libraryPath, 0, 4);
        require(outbox->enqueueBatch({
            event("byte-1", "main", "change", "event/1", std::string(20, 'a'), 1000),
            event("byte-2", "main", "change", "event/2", std::string(20, 'b'), 2000),
            event("byte-3", "main", "change", "event/3", std::string(20, 'c'), 3000)
        }).size() == 3, "failed to seed replay byte-limit rows");

        std::vector<std::string> topics;
        const auto stats = outbox->replayWithStats(
            "main",
            included({"change"}),
            54,
            0,
            [&](const std::string& topic, const std::string&) { topics.push_back(topic); }
        );
        require(stats.count == 2 && stats.bytes == 54,
            "replay byte limit did not keep the largest sendable prefix");
        require(topics == std::vector<std::string>({"event/1", "event/2"}),
            "replay byte limit reordered or skipped its sendable prefix");
        require(outbox->pendingCount("main") == 1,
            "replay byte limit acknowledged an unsent suffix");
        {
            LegacyDatabaseBuilder database(path, libraryPath);
            require(database.scalarInt64(
                "SELECT COUNT(*) FROM mqtt_event_outbox "
                "WHERE sent=0 AND claim_token IS NOT NULL;"
            ) == 0, "replay byte limit left its unsent suffix claimed");
        }
        require(outbox->replay("main", [](const std::string&, const std::string&) {}) == 1,
            "replay byte-limit suffix was not available on the next pass");
    }
    removeDatabase(path);
}

void testReplayClaimProtectsRowUntilAcknowledged(const std::string& libraryPath) {
    const auto path = testDatabasePath("replay_claim");
    removeDatabase(path);
    {
        auto seed = makeOutbox(path, libraryPath);
        require(seed->enqueueBatch({
            event("old-change", "main", "change", "old/topic", std::string(60, 'o'), 1000),
            event("expendable-change", "main", "change", "expendable/topic", std::string(60, 'x'), 2000)
        }).size() == 2, "failed to seed replay claim test");
    }

    {
        auto replaying = makeOutbox(path, libraryPath, 0, 1);
        auto trimming = makeOutbox(path, libraryPath, 150, 1);
        bool sendFailed = false;
        try {
            replaying->replay("main", [&](const std::string& topic, const std::string&) {
                require(topic == "old/topic", "replay did not select the oldest row");
                require(trimming->enqueueBatch({
                    event("new-change", "main", "change", "new/topic", std::string(60, 'n'), 3000)
                }).size() == 1, "concurrent enqueue failed while replay row was in flight");
                throw std::runtime_error("injected send failure");
            });
        } catch (const std::exception& ex) {
            sendFailed = std::string(ex.what()) == "injected send failure";
        }
        require(sendFailed, "replay did not surface the send failure");
        require(replaying->pendingCount("main") == 2,
            "capacity enforcement removed the wrong number of pending rows");

        std::vector<std::string> retriedTopics;
        require(replaying->replay("main", [&](const std::string& topic, const std::string&) {
            retriedTopics.push_back(topic);
        }) == 1, "failed replay did not leave one row available for retry");
        require(retriedTopics == std::vector<std::string>{"old/topic"},
            "capacity enforcement deleted the in-flight row before it could be acknowledged");
    }
    removeDatabase(path);
}

void testExpiredReplayClaimIsReclaimedWithoutStartupCleanup(const std::string& libraryPath) {
    const auto path = testDatabasePath("expired_replay_claim");
    removeDatabase(path);
    {
        auto outbox = makeOutbox(path, libraryPath, 0, 1);
        require(outbox->enqueueBatch({
            event("expired-claim", "main", "alarm", "expired/topic", "payload", 1000)
        }).size() == 1, "failed to seed expired replay claim");
        LegacyDatabaseBuilder database(path, libraryPath);
        database.exec(
            "UPDATE mqtt_event_outbox SET claim_token='dead-owner',claim_until=1 WHERE sent=0;"
        );

        std::vector<std::string> topics;
        require(outbox->replay("main", [&](const std::string& topic, const std::string&) {
            topics.push_back(topic);
        }) == 1, "expired replay claim was not reclaimed by the normal claim query");
        require(topics == std::vector<std::string>{"expired/topic"} &&
                outbox->pendingCount("main") == 0,
            "expired replay claim recovery lost or duplicated the event");
    }
    removeDatabase(path);
}

void testUnexpiredReplayClaimIsNotStolen(const std::string& libraryPath) {
    const auto path = testDatabasePath("active_replay_claim");
    removeDatabase(path);
    {
        auto outbox = makeOutbox(path, libraryPath, 0, 1);
        require(outbox->enqueueBatch({
            event("active-claim", "main", "alarm", "claimed/topic", "claimed", 1000),
            event("available-row", "main", "alarm", "available/topic", "available", 2000)
        }).size() == 2, "failed to seed active replay claim rows");
        LegacyDatabaseBuilder database(path, libraryPath);
        database.exec(
            "UPDATE mqtt_event_outbox SET claim_token='live-owner',claim_until=4102444800000 "
            "WHERE event_id='active-claim';"
        );

        std::vector<std::string> topics;
        require(outbox->replay("main", [&](const std::string& topic, const std::string&) {
            topics.push_back(topic);
        }) == 1, "active replay claim blocked every eligible row");
        require(topics == std::vector<std::string>{"available/topic"},
            "replay stole a row protected by an unexpired claim");
        require(outbox->pendingCount("main") == 1,
            "active replay claim changed the protected row's pending state");

        database.exec(
            "UPDATE mqtt_event_outbox SET claim_until=1 WHERE event_id='active-claim';"
        );
        topics.clear();
        require(outbox->replay("main", [&](const std::string& topic, const std::string&) {
            topics.push_back(topic);
        }) == 1 && topics == std::vector<std::string>{"claimed/topic"},
            "expired active claim was not available to the next replay owner");
    }
    removeDatabase(path);
}

void testBusyDatabaseRetriesWholeEnqueueTransaction(const std::string& libraryPath) {
    const auto path = testDatabasePath("busy_retry");
    removeDatabase(path);
    {
        auto outbox = makeOutbox(path, libraryPath);
        LegacyDatabaseBuilder blocker(path, libraryPath);
        blocker.exec("BEGIN EXCLUSIVE;");

        auto enqueue = std::async(std::launch::async, [&outbox]() {
            try {
                return outbox->enqueueBatch({
                    event("busy-change", "main", "change", "busy/topic", "busy", 1000)
                }).size() == 1;
            } catch (const std::exception&) {
                return false;
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        blocker.exec("ROLLBACK;");
        require(enqueue.wait_for(std::chrono::seconds(2)) == std::future_status::ready && enqueue.get(),
            "bounded whole-transaction retry dropped an event after a transient writer lock");
        require(outbox->pendingCount("main") == 1,
            "retried enqueue did not persist exactly one event");
    }
    removeDatabase(path);
}

void testPersistentBusyDatabaseStillFailsWithinBoundedTime(const std::string& libraryPath) {
    const auto path = testDatabasePath("busy_bounded_failure");
    removeDatabase(path);
    {
        auto outbox = makeOutbox(path, libraryPath);
        LegacyDatabaseBuilder blocker(path, libraryPath);
        blocker.exec("BEGIN EXCLUSIVE;");

        auto enqueue = std::async(std::launch::async, [&outbox]() {
            try {
                outbox->enqueueBatch({
                    event("persistent-busy", "main", "alarm", "busy/alarm", "busy", 1000)
                });
                return false;
            } catch (const std::exception&) {
                return true;
            }
        });
        const bool completed =
            enqueue.wait_for(std::chrono::milliseconds(2500)) == std::future_status::ready;
        blocker.exec("ROLLBACK;");
        require(completed && enqueue.get(),
            "persistent SQLite writer contention exceeded the bounded enqueue retry budget");
    }
    removeDatabase(path);
}

void testAlarmIsNeverPruned(const std::string& libraryPath) {
    const auto path = testDatabasePath("disk_limit");
    removeDatabase(path);
    {
        auto outbox = makeOutbox(path, libraryPath, 120);
        require(outbox->enqueueBatch({
            event("old-change", "main", "change", "c", std::string(80, 'c'), 1000)
        }).size() == 1, "initial change was not persisted");
        require(outbox->enqueueBatch({
            event("protected-alarm", "main", "alarm", "a", std::string(80, 'a'), 2000)
        }).size() == 1, "alarm was not persisted after pruning an old change");

        const auto alarms = included({"alarm"});
        const auto changes = included({"change"});
        require(outbox->pendingCount("main", alarms) == 1, "alarm was pruned by disk enforcement");
        require(outbox->pendingCount("main", changes) == 0, "old change was not pruned first");

        bool threw = false;
        std::string error;
        try {
            outbox->enqueueBatch({
                event("second-alarm", "main", "alarm", "b", std::string(80, 'b'), 3000)
            });
        } catch (const std::exception& ex) {
            threw = true;
            error = ex.what();
        }
        require(threw, "disk limit silently accepted data when no change could be pruned");
        require(error.find("disk limit exceeded") != std::string::npos &&
                error.find("alarm events are protected") != std::string::npos,
            "disk limit failure did not explain alarm protection");
        require(outbox->pendingCount("main", alarms) == 1 && outbox->pendingCount() == 1,
            "failed disk-limit transaction removed or added an alarm");
    }
    removeDatabase(path);
}

void testFanoutSavepointIsolatesFullOptionalTarget(const std::string& libraryPath) {
    const auto path = testDatabasePath("fanout_savepoint");
    removeDatabase(path);
    constexpr std::size_t maxPendingBytesPerTarget = 600;
    {
        auto outbox = makeOutbox(path, libraryPath, maxPendingBytesPerTarget);
        require(outbox->enqueueBatch({
            event("partner-backlog", "partner-a", "alarm", "third/alarm", std::string(520, 'x'), 1000)
        }).size() == 1, "failed to seed optional target backlog");

        edge_gateway::MqttEventOutbox::EventState state;
        state.stateKey = "alarm:610001:high";
        state.eventType = "alarm";
        state.index = 610001;
        state.alarmType = "high";
        state.active = true;
        state.sourceTs = 2000;
        state.lifecycle = "raised:1";
        const auto result = outbox->enqueueFanoutWithStates({
            event("fanout-main", "main", "alarm", "edge/alarm", "main-alarm", 2000),
            event("fanout-partner", "partner-a", "alarm", "third/alarm", std::string(100, 'p'), 2000)
        }, {state});

        require(result.ids.size() == 1 &&
                result.failedTargetIds == std::vector<std::string>{"partner-a"},
            "fanout result did not identify only the rolled-back optional target");
        require(outbox->pendingCount("main") == 1,
            "optional target failure rolled back the primary event");
        require(outbox->pendingCount("partner-a") == 1,
            "optional target rollback changed its protected backlog");
        require(outbox->loadStates().size() == 1,
            "optional target failure rolled back primary lifecycle state");
    }
    removeDatabase(path);
}

void testIncrementalStatsTrackDuplicateFanoutAndAck(const std::string& libraryPath) {
    const auto path = testDatabasePath("incremental_stats");
    removeDatabase(path);
    {
        auto outbox = makeOutbox(path, libraryPath);
        const auto ids = outbox->enqueueBatch({
            event("shared-event", "main", "change", "main/topic", "main", 1000),
            event("shared-event", "third", "alarm", "third/topic", "alarm", 1000),
            event("shared-event", "main", "change", "ignored/topic", "ignored", 1000)
        });
        require(ids.size() == 2, "stats test did not preserve target-scoped idempotency");

        LegacyDatabaseBuilder database(path, libraryPath);
        require(database.scalarInt64(
            "SELECT pending_count FROM mqtt_event_outbox_stats "
            "WHERE target_id='main' AND event_type='change';"
        ) == 1, "duplicate event incremented main pending stats");
        require(database.scalarInt64(
            "SELECT pending_bytes FROM mqtt_event_outbox_stats "
            "WHERE target_id='main' AND event_type='change';"
        ) == 14, "main pending byte stats used a different size definition");
        require(database.scalarInt64(
            "SELECT pending_count FROM mqtt_event_outbox_stats "
            "WHERE target_id='third' AND event_type='alarm';"
        ) == 1, "fanout target did not receive independent pending stats");

        outbox->markSent(ids.front(), 2000);
        outbox->markSent(ids.front(), 3000);
        require(outbox->pendingCount("main") == 0, "duplicate ACK changed main pending count");
        require(outbox->pendingCount("third") == 1, "main ACK changed third target stats");
        require(database.scalarInt64(
            "SELECT pending_count + pending_bytes FROM mqtt_event_outbox_stats "
            "WHERE target_id='main' AND event_type='change';"
        ) == 0, "ACK did not atomically clear count and byte stats");
        outbox->markSentBatch({ids.back(), ids.back()}, 4000);
        outbox->markSentBatch({ids.back()}, 5000);
        require(outbox->pendingCount("third") == 0, "batch or duplicate batch ACK changed stats incorrectly");
        require(database.scalarInt64(
            "SELECT pending_count + pending_bytes FROM mqtt_event_outbox_stats "
            "WHERE target_id='third' AND event_type='alarm';"
        ) == 0, "batch ACK did not atomically clear target stats");
    }
    removeDatabase(path);
}

void testStatsMigrationResumesWithoutDoubleCounting(const std::string& libraryPath) {
    const auto path = testDatabasePath("stats_resume");
    removeDatabase(path);
    {
        auto outbox = makeOutbox(path, libraryPath);
        require(outbox->enqueueBatch({
            event("one", "main", "change", "m/1", "one", 1000),
            event("two", "main", "alarm", "m/2", "two", 2000),
            event("three", "third", "change", "t/3", "three", 3000),
            event("four", "third", "alarm", "t/4", "four", 4000)
        }).size() == 4, "failed to seed resumable stats migration");
    }
    {
        LegacyDatabaseBuilder database(path, libraryPath);
        database.exec("DELETE FROM mqtt_event_outbox_stats;");
        database.exec(
            "INSERT INTO mqtt_event_outbox_stats(target_id,event_type,pending_count,pending_bytes,updated_at) "
            "VALUES('main','change',1,6,1),('third','alarm',1,7,1);"
        );
        database.exec("UPDATE mqtt_event_outbox_meta SET value='3' WHERE key='stats_migrate_cap';");
        database.exec("UPDATE mqtt_event_outbox_meta SET value='1' WHERE key='stats_migrate_hw';");
        database.exec("UPDATE mqtt_event_outbox_meta SET value='0' WHERE key='stats_migrate_done';");
    }
    {
        auto outbox = makeOutbox(path, libraryPath);
        require(outbox->pendingCount("main") == 2 && outbox->pendingCount("third") == 2,
            "resumed stats migration omitted or double-counted a range");
    }
    {
        LegacyDatabaseBuilder database(path, libraryPath);
        require(database.scalarInt64(
            "SELECT value FROM mqtt_event_outbox_meta WHERE key='stats_migrate_done';"
        ) == 1, "resumed stats migration was not marked complete");
        require(database.scalarInt64(
            "SELECT value FROM mqtt_event_outbox_meta WHERE key='stats_migrate_hw';"
        ) == 3, "resumed stats migration did not persist its high-water mark");
        require(database.scalarInt64(
            "SELECT COALESCE(SUM(pending_count),0) FROM mqtt_event_outbox_stats;"
        ) == 4, "resumed stats migration produced an incorrect total");
    }
    {
        auto reopened = makeOutbox(path, libraryPath);
        require(reopened->pendingCount("main") == 2 && reopened->pendingCount("third") == 2,
            "completed stats migration ran again after restart");
    }
    removeDatabase(path);
}

void testStatsMigrationAckWatermarkBoundaries(const std::string& libraryPath) {
    const auto path = testDatabasePath("stats_ack_watermark");
    removeDatabase(path);
    {
        auto outbox = makeOutbox(path, libraryPath);
        require(outbox->enqueueBatch({
            event("one", "main", "change", "m/1", "one", 1000),
            event("two", "main", "change", "m/2", "two", 2000),
            event("three", "main", "change", "m/3", "three", 3000)
        }).size() == 3, "failed to seed migration ACK boundary test");
    }
    {
        LegacyDatabaseBuilder database(path, libraryPath);
        database.exec("DELETE FROM mqtt_event_outbox_stats;");
        database.exec(
            "INSERT INTO mqtt_event_outbox_stats(target_id,event_type,pending_count,pending_bytes,updated_at) "
            "VALUES('main','change',2,14,1);"
        );
        database.exec("UPDATE mqtt_event_outbox_meta SET value='2' WHERE key='stats_migrate_cap';");
        database.exec("UPDATE mqtt_event_outbox_meta SET value='1' WHERE key='stats_migrate_hw';");
        database.exec("UPDATE mqtt_event_outbox_meta SET value='0' WHERE key='stats_migrate_done';");

        database.exec("UPDATE mqtt_event_outbox SET sent=1, sent_at=10 WHERE id=1;");
        require(database.scalarInt64(
            "SELECT pending_count FROM mqtt_event_outbox_stats "
            "WHERE target_id='main' AND event_type='change';"
        ) == 1, "ACK below migration high-water mark did not decrement stats");
        database.exec("UPDATE mqtt_event_outbox SET sent=1, sent_at=20 WHERE id=2;");
        require(database.scalarInt64(
            "SELECT pending_count FROM mqtt_event_outbox_stats "
            "WHERE target_id='main' AND event_type='change';"
        ) == 1, "ACK in the uncounted migration gap decremented stats");
        database.exec("UPDATE mqtt_event_outbox SET sent=1, sent_at=30 WHERE id=3;");
        require(database.scalarInt64(
            "SELECT pending_count FROM mqtt_event_outbox_stats "
            "WHERE target_id='main' AND event_type='change';"
        ) == 0, "ACK above migration cap did not decrement stats");
    }
    {
        auto outbox = makeOutbox(path, libraryPath);
        require(outbox->pendingCount("main") == 0,
            "migration resume counted an event acknowledged inside the migration gap");
    }
    removeDatabase(path);
}

void testPendingCountDoesNotReadTheEventTable(const std::string& libraryPath) {
    const auto path = testDatabasePath("stats_query_path");
    removeDatabase(path);
    {
        auto outbox = makeOutbox(path, libraryPath);
        require(outbox->enqueueBatch({
            event("stats-only", "main", "change", "m/1", "one", 1000)
        }).size() == 1, "failed to seed stats-only query test");
        LegacyDatabaseBuilder database(path, libraryPath);
        database.exec("ALTER TABLE mqtt_event_outbox RENAME TO mqtt_event_outbox_unavailable;");
        require(outbox->pendingCount("main") == 1,
            "pendingCount still depends on the large event table");
    }
    removeDatabase(path);
}

#ifndef _WIN32
void testReplayRetriesCommitBusyWithoutRepublishing(const std::string& libraryPath) {
    const auto path = testDatabasePath("replay_commit_busy");
    removeDatabase(path);
    {
        auto outbox = makeOutbox(path, libraryPath, 0, 1);
        require(outbox->enqueueBatch({
            event("commit-busy", "main", "change", "commit/topic", "payload", 1000)
        }).size() == 1, "failed to seed replay COMMIT-busy row");

        LegacyDatabaseBuilder reader(path, libraryPath);
        reader.exec("BEGIN;");
        require(reader.scalarInt64(
            "SELECT COUNT(*) FROM mqtt_event_outbox WHERE sent=0;"
        ) == 1, "reader did not establish the expected DELETE-journal read lock");

        std::atomic<int> publishCount{0};
        auto replaying = std::async(std::launch::async, [&]() {
            return outbox->replay("main", [&](const std::string&, const std::string&) {
                publishCount.fetch_add(1);
            });
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        reader.exec("ROLLBACK;");

        require(replaying.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
            "replay did not recover after a transient COMMIT reader lock");
        require(replaying.get() == 1 && publishCount.load() == 1,
            "COMMIT-busy retry republished or lost the event");
        require(outbox->pendingCount("main") == 0,
            "COMMIT-busy retry did not persist the final acknowledgement");
    }
    removeDatabase(path);
}

void testConcurrentEnqueueAndReplayAvoidWriterStarvation(const std::string& libraryPath) {
    const auto path = testDatabasePath("enqueue_replay_fairness");
    removeDatabase(path);
    {
        auto writer = makeOutbox(path, libraryPath, 0, 64);
        auto replay = makeOutbox(path, libraryPath, 0, 64);
        {
            LegacyDatabaseBuilder history(path, libraryPath);
            history.exec(
                "WITH RECURSIVE seq(x) AS ("
                "VALUES(1) UNION ALL SELECT x+1 FROM seq WHERE x<100000) "
                "INSERT INTO mqtt_event_outbox("
                "event_id,target_id,event_type,topic,payload,event_ts,event_month,created_at,sent,sent_at) "
                "SELECT 'history-'||x,'main','change','history/topic','history-payload',"
                "x,'1970-01',x,1,x FROM seq;"
            );
        }
        std::vector<edge_gateway::MqttEventOutbox::EventMessage> backlog;
        for (int index = 0; index < 320; ++index) {
            backlog.push_back(event(
                "fairness-backlog-" + std::to_string(index),
                "main",
                index % 5 == 0 ? "alarm" : "change",
                "fairness/backlog/" + std::to_string(index),
                "payload",
                1000 + index
            ));
        }
        require(writer->enqueueBatch(backlog).size() == backlog.size(),
            "failed to seed enqueue/replay fairness backlog");

        std::atomic<bool> writerDone{false};
        std::atomic<int> enqueueFailures{0};
        std::atomic<int> replayFailures{0};
        std::mutex failureMutex;
        std::string firstEnqueueFailure;
        std::string firstReplayFailure;
        std::thread enqueueThread([&]() {
            for (int index = 0; index < 200; ++index) {
                try {
                    const auto id = "fairness-live-" + std::to_string(index);
                    if (writer->enqueueBatch({event(
                            id,
                            "main",
                            index % 7 == 0 ? "alarm" : "change",
                            "fairness/live/" + std::to_string(index),
                            "payload",
                            100000 + index
                        )}).size() != 1) {
                        enqueueFailures.fetch_add(1);
                    }
                } catch (const std::exception& ex) {
                    enqueueFailures.fetch_add(1);
                    std::lock_guard<std::mutex> lock(failureMutex);
                    if (firstEnqueueFailure.empty()) {
                        firstEnqueueFailure = ex.what();
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            writerDone.store(true);
        });
        std::thread replayThread([&]() {
            while (!writerDone.load() || replay->pendingCount("main") > 0) {
                try {
                    const auto stats = replay->replayBatchWithStats(
                        "main",
                        included({"alarm", "change"}),
                        0,
                        0,
                        [](const std::vector<edge_gateway::MqttEventOutbox::ReplayMessage>&) {}
                    );
                    if (stats.count == 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                } catch (const std::exception& ex) {
                    replayFailures.fetch_add(1);
                    std::lock_guard<std::mutex> lock(failureMutex);
                    if (firstReplayFailure.empty()) {
                        firstReplayFailure = ex.what();
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        });
        enqueueThread.join();
        replayThread.join();

        require(enqueueFailures.load() == 0,
            "concurrent replay exhausted the bounded whole-transaction enqueue retry: " +
                firstEnqueueFailure);
        require(replayFailures.load() == 0,
            "concurrent enqueue caused a replay lock failure: " + firstReplayFailure);
        require(replay->pendingCount("main") == 0,
            "concurrent replay did not drain every persisted event");
        LegacyDatabaseBuilder database(path, libraryPath);
        require(database.scalarInt64(
            "SELECT COALESCE(SUM(pending_count),0) FROM mqtt_event_outbox_stats;"
        ) == database.scalarInt64(
            "SELECT COUNT(*) FROM mqtt_event_outbox WHERE sent=0;"
        ), "concurrent enqueue/replay left incremental stats inconsistent");
    }
    removeDatabase(path);
}

void testConcurrentProcessesKeepStatsConsistent(const std::string& libraryPath) {
    const auto path = testDatabasePath("stats_multiprocess");
    removeDatabase(path);
    {
        auto seeded = makeOutbox(path, libraryPath);
    }

    std::vector<pid_t> children;
    for (int process = 0; process < 2; ++process) {
        const auto child = fork();
        require(child >= 0, "failed to fork outbox writer");
        if (child == 0) {
            try {
                auto outbox = makeOutbox(path, libraryPath);
                std::vector<edge_gateway::MqttEventOutbox::EventMessage> events;
                for (int row = 0; row < 200; ++row) {
                    const auto suffix = std::to_string(process) + "-" + std::to_string(row);
                    events.push_back(event(
                        "process-" + suffix,
                        process == 0 ? "main" : "third",
                        row % 2 == 0 ? "change" : "alarm",
                        "process/" + suffix,
                        "payload-" + suffix,
                        1000 + row
                    ));
                }
                if (outbox->enqueueBatch(events).size() != events.size()) {
                    _exit(2);
                }
                _exit(0);
            } catch (...) {
                _exit(3);
            }
        }
        children.push_back(child);
    }
    for (const auto child : children) {
        int status = 0;
        require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "concurrent outbox writer failed under cross-process lock contention");
    }
    {
        auto outbox = makeOutbox(path, libraryPath);
        require(outbox->pendingCount("main") == 200 && outbox->pendingCount("third") == 200,
            "cross-process writes produced incorrect target stats");
        LegacyDatabaseBuilder database(path, libraryPath);
        require(database.scalarInt64(
            "SELECT COALESCE(SUM(pending_count),0) FROM mqtt_event_outbox_stats;"
        ) == database.scalarInt64(
            "SELECT COUNT(*) FROM mqtt_event_outbox WHERE sent=0;"
        ), "cross-process incremental stats diverged from pending rows");
    }
    removeDatabase(path);
}

void testStatsMigrationRecoversAfterProcessKill(const std::string& libraryPath) {
    const auto path = testDatabasePath("stats_kill_recovery");
    removeDatabase(path);
    {
        auto seeded = makeOutbox(path, libraryPath);
    }
    {
        LegacyDatabaseBuilder database(path, libraryPath);
        database.exec(
            "BEGIN IMMEDIATE;"
            "WITH RECURSIVE seq(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM seq WHERE x<100000) "
            "INSERT INTO mqtt_event_outbox("
            "event_id,target_id,event_type,topic,payload,event_ts,event_month,created_at,sent) "
            "SELECT 'kill-'||x,CASE WHEN x%2=0 THEN 'main' ELSE 'third' END,"
            "CASE WHEN x%3=0 THEN 'alarm' ELSE 'change' END,"
            "'kill/'||x,'payload-'||x,x,'1970-01',x,0 FROM seq;"
            "DELETE FROM mqtt_event_outbox_stats;"
            "UPDATE mqtt_event_outbox_meta SET value='100000' WHERE key='stats_migrate_cap';"
            "UPDATE mqtt_event_outbox_meta SET value='0' WHERE key='stats_migrate_hw';"
            "UPDATE mqtt_event_outbox_meta SET value='0' WHERE key='stats_migrate_done';"
            "COMMIT;"
        );
    }

    int ready[2] = {-1, -1};
    require(pipe(ready) == 0, "failed to create migration kill synchronization pipe");
    const auto child = fork();
    require(child >= 0, "failed to fork migration worker");
    if (child == 0) {
        close(ready[0]);
        const char started = '1';
        (void)write(ready[1], &started, 1);
        close(ready[1]);
        try {
            auto outbox = makeOutbox(path, libraryPath);
            _exit(0);
        } catch (...) {
            _exit(4);
        }
    }
    close(ready[1]);
    char started = 0;
    require(read(ready[0], &started, 1) == 1 && started == '1',
        "migration worker did not start");
    close(ready[0]);
    usleep(500);
    require(kill(child, SIGKILL) == 0, "migration worker completed before kill injection");
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFSIGNALED(status),
        "migration worker was not interrupted by kill injection");

    {
        auto recovered = makeOutbox(path, libraryPath);
        require(recovered->pendingCount("main") == 50000 &&
                recovered->pendingCount("third") == 50000,
            "stats migration did not resume exactly after process termination");
        LegacyDatabaseBuilder database(path, libraryPath);
        require(database.scalarInt64(
            "SELECT COALESCE(SUM(pending_count),0) FROM mqtt_event_outbox_stats;"
        ) == 100000, "recovered migration lost or double-counted pending rows");
        require(database.scalarInt64(
            "SELECT value FROM mqtt_event_outbox_meta WHERE key='stats_migrate_done';"
        ) == 1, "recovered migration did not persist completion");
    }
    removeDatabase(path);
}
#endif

void probeDatabase(const std::string& libraryPath, const std::string& path) {
    const auto openStarted = std::chrono::steady_clock::now();
    auto outbox = makeOutbox(path, libraryPath);
    const auto openFinished = std::chrono::steady_clock::now();
    std::size_t reportedPending = 0;
    const auto queryStarted = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < 1000; ++iteration) {
        reportedPending = outbox->pendingCount("main") + outbox->pendingCount("third");
    }
    const auto queryFinished = std::chrono::steady_clock::now();

    LegacyDatabaseBuilder database(path, libraryPath);
    const auto actualPending = database.scalarInt64(
        "SELECT COUNT(*) FROM mqtt_event_outbox WHERE sent=0;"
    );
    const auto actualBytes = database.scalarInt64(
        "SELECT COALESCE(SUM(length(topic)+length(payload)),0) "
        "FROM mqtt_event_outbox WHERE sent=0;"
    );
    const auto statsPending = database.scalarInt64(
        "SELECT COALESCE(SUM(pending_count),0) FROM mqtt_event_outbox_stats;"
    );
    const auto statsBytes = database.scalarInt64(
        "SELECT COALESCE(SUM(pending_bytes),0) FROM mqtt_event_outbox_stats;"
    );
    const auto migrationDone = database.scalarInt64(
        "SELECT value FROM mqtt_event_outbox_meta WHERE key='stats_migrate_done';"
    );
    require(reportedPending == static_cast<std::size_t>(actualPending),
        "probe pendingCount disagrees with the event table");
    require(statsPending == actualPending && statsBytes == actualBytes && migrationDone == 1,
        "probe stats table is inconsistent after migration");

    const auto openMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        openFinished - openStarted
    ).count();
    const auto queryUs = std::chrono::duration_cast<std::chrono::microseconds>(
        queryFinished - queryStarted
    ).count();
    std::cout << "{\"migrationMs\":" << openMs
              << ",\"statsQueries\":2000"
              << ",\"statsQueryTotalUs\":" << queryUs
              << ",\"pendingCount\":" << actualPending
              << ",\"pendingBytes\":" << actualBytes
              << ",\"migrationDone\":" << migrationDone << "}" << std::endl;
}

void benchmarkReplaySelection(
    const std::string& libraryPath,
    const std::string& path,
    std::size_t rowCount,
    std::size_t replayCount
) {
    removeDatabase(path);
    const auto seedStarted = std::chrono::steady_clock::now();
    auto outbox = makeOutbox(path, libraryPath, 0, std::max<std::size_t>(1, replayCount));
    std::vector<edge_gateway::MqttEventOutbox::EventMessage> events;
    events.reserve(rowCount);
    for (std::size_t index = 0; index < rowCount; ++index) {
        events.push_back(event(
            "benchmark-" + std::to_string(index),
            "main",
            "change",
            "edge/event/change",
            std::string(280, 'x'),
            static_cast<std::int64_t>(index + 1)
        ));
    }
    require(outbox->enqueueBatch(events).size() == rowCount,
        "failed to seed replay selection benchmark");
    const auto seedFinished = std::chrono::steady_clock::now();
    const auto replayStarted = std::chrono::steady_clock::now();
    const auto stats = outbox->replayWithStats(
        "main",
        included({"alarm", "change"}),
        0,
        replayCount,
        [](const std::string&, const std::string&) {}
    );
    const auto replayFinished = std::chrono::steady_clock::now();
    const auto seedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        seedFinished - seedStarted
    ).count();
    const auto replayMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        replayFinished - replayStarted
    ).count();
    std::cout << "{\"seedRows\":" << rowCount
              << ",\"seedMs\":" << seedMs
              << ",\"requestedReplay\":" << replayCount
              << ",\"replayed\":" << stats.count
              << ",\"replayMs\":" << replayMs
              << ",\"pending\":" << outbox->pendingCount("main") << "}" << std::endl;
    removeDatabase(path);
}

}  // namespace

int main(int argc, char** argv) {
    const char* environmentLibrary = std::getenv("SQLITE3_LIBRARY_PATH");
    const std::string libraryPath = argc > 1
        ? argv[1]
        : (environmentLibrary == nullptr ? std::string() : std::string(environmentLibrary));
    try {
        if (argc == 4 && std::string(argv[2]) == "--probe") {
            probeDatabase(libraryPath, argv[3]);
            return 0;
        }
        if (argc == 6 && std::string(argv[2]) == "--replay-benchmark") {
            benchmarkReplaySelection(
                libraryPath,
                argv[3],
                static_cast<std::size_t>(std::stoull(argv[4])),
                static_cast<std::size_t>(std::stoull(argv[5]))
            );
            return 0;
        }
        testStorageProfiles(libraryPath);
        testLegacySchemaMigration(libraryPath);
        testCompletedMigrationDoesNotRewriteRows(libraryPath);
        testConcurrentSchemaStartupWaitsForLock(libraryPath);
        testTargetIsolationAndIdempotency(libraryPath);
        testFanoutAndStateTransaction(libraryPath);
        testEventTypeConsumerIsolation(libraryPath);
        testReplayKeepsPriorityAcrossIndexedEventTypes(libraryPath);
        testReplayBatchAcknowledgesPrefixAndReleasesRemainder(libraryPath);
        testReplayPublisherBatchFailureReleasesWholeClaim(libraryPath);
        testReplayPublisherSecondSubBatchAcknowledgesPrefix(libraryPath);
        testReplayByteLimitClaimsOnlySendablePrefix(libraryPath);
        testReplayClaimProtectsRowUntilAcknowledged(libraryPath);
        testExpiredReplayClaimIsReclaimedWithoutStartupCleanup(libraryPath);
        testUnexpiredReplayClaimIsNotStolen(libraryPath);
        testAlarmIsNeverPruned(libraryPath);
        testFanoutSavepointIsolatesFullOptionalTarget(libraryPath);
        testIncrementalStatsTrackDuplicateFanoutAndAck(libraryPath);
        testStatsMigrationResumesWithoutDoubleCounting(libraryPath);
        testStatsMigrationAckWatermarkBoundaries(libraryPath);
        testPendingCountDoesNotReadTheEventTable(libraryPath);
#ifndef _WIN32
        testReplayRetriesCommitBusyWithoutRepublishing(libraryPath);
        testConcurrentEnqueueAndReplayAvoidWriterStarvation(libraryPath);
        testConcurrentProcessesKeepStatsConsistent(libraryPath);
        testStatsMigrationRecoversAfterProcessKill(libraryPath);
#endif
        testBusyDatabaseRetriesWholeEnqueueTransaction(libraryPath);
        testPersistentBusyDatabaseStillFailsWithinBoundedTime(libraryPath);
        std::cout << "mqtt_event_outbox_target_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "mqtt_event_outbox_target_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
