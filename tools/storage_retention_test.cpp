#include "edge_gateway/event_engine_service.hpp"
#include "edge_gateway/event_store.hpp"
#include "edge_gateway/sqlite_alarm_writer.hpp"

#include <algorithm>
#include <cstdio>
#include <dlfcn.h>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unistd.h>

namespace {
using namespace edge_gateway;
using namespace std::chrono_literals;
using Clock = SqliteAlarmWriter::MaintenanceClock;
constexpr std::int64_t kDay = 86400000;
constexpr std::int64_t kNow = 1800000000000;
class NoNetworkPublisher : public IMqttDriverPublisher {
public:
    void publishFullSnapshot(const std::string&, const std::vector<StoredPointValue>&, const std::string&) override {}
    void publishAlarm(const std::string&, std::uint32_t, const StoredPointValue&, const std::string&, bool) override {}
    void publishOnDemand(const std::string&, const std::vector<StoredPointValue>&, const std::string&) override {}
    void publishChangeEvent(const std::string&, const StoredPointValue&) override {}
    void publishCommandReply(const std::string&, const MqttCommandReply&) override {}
    void publishOtaReply(const std::string&, const OtaReply&) override {}
    void publishOtaStatus(const std::string&, const OtaStatus&) override {}
    void publishJsonMessage(const std::string&, const std::string&) override {}
    std::vector<MqttIncomingMessage> pollIncoming(int) override { return {}; }
};
void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
void fails(const std::function<void()>& operation) {
    bool failed = false;
    try { operation(); } catch (const std::exception&) { failed = true; }
    require(failed, "expected failure did not occur");
}
class Database {
public:
    Database() {
        static unsigned sequence = 0;
        path = "storage_retention_" + std::to_string(getpid()) + "_" + std::to_string(++sequence) + ".db";
        lib_ = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
        require(lib_ != nullptr, "SQLite unavailable");
        const auto open = reinterpret_cast<int (*)(const char*, void**)>(dlsym(lib_, "sqlite3_open"));
        exec_ = reinterpret_cast<Exec>(dlsym(lib_, "sqlite3_exec"));
        close_ = reinterpret_cast<int (*)(void*)>(dlsym(lib_, "sqlite3_close"));
        require(open && exec_ && close_ && open(path.c_str(), &db_) == 0, "SQLite test open failed");
    }
    ~Database() {
        close_(db_); dlclose(lib_);
        for (const auto* suffix : {"", "-wal", "-shm", "-journal"}) std::remove((path + suffix).c_str());
    }
    std::string sql(const std::string& query) {
        std::string result;
        require(exec_(db_, query.c_str(), [](void* out, int count, char** values, char**) {
            for (int i = 0; i < count; ++i) *static_cast<std::string*>(out) += std::string(values[i] ? values[i] : "NULL") + '|';
            return 0;
        }, &result, nullptr) == 0, "test SQL failed: " + query);
        return result;
    }
    int rows(const char* table) { return std::stoi(sql(std::string("SELECT count(*) FROM ") + table)); }
    std::string path;
private:
    using Exec = int (*)(void*, const char*, int (*)(void*, int, char**, char**), void*, char**);
    void* lib_ = nullptr;
    void* db_ = nullptr;
    Exec exec_ = nullptr;
    int (*close_)(void*) = nullptr;
};
AlarmEvent alarm(const std::string& id, std::int64_t ts) {
    AlarmEvent event{};
    event.eventId = id; event.ts = ts; event.index = 1; event.alarmType = "high";
    return event;
}
void alarmRetention() {
    for (int days : {30, 45, 0, 99999}) {
        Database db;
        SqliteAlarmWriter writer(db.path, "", days);
        const auto cutoff = kNow - std::min(std::max(days, 1), 3650) * kDay;
        const auto base = Clock::now() + 1h;
        writer.writeEvents({alarm("old", cutoff - 1), alarm("boundary", cutoff), alarm("future", kNow + kDay)});
        require(db.rows("alarm_events") == 3, "writes must not perform inline expiry");
        writer.cleanupExpiredEvents(kNow, base);
        require(db.rows("alarm_events") == 2 && db.sql("SELECT event_id FROM alarm_events ORDER BY id") == "boundary|future|",
            "alarm day boundary/clamp failed");
        writer.writeEvents({alarm("boundary", cutoff)});
        require(db.rows("alarm_events") == 2, "alarm idempotency regressed");
        const auto plan = db.sql("EXPLAIN QUERY PLAN SELECT id FROM alarm_events INDEXED BY idx_alarm_events_ts_id WHERE ts < 1 ORDER BY ts,id LIMIT 512");
        require(plan.find("SEARCH alarm_events USING COVERING INDEX idx_alarm_events_ts_id") != std::string::npos &&
            plan.find("TEMP B-TREE") == std::string::npos, "alarm cleanup must use timestamp index");
    }
    Database db;
    SqliteAlarmWriter writer(db.path);
    writer.writeEvents({alarm("minimum", std::numeric_limits<std::int64_t>::min())});
    writer.cleanupExpiredEvents(std::numeric_limits<std::int64_t>::min());
    require(db.rows("alarm_events") == 1, "alarm cutoff underflow");
}
void alarmBatchesAndFailure() {
    Database db;
    SqliteAlarmWriter writer(db.path);
    std::vector<AlarmEvent> events;
    for (int i = 0; i < 1200; ++i) events.push_back(alarm(std::to_string(i), 1));
    writer.writeEvents(events);
    const auto base = Clock::now() + 1h;
    writer.cleanupExpiredEvents(kNow, base);
    require(db.rows("alarm_events") == 688, "alarm batch exceeds 512");
    writer.cleanupExpiredEvents(kNow, base + 999ms);
    require(db.rows("alarm_events") == 688, "alarm batch retry too early");
    writer.cleanupExpiredEvents(kNow, base + 1s);
    require(db.rows("alarm_events") == 176, "alarm backlog did not continue");
    db.sql("CREATE TRIGGER fail_cleanup BEFORE DELETE ON alarm_events BEGIN SELECT RAISE(ABORT,'failure'); END");
    fails([&] { writer.cleanupExpiredEvents(kNow, base + 2s); });
    require(db.rows("alarm_events") == 176, "failed alarm cleanup was not atomic");
    writer.writeEvents({alarm("new", kNow)});
    db.sql("DROP TRIGGER fail_cleanup");
    writer.cleanupExpiredEvents(kNow, base + 6999ms);
    require(db.rows("alarm_events") == 177, "alarm failure backoff too short");
    writer.cleanupExpiredEvents(kNow, base + 7s);
    require(db.rows("alarm_events") == 1, "alarm cleanup failed to recover");
    writer.writeEvents({alarm("late", 1)});
    writer.cleanupExpiredEvents(kNow, base + 66s);
    require(db.rows("alarm_events") == 2, "alarm idle interval too short");
    writer.cleanupExpiredEvents(kNow, base + 67s);
    require(db.rows("alarm_events") == 1, "alarm idle cleanup failed");
}
void outboxRetention() {
    for (int days : {30, 45}) {
        Database db;
        MqttEventOutbox box(db.path, "", 12, 24, 100, 0,
            MqttEventOutbox::StorageProfile::DeleteNormal, MqttEventOutbox::AccessMode::ReadWrite, days);
        const auto cutoff = kNow - days * kDay;
        box.enqueueBatch({
            {"alarm", "main", "old", cutoff - 1, "old", "main"},
            {"alarm", "third", "pending", 1, "old", "third"},
            {"alarm", "main", "boundary", cutoff, "boundary", "main"},
            {"change", "third", "old", cutoff - 1, "change", "third"},
            {"ota_status", "main", "pending", 1, "ota", "main"}});
        for (const auto* id : {"old", "boundary", "change"}) {
            const auto target = std::string(id) == "change" ? "third" : "main";
            box.markSent(std::stoll(db.sql(std::string("SELECT id FROM mqtt_event_outbox WHERE event_id='") + id +
                "' AND target_id='" + target + "'")), kNow);
        }
        box.cleanupIfDue(kNow);
        require(db.rows("mqtt_event_outbox") == 3, "only confirmed rows strictly older than cutoff may expire");
        require(box.pendingCount("main") == 1 && box.pendingCount("third") == 1, "expiry deleted unconfirmed fanout/critical events");
        const auto plan = db.sql("EXPLAIN QUERY PLAN SELECT id FROM mqtt_event_outbox INDEXED BY idx_mqtt_event_outbox_expiry_days WHERE sent=1 AND event_ts < 1 ORDER BY event_ts,id LIMIT 512");
        require(plan.find("SEARCH mqtt_event_outbox USING COVERING INDEX idx_mqtt_event_outbox_expiry_days") != std::string::npos &&
            plan.find("TEMP B-TREE") == std::string::npos, "outbox expiry requires indexed bounded seek");
    }
}
void outboxBatchesFailureAndIpcGuard() {
    Database db;
    MqttEventOutbox box(db.path, "", 12, 1, 100);
    std::vector<MqttEventOutbox::EventMessage> events;
    for (int i = 0; i < 1200; ++i) events.push_back({"alarm", "topic", "old", 1, std::to_string(i), "main"});
    box.markSentBatch(box.enqueueBatch(events), kNow);
    const auto base = Clock::now() + 1h;
    box.cleanupIfDue(kNow, base);
    require(db.rows("mqtt_event_outbox") == 688, "outbox cleanup exceeds 512");
    box.cleanupIfDue(kNow, base + 999ms);
    require(db.rows("mqtt_event_outbox") == 688, "outbox cleanup unthrottled");
    db.sql("CREATE TRIGGER fail_cleanup BEFORE DELETE ON mqtt_event_outbox BEGIN SELECT RAISE(ABORT,'failure'); END");
    fails([&] { box.cleanupIfDue(kNow, base + 1s); });
    require(db.rows("mqtt_event_outbox") == 688, "failed outbox cleanup lost rows");
    box.enqueue("alarm", "topic", "pending", 1);
    db.sql("DROP TRIGGER fail_cleanup");
    box.cleanupIfDue(kNow, base + 5999ms);
    require(db.rows("mqtt_event_outbox") == 689, "outbox cleanup ignored failure backoff");
    box.cleanupIfDue(kNow, base + 6s);
    require(db.rows("mqtt_event_outbox") == 177, "outbox retry failed");
    box.cleanupIfDue(kNow, base + 7s);
    require(db.rows("mqtt_event_outbox") == 1 && box.pendingCount() == 1, "outbox backlog or pending protection failed");
    const auto id = box.enqueue("alarm", "topic", "old", 1);
    box.markSent(id, kNow);
    box.cleanupIfDue(kNow, base + 3606s);
    require(db.rows("mqtt_event_outbox") == 2, "configured idle interval ignored");
    box.cleanupIfDue(kNow, base + 3607s);
    require(db.rows("mqtt_event_outbox") == 1, "configured idle cleanup did not resume");

    Database ipc;
    MqttEventOutbox ipcBox(ipc.path, "", 12, 24, 100, 0, MqttEventOutbox::StorageProfile::WalFull);
    EventStoreDatabase store(ipcBox, {"storage-test", "v1"}, {"producer"});
    store.registerProducer("producer", "session", 0);
    EventStoreAppend append;
    append.producerId = "producer"; append.epoch = 1; append.sequence = 1; append.request = "immutable";
    append.events.push_back({"alarm", "topic", "identity", 1, "identity", "main"});
    append.localEvents.push_back({"alarm", alarm("identity", 1), 0, "v1"});
    const auto receipt = store.append(append);
    const auto sent = std::stoll(ipc.sql("SELECT id FROM mqtt_event_outbox"));
    ipcBox.markSent(sent, kNow);
    ipcBox.cleanupIfDue(kNow);
    require(ipc.rows("mqtt_event_outbox") == 1, "legacy expiry must not destroy IPC immutable identities");
    require(store.readJournal(0, 64).size() == 1 && store.append(append).receipt == receipt.receipt &&
        ipcBox.pendingCount() == 0, "cleanup changed IPC journal/receipt or requeued an acknowledged event");
}
void criticalCapacityProtection() {
    for (const auto* type : {"alarm", "ota_status", "command_reply", "unknown"}) {
        Database db;
        MqttEventOutbox box(db.path, "", 12, 24, 100, 100);
        const auto id = box.enqueue(type, "t", std::string(80, 'x'), 1);
        fails([&] { box.enqueue("alarm", "t", std::string(80, 'y'), 2); });
        box.cleanupIfDue(kNow);
        require(db.rows("mqtt_event_outbox") == 1 && box.pendingCount() == 1 &&
            std::stoll(db.sql("SELECT id FROM mqtt_event_outbox")) == id,
            "capacity enforcement or expiry deleted an unconfirmed critical event");
    }
}
void idleProductionMaintenance() {
    Database alarms;
    Database events;
    auto writer = std::make_unique<SqliteAlarmWriter>(alarms.path);
    writer->writeEvents({alarm("old", 1)});
    auto box = std::make_unique<MqttEventOutbox>(events.path, "", 12, 24, 100);
    box->markSent(box->enqueue("alarm", "topic", "old", 1), kNow);
    PointStoreRouter router;
    EventEngineService service({}, {}, {}, router, {}, std::make_shared<NoNetworkPublisher>(),
        std::move(box), std::move(writer));
    service.runOnce(kNow);
    require(alarms.rows("alarm_events") == 0 && events.rows("mqtt_event_outbox") == 0,
        "production idle scan did not run history/outbox cleanup");
}
} // namespace
int main() {
    try {
        alarmRetention(); alarmBatchesAndFailure(); outboxRetention();
        outboxBatchesFailureAndIpcGuard(); criticalCapacityProtection(); idleProductionMaintenance();
        std::cout << "storage retention: 6 groups PASS\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n'; return 1;
    }
}
