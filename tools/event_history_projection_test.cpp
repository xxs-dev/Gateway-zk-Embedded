#include "edge_gateway/event_history_projection.hpp"
#include <unistd.h>
#include <sys/wait.h>
#include <cstdlib>
#include <dlfcn.h>
#include "event_store_test_files.hpp"
#include <iostream>
#include <stdexcept>

using namespace edge_gateway;
using Outbox = MqttEventOutbox;

// Inspection follows the same selected engine, without requiring SQLite development headers.
struct sqlite3;
struct sqlite3_stmt;
int (*sqlite3_open)(const char*, sqlite3**);
int (*sqlite3_open_v2)(const char*, sqlite3**, int, const char*);
int (*sqlite3_close)(sqlite3*);
int (*sqlite3_exec)(sqlite3*, const char*, int(*)(void*, int, char**, char**), void*, char**);
void (*sqlite3_free)(void*);
int (*sqlite3_prepare_v2)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int (*sqlite3_step)(sqlite3_stmt*);
long long (*sqlite3_column_int64)(sqlite3_stmt*, int);
int (*sqlite3_finalize)(sqlite3_stmt*);
constexpr int SQLITE_OK = 0, SQLITE_ROW = 100, SQLITE_OPEN_READONLY = 1;

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F fn, const std::string& expected) {
    try { fn(); } catch (const std::exception& e) {
        require(std::string(e.what()).find(expected) != std::string::npos, e.what());
        return;
    }
    throw std::runtime_error("expected rejection: " + expected);
}
struct Temp {
    std::string path;
    Temp() { char p[] = "/tmp/event_projection_XXXXXX"; auto* s = mkdtemp(p); require(s, "mkdtemp"); path = s; }
    ~Temp() { removeEventStoreTestTree(path); }
};
void sql(const std::string& path, const std::string& statement) {
    sqlite3* db = nullptr;
    require(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "test open");
    char* error = nullptr;
    const int rc = sqlite3_exec(db, statement.c_str(), nullptr, nullptr, &error);
    const std::string message = error ? error : "test SQL";
    sqlite3_free(error); sqlite3_close(db);
    require(rc == SQLITE_OK, message.c_str());
}
long long scalar(const std::string& path, const char* query) {
    sqlite3* db = nullptr; sqlite3_stmt* stmt = nullptr;
    require(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "scalar open");
    require(sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) == SQLITE_OK, "scalar prepare");
    require(sqlite3_step(stmt) == SQLITE_ROW, "scalar row");
    auto value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt); sqlite3_close(db); return value;
}
EventStoreAppend request(int seq, std::string id, std::string kind = "alarm") {
    EventStoreAppend r;
    r.producerId = "p"; r.epoch = 1; r.sequence = seq; r.request = "request-" + std::to_string(seq);
    EventStoreLocalEvent local;
    local.kind = kind; local.configGeneration = "v1"; local.stateVersion = 1;
    auto& e = local.event;
    e.eventId = std::move(id); e.index = 7; e.ts = 1780000000123; e.alarmType = "high";
    e.active = true; e.threshold = 12.25; e.value = 19.875; e.quality = 3; e.stale = true;
    e.persistValue = "raw-original"; e.machineCode = "gateway"; e.meterCode = "device"; e.pointCode = "point";
    r.localEvents.push_back(local); return r;
}
}

int main(int argc, char** argv) {
    try {
        require(argc <= 2, "usage: event_history_projection_test [SQLITE_LIBRARY]");
        const std::string library = argc == 2 ? argv[1] : "";
        auto* handle = dlopen(library.empty() ? "libsqlite3.so.0" : library.c_str(), RTLD_NOW | RTLD_LOCAL);
        require(handle, "load test inspection SQLite");
#define LOAD(name) name = reinterpret_cast<decltype(name)>(dlsym(handle, #name)); require(name, #name)
        LOAD(sqlite3_open); LOAD(sqlite3_open_v2); LOAD(sqlite3_close); LOAD(sqlite3_exec);
        LOAD(sqlite3_free); LOAD(sqlite3_prepare_v2); LOAD(sqlite3_step);
        LOAD(sqlite3_column_int64); LOAD(sqlite3_finalize);
#undef LOAD
        Temp temp;
        for (const auto profile : {Outbox::StorageProfile::DeleteFull, Outbox::StorageProfile::WalFull}) {
            Temp locked;
            const auto sourcePath = locked.path + "/source.db", historyPath = locked.path + "/history.db";
            Outbox box(sourcePath, library, 1, 1, 8, 0, profile);
            EventStoreDatabase store(box, {"lock-test", "v1"}, {"p"});
            store.registerProducer("p", "s", 0);
            store.append(request(1, "verify-before-cas"));
            EventHistoryProjection projection(store, historyPath);
            projection.replay(store, 0, 64);
            sqlite3* blocker = nullptr;
            require(sqlite3_open(sourcePath.c_str(), &blocker) == SQLITE_OK, "source blocker open");
            require(sqlite3_exec(blocker, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK,
                "source blocker begin");
            sql(historyPath, "UPDATE alarm_events SET threshold=999;");
            // If confirmation acquires the source write lock first, this returns BUSY
            // instead of detecting the independent history content conflict.
            rejects([&] { store.confirmProjection(projection); }, "PROJECTION_CONFLICT");
            require(store.projectionCursor().projectedThrough == 0, "conflict advanced source cursor");
            sql(historyPath, "UPDATE alarm_events SET threshold=12.25;");
            rejects([&] { store.confirmProjection(projection); }, "locked");
            require(store.projectionCursor().projectedThrough == 0, "blocked CAS advanced source cursor");
            require(sqlite3_exec(blocker, "ROLLBACK;", nullptr, nullptr, nullptr) == SQLITE_OK,
                "source blocker release");
            sqlite3_close(blocker);
            store.confirmProjection(projection);
            require(store.projectionCursor().projectedThrough == 1, "confirmation did not recover after lock");
        }
        std::cout << "PASS confirmation-history-verify-before-source-cas-delete-wal\n";
        const auto path = temp.path + "/source.db", history = temp.path + "/history.db";
        Outbox outbox(path, library, 1, 1, 8, 0, Outbox::StorageProfile::WalFull);
        EventStoreDatabase source(outbox, {"store", "v1"}, {"p"});
        source.registerProducer("p", "session", 0);
        auto a = request(1, "a");
        auto receipt = source.append(a);
        {
            const auto moving = temp.path + "/moving.db";
            EventHistoryProjection p(source, moving);
            require(::rename(moving.c_str(), (moving + ".old").c_str()) == 0, "rename history");
            sql(moving, "CREATE TABLE replacement(id INTEGER);");
            rejects([&] { p.watermark(); }, "physical");
            rejects([&] { p.replay(source, 0, 1); }, "physical");
            rejects([&] { source.reconcileProjection(p); }, "physical");
        }
        {
            const auto target = temp.path + "/symlink-target.db";
            sql(target, "CREATE TABLE untouched(id INTEGER);");
            const auto alias = temp.path + "/symlink.db";
            require(symlink(target.c_str(), alias.c_str()) == 0, "symlink");
            rejects([&] { EventHistoryProjection p(source, alias); }, "ownership");
            const auto live = temp.path + "/live.db";
            EventHistoryProjection p(source, live);
            require(::rename(live.c_str(), (live + ".old").c_str()) == 0, "rename live history");
            require(symlink(target.c_str(), live.c_str()) == 0, "replacement symlink");
            rejects([&] { p.watermark(); }, "physical");
        }
        {
            const auto parent = temp.path + "/parent";
            const auto first = temp.path + "/first", second = temp.path + "/second";
            require(mkdir(first.c_str(), 0700) == 0 && mkdir(second.c_str(), 0700) == 0, "mkdir parent targets");
            require(symlink(first.c_str(), parent.c_str()) == 0, "parent symlink");
            EventHistoryProjection p(source, parent + "/history.db");
            require(unlink(parent.c_str()) == 0 && symlink(second.c_str(), parent.c_str()) == 0, "retarget parent");
            rejects([&] { p.watermark(); }, "physical");
        }
        require(receipt.receipt.find("journalIds") != std::string::npos, "journal receipt");
        require(source.append(a).receipt == receipt.receipt, "receipt retry");
        auto changed = a; changed.localEvents[0].event.threshold = 13;
        rejects([&] { source.append(changed); }, "REQUEST_CONFLICT");
        auto duplicate = request(2, "a"); source.append(duplicate);
        require(source.readJournal(0, 64).size() == 1, "same content deduplication");
        auto conflict = request(3, "a"); conflict.localEvents[0].event.pointCode = "changed";
        rejects([&] { source.append(conflict); }, "JOURNAL_CONFLICT");
        require(source.receipt("p").sequence == 2, "conflict rolled back receipt");
        auto b = request(3, "b", "change"); source.append(b);
        auto c = request(4, "c");
        c.states.push_back({}); c.states[0].state.stateKey = "state";
        c.states[0].state.eventType = "alarm"; c.states[0].version = 9;
        rejects([&] { source.append(c); }, "STATE_CONFLICT");
        require(source.readJournal(0, 64).size() == 2, "journal rollback with CAS failure");
        c.states.clear(); source.append(c);
        require(source.readJournal(2, 1)[0].id == 3, "rolled back allocation has no hole");
        auto bad = request(5, "bad"); bad.localEvents[0].event.eventId.clear();
        rejects([&] { source.append(bad); }, "empty");
        rejects([&] { source.readJournal(0, 65); }, "1..64");
        sql(path, "CREATE TRIGGER reject_journal BEFORE INSERT ON event_local_journal WHEN NEW.event_id='fail' "
            "BEGIN SELECT RAISE(ABORT,'injected journal failure'); END;");
        auto fail = request(5, "fail");
        fail.events.push_back({"alarm", "topic", "payload", 1780000000123, "fail", "main"});
        rejects([&] { source.append(fail); }, "injected journal failure");
        require(outbox.pendingCount() == 0 && source.receipt("p").sequence == 4, "append atomic failure");
        sql(path, "CREATE TRIGGER reject_upload BEFORE INSERT ON mqtt_event_outbox WHEN NEW.event_id='upload-fail' "
            "BEGIN SELECT RAISE(ABORT,'injected upload failure'); END;");
        auto uploadFail = request(5, "upload-fail");
        uploadFail.events.push_back({"alarm", "topic", "payload", 1780000000123, "upload-fail", "main"});
        rejects([&] { source.append(uploadFail); }, "injected upload failure");
        require(source.readJournal(0, 64).size() == 3 && source.receipt("p").sequence == 4,
            "upload failure rolls back earlier journal insert");

        Outbox ro(path, library, 1, 1, 8, 0, Outbox::StorageProfile::WalFull, Outbox::AccessMode::ReadOnly);
        EventStoreDatabase reader(ro, {"store", "v1"}, {"p"}, true);
        require(reader.readJournal(1, 1)[0].local.kind == "change", "read-only pagination");
        rejects([&] { reader.append(a); }, "read-only");
        {
            EventHistoryProjection p(source, history);
            rejects([&] { EventHistoryProjection second(source, history); }, "already owned");
            const auto alias = temp.path + "/alias.db";
            require(link(history.c_str(), alias.c_str()) == 0, "hardlink");
            rejects([&] { EventHistoryProjection second(source, alias); }, "physical");
            rejects([&] { p.watermark(); }, "physical");
            require(unlink(alias.c_str()) == 0, "remove history alias");
            const auto sourceAlias = temp.path + "/source-alias.db";
            require(link(path.c_str(), sourceAlias.c_str()) == 0, "source hardlink");
            rejects([&] { p.replay(reader, 0, 1); }, "physical");
            rejects([&] { EventHistoryProjection second(source, sourceAlias); }, "physical");
            require(unlink(sourceAlias.c_str()) == 0, "remove source alias");
            rejects([&] { EventHistoryProjection wrong(source, path); }, "distinct");
            const auto foreignPath = temp.path + "/foreign-source.db";
            Outbox foreign(foreignPath, library, 1, 1, 8, 0, Outbox::StorageProfile::DeleteFull);
            EventStoreDatabase foreignStore(foreign, {"store", "v1"}, {"p"});
            rejects([&] { p.replay(foreignStore, 0, 1); }, "physical");
            require(source.reconcileProjection(p) == 0, "initial cursor");
            rejects([&] { p.replay(reader, 1, 1); }, "skip");
            require(p.replay(reader, 0, 2) == 2 && p.watermark() == 2, "continuous watermark includes change");
            require(scalar(history, "SELECT COUNT(*) FROM alarm_events") == 1, "change excluded from alarms");
            // Simulate response/cursor loss after history COMMIT.
        }
        {
            EventHistoryProjection p(source, history);
            require(source.reconcileProjection(p) == 0, "history committed before source cursor");
            p.replay(reader, 0, 2); source.confirmProjection(p);
            require(scalar(history, "SELECT COUNT(*) FROM alarm_events") == 1, "restart idempotency");
            sql(history, "CREATE TRIGGER fail_projection BEFORE INSERT ON alarm_events WHEN NEW.event_id='c' "
                "BEGIN SELECT RAISE(ABORT,'injected projection failure'); END;");
            rejects([&] { p.replay(reader, 2, 2); }, "injected projection failure");
            require(p.watermark() == 2 && source.readJournal(0, 64).size() == 3, "failed projection retained journal");
            sql(history, "DROP TRIGGER fail_projection;");
            p.replay(reader, 2, 2); source.confirmProjection(p);
            require(source.reconcileProjection(p) == 3, "fully projected");
        }
        sql(history, "DELETE FROM alarm_events WHERE event_id='a';");
        {
            EventHistoryProjection p(source, history);
            require(source.reconcileProjection(p) == 0, "missing copy rewinds despite high metadata");
            p.replay(reader, 0, 64); source.confirmProjection(p);
            require(scalar(history, "SELECT COUNT(*) FROM alarm_events") == 2, "missing copy repaired");
        }
        sql(history, "DELETE FROM alarm_events WHERE event_id='c'; UPDATE alarm_projection_meta SET last_contiguous_journal_id=2;");
        {
            EventHistoryProjection p(source, history);
            require(source.reconcileProjection(p) == 2, "older history backup rewinds source");
            p.replay(reader, 2, 64); source.confirmProjection(p);
        }
        auto more = request(5, "d");
        more.localEvents.push_back(request(5, "e").localEvents[0]);
        source.append(more);
        sql(history, "CREATE TRIGGER fail_watermark BEFORE UPDATE ON alarm_projection_meta "
            "WHEN NEW.last_contiguous_journal_id=5 BEGIN SELECT RAISE(ABORT,'injected watermark failure'); END;");
        {
            EventHistoryProjection p(source, history);
            rejects([&] { p.replay(reader, 3, 64); }, "injected watermark failure");
            require(p.watermark() == 3 && scalar(history, "SELECT COUNT(*) FROM alarm_events") == 2,
                "rows and watermark roll back together");
            sql(history, "DROP TRIGGER fail_watermark;");
            p.replay(reader, 3, 64); source.confirmProjection(p);
        }
        sql(history, "UPDATE alarm_events SET threshold=999 WHERE event_id='a';");
        {
            EventHistoryProjection p(source, history);
            rejects([&] { source.reconcileProjection(p); }, "PROJECTION_CONFLICT");
            rejects([&] { p.replay(reader, 0, 64); }, "PROJECTION_CONFLICT");
        }
        sql(history, "UPDATE alarm_events SET threshold=12.25 WHERE event_id='a'; "
            "UPDATE alarm_projection_meta SET journal_generation='wrong';");
        rejects([&] { EventHistoryProjection p(source, history); }, "identity mismatch");
        outbox.cleanupIfDue(1990000000123);
        require(source.readJournal(0, 64).size() == 5, "outbox maintenance never deletes journal");
        {
            const auto movingSource = temp.path + "/moving-source.db";
            Outbox other(movingSource, library, 1, 1, 8, 0, Outbox::StorageProfile::DeleteFull);
            EventStoreDatabase otherStore(other, {"other", "v1"}, {"p"});
            EventHistoryProjection p(otherStore, temp.path + "/other-history.db");
            require(::rename(movingSource.c_str(), (movingSource + ".old").c_str()) == 0, "rename source");
            sql(movingSource, "CREATE TABLE replacement(id INTEGER);");
            rejects([&] { p.watermark(); }, "physical");
            rejects([&] { p.replay(otherStore, 0, 1); }, "physical");
            rejects([&] { otherStore.confirmProjection(p); }, "physical");
        }
        std::cout << "PASS: journal atomicity, identities, read-only pages, FULL projection, locks, rollback, restart audit, retention safety\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
