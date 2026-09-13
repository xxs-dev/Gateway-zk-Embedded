#include <chrono>
#include <cstdio>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <unistd.h>

#include "edge_gateway/can_driver_service.hpp"
#include "edge_gateway/gateway_daemon.hpp"
#include "edge_gateway/common/persistent_flush_schedule.hpp"

namespace {

using namespace edge_gateway;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout = 2000ms) {
    const auto deadline = Clock::now() + timeout;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(10ms);
    } while (Clock::now() < deadline);
    return predicate();
}

class Database {
public:
    explicit Database(const std::string& path) {
        library_ = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
        require(library_ != nullptr, "local SQLite library unavailable");
        const auto open = reinterpret_cast<int (*)(const char*, void**)>(dlsym(library_, "sqlite3_open"));
        exec_ = reinterpret_cast<Exec>(dlsym(library_, "sqlite3_exec"));
        close_ = reinterpret_cast<int (*)(void*)>(dlsym(library_, "sqlite3_close"));
        require(open && exec_ && close_, "SQLite query symbols unavailable");
        require(open(path.c_str(), &db_) == 0, "cannot open test database");
    }

    ~Database() {
        if (db_) close_(db_);
        if (library_) dlclose(library_);
    }

    int rows() const {
        int count = -1;
        require(exec_(db_, "SELECT COUNT(*) FROM point_samples;",
            [](void* result, int, char** values, char**) {
                *static_cast<int*>(result) = std::stoi(values[0]);
                return 0;
            }, &count, nullptr) == 0, "cannot query persisted rows");
        return count;
    }

    void execute(const std::string& sql) const {
        require(exec_(db_, sql.c_str(), nullptr, nullptr, nullptr) == 0, "test SQL failed: " + sql);
    }

private:
    using Exec = int (*)(void*, const char*, int (*)(void*, int, char**, char**), void*, char**);
    void* library_ = nullptr;
    void* db_ = nullptr;
    Exec exec_ = nullptr;
    int (*close_)(void*) = nullptr;
};

class FakeCollector : public ICollector {
public:
    explicit FakeCollector(std::function<void()> collect) : collect_(std::move(collect)) {}
    CollectCycleResult collectOnce(std::int64_t, bool) override { collect_(); return {}; }
    void publishDeviceOnlineStatus(bool, std::int64_t) const override {}

private:
    std::function<void()> collect_;
};

class FailureApi {
public:
    explicit FailureApi(const std::string& libraryPath) {
        library_ = dlopen(libraryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
        require(library_ != nullptr, "cannot load SQLite failure fixture");
        reset = load<void (*)()>("fake_sqlite_reset");
        failCommit = load<void (*)(int)>("fake_sqlite_set_commit_failure");
        rollbacks = load<int (*)()>("fake_sqlite_rollback_count");
        finalized = load<int (*)()>("fake_sqlite_finalize_count");
        executions = load<int (*)()>("fake_sqlite_exec_count");
    }
    ~FailureApi() { dlclose(library_); }
    void (*reset)() = nullptr;
    void (*failCommit)(int) = nullptr;
    int (*rollbacks)() = nullptr;
    int (*finalized)() = nullptr;
    int (*executions)() = nullptr;

private:
    template <typename Function>
    Function load(const char* symbol) {
        auto* value = dlsym(library_, symbol);
        require(value != nullptr, std::string("missing failure fixture symbol: ") + symbol);
        return reinterpret_cast<Function>(value);
    }
    void* library_ = nullptr;
};

int availableLoopbackPort() {
    const auto fd = socket(AF_INET, SOCK_DGRAM, 0);
    require(fd >= 0, "cannot allocate loopback port");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    const bool ok = bind(fd, reinterpret_cast<sockaddr*>(&address), size) == 0 &&
        getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0;
    close(fd);
    require(ok, "cannot reserve loopback test address");
    return ntohs(address.sin_port);
}

class ServiceFixture {
public:
    explicit ServiceFixture(bool can, std::size_t limit = 16, int intervalMs = 60000,
                            const std::string& sqliteLibrary = {}, int retentionDays = 30) {
        static unsigned sequence = 0;
        name_ = "persist_pressure_" + std::to_string(getpid()) + "_" + std::to_string(++sequence);
        config.machineCode = "PRESSURE_TEST";
        config.meterCode = "FAKE_METER";
        config.protocol.type = can ? "can_socketcan" : "modbus_rtu";
        config.collect.defaultIntervalMs = 50;
        config.memoryStore.sharedMemoryName = name_;
        config.memoryStore.maxLatestPoints = 16;
        config.memoryStore.maxPendingWrites = 4;
        config.memoryStore.maxPersistentSamples = limit;
        config.memoryStore.persistFlushIntervalMs = intervalMs;
        config.memoryStore.sqlitePath = name_ + ".db";
        config.memoryStore.sqliteLibraryPath = sqliteLibrary;
        config.memoryStore.historyRetentionDays = retentionDays;
        config.mqttDriver.priorityControlLeaseFile = name_ + "_priority.json";
        config.mqttDriver.powerControlOwnershipFile = name_ + "_owner.json";
        store = std::make_unique<MemoryPointStore>(config.memoryStore);
        if (can) {
            config.protocol.can.transportMode = "udp_test";
            config.protocol.can.manageInterface = false;
            config.protocol.can.udpBindAddress = "127.0.0.1";
            config.protocol.can.udpPeerAddress = "127.0.0.1";
            config.protocol.can.udpListenPort = availableLoopbackPort();
            config.protocol.can.udpPeerPort = config.protocol.can.udpListenPort;
            can_ = std::make_unique<CanDriverService>(config, *store);
        } else {
            gateway_ = std::make_unique<GatewayDaemon>(config, *store,
                [this](const DeviceConfig&, MemoryPointStore&) {
                    return std::make_unique<FakeCollector>([this] {
                        const auto count = toCollect_.exchange(0);
                        if (count > 0) append(count);
                    });
                },
                [](const DeviceConfig& device, MemoryPointStore&) {
                    return std::make_unique<UnsupportedCommandExecutor>(device);
                }, GatewayDaemon::ServiceStartStop{});
        }
        if (sqliteLibrary.empty()) database = std::make_unique<Database>(config.memoryStore.sqlitePath);
    }

    ~ServiceFixture() {
        stop();
        database.reset();
        can_.reset();
        gateway_.reset();
        store.reset();
        MemoryPointStore::cleanupOrphanedSegment(name_);
        for (const auto& path : {config.memoryStore.sqlitePath, config.mqttDriver.priorityControlLeaseFile,
                                config.mqttDriver.powerControlOwnershipFile}) {
            for (const auto* suffix : {"", "-wal", "-shm", ".lock"}) {
                std::remove((path + suffix).c_str());
            }
        }
    }

    void start() { if (can_) can_->start(); else gateway_->start(); }
    void stop() { if (can_) can_->stop(); else if (gateway_) gateway_->stop(); }

    void collectOrdinary(std::size_t count) {
        toCollect_ = count;
        gateway_->collectOnce(1000);
    }

    void flushImmediate() {
        PointValue value;
        value.index = 1002;
        value.machineCode = config.machineCode;
        value.meterCode = config.meterCode;
        value.pointCode = "IMMEDIATE_HISTORY";
        value.value = 1;
        value.ts = sampleBaseTs_;
        value.quality = 1;
        value.isStore = true;
        value.persistOnChange = true;
        store->putLatest(value);
        require(store->immediatePersistentGeneration() > 0, "immediate history not flagged");
        if (can_) can_->processWritebackOnce(sampleBaseTs_);
        else gateway_->collectOnce(sampleBaseTs_);
    }

    void blockControl() {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        PriorityControlLease(config.mqttDriver.priorityControlLeaseFile, "external-test-owner")
            .acquire("PRESSURE_CONTROL", config.meterCode, 1001, now, 30000);
    }

    void releaseControl() {
        PriorityControlLease(config.mqttDriver.priorityControlLeaseFile, "external-test-owner")
            .release("PRESSURE_CONTROL");
    }

    void append(std::size_t count, int ageDays = 0) {
        for (std::size_t i = 0; i < count; ++i) {
            PointValue value;
            value.index = 1001;
            value.machineCode = config.machineCode;
            value.meterCode = config.meterCode;
            value.pointCode = "ORDINARY_HISTORY";
            value.value = ++samples_;
            value.ts = sampleBaseTs_ + 1000 * samples_ - static_cast<std::int64_t>(ageDays) * 86400000;
            value.quality = 1;
            value.isStore = true;
            value.persistOnChange = false;
            value.persistIntervalSec = 1;
            store->putLatest(value);
        }
        require(store->immediatePersistentGeneration() == 0, "ordinary history requested an immediate flush");
    }

    DeviceConfig config;
    std::unique_ptr<MemoryPointStore> store;
    std::unique_ptr<Database> database;

private:
    std::string name_;
    std::int64_t samples_ = 0;
    const std::int64_t sampleBaseTs_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::atomic<std::size_t> toCollect_{0};
    std::unique_ptr<GatewayDaemon> gateway_;
    std::unique_ptr<CanDriverService> can_;
};

void verifyHighWaterBeforePeriodicDeadline(bool can) {
    ServiceFixture fixture(can);
    fixture.start();
    // Let the original startup flush finish before producing ordinary history.
    std::this_thread::sleep_for(400ms);
    fixture.append(12);
    require(waitUntil([&] { return fixture.store->getStats().persistentCount == 0; }),
            "75% ordinary history must flush before the configured 60000ms deadline");
    require(fixture.database->rows() == 12, "background flush must commit all ordinary samples to SQLite");
    require(fixture.store->consumePersistentDropCount() == 0, "history was dropped before persistence");
}

void verifyEmptyRingRetention(bool can) {
    ServiceFixture fixture(can);
    fixture.database->execute("INSERT INTO point_samples VALUES(1, 1, 1);");
    require(fixture.store->getStats().persistentCount == 0, "retention fixture ring must be empty");
    fixture.start();
    require(waitUntil([&] { return fixture.database->rows() == 0; }),
            "empty-ring background must reclaim expired SQLite history");
}

void verifyRetentionBatchesPriorityAndCustomDays(bool can) {
    ServiceFixture fixture(can, 16, 60000, {}, 2);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    fixture.database->execute("WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE i<1200) "
        "INSERT INTO point_samples SELECT i," + std::to_string(now - 3LL * 86400000) + ",1 FROM n;");
    fixture.database->execute("INSERT INTO point_samples VALUES(2001," + std::to_string(now - 86400000) +
        ",1),(2002," + std::to_string(now + 86400000) + ",1);");
    fixture.blockControl();
    fixture.append(12);
    fixture.start();
    std::this_thread::sleep_for(650ms);
    require(fixture.database->rows() == 1202 && fixture.store->getStats().persistentCount == 12,
            "control veto must cover both retention and pending writes");
    fixture.releaseControl();
    require(waitUntil([&] { return fixture.store->getStats().persistentCount == 0 && fixture.database->rows() == 702; }),
            "release must commit/ACK first and reclaim only one 512-row batch");
    std::this_thread::sleep_for(350ms);
    require(fixture.database->rows() == 702, "background cleanup loop exceeded one batch per second");
    require(waitUntil([&] { return fixture.database->rows() == 14; }, 3000ms),
            "empty-ring continuation must reclaim remaining batches using custom retention");
    require(fixture.store->consumePersistentDropCount() == 0, "cleanup disturbed history ring accounting");
}

void verifyCleanupFailureDoesNotUndoAck(bool can) {
    ServiceFixture fixture(can);
    fixture.database->execute("INSERT INTO point_samples VALUES(1,1,1); "
        "CREATE TRIGGER fail_cleanup BEFORE DELETE ON point_samples "
        "BEGIN SELECT RAISE(ABORT,'injected background cleanup failure'); END;");
    fixture.append(12);
    fixture.start();
    require(waitUntil([&] { return fixture.store->getStats().persistentCount == 0; }),
            "cleanup failure must not turn committed history into unacknowledged samples");
    std::this_thread::sleep_for(650ms);
    require(fixture.database->rows() == 13, "cleanup failure changed committed data");
    fixture.database->execute("DROP TRIGGER fail_cleanup;");
    fixture.append(12);
    require(waitUntil([&] { return fixture.store->getStats().persistentCount == 0; }),
            "cleanup retry delay must not delay high-water write/ACK");
    std::this_thread::sleep_for(650ms);
    require(fixture.database->rows() == 25, "cleanup retried immediately after failure");
    require(waitUntil([&] { return fixture.database->rows() == 24; }, 5000ms),
            "failed cleanup must resume with empty ring after backoff");
}

void verifyRealWriteFailureAndRetentionRecovery(bool can) {
    ServiceFixture fixture(can);
    fixture.database->execute("INSERT INTO point_samples VALUES(1,1,1); "
        "CREATE TRIGGER fail_write BEFORE INSERT ON point_samples "
        "BEGIN SELECT RAISE(ABORT,'injected background write failure'); END;");
    fixture.append(12);
    const auto pending = fixture.store->peekPersistentSamples();
    fixture.start();
    require(waitUntil([&] { return fixture.database->rows() == 0; }),
            "write failure must not disable independent retention");
    require(fixture.store->getStats().persistentCount == 12, "failed write acknowledged ring");
    fixture.blockControl();
    fixture.database->execute("DROP TRIGGER fail_write;");
    std::this_thread::sleep_for(1300ms);
    require(fixture.database->rows() == 0 && fixture.store->peekPersistentSamples().front().sequence == pending.front().sequence,
            "failed write retry bypassed priority or changed pending history");
    fixture.releaseControl();
    require(waitUntil([&] { return fixture.store->getStats().persistentCount == 0; }),
            "real failed write did not recover and ACK");
    require(fixture.database->rows() == 12, "write recovery did not commit retained batch");
}

void verifyExpiredFlaggedRingAndImmediateIsolation(bool can) {
    ServiceFixture fixture(can);
    fixture.append(12, 31);
    fixture.start();
    require(waitUntil([&] { return fixture.store->getStats().persistentCount == 0; }), "expired flagged ring was not ACKed");
    require(waitUntil([&] { return fixture.database->rows() == 0; }), "expired flagged ring history escaped retention");
    fixture.stop();
    // The stopped service's immediate write path must never perform retention.
    fixture.database->execute("INSERT INTO point_samples VALUES(1,1,1);");
    fixture.flushImmediate();
    require(fixture.database->rows() == 2 && fixture.store->getStats().persistentCount == 0,
            "immediate write must commit/ACK without inline retention");
}

void verifyScheduleBoundaries() {
    using Schedule = PersistentFlushSchedule;
    const auto start = Clock::time_point{};
    for (const auto limit : {std::size_t(0), std::size_t(1), std::size_t(2), std::size_t(3),
                            std::size_t(4), std::size_t(5), std::size_t(19999),
                            std::numeric_limits<std::size_t>::max()}) {
        const auto expected = std::max<std::size_t>(1, (limit / 4) * 3 + ((limit % 4) * 3 + 3) / 4);
        require(Schedule::highWatermark(limit) == expected, "75% ceil threshold or overflow boundary failed");
        Schedule schedule(60000, start);
        require(!schedule.shouldFlush(0, limit, start), "empty startup must not flush");
        require(!schedule.shouldFlush(expected - 1, limit, start + 250ms), "below threshold flushed early");
        require(schedule.shouldFlush(expected, limit, start + 500ms), "exact threshold did not flush");
        require(schedule.shouldFlush(expected + 1, limit, start + 750ms), "above threshold did not flush");
    }
    Schedule periodic(60000, start);
    require(periodic.shouldFlush(1, 16, start), "startup must still flush pre-existing low water");
    periodic.succeeded();
    require(!periodic.shouldFlush(1, 16, start + 59999ms), "configured period was shortened");
    require(periodic.shouldFlush(12, 16, start + 59999ms), "pressure trigger missing");
    periodic.succeeded();
    require(periodic.shouldFlush(1, 16, start + 60000ms), "pressure flush postponed periodic low water");
    periodic.failed(start + 60000ms);
    require(!periodic.shouldFlush(16, 16, start + 60999ms), "pressure bypassed 1s failure backoff");
    require(periodic.shouldFlush(1, 16, start + 61000ms), "failed batch must retry even below high water");
    periodic.failed(start + 61000ms);
    require(!periodic.shouldFlush(1, 16, start + 61999ms), "repeat failure bypassed backoff");
    require(periodic.shouldFlush(1, 16, start + 62000ms), "repeat retry was lost");
    periodic.succeeded();
    require(!periodic.shouldFlush(1, 16, start + 62250ms), "success did not clear retry state");
    for (const auto interval : {-1, 0, 500, 1000, 1750}) {
        Schedule bounded(interval, start);
        require(!bounded.shouldFlush(0, 16, start), "empty tick flushed");
        const auto deadline = start + std::chrono::milliseconds(std::max(1000, interval));
        require(!bounded.shouldFlush(1, 16, deadline - 1ms), "existing minimum interval changed");
        require(bounded.shouldFlush(1, 16, deadline), "configured short interval did not fire");
    }
}

void verifyEffectiveLimitBoundaries(bool can) {
    for (const auto configured : {std::size_t(0), std::size_t(1), std::size_t(4), std::size_t(5),
                                 std::numeric_limits<std::size_t>::max()}) {
        ServiceFixture fixture(can, configured);
        const auto stats = fixture.store->getStats();
        const auto effective = std::max<std::size_t>(1, std::min(configured, stats.persistentCapacity));
        require(stats.persistentConfiguredLimit == effective, "store effective limit was not clamped");
        const auto threshold = (effective * 3 + 3) / 4;
        fixture.start();
        std::this_thread::sleep_for(400ms);
        fixture.append(threshold - 1);
        std::this_thread::sleep_for(400ms);
        require(fixture.store->getStats().persistentCount == threshold - 1,
                "caller flushed ordinary samples below the effective 75% limit");
        require(fixture.database->rows() == 0, "below-threshold caller wrote SQLite early");
        fixture.append(1);
        require(waitUntil([&] { return fixture.store->getStats().persistentCount == 0; }),
                "caller did not flush at the exact effective 75% limit");
        require(fixture.database->rows() == static_cast<int>(threshold), "boundary flush lost rows");
        require(fixture.store->consumePersistentDropCount() == 0, "boundary flush dropped history");
    }
}

void verifyLowWaterTimerAndLifecycle(bool can) {
    ServiceFixture fixture(can, 16, 1750);
    fixture.append(1);
    fixture.start();
    require(waitUntil([&] { return fixture.database->rows() == 1; }, 1000ms), "startup low water was delayed");
    fixture.append(1);
    std::this_thread::sleep_for(600ms);
    require(fixture.database->rows() == 1 && fixture.store->getStats().persistentCount == 1,
            "low water was flushed before the configured period");
    require(waitUntil([&] { return fixture.store->getStats().persistentCount == 0; }),
            "low water did not flush at the configured period");
    require(fixture.database->rows() == 2, "periodic flush did not commit");
    fixture.append(1);
    const auto beforeStop = Clock::now();
    fixture.stop();
    require(Clock::now() - beforeStop < 1000ms, "stop waited for the persistence period");
    require(fixture.database->rows() == 2 && fixture.store->getStats().persistentCount == 1,
            "stop introduced an unrequested final drain");
    fixture.start();
    require(waitUntil([&] { return fixture.store->getStats().persistentCount == 0; }, 1000ms),
            "restart must preserve startup flush behavior");
    require(fixture.database->rows() == 3, "restart lost pending history");
}

void verifyPriorityBlocksBothTriggers(bool can) {
    for (const auto count : {std::size_t(1), std::size_t(12)}) {
        ServiceFixture fixture(can, 16, 1000);
        fixture.start();
        std::this_thread::sleep_for(400ms);
        fixture.blockControl();
        fixture.append(count);
        std::this_thread::sleep_for(1300ms);
        require(fixture.store->getStats().persistentCount == count && fixture.database->rows() == 0,
                "background pressure or timer bypassed priority control");
        fixture.releaseControl();
        require(waitUntil([&] { return fixture.store->getStats().persistentCount == 0; }),
                "pending flush was not resumed after priority control release");
        require(fixture.database->rows() == static_cast<int>(count), "release flush lost rows");
    }
}

void verifyOrdinaryHistoryPersistsBeforeFull(bool can) {
    ServiceFixture fixture(can);
    fixture.start();
    std::this_thread::sleep_for(400ms);
    for (int sample = 0; sample < 40; ++sample) {
        fixture.append(1);
        require(fixture.store->getStats().persistentCount < 16, "paced ordinary history filled the ring");
        std::this_thread::sleep_for(75ms);
    }
    fixture.stop();
    const auto pending = fixture.store->getStats().persistentCount;
    const auto persisted = fixture.database->rows();
    require(persisted >= 24, "ordinary history did not persist repeatedly before 60 seconds");
    require(persisted + pending == 40, "ordinary history accounting mismatch");
    require(fixture.store->consumePersistentDropCount() == 0, "paced ordinary history was dropped");
}

void verifyFailureBackoffAndEmptyWork(bool can, const std::string& library) {
    FailureApi api(library);
    if (!can) {
        api.reset();
        ServiceFixture fixture(false, 16, 60000, library);
        const auto schemaExecutions = api.executions();
        fixture.collectOrdinary(12);
        require(fixture.store->getStats().persistentCount == 12 && api.executions() == schemaExecutions,
                "ordinary high water performed synchronous SQLite work on collection");
    }
    api.reset();
    {
        ServiceFixture empty(can, 16, 1000, library);
        const auto startupExecutions = api.executions();
        empty.start();
        std::this_thread::sleep_for(1300ms);
        empty.stop();
        require(api.executions() == startupExecutions && api.finalized() == 1,
                "empty ring must do only one bounded startup cleanup, no periodic writes");
    }
    for (const auto count : {std::size_t(1), std::size_t(12)}) {
        api.reset();
        ServiceFixture fixture(can, 16, count == 1 ? 1000 : 60000, library);
        fixture.start();
        std::this_thread::sleep_for(400ms);
        const auto cleanupFinalized = api.finalized();
        require(cleanupFinalized == 1, "startup cleanup was not exercised");
        fixture.append(count);
        const auto pending = fixture.store->peekPersistentSamples();
        require(waitUntil([&] { return api.rollbacks() == 1; }), "COMMIT failure was not exercised");
        require(fixture.store->getStats().persistentCount == count, "COMMIT failure acknowledged pending history");
        std::this_thread::sleep_for(700ms);
        require(api.finalized() == cleanupFinalized + 1 && api.rollbacks() == 1, "failed background flush retried before 1 second");
        require(waitUntil([&] { return api.rollbacks() == 2; }), "failed batch was not retried after backoff");
        const auto retained = fixture.store->peekPersistentSamples();
        require(retained.size() == pending.size() && retained.front().sequence == pending.front().sequence &&
                retained.back().sequence == pending.back().sequence, "failed retry changed unacknowledged sequences");
        fixture.blockControl();
        api.failCommit(0);
        std::this_thread::sleep_for(1300ms);
        require(api.finalized() == cleanupFinalized + 2 && fixture.store->getStats().persistentCount == count,
                "failure retry bypassed priority control");
        fixture.releaseControl();
        require(waitUntil([&] { return fixture.store->getStats().persistentCount == 0; }),
                "recovered COMMIT did not acknowledge retained history");
        require(api.finalized() == cleanupFinalized + 3 && api.rollbacks() == 2, "recovery did not commit exactly one retained batch");
        std::this_thread::sleep_for(1100ms);
        require(api.finalized() == cleanupFinalized + 3, "acknowledged history was written again");
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3 || argc == 4, "usage: persistent_flush_pressure_test gateway|can <sqlite-failure-fixture> [retention]");
        const std::string path = argv[1];
        require(path == "gateway" || path == "can", "unknown caller path");
        const bool can = path == "can";
        const auto run = [&](const char* name, const std::function<void()>& test) {
            test();
            std::cout << path << " " << name << " passed" << std::endl;
        };
        if (argc == 4) {
            require(std::string(argv[3]) == "retention", "unknown test selection");
            run("empty ring retention", [&] { verifyEmptyRingRetention(can); });
            run("retention batches priority and custom days", [&] { verifyRetentionBatchesPriorityAndCustomDays(can); });
            run("cleanup failure preserves ACK and write scheduling", [&] { verifyCleanupFailureDoesNotUndoAck(can); });
            run("real write failure priority and retention recovery", [&] { verifyRealWriteFailureAndRetentionRecovery(can); });
            run("expired flagged ring and inline isolation", [&] { verifyExpiredFlaggedRingAndImmediateIsolation(can); });
            return 0;
        }
        run("schedule boundaries", verifyScheduleBoundaries);
        run("high water before 60s", [&] { verifyHighWaterBeforePeriodicDeadline(can); });
        run("effective limit boundaries", [&] { verifyEffectiveLimitBoundaries(can); });
        run("low water timer and lifecycle", [&] { verifyLowWaterTimerAndLifecycle(can); });
        run("priority blocks pressure and timer", [&] { verifyPriorityBlocksBothTriggers(can); });
        run("ordinary history before full", [&] { verifyOrdinaryHistoryPersistsBeforeFull(can); });
        run("failure no-ack, backoff, priority and empty work", [&] { verifyFailureBackoffAndEmptyWork(can, argv[2]); });
        std::cout << path << " persistent_flush_pressure_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "persistent_flush_pressure_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
