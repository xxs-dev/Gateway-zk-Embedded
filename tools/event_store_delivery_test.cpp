#include "edge_gateway/event_store.hpp"
#include "edge_gateway/event_store_clock.hpp"
#include "edge_gateway/json_value.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

struct sqlite3;

namespace {
using namespace edge_gateway;
using Outbox = MqttEventOutbox;
using Json = json::JsonValue;
using Item = EventStoreClaimItem;
using Request = EventStoreDeliveryRequest;
const char* const bootOne = "11111111-1111-1111-1111-111111111111";
const char* const bootTwo = "22222222-2222-2222-2222-222222222222";
std::string testLibrary;

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

template <typename Action> void rejectsInvalidArgument(Action action) {
    try { action(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("expected invalid_argument");
}

const Json& field(const Json& value, const std::string& name) {
    const auto* result = value.find(name);
    require(result != nullptr, "missing JSON field: " + name);
    return *result;
}

std::string stringField(const Json& value, const std::string& name) {
    return field(value, name).asString();
}

std::int64_t integerField(const Json& value, const std::string& name) {
    const auto& number = field(value, name);
    if (number.isString()) {
        std::size_t consumed = 0;
        const auto result = std::stoll(number.asString(), &consumed);
        require(consumed == number.asString().size(), "invalid integer field: " + name);
        return result;
    }
    require(number.isNumber(), "not an integer field: " + name);
    // Fixture ids/timestamps are below the exact JSON-number integer limit.
    const auto raw = number.asNumber();
    require(raw >= 0 && raw <= 9007199254740991.0, "integer field outside test range: " + name);
    const auto result = static_cast<std::int64_t>(raw);
    require(static_cast<double>(result) == raw, "fractional integer field: " + name);
    return result;
}

Json success(const std::string& raw, const std::string& sender, std::int64_t epoch,
    std::int64_t sequence, const std::string& status = {}) {
    const auto result = json::JsonParser(raw, 16, 8192).parse();
    require(field(result, "ok").asBool(), "delivery API did not return ok: " + raw);
    require(stringField(result, "senderId") == sender, "receipt sender mismatch");
    require(stringField(result, "epoch") == std::to_string(epoch), "receipt epoch must be a decimal string");
    require(stringField(result, "sequence") == std::to_string(sequence), "receipt sequence must be a decimal string");
    const auto actual = stringField(result, "status");
    require(!actual.empty() && (status.empty() || actual == status), "unexpected receipt status: " + actual);
    return result;
}

void noPayload(const Json& value) {
    require(!value.find("payload"), "inactive receipt exposed payload");
    const auto* messages = value.find("messages");
    require(!messages || messages->asArray().values.empty(), "inactive receipt exposed old messages");
}

struct Directory {
    std::string path;
    Directory() {
        char name[] = "/tmp/event_store_delivery_test_XXXXXX";
        auto* created = mkdtemp(name);
        require(created != nullptr, "mkdtemp failed");
        path = created;
    }
    ~Directory() {
        auto* dir = opendir(path.c_str());
        if (dir) {
            while (auto* entry = readdir(dir)) {
                if (std::strcmp(entry->d_name, ".") && std::strcmp(entry->d_name, "..")) {
                    unlink((path + "/" + entry->d_name).c_str());
                }
            }
            closedir(dir);
        }
        rmdir(path.c_str());
    }
};

std::vector<EventStoreSenderConfig> senderConfigs() {
    return {{"main-sender", "main", {"alarm", "change"}},
        {"peer-sender", "main", {"alarm", "change"}},
        {"management-sender", "main", {"management"}},
        {"third-sender", "third", {"alarm", "change"}}};
}

struct Database {
    Directory temp;
    EventStoreLeaseTime now{bootOne, 100000};
    EventStoreIdentity identity{"delivery-test-store", "delivery-v1"};
    std::vector<std::string> producers{"p0"};
    std::vector<EventStoreSenderConfig> senders = senderConfigs();
    std::string library;
    Outbox::StorageProfile profile;
    std::size_t maxDiskBytes;
    std::unique_ptr<Outbox> box;
    std::unique_ptr<EventStoreDatabase> store;
    std::int64_t appendSequence = 0;

    explicit Database(const std::string& sqlite = testLibrary,
        Outbox::StorageProfile storage = Outbox::StorageProfile::DeleteFull, std::size_t capacity = 0)
        : library(sqlite), profile(storage), maxDiskBytes(capacity) { reopen(); }

    std::string path() const { return temp.path + "/events.db"; }

    void reopen() {
        store.reset();
        box.reset();
        box.reset(new Outbox(path(), library, 12, 24, 100, maxDiskBytes, profile));
        store.reset(new EventStoreDatabase(*box, identity, producers, false, senders, [this] { return now; }));
    }

    std::string registerSender(const std::string& sender = "main-sender",
        const std::string& session = "first-session", std::int64_t expected = 0) {
        return store->registerSender(sender, session, expected);
    }

    void seed(const std::vector<Outbox::EventMessage>& events) {
        if (appendSequence == 0) store->registerProducer("p0", "producer-session", 0);
        EventStoreAppend request;
        request.producerId = "p0";
        request.epoch = 1;
        request.sequence = appendSequence + 1;
        request.request = "events-only-" + std::to_string(request.sequence);
        request.events = events;
        require(store->append(request).sequence == request.sequence, "seed append failed");
        appendSequence = request.sequence;
        require(store->states("p0", "", 8).empty(), "events-only append created states");
    }
};

Outbox::EventMessage event(const std::string& id, const std::string& type = "change",
    const std::string& target = "main", std::int64_t timestamp = 1780000000000LL,
    const std::string& payload = "{\"value\":1}") {
    Outbox::EventMessage result;
    result.eventId = id;
    result.eventType = type;
    result.targetId = target;
    result.topic = "test/" + target + "/" + type;
    result.payload = payload;
    result.eventTs = timestamp;
    return result;
}

Request claimRequest(std::int64_t sequence = 1, const std::string& sender = "main-sender",
    std::int64_t epoch = 1) {
    Request result;
    result.operation = "ClaimBatch";
    result.senderId = sender;
    result.epoch = epoch;
    result.sequence = sequence;
    result.request = "immutable-claim-" + sender + "-" + std::to_string(sequence);
    return result;
}

struct Claim {
    std::string raw;
    std::string token;
    std::vector<Item> items;
    Json response;
};

Claim claim(Database& db, const Request& request, std::size_t count) {
    Claim result;
    result.raw = db.store->deliver(request);
    result.response = success(result.raw, request.senderId, request.epoch, request.sequence,
        count ? "CLAIMED" : "EMPTY");
    const auto& messages = field(result.response, "messages").asArray().values;
    require(messages.size() == count, "unexpected claimed message count");
    if (count) {
        result.token = stringField(result.response, "claimToken");
        require(!result.token.empty(), "empty claim token");
        require(stringField(result.response, "bootId") == db.now.bootId, "claim boot mismatch");
        require(integerField(result.response, "leaseUntilMs") == db.now.milliseconds + request.leaseMs,
            "claim deadline is not based on the injected clock");
    }
    for (const auto& message : messages) {
        Item item;
        item.id = integerField(*message, "id");
        item.eventId = stringField(*message, "eventId");
        require(item.id > 0 && !item.eventId.empty(), "claim missing stable row identity");
        require(!stringField(*message, "eventType").empty(), "claim missing event type");
        require(!stringField(*message, "topic").empty(), "claim missing topic");
        stringField(*message, "payload");
        require(integerField(*message, "eventTs") > 0, "claim missing event timestamp");
        require(std::none_of(result.items.begin(), result.items.end(), [&](const Item& old) {
            return old.id == item.id;
        }), "claim duplicated a row");
        result.items.push_back(item);
    }
    require(db.store->deliveryReceipt(request.senderId) == result.raw, "claim did not persist its receipt");
    return result;
}

Request finishRequest(const std::string& operation, std::int64_t sequence, const Claim& batch,
    const std::vector<Item>& items, const std::string& sender = "main-sender", std::int64_t epoch = 1) {
    auto result = claimRequest(sequence, sender, epoch);
    result.operation = operation;
    result.request = "immutable-" + operation + "-" + sender + "-" + std::to_string(sequence);
    result.claimToken = batch.token;
    result.items = items;
    return result;
}

std::string finish(Database& db, const Request& request, const std::vector<std::string>& expected) {
    require(expected.size() == request.items.size(), "invalid test result expectations");
    const auto raw = db.store->deliver(request);
    const auto response = success(raw, request.senderId, request.epoch, request.sequence, "FINISHED");
    const auto& results = field(response, "results").asArray().values;
    require(results.size() == request.items.size(), "missing per-item completion results");
    for (std::size_t i = 0; i < request.items.size(); ++i) {
        const auto& item = request.items[i];
        const auto found = std::find_if(results.begin(), results.end(), [&](const std::shared_ptr<Json>& row) {
            return integerField(*row, "id") == item.id && stringField(*row, "eventId") == item.eventId;
        });
        require(found != results.end(), "completion result lost id/eventId");
        const auto status = stringField(**found, "status");
        // A made-up/obsolete token may be classified before or after claim-history lookup.
        const bool refused = expected[i] == "REFUSED" &&
            (status == "NOT_CLAIMED" || status == "REVOKED" || status == "EXPIRED" || status == "ALREADY_RELEASED");
        require(status == expected[i] || refused, "unexpected item status for " + item.eventId + ": " + status);
    }
    require(db.store->deliveryReceipt(request.senderId) == raw, "completion did not persist its receipt");
    return raw;
}

void rejectsUnchanged(Database& db, const Request& request) {
    const auto before = db.store->deliveryReceipt(request.senderId);
    const auto pending = db.box->pendingCount("main");
    rejects([&] { db.store->deliver(request); });
    require(db.store->deliveryReceipt(request.senderId) == before, "rejection advanced/overwrote receipt");
    require(db.box->pendingCount("main") == pending, "rejection modified pending events");
}

// Read-only by default; writable access is only for stopped-database fault fixtures.
struct DataVersion {
    void* library = nullptr;
    sqlite3* db = nullptr;
    using Exec = int (*)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
    Exec execute = nullptr;
    int (*closeDb)(sqlite3*) = nullptr;

    DataVersion(const std::string& path, const std::string& sqlite, bool writable = false) {
        library = dlopen(sqlite.empty() ? "libsqlite3.so.0" : sqlite.c_str(), RTLD_NOW | RTLD_LOCAL);
        require(library != nullptr, "observer SQLite load failed");
        const auto openDb = reinterpret_cast<int (*)(const char*, sqlite3**, int, const char*)>(dlsym(library, "sqlite3_open_v2"));
        execute = reinterpret_cast<Exec>(dlsym(library, "sqlite3_exec"));
        closeDb = reinterpret_cast<int (*)(sqlite3*)>(dlsym(library, "sqlite3_close_v2"));
        require(openDb && execute && closeDb, "observer SQLite symbols missing");
        require(openDb(path.c_str(), &db, writable ? 2 : 1, nullptr) == 0, "observer SQLite open failed");
    }
    ~DataVersion() {
        if (db && closeDb) closeDb(db);
        if (library) dlclose(library);
    }
    std::int64_t scalar(const std::string& sql) const {
        std::int64_t version = -1;
        const auto callback = [](void* context, int count, char** values, char**) -> int {
            if (count != 1 || !values[0]) return 1;
            *static_cast<std::int64_t*>(context) = std::strtoll(values[0], nullptr, 10);
            return 0;
        };
        require(execute(db, sql.c_str(), callback, &version, nullptr) == 0 && version >= 0,
            "cannot read SQLite scalar: " + sql);
        return version;
    }
    std::int64_t get() const { return scalar("PRAGMA data_version;"); }
};

void causalDeliveryOrder(Outbox::StorageProfile profile) {
    Database db(testLibrary, profile);
    const std::vector<Outbox::EventMessage> originals{
        event("change-first", "change", "main", 9000),
        event("change-backward", "change", "main", 1000),
        event("change-third", "change", "main", 5000),
        event("alarm-first", "alarm", "main", 12000),
        event("alarm-backward", "alarm", "main", 500)};
    for (const auto& original : originals) {
        auto third = original; third.targetId = "third";
        db.seed({original, third});
    }
    {
        DataVersion observer(db.path(), db.library);
        std::string plan;
        const auto collect = [](void* context, int count, char** values, char**) -> int {
            if (count != 4 || !values[3]) return 1;
            *static_cast<std::string*>(context) += std::string(values[3]) + '\n';
            return 0;
        };
        require(observer.execute(observer.db,
            "EXPLAIN QUERY PLAN SELECT o.id,o.event_id,o.event_type,o.event_ts,"
            "length(CAST(o.topic AS BLOB))+length(CAST(o.payload AS BLOB)) "
            "FROM mqtt_event_outbox o LEFT JOIN event_store_claim_item i ON i.row_id=o.id "
            "LEFT JOIN event_store_claim_batch b ON b.sender_id=i.sender_id "
            "WHERE o.target_id='main' AND o.event_type='alarm' AND o.sent=0 "
            "AND (i.row_id IS NULL OR i.state!='active' OR b.boot_id!='test-boot' OR b.lease_until<=100000) "
            "AND (o.claim_token IS NULL OR o.claim_token=b.claim_token) ORDER BY o.id LIMIT 8;",
            collect, &plan, nullptr) == 0, "cannot inspect causal claim query plan");
        require(plan.find("idx_event_store_delivery_order") != std::string::npos &&
            plan.find("TEMP B-TREE") == std::string::npos, "causal claim query needs a temporary sort: " + plan);
    }
    db.registerSender(); db.registerSender("third-sender");
    auto one = claimRequest(); one.limit = 1;
    const auto head = claim(db, one, 1);
    require(head.items[0].eventId == "alarm-first", "limited SQL candidates reordered by wall clock");
    finish(db, finishRequest("AckBatch", 2, head, head.items), {"APPLIED"});
    db.reopen();
    const auto rest = claim(db, claimRequest(3), 4);
    const std::vector<std::string> expected{"alarm-backward", "change-first", "change-backward", "change-third"};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        require(rest.items[i].eventId == expected[i], "merged candidates lost type priority/commit order");
        const auto original = std::find_if(originals.begin(), originals.end(), [&](const Outbox::EventMessage& e) {
            return e.eventId == expected[i];
        });
        require(integerField(*field(rest.response, "messages").asArray().values[i], "eventTs") == original->eventTs,
            "delivery changed original timestamp");
    }
    const auto third = claim(db, claimRequest(1, "third-sender"), 5);
    const std::vector<std::string> thirdExpected{"alarm-first", "alarm-backward", "change-first", "change-backward", "change-third"};
    for (std::size_t i = 0; i < thirdExpected.size(); ++i)
        require(third.items[i].eventId == thirdExpected[i], "third target lost independent causal order");
}

void legacyWallClockOrder() {
    Database db;
    db.seed({event("legacy-first", "change", "main", 9000, "first"),
        event("legacy-backward", "change", "main", 1000, "backward")});
    std::vector<std::string> sent;
    db.box->replay("main", [&](const std::string&, const std::string& payload) { sent.push_back(payload); });
    require(sent == std::vector<std::string>({"backward", "first"}), "Legacy wall-clock ordering changed");
}

void registrationAndSequence() {
    Database db;
    rejects([&] { db.store->registerSender("unknown", "session", 0); });
    rejects([&] { db.store->deliveryReceipt("unknown"); });
    rejects([&] { db.store->deliver(claimRequest(1, "unknown")); });
    rejects([&] { db.store->deliver(claimRequest()); });
    const auto registered = db.registerSender();
    success(registered, "main-sender", 1, 0);
    require(db.registerSender() == registered, "same-session original-CAS registration not idempotent");
    rejects([&] { db.registerSender("main-sender", "first-session", 99); });
    rejects([&] { db.registerSender("main-sender", "replacement", 0); });
    for (const auto sequence : {std::int64_t(0), std::int64_t(-1), std::int64_t(2)}) {
        rejectsUnchanged(db, claimRequest(sequence));
    }
    const auto empty = claim(db, claimRequest(), 0);
    require(db.store->deliver(claimRequest()) == empty.raw, "EMPTY retry changed receipt");
    auto changed = claimRequest();
    changed.request += " ";
    rejectsUnchanged(db, changed);
    db.registerSender();
    require(db.store->deliveryReceipt("main-sender") == empty.raw, "registration retry erased last receipt");
    db.reopen();
    require(db.store->deliveryReceipt("main-sender") == empty.raw, "reopen lost sender sequence");
    const auto next = claim(db, claimRequest(2), 0);
    rejectsUnchanged(db, claimRequest(1));
    require(db.store->deliveryReceipt("main-sender") == next.raw, "old sequence replaced high water");
}

void batchLifecycle(Outbox::StorageProfile profile) {
    Database db(testLibrary, profile);
    const auto late = event("late", "change", "main", 1780000000002LL);
    const auto first = event("first", "change", "main", 1780000000001LL,
        "{\"text\":\"quote\\\" slash\\\\\"}\n\xe4\xb8\xad\xe6\x96\x87");
    const auto second = event("second", "change", "main", 1780000000001LL);
    db.seed({late, first, second});
    db.registerSender();
    auto request = claimRequest();
    request.limit = 2;
    const auto batch = claim(db, request, 2);
    require(batch.items[0].eventId == "late" && batch.items[1].eventId == "first",
        "claim did not use stable id ordering within type");
    const auto& message = *field(batch.response, "messages").asArray().values[1];
    require(stringField(message, "topic") == first.topic && stringField(message, "payload") == first.payload &&
        stringField(message, "eventType") == first.eventType && integerField(message, "eventTs") == first.eventTs,
        "claim changed stored event fields/escaping");
    require(db.box->pendingCount("main") == 3, "claim marked events sent");
    ++db.now.milliseconds;
    require(db.store->deliver(request) == batch.raw, "claim retry replaced token/batch or extended lease");
    for (int mutation = 0; mutation < 7; ++mutation) {
        auto conflict = request;
        switch (mutation) {
        case 0: conflict.request += "changed"; break;
        case 1: ++conflict.limit; break;
        case 2: --conflict.maxBytes; break;
        case 3: ++conflict.leaseMs; break;
        case 4: conflict.operation = "ReleaseBatch"; break;
        case 5: conflict.claimToken = "changed-token"; break;
        case 6: conflict.items = batch.items; break;
        }
        rejectsUnchanged(db, conflict);
    }
    rejectsUnchanged(db, claimRequest(3));
    rejectsUnchanged(db, claimRequest(2));
    const auto ack = finishRequest("AckBatch", 2, batch, {batch.items[0]});
    const auto ackRaw = finish(db, ack, {"APPLIED"});
    require(db.box->pendingCount("main") == 2, "partial ACK did not change exactly one row");
    DataVersion observer(db.path(), db.library);
    const auto version = observer.get();
    db.now.milliseconds += 10;
    require(db.store->deliver(ack) == ackRaw && observer.get() == version, "same-sequence ACK performed a second write");
    for (int mutation = 0; mutation < 5; ++mutation) {
        auto conflict = ack;
        switch (mutation) {
        case 0: conflict.request += "changed"; break;
        case 1: conflict.claimToken += "changed"; break;
        case 2: conflict.items[0].id = batch.items[1].id; break;
        case 3: conflict.items[0].eventId = batch.items[1].eventId; break;
        case 4: conflict.items.push_back(batch.items[1]); break;
        }
        rejectsUnchanged(db, conflict);
    }
    rejectsUnchanged(db, claimRequest(3));
    finish(db, finishRequest("AckBatch", 3, batch, {batch.items[0]}), {"ALREADY_ACKED"});
    rejectsUnchanged(db, claimRequest(4));
    const auto release = finishRequest("ReleaseBatch", 4, batch, {batch.items[1]});
    const auto released = finish(db, release, {"APPLIED"});
    require(observer.get() != version, "data_version observer did not notice a real commit");
    const auto releaseVersion = observer.get();
    require(db.store->deliver(release) == released && observer.get() == releaseVersion,
        "same-sequence Release performed a second write");
    finish(db, finishRequest("ReleaseBatch", 5, batch, {batch.items[1]}), {"ALREADY_RELEASED"});
    const auto again = claim(db, claimRequest(6), 2);
    require(again.token != batch.token && again.items[0].eventId == "first" && again.items[1].eventId == "second",
        "release did not make remaining rows reclaimable with a fresh token");
    finish(db, finishRequest("AckBatch", 7, batch, {batch.items[1]}), {"REFUSED"});
    require(db.box->pendingCount("main") == 2, "old token ACK consumed a newly claimed row");
    const auto finalAck = finishRequest("AckBatch", 8, again, again.items);
    finish(db, finalAck, {"APPLIED", "APPLIED"});
    auto reordered = finalAck;
    std::reverse(reordered.items.begin(), reordered.items.end());
    rejectsUnchanged(db, reordered);
    require(db.box->pendingCount("main") == 0, "completed batch left pending events");
    claim(db, claimRequest(9), 0);
}

void targetTypeAndIdentityIsolation() {
    Database db;
    db.seed({event("shared"), event("shared", "change", "third"), event("management", "management"),
        event("alarm", "alarm")});
    for (const auto& sender : db.senders) db.registerSender(sender.senderId);
    const auto main = claim(db, claimRequest(), 2);
    require(main.items[0].eventId == "alarm" && main.items[1].eventId == "shared",
        "main sender scope or alarm-before-change priority changed");
    const auto third = claim(db, claimRequest(1, "third-sender"), 1);
    const auto management = claim(db, claimRequest(1, "management-sender"), 1);
    claim(db, claimRequest(1, "peer-sender"), 0);
    require(third.items[0].eventId == "shared" && management.items[0].eventId == "management",
        "sender scope selected the wrong event");
    Item wrongEvent = main.items[0];
    wrongEvent.eventId = "wrong-event-id";
    Item wrongRow = main.items[0];
    wrongRow.id = 9000000000LL;
    auto forged = finishRequest("AckBatch", 2, main, {main.items[0]});
    forged.claimToken = third.token;
    finish(db, forged, {"REFUSED"});
    finish(db, finishRequest("AckBatch", 3, main,
        {third.items[0], management.items[0], wrongEvent, wrongRow}),
        {"NOT_CLAIMED", "NOT_CLAIMED", "NOT_CLAIMED", "NOT_CLAIMED"});
    finish(db, finishRequest("ReleaseBatch", 4, main, {third.items[0], management.items[0]}),
        {"NOT_CLAIMED", "NOT_CLAIMED"});
    require(db.box->pendingCount("main") == 3 && db.box->pendingCount("third") == 1,
        "invalid completion crossed identity/target/type boundaries");
    rejectsUnchanged(db, claimRequest(5));
    finish(db, finishRequest("AckBatch", 5, main, {main.items[0], third.items[0], main.items[1]}),
        {"APPLIED", "NOT_CLAIMED", "APPLIED"});
    require(db.box->pendingCount("main") == 1 && db.box->pendingCount("third") == 1,
        "authorized main ACK consumed unrelated events");
    finish(db, finishRequest("AckBatch", 2, management, management.items, "management-sender"), {"APPLIED"});
    finish(db, finishRequest("AckBatch", 2, third, third.items, "third-sender"), {"APPLIED"});
    require(db.box->pendingCount("main") == 0 && db.box->pendingCount("third") == 0, "scoped ACK did not finish");
}

void sessionFence() {
    Database db;
    db.seed({event("fenced")});
    db.registerSender();
    const auto old = claim(db, claimRequest(), 1);
    db.registerSender();
    require(db.store->deliveryReceipt("main-sender") == old.raw, "same-session retry revoked its claim");
    success(db.registerSender("main-sender", "replacement", 1), "main-sender", 2, 0);
    success(db.registerSender("main-sender", "replacement", 1), "main-sender", 2, 0);
    rejects([&] { db.registerSender("main-sender", "first-session", 0); });
    rejects([&] { db.registerSender("main-sender", "third-session", 1); });
    rejectsUnchanged(db, claimRequest());
    rejectsUnchanged(db, finishRequest("AckBatch", 2, old, old.items));
    rejectsUnchanged(db, finishRequest("ReleaseBatch", 2, old, old.items));
    finish(db, finishRequest("AckBatch", 1, old, old.items, "main-sender", 2), {"REFUSED"});
    require(db.box->pendingCount("main") == 1, "new epoch accepted the old epoch token");
    const auto replacement = claim(db, claimRequest(2, "main-sender", 2), 1);
    require(replacement.items[0].id == old.items[0].id && replacement.token != old.token,
        "registration fence did not revoke old claim");
    db.reopen();
    require(db.store->deliveryReceipt("main-sender") == replacement.raw, "reopen reset sender epoch/claim");
    finish(db, finishRequest("AckBatch", 3, replacement, replacement.items, "main-sender", 2), {"APPLIED"});
}

void sameBootRestart(Outbox::StorageProfile profile) {
    Database db(testLibrary, profile);
    db.seed({event("restart-a"), event("restart-b")});
    db.registerSender();
    const auto request = claimRequest();
    const auto original = claim(db, request, 2);
    db.now.milliseconds += 100;
    db.reopen();
    require(db.store->deliveryReceipt("main-sender") == original.raw && db.store->deliver(request) == original.raw,
        "same-boot restart discarded or renewed a live claim");
    db.registerSender();
    require(db.store->deliveryReceipt("main-sender") == original.raw, "reconnect registration erased live claim");
    rejectsUnchanged(db, claimRequest(2));
    db.registerSender("peer-sender");
    claim(db, claimRequest(1, "peer-sender"), 0);
    const auto ack = finishRequest("AckBatch", 2, original, original.items);
    const auto receipt = finish(db, ack, {"APPLIED", "APPLIED"});
    db.reopen();
    DataVersion observer(db.path(), db.library);
    const auto version = observer.get();
    require(db.store->deliveryReceipt("main-sender") == receipt && db.store->deliver(ack) == receipt &&
        observer.get() == version && db.box->pendingCount("main") == 0,
        "restart lost ACK receipt or repeated its write");
}

void expiredClaim(bool reboot) {
    Database db;
    db.seed({event("expires-a"), event("expires-b")});
    db.registerSender();
    const auto request = claimRequest();
    const auto original = claim(db, request, 2);
    if (reboot) {
        db.now = {bootTwo, 10};
        db.reopen();
    } else {
        db.now.milliseconds = integerField(original.response, "leaseUntilMs");
    }
    noPayload(success(db.store->deliveryReceipt("main-sender"), "main-sender", 1, 1, "EXPIRED"));
    noPayload(success(db.store->deliver(request), "main-sender", 1, 1, "EXPIRED"));
    require(db.box->pendingCount("main") == 2, "expiry marked rows sent");
    finish(db, finishRequest("AckBatch", 2, original, original.items), {"EXPIRED", "EXPIRED"});
    const auto renewed = claim(db, claimRequest(3), 2);
    require(renewed.token != original.token && renewed.items[0].id == original.items[0].id &&
        renewed.items[1].id == original.items[1].id, "expiry did not permit a fresh claim of the same rows");
    finish(db, finishRequest("AckBatch", 4, original, original.items), {"REFUSED", "REFUSED"});
    require(db.box->pendingCount("main") == 2, "expired token consumed new claim");
    rejectsUnchanged(db, claimRequest(5));
    finish(db, finishRequest("AckBatch", 5, renewed, renewed.items), {"APPLIED", "APPLIED"});
}

void expiredBatchAllowsNextClaim() {
    Database db;
    db.seed({event("expire-without-ack")});
    db.registerSender();
    const auto old = claim(db, claimRequest(), 1);
    db.now.milliseconds += 5000;
    const auto next = claim(db, claimRequest(2), 1);
    require(next.token != old.token && next.items[0].id == old.items[0].id,
        "expired unfinished batch blocked the next sequence");
    finish(db, finishRequest("ReleaseBatch", 3, old, old.items), {"REFUSED"});
    rejectsUnchanged(db, claimRequest(4));
    finish(db, finishRequest("AckBatch", 4, next, next.items), {"APPLIED"});
}

void partiallyExpiredResults() {
    Database db;
    db.seed({event("already-sent"), event("still-pending")});
    db.registerSender();
    const auto batch = claim(db, claimRequest(), 2);
    finish(db, finishRequest("AckBatch", 2, batch, {batch.items[0]}), {"APPLIED"});
    db.now.milliseconds = integerField(batch.response, "leaseUntilMs");
    finish(db, finishRequest("AckBatch", 3, batch, batch.items), {"ALREADY_ACKED", "EXPIRED"});
    require(db.box->pendingCount("main") == 1, "partially expired ACK changed the expired row");
    const auto remaining = claim(db, claimRequest(4), 1);
    require(remaining.items[0].eventId == "still-pending", "partial expiry reclaimed an ACKed event");
    finish(db, finishRequest("AckBatch", 5, remaining, remaining.items), {"APPLIED"});
}

void limitsAndOversizedHead() {
    Database db;
    std::string multibyte;
    for (int i = 0; i < 240; ++i) multibyte += "\xe4\xb8\xad";
    db.seed({event("bytes-a", "change", "main", 1780000000000LL, multibyte),
        event("bytes-b", "change", "main", 1780000000001LL, multibyte),
        event("bytes-c", "change", "main", 1780000000002LL, std::string(720, 'c'))});
    db.registerSender();
    auto request = claimRequest();
    request.maxBytes = 1024;
    const auto limited = claim(db, request, 1);
    require(limited.items[0].eventId == "bytes-a", "byte limit skipped the queue head");
    finish(db, finishRequest("ReleaseBatch", 2, limited, limited.items), {"APPLIED"});
    auto counted = claimRequest(3);
    counted.limit = 2;
    const auto two = claim(db, counted, 2);
    finish(db, finishRequest("AckBatch", 4, two, two.items), {"APPLIED", "APPLIED"});
    const auto tail = claim(db, claimRequest(5), 1);
    require(tail.items[0].eventId == "bytes-c", "count limit lost/skipped the tail");

    Database oversized;
    oversized.seed({event("oversized", "change", "main", 1780000000000LL, std::string(2048, 'x')),
        event("small-tail", "change", "main", 1780000000001LL)});
    oversized.registerSender();
    auto small = claimRequest();
    small.maxBytes = 1024;
    rejectsUnchanged(oversized, small);
    oversized.reopen();
    success(oversized.store->deliveryReceipt("main-sender"), "main-sender", 1, 0);
    auto sufficient = claimRequest();
    sufficient.maxBytes = 4096;
    sufficient.limit = 1;
    const auto head = claim(oversized, sufficient, 1);
    require(head.items[0].eventId == "oversized", "oversized head was silently skipped/partially claimed");
    finish(oversized, finishRequest("AckBatch", 2, head, head.items), {"APPLIED"});
    require(claim(oversized, claimRequest(3), 1).items[0].eventId == "small-tail", "oversize failure lost tail");
}

void readOnlyContract() {
    Database db;
    db.seed({event("readonly")});
    db.registerSender();
    const auto batch = claim(db, claimRequest(), 1);
    Outbox read(db.path(), db.library, 12, 24, 100, 0, Outbox::StorageProfile::WalFull, Outbox::AccessMode::ReadOnly);
    EventStoreDatabase reader(read, db.identity, db.producers, true, db.senders, [&] { return db.now; });
    require(read.storageSettings().journalMode == "delete", "read-only open changed journal mode");
    rejects([&] { reader.registerSender("main-sender", "replacement", 1); });
    rejects([&] { reader.deliver(claimRequest(2)); });
    rejects([&] { reader.deliver(finishRequest("AckBatch", 2, batch, batch.items)); });
    rejects([&] { reader.deliver(finishRequest("ReleaseBatch", 2, batch, batch.items)); });
    require(db.box->pendingCount("main") == 1 && db.store->deliveryReceipt("main-sender") == batch.raw,
        "read-only delivery mutated data");
}

void claimedRowPruneProtection() {
    Database db(testLibrary, Outbox::StorageProfile::DeleteFull, 1024);
    db.seed({event("protected-head", "change", "main", 1600000000000LL, std::string(300, 'p')),
        event("disposable", "change", "main", 1600000000001LL, std::string(300, 'd'))});
    db.registerSender();
    auto request = claimRequest();
    request.limit = 1;
    const auto batch = claim(db, request, 1);
    require(batch.items[0].eventId == "protected-head", "prune fixture did not claim the oldest row");
    DataVersion observer(db.path(), db.library);
    const auto protectedId = std::to_string(batch.items[0].id);
    require(observer.scalar("SELECT claim_until FROM mqtt_event_outbox WHERE id=" + protectedId + ";") ==
        std::numeric_limits<std::int64_t>::max(), "delivery claim did not install legacy INT64_MAX protection");
    db.seed({event("incoming", "change", "main", 1600000000002LL, std::string(600, 'i'))});
    require(observer.scalar("SELECT count(*) FROM mqtt_event_outbox WHERE event_id='disposable';") == 0,
        "capacity fixture did not actually prune an unclaimed row");
    require(db.box->pendingCount("main") == 2 && db.store->deliver(request) == batch.raw,
        "pruning deleted or invalidated the claimed row");
    const auto producerReceipt = db.store->receipt("p0").receipt;
    rejects([&] { db.seed({event("too-much", "change", "main", 1600000000003LL, std::string(900, 'x'))}); });
    require(db.box->pendingCount("main") == 2 && db.store->receipt("p0").receipt == producerReceipt &&
        db.store->deliver(request) == batch.raw,
        "capacity failure pruned a claimed row or failed to roll back other pruning");
    require(observer.scalar("SELECT count(*) FROM mqtt_event_outbox WHERE event_id='incoming';") == 1 &&
        observer.scalar("SELECT count(*) FROM mqtt_event_outbox WHERE event_id='too-much';") == 0,
        "capacity failure left partial append/pruning");
    finish(db, finishRequest("ReleaseBatch", 2, batch, batch.items), {"APPLIED"});
    db.seed({event("after-release", "change", "main", 1600000000004LL, std::string(900, 'r'))});
    require(observer.scalar("SELECT count(*) FROM mqtt_event_outbox WHERE id=" + protectedId + ";") == 0 &&
        db.box->pendingCount("main") == 1, "released claim kept permanent pruning protection");
    require(claim(db, claimRequest(3), 1).items[0].eventId == "after-release", "post-prune claim selected wrong row");
}

void claimedRowCleanupProtection() {
    Database db;
    db.seed({event("old-sent", "change", "main", 946684800000LL),
        event("old-claimed", "change", "main", 946684800001LL),
        event("old-unclaimed", "change", "main", 946684800002LL)});
    db.registerSender();
    auto request = claimRequest();
    request.limit = 2;
    const auto batch = claim(db, request, 2);
    finish(db, finishRequest("AckBatch", 2, batch, {batch.items[0]}), {"APPLIED"});
    DataVersion observer(db.path(), db.library);
    const std::int64_t future = 2000000000000LL;
    const auto maintenance = Outbox::MaintenanceClock::now() + std::chrono::hours(1);
    db.box->cleanupIfDue(future, maintenance);
    require(observer.scalar("SELECT count(*) FROM mqtt_event_outbox WHERE event_id='old-sent' AND sent=1;") == 1,
        "legacy cleanup removed IPC acknowledged dedup identity");
    require(observer.scalar("SELECT count(*) FROM mqtt_event_outbox WHERE sent=0;") == 2 &&
        db.box->pendingCount("main") == 2, "cleanup removed old pending/claimed rows");
    require(observer.scalar("SELECT claim_until FROM mqtt_event_outbox WHERE event_id='old-claimed';") ==
        std::numeric_limits<std::int64_t>::max(), "cleanup changed claim protection");
    rejectsUnchanged(db, claimRequest(3));
    finish(db, finishRequest("AckBatch", 3, batch, {batch.items[1]}), {"APPLIED"});
    db.box->cleanupIfDue(future + 24 * 60 * 60 * 1000LL + 1, maintenance + std::chrono::hours(25));
    require(observer.scalar("SELECT count(*) FROM mqtt_event_outbox WHERE event_id='old-claimed' AND sent=1;") == 1 &&
        observer.scalar("SELECT count(*) FROM mqtt_event_outbox WHERE event_id='old-unclaimed';") == 1 &&
        db.box->pendingCount("main") == 1, "cleanup changed IPC delivery/dedup state");
}

void realLeaseClock() {
    std::ifstream bootFile("/proc/sys/kernel/random/boot_id");
    std::string kernelBoot;
    require(static_cast<bool>(std::getline(bootFile, kernelBoot)), "cannot read kernel boot UUID for comparison");
    require(kernelBoot.size() == 36, "kernel boot UUID length changed");
    for (std::size_t i = 0; i < kernelBoot.size(); ++i) {
        const auto ch = kernelBoot[i];
        const bool hyphen = i == 8 || i == 13 || i == 18 || i == 23;
        require(hyphen ? ch == '-' : ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')),
            "kernel boot UUID is not canonical");
    }
    std::int64_t previous = -1;
    for (int i = 0; i < 64; ++i) {
        timespec before{}, after{};
        require(clock_gettime(CLOCK_BOOTTIME, &before) == 0, "cannot sample CLOCK_BOOTTIME before helper");
        const auto sample = readEventStoreLeaseTime();
        require(clock_gettime(CLOCK_BOOTTIME, &after) == 0, "cannot sample CLOCK_BOOTTIME after helper");
        const auto lower = static_cast<std::int64_t>(before.tv_sec) * 1000 + before.tv_nsec / 1000000;
        const auto upper = static_cast<std::int64_t>(after.tv_sec) * 1000 + after.tv_nsec / 1000000;
        require(sample.bootId == kernelBoot && sample.milliseconds >= previous &&
            sample.milliseconds >= lower && sample.milliseconds <= upper,
            "lease clock changed boot ID, regressed, or used a different clock domain/unit");
        previous = sample.milliseconds;
    }
}

void deliveryResponseLimit() {
    Database db;
    std::string multibyte;
    for (int i = 0; i < 1800; ++i) multibyte += "\xe4\xb8\xad";
    db.seed({event("large-receipt", "change", "main", 1780000000000LL, multibyte),
        event("receipt-tail", "change", "main", 1780000000001LL)});
    db.registerSender();
    db.registerSender("peer-sender");
    for (const std::size_t invalid : {std::size_t(0), std::size_t(4095), std::size_t(256 * 1024 + 1)}) {
        rejectsInvalidArgument([&] { db.store->setDeliveryResponseLimit(invalid); });
    }
    db.store->setDeliveryResponseLimit(4096);
    const auto request = claimRequest();
    rejectsUnchanged(db, request);
    success(db.store->deliveryReceipt("main-sender"), "main-sender", 1, 0);
    DataVersion observer(db.path(), db.library);
    require(observer.scalar("SELECT count(*) FROM mqtt_event_outbox WHERE claim_token IS NOT NULL;") == 0 &&
        db.box->pendingCount("main") == 2, "oversized response left partial claim rows");
    db.store->setDeliveryResponseLimit(256 * 1024);
    const auto peer = claim(db, claimRequest(1, "peer-sender"), 2);
    require(peer.raw.size() > 4096, "response-limit fixture did not exceed the small byte limit");
    rejectsInvalidArgument([&] { db.store->setDeliveryResponseLimit(4096); });
    require(db.store->deliver(claimRequest(1, "peer-sender")) == peer.raw,
        "limit setter missed another sender's receipt or changed the limit after rejecting it");
    finish(db, finishRequest("ReleaseBatch", 2, peer, peer.items, "peer-sender"), {"APPLIED", "APPLIED"});
    const auto batch = claim(db, request, 2);
    require(batch.raw.size() > 4096, "large-limit claim unexpectedly returned a small receipt");
    rejectsInvalidArgument([&] { db.store->setDeliveryResponseLimit(4096); });
    require(db.store->deliver(request) == batch.raw, "failed limit reduction broke exact claim retry");
    db.reopen();
    rejectsInvalidArgument([&] { db.store->setDeliveryResponseLimit(4096); });
    require(db.store->deliveryReceipt("main-sender") == batch.raw && db.store->deliver(request) == batch.raw,
        "reopen did not protect the persisted large receipt/default response limit");
    db.store->setDeliveryResponseLimit(256 * 1024);
    require(db.store->deliver(request) == batch.raw, "retaining the large limit did not preserve retry");
    finish(db, finishRequest("AckBatch", 2, batch, batch.items), {"APPLIED", "APPLIED"});
    db.store->setDeliveryResponseLimit(4096);
    claim(db, claimRequest(3), 0);
}

void foreignClaimStartupGuard() {
    // Both deadlines are in the past in wall-clock time. One is greater than
    // the injected boot time and one smaller; neither authorizes migration.
    for (const std::int64_t until : {946684800000LL, 1LL}) {
        Database db;
        const auto original = event("legacy-claimed");
        db.seed({original});
        db.registerSender();
        const auto receipt = db.store->deliveryReceipt("main-sender");
        db.store.reset();
        db.box.reset();
        {
            DataVersion fixture(db.path(), db.library, true);
            const auto inject = "UPDATE mqtt_event_outbox SET claim_token='legacy-wallclock-token',claim_until=" +
                std::to_string(until) + " WHERE event_id='legacy-claimed' AND sent=0;";
            require(fixture.execute(fixture.db, inject.c_str(), nullptr, nullptr, nullptr) == 0 &&
                fixture.scalar("SELECT changes();") == 1, "legacy claim fixture injection failed");
            rejects([&] { db.reopen(); }, "foreign or inconsistent claim; explicit migration required");
            db.store.reset();
            db.box.reset();
            require(fixture.scalar("SELECT count(*) FROM mqtt_event_outbox WHERE event_id='legacy-claimed' "
                "AND sent=0 AND claim_token='legacy-wallclock-token';") == 1 &&
                fixture.scalar("SELECT claim_until FROM mqtt_event_outbox WHERE event_id='legacy-claimed';") == until,
                "rejected startup silently cleared or discarded the foreign claim");
            require(fixture.execute(fixture.db,
                "UPDATE mqtt_event_outbox SET claim_token=NULL,claim_until=NULL WHERE event_id='legacy-claimed' AND sent=0;",
                nullptr, nullptr, nullptr) == 0 && fixture.scalar("SELECT changes();") == 1,
                "explicit legacy claim repair failed");
        }
        db.reopen();
        require(db.store->deliveryReceipt("main-sender") == receipt && db.box->pendingCount("main") == 1,
            "foreign claim rejection/repair changed sender sequence or lost the event");
        const auto recovered = claim(db, claimRequest(), 1);
        const auto& message = *field(recovered.response, "messages").asArray().values[0];
        require(recovered.items[0].eventId == original.eventId &&
            stringField(message, "eventType") == original.eventType &&
            stringField(message, "topic") == original.topic && stringField(message, "payload") == original.payload &&
            integerField(message, "eventTs") == original.eventTs, "legacy claim repair changed the stored event");
        finish(db, finishRequest("AckBatch", 2, recovered, recovered.items), {"APPLIED"});
        require(db.box->pendingCount("main") == 0, "repaired foreign-claim event could not be ACKed");
    }
}

void commitFailure(const std::string& library, bool automaticRollback) {
    if (!testLibrary.empty()) {
        require(setenv("GATEWAY_SQLITE_FIXTURE_REAL_LIBRARY", testLibrary.c_str(), 1) == 0,
            "cannot configure fixture's real SQLite library");
    }
    void* handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    require(handle != nullptr, "fault fixture load failed");
    const auto fail = reinterpret_cast<void (*)(int, int)>(dlsym(handle, "outbox_fault_commit"));
    require(fail != nullptr, "fault fixture COMMIT symbol missing");
    {
        Database db(library);
        db.seed({event("atomic-a"), event("atomic-b"), event("atomic-c")});
        db.registerSender();
        db.registerSender("peer-sender");
        const auto before = db.store->deliveryReceipt("main-sender");
        fail(13, automaticRollback ? 1 : 0);
        rejects([&] { db.store->deliver(claimRequest()); }, "full");
        require(db.store->deliveryReceipt("main-sender") == before && db.box->pendingCount("main") == 3,
            "failed Claim COMMIT left partial receipt/events");
        db.reopen();
        require(db.store->deliveryReceipt("main-sender") == before, "failed Claim receipt survived reopen");
        // A competing sender must see every row, not just a rolled-back receipt.
        const auto peer = claim(db, claimRequest(1, "peer-sender"), 3);
        finish(db, finishRequest("ReleaseBatch", 2, peer, peer.items, "peer-sender"),
            {"APPLIED", "APPLIED", "APPLIED"});
        const auto owned = claim(db, claimRequest(), 3);
        const auto ack = finishRequest("AckBatch", 2, owned, owned.items);
        fail(13, automaticRollback ? 1 : 0);
        rejects([&] { db.store->deliver(ack); }, "full");
        require(db.store->deliveryReceipt("main-sender") == owned.raw && db.box->pendingCount("main") == 3,
            "failed ACK COMMIT left partial sent rows/receipt");
        db.reopen();
        require(db.store->deliveryReceipt("main-sender") == owned.raw && db.box->pendingCount("main") == 3,
            "failed ACK changes survived reopen");
        claim(db, claimRequest(3, "peer-sender"), 0);
        const auto receipt = finish(db, ack, {"APPLIED", "APPLIED", "APPLIED"});
        require(db.box->pendingCount("main") == 0, "same ACK could not recover after rollback");
        db.reopen();
        DataVersion observer(db.path(), library);
        const auto version = observer.get();
        require(db.store->deliver(ack) == receipt && observer.get() == version && db.box->pendingCount("main") == 0,
            "recovered ACK retry lost deduplication");
    }
    dlclose(handle);
}

void managementResubmission(Outbox::StorageProfile profile) {
    Database db(testLibrary, profile);
    db.store->registerProducer("p0", "management-session", 0);
    EventStoreAppend request;
    request.producerId = "p0";
    request.epoch = 1;
    request.sequence = 1;
    request.request = "management-resubmission";
    const auto original = event("stable-management-id", "ota_status");
    request.events = {original};
    auto ids = [](const EventStoreProducer& receipt) {
        auto value = json::JsonParser(receipt.receipt, 16, 8192).parse();
        std::vector<std::int64_t> result;
        for (const auto& id : field(value, "ids").asArray().values) result.push_back(std::stoll(id->asString()));
        return result;
    };
    const auto first = ids(db.store->append(request));
    require(first.size() == 1, "initial management ID missing");
    request.sequence = 2;
    require(ids(db.store->append(request)) == first && db.box->pendingCount("main") == 1,
        "pending management retry changed row identity/count");
    db.box->markSent(first[0], 1780000000123LL);
    db.reopen();
    db.store->registerProducer("p0", "replacement-session", 1);
    request.epoch = 2;
    request.sequence = 1;
    require(ids(db.store->append(request)) == first && db.box->pendingCount("main") == 0,
        "sent management retry after restart resurrected row");
    request.sequence = 2;
    for (int fieldIndex = 0; fieldIndex < 4; ++fieldIndex) {
        auto changed = original;
        if (fieldIndex == 0) changed.payload += " ";
        if (fieldIndex == 1) changed.topic += "/changed";
        if (fieldIndex == 2) --changed.eventTs;
        if (fieldIndex == 3) changed.eventType = "change";
        request.events = {event("must-rollback", "ota_status"), changed};
        rejects([&] { db.store->append(request); }, "EVENT_CONFLICT");
        require(db.box->pendingCount("main") == 0, "conflicting management batch partially committed");
    }
    request.events = {original, event("mixed-type", "change")};
    rejects([&] { db.store->append(request); }, "EVENT_CONFLICT");
    request.events = {original};
    EventStoreState state;
    state.state.stateKey = "management-state";
    state.state.eventType = "change";
    request.states = {state};
    rejects([&] { db.store->append(request); }, "EVENT_CONFLICT");
    require(db.store->states("p0", "", 8).empty(), "rejected reuse wrote state");
    request.states.clear();
    EventStoreLocalEvent local;
    local.kind = "change";
    local.event.eventId = "local-must-rollback";
    local.event.ts = original.eventTs;
    local.configGeneration = db.identity.configGeneration;
    request.localEvents = {local};
    rejects([&] { db.store->append(request); }, "EVENT_CONFLICT");
    require(db.store->readJournal(0, 8).empty(), "rejected reuse wrote journal");
    request.localEvents.clear();
    request.events = {event(original.eventId, "ota_status", "third"), original,
        event("new-management", "ota_status")};
    const auto mixed = ids(db.store->append(request));
    require(mixed.size() == 3 && mixed[1] == first[0] && mixed[0] != mixed[1] && mixed[2] != mixed[0],
        "mixed existing/new/target management IDs not in request order");
    require(db.box->pendingCount("main") == 1 && db.box->pendingCount("third") == 1,
        "mixed management retry reset sent state or lost new target");
    require(ids(db.store->append(request)) == mixed, "exact receipt retry changed management IDs");
}

bool run(const char* name, const std::function<void()>& action) {
    // Outbox's dynamically resolved SQLite API is process-global.
    const pid_t child = fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        try { action(); std::cout << "PASS " << name << std::endl; _exit(0); }
        catch (const std::exception& ex) { std::cerr << "FAIL " << name << ": " << ex.what() << std::endl; _exit(1); }
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    require(waited == child, "waitpid failed");
    if (!WIFEXITED(status)) std::cerr << "FAIL " << name << ": child terminated abnormally" << std::endl;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
}

int main(int argc, char** argv) {
    if (argc > 3) {
        std::cerr << "usage: event_store_delivery_test [FAULT_LIBRARY [SQLITE_LIBRARY]]\n";
        return 2;
    }
    if (argc == 3) testLibrary = argv[2];
    int failed = 0;
    try {
        failed += !run("management-resubmission-delete", [] { managementResubmission(Outbox::StorageProfile::DeleteFull); });
        failed += !run("management-resubmission-wal", [] { managementResubmission(Outbox::StorageProfile::WalFull); });
        failed += !run("causal-delivery-clock-rollback-delete", [] { causalDeliveryOrder(Outbox::StorageProfile::DeleteFull); });
        failed += !run("causal-delivery-clock-rollback-wal", [] { causalDeliveryOrder(Outbox::StorageProfile::WalFull); });
        failed += !run("legacy-wall-clock-order-preserved", legacyWallClockOrder);
        failed += !run("sender-registration-cas-sequence", registrationAndSequence);
        failed += !run("claim-ack-release-dedup-delete", [] { batchLifecycle(Outbox::StorageProfile::DeleteFull); });
        failed += !run("claim-ack-release-dedup-wal", [] { batchLifecycle(Outbox::StorageProfile::WalFull); });
        failed += !run("target-type-token-row-event-isolation", targetTypeAndIdentityIsolation);
        failed += !run("session-cas-epoch-fencing", sessionFence);
        failed += !run("same-boot-restart-delete", [] { sameBootRestart(Outbox::StorageProfile::DeleteFull); });
        failed += !run("same-boot-restart-wal", [] { sameBootRestart(Outbox::StorageProfile::WalFull); });
        failed += !run("absolute-deadline-expired-no-payload", [] { expiredClaim(false); });
        failed += !run("changed-boot-expired-no-payload", [] { expiredClaim(true); });
        failed += !run("expired-unfinished-batch-next-claim", expiredBatchAllowsNextClaim);
        failed += !run("mixed-already-acked-and-expired-results", partiallyExpiredResults);
        failed += !run("byte-count-limits-oversized-head", limitsAndOversizedHead);
        failed += !run("read-only-delivery-contract", readOnlyContract);
        failed += !run("claimed-row-prune-int64-max-protection", claimedRowPruneProtection);
        failed += !run("legacy-cleanup-preserves-ipc-delivery-identities", claimedRowCleanupProtection);
        failed += !run("real-boot-clock-consecutive-samples", realLeaseClock);
        failed += !run("delivery-response-byte-limit-rollback-reopen", deliveryResponseLimit);
        failed += !run("foreign-claim-startup-reject-explicit-repair", foreignClaimStartupGuard);
        if (argc >= 2 && *argv[1]) {
            failed += !run("commit-full-explicit-rollback", [&] { commitFailure(argv[1], false); });
            failed += !run("commit-full-auto-rollback", [&] { commitFailure(argv[1], true); });
        } else {
            std::cout << "SKIP commit-full: no FAULT_LIBRARY supplied" << std::endl;
        }
    } catch (const std::exception& ex) {
        std::cerr << "FAIL delivery-test-runner: " << ex.what() << std::endl;
        return 1;
    }
    return failed ? 1 : 0;
}
