#include "edge_gateway/event_history_projection.hpp"
#include "edge_gateway/event_store_runtime.hpp"
#include "edge_gateway/json_value.hpp"
#include "edge_gateway/sqlite_error.hpp"
#include "event_store_test_files.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <unistd.h>

using namespace edge_gateway;
using Outbox = MqttEventOutbox;
using Json = json::JsonValue;

namespace {
void require(bool ok, const std::string& text) { if (!ok) throw std::runtime_error(text); }
template<class Action> void rejected(Action action) {
    try { action(); }
    catch (const EventStoreConflict& ex) { require(ex.code() == "CAPACITY_REJECTED", ex.what()); return; }
    throw std::runtime_error("expected CAPACITY_REJECTED");
}
struct Temp {
    std::string path;
    bool mounted = false;
    explicit Temp(bool tmpfs = false) {
        char name[] = "/tmp/event_store_capacity_XXXXXX";
        const auto* value = mkdtemp(name); require(value, "mkdtemp"); path = value;
        if (tmpfs) {
            if (mount("tmpfs", path.c_str(), "tmpfs", MS_NOSUID | MS_NODEV, "size=16m") != 0) {
                rmdir(path.c_str());
                throw std::runtime_error("ENOSPC test needs a private mount namespace: unshare -Ur -m --propagation private TEST --enospc");
            }
            mounted = true;
        }
    }
    ~Temp() {
        if (mounted) umount2(path.c_str(), MNT_DETACH);
        removeEventStoreTestTree(path);
    }
};
std::vector<EventStoreSenderConfig> senders() { return {{"sender", "main", {"alarm"}}}; }
EventStoreAppend request(int sequence, std::size_t size = 8) {
    EventStoreAppend r;
    r.producerId = "p"; r.epoch = 1; r.sequence = sequence; r.request = "wire-" + std::to_string(sequence);
    const auto id = "e-" + std::to_string(sequence);
    r.events.push_back({"alarm", "events", std::string(size, 'x'), 1780000000000LL, id, "main"});
    EventStoreLocalEvent local;
    local.kind = "alarm"; local.configGeneration = "v1"; local.stateVersion = sequence;
    local.event.eventId = id; local.event.ts = 1780000000000LL; local.event.alarmType = "high";
    local.event.value = sequence; local.event.persistValue = std::to_string(sequence);
    r.localEvents.push_back(local);
    EventStoreState state;
    state.state.stateKey = "point"; state.state.eventType = "alarm"; state.state.value = sequence;
    state.version = sequence - 1; r.states.push_back(state);
    return r;
}
EventStoreDeliveryRequest delivery(const std::string& op, int sequence) {
    EventStoreDeliveryRequest r;
    r.operation = op; r.sequence = sequence; r.senderId = "sender"; r.epoch = 1;
    r.request = op + std::to_string(sequence); r.limit = 2; return r;
}
Json decode(const std::string& text) { return json::JsonParser(text).parse(); }
std::vector<EventStoreClaimItem> items(const Json& response) {
    std::vector<EventStoreClaimItem> result;
    for (const auto& row : response.find("messages")->asArray().values)
        result.push_back({std::stoll(row->find("id")->asString()), row->find("eventId")->asString()});
    return result;
}
void finish(EventStoreDatabase& store, const std::string& op, int sequence, const Json& claimed,
    const std::vector<EventStoreClaimItem>& selected) {
    auto r = delivery(op, sequence); r.claimToken = claimed.find("claimToken")->asString(); r.items = selected;
    const auto response = decode(store.deliver(r));
    for (const auto& row : response.find("results")->asArray().values)
        require(row->find("status")->asString() == "APPLIED", "delivery rejected by admission gate");
}
std::uint64_t footprint(const std::string& path) {
    std::uint64_t size = 0;
    for (const auto* suffix : {"", "-wal", "-shm", "-journal"}) {
        struct stat file{};
        if (stat((path + suffix).c_str(), &file) == 0) size += static_cast<std::uint64_t>(file.st_size);
    }
    return size;
}

void thresholdRecovery(Outbox::StorageProfile profile) {
    Temp temp;
    const auto path = temp.path + "/events.db", history = temp.path + "/history.db";
    Outbox box(path, "", 12, 24, 8, 0, profile);
    EventStoreDatabase store(box, {"capacity", "v1"}, {"p"}, false, senders());
    store.registerProducer("p", "session", 0); store.registerSender("sender", "session", 0);
    store.append(request(1)); const auto original = store.append(request(2));
    const auto claimed = decode(store.deliver(delivery("ClaimBatch", 1)));
    EventHistoryProjection projection(store, history);
    store.setCapacityLimits({std::numeric_limits<std::uint64_t>::max(), 0, history});
    const auto status = store.capacityStatus();
    require(status.blocked && status.reason.find("minFreeBytes") != std::string::npos, "free-space gate inactive");
    require(status.storeBytes == footprint(path) + footprint(history), "capacity omitted SQLite sidecars/history");
    rejected([&] { store.append(request(3)); });
    require(store.append(request(2)).receipt == original.receipt, "capacity blocked immutable receipt retry");
    require(store.receipt("p").sequence == 2 && store.readJournal(0, 64).size() == 2 && store.states("p", "", 64)[0].version == 2,
        "rejected append mutated journal/state/receipt or blocked reads");
    auto selected = items(claimed);
    finish(store, "AckBatch", 2, claimed, {selected[0]});
    finish(store, "ReleaseBatch", 3, claimed, {selected[1]});
    const auto again = decode(store.deliver(delivery("ClaimBatch", 4)));
    finish(store, "AckBatch", 5, again, items(again));
    require(box.pendingCount() == 0, "capacity prevented backlog recovery");
    projection.replay(store, 0, 64); store.confirmProjection(projection);
    require(store.projectionCursor().projectedThrough == 2, "capacity blocked projection recovery");
    store.setCapacityLimits({0, 1, history});
    rejected([&] { store.append(request(3)); });
    require(store.capacityStatus().reason.find("maxStoreBytes") != std::string::npos, "size gate inactive");
    store.setCapacityLimits({1, store.capacityStatus().storeBytes + 1024 * 1024, history});
    store.append(request(3));
    require(store.receipt("p").sequence == 3 && store.readJournal(0, 64).size() == 3, "same request did not recover after threshold change");
}

void actualEnospc(Outbox::StorageProfile profile) {
    Temp temp(true);
    const auto path = temp.path + "/events.db", filler = temp.path + "/filler";
    Outbox box(path, "", 12, 24, 8, 0, profile);
    EventStoreDatabase store(box, {"capacity", "v1"}, {"p"});
    store.registerProducer("p", "session", 0); const auto first = store.append(request(1));
    const int fd = open(filler.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    require(fd >= 0, "open isolated filler");
    std::string chunk(65536, 'x');
    int failure = 0;
    for (;;) {
        const auto count = write(fd, chunk.data(), chunk.size());
        if (count >= 0) continue;
        if (errno == EINTR) continue;
        failure = errno; break;
    }
    close(fd); require(failure == ENOSPC, "filler did not reach real ENOSPC");
    store.setCapacityLimits({512 * 1024, 0, ""});
    require(store.capacityStatus().availableBytes == 0, "tmpfs not full");
    rejected([&] { store.append(request(2, 200000)); });
    require(store.readJournal(0, 64).size() == 1 && store.receipt("p").receipt == first.receipt, "full-space gate lost committed data");
    // Bypass the soft gate only in this fixture to exercise real SQLite FULL propagation.
    store.setCapacityLimits({});
    bool full = false;
    try { store.append(request(2, 200000)); }
    catch (const SqliteError& error) { full = error.primaryCode() == 13; require(full, error.what()); }
    require(full, "real full filesystem did not produce SQLITE_FULL");
    require(store.receipt("p").sequence == 1 && store.readJournal(0, 64).size() == 1 && store.states("p", "", 64)[0].version == 1,
        "SQLITE_FULL left a partial append");
    require(unlink(filler.c_str()) == 0, "release filler");
    store.setCapacityLimits({512 * 1024, 0, ""});
    store.append(request(2, 200000));
    require(store.receipt("p").sequence == 2 && store.readJournal(0, 64).size() == 2, "same connection/request did not recover after ENOSPC");
}

void runtimeGate() {
    Temp temp; EventStoreRuntimeOptions o;
    o.identity = {"capacity", "v1"}; o.producers = {"p"}; o.databasePath = temp.path + "/events.db";
    o.socketPath = temp.path + "/events.sock"; o.maxStoreBytes = 1;
    const auto config = temp.path + "/config.json";
    std::ofstream(config) << "{\"laboratoryOnly\":true,\"storeId\":\"capacity\",\"configGeneration\":\"v1\","
        "\"databasePath\":\"" << o.databasePath << "\",\"socketPath\":\"" << o.socketPath <<
        "\",\"sqliteLibraryPath\":\"\",\"storageProfile\":\"delete-full\",\"producers\":[\"p\"],\"minFreeBytes\":1024,\"maxStoreBytes\":1}";
    o = loadEventStoreLabConfig(config);
    EventStoreRuntime runtime(o); runtime.start();
    const auto call = [&](const std::string& op, const std::string& args) {
        return decode(callEventStore(o.socketPath, "{\"version\":\"1\",\"storeId\":\"capacity\",\"configGeneration\":\"v1\",\"op\":\"" + op + "\",\"args\":" + args + "}"));
    };
    require(call("RegisterProducer", "{\"producerId\":\"p\",\"sessionId\":\"s\",\"expectedEpoch\":\"0\"}").find("ok")->asBool(), "registration admission-gated");
    const auto status = call("GetCapacityStatus", "{}");
    require(status.find("ok")->asBool() && status.find("blocked")->asBool(), "capacity status unavailable while blocked");
    const auto response = call("AppendEventsAndStates", "{\"producerId\":\"p\",\"epoch\":\"1\",\"sequence\":\"1\",\"events\":[{"
        "\"eventId\":\"x\",\"targetId\":\"main\",\"eventType\":\"alarm\",\"topic\":\"events\",\"payload\":\"x\",\"eventTs\":\"1\"}],\"states\":[]}");
    require(!response.find("ok")->asBool() && response.find("code")->asString() == "CAPACITY_REJECTED" &&
        response.find("outcome")->asString() == "not_committed", "incorrect capacity IPC outcome");
    require(call("GetReceipt", "{\"producerId\":\"p\"}").find("sequence")->asString() == "0", "rejected sequence committed");
    require(call("Hello", "{}").find("capacityAdmissionVersion")->asString() == "1", "missing admission capability");
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--enospc") {
            actualEnospc(Outbox::StorageProfile::DeleteFull); std::cout << "PASS real-ENOSPC-delete-recovery\n";
            actualEnospc(Outbox::StorageProfile::WalFull); std::cout << "PASS real-ENOSPC-wal-recovery\n";
        } else {
            require(argc == 1, "usage: event_store_capacity_test [--enospc]");
            thresholdRecovery(Outbox::StorageProfile::DeleteFull); std::cout << "PASS capacity-delete-threshold-recovery\n";
            thresholdRecovery(Outbox::StorageProfile::WalFull); std::cout << "PASS capacity-wal-threshold-recovery\n";
            runtimeGate(); std::cout << "PASS capacity-runtime-config-outcome\n";
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
