#include "edge_gateway/event_store_runtime.hpp"
#include "edge_gateway/json_value.hpp"

#include <chrono>
#include <dlfcn.h>
#include "event_store_test_files.hpp"
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace edge_gateway;
using Outbox = MqttEventOutbox;
using Clock = std::chrono::steady_clock;
struct sqlite3;

namespace {
void require(bool ok, const std::string& text) { if (!ok) throw std::runtime_error(text); }
template<class F> void rejects(F fn, const std::string& fragment) {
    try { fn(); } catch (const std::exception& e) {
        require(std::string(e.what()).find(fragment) != std::string::npos, e.what()); return;
    }
    throw std::runtime_error("expected rejection: " + fragment);
}
template<class F> void eventually(F fn, const std::string& message) {
    const auto deadline = Clock::now() + std::chrono::seconds(8);
    while (!fn()) {
        require(Clock::now() < deadline, message);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
struct Temp {
    std::string path;
    Temp() { char p[] = "/tmp/event_store_history_runtime_XXXXXX"; auto* value = mkdtemp(p); require(value, "mkdtemp"); path = value; }
    ~Temp() { removeEventStoreTestTree(path); }
    EventStoreRuntimeOptions options(Outbox::StorageProfile profile) const {
        EventStoreRuntimeOptions o;
        o.identity = {"history-store", "v1"}; o.producers = {"p"};
        o.databasePath = path + "/events.db"; o.historyPath = path + "/history.db";
        o.socketPath = path + "/events.sock"; o.profile = profile;
        o.historyBatchSize = 2; o.historyPollIntervalMs = 20;
        return o;
    }
};
struct Sql {
    void* library = nullptr;
    sqlite3* db = nullptr;
    int (*exec)(sqlite3*, const char*, int(*)(void*, int, char**, char**), void*, char**) = nullptr;
    int (*close)(sqlite3*) = nullptr;
    explicit Sql(const std::string& path) {
        library = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
        require(library, "SQLite inspection library");
        auto open = reinterpret_cast<int(*)(const char*, sqlite3**, int, const char*)>(dlsym(library, "sqlite3_open_v2"));
        exec = reinterpret_cast<decltype(exec)>(dlsym(library, "sqlite3_exec"));
        close = reinterpret_cast<decltype(close)>(dlsym(library, "sqlite3_close_v2"));
        require(open && exec && close && open(path.c_str(), &db, 2, nullptr) == 0, "SQLite inspection open");
        run("PRAGMA busy_timeout=2000;");
    }
    ~Sql() { if (db) close(db); if (library) dlclose(library); }
    void run(const std::string& sql) { require(exec(db, sql.c_str(), nullptr, nullptr, nullptr) == 0, "inspection SQL: " + sql); }
    long long scalar(const std::string& sql) {
        long long result = -1;
        auto read = [](void* p, int n, char** v, char**) -> int {
            if (n != 1 || !v[0]) return 1;
            *static_cast<long long*>(p) = std::stoll(v[0]); return 0;
        };
        require(exec(db, sql.c_str(), read, &result, nullptr) == 0, "scalar SQL: " + sql); return result;
    }
};
std::string wire(const EventStoreRuntimeOptions& o, const std::string& op, const std::string& args) {
    return "{\"version\":\"1\",\"storeId\":\"" + o.identity.storeId + "\",\"configGeneration\":\"v1\",\"op\":\"" + op + "\",\"args\":" + args + "}";
}
json::JsonValue call(const EventStoreRuntimeOptions& o, const std::string& op, const std::string& args = "{}") {
    const auto raw = callEventStore(o.socketPath, wire(o, op, args), 2000);
    auto value = json::JsonParser(raw).parse();
    require(value.find("ok") && value.find("ok")->asBool(), raw); return value;
}
void registerProducer(const EventStoreRuntimeOptions& o) {
    call(o, "RegisterProducer", "{\"producerId\":\"p\",\"sessionId\":\"one\",\"expectedEpoch\":\"0\"}");
}
void append(const EventStoreRuntimeOptions& o, int sequence, const std::string& kind = "alarm") {
    const auto seq = std::to_string(sequence);
    call(o, "AppendEventsAndStates", "{\"producerId\":\"p\",\"epoch\":\"1\",\"sequence\":\"" + seq +
        "\",\"events\":[],\"states\":[],\"localEvents\":[{\"kind\":\"" + kind +
        "\",\"eventId\":\"event-" + seq + "\",\"index\":\"1\",\"machineCode\":\"g\",\"meterCode\":\"d\",\"pointCode\":\"p\","
        "\"alarmType\":\"high\",\"active\":true,\"threshold\":1.5,\"value\":2.5,\"quality\":\"1\",\"ts\":\"1780000000000\","
        "\"stale\":false,\"persistValue\":\"2.5\",\"stateVersion\":\"" + seq + "\",\"configGeneration\":\"v1\"}]}");
}
void waitThrough(EventStoreRuntime& runtime, long long through) {
    eventually([&] { return runtime.historyStatus().projectedThrough == through; }, "history did not catch up: " + runtime.historyStatus().lastError);
}

void runtimeRecovery(Outbox::StorageProfile profile) {
    Temp temp; auto o = temp.options(profile);
    EventStoreRuntime runtime(o); runtime.start();
    auto hello = call(o, "Hello");
    require(hello.find("localJournalVersion")->asString() == "1" && hello.find("historyEnabled")->asBool(), "Hello capabilities");
    registerProducer(o);
    append(o, 1); append(o, 2, "change"); append(o, 3);
    waitThrough(runtime, 3);
    {
        Sql history(o.historyPath);
        require(history.scalar("SELECT COUNT(*) FROM alarm_events;") == 2, "change projected as alarm");
        history.run("BEGIN EXCLUSIVE;");
        // Keep history blocked until both the projection failure and another producer COMMIT are observed.
        append(o, 4);
        eventually([&] { return runtime.historyStatus().failures != 0; }, "history lock not observed");
        auto producer = std::async(std::launch::async, [&] { append(o, 5); });
        const bool finished = producer.wait_for(std::chrono::milliseconds(750)) == std::future_status::ready;
        history.run("ROLLBACK;");
        producer.get();
        require(finished, "producer waited for history transaction");
    }
    waitThrough(runtime, 5);
    {
        Sql source(o.databasePath);
        source.run("CREATE TRIGGER fail_cursor BEFORE UPDATE ON event_history_projection_cursor "
            "BEGIN SELECT RAISE(ABORT,'injected cursor failure'); END;");
        append(o, 6);
        Sql history(o.historyPath);
        eventually([&] { return history.scalar("SELECT COUNT(*) FROM alarm_events WHERE event_id='event-6';") == 1; }, "history never committed");
        require(source.scalar("SELECT projected_through FROM event_history_projection_cursor;") == 5,
            "source cursor advanced despite injected failure");
        append(o, 7);
        source.run("DROP TRIGGER fail_cursor;");
    }
    waitThrough(runtime, 7);
    auto status = call(o, "GetHistoryProjectionStatus");
    require(status.find("projectedThrough")->asString() == "7", "IPC projection status");
    runtime.stop();
    {
        Sql history(o.historyPath);
        history.run("DELETE FROM alarm_events WHERE event_id='event-1';");
    }
    runtime.start();
    eventually([&] { Sql history(o.historyPath); return history.scalar("SELECT COUNT(*) FROM alarm_events WHERE event_id='event-1';") == 1; }, "restart audit did not repair missing history");
    waitThrough(runtime, 7);
    {
        Sql history(o.historyPath);
        require(history.scalar("SELECT COUNT(*) FROM alarm_events;") == 6, "restart duplicated rows");
    }
    runtime.stop();
    {
        Sql history(o.historyPath);
        history.run("UPDATE alarm_projection_meta SET store_id='wrong';");
    }
    rejects([&] { runtime.start(); }, "identity mismatch");
    require(!runtime.ready(), "identity mismatch accepted READY");
}

void disabledAndAliases() {
    Temp temp; auto o = temp.options(Outbox::StorageProfile::WalFull);
    auto disabled = o; disabled.historyPath.clear();
    {
        EventStoreRuntime runtime(disabled); runtime.start(); registerProducer(disabled); append(disabled, 1);
        require(!runtime.historyStatus().enabled && access(o.historyPath.c_str(), F_OK) != 0, "disabled projection opened history");
    }
    for (const auto& alias : {o.databasePath, o.socketPath, o.databasePath + "-wal", o.socketPath + ".lock"}) {
        auto wrong = o; wrong.historyPath = alias;
        EventStoreRuntime runtime(wrong); rejects([&] { runtime.start(); }, "alias");
    }
    const auto linkPath = temp.path + "/source-link.db";
    require(symlink(o.databasePath.c_str(), linkPath.c_str()) == 0, "symlink");
    auto wrong = o; wrong.historyPath = linkPath;
    { EventStoreRuntime runtime(wrong); rejects([&] { runtime.start(); }, "symlink"); }
    require(unlink(linkPath.c_str()) == 0 && link(o.databasePath.c_str(), linkPath.c_str()) == 0, "hardlink");
    { EventStoreRuntime runtime(wrong); rejects([&] { runtime.start(); }, "alias"); }
    require(unlink(linkPath.c_str()) == 0, "unlink hardlink");
    {
        EventStoreRuntime runtime(o); runtime.start(); waitThrough(runtime, 1);
        require(::rename(o.historyPath.c_str(), (o.historyPath + ".saved").c_str()) == 0, "rename runtime history");
        std::ofstream(o.historyPath) << "replacement";
        eventually([&] { return runtime.historyStatus().state == "error"; }, "runtime missed replaced history");
        append(o, 2);
        require(runtime.ready() && runtime.historyStatus().projectedThrough == 1, "history replacement blocked producer or advanced cursor");
    }
}

void configContract() {
    Temp temp; auto o = temp.options(Outbox::StorageProfile::WalFull);
    const auto config = temp.path + "/config.json";
    const auto write = [&](bool lab, const std::string& fields) {
        std::ofstream(config) << "{\"laboratoryOnly\":" << (lab ? "true" : "false") <<
            ",\"storeId\":\"s\",\"configGeneration\":\"v1\",\"databasePath\":\"/tmp/s.db\",\"socketPath\":\"/tmp/s.sock\","
            "\"sqliteLibraryPath\":\"\",\"storageProfile\":\"wal-full\",\"producers\":[\"p\"]" << fields << "}";
    };
    write(true, ",\"historyPath\":\"/tmp/h.db\",\"historyBatchSize\":64,\"historyPollIntervalMs\":40");
    const auto loaded = loadEventStoreLabConfig(config);
    require(loaded.historyPath == "/tmp/h.db" && loaded.historyBatchSize == 64 && loaded.historyPollIntervalMs == 40, "history config fields");
    write(true, ""); require(loadEventStoreLabConfig(config).historyPath.empty(), "old config compatibility");
    write(false, ""); rejects([&] { loadEventStoreLabConfig(config); }, "laboratory-only");
    write(true, ",\"historyBatchSize\":65"); rejects([&] { loadEventStoreLabConfig(config); }, "bounded config integer");
}

void shutdownWhileBlocked() {
    Temp temp; auto o = temp.options(Outbox::StorageProfile::DeleteFull);
    EventStoreRuntime runtime(o); runtime.start(); registerProducer(o);
    Sql history(o.historyPath); history.run("BEGIN EXCLUSIVE;");
    append(o, 1);
    eventually([&] { return runtime.historyStatus().failures > 0; }, "blocked projection did not retry");
    auto stop = std::async(std::launch::async, [&] { runtime.stop(); });
    const bool finished = stop.wait_for(std::chrono::milliseconds(750)) == std::future_status::ready;
    history.run("ROLLBACK;"); stop.get();
    require(finished, "shutdown waited for history lock to be released");
    runtime.start(); waitThrough(runtime, 1);
}

void retryAuditBoundary() {
    Temp temp; auto o = temp.options(Outbox::StorageProfile::WalFull);
    EventStoreRuntime runtime(o); runtime.start(); registerProducer(o);
    for (int i = 1; i <= 20; ++i) append(o, i);
    waitThrough(runtime, 20);
    const auto audited = runtime.historyStatus().auditedRows;
    {
        Sql history(o.historyPath); history.run("BEGIN EXCLUSIVE;");
        const auto failures = runtime.historyStatus().failures;
        append(o, 21);
        eventually([&] { return runtime.historyStatus().failures >= failures + 2; }, "repeated BUSY not observed");
        history.run("ROLLBACK;");
    }
    waitThrough(runtime, 21);
    require(runtime.historyStatus().auditedRows == audited, "BUSY retry rescanned a verified historical prefix");
    {
        Sql history(o.historyPath); history.run("BEGIN EXCLUSIVE;");
        const auto failures = runtime.historyStatus().failures;
        append(o, 22);
        eventually([&] { return runtime.historyStatus().failures > failures; }, "rollback fixture did not interrupt projection");
        // In-place restore keeps the inode but rolls metadata back. A retained retry
        // prefix must not hide a missing row below the restored watermark.
        history.run("DELETE FROM alarm_events WHERE event_id IN ('event-5','event-21');"
            "UPDATE alarm_projection_meta SET last_contiguous_journal_id=19; COMMIT;");
    }
    waitThrough(runtime, 22);
    Sql history(o.historyPath);
    require(history.scalar("SELECT COUNT(*) FROM alarm_events WHERE event_id='event-5';") == 1,
        "backup rollback incorrectly reused retry audit prefix");
    require(runtime.historyStatus().auditedRows >= audited + 4, "backup rollback did not restart audit");
}
}

int main() {
    try {
        runtimeRecovery(Outbox::StorageProfile::DeleteFull); std::cout << "PASS history-runtime-delete\n";
        runtimeRecovery(Outbox::StorageProfile::WalFull); std::cout << "PASS history-runtime-wal\n";
        disabledAndAliases(); std::cout << "PASS history-disabled-alias-replacement\n";
        configContract(); std::cout << "PASS history-config-lab-gate\n";
        shutdownWhileBlocked(); std::cout << "PASS history-bounded-shutdown-restart\n";
        retryAuditBoundary(); std::cout << "PASS history-busy-audit-prefix-backup-rollback\n";
        return 0;
    } catch (const std::exception& ex) { std::cerr << "FAIL: " << ex.what() << '\n'; return 1; }
}
