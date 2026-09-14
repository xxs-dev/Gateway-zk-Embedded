#include <cstdio>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <unistd.h>

#include "edge_gateway/event_engine_service.hpp"
#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/point_store_router.hpp"
#include "edge_gateway/sqlite_alarm_writer.hpp"
#include "edge_gateway/sqlite_sample_writer.hpp"

namespace {

struct sqlite3;

void require(bool condition, const std::string& message);

class SqliteApi {
public:
    SqliteApi() {
        handle_ = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
        if (handle_ == nullptr) {
            handle_ = dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
        }
        require(handle_ != nullptr, "failed to load real sqlite library");
        open = load<int (*)(const char*, sqlite3**)>("sqlite3_open");
        close = load<int (*)(sqlite3*)>("sqlite3_close");
        exec = load<int (*)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**)>(
            "sqlite3_exec"
        );
        free = load<void (*)(void*)>("sqlite3_free");
    }

    ~SqliteApi() {
        if (handle_ != nullptr) {
            dlclose(handle_);
        }
    }

    SqliteApi(const SqliteApi&) = delete;
    SqliteApi& operator=(const SqliteApi&) = delete;

    int (*open)(const char*, sqlite3**) = nullptr;
    int (*close)(sqlite3*) = nullptr;
    int (*exec)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**) = nullptr;
    void (*free)(void*) = nullptr;

private:
    template <typename Function>
    Function load(const char* name) {
        auto* symbol = dlsym(handle_, name);
        require(symbol != nullptr, std::string("failed to load sqlite symbol: ") + name);
        return reinterpret_cast<Function>(symbol);
    }

    void* handle_ = nullptr;
};

void execSql(SqliteApi& api, sqlite3* db, const std::string& sql) {
    char* error = nullptr;
    const auto rc = api.exec(db, sql.c_str(), nullptr, nullptr, &error);
    const std::string message = error == nullptr ? std::string() : std::string(error);
    if (error != nullptr) {
        api.free(error);
    }
    require(rc == 0, "sqlite exec failed: " + message);
}

int queryInt(SqliteApi& api, sqlite3* db, const std::string& sql) {
    int value = -1;
    char* error = nullptr;
    const auto callback = [](void* context, int columns, char** values, char**) -> int {
        if (columns > 0 && values[0] != nullptr) {
            *static_cast<int*>(context) = std::stoi(values[0]);
        }
        return 0;
    };
    const auto rc = api.exec(db, sql.c_str(), callback, &value, &error);
    const std::string message = error == nullptr ? std::string() : std::string(error);
    if (error != nullptr) {
        api.free(error);
    }
    require(rc == 0, "sqlite query failed: " + message);
    return value;
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class FixtureApi {
public:
    explicit FixtureApi(const char* libraryPath) {
        handle_ = dlopen(libraryPath, RTLD_NOW | RTLD_LOCAL);
        require(handle_ != nullptr, "failed to load sqlite failure fixture");
        reset = load<void (*)()>("fake_sqlite_reset");
        setCommitFailure = load<void (*)(int)>("fake_sqlite_set_commit_failure");
        finalizeCount = load<int (*)()>("fake_sqlite_finalize_count");
        doubleFinalizeCount = load<int (*)()>("fake_sqlite_double_finalize_count");
        rollbackCount = load<int (*)()>("fake_sqlite_rollback_count");
    }

    ~FixtureApi() {
        if (handle_ != nullptr) {
            dlclose(handle_);
        }
    }

    FixtureApi(const FixtureApi&) = delete;
    FixtureApi& operator=(const FixtureApi&) = delete;

    void (*reset)() = nullptr;
    void (*setCommitFailure)(int) = nullptr;
    int (*finalizeCount)() = nullptr;
    int (*doubleFinalizeCount)() = nullptr;
    int (*rollbackCount)() = nullptr;

private:
    template <typename Function>
    Function load(const char* name) {
        auto* symbol = dlsym(handle_, name);
        require(symbol != nullptr, std::string("failed to load fixture symbol: ") + name);
        return reinterpret_cast<Function>(symbol);
    }

    void* handle_ = nullptr;
};

class CapturingPublisher : public edge_gateway::IMqttDriverPublisher {
public:
    void publishFullSnapshot(const std::string&, const std::vector<edge_gateway::StoredPointValue>&, const std::string&) override {}
    void publishAlarm(const std::string&, std::uint32_t, const edge_gateway::StoredPointValue&, const std::string&, bool) override {}
    void publishOnDemand(const std::string&, const std::vector<edge_gateway::StoredPointValue>&, const std::string&) override {}
    void publishChangeEvent(const std::string&, const edge_gateway::StoredPointValue&) override {}
    void publishCommandReply(const std::string&, const edge_gateway::MqttCommandReply&) override {}
    void publishOtaReply(const std::string&, const edge_gateway::OtaReply&) override {}
    void publishOtaStatus(const std::string&, const edge_gateway::OtaStatus&) override {}
    void publishJsonMessage(const std::string&, const std::string& payload) override {
        if (payload.find("\"type\":\"alarm\"") != std::string::npos) {
            ++alarmCount;
        }
    }
    std::vector<edge_gateway::MqttIncomingMessage> pollIncoming(int) override { return {}; }

    int alarmCount = 0;
};

template <typename WriteOperation>
void verifyCommitFailure(FixtureApi& fixture, WriteOperation write, const std::string& writerName) {
    fixture.reset();
    bool commitFailed = false;
    try {
        write();
    } catch (const std::exception& ex) {
        commitFailed = std::string(ex.what()) == "injected commit failure";
    }

    require(commitFailed, writerName + " should surface the injected COMMIT failure");
    require(fixture.finalizeCount() == 1, writerName + " should finalize its statement exactly once");
    require(fixture.doubleFinalizeCount() == 0, writerName + " double-finalized its statement");
    require(fixture.rollbackCount() == 1, writerName + " should roll back after COMMIT failure");
}

void verifySampleWriter(FixtureApi& fixture, const std::string& libraryPath) {
    edge_gateway::SqliteSampleWriter writer("fake-samples.db", libraryPath);
    verifyCommitFailure(
        fixture,
        [&writer]() {
            writer.writeSamples({edge_gateway::PersistentPointSample{1001, 1.0, 1000}});
        },
        "sample writer"
    );

    fixture.setCommitFailure(0);
    writer.writeSamples({edge_gateway::PersistentPointSample{1002, 2.0, 2000}});
    require(fixture.doubleFinalizeCount() == 0, "sample writer should remain usable after rollback");
}

void verifyAlarmWriter(FixtureApi& fixture, const std::string& libraryPath) {
    edge_gateway::SqliteAlarmWriter writer("fake-alarms.db", libraryPath);
    edge_gateway::AlarmEvent event;
    event.index = 2001;
    event.machineCode = "GW_TEST";
    event.meterCode = "METER_TEST";
    event.pointCode = "ALARM_TEST";
    event.alarmType = "high";
    event.active = true;
    event.ts = 1000;

    verifyCommitFailure(fixture, [&writer, &event]() { writer.writeEvents({event}); }, "alarm writer");

    fixture.setCommitFailure(0);
    writer.writeEvents({event});
    require(fixture.doubleFinalizeCount() == 0, "alarm writer should remain usable after rollback");
}

void verifyEventEngineRetriesAlarmPersistence(FixtureApi& fixture, const std::string& libraryPath) {
    using namespace edge_gateway;
    const std::string storeName = "event_engine_alarm_retry_test";
    MemoryPointStore::cleanupOrphanedSegment(storeName);

    MemoryStoreConfig storeConfig;
    storeConfig.sharedMemoryName = storeName;
    storeConfig.maxLatestPoints = 16;
    MemoryPointStore store(storeConfig);

    PointDefinition point;
    point.index = 410001;
    point.pointCode = "ALARM_RETRY";
    point.enabled = true;
    point.read.enable = true;
    point.read.cachePolicy.ttlMs = 60000;
    AlarmRuleConfig alarm;
    alarm.type = "high";
    alarm.threshold = 50.0;
    alarm.persistValue = "alarm";
    point.alarms.push_back(alarm);

    DeviceConfig device;
    device.machineCode = "GW_RETRY";
    device.meterCode = "METER_RETRY";
    device.memoryStore.sharedMemoryName = storeName;
    device.points.push_back(point);
    store.registerPoints(device.machineCode, device.meterCode, device.points);

    PointStoreRouter router;
    router.addStore(storeName, store);
    router.addRoutesFromDeviceConfigs({device}, storeName);
    EventEngineConfig eventConfig;
    eventConfig.enabled = true;
    eventConfig.scanFallbackIntervalMs = 5000;
    MqttConfig mqttConfig;
    mqttConfig.alarmTopic = "edge/alarm";
    auto publisher = std::make_shared<CapturingPublisher>();
    auto writer = std::unique_ptr<SqliteAlarmWriter>(
        new SqliteAlarmWriter("fake-event-alarms.db", libraryPath)
    );
    EventEngineService service(
        eventConfig,
        mqttConfig,
        {device},
        router,
        {&store},
        publisher,
        nullptr,
        std::move(writer)
    );

    fixture.reset();
    PointValue value;
    value.index = point.index;
    value.machineCode = device.machineCode;
    value.meterCode = device.meterCode;
    value.pointCode = point.pointCode;
    value.value = 100.0;
    value.quality = 1;
    value.ts = 1000;
    value.expireAt = 61000;
    store.putLatest(value);
    service.runOnce(1000);
    require(fixture.rollbackCount() == 1, "event engine must retain an alarm after COMMIT failure");
    require(publisher->alarmCount == 1, "alarm MQTT event must be emitted once while persistence retries");

    const auto finalizedAfterFailure = fixture.finalizeCount();
    fixture.setCommitFailure(0);
    service.runOnce(1999);
    require(
        fixture.finalizeCount() == finalizedAfterFailure,
        "event engine retried alarm persistence before the retry interval elapsed"
    );
    service.runOnce(2001);
    require(
        fixture.finalizeCount() == finalizedAfterFailure + 1,
        "event engine did not retry the retained alarm batch"
    );
    service.runOnce(2002);
    require(
        fixture.finalizeCount() == finalizedAfterFailure + 1,
        "successfully persisted alarm batch was retried again"
    );
    require(publisher->alarmCount == 1, "persistence retry duplicated the MQTT alarm event");

    MemoryPointStore::cleanupOrphanedSegment(storeName);
}

void removeDatabaseFiles(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

void verifyRealSqliteSmoke() {
    const auto prefix = std::string("/tmp/gateway_sqlite_writer_smoke_") + std::to_string(getpid());
    const auto samplePath = prefix + "_samples.db";
    const auto alarmPath = prefix + "_alarms.db";
    removeDatabaseFiles(samplePath);
    removeDatabaseFiles(alarmPath);

    {
        edge_gateway::SqliteSampleWriter writer(samplePath);
        writer.writeSamples({edge_gateway::PersistentPointSample{3001, 1.0, 1000}});
        writer.writeSamples({edge_gateway::PersistentPointSample{3001, 2.0, 1000}});
    }
    {
        edge_gateway::SqliteAlarmWriter writer(alarmPath);
        edge_gateway::AlarmEvent event;
        event.index = 4001;
        event.machineCode = "GW_TEST";
        event.meterCode = "METER_TEST";
        event.pointCode = "ALARM_TEST";
        event.alarmType = "high";
        event.ts = 1000;
        writer.writeEvents({event});
    }

    removeDatabaseFiles(samplePath);
    removeDatabaseFiles(alarmPath);
}

void verifyAlarmEventIdMigrationAndIdempotency() {
    const auto path = std::string("/tmp/gateway_alarm_event_id_") + std::to_string(getpid()) + ".db";
    removeDatabaseFiles(path);

    SqliteApi api;
    sqlite3* db = nullptr;
    require(api.open(path.c_str(), &db) == 0 && db != nullptr, "failed to create legacy alarm db");
    execSql(
        api,
        db,
        "CREATE TABLE alarm_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "point_index INTEGER NOT NULL, ts INTEGER NOT NULL, alarm_type TEXT NOT NULL,"
        "active INTEGER NOT NULL, threshold REAL NOT NULL, value REAL NOT NULL,"
        "quality INTEGER NOT NULL, stale INTEGER NOT NULL, persist_value TEXT NOT NULL,"
        "gateway_code TEXT NOT NULL, device_code TEXT NOT NULL, point_code TEXT NOT NULL);"
    );
    require(api.close(db) == 0, "failed to close legacy alarm db");

    edge_gateway::AlarmEvent event;
    event.eventId = "alarm:v1:GW_TEST:4001:high:1";
    event.index = 4001;
    event.machineCode = "GW_TEST";
    event.meterCode = "METER_TEST";
    event.pointCode = "ALARM_TEST";
    event.alarmType = "high";
    event.active = true;
    event.ts = 1000;
    {
        edge_gateway::SqliteAlarmWriter writer(path);
        writer.writeEvents({event});
        writer.writeEvents({event});
    }

    db = nullptr;
    require(api.open(path.c_str(), &db) == 0 && db != nullptr, "failed to reopen alarm db");
    require(
        queryInt(api, db, "SELECT COUNT(*) FROM alarm_events WHERE event_id='alarm:v1:GW_TEST:4001:high:1';") == 1,
        "stable alarm eventId must make a retried local alarm write idempotent"
    );
    require(api.close(db) == 0, "failed to close alarm db");
    removeDatabaseFiles(path);
}

void verifyAlarmWriterBusyDatabaseFailsWithinEventLoopBudget() {
    const auto path = std::string("/tmp/gateway_alarm_busy_budget_") + std::to_string(getpid()) + ".db";
    removeDatabaseFiles(path);

    edge_gateway::SqliteAlarmWriter writer(path);
    SqliteApi api;
    sqlite3* blocker = nullptr;
    require(api.open(path.c_str(), &blocker) == 0 && blocker != nullptr,
        "failed to open alarm busy blocker");
    execSql(api, blocker, "BEGIN EXCLUSIVE;");

    edge_gateway::AlarmEvent event;
    event.eventId = "alarm:v1:GW_BUSY:4101:high:1";
    event.index = 4101;
    event.machineCode = "GW_BUSY";
    event.meterCode = "METER_BUSY";
    event.pointCode = "ALARM_BUSY";
    event.alarmType = "high";
    event.active = true;
    event.ts = 1000;
    auto write = std::async(std::launch::async, [&writer, &event]() {
        try {
            writer.writeEvents({event});
            return false;
        } catch (const std::exception&) {
            return true;
        }
    });
    const bool completedBeforeRelease =
        write.wait_for(std::chrono::milliseconds(400)) == std::future_status::ready;
    execSql(api, blocker, "ROLLBACK;");
    const bool failedWhileBusy = write.get();

    require(completedBeforeRelease,
        "alarm sqlite contention blocked the event loop beyond 400ms");
    require(failedWhileBusy,
        "alarm writer waited for the lock and succeeded instead of failing fast");
    require(api.close(blocker) == 0, "failed to close alarm busy blocker");
    removeDatabaseFiles(path);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: sqlite_writer_failure_test <fixture-library>");
        const std::string libraryPath = argv[1];
        FixtureApi fixture(libraryPath.c_str());
        verifySampleWriter(fixture, libraryPath);
        verifyAlarmWriter(fixture, libraryPath);
        verifyEventEngineRetriesAlarmPersistence(fixture, libraryPath);
        verifyRealSqliteSmoke();
        verifyAlarmEventIdMigrationAndIdempotency();
        verifyAlarmWriterBusyDatabaseFailsWithinEventLoopBudget();
        std::cout << "sqlite_writer_failure_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "sqlite_writer_failure_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
