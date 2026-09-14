#include "edge_gateway/event_store_runtime.hpp"
#include "edge_gateway/json_value.hpp"
#include "edge_gateway/sqlite_error.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

struct sqlite3;
struct sqlite3_stmt;
struct sqlite3_api_routines;

namespace {
using namespace edge_gateway;
using Outbox = MqttEventOutbox;
using Profile = Outbox::StorageProfile;
using Selection = EventStatsSelection;
using Json = json::JsonValue;
std::string libraryPath;
void* library = nullptr;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Action> void rejects(Action action, const std::string& fragment = {}) {
    try { action(); }
    catch (const std::exception& ex) {
        require(fragment.empty() || std::string(ex.what()).find(fragment) != std::string::npos,
            "unexpected error: " + std::string(ex.what()));
        return;
    }
    throw std::runtime_error("expected rejection: " + fragment);
}

template <typename Function> Function symbol(const char* name) {
    auto* address = dlsym(library, name);
    require(address != nullptr, std::string("missing SQLite symbol: ") + name);
    return reinterpret_cast<Function>(address);
}

struct Sql {
    sqlite3* db = nullptr;
    explicit Sql(const std::string& path) {
        require(symbol<int (*)(const char*, sqlite3**, int, const char*)>("sqlite3_open_v2")(
            path.c_str(), &db, 6, nullptr) == 0, "SQLite fixture open failed");
    }
    ~Sql() { symbol<int (*)(sqlite3*)>("sqlite3_close_v2")(db); }
    std::string query(const std::string& sql) {
        std::string result;
        const auto callback = [](void* context, int count, char** values, char**) {
            auto& output = *static_cast<std::string*>(context);
            for (int i = 0; i < count; ++i) {
                const std::string value = values[i] ? values[i] : "NULL";
                output += std::to_string(value.size()) + ':' + value;
            }
            output += '\n';
            return 0;
        };
        const int rc = symbol<int (*)(sqlite3*, const char*, int (*)(void*, int, char**, char**),
            void*, char**)>("sqlite3_exec")(db, sql.c_str(), callback, &result, nullptr);
        require(rc == 0, "fixture SQL failed: " + sql);
        return result;
    }
};

struct Directory {
    std::string path;
    Directory() {
        char name[] = "/tmp/event_stats_XXXXXX";
        const auto* created = mkdtemp(name);
        require(created != nullptr, "mkdtemp failed");
        path = created;
    }
    ~Directory() {
        if (auto* dir = opendir(path.c_str())) {
            while (auto* item = readdir(dir)) {
                if (std::strcmp(item->d_name, ".") && std::strcmp(item->d_name, "..")) {
                    unlink((path + '/' + item->d_name).c_str());
                }
            }
            closedir(dir);
        }
        rmdir(path.c_str());
    }
    EventStoreRuntimeOptions options(Profile profile = Profile::DeleteFull) const {
        EventStoreRuntimeOptions result;
        result.identity = {"stats-store", "stats-v1"};
        result.producers = {"p0"};
        result.senders = {{"s0", "main", {"alarm", "change"}}};
        result.databasePath = path + "/events.db";
        result.socketPath = path + "/events.sock";
        result.sqliteLibraryPath = libraryPath;
        result.profile = profile;
        return result;
    }
};

std::string quote(const std::string& value) {
    std::string result = "\"";
    for (unsigned char ch : value) {
        if (ch == '"' || ch == '\\') result += '\\';
        if (ch < 32) {
            char escaped[7];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
            result += escaped;
        } else result += static_cast<char>(ch);
    }
    return result + '"';
}

std::string typesJson(const std::vector<std::string>& values) {
    std::string result = "[";
    for (const auto& value : values) {
        if (result.size() > 1) result += ',';
        result += quote(value);
    }
    return result + ']';
}

std::string scopeJson(const EventStatsScope& scope) {
    return "{\"targetId\":" + quote(scope.targetId) + ",\"selection\":" +
        quote(scope.selection == Selection::All ? "all" : "only") +
        ",\"include\":" + typesJson(scope.include) + ",\"exclude\":" + typesJson(scope.exclude) + '}';
}

std::string wire(const EventStoreRuntimeOptions& options, const std::string& op, const std::string& args) {
    return "{\"version\":\"1\",\"storeId\":" + quote(options.identity.storeId) +
        ",\"configGeneration\":" + quote(options.identity.configGeneration) +
        ",\"op\":" + quote(op) + ",\"args\":" + args + '}';
}

Json call(const EventStoreRuntimeOptions& options, const std::string& op, const std::string& args) {
    return json::JsonParser(callEventStore(options.socketPath, wire(options, op, args)), 16, 4096).parse();
}

const Json& field(const Json& value, const char* name) {
    const auto* result = value.find(name);
    require(result != nullptr, std::string("missing field: ") + name);
    return *result;
}

std::vector<std::string> strings(const Json& values) {
    std::vector<std::string> result;
    for (const auto& value : values.asArray().values) result.push_back(value->asString());
    return result;
}

EventStatsScope returnedScope(const Json& response) {
    const auto& scope = field(response, "scope");
    require(scope.asObject().values.size() == 4, "scope fields drifted");
    const auto selection = field(scope, "selection").asString();
    require(selection == "all" || selection == "only", "invalid selection response");
    return {field(scope, "targetId").asString(), selection == "all" ? Selection::All : Selection::Only,
        strings(field(scope, "include")), strings(field(scope, "exclude"))};
}

void check(const EventPendingStats& stats, std::int64_t count, std::int64_t units) {
    require(stats.pendingCount == count && stats.pendingTextUnits == units,
        "stats mismatch: " + std::to_string(stats.pendingCount) + "/" + std::to_string(stats.pendingTextUnits));
}

struct Case {
    EventStatsScope scope;
    std::int64_t count;
    std::int64_t units;
};

std::vector<Case> cases() {
    return {
        {{"main", Selection::All, {}, {}}, 3, 9},
        {{"third", Selection::All, {}, {}}, 1, 3},
        {{"absent", Selection::All, {}, {}}, 0, 0},
        {{"main", Selection::Only, {"change", "alarm", "alarm"}, {}}, 2, 6},
        {{"main", Selection::All, {}, {"change", "alarm", "alarm"}}, 1, 3},
        {{"main", Selection::Only, {}, {}}, 0, 0},
        {{"third", Selection::Only, {}, {"change"}}, 0, 0},
        {{"main", Selection::Only, {"alarm", "change"}, {"alarm"}}, 1, 2},
        {{"main", Selection::Only, {"alarm"}, {"alarm"}}, 0, 0},
        {{"main", Selection::Only, {"absent"}, {}}, 0, 0},
        {{"main", Selection::Only, {"alarm') OR 1=1 --"}, {}}, 0, 0},
        {{"main", Selection::All, {}, {"third"}}, 3, 9}
    };
}

std::vector<Outbox::EventMessage> events() {
    return {
        {"alarm", u8"\u544a\u8b66", u8"\u4e2d\u6587", 1780000000000LL, "e1", "main"},
        {"change", "t", "x", 1780000000000LL, "e2", "main"},
        {"status", "t", "xx", 1780000000000LL, "e3", "main"},
        {"alarm", "t", "xx", 1780000000000LL, "e4", "third"}
    };
}

std::string databaseState(Sql& sql) {
    return sql.query("SELECT * FROM sqlite_master ORDER BY type,name;") +
        sql.query("SELECT * FROM mqtt_event_outbox ORDER BY id;") +
        sql.query("SELECT * FROM mqtt_event_outbox_stats ORDER BY target_id,event_type;") +
        sql.query("SELECT * FROM mqtt_event_outbox_meta ORDER BY key;") +
        sql.query("PRAGMA data_version;");
}

std::map<std::string, std::string> durableFiles(const std::string& path) {
    std::map<std::string, std::string> result;
    for (const std::string suffix : {"", "-wal", "-journal"}) {
        struct stat info{};
        if (stat((path + suffix).c_str(), &info) != 0) continue;
        std::ifstream input(path + suffix, std::ios::binary);
        result[suffix] = std::to_string(info.st_mtim.tv_sec) + ':' +
            std::to_string(info.st_mtim.tv_nsec) + ':' +
            std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    return result;
}

void normalization() {
    const EventStatsScope raw{"main", Selection::Only, {"change", "alarm", "alarm"}, {"z", "z", "a"}};
    const auto canonical = normalizeEventStatsScope(raw);
    require(canonical.include == std::vector<std::string>({"alarm", "change"}) &&
        canonical.exclude == std::vector<std::string>({"a", "z"}), "not sorted/unique");
    require(sameEventStatsScope(raw, canonical), "canonical equality failed");
    require(!sameEventStatsScope({"main", Selection::All, {}, {}}, {"main", Selection::Only, {}, {}}), "Only empty became All");
    require(!sameEventStatsScope({"main", Selection::All, {}, {}}, {"third", Selection::All, {}, {}}), "target identity ignored");
    normalizeEventStatsScope({std::string(96, 'x'), Selection::Only, {std::string(96, 'a')}, {}});
    normalizeEventStatsScope({"main", Selection::Only, std::vector<std::string>(32, "a"), {}});
    for (const auto& bad : std::vector<EventStatsScope>{
            {"", Selection::All, {}, {}}, {std::string(97, 'a'), Selection::All, {}, {}}, {std::string("a\0b", 3), Selection::All, {}, {}},
            {"main", static_cast<Selection>(99), {}, {}}, {"main", Selection::All, {"alarm"}, {}},
            {"main", Selection::Only, {""}, {}}, {"main", Selection::All, {}, {""}},
            {"main", Selection::Only, {std::string(97, 'a')}, {}},
            {"main", Selection::All, {}, {std::string("a\0b", 3)}},
            {"main", Selection::Only, std::vector<std::string>(33, "alarm"), {}},
            {"main", Selection::All, {}, std::vector<std::string>(33, "alarm")}}) {
        rejects([&] { normalizeEventStatsScope(bad); });
        rejects([&] { sameEventStatsScope(bad, {"main", Selection::All, {}, {}}); });
    }
    const std::string chinese = u8"\u4e2d";
    std::string boundary;
    for (int i = 0; i < 32; ++i) boundary += chinese;
    normalizeEventStatsScope({boundary, Selection::All, {}, {}});
    rejects([&] { normalizeEventStatsScope({boundary + chinese, Selection::All, {}, {}}); });
}

void legacy(Profile profile) {
    Directory directory;
    const auto options = directory.options(profile);
    {
        Outbox writer(options.databasePath, libraryPath, 12, 24, 100, 0, profile);
        writer.enqueueBatch(events());
        const auto sent = writer.enqueue("alarm", "sent", "ignored", 1780000000000LL);
        writer.markSent(sent, 1780000000001LL);
    }
    Sql observer(options.databasePath);
    const auto before = databaseState(observer);
    const auto files = durableFiles(options.databasePath);
    {
        Outbox reader(options.databasePath, libraryPath, 12, 24, 100, 1, profile, Outbox::AccessMode::ReadOnly);
        for (int repeat = 0; repeat < 3; ++repeat) for (const auto& item : cases()) {
            check(reader.readPendingStats(item.scope), item.count, item.units);
        }
        require(reader.pendingCount() == 3 && reader.pendingCount("third") == 1,
            "legacy pendingCount changed");
        require(reader.pendingCount("main", {}) == 3, "old empty filter no longer means all");
    }
    require(before == databaseState(observer), "legacy read modified DB/schema/data version");
    require(files == durableFiles(options.databasePath), "legacy read changed durable files/mtime");
}

// SQLite authorizer observes the real Outbox connection, without exposing its private handle.
sqlite3* observedDb = nullptr;
int selectCount = 0;
bool forbidden = false;
bool denySelect = false;

int authorize(void*, int action, const char* table, const char*, const char*, const char*) {
    if (action == 21) { ++selectCount; return denySelect ? 1 : 0; }
    // SQLITE_FUNCTION is used by SUM/COALESCE/MAX and does not read event payloads.
    if (action == 31) return 0;
    if (action != 20 || !table || (std::strcmp(table, "mqtt_event_outbox_stats") &&
            std::strcmp(table, "mqtt_event_outbox_meta"))) forbidden = true;
    return 0;
}

int observeOpen(sqlite3* db, char**, const sqlite3_api_routines*) {
    observedDb = db;
    return 0;
}

void querySafety() {
    Directory directory;
    const auto options = directory.options();
    {
        Outbox writer(options.databasePath, libraryPath, 12, 24, 100);
        writer.enqueueBatch(events());
    }
    const auto autoExtension = symbol<int (*)(void (*)())>("sqlite3_auto_extension");
    require(autoExtension(reinterpret_cast<void (*)()>(observeOpen)) == 0, "auto extension failed");
    Outbox reader(options.databasePath, libraryPath, 12, 24, 100, 0, Profile::DeleteNormal, Outbox::AccessMode::ReadOnly);
    symbol<void (*)()>("sqlite3_reset_auto_extension")();
    require(observedDb != nullptr, "Outbox connection not observed");
    const auto setAuthorizer = symbol<int (*)(sqlite3*, int (*)(void*, int, const char*, const char*,
        const char*, const char*), void*)>("sqlite3_set_authorizer");
    setAuthorizer(observedDb, authorize, nullptr);
    for (const auto& item : cases()) {
        selectCount = 0;
        forbidden = false;
        check(reader.readPendingStats(item.scope), item.count, item.units);
        require(selectCount == 1 && !forbidden, "stats was not one read-only aggregate SELECT");
        require(symbol<sqlite3_stmt* (*)(sqlite3*, sqlite3_stmt*)>("sqlite3_next_stmt")(
            observedDb, nullptr) == nullptr, "stats retained cursor");
    }
    denySelect = true;
    try { reader.readPendingStats({"main", Selection::All, {}, {}}); throw std::runtime_error("expected SqliteError"); }
    catch (const SqliteError& ex) {
        require(ex.primaryCode() == 23 && ex.operation() == "store.prepare" && !ex.transactionActive(),
            "prepare diagnostics were not preserved");
    }
    denySelect = false;
    setAuthorizer(observedDb, nullptr, nullptr);
    check(reader.readPendingStats({"main", Selection::All, {}, {}}), 3, 9);
    Sql editor(options.databasePath);
    const std::vector<EventStatsScope> migrationScopes{
        {"main", Selection::All, {}, {}}, {"main", Selection::Only, {"alarm"}, {}},
        {"main", Selection::Only, {}, {}}, {"main", Selection::All, {}, {"alarm", "change"}}, {"absent", Selection::All, {}, {}}};
    // Simulate a partially built stats table; no selected subset may appear READY.
    editor.query("DELETE FROM mqtt_event_outbox_stats WHERE event_type='change';");
    for (const std::string marker : {"0", "01", "invalid"}) {
        editor.query("UPDATE mqtt_event_outbox_meta SET value='" + marker + "' WHERE key='stats_migrate_done';");
        for (const auto& scope : migrationScopes) {
            rejects([&] { reader.readPendingStats(scope); }, "migration");
        }
    }
    editor.query("DELETE FROM mqtt_event_outbox_meta WHERE key='stats_migrate_done';");
    for (const auto& scope : migrationScopes) {
        rejects([&] { reader.readPendingStats(scope); }, "migration");
    }
    editor.query("INSERT INTO mqtt_event_outbox_meta(key,value) VALUES('stats_migrate_done','1');");
    check(reader.readPendingStats({"absent", Selection::All, {}, {}}), 0, 0);
    editor.query("INSERT INTO mqtt_event_outbox_stats(target_id,event_type,pending_count,pending_bytes,updated_at) "
        "VALUES('main','change',1,2,0);");
    editor.query("UPDATE mqtt_event_outbox_stats SET pending_count=9223372036854775807 WHERE target_id='main';");
    try { reader.readPendingStats({"main", Selection::All, {}, {}}); throw std::runtime_error("expected overflow SqliteError"); }
    catch (const SqliteError& ex) {
        require(ex.primaryCode() == 1 && ex.operation() == "store.step" && !ex.transactionActive(),
            "step diagnostics were not preserved");
    }
    require(symbol<sqlite3_stmt* (*)(sqlite3*, sqlite3_stmt*)>("sqlite3_next_stmt")(
        observedDb, nullptr) == nullptr, "failure leaked cursor");
    editor.query("UPDATE mqtt_event_outbox_stats SET pending_count=1 WHERE target_id='main';");
    check(reader.readPendingStats({"main", Selection::All, {}, {}}), 3, 9);
    editor.query("UPDATE mqtt_event_outbox_stats SET pending_count=1.5 WHERE target_id='main';");
    rejects([&] { reader.readPendingStats({"main", Selection::All, {}, {}}); }, "counters");
    editor.query("UPDATE mqtt_event_outbox_stats SET pending_count=0 WHERE target_id='main';");
    rejects([&] { reader.readPendingStats({"main", Selection::All, {}, {}}); }, "counters");
    editor.query("UPDATE mqtt_event_outbox_stats SET pending_count=1,pending_bytes=9223372036854775807 "
        "WHERE target_id='main';");
    try { reader.readPendingStats({"main", Selection::All, {}, {}}); throw std::runtime_error("expected text-units overflow"); }
    catch (const SqliteError& ex) {
        require(ex.primaryCode() == 1 && ex.operation() == "store.step" && !ex.transactionActive(),
            "text-units overflow diagnostics were not preserved");
    }
    editor.query("UPDATE mqtt_event_outbox_stats SET pending_bytes=1.5 WHERE target_id='main';");
    rejects([&] { reader.readPendingStats({"main", Selection::All, {}, {}}); }, "counters");
    Outbox invalid(options.databasePath, libraryPath, 12, 24, 100, 0,
        static_cast<Profile>(99), Outbox::AccessMode::ReadOnly);
    rejects([&] { invalid.readPendingStats({"main", Selection::All, {}, {}}); }, "profile");
}

void missingSchema() {
    Directory directory;
    const auto path = directory.path + "/legacy.db";
    rejects([&] { Outbox reader(path, libraryPath, 12, 24, 100, 0,
        Profile::DeleteNormal, Outbox::AccessMode::ReadOnly); });
    require(access(path.c_str(), F_OK) != 0, "read-only open created missing DB");
    Sql fixture(path);
    fixture.query("CREATE TABLE mqtt_event_outbox(id INTEGER PRIMARY KEY,payload TEXT);");
    const auto files = durableFiles(path);
    Outbox reader(path, libraryPath, 12, 24, 100, 0, Profile::DeleteNormal, Outbox::AccessMode::ReadOnly);
    try { reader.readPendingStats({"main", Selection::All, {}, {}}); throw std::runtime_error("expected missing-schema error"); }
    catch (const SqliteError& ex) { require(ex.operation() == "store.prepare", "missing schema error lost"); }
    require(files == durableFiles(path), "legacy missing schema was migrated");
}

std::string appendArgs() {
    std::string rows;
    for (const auto& event : events()) {
        if (!rows.empty()) rows += ',';
        rows += "{\"eventId\":" + quote(event.eventId) + ",\"targetId\":" + quote(event.targetId) +
            ",\"eventType\":" + quote(event.eventType) + ",\"topic\":" + quote(event.topic) +
            ",\"payload\":" + quote(event.payload) + ",\"eventTs\":\"1780000000000\"}";
    }
    return "{\"producerId\":\"p0\",\"epoch\":\"1\",\"sequence\":\"1\",\"events\":[" + rows + "],\"states\":[]}";
}

void runtime(Profile profile) {
    Directory directory;
    const auto options = directory.options(profile);
    EventStoreRuntime service(options);
    service.start();
    const auto hello = call(options, "Hello", "{}");
    require(field(hello, "ok").asBool(), "Hello failed");
    auto empty = call(options, "GetScopedStats", scopeJson({"main", Selection::All, {}, {}}));
    require(field(empty, "ok").asBool() && field(empty, "pendingCount").asString() == "0" &&
        field(empty, "pendingTextUnits").asString() == "0", "empty runtime did not report zero");
    require(field(call(options, "RegisterProducer",
        "{\"producerId\":\"p0\",\"sessionId\":\"session\",\"expectedEpoch\":\"0\"}"), "ok").asBool(), "register failed");
    require(field(call(options, "AppendEventsAndStates", appendArgs()), "ok").asBool(), "append failed");
    Sql observer(options.databasePath);
    const auto before = databaseState(observer);
    const auto actors = observer.query("SELECT * FROM event_store_producer;") +
        observer.query("SELECT * FROM event_store_sender;");
    const auto files = durableFiles(options.databasePath);
    {
        Outbox legacyReader(options.databasePath, libraryPath, 12, 24, 100, 0, profile, Outbox::AccessMode::ReadOnly);
        for (const auto& item : cases()) {
            const auto response = call(options, "GetScopedStats", scopeJson(item.scope));
            require(field(response, "ok").asBool(), "GetScopedStats failed");
            require(response.asObject().values.size() == 13, "unexpected envelope fields");
            for (const char* key : {"version", "schemaVersion", "backend", "storeId", "configGeneration",
                    "storageProfile", "journalMode", "synchronous"}) {
                require(field(response, key).asString() == field(hello, key).asString(), "Hello/Stats identity drift");
            }
            require(field(response, "laboratoryOnly").asBool(), "lab gate lost");
            const auto actualScope = returnedScope(response);
            require(sameEventStatsScope(actualScope, item.scope), "scope echo mismatch");
            require(scopeJson(actualScope) == scopeJson(normalizeEventStatsScope(item.scope)), "scope not canonical");
            require(field(response, "pendingCount").asString() == std::to_string(item.count) &&
                field(response, "pendingTextUnits").asString() == std::to_string(item.units), "wire counts mismatch");
            check(legacyReader.readPendingStats(item.scope), item.count, item.units);
        }
    }
    const auto old = callEventStore(options.socketPath, wire(options, "GetStats", "{\"targetId\":\"main\"}"));
    require(old == "{\"ok\":true,\"pendingCount\":\"3\"}", "old GetStats envelope changed");
    for (const auto& args : std::vector<std::string>{
            "{}", "[]", "null",
            "{\"targetId\":\"main\",\"selection\":\"all\",\"include\":[]}",
            "{\"targetId\":\"main\",\"selection\":\"all\",\"include\":[],\"exclude\":[],\"producerId\":\"p0\"}",
            "{\"targetId\":\"main\",\"targetId\":\"third\",\"selection\":\"all\",\"include\":[],\"exclude\":[]}",
            "{\"targetId\":\"main\",\"selection\":\"ALL\",\"include\":[],\"exclude\":[]}",
            "{\"targetId\":\"main\",\"selection\":\"only\",\"include\":[1],\"exclude\":[]}",
            "{\"targetId\":\"main\",\"selection\":\"only\",\"include\":[],\"exclude\":null}",
            "{\"targetId\":1,\"selection\":\"all\",\"include\":[],\"exclude\":[]}",
            scopeJson({"", Selection::All, {}, {}}), scopeJson({std::string(97, 'x'), Selection::All, {}, {}}), scopeJson({std::string("a\0b", 3), Selection::All, {}, {}}),
            scopeJson({"main", Selection::All, {"alarm"}, {}}),
            scopeJson({"main", Selection::Only, {""}, {}}),
            scopeJson({"main", Selection::Only, {std::string("a\0b", 3)}, {}}),
            scopeJson({"main", Selection::Only, {std::string(97, 'x')}, {}}),
            scopeJson({"main", Selection::Only, std::vector<std::string>(33, "alarm"), {}}),
            scopeJson({"main", Selection::All, {}, std::vector<std::string>(33, "alarm")})}) {
        rejects([&] { validateEventStoreRequest(wire(options, "GetScopedStats", args), options); });
        const auto response = call(options, "GetScopedStats", args);
        require(!field(response, "ok").asBool() && field(response, "outcome").asString() == "not_queued",
            "bad request accepted or treated as pending write");
    }
    auto wrongIdentity = options;
    wrongIdentity.identity.storeId = "wrong";
    require(!field(call(wrongIdentity, "GetScopedStats", scopeJson({"main", Selection::All, {}, {}})), "ok").asBool(), "wrong identity accepted");
    require(before == databaseState(observer), "runtime reads wrote DB/schema/data version");
    require(actors == observer.query("SELECT * FROM event_store_producer;") +
        observer.query("SELECT * FROM event_store_sender;"), "stats changed actor ownership/receipts");
    require(files == durableFiles(options.databasePath), "runtime reads changed durable files/mtime");
    observer.query("DELETE FROM mqtt_event_outbox_stats WHERE event_type='change';");
    for (bool missing : {false, true}) {
        observer.query(missing ? "DELETE FROM mqtt_event_outbox_meta WHERE key='stats_migrate_done';" :
            "UPDATE mqtt_event_outbox_meta SET value='0' WHERE key='stats_migrate_done';");
        for (const EventStatsScope& scope : std::vector<EventStatsScope>{
                {"main", Selection::All, {}, {}}, {"main", Selection::Only, {"alarm"}, {}},
                {"main", Selection::Only, {}, {}}, {"main", Selection::All, {}, {"alarm", "change"}}, {"absent", Selection::All, {}, {}}}) {
            const auto invalid = call(options, "GetScopedStats", scopeJson(scope));
            require(!field(invalid, "ok").asBool() && field(invalid, "outcome").asString() == "not_committed",
                "runtime accepted incomplete migration or reported unknown write");
        }
    }
    observer.query("INSERT INTO mqtt_event_outbox_meta(key,value) VALUES('stats_migrate_done','1');");
    for (const std::string value : {"9223372036854775807", "1.5", "0"}) {
        observer.query("UPDATE mqtt_event_outbox_stats SET pending_count=" + value + " WHERE target_id='main';");
        const auto invalid = call(options, "GetScopedStats", scopeJson({"main", Selection::All, {}, {}}));
        require(!field(invalid, "ok").asBool() && field(invalid, "outcome").asString() == "not_committed",
            "runtime accepted inconsistent/overflow/floating counters");
    }
    for (const std::string value : {"9223372036854775807", "1.5"}) {
        observer.query("UPDATE mqtt_event_outbox_stats SET pending_count=1,pending_bytes=" + value +
            " WHERE target_id='main';");
        const auto invalid = call(options, "GetScopedStats", scopeJson({"main", Selection::All, {}, {}}));
        require(!field(invalid, "ok").asBool() && field(invalid, "outcome").asString() == "not_committed",
            "runtime accepted overflow/floating text units");
    }
    service.stop();
}

void runtimeProfile() {
    Directory directory;
    for (auto profile : {Profile::DeleteNormal, Profile::WalNormal, static_cast<Profile>(99)}) {
        EventStoreRuntime service(directory.options(profile));
        rejects([&] { service.start(); }, "FULL");
    }
    require(access(directory.options().databasePath.c_str(), F_OK) != 0, "invalid profile created DB");
}

} // namespace

int main(int argc, char** argv) {
    const auto* configured = std::getenv("SQLITE_LIBRARY");
    libraryPath = argc > 1 ? argv[1] : (configured && *configured ? configured : "libsqlite3.so.0");
    library = dlopen(libraryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) { std::cerr << "cannot load test SQLite\n"; return 1; }
    int failed = 0;
    const auto run = [&](const char* name, const std::function<void()>& action) {
        try { action(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& ex) { ++failed; std::cerr << "FAIL " << name << ": " << ex.what() << '\n'; }
    };
    run("scope-normalization-boundaries", normalization);
    run("legacy-delete-normal-readonly", [] { legacy(Profile::DeleteNormal); });
    run("legacy-wal-full-readonly", [] { legacy(Profile::WalFull); });
    run("single-select-raii-errors-migration", querySafety);
    run("legacy-missing-schema-no-migration", missingSchema);
    run("runtime-delete-full-protocol-parity", [] { runtime(Profile::DeleteFull); });
    run("runtime-wal-full-protocol-parity", [] { runtime(Profile::WalFull); });
    run("runtime-reject-invalid-profile", runtimeProfile);
    return failed ? 1 : 0;
}
