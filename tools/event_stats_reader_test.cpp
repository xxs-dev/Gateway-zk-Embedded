#include "edge_gateway/event_stats_reader.hpp"
#include "edge_gateway/event_store_runtime.hpp"
#include "edge_gateway/json_value.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <locale>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace edge_gateway;
using Json = json::JsonValue;
using Profile = MqttEventOutbox::StorageProfile;
using Selection = EventStatsSelection;
using Status = EventStatsStatus;
using Clock = std::chrono::steady_clock;
constexpr int kIoTimeoutMs = 5000;
constexpr auto kWait = std::chrono::seconds(15);
constexpr std::size_t kMaxFrame = 256 * 1024;
const char* const kStats = "GetScopedStats";
std::string testLibrary;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Action> void rejects(Action action, const std::string& label) {
    try { action(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("unexpected success: " + label);
}

template <typename Predicate> void eventually(Predicate predicate, const std::string& label) {
    const auto deadline = Clock::now() + kWait;
    while (!predicate()) {
        require(Clock::now() < deadline, "condition not reached: " + label);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

Json text(const std::string& value) { return Json::makeString(value); }
Json object(std::initializer_list<std::pair<std::string, Json>> fields = {}) {
    Json::Object result;
    for (const auto& item : fields) result.values.push_back({item.first, std::make_shared<Json>(item.second)});
    return Json::makeObject(std::move(result));
}
Json array(const std::vector<Json>& values = {}) {
    Json::Array result;
    for (const auto& item : values) result.values.push_back(std::make_shared<Json>(item));
    return Json::makeArray(std::move(result));
}
Json strings(const std::vector<std::string>& values) {
    std::vector<Json> result;
    for (const auto& value : values) result.push_back(text(value));
    return array(result);
}
const Json& field(const Json& value, const std::string& key) {
    const auto* found = value.find(key);
    require(found != nullptr, "missing field: " + key);
    return *found;
}
std::string stringField(const Json& value, const std::string& key) { return field(value, key).asString(); }
Json parse(const std::string& value) { return json::JsonParser(value, 32, 16384).parse(); }

void writeJson(std::ostream& out, const Json& value) {
    if (value.isNull()) { out << "null"; return; }
    if (value.isBool()) { out << (value.asBool() ? "true" : "false"); return; }
    if (value.isNumber()) { out << std::setprecision(17) << value.asNumber(); return; }
    if (value.isString()) {
        out << '"';
        const char* hex = "0123456789abcdef";
        for (const unsigned char ch : value.asString()) {
            if (ch == '"' || ch == '\\') out << '\\' << static_cast<char>(ch);
            else if (ch < 32) out << "\\u00" << hex[ch >> 4] << hex[ch & 15];
            else out << static_cast<char>(ch);
        }
        out << '"';
        return;
    }
    bool first = true;
    out << (value.isArray() ? '[' : '{');
    if (value.isArray()) {
        for (const auto& item : value.asArray().values) {
            if (!first) out << ',';
            first = false;
            writeJson(out, *item);
        }
    } else {
        for (const auto& item : value.asObject().values) {
            if (!first) out << ',';
            first = false;
            writeJson(out, text(item.key));
            out << ':';
            writeJson(out, *item.value);
        }
    }
    out << (value.isArray() ? ']' : '}');
}
std::string encode(const Json& value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    writeJson(out, value);
    return out.str();
}
Json replaceField(const Json& value, const std::string& key, const Json& replacement) {
    Json::Object result;
    bool found = false;
    for (const auto& item : value.asObject().values) {
        result.values.push_back({item.key, std::make_shared<Json>(item.key == key ? replacement : *item.value)});
        found = found || item.key == key;
    }
    require(found, "injection field missing: " + key);
    return Json::makeObject(std::move(result));
}
Json addField(const Json& value, const std::string& key, const Json& added) {
    auto result = value.asObject();
    result.values.push_back({key, std::make_shared<Json>(added)});
    return Json::makeObject(std::move(result));
}
Json withoutField(const Json& value, const std::string& key) {
    auto result = value.asObject();
    const auto size = result.values.size();
    result.values.erase(std::remove_if(result.values.begin(), result.values.end(),
        [&](const json::JsonMember& item) { return item.key == key; }), result.values.end());
    require(result.values.size() + 1 == size, "removal field missing: " + key);
    return Json::makeObject(std::move(result));
}

class Fd {
public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() { if (fd_ >= 0) close(fd_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
    void reset(int fd) { if (fd_ >= 0) close(fd_); fd_ = fd; }
private:
    int fd_;
};

std::vector<std::string> children(const std::string& path) {
    auto* directory = opendir(path.c_str());
    require(directory != nullptr, "cannot inspect directory: " + path);
    std::vector<std::string> result;
    while (const auto* item = readdir(directory)) {
        if (std::strcmp(item->d_name, ".") && std::strcmp(item->d_name, "..")) result.push_back(item->d_name);
    }
    closedir(directory);
    return result;
}

struct Directory {
    std::string path;
    Directory() {
        char pattern[] = "/tmp/es-stats-XXXXXX";
        const auto* created = mkdtemp(pattern);
        require(created != nullptr, "mkdtemp failed");
        path = created;
    }
    static void removeChildren(const std::string& root) noexcept {
        // Only remove the private mkdtemp tree; never follow directory aliases.
        if (auto* directory = opendir(root.c_str())) {
            while (const auto* item = readdir(directory)) {
                if (!std::strcmp(item->d_name, ".") || !std::strcmp(item->d_name, "..")) continue;
                const auto child = root + "/" + item->d_name;
                struct stat info{};
                if (lstat(child.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
                    removeChildren(child);
                    rmdir(child.c_str());
                } else unlink(child.c_str());
            }
            closedir(directory);
        }
    }
    ~Directory() { removeChildren(path); rmdir(path.c_str()); }
    static void collect(const std::string& root, std::set<std::string>& result) {
        for (const auto& name : children(root)) {
            const auto child = root + "/" + name;
            struct stat info{};
            require(lstat(child.c_str(), &info) == 0, "cannot stat fixture entry");
            result.insert(child);
            if (S_ISDIR(info.st_mode)) collect(child, result);
        }
    }
    std::set<std::string> files() const {
        std::set<std::string> result;
        collect(path, result);
        return result;
    }
    EventStoreRuntimeOptions options(Profile profile = Profile::DeleteFull) const {
        EventStoreRuntimeOptions result;
        result.identity = {"stats-test-store", "stats-test-v1"};
        result.producers = {"p0"};
        result.senders = {{"s0", "main", {"alarm", "change"}}};
        result.databasePath = path + "/events.db";
        result.socketPath = path + "/runtime.sock";
        result.sqliteLibraryPath = testLibrary;
        result.profile = profile;
        result.ioTimeoutMs = kIoTimeoutMs;
        result.maxFrameBytes = kMaxFrame;
        return result;
    }
};

std::string fileBytes(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot read fixture bytes: " + path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
IpcEventStatsOptions ipcOptions(const EventStoreRuntimeOptions& runtime, const std::string& socket = {}) {
    IpcEventStatsOptions result;
    result.identity = runtime.identity;
    result.socketPath = socket.empty() ? runtime.socketPath : socket;
    result.profile = runtime.profile;
    result.timeoutMs = kIoTimeoutMs;
    result.maxFrameBytes = kMaxFrame;
    return result;
}
LegacyEventStatsOptions legacyOptions(const EventStoreRuntimeOptions& runtime) {
    return {runtime.databasePath, runtime.sqliteLibraryPath, runtime.profile};
}
std::string wire(const EventStoreRuntimeOptions& options, const std::string& op, const Json& args) {
    return encode(object({{"version", text("1")}, {"storeId", text(options.identity.storeId)},
        {"configGeneration", text(options.identity.configGeneration)}, {"op", text(op)}, {"args", args}}));
}
Json call(const EventStoreRuntimeOptions& options, const std::string& op, const Json& args = object()) {
    return parse(callEventStore(options.socketPath, wire(options, op, args), kIoTimeoutMs));
}
Json scopeJson(const EventStatsScope& scope) {
    return object({{"targetId", text(scope.targetId)},
        {"selection", text(scope.selection == Selection::All ? "all" : "only")},
        {"include", strings(scope.include)}, {"exclude", strings(scope.exclude)}});
}
void checkStats(const EventPendingStats& value, std::int64_t count, std::int64_t units, const std::string& label) {
    require(value.pendingCount == count && value.pendingTextUnits == units,
        label + ": got count=" + std::to_string(value.pendingCount) + " textUnits=" + std::to_string(value.pendingTextUnits));
}

class Listener {
public:
    explicit Listener(std::string path) : path_(std::move(path)) {
        socket_.reset(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
        require(socket_.get() >= 0, "fixture socket failed");
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        require(path_.size() < sizeof(address.sun_path), "fixture socket path too long");
        std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
        require(bind(socket_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
            listen(socket_.get(), 16) == 0, "fixture bind/listen failed");
    }
    ~Listener() { unlink(path_.c_str()); }
    int get() const { return socket_.get(); }
    bool hasConnection() const {
        pollfd item{get(), POLLIN, 0};
        int result;
        do { result = poll(&item, 1, 0); } while (result < 0 && errno == EINTR);
        require(result >= 0, "fixture listener poll failed");
        return result != 0;
    }
private:
    std::string path_;
    Fd socket_;
};

void readyFd(int fd, short events, Clock::time_point deadline) {
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        require(remaining > 0, "fixture I/O deadline exceeded");
        pollfd item{fd, events, 0};
        const int result = poll(&item, 1, static_cast<int>(remaining));
        if (result < 0 && errno == EINTR) continue;
        require(result > 0 && !(item.revents & POLLNVAL), "fixture I/O poll failed/timed out");
        return;
    }
}
std::string receiveBytes(int fd, std::size_t size, Clock::time_point deadline) {
    std::string result(size, '\0');
    std::size_t position = 0;
    while (position < size) {
        readyFd(fd, POLLIN, deadline);
        const auto count = recv(fd, &result[position], size - position, MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        require(count > 0, "fixture peer closed before complete request");
        position += static_cast<std::size_t>(count);
    }
    return result;
}
bool sendFrame(int fd, const std::string& body) {
    require(!body.empty() && body.size() <= kMaxFrame, "invalid fixture response size");
    std::string bytes(4, '\0');
    for (int i = 0; i < 4; ++i) bytes[i] = static_cast<char>(body.size() >> (24 - 8 * i));
    bytes += body;
    const auto deadline = Clock::now() + std::chrono::milliseconds(kIoTimeoutMs);
    std::size_t position = 0;
    while (position < bytes.size()) {
        readyFd(fd, POLLOUT, deadline);
        const auto count = send(fd, bytes.data() + position, bytes.size() - position, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count < 0 && (errno == EPIPE || errno == ECONNRESET)) return false;
        require(count > 0, "fixture send failed");
        position += static_cast<std::size_t>(count);
    }
    return true;
}

class Gate {
public:
    void enter() {
        std::unique_lock<std::mutex> lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        require(changed_.wait_for(lock, kWait * 2, [&] { return released_; }), "test did not release response gate");
    }
    void waitEntered() {
        std::unique_lock<std::mutex> lock(mutex_);
        require(changed_.wait_for(lock, kWait, [&] { return entered_; }), "response gate was not reached");
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false, released_ = false;
};

class ReplyProxy {
public:
    using Rewrite = std::function<std::string(const std::string&)>;
    struct Exchange { std::string operation, request; bool completed = false, delivered = false; };
    ReplyProxy(std::string path, std::string upstream) : listener_(std::move(path)), upstream_(std::move(upstream)) {
        int wake[2];
        require(pipe2(wake, O_CLOEXEC) == 0, "fixture wake pipe failed");
        wakeRead_.reset(wake[0]);
        wakeWrite_.reset(wake[1]);
        worker_ = std::thread([this] { serve(); });
    }
    ~ReplyProxy() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& gate : gates_) gate->release();
        }
        const char wake = 'x';
        ssize_t written;
        do { written = write(wakeWrite_.get(), &wake, 1); } while (written < 0 && errno == EINTR);
        if (worker_.joinable()) worker_.join();
    }
    void arm(const std::string& operation, Rewrite rewrite = {}, std::shared_ptr<Gate> gate = {}, bool persistent = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        checkedLocked();
        operation_ = operation;
        rewrite_ = std::move(rewrite);
        gate_ = std::move(gate);
        persistent_ = persistent;
        if (gate_) gates_.push_back(gate_);
    }
    void clear() { arm(""); }
    std::vector<Exchange> exchanges(const std::string& op = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        checkedLocked();
        std::vector<Exchange> result;
        for (const auto& exchange : exchanges_)
            if (op.empty() || exchange.operation == op) result.push_back(exchange);
        return result;
    }
    void waitCompleted(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        require(changed_.wait_for(lock, kWait, [&] {
            return failure_ || static_cast<std::size_t>(std::count_if(exchanges_.begin(), exchanges_.end(),
                [](const Exchange& exchange) { return exchange.completed; })) >= count;
        }), "proxy exchanges did not complete");
        checkedLocked();
    }
    void checked() { std::lock_guard<std::mutex> lock(mutex_); checkedLocked(); }
private:
    void checkedLocked() const { if (failure_) std::rethrow_exception(failure_); }
    void serve() noexcept {
        try {
            for (;;) {
                pollfd ready[] = {{wakeRead_.get(), POLLIN, 0}, {listener_.get(), POLLIN, 0}};
                int result;
                do { result = poll(ready, 2, -1); } while (result < 0 && errno == EINTR);
                require(result > 0, "proxy accept poll failed");
                if (ready[0].revents) return;
                require(ready[1].revents & POLLIN, "proxy listener error");
                Fd peer(accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC));
                require(peer.get() >= 0, "proxy accept failed");
                const auto deadline = Clock::now() + std::chrono::milliseconds(kIoTimeoutMs);
                const auto header = receiveBytes(peer.get(), 4, deadline);
                std::size_t size = 0;
                for (const unsigned char ch : header) size = (size << 8) | ch;
                require(size > 0 && size <= kMaxFrame, "invalid request frame at proxy");
                Exchange exchange;
                exchange.request = receiveBytes(peer.get(), size, deadline);
                exchange.operation = stringField(parse(exchange.request), "op");
                Rewrite rewrite;
                std::shared_ptr<Gate> gate;
                std::size_t index;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    index = exchanges_.size();
                    exchanges_.push_back(exchange);
                    if (operation_ == exchange.operation) {
                        rewrite = rewrite_;
                        gate = gate_;
                        if (!persistent_) operation_.clear();
                    }
                    changed_.notify_all();
                }
                // Forward first, so corruption tests always start from an actual runtime reply.
                auto response = callEventStore(upstream_, exchange.request, kIoTimeoutMs);
                if (rewrite) response = rewrite(response);
                if (gate) gate->enter();
                const bool delivered = sendFrame(peer.get(), response);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    exchanges_[index].completed = true;
                    exchanges_[index].delivered = delivered;
                    changed_.notify_all();
                }
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            failure_ = std::current_exception();
            changed_.notify_all();
        }
    }
    Listener listener_;
    std::string upstream_;
    Fd wakeRead_, wakeWrite_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Exchange> exchanges_;
    std::string operation_;
    Rewrite rewrite_;
    std::shared_ptr<Gate> gate_;
    std::vector<std::shared_ptr<Gate>> gates_;
    bool persistent_ = false;
    std::exception_ptr failure_;
};

struct Lab {
    Directory directory;
    EventStoreRuntimeOptions options;
    EventStoreRuntime runtime;
    ReplyProxy proxy;
    explicit Lab(Profile profile = Profile::DeleteFull)
        : options(directory.options(profile)), runtime(options),
          proxy(directory.path + "/proxy.sock", options.socketPath) {
        runtime.start();
        require(runtime.ready(), "runtime did not become ready");
    }
    IpcEventStatsOptions ipc() const { return ipcOptions(options, directory.path + "/proxy.sock"); }
};

Json event(const std::string& id, const std::string& target, const std::string& type,
    const std::string& topic, const std::string& payload) {
    return object({{"eventId", text(id)}, {"targetId", text(target)}, {"eventType", text(type)},
        {"topic", text(topic)}, {"payload", text(payload)}, {"eventTs", text("1780000000000")}});
}
void seed(Lab& lab) {
    const auto registration = call(lab.options, "RegisterProducer", object({{"producerId", text("p0")},
        {"sessionId", text("stats-seed")}, {"expectedEpoch", text("0")}}));
    require(field(registration, "ok").asBool(), "seed registration failed");
    const auto reply = call(lab.options, "AppendEventsAndStates", object({{"producerId", text("p0")},
        {"epoch", text("1")}, {"sequence", text("1")}, {"states", array()}, {"events", array({
            event("main-alarm", "main", "alarm", "a", "1"),
            event("main-change", "main", "change", "cc", "22"),
            event("main-status", "main", "status", "sss", "333"),
            event("main-unicode", "main", "alarm", u8"\u4e2d/\u6587", u8"\u544a\u8b66"),
            event("third-alarm", "third", "alarm", "a", "1"),
            event("third-change", "third", "change", "b", "22"),
            event("third-status", "third", "status", "c", "333")})}}));
    require(field(reply, "ok").asBool(), "IPC seed append failed: " + encode(reply));
}

struct ScopeCase { std::string name; EventStatsScope scope; std::int64_t count, units; };
std::vector<ScopeCase> scopeCases() {
    return {
        {"main-all", {"main", Selection::All, {}, {}}, 4, 17},
        {"third-all", {"third", Selection::All, {}, {}}, 3, 9},
        {"driver-business", {"main", Selection::Only, {"change", "alarm", "change"}, {}}, 3, 11},
        {"driver-management", {"main", Selection::All, {}, {"change", "alarm", "alarm"}}, 1, 6},
        {"forwarder-business", {"third", Selection::Only, {"alarm", "change"}, {}}, 2, 5},
        {"forwarder-no-topics", {"third", Selection::Only, {}, {}}, 0, 0},
        {"overlap", {"main", Selection::Only, {"change", "alarm"}, {"alarm", "status"}}, 1, 4},
        {"exclude-all-included", {"main", Selection::Only, {"alarm"}, {"alarm"}}, 0, 0},
        {"unknown-type", {"main", Selection::Only, {"missing"}, {}}, 0, 0},
        {"unknown-target", {"missing", Selection::All, {}, {}}, 0, 0},
        {"exclude-not-target", {"main", Selection::All, {}, {"third"}}, 4, 17},
        {"unicode-units", {"main", Selection::Only, {"alarm"}, {}}, 2, 7}
    };
}

EventStatsCacheOptions cacheOptions(const EventStatsReaderOptions& reader) {
    EventStatsCacheOptions result;
    result.reader = reader;
    result.queries = {{"main", "driver-total", {"main", Selection::All, {}, {}}}};
    result.pollIntervalMs = 100;
    result.staleAfterMs = 10000;
    return result;
}

void constructorOfflineAndBackendOptions() {
    static_assert(!std::is_constructible<EventStatsReaderOptions, LegacyEventStatsOptions, IpcEventStatsOptions>::value,
        "a reader must not carry both backend configurations");
    Directory directory;
    const auto runtime = directory.options();
    const auto socketPath = directory.path + "/passive.sock";
    Listener listener(socketPath);
    const auto before = directory.files();
    const auto threads = children("/proc/self/task").size();
    const auto descriptors = children("/proc/self/fd").size();
    auto legacy = legacyOptions(runtime);
    legacy.databasePath = directory.path + "/missing-parent/missing.db";
    legacy.sqliteLibraryPath = directory.path + "/missing-sqlite.so";
    const auto ipc = ipcOptions(runtime, socketPath);
    {
        auto legacyReader = makeEventStatsReader(legacy);
        auto ipcReader = makeEventStatsReader(ipc);
        auto secondIpcReader = makeEventStatsReader(ipc);
        auto missing = ipc;
        missing.socketPath = directory.path + "/missing-parent/runtime.sock";
        auto missingReader = makeEventStatsReader(missing);
        EventStatsCache legacyCache(cacheOptions(legacy));
        EventStatsCache ipcCache(cacheOptions(ipc));
        for (const auto* cache : {&legacyCache, &ipcCache}) {
            const auto entry = cache->snapshot("main");
            require(entry.status == Status::NeverSampled && !entry.hasValue && !entry.valid &&
                entry.sampledAtUnixMs == 0 && entry.ageMs == 0 && entry.error.empty(), "initial cache claimed sampled zero backlog");
        }
        require(!listener.hasConnection(), "reader/cache construction connected to IPC");
        require(children("/proc/self/task").size() == threads, "cache constructor started a worker");
        require(children("/proc/self/fd").size() == descriptors, "constructor retained an I/O descriptor");
        require(directory.files() == before, "constructor created database, directories, or actor locks");
    }
    require(!listener.hasConnection() && directory.files() == before, "offline destruction performed I/O");
    EventStatsReaderOptions selected = legacy;
    require(selected.legacy() && !selected.ipc(), "legacy tag is ambiguous");
    selected = ipc;
    require(!selected.legacy() && selected.ipc(), "IPC assignment retained a legacy backend");
    selected = legacy;
    require(selected.legacy() && !selected.ipc(), "legacy assignment retained an IPC backend");
    rejects([&] { (void)makeEventStatsReader(EventStatsReaderOptions{}); }, "unspecified backend");
}

void invalidReaderOptions() {
    Directory directory;
    const auto runtime = directory.options();
    const auto before = directory.files();
    const auto validIpc = ipcOptions(runtime);
    const std::vector<std::function<void(IpcEventStatsOptions&)>> ipcChanges = {
        [](IpcEventStatsOptions& o) { o.identity.storeId.clear(); },
        [](IpcEventStatsOptions& o) { o.identity.configGeneration.clear(); },
        [](IpcEventStatsOptions& o) { o.identity.storeId.assign(97, 'x'); },
        [](IpcEventStatsOptions& o) { o.identity.configGeneration.assign(97, 'x'); },
        [](IpcEventStatsOptions& o) { o.identity.storeId = std::string("s\0x", 3); },
        [](IpcEventStatsOptions& o) { o.identity.configGeneration = std::string("g\0x", 3); },
        [](IpcEventStatsOptions& o) { o.socketPath.clear(); },
        [](IpcEventStatsOptions& o) { o.socketPath = "relative.sock"; },
        [](IpcEventStatsOptions& o) { o.socketPath += '/'; },
        [](IpcEventStatsOptions& o) { o.socketPath.assign(108, '/'); },
        [](IpcEventStatsOptions& o) { o.socketPath += std::string("\0x", 2); },
        [](IpcEventStatsOptions& o) { o.timeoutMs = 0; },
        [](IpcEventStatsOptions& o) { o.timeoutMs = -1; },
        [](IpcEventStatsOptions& o) { o.timeoutMs = 30001; },
        [](IpcEventStatsOptions& o) { o.maxFrameBytes = 4095; },
        [](IpcEventStatsOptions& o) { o.maxFrameBytes = kMaxFrame + 1; },
        [](IpcEventStatsOptions& o) { o.profile = Profile::DeleteNormal; },
        [](IpcEventStatsOptions& o) { o.profile = Profile::WalNormal; },
        [](IpcEventStatsOptions& o) { o.profile = static_cast<Profile>(99); }
    };
    for (std::size_t i = 0; i < ipcChanges.size(); ++i) {
        auto bad = validIpc;
        ipcChanges[i](bad);
        rejects([&] { (void)makeEventStatsReader(bad); }, "IPC option " + std::to_string(i));
        rejects([&] { EventStatsCache cache(cacheOptions(bad)); }, "cache IPC option " + std::to_string(i));
    }
    const auto validLegacy = legacyOptions(runtime);
    const std::vector<std::function<void(LegacyEventStatsOptions&)>> legacyChanges = {
        [](LegacyEventStatsOptions& o) { o.databasePath.clear(); },
        [](LegacyEventStatsOptions& o) { o.databasePath = "relative.db"; },
        [](LegacyEventStatsOptions& o) { o.databasePath.assign(4097, '/'); },
        [](LegacyEventStatsOptions& o) { o.databasePath += std::string("\0x", 2); },
        [](LegacyEventStatsOptions& o) { o.sqliteLibraryPath.assign(4097, 'x'); },
        [](LegacyEventStatsOptions& o) { o.sqliteLibraryPath = std::string("a\0b", 3); },
        [](LegacyEventStatsOptions& o) { o.profile = static_cast<Profile>(99); }
    };
    for (std::size_t i = 0; i < legacyChanges.size(); ++i) {
        auto bad = validLegacy;
        legacyChanges[i](bad);
        rejects([&] { (void)makeEventStatsReader(bad); }, "legacy option " + std::to_string(i));
        rejects([&] { EventStatsCache cache(cacheOptions(bad)); }, "cache legacy option " + std::to_string(i));
    }
    for (int timeout : {1, 30000}) {
        auto boundary = validIpc;
        boundary.identity.storeId.assign(96, 's');
        boundary.identity.configGeneration.assign(96, 'g');
        boundary.timeoutMs = timeout;
        boundary.maxFrameBytes = timeout == 1 ? 4096 : kMaxFrame;
        require(makeEventStatsReader(boundary) != nullptr, "valid IPC limits rejected");
    }
    for (const auto profile : {Profile::DeleteNormal, Profile::DeleteFull, Profile::WalNormal, Profile::WalFull}) {
        auto boundary = validLegacy;
        boundary.profile = profile;
        require(makeEventStatsReader(boundary) != nullptr, "valid explicit legacy profile rejected");
    }
    require(directory.files() == before, "option validation performed filesystem writes");
}

void unavailableNeverFallsBack() {
    Directory directory;
    const auto runtime = directory.options();
    const EventStatsScope scope{"main", Selection::All, {}, {}};
    const auto before = directory.files();
    {
        auto first = makeEventStatsReader(ipcOptions(runtime));
        auto second = makeEventStatsReader(ipcOptions(runtime));
        for (int i = 0; i < 3; ++i) {
            rejects([&] { (void)first->read(scope); }, "offline IPC read");
            rejects([&] { (void)second->read(scope); }, "second offline IPC read");
        }
        EventStatsCache cache(cacheOptions(ipcOptions(runtime)));
        cache.start();
        eventually([&] { return cache.snapshot("main").status == Status::Error; }, "offline cache error");
        const auto failed = cache.snapshot("main");
        require(!failed.hasValue && !failed.valid && failed.sampledAtUnixMs == 0 && !failed.error.empty(),
            "failed first read became a valid zero sample");
        cache.stop();
        require(cache.snapshot("main").status == Status::Stopped && !cache.snapshot("main").valid,
            "offline cache did not stop invalid");
    }
    require(directory.files() == before, "unavailable IPC created SQLite, fallback directory, or actor locks");
    {
        Listener abandoned(runtime.socketPath);
    }
    // A stale socket inode and a regular file are distinct failure paths from ENOENT.
    Fd stale(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, runtime.socketPath.c_str(), runtime.socketPath.size() + 1);
    require(bind(stale.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "stale socket bind failed");
    stale.reset(-1);
    const auto withStale = directory.files();
    auto reader = makeEventStatsReader(ipcOptions(runtime));
    rejects([&] { (void)reader->read(scope); }, "stale IPC endpoint");
    require(directory.files() == withStale, "stale IPC endpoint caused fallback writes");
    require(unlink(runtime.socketPath.c_str()) == 0, "stale fixture unlink failed");
    Fd regular(open(runtime.socketPath.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600));
    require(regular.get() >= 0, "regular endpoint fixture failed");
    const auto withRegular = directory.files();
    rejects([&] { (void)reader->read(scope); }, "regular file IPC endpoint");
    require(directory.files() == withRegular && fileBytes(runtime.socketPath).empty(), "bad endpoint changed a file");
}

void scopeBoundsAndNormalization() {
    Directory directory;
    const auto socketPath = directory.path + "/passive.sock";
    Listener listener(socketPath);
    auto reader = makeEventStatsReader(ipcOptions(directory.options(), socketPath));
    const EventStatsScope base{"main", Selection::Only, {"change", "alarm", "alarm"}, {"status", "alarm", "status"}};
    const auto normalized = normalizeEventStatsScope(base);
    require(normalized.targetId == "main" && normalized.selection == Selection::Only &&
        normalized.include == std::vector<std::string>({"alarm", "change"}) &&
        normalized.exclude == std::vector<std::string>({"alarm", "status"}), "scope was not sorted/deduplicated");
    require(base.include.size() == 3, "normalization modified its input");
    require(sameEventStatsScope(normalized, normalizeEventStatsScope(normalized)), "normalization is not idempotent");
    auto different = normalized;
    different.targetId = "third";
    require(!sameEventStatsScope(normalized, different), "target is absent from scope equality");
    different = normalized;
    different.exclude.clear();
    require(!sameEventStatsScope(normalized, different), "exclude is absent from scope equality");
    const auto empty = normalizeEventStatsScope({"main", Selection::Only, {}, {}});
    require(empty.selection == Selection::Only && empty.include.empty(), "empty only widened into all");
    require(!sameEventStatsScope(empty, {"main", Selection::All, {}, {}}), "all and empty only compare equal");

    const std::vector<std::function<void(EventStatsScope&)>> changes = {
        [](EventStatsScope& s) { s.targetId.clear(); },
        [](EventStatsScope& s) { s.targetId.assign(97, 'x'); },
        [](EventStatsScope& s) { s.targetId = std::string("m\0x", 3); },
        [](EventStatsScope& s) { s.selection = static_cast<Selection>(99); },
        [](EventStatsScope& s) { s.selection = Selection::All; },
        [](EventStatsScope& s) { s.include = {""}; },
        [](EventStatsScope& s) { s.exclude = {""}; },
        [](EventStatsScope& s) { s.include = {std::string(97, 'x')}; },
        [](EventStatsScope& s) { s.exclude = {std::string(97, 'x')}; },
        [](EventStatsScope& s) { s.include = {std::string("a\0b", 3)}; },
        [](EventStatsScope& s) { s.exclude = {std::string("a\0b", 3)}; },
        [](EventStatsScope& s) { s.include.assign(33, "alarm"); },
        [](EventStatsScope& s) { s.exclude.assign(33, "alarm"); },
        [](EventStatsScope& s) { s.targetId.clear(); for (int i = 0; i < 33; ++i) s.targetId += u8"\u4e2d"; }
    };
    for (std::size_t i = 0; i < changes.size(); ++i) {
        auto invalid = base;
        changes[i](invalid);
        rejects([&] { (void)normalizeEventStatsScope(invalid); }, "scope normalization " + std::to_string(i));
        rejects([&] { (void)reader->read(invalid); }, "reader scope " + std::to_string(i));
    }
    EventStatsScope boundary{std::string(96, 't'), Selection::Only, {}, {}};
    for (int i = 0; i < 32; ++i) {
        boundary.include.push_back(std::to_string(i) + std::string(94, 'i'));
        boundary.exclude.push_back(std::to_string(i) + std::string(94, 'e'));
    }
    const auto accepted = normalizeEventStatsScope(boundary);
    require(accepted.include.size() == 32 && accepted.exclude.size() == 32, "valid scope bounds were rejected");
    require(!listener.hasConnection(), "invalid scope was sent to IPC");
}

void realRuntimeParity(Profile profile) {
    Lab lab(profile);
    seed(lab);
    require(field(call(lab.options, "RegisterSender", object({{"senderId", text("s0")},
        {"sessionId", text("stats-sender-owner")}, {"expectedEpoch", text("0")}})), "ok").asBool(),
        "sender observation fixture registration failed");
    const auto files = lab.directory.files();
    const auto bytes = fileBytes(lab.options.databasePath);
    const auto receipt = encode(call(lab.options, "GetReceipt", object({{"producerId", text("p0")}})));
    const auto senderReceipt = encode(call(lab.options, "GetDeliveryReceipt", object({{"senderId", text("s0")}})));
    require(stringField(parse(receipt), "epoch") == "1" && stringField(parse(receipt), "sequence") == "1" &&
        stringField(parse(senderReceipt), "epoch") == "1" && stringField(parse(senderReceipt), "sequence") == "0",
        "actor observation fixture has unexpected initial epoch/sequence");
    auto ipc = makeEventStatsReader(lab.ipc());
    auto legacy = makeEventStatsReader(legacyOptions(lab.options));
    const auto cases = scopeCases();
    for (const auto& item : cases) {
        const auto remote = ipc->read(item.scope);
        const auto local = legacy->read(item.scope);
        checkStats(remote, item.count, item.units, "IPC " + item.name);
        checkStats(local, item.count, item.units, "legacy " + item.name);
        require(remote.pendingCount == local.pendingCount && remote.pendingTextUnits == local.pendingTextUnits,
            "backend parity failed: " + item.name);
    }
    checkStats(ipc->read({"main", Selection::Only, {"alarm') OR 1=1--"}, {}}), 0, 0, "literal SQL-looking type");
    checkStats(legacy->read({"main", Selection::Only, {"alarm') OR 1=1--"}, {}}), 0, 0, "legacy literal type");
    const auto oldMain = call(lab.options, "GetStats", object({{"targetId", text("main")}}));
    const auto oldThird = call(lab.options, "GetStats", object({{"targetId", text("third")}}));
    require(field(oldMain, "ok").asBool() && stringField(oldMain, "pendingCount") == "4" &&
        stringField(oldThird, "pendingCount") == "3" && oldMain.asObject().values.size() == 2,
        "legacy GetStats response meaning changed");
    require(encode(call(lab.options, "GetReceipt", object({{"producerId", text("p0")}}))) == receipt,
        "statistics changed actor ownership or mutation receipt");
    require(encode(call(lab.options, "GetDeliveryReceipt", object({{"senderId", text("s0")}}))) == senderReceipt,
        "statistics changed sender epoch/sequence/session or delivery receipt");
    require(lab.directory.files() == files, "read-only reader created a file or actor lock");
    require(fileBytes(lab.options.databasePath) == bytes, "read-only queries changed database bytes");
    const auto exchanges = lab.proxy.exchanges();
    require(exchanges.size() == (cases.size() + 1) * 2, "reader made extra requests or omitted per-read Hello");
    for (std::size_t i = 0; i < exchanges.size(); i += 2) {
        require(exchanges[i].operation == "Hello" && exchanges[i + 1].operation == kStats,
            "read-only IPC registered an actor or skipped Hello");
        if (i / 2 < cases.size()) {
            const auto args = field(parse(exchanges[i + 1].request), "args");
            require(encode(args) == encode(scopeJson(normalizeEventStatsScope(cases[i / 2].scope))),
                "reader did not send a canonical scoped request");
        }
    }
    lab.proxy.checked();
}

void legacyReadOnlyMissingAndEmpty() {
    Directory directory;
    const auto options = directory.options();
    const EventStatsScope scope{"main", Selection::All, {}, {}};
    auto reader = makeEventStatsReader(legacyOptions(options));
    rejects([&] { (void)reader->read(scope); }, "missing legacy database");
    require(directory.files().empty(), "legacy read created a missing database");
    Fd empty(open(options.databasePath.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600));
    require(empty.get() >= 0, "empty SQLite fixture failed");
    empty.reset(-1);
    const auto files = directory.files();
    rejects([&] { (void)reader->read(scope); }, "empty legacy schema");
    require(fileBytes(options.databasePath).empty() && directory.files() == files, "legacy reader created schema or journals");
}

void readerThreadAndForkOwnership() {
    Lab lab;
    seed(lab);
    const EventStatsScope scope{"main", Selection::All, {}, {}};
    const std::vector<EventStatsReaderOptions> configurations = {legacyOptions(lab.options), lab.ipc()};
    for (const auto& configuration : configurations) {
        auto reader = makeEventStatsReader(configuration);
        checkStats(reader->read(scope), 4, 17, "owner warm read");
        const auto requests = lab.proxy.exchanges().size();
        std::exception_ptr failure;
        std::thread other([&] {
            try {
                bool guarded = false;
                try { (void)reader->read(scope); }
                catch (const std::logic_error&) { guarded = true; }
                require(guarded, "reader cross-thread use did not fail with ownership error");
                auto ownedHere = makeEventStatsReader(configuration);
                checkStats(ownedHere->read(scope), 4, 17, "worker-owned reader");
            } catch (...) { failure = std::current_exception(); }
        });
        other.join();
        if (failure) std::rethrow_exception(failure);
        require(lab.proxy.exchanges().size() == requests + (configuration.ipc() ? 2 : 0),
            "cross-thread misuse performed IPC before checking ownership");
        const auto beforeFork = lab.proxy.exchanges().size();
        const auto child = fork();
        require(child >= 0, "ownership fork failed");
        if (child == 0) {
            alarm(10);
            try { (void)reader->read(scope); }
            catch (const std::logic_error&) { _exit(0); }
            catch (...) { _exit(2); }
            _exit(1);
        }
        int status = 0;
        pid_t waited;
        do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
        require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "reader cross-fork ownership guard failed");
        require(lab.proxy.exchanges().size() == beforeFork, "fork misuse reached IPC");
        checkStats(reader->read(scope), 4, 17, "parent reader after misuse");
    }
    lab.proxy.checked();
}

using Mutation = std::pair<std::string, ReplyProxy::Rewrite>;
Mutation change(const std::string& key, const Json& value) {
    return {key + "=" + encode(value), [key, value](const std::string& response) {
        return encode(replaceField(parse(response), key, value));
    }};
}
std::vector<Mutation> identityMutations() {
    return {
        change("version", text("2")), change("schemaVersion", text("2")),
        change("backend", text("legacy")), change("laboratoryOnly", Json::makeBool(false)),
        change("laboratoryOnly", text("true")), change("storeId", text("foreign-store")),
        change("configGeneration", text("foreign-generation")), change("synchronous", text("1")),
        change("synchronous", text("02")), change("synchronous", Json::makeNumber(2)),
        change("storageProfile", text("delete-normal")), change("storageProfile", text("wal-full")),
        change("journalMode", text("wal")), change("version", Json::makeNumber(1)),
        {"missing-storeId", [](const std::string& r) { return encode(withoutField(parse(r), "storeId")); }},
        {"duplicate-storeId", [](const std::string& r) { const auto j = parse(r); return encode(addField(j, "storeId", field(j, "storeId"))); }},
        {"duplicate-ok", [](const std::string& r) { return encode(addField(parse(r), "ok", Json::makeBool(true))); }},
        {"unknown-field", [](const std::string& r) { return encode(addField(parse(r), "extra", text("x"))); }},
        {"broken-json", [](const std::string&) { return std::string("{broken-json"); }},
        {"non-object", [](const std::string&) { return std::string("[]"); }},
        {"error-envelope", [](const std::string&) {
            return std::string(R"({"ok":false,"code":"READ_LIMIT","outcome":"not_committed","message":"injected"})");
        }}
    };
}
void rejectedReplies(const std::string& operation, const std::vector<Mutation>& mutations,
    const EventStatsScope& scope = {"main", Selection::Only, {"change", "alarm"}, {"unused", "status"}},
    std::int64_t expectedCount = 3, std::int64_t expectedUnits = 11) {
    Lab lab;
    seed(lab);
    const auto files = lab.directory.files();
    auto reader = makeEventStatsReader(lab.ipc());
    for (const auto& mutation : mutations) {
        const auto requests = lab.proxy.exchanges().size();
        const auto stats = lab.proxy.exchanges(kStats).size();
        lab.proxy.arm(operation, mutation.second);
        rejects([&] { (void)reader->read(scope); }, operation + " " + mutation.first);
        lab.proxy.waitCompleted(requests + (operation == "Hello" ? 1 : 2));
        require(lab.proxy.exchanges().size() == requests + (operation == "Hello" ? 1 : 2),
            "bad response caused implicit retry: " + mutation.first);
        if (operation == "Hello") require(lab.proxy.exchanges(kStats).size() == stats, "invalid Hello reached stats");
        checkStats(reader->read(scope), expectedCount, expectedUnits, "recovery after " + mutation.first);
    }
    for (const auto& exchange : lab.proxy.exchanges())
        require(exchange.operation == "Hello" || exchange.operation == kStats, "read failure invoked an actor operation");
    require(lab.directory.files() == files, "bad response created database/actor fallback files");
    lab.proxy.checked();
}

void badStatisticsScope() {
    std::vector<Mutation> mutations;
    const std::vector<std::pair<std::string, Json>> changes = {
        {"targetId", text("third")}, {"targetId", text("")}, {"targetId", text(std::string(97, 'x'))},
        {"targetId", text(std::string("main\0x", 6))}, {"selection", text("all")}, {"selection", text("ALL")},
        {"include", strings({"alarm"})}, {"include", strings({"change", "alarm"})},
        {"include", strings({"alarm", "alarm", "change"})}, {"include", array({Json::makeNumber(1)})},
        {"include", text("alarm")}, {"include", strings(std::vector<std::string>(33, "alarm"))},
        {"include", strings({std::string(97, 'x')})}, {"include", strings({""})},
        {"include", strings({std::string("a\0b", 3)})}, {"exclude", strings({"status"})},
        {"exclude", strings({"unused", "status"})}, {"exclude", strings({"status", "status", "unused"})},
        {"exclude", array({Json::makeBool(false)})}, {"exclude", Json::makeNull()},
        {"exclude", strings(std::vector<std::string>(33, "status"))}
    };
    for (const auto& item : changes) mutations.push_back({"scope." + item.first + "=" + encode(item.second),
        [item](const std::string& response) {
            const auto root = parse(response);
            return encode(replaceField(root, "scope", replaceField(field(root, "scope"), item.first, item.second)));
        }});
    mutations.push_back({"scope-duplicate", [](const std::string& response) {
        const auto root = parse(response);
        const auto scope = field(root, "scope");
        return encode(replaceField(root, "scope", addField(scope, "targetId", field(scope, "targetId"))));
    }});
    mutations.push_back({"scope-unknown", [](const std::string& response) {
        const auto root = parse(response);
        return encode(replaceField(root, "scope", addField(field(root, "scope"), "extra", text("x"))));
    }});
    mutations.push_back({"scope-missing", [](const std::string& response) {
        const auto root = parse(response);
        return encode(replaceField(root, "scope", withoutField(field(root, "scope"), "exclude")));
    }});
    mutations.push_back(change("scope", Json::makeNull()));
    rejectedReplies(kStats, mutations);
}

void badStatisticsIntegersAndDuplicates() {
    std::vector<Mutation> mutations;
    const std::vector<Json> invalid = {text(""), text("-1"), text("-0"), text("+1"), text("01"),
        text(" 1"), text("1 "), text("1.0"), text("1e0"), text("1x"), text("9223372036854775808"),
        text(std::string("1\0", 2)), Json::makeNumber(1), Json::makeBool(true), Json::makeNull()};
    for (const auto* key : {"pendingCount", "pendingTextUnits"}) {
        for (const auto& value : invalid) mutations.push_back(change(key, value));
        mutations.push_back({std::string("duplicate-") + key, [key](const std::string& response) {
            const auto root = parse(response);
            return encode(addField(root, key, field(root, key)));
        }});
        mutations.push_back({std::string("missing-") + key, [key](const std::string& response) {
            return encode(withoutField(parse(response), key));
        }});
    }
    mutations.push_back({"unknown-top-level", [](const std::string& response) {
        return encode(addField(parse(response), "pendingBytes", text("11")));
    }});
    mutations.push_back(change("pendingCount", text("0")));
    rejectedReplies(kStats, mutations);
    rejectedReplies(kStats, {change("pendingCount", text("1"))}, {"third", Selection::Only, {}, {}}, 0, 0);
    Lab lab;
    seed(lab);
    auto reader = makeEventStatsReader(lab.ipc());
    lab.proxy.arm(kStats, [](const std::string& response) {
        return encode(replaceField(replaceField(parse(response), "pendingCount", text("9223372036854775807")),
            "pendingTextUnits", text("9223372036854775807")));
    });
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    checkStats(reader->read({"main", Selection::All, {}, {}}), maximum, maximum, "int64 maximum without double rounding");
}

void cacheConfigurationBounds() {
    Directory directory;
    const auto before = directory.files();
    const auto valid = cacheOptions(ipcOptions(directory.options()));
    const std::vector<std::function<void(EventStatsCacheOptions&)>> changes = {
        [](EventStatsCacheOptions& o) { o.queries.clear(); },
        [](EventStatsCacheOptions& o) { o.queries.assign(17, o.queries.front()); },
        [](EventStatsCacheOptions& o) { o.queries.push_back(o.queries.front()); },
        [](EventStatsCacheOptions& o) { o.queries.front().key.clear(); },
        [](EventStatsCacheOptions& o) { o.queries.front().key.assign(97, 'k'); },
        [](EventStatsCacheOptions& o) { o.queries.front().key = std::string("k\0x", 3); },
        [](EventStatsCacheOptions& o) { o.queries.front().context.assign(257, 'c'); },
        [](EventStatsCacheOptions& o) { o.queries.front().context = std::string("c\0x", 3); },
        [](EventStatsCacheOptions& o) { o.queries.front().scope.targetId.clear(); },
        [](EventStatsCacheOptions& o) { o.queries.front().scope.include = {"alarm"}; },
        [](EventStatsCacheOptions& o) { o.pollIntervalMs = -1; },
        [](EventStatsCacheOptions& o) { o.pollIntervalMs = 0; },
        [](EventStatsCacheOptions& o) { o.pollIntervalMs = 9; },
        [](EventStatsCacheOptions& o) { o.pollIntervalMs = 3600001; },
        [](EventStatsCacheOptions& o) { o.staleAfterMs = 0; },
        [](EventStatsCacheOptions& o) { o.staleAfterMs = -1; },
        [](EventStatsCacheOptions& o) { o.staleAfterMs = 3600001; },
        [](EventStatsCacheOptions& o) { o.reader = EventStatsReaderOptions{}; }
    };
    for (std::size_t i = 0; i < changes.size(); ++i) {
        auto invalid = valid;
        changes[i](invalid);
        rejects([&] { EventStatsCache cache(invalid); }, "cache config " + std::to_string(i));
    }
    for (const auto limit : {10, 3600000}) {
        auto boundary = valid;
        boundary.pollIntervalMs = limit;
        boundary.staleAfterMs = limit == 10 ? 1 : 3600000;
        boundary.queries.clear();
        for (int i = 0; i < 16; ++i) boundary.queries.push_back({
            std::to_string(i) + std::string(94, 'k'), std::string(256, 'c'), {"main", Selection::All, {}, {}}});
        EventStatsCache cache(boundary);
        for (const auto& query : boundary.queries)
            require(cache.snapshot(query.key).status == Status::NeverSampled, "valid sixteen-query boundary rejected");
        rejects([&] { (void)cache.snapshot("unregistered"); }, "unknown fixed cache key");
        cache.stop();
        rejects([&] { cache.start(); }, "start after stop-before-start");
    }
    require(directory.files() == before, "cache validation created files");
}

void cacheMultiScope(bool ipcBackend) {
    Lab lab;
    seed(lab);
    auto options = cacheOptions(ipcBackend ? EventStatsReaderOptions(lab.ipc()) : EventStatsReaderOptions(legacyOptions(lab.options)));
    options.pollIntervalMs = 3600000;
    options.staleAfterMs = 3600000;
    options.queries.clear();
    const auto cases = scopeCases();
    for (const auto& item : cases) options.queries.push_back({item.name, "context:" + item.name, item.scope});
    const auto original = options;
    EventStatsCache cache(options);
    options.queries.front().scope.targetId = "not-the-configured-target";
    options.queries.front().context = "changed-after-construction";
    for (const auto& item : cases) {
        const auto initial = cache.snapshot(item.name);
        require(initial.status == Status::NeverSampled && !initial.valid && !initial.hasValue,
            "multi-scope initial sample was valid");
    }
    cache.start();
    rejects([&] { cache.start(); }, "second cache start");
    eventually([&] {
        for (const auto& item : cases) if (!cache.snapshot(item.name).valid) return false;
        return true;
    }, "all registered scopes fresh");
    const auto requests = lab.proxy.exchanges().size();
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& item = cases[i];
        auto entry = cache.snapshot(item.name);
        require(entry.status == Status::Fresh && entry.valid && entry.hasValue && entry.sampledAtUnixMs > 0 &&
            entry.ageMs >= 0 && entry.error.empty(), "fresh metadata invalid: " + item.name);
        require(entry.query.key == item.name && entry.query.context == original.queries[i].context &&
            sameEventStatsScope(entry.query.scope, normalizeEventStatsScope(item.scope)), "cache query metadata changed");
        checkStats(entry.value, item.count, item.units, "cache " + item.name);
        require(entry.backend == (ipcBackend ? EventStatsBackend::Ipc : EventStatsBackend::Legacy), "wrong cache backend");
        require(entry.identity.storeId == (ipcBackend ? lab.options.identity.storeId : "") &&
            entry.identity.configGeneration == (ipcBackend ? lab.options.identity.configGeneration : ""), "wrong cache identity");
        entry.query.context = "caller-edited-copy";
        entry.value.pendingCount = 999;
        require(cache.snapshot(item.name).query.context == original.queries[i].context &&
            cache.snapshot(item.name).value.pendingCount == item.count, "snapshot exposed mutable cache storage");
    }
    require(lab.proxy.exchanges().size() == requests && requests == (ipcBackend ? cases.size() * 2 : 0),
        "cache snapshot performed IPC or poll unexpectedly queued work");
    cache.stop();
    cache.stop();
    for (const auto& item : cases) {
        const auto stopped = cache.snapshot(item.name);
        require(stopped.status == Status::Stopped && !stopped.valid && stopped.hasValue,
            "stop discarded diagnostic value or kept READY");
        checkStats(stopped.value, item.count, item.units, "stopped cache " + item.name);
    }
    require(lab.proxy.exchanges().size() == requests, "stop performed another polling pass");
    rejects([&] { cache.start(); }, "restart of stopped cache");
    lab.proxy.checked();
}

void cacheFailurePreservesAndRecovers() {
    Lab lab;
    seed(lab);
    EventStatsCache cache(cacheOptions(lab.ipc()));
    cache.start();
    eventually([&] { return cache.snapshot("main").valid; }, "initial fresh before fault");
    auto gate = std::make_shared<Gate>();
    lab.proxy.arm(kStats, [](const std::string& response) {
        return encode(replaceField(parse(response), "storeId", text("foreign-store")));
    }, gate, true);
    gate->waitEntered();
    const auto previous = cache.snapshot("main");
    require(previous.valid && previous.hasValue, "fault barrier did not retain the previous sample");
    gate->release();
    eventually([&] { return cache.snapshot("main").status == Status::Error; }, "failed refresh");
    const auto failed = cache.snapshot("main");
    require(!failed.valid && failed.hasValue && !failed.error.empty() && failed.error.size() <= 512 &&
        failed.sampledAtUnixMs == previous.sampledAtUnixMs && failed.ageMs >= previous.ageMs,
        "failure refreshed timestamp, lost value, or stayed valid");
    checkStats(failed.value, previous.value.pendingCount, previous.value.pendingTextUnits, "failed retained value");
    require(failed.query.context == previous.query.context && failed.identity.storeId == previous.identity.storeId &&
        failed.identity.configGeneration == previous.identity.configGeneration && failed.backend == previous.backend,
        "failure relabeled previous value with foreign metadata");
    lab.proxy.clear();
    eventually([&] { return cache.snapshot("main").valid; }, "recovery from bad statistics response");
    const auto recovered = cache.snapshot("main");
    require(recovered.status == Status::Fresh && recovered.error.empty() && recovered.sampledAtUnixMs >= previous.sampledAtUnixMs,
        "successful retry did not clear error/restore Fresh");
    checkStats(recovered.value, 4, 17, "cache recovered");
    cache.stop();
    require(!cache.snapshot("main").valid, "recovered cache stayed valid after stop");
    lab.proxy.checked();
}

void cacheStaleAndSnapshotNoIo() {
    Lab lab;
    seed(lab);
    auto options = cacheOptions(lab.ipc());
    options.staleAfterMs = 100;
    EventStatsCache cache(options);
    auto firstGate = std::make_shared<Gate>();
    lab.proxy.arm("Hello", {}, firstGate);
    cache.start();
    firstGate->waitEntered();
    require(cache.snapshot("main").status == Status::NeverSampled && !cache.snapshot("main").valid,
        "in-flight first read became fresh");
    firstGate->release();
    eventually([&] { return cache.snapshot("main").hasValue; }, "first sample before stale test");
    auto gate = std::make_shared<Gate>();
    lab.proxy.arm(kStats, {}, gate);
    gate->waitEntered();
    const auto previous = cache.snapshot("main");
    const auto requests = lab.proxy.exchanges().size();
    eventually([&] { return cache.snapshot("main").status == Status::Stale; }, "steady-clock stale expiry");
    const auto stale = cache.snapshot("main");
    require(!stale.valid && stale.hasValue && stale.ageMs > options.staleAfterMs &&
        stale.sampledAtUnixMs == previous.sampledAtUnixMs, "stale value/timestamp contract failed");
    checkStats(stale.value, 4, 17, "stale retained value");
    for (int i = 0; i < 100; ++i) require(!cache.snapshot("main").valid, "snapshot refreshed stale data");
    require(lab.proxy.exchanges().size() == requests, "snapshot or concurrent sampler sent IPC while one read was blocked");
    gate->release();
    cache.stop();
    require(cache.snapshot("main").status == Status::Stopped && !cache.snapshot("main").valid, "stale cache failed to stop");
    lab.proxy.checked();
}

void cacheStopInFlightNoNextQuery() {
    Lab lab;
    seed(lab);
    auto options = cacheOptions(lab.ipc());
    options.queries.push_back({"third", "forwarder", {"third", Selection::All, {}, {}}});
    auto gate = std::make_shared<Gate>();
    lab.proxy.arm(kStats, {}, gate);
    EventStatsCache cache(options);
    cache.start();
    gate->waitEntered();
    const auto requests = lab.proxy.exchanges().size();
    require(requests == 2 && lab.proxy.exchanges(kStats).size() == 1, "first read was not the only in-flight read");
    auto stopping = std::async(std::launch::async, [&] { cache.stop(); });
    eventually([&] { return cache.snapshot("main").status == Status::Stopped; }, "stop visible before join");
    require(!cache.snapshot("main").valid && cache.snapshot("third").status == Status::Stopped,
        "stop did not immediately invalidate every query");
    gate->release();
    require(stopping.wait_for(kWait) == std::future_status::ready, "stop failed to join completed read");
    stopping.get();
    require(lab.proxy.exchanges().size() == requests && lab.proxy.exchanges(kStats).size() == 1,
        "stop executed the next query after in-flight read");
    require(!cache.snapshot("main").hasValue && !cache.snapshot("third").hasValue,
        "post-stop reply published a successful cache sample");
    lab.proxy.checked();
}

void cacheStopWakesPollAndBoundedNetwork() {
    Lab lab;
    seed(lab);
    auto options = cacheOptions(lab.ipc());
    options.pollIntervalMs = 3600000;
    options.staleAfterMs = 3600000;
    {
        EventStatsCache cache(options);
        cache.start();
        eventually([&] { return cache.snapshot("main").valid; }, "sample before long poll wait");
        auto stopped = std::async(std::launch::async, [&] { cache.stop(); });
        require(stopped.wait_for(kWait) == std::future_status::ready, "stop did not wake one-hour poll wait");
        stopped.get();
        require(cache.snapshot("main").status == Status::Stopped && !cache.snapshot("main").valid,
            "poll-wait stop left fresh status");
    }
    auto ipc = lab.ipc();
    ipc.timeoutMs = 1000;
    options = cacheOptions(ipc);
    options.queries.push_back({"third", "must-not-read", {"third", Selection::All, {}, {}}});
    auto gate = std::make_shared<Gate>();
    lab.proxy.arm("Hello", {}, gate);
    EventStatsCache cache(options);
    const auto before = lab.proxy.exchanges().size();
    cache.start();
    gate->waitEntered();
    auto stopped = std::async(std::launch::async, [&] { cache.stop(); });
    eventually([&] { return cache.snapshot("main").status == Status::Stopped; }, "bounded read stop request");
    // Keep the server blocked: only the reader's own network deadline can finish stop.
    require(stopped.wait_for(kWait) == std::future_status::ready, "network read had no finite timeout");
    stopped.get();
    require(lab.proxy.exchanges().size() == before + 1 && !cache.snapshot("third").hasValue,
        "timed-out stop continued with stats or the next scope");
    gate->release();
    lab.proxy.waitCompleted(before + 1);
    lab.proxy.checked();
}

bool run(const char* name, const std::function<void()>& action) {
    // Separate processes isolate SQLite's dynamically loaded API and bound deadlock regressions.
    std::cout.flush();
    std::cerr.flush();
    const auto child = fork();
    require(child >= 0, "test runner fork failed");
    if (child == 0) {
        alarm(60);
        try { action(); std::cout << "PASS " << name << std::endl; _exit(0); }
        catch (const std::exception& error) { std::cerr << "FAIL " << name << ": " << error.what() << std::endl; _exit(1); }
        catch (...) { std::cerr << "FAIL " << name << ": nonstandard exception" << std::endl; _exit(1); }
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    require(waited == child, "test runner waitpid failed");
    if (!WIFEXITED(status)) std::cerr << "FAIL " << name << ": signal " << WTERMSIG(status) << std::endl;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

int main() {
    if (const auto* library = std::getenv("SQLITE_LIBRARY")) testLibrary = library;
    constexpr int groups = 19;
    int failed = 0;
    try {
        failed += !run("constructor-offline-explicit-backend-no-thread-no-lock", constructorOfflineAndBackendOptions);
        failed += !run("reader-invalid-backend-identity-path-profile-budget", invalidReaderOptions);
        failed += !run("ipc-unavailable-never-sqlite-or-actor-fallback", unavailableNeverFallsBack);
        failed += !run("scope-canonicalization-byte-limits-empty-only", scopeBoundsAndNormalization);
        failed += !run("real-ipc-legacy-delete-full-scope-unicode-readonly-actors", [] { realRuntimeParity(Profile::DeleteFull); });
        failed += !run("real-ipc-legacy-wal-full-scope-unicode-readonly-actors", [] { realRuntimeParity(Profile::WalFull); });
        failed += !run("legacy-missing-empty-schema-no-write", legacyReadOnlyMissingAndEmpty);
        failed += !run("factory-reader-thread-fork-owner-survival", readerThreadAndForkOwnership);
        failed += !run("bad-hello-identity-full-duplicate-unknown-no-stats", [] { rejectedReplies("Hello", identityMutations()); });
        failed += !run("bad-hello-new-capability-types-and-versions", [] {
            rejectedReplies("Hello", {
                change("localJournalVersion", text("2")),
                change("historyProjectionVersion", Json::makeNumber(1)),
                change("historyEnabled", text("true")),
                change("capacityAdmissionVersion", text("0")),
                change("minFreeBytes", text("-1")),
                change("maxStoreBytes", Json::makeNumber(1024))
            });
        });
        failed += !run("bad-stats-identity-full-duplicate-unknown", [] { rejectedReplies(kStats, identityMutations()); });
        failed += !run("bad-stats-scope-types-order-duplicate-unknown", badStatisticsScope);
        failed += !run("bad-stats-integers-duplicate-empty-int64", badStatisticsIntegersAndDuplicates);
        failed += !run("cache-fixed-query-config-limits", cacheConfigurationBounds);
        failed += !run("cache-ipc-initial-fresh-multi-scope-stop", [] { cacheMultiScope(true); });
        failed += !run("cache-legacy-initial-fresh-multi-scope-stop", [] { cacheMultiScope(false); });
        failed += !run("cache-failure-retains-invalid-value-recovery", cacheFailurePreservesAndRecovers);
        failed += !run("cache-stale-steady-age-snapshot-no-io-single-flight", cacheStaleAndSnapshotNoIo);
        failed += !run("cache-stop-in-flight-invalid-no-next-query", cacheStopInFlightNoNextQuery);
        failed += !run("cache-stop-wakes-poll-bounded-network-wait", cacheStopWakesPollAndBoundedNetwork);
    } catch (const std::exception& error) {
        std::cerr << "FAIL stats-test-runner: " << error.what() << std::endl;
        return 1;
    }
    std::cout << "SUMMARY groups=" << groups << " failed=" << failed << std::endl;
    return failed ? 1 : 0;
}
