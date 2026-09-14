#include "edge_gateway/event_store_runtime.hpp"
#include "edge_gateway/json_value.hpp"
#include "edge_gateway/sqlite_error.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <functional>
#include <future>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>

struct sqlite3;

namespace {
using namespace edge_gateway;
using Outbox = MqttEventOutbox;
using Json = json::JsonValue;
std::string testLibrary;

void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }

template <typename Action> void rejects(Action action, const std::string& fragment = {}) {
    try { action(); }
    catch (const std::exception& ex) {
        require(fragment.empty() || std::string(ex.what()).find(fragment) != std::string::npos,
            "unexpected error: " + std::string(ex.what()));
        return;
    }
    throw std::runtime_error("expected rejection: " + fragment);
}

struct Directory {
    std::string path;
    Directory() {
        char name[] = "/tmp/event_store_test_XXXXXX";
        auto* created = mkdtemp(name);
        require(created != nullptr, "mkdtemp failed");
        path = created;
    }
    ~Directory() {
        auto* dir = opendir(path.c_str());
        if (dir) {
            while (auto* item = readdir(dir)) {
                if (std::strcmp(item->d_name, ".") && std::strcmp(item->d_name, "..")) {
                    unlink((path + "/" + item->d_name).c_str());
                }
            }
            closedir(dir);
        }
        rmdir(path.c_str());
    }
    EventStoreRuntimeOptions options() const {
        EventStoreRuntimeOptions result;
        result.databasePath = path + "/events.db";
        result.socketPath = path + "/events.sock";
        result.identity = {"test-store", "test-v1"};
        result.sqliteLibraryPath = testLibrary;
        for (int i = 0; i < 8; ++i) result.producers.push_back("p" + std::to_string(i));
        return result;
    }
};

EventStoreAppend append(std::int64_t sequence, std::int64_t version, bool withEvent = true) {
    EventStoreAppend result;
    result.producerId = "p0";
    result.epoch = 1;
    result.sequence = sequence;
    result.request = "immutable-wire-" + std::to_string(sequence);
    if (withEvent) {
        Outbox::EventMessage event;
        event.eventId = "event-" + std::to_string(sequence);
        event.eventType = "change";
        event.topic = "test/events";
        event.payload = "{\"value\":1}";
        event.eventTs = 1780000000000LL;
        result.events.push_back(event);
    }
    EventStoreState state;
    state.state.stateKey = "state-p0";
    state.state.eventType = "change";
    state.state.value = 1;
    state.state.index = 1;
    state.state.lifecycle = "observed";
    state.version = version;
    result.states.push_back(state);
    return result;
}

std::string wire(const std::string& op, const std::string& args) {
    return "{\"version\":\"1\",\"storeId\":\"test-store\",\"configGeneration\":\"test-v1\",\"op\":\"" + op + "\",\"args\":" + args + "}";
}

Json call(const EventStoreRuntimeOptions& options, const std::string& op, const std::string& args) {
    return json::JsonParser(callEventStore(options.socketPath, wire(op, args)), 16, 4096).parse();
}

bool ok(const Json& value) { return value.find("ok") && value.find("ok")->asBool(); }

void registerProducer(const EventStoreRuntimeOptions& options, const std::string& id = "p0") {
    auto response = call(options, "RegisterProducer", "{\"producerId\":\"" + id + "\",\"sessionId\":\"session-" + id + "\",\"expectedEpoch\":\"0\"}");
    require(ok(response) && response.find("epoch")->asString() == "1", "producer registration failed");
}

std::string appendArgs(const std::string& id, int sequence) {
    return "{\"producerId\":\"" + id + "\",\"epoch\":\"1\",\"sequence\":\"" + std::to_string(sequence) +
        "\",\"events\":[{\"eventId\":\"" + id + "-" + std::to_string(sequence) +
        "\",\"targetId\":\"main\",\"eventType\":\"change\",\"topic\":\"test/events\",\"payload\":\"\\u4e2d\\u6587\",\"eventTs\":\"1780000000000\"}],"
        "\"states\":[{\"stateKey\":\"" + id + "\",\"eventType\":\"change\",\"index\":\"1\",\"alarmType\":\"\",\"active\":false,\"value\":1.25,\"quality\":\"0\",\"sourceTs\":\"1780000000000\",\"lifecycle\":\"test\",\"expectedVersion\":\"" +
        std::to_string(sequence - 1) + "\"}]}";
}

int rawConnect(const std::string& path) {
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(fd >= 0, "raw socket failed");
    sockaddr_un endpoint{};
    endpoint.sun_family = AF_UNIX;
    std::strcpy(endpoint.sun_path, path.c_str());
    if (connect(fd, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
        close(fd); throw std::runtime_error("raw connect failed");
    }
    return fd;
}

struct RawPeer {
    int fd;
    explicit RawPeer(const std::string& path) : fd(rawConnect(path)) {}
    ~RawPeer() { close(fd); }
    void sendBytes(const std::string& value) {
        std::size_t position = 0;
        while (position < value.size()) {
            const auto n = send(fd, value.data() + position, value.size() - position, MSG_NOSIGNAL);
            require(n > 0, "raw send failed");
            position += static_cast<std::size_t>(n);
        }
    }
    std::string readBytes(std::size_t size) {
        std::string result;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (result.size() < size) {
            pollfd p{fd, POLLIN, 0};
            require(std::chrono::steady_clock::now() < deadline && poll(&p, 1, 100) >= 0, "raw read deadline exceeded");
            if (!(p.revents & POLLIN)) continue;
            char buffer[8192];
            const auto n = recv(fd, buffer, std::min(sizeof(buffer), size - result.size()), 0);
            require(n > 0, "raw peer closed");
            result.append(buffer, static_cast<std::size_t>(n));
        }
        return result;
    }
    std::string readFrame() {
        const auto header = readBytes(4);
        std::size_t size = 0;
        for (unsigned char ch : header) size = (size << 8) | ch;
        require(size > 0 && size <= 256 * 1024, "raw invalid response frame");
        return readBytes(size);
    }
};

std::string framed(const std::string& value) {
    std::string result(4, '\0');
    for (int i = 0; i < 4; ++i) result[i] = static_cast<char>(value.size() >> (24 - 8 * i));
    return result + value;
}

void databaseContract() {
    Directory temp;
    const auto options = temp.options();
    {
        Outbox box(options.databasePath, testLibrary, 12, 24, 100, 0, Outbox::StorageProfile::DeleteFull);
        EventStoreDatabase store(box, options.identity, options.producers);
        const auto registered = store.registerProducer("p0", "first", 0);
        require(registered.epoch == 1 && store.registerProducer("p0", "first", 0).epoch == 1, "registration not idempotent");
        rejects([&] { store.registerProducer("p0", "first", 99); }, "REGISTER_CONFLICT");
        auto baseline = append(1, 0, false);
        const auto first = store.append(baseline);
        require(first.sequence == 1 && box.pendingCount() == 0, "baseline generated a fake event");
        require(store.append(baseline).receipt == first.receipt, "receipt changed on retry");
        auto conflict = baseline;
        conflict.states[0].state.value = 2;
        rejects([&] { store.append(conflict); }, "REQUEST_CONFLICT");
        require(store.states("p0", "", 64)[0].version == 1, "baseline version advanced twice");
        auto second = append(2, 1);
        require(store.append(second).sequence == 2 && box.pendingCount() == 1, "event/state commit failed");
        rejects([&] { store.append(append(4, 2)); }, "GAPPED_SEQUENCE");
        rejects([&] { store.append(append(3, 0)); }, "STATE_CONFLICT");
        auto other = append(1, 2);
        other.producerId = "p1";
        store.registerProducer("p1", "other", 0);
        rejects([&] { store.append(other); }, "STATE_CONFLICT");
        require(store.registerProducer("p0", "replacement", 1).epoch == 2, "new registration did not fence old one");
        rejects([&] { store.append(second); }, "STALE_EPOCH");
        rejects([&] { store.registerProducer("p0", "first", 0); }, "STALE_EPOCH");
        Outbox read(options.databasePath, testLibrary, 12, 24, 100, 0, Outbox::StorageProfile::WalFull, Outbox::AccessMode::ReadOnly);
        EventStoreDatabase reader(read, options.identity, options.producers, true);
        require(read.storageSettings().journalMode == "delete", "reader changed journal mode");
        require(reader.states("p0", "", 1).at(0).version == 2, "read-only state page failed");
        require(reader.states("p0", "state-p0", 1).empty(), "keyset cursor repeated a state");
        rejects([&] { reader.append(append(3, 2)); }, "read-only");
        rejects([&] { reader.registerProducer("p0", "bad", 2); }, "read-only");
    }
    Outbox reopened(options.databasePath, testLibrary, 12, 24, 100, 0, Outbox::StorageProfile::DeleteFull);
    EventStoreDatabase store(reopened, options.identity, options.producers);
    require(store.receipt("p0").epoch == 2 && store.states("p0", "", 1).at(0).version == 2,
        "epoch/state reset after store reopen");
}

void commitFailure(const std::string& library) {
    Directory temp;
    const auto options = temp.options();
    void* handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    require(handle != nullptr, "fixture load failed");
    const auto fail = reinterpret_cast<void (*)(int, int)>(dlsym(handle, "outbox_fault_commit"));
    require(fail != nullptr, "fixture symbol missing");
    {
        Outbox box(options.databasePath, library, 12, 24, 100, 0, Outbox::StorageProfile::DeleteFull);
        EventStoreDatabase store(box, options.identity, options.producers);
        store.registerProducer("p0", "session", 0);
        fail(13, 1);
        rejects([&] { store.append(append(1, 0)); }, "injected full");
        require(store.receipt("p0").sequence == 0 && box.pendingCount() == 0 && store.states("p0", "", 1).empty(),
            "failed COMMIT left partial receipt/events/states");
        const auto committed = store.append(append(1, 0));
        require(committed.sequence == 1 && box.pendingCount() == 1, "same request could not recover after rollback");
        fail(517, 0);
        require(store.append(append(2, 1)).sequence == 2 && box.pendingCount() == 2,
            "BUSY_SNAPSHOT did not retry the entire transaction");
    }
    Outbox reopened(options.databasePath, library, 12, 24, 100, 0, Outbox::StorageProfile::DeleteFull);
    EventStoreDatabase store(reopened, options.identity, options.producers);
    require(store.append(append(2, 1)).sequence == 2 && reopened.pendingCount() == 2, "reopen lost dedup receipt");
    dlclose(handle);
}

void ipcRoundTrip() {
    Directory temp;
    auto options = temp.options();
    EventStoreRuntime runtime(options);
    runtime.start();
    require(runtime.ready() && ok(call(options, "Hello", "{}")), "runtime not ready");
    registerProducer(options);
    const auto request = wire("AppendEventsAndStates", appendArgs("p0", 1));
    const auto first = callEventStore(options.socketPath, request);
    require(ok(json::JsonParser(first).parse()), "append failed");
    require(callEventStore(options.socketPath, request) == first, "retry did not return exact receipt");
    const auto gap = call(options, "AppendEventsAndStates", appendArgs("p0", 3));
    require(!ok(gap) && gap.find("outcome")->asString() == "not_committed", "known sequence rejection reported unknown");
    auto emptyTarget = appendArgs("p0", 2);
    const auto targetPosition = emptyTarget.find("\"targetId\":\"main\"");
    emptyTarget.replace(targetPosition, std::strlen("\"targetId\":\"main\""), "\"targetId\":\"\"");
    const auto invalidTarget = call(options, "AppendEventsAndStates", emptyTarget);
    require(!ok(invalidTarget) && invalidTarget.find("outcome")->asString() == "not_queued", "empty target entered the write queue");
    const auto states = call(options, "LoadStates", "{\"producerId\":\"p0\",\"afterKey\":\"\",\"limit\":\"1\"}");
    require(ok(states) && states.find("states")->asArray().values.size() == 1, "state page missing");
    require(call(options, "GetStats", "{\"targetId\":\"main\"}").find("pendingCount")->asString() == "1", "pending count mismatch");
    auto mismatch = options;
    mismatch.identity.storeId = "different";
    const auto bad = "{\"version\":\"1\",\"storeId\":\"different\",\"configGeneration\":\"test-v1\",\"op\":\"Hello\",\"args\":{}}";
    require(!ok(json::JsonParser(callEventStore(options.socketPath, bad)).parse()), "identity mismatch accepted");
    runtime.stop();
    runtime.start();
    require(callEventStore(options.socketPath, request) == first, "service restart lost committed response");
    registerProducer(options);
    require(call(options, "GetReceipt", "{\"producerId\":\"p0\"}").find("sequence")->asString() == "1", "registration retry erased receipt");
    runtime.stop();
    try { callEventStore(options.socketPath, wire("Hello", "{}")); }
    catch (const EventStoreTransportError& ex) { require(!ex.outcomeUnknown(), "unavailable connect claimed a submission"); return; }
    throw std::runtime_error("client silently fell back without service");
}

void concurrentProducers(Outbox::StorageProfile profile) {
    Directory temp;
    auto options = temp.options();
    options.profile = profile;
    EventStoreRuntime runtime(options);
    runtime.start();
    std::vector<std::future<void>> tasks;
    tasks.push_back(std::async(std::launch::async, [&] {
        for (int n = 0; n < 30; ++n) {
            const auto response = call(options, "LoadStates", "{\"producerId\":\"p0\",\"afterKey\":\"\",\"limit\":\"64\"}");
            require(ok(response), "concurrent state query failed: " +
                (response.find("message") ? response.find("message")->asString() : "missing message"));
        }
    }));
    for (int i = 0; i < 8; ++i) tasks.push_back(std::async(std::launch::async, [&, i] {
        const auto id = "p" + std::to_string(i);
        registerProducer(options, id);
        for (int n = 1; n <= 10; ++n) require(ok(call(options, "AppendEventsAndStates", appendArgs(id, n))), "concurrent append failed");
    }));
    for (auto& task : tasks) task.get();
    require(call(options, "GetStats", "{\"targetId\":\"main\"}").find("pendingCount")->asString() == "80", "lost concurrent events");
    for (int i = 0; i < 8; ++i) {
        const auto id = "p" + std::to_string(i);
        const auto receipt = call(options, "GetReceipt", "{\"producerId\":\"" + id + "\"}");
        require(receipt.find("sequence")->asString() == "10", "producer sequence missing");
    }
}

void malformedAndSlowPeers() {
    Directory temp;
    auto options = temp.options();
    options.ioTimeoutMs = 250;
    EventStoreRuntime runtime(options);
    runtime.start();
    {
        RawPeer oversized(options.socketPath);
        oversized.sendBytes(std::string(4, static_cast<char>(255)));
        require(!ok(json::JsonParser(oversized.readFrame()).parse()), "oversized frame accepted");
    }
    {
        RawPeer partial(options.socketPath);
        const auto hello = framed(wire("Hello", "{}"));
        for (char ch : hello) partial.sendBytes(std::string(1, ch));
        require(ok(json::JsonParser(partial.readFrame()).parse()), "partial frame assembly failed");
    }
    {
        RawPeer slow(options.socketPath);
        slow.sendBytes(std::string(1, '\0'));
        require(ok(call(options, "Hello", "{}")), "slow peer blocked unrelated request");
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        pollfd p{slow.fd, POLLIN, 0};
        require(poll(&p, 1, 500) > 0, "partial-frame deadline did not close peer");
        char byte;
        require(recv(slow.fd, &byte, 1, 0) == 0, "expired slow peer remained open");
    }
    for (const auto& raw : {
        std::string("{\"version\":\"1\",\"version\":\"1\"}"),
        wire("UnrestrictedSQL", "{}"), wire("Hello", "{\"unexpected\":1}"),
        std::string("{\"x\":") + std::string(30, '[') + "0" + std::string(30, ']') + "}"}) {
        require(!ok(json::JsonParser(callEventStore(options.socketPath, raw)).parse()), "invalid request was accepted");
    }
    for (const auto& raw : {"01", "1.", "1e", "-", "[\"\\ud800\"]"}) {
        rejects([&] { json::JsonParser(raw, 16, 4096).parse(); });
    }
    for (const auto& raw : {std::string("\"\xc0\xaf\""), std::string("\"\xed\xa0\x80\""),
                           std::string("\"\xf4\x90\x80\x80\""), std::string("\"\x80\""),
                           std::string("\"\xe4\xb8\""), std::string("\v1"), std::string("1e9999")}) {
        rejects([&] { json::JsonParser(raw, 16, 4096).parse(); });
    }
    require(json::JsonParser("1.25", 16, 4096).parse().asNumber() == 1.25, "fraction parsed incorrectly");
    require(json::JsonParser("\"\\u4e2d\\u6587\"", 16, 4096).parse().asString() == "\xe4\xb8\xad\xe6\x96\x87", "Unicode decoding failed");
    require(json::JsonParser("\"\xe4\xb8\xad\xf0\x9f\x98\x80\"", 16, 4096).parse().asString() ==
        "\xe4\xb8\xad\xf0\x9f\x98\x80", "valid raw UTF-8 rejected");
}

void boundedBackpressure() {
    Directory temp;
    auto options = temp.options();
    options.maxFrameBytes = 4096;
    options.memoryBudgetBytes = 34 * 4096 + 65536;
    options.ioTimeoutMs = 300;
    EventStoreRuntime runtime(options);
    runtime.start();
    RawPeer occupying(options.socketPath);
    occupying.sendBytes(std::string("\0\0\x10\0", 4));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto rejected = json::JsonParser(callEventStore(options.socketPath, wire("Hello", "{}"))).parse();
    require(!ok(rejected) && rejected.find("outcome")->asString() == "not_queued", "queue saturation was not explicit");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    require(ok(call(options, "Hello", "{}")), "buffer reservation leaked after peer deadline");
}

void ownershipAndLegacyGuard() {
    Directory temp;
    auto options = temp.options();
    EventStoreRuntime primary(options);
    primary.start();
    auto secondOptions = options;
    secondOptions.socketPath = temp.path + "/second.sock";
    EventStoreRuntime second(secondOptions);
    rejects([&] { second.start(); }, "already has a writer");
    auto socketOptions = options;
    socketOptions.databasePath = temp.path + "/other.db";
    EventStoreRuntime duplicateSocket(socketOptions);
    rejects([&] { duplicateSocket.start(); }, "endpoint already in use");
    require(ok(call(options, "Hello", "{}")), "second writer disrupted existing socket");
    primary.stop();
    require(link(options.databasePath.c_str(), (temp.path + "/hardlink.db").c_str()) == 0, "hardlink setup failed");
    rejects([&] { primary.start(); }, "hard-linked");
    unlink((temp.path + "/hardlink.db").c_str());
    {
        Outbox legacy(temp.path + "/legacy.db", testLibrary, 12, 24, 100, 0, Outbox::StorageProfile::WalNormal);
    }
    auto legacyOptions = options;
    legacyOptions.databasePath = temp.path + "/legacy.db";
    EventStoreRuntime legacyRuntime(legacyOptions);
    rejects([&] { legacyRuntime.start(); });
    Outbox read(legacyOptions.databasePath, testLibrary, 12, 24, 100, 0, Outbox::StorageProfile::DeleteFull, Outbox::AccessMode::ReadOnly);
    require(read.storageSettings().journalMode == "wal", "legacy guard mutated journal mode before rejection");
}

void lostReplyAndReopen() {
    Directory temp;
    auto options = temp.options();
    EventStoreRuntime runtime(options);
    runtime.start();
    registerProducer(options);
    const auto request = wire("AppendEventsAndStates", appendArgs("p0", 1));
    {
        RawPeer unobserved(options.socketPath);
        unobserved.sendBytes(framed(request));
        bool committed = false;
        for (int i = 0; i < 50 && !committed; ++i) {
            const auto receipt = call(options, "GetReceipt", "{\"producerId\":\"p0\"}");
            committed = receipt.find("sequence") && receipt.find("sequence")->asString() == "1";
            if (!committed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        require(committed, "lost-reply request never committed");
        // Deliberately discard the unread response after confirmed COMMIT.
    }
    runtime.stop();
    runtime.start();
    require(ok(json::JsonParser(callEventStore(options.socketPath, request)).parse()), "lost reply could not be reconciled after restart");
    require(call(options, "GetStats", "{\"targetId\":\"main\"}").find("pendingCount")->asString() == "1", "lost reply duplicated the event");
}

void replacedFiles() {
    Directory temp;
    auto options = temp.options();
    EventStoreRuntime runtime(options);
    runtime.start();
    registerProducer(options);
    const auto moved = options.databasePath + ".moved";
    require(rename(options.databasePath.c_str(), moved.c_str()) == 0, "rename database failed");
    const int replacement = open(options.databasePath.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    require(replacement >= 0, "replacement database creation failed");
    close(replacement);
    require(!ok(call(options, "AppendEventsAndStates", appendArgs("p0", 1))), "renamed database accepted a write");
    require(unlink(options.databasePath.c_str()) == 0 && rename(moved.c_str(), options.databasePath.c_str()) == 0,
        "restore database failed");
    require(call(options, "GetReceipt", "{\"producerId\":\"p0\"}").find("sequence")->asString() == "0",
        "rejected replacement write advanced receipt");
    require(unlink(options.socketPath.c_str()) == 0, "unlink endpoint setup failed");
    const int other = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(other >= 0, "replacement socket failed");
    sockaddr_un endpoint{};
    endpoint.sun_family = AF_UNIX;
    std::strcpy(endpoint.sun_path, options.socketPath.c_str());
    const auto bound = bind(other, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint));
    close(other);
    require(bound == 0, "replacement socket bind failed");
    runtime.stop();
    struct stat current{};
    require(lstat(options.socketPath.c_str(), &current) == 0 && S_ISSOCK(current.st_mode),
        "shutdown deleted another endpoint");
}

void clientTimeoutIsUnknown() {
    Directory temp;
    const auto options = temp.options();
    const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(listener >= 0, "timeout listener failed");
    sockaddr_un endpoint{};
    endpoint.sun_family = AF_UNIX;
    std::strcpy(endpoint.sun_path, options.socketPath.c_str());
    require(bind(listener, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) == 0 && listen(listener, 1) == 0,
        "timeout endpoint setup failed");
    auto server = std::async(std::launch::async, [&] {
        pollfd p{listener, POLLIN, 0};
        require(poll(&p, 1, 2000) > 0, "timeout client did not connect");
        const int peer = accept(listener, nullptr, nullptr);
        require(peer >= 0, "timeout accept failed");
        char buffer[8192];
        const auto count = recv(peer, buffer, sizeof(buffer), 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        close(peer);
        require(count > 0, "timeout client never submitted bytes");
    });
    bool unknown = false;
    try { callEventStore(options.socketPath, wire("Hello", "{}"), 200); }
    catch (const EventStoreTransportError& ex) { unknown = ex.outcomeUnknown(); }
    server.get();
    close(listener);
    require(unknown, "post-submission timeout was classified as definitely unsubmitted");
}

void transientReadLock() {
    Directory temp;
    auto options = temp.options();
    EventStoreRuntime runtime(options);
    runtime.start();
    registerProducer(options);
    void* library = dlopen(testLibrary.empty() ? "libsqlite3.so.0" : testLibrary.c_str(), RTLD_NOW | RTLD_LOCAL);
    require(library != nullptr, "read lock fixture library failed");
    const auto openDb = reinterpret_cast<int (*)(const char*, sqlite3**, int, const char*)>(dlsym(library, "sqlite3_open_v2"));
    const auto execute = reinterpret_cast<int (*)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**)>(dlsym(library, "sqlite3_exec"));
    const auto closeDb = reinterpret_cast<int (*)(sqlite3*)>(dlsym(library, "sqlite3_close_v2"));
    require(openDb && execute && closeDb, "read lock fixture symbols missing");
    sqlite3* db = nullptr;
    require(openDb(options.databasePath.c_str(), &db, 2, nullptr) == 0, "read lock fixture open failed");
    require(execute(db, "BEGIN EXCLUSIVE;", nullptr, nullptr, nullptr) == 0, "exclusive lock setup failed");
    std::promise<void> started;
    auto ready = started.get_future();
    auto query = std::async(std::launch::async, [&] {
        started.set_value();
        return call(options, "LoadStates", "{\"producerId\":\"p0\",\"afterKey\":\"\",\"limit\":\"64\"}");
    });
    ready.get();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const int released = execute(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    closeDb(db);
    dlclose(library);
    require(released == 0, "read lock release failed");
    const auto response = query.get();
    require(ok(response), "transient read lock: " + (response.find("message") ? response.find("message")->asString() : "unknown"));
}

void queuedRequestGetsOwnDeadline() {
    Directory temp;
    auto options = temp.options();
    options.ioTimeoutMs = 300;
    options.senders = {{"sender", "main", {"change"}}};
    EventStoreRuntime runtime(options);
    runtime.start();
    require(ok(call(options, "RegisterSender", "{\"senderId\":\"sender\",\"sessionId\":\"s1\",\"expectedEpoch\":\"0\"}")),
        "deadline fixture sender register failed");
    void* library = dlopen(testLibrary.empty() ? "libsqlite3.so.0" : testLibrary.c_str(), RTLD_NOW | RTLD_LOCAL);
    require(library != nullptr, "deadline fixture library failed");
    const auto openDb = reinterpret_cast<int (*)(const char*, sqlite3**, int, const char*)>(dlsym(library, "sqlite3_open_v2"));
    const auto execute = reinterpret_cast<int (*)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**)>(dlsym(library, "sqlite3_exec"));
    const auto closeDb = reinterpret_cast<int (*)(sqlite3*)>(dlsym(library, "sqlite3_close_v2"));
    require(openDb && execute && closeDb, "deadline fixture symbols missing");
    sqlite3* db = nullptr;
    require(openDb(options.databasePath.c_str(), &db, 2, nullptr) == 0, "deadline fixture open failed");
    try {
        require(execute(db, "BEGIN EXCLUSIVE;", nullptr, nullptr, nullptr) == 0, "deadline fixture lock failed");
        RawPeer peer(options.socketPath);
        const auto body = framed(wire("ClaimBatch", "{\"senderId\":\"sender\",\"epoch\":\"1\",\"sequence\":\"1\","
            "\"limit\":\"1\",\"maxBytes\":\"100\",\"leaseMs\":\"30000\"}"));
        peer.sendBytes(body.substr(0, body.size() - 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        peer.sendBytes(body.substr(body.size() - 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        require(execute(db, "ROLLBACK;", nullptr, nullptr, nullptr) == 0, "deadline fixture unlock failed");
        const auto raw = peer.readFrame();
        const auto response = json::JsonParser(raw, 16, 4096).parse();
        // The bounded SQL retry may finish with BUSY before the fixture unlocks.
        // Either result must still reach the peer on the new processing deadline.
        const bool committed = ok(response) && response.find("status")->asString() == "EMPTY";
        const bool busy = !ok(response) && response.find("code") &&
            response.find("code")->asString() == "SQLITE_ERROR" && response.find("message") &&
            response.find("message")->asString().find("locked") != std::string::npos;
        require(committed || busy, "queued job did not return its result: " + raw);
        const auto receipt = call(options, "GetDeliveryReceipt", "{\"senderId\":\"sender\"}");
        require(ok(receipt) && receipt.find("sequence")->asString() == (committed ? "1" : "0"),
            "queued response and durable receipt disagree");
    } catch (...) {
        execute(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        closeDb(db); dlclose(library); throw;
    }
    closeDb(db); dlclose(library);
}

void deliveryFrameBounds() {
    Directory temp;
    auto options = temp.options();
    options.senders = {{"sender", "main", {"change"}}};
    const auto request = [](int bytes) {
        return "{\"senderId\":\"sender\",\"epoch\":\"1\",\"sequence\":\"1\",\"limit\":\"1\",\"maxBytes\":\"" +
            std::to_string(bytes) + "\",\"leaseMs\":\"30000\"}";
    };
    options.maxFrameBytes = 65536;
    {
        EventStoreRuntime runtime(options);
        runtime.start();
        require(ok(call(options, "RegisterSender", "{\"senderId\":\"sender\",\"sessionId\":\"s1\",\"expectedEpoch\":\"0\"}")),
            "small-frame sender register failed");
        const auto result = call(options, "ClaimBatch", request(1));
        require(!ok(result) && result.find("outcome")->asString() == "not_queued", "small frame claim was queued");
        require(call(options, "GetDeliveryReceipt", "{\"senderId\":\"sender\"}").find("sequence")->asString() == "0",
            "rejected frame consumed sequence");
        runtime.stop();
    }
    options.maxFrameBytes = 65536 + 6 * 8000;
    {
        EventStoreRuntime runtime(options);
        runtime.start();
        const auto rejected = call(options, "ClaimBatch", request(8001));
        require(!ok(rejected) && rejected.find("outcome")->asString() == "not_queued", "frame byte budget was not enforced");
        registerProducer(options);
        const std::string args = "{\"producerId\":\"p0\",\"epoch\":\"1\",\"sequence\":\"1\",\"states\":[],\"events\":[{"
            "\"eventId\":\"escaped\",\"targetId\":\"main\",\"eventType\":\"change\",\"topic\":\"t\",\"payload\":\"" +
            std::string(7999, 'x') + "\",\"eventTs\":\"1780000000000\"}]}";
        require(ok(call(options, "AppendEventsAndStates", args)), "frame fixture append failed");
        const auto claimed = call(options, "ClaimBatch", request(8000));
        require(ok(claimed) && claimed.find("status")->asString() == "CLAIMED", "exact frame budget failed");
        runtime.stop();
    }
    options.maxFrameBytes = 4096;
    {
        EventStoreRuntime runtime(options);
        rejects([&] { runtime.start(); }, "persisted delivery receipt");
    }
    options.maxFrameBytes = 256 * 1024;
    {
        EventStoreRuntime runtime(options);
        runtime.start();
        const auto recovered = call(options, "GetDeliveryReceipt", "{\"senderId\":\"sender\"}");
        require(ok(recovered) && recovered.find("status")->asString() == "CLAIMED", "restored frame lost persisted claim");
    }
}

bool run(const char* name, const std::function<void()>& action) {
    const pid_t child = fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        try { action(); std::cout << "PASS " << name << std::endl; _exit(0); }
        catch (const std::exception& ex) { std::cerr << "FAIL " << name << ": " << ex.what() << std::endl; _exit(1); }
    }
    int status = 0;
    require(waitpid(child, &status, 0) == child, "waitpid failed");
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) { std::cerr << "usage: event_store_test FAULT_LIBRARY [SQLITE_LIBRARY]\n"; return 2; }
    if (argc == 3) testLibrary = argv[2];
    int failed = 0;
    failed += !run("atomic-receipt-state-epoch", databaseContract);
    failed += !run("commit-failure-atomicity", [&] { commitFailure(argv[1]); });
    failed += !run("ipc-roundtrip-restart-no-fallback", ipcRoundTrip);
    failed += !run("concurrent-producers-wal", [&] { concurrentProducers(Outbox::StorageProfile::WalFull); });
    failed += !run("concurrent-producers-delete", [&] { concurrentProducers(Outbox::StorageProfile::DeleteFull); });
    failed += !run("partial-oversize-slow-invalid-peers", malformedAndSlowPeers);
    failed += !run("bounded-backpressure-recovery", boundedBackpressure);
    failed += !run("physical-ownership-and-legacy-guard", ownershipAndLegacyGuard);
    failed += !run("lost-reply-reopen-dedup", lostReplyAndReopen);
    failed += !run("replaced-database-and-socket-guard", replacedFiles);
    failed += !run("post-submission-timeout-unknown", clientTimeoutIsUnknown);
    failed += !run("transient-delete-read-lock", transientReadLock);
    failed += !run("delivery-frame-ingress-restart-guard", deliveryFrameBounds);
    failed += !run("queued-request-receives-new-deadline", queuedRequestGetsOwnDeadline);
    return failed ? 1 : 0;
}
