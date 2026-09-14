#include "edge_gateway/event_store_client.hpp"
#include "edge_gateway/event_store_runtime.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <locale>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
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
using Role = EventStoreClientRole;
using Profile = MqttEventOutbox::StorageProfile;
using Clock = std::chrono::steady_clock;
constexpr int kTimeoutMs = 5000;
constexpr std::size_t kMaxFrame = 256 * 1024;
const char* const kAppend = "AppendEventsAndStates";
std::string testLibrary;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Action> void rejects(Action action) {
    bool rejected = false;
    try { action(); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "operation unexpectedly succeeded");
}

template <typename Action> EventStoreClientError clientError(Action action) {
    try { action(); }
    catch (const EventStoreClientError& error) { return error; }
    throw std::runtime_error("expected EventStoreClientError");
}

Json object(std::initializer_list<std::pair<std::string, Json>> fields = {}) {
    Json::Object result;
    for (const auto& field : fields) result.values.push_back({field.first, std::make_shared<Json>(field.second)});
    return Json::makeObject(std::move(result));
}

Json array(const std::vector<Json>& values = {}) {
    Json::Array result;
    for (const auto& value : values) result.values.push_back(std::make_shared<Json>(value));
    return Json::makeArray(std::move(result));
}

Json text(const std::string& value) { return Json::makeString(value); }
Json decimal(std::int64_t value) { return text(std::to_string(value)); }

const Json& field(const Json& value, const char* key) {
    const auto* found = value.find(key);
    require(found != nullptr, std::string("missing response field: ") + key);
    return *found;
}

std::string stringField(const Json& value, const char* key) { return field(value, key).asString(); }
std::int64_t integerField(const Json& value, const char* key) {
    const auto valueText = stringField(value, key);
    std::size_t consumed = 0;
    const auto number = std::stoll(valueText, &consumed);
    require(consumed == valueText.size() && std::to_string(number) == valueText, "noncanonical decimal response");
    return number;
}

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

Json parse(const std::string& value) { return json::JsonParser(value, 32, 16384).parse(); }

Json replaceField(const Json& value, const std::string& key, const Json& replacement) {
    Json::Object result;
    bool found = false;
    for (const auto& entry : value.asObject().values) {
        result.values.push_back({entry.key, std::make_shared<Json>(entry.key == key ? replacement : *entry.value)});
        found = found || entry.key == key;
    }
    require(found, "fault injection field missing: " + key);
    return Json::makeObject(std::move(result));
}

void success(const Json& reply, Role role, std::int64_t epoch, std::int64_t sequence) {
    require(field(reply, "ok").asBool(), "runtime/client returned an unsuccessful response");
    require(stringField(reply, role == Role::Producer ? "producerId" : "senderId") ==
        (role == Role::Producer ? "p0" : "s0"), "wrong actor in response");
    require(!stringField(reply, "sessionId").empty(), "empty session in response");
    require(integerField(reply, "epoch") == epoch && integerField(reply, "sequence") == sequence,
        "wrong epoch/sequence in response");
}

class Fd {
public:
    explicit Fd(int value = -1) : value_(value) {}
    ~Fd() { if (value_ >= 0) close(value_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    void reset(int value) { if (value_ >= 0) close(value_); value_ = value; }
    int get() const { return value_; }
private:
    int value_;
};

struct Directory {
    std::string path;
    Directory() {
        char pattern[] = "/tmp/es-client-XXXXXX";
        const auto* created = mkdtemp(pattern);
        require(created != nullptr, "mkdtemp failed");
        path = created;
    }
    static void removeChildren(const std::string& root) {
        // Only the mkdtemp fixture tree is removed; lstat prevents following aliases.
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
    ~Directory() {
        removeChildren(path);
        rmdir(path.c_str());
    }
    static void collectFiles(const std::string& root, std::set<std::string>& result) {
        auto* directory = opendir(root.c_str());
        require(directory != nullptr, "cannot inspect fixture directory");
        std::vector<std::string> children;
        while (const auto* item = readdir(directory)) {
            if (std::strcmp(item->d_name, ".") && std::strcmp(item->d_name, "..")) children.push_back(root + "/" + item->d_name);
        }
        closedir(directory);
        for (const auto& child : children) {
            struct stat info{};
            require(lstat(child.c_str(), &info) == 0, "cannot stat fixture entry");
            if (S_ISDIR(info.st_mode)) collectFiles(child, result);
            else result.insert(child);
        }
    }
    std::set<std::string> files() const {
        std::set<std::string> result;
        collectFiles(path, result);
        return result;
    }
    EventStoreRuntimeOptions options(Profile profile = Profile::DeleteFull, std::size_t maxFrame = kMaxFrame) const {
        EventStoreRuntimeOptions result;
        result.identity = {"client-test-store", "client-test-v1"};
        result.producers = {"p0", "p1"};
        result.senders = {{"s0", "main", {"change", "alarm"}}, {"s1", "main", {"change", "alarm"}}};
        result.databasePath = path + "/events.db";
        result.socketPath = path + "/runtime.sock";
        result.sqliteLibraryPath = testLibrary;
        result.profile = profile;
        result.ioTimeoutMs = kTimeoutMs;
        result.maxFrameBytes = maxFrame;
        return result;
    }
};

EventStoreClientOptions clientOptions(const EventStoreRuntimeOptions& options,
    Role role = Role::Producer, const std::string& socket = {}) {
    EventStoreClientOptions result;
    result.identity = options.identity;
    result.socketPath = socket.empty() ? options.socketPath : socket;
    result.actorId = role == Role::Producer ? "p0" : "s0";
    result.role = role;
    result.timeoutMs = kTimeoutMs;
    result.maxFrameBytes = options.maxFrameBytes;
    return result;
}

std::string wire(const EventStoreRuntimeOptions& options, const std::string& op, const Json& args) {
    return encode(object({{"version", text("1")}, {"storeId", text(options.identity.storeId)},
        {"configGeneration", text(options.identity.configGeneration)}, {"op", text(op)}, {"args", args}}));
}

Json call(const EventStoreRuntimeOptions& options, const std::string& op, const Json& args = object()) {
    return parse(callEventStore(options.socketPath, wire(options, op, args), kTimeoutMs));
}

std::int64_t pendingCount(const EventStoreRuntimeOptions& options) {
    const auto result = call(options, "GetStats", object({{"targetId", text("main")}}));
    require(field(result, "ok").asBool(), "GetStats failed");
    return integerField(result, "pendingCount");
}

std::string framed(const std::string& body) {
    require(!body.empty() && body.size() <= kMaxFrame, "invalid proxy frame size");
    std::string result(4, '\0');
    for (int i = 0; i < 4; ++i) result[i] = static_cast<char>(body.size() >> (24 - 8 * i));
    return result + body;
}

void readyFd(int fd, short events, Clock::time_point deadline) {
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        require(remaining > 0, "proxy I/O deadline exceeded");
        pollfd item{fd, events, 0};
        const int result = poll(&item, 1, static_cast<int>(remaining));
        if (result < 0 && errno == EINTR) continue;
        require(result > 0 && !(item.revents & POLLNVAL), "proxy I/O poll failed/timed out");
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
        require(count > 0, "proxy peer closed before completing request");
        position += static_cast<std::size_t>(count);
    }
    return result;
}

void sendBytes(int fd, const std::string& bytes) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(kTimeoutMs);
    std::size_t position = 0;
    while (position < bytes.size()) {
        readyFd(fd, POLLOUT, deadline);
        const auto count = send(fd, bytes.data() + position, bytes.size() - position, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        require(count > 0, "proxy send failed");
        position += static_cast<std::size_t>(count);
    }
}

bool delayUntilReplyOrDisconnect(int fd, int delayMs) {
    const auto until = Clock::now() + std::chrono::milliseconds(delayMs);
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
        if (remaining <= 0) return false;
        pollfd item{fd, POLLIN, 0};
        const auto result = poll(&item, 1, static_cast<int>(remaining));
        if (result < 0 && errno == EINTR) continue;
        require(result >= 0 && !(item.revents & POLLNVAL), "proxy delay poll failed");
        if (!result) continue;
        char byte;
        const auto count = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        require(count == 0, "unexpected data/error while delaying completed runtime reply");
        return true;
    }
}

class ReplyProxy {
public:
    enum class Fault { None, Truncate, Reject, Rewrite, Delay };
    struct Exchange {
        std::string operation, request, runtimeReply, deliveredReply;
        bool clientDisconnected = false;
    };
    using Rewrite = std::function<std::string(const std::string&)>;

    ReplyProxy(std::string path, std::string upstream) : path_(std::move(path)), upstream_(std::move(upstream)) {
        listener_.reset(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
        require(listener_.get() >= 0, "proxy socket failed");
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        require(path_.size() < sizeof(address.sun_path), "proxy socket path too long");
        std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
        require(bind(listener_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
            listen(listener_.get(), 8) == 0, "proxy bind/listen failed");
        int wake[2];
        require(pipe2(wake, O_CLOEXEC) == 0, "proxy wake pipe failed");
        wakeRead_.reset(wake[0]);
        wakeWrite_.reset(wake[1]);
        worker_ = std::thread([this] { serve(); });
    }
    ~ReplyProxy() {
        const char wake = 'x';
        ssize_t written;
        do { written = write(wakeWrite_.get(), &wake, 1); } while (written < 0 && errno == EINTR);
        if (worker_.joinable()) worker_.join();
        unlink(path_.c_str());
    }
    void arm(const std::string& operation, Fault fault, Rewrite rewrite = {}, unsigned skip = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        rethrowFailure();
        require(fault_ == Fault::None, "unconsumed proxy injection");
        operation_ = operation;
        fault_ = fault;
        rewrite_ = std::move(rewrite);
        skip_ = skip;
        remaining_ = 1;
    }
    void delayResponses(const std::string& operation, int delayMs, unsigned count) {
        std::lock_guard<std::mutex> lock(mutex_);
        rethrowFailure();
        require(fault_ == Fault::None && delayMs > 0 && delayMs < kTimeoutMs && count > 0,
            "invalid/unconsumed proxy delay injection");
        operation_ = operation;
        fault_ = Fault::Delay;
        delayMs_ = delayMs;
        remaining_ = count;
        skip_ = 0;
    }
    void waitForExchanges(const std::string& operation, std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        require(completed_.wait_for(lock, std::chrono::milliseconds(kTimeoutMs), [&] {
            return failure_ || static_cast<std::size_t>(std::count_if(exchanges_.begin(), exchanges_.end(),
                [&](const Exchange& exchange) { return exchange.operation == operation; })) >= count;
        }), "proxy did not observe expected completed exchanges");
        rethrowFailure();
    }
    std::vector<Exchange> exchanges(const std::string& operation = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        rethrowFailure();
        std::vector<Exchange> result;
        for (const auto& exchange : exchanges_)
            if (operation.empty() || operation == exchange.operation) result.push_back(exchange);
        return result;
    }
    void checked() {
        std::lock_guard<std::mutex> lock(mutex_);
        rethrowFailure();
        require(fault_ == Fault::None, "requested proxy fault was not exercised");
    }
private:
    void rethrowFailure() const { if (failure_) std::rethrow_exception(failure_); }
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
                const auto deadline = Clock::now() + std::chrono::milliseconds(kTimeoutMs);
                const auto header = receiveBytes(peer.get(), 4, deadline);
                std::size_t size = 0;
                for (const unsigned char ch : header) size = (size << 8) | ch;
                require(size > 0 && size <= kMaxFrame, "invalid request frame at proxy");
                Exchange exchange;
                exchange.request = receiveBytes(peer.get(), size, deadline);
                exchange.operation = stringField(parse(exchange.request), "op");
                Fault fault = Fault::None;
                Rewrite rewrite;
                int delayMs = 0;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (exchange.operation == operation_ && fault_ != Fault::None) {
                        if (skip_) --skip_;
                        else {
                            fault = fault_;
                            if (--remaining_ == 0) fault_ = Fault::None;
                            rewrite = rewrite_;
                            delayMs = delayMs_;
                        }
                    }
                }
                if (fault == Fault::Reject) {
                    exchange.deliveredReply = R"({"ok":false,"code":"REJECTED","outcome":"not_queued","message":"injected queue rejection"})";
                } else {
                    // A full real runtime reply is the commit barrier. Only then may the proxy lose/corrupt it.
                    exchange.runtimeReply = callEventStore(upstream_, exchange.request, kTimeoutMs);
                    exchange.deliveredReply = fault == Fault::Rewrite ? rewrite(exchange.runtimeReply) : exchange.runtimeReply;
                }
                auto output = framed(exchange.deliveredReply);
                if (fault == Fault::Truncate) output.resize(5); // Complete header, incomplete JSON body, then EOF.
                if (fault == Fault::Delay) {
                    exchange.clientDisconnected = delayUntilReplyOrDisconnect(peer.get(), delayMs);
                    if (exchange.clientDisconnected) exchange.deliveredReply.clear();
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    exchanges_.push_back(exchange);
                }
                completed_.notify_all();
                if (!exchange.clientDisconnected) sendBytes(peer.get(), output);
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            failure_ = std::current_exception();
            completed_.notify_all();
        }
    }
    std::string path_, upstream_;
    Fd listener_, wakeRead_, wakeWrite_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable completed_;
    Fault fault_ = Fault::None;
    std::string operation_;
    unsigned skip_ = 0;
    unsigned remaining_ = 1;
    int delayMs_ = 0;
    Rewrite rewrite_;
    std::vector<Exchange> exchanges_;
    std::exception_ptr failure_;
};

struct Lab {
    Directory directory;
    EventStoreRuntimeOptions options;
    EventStoreRuntime runtime;
    ReplyProxy proxy;
    explicit Lab(Profile profile = Profile::DeleteFull, std::size_t maxFrame = kMaxFrame)
        : options(directory.options(profile, maxFrame)), runtime(options),
          proxy(directory.path + "/proxy.sock", options.socketPath) {
        runtime.start();
        require(runtime.ready(), "runtime.start did not complete readiness");
    }
    EventStoreClientOptions client(Role role = Role::Producer) const {
        return clientOptions(options, role, directory.path + "/proxy.sock");
    }
    void restart() {
        runtime.stop();
        require(!runtime.ready(), "runtime.stop did not clear readiness");
        runtime.start();
        require(runtime.ready(), "runtime restart not ready");
    }
};

Json event(const std::string& id) {
    return object({{"eventId", text(id)}, {"targetId", text("main")}, {"eventType", text("change")},
        {"topic", text("lab/client/events")}, {"payload", text("{\"value\":1.25,\"text\":\"\\n\\u4e2d\"}")},
        {"eventTs", text("1780000000000")}});
}

Json state(const std::string& key, std::int64_t expectedVersion = 0, int index = 1) {
    return object({{"stateKey", text(key)}, {"eventType", text("change")}, {"index", decimal(index)},
        {"alarmType", text("threshold")}, {"active", Json::makeBool(true)}, {"value", Json::makeNumber(1.25)},
        {"quality", text("0")}, {"sourceTs", text("1780000000000")}, {"lifecycle", text("observed")},
        {"expectedVersion", decimal(expectedVersion)}});
}

Json appendArgs(const std::string& id = "event-1", std::int64_t expectedVersion = 0) {
    return object({{"events", array({event(id)})}, {"states", array({state("state-1", expectedVersion)})}});
}

Json claimArgs() {
    return object({{"limit", text("8")}, {"maxBytes", text("32768")}, {"leaseMs", text("30000")}});
}

Json finishArgs(const Json& claimed) {
    std::vector<Json> items;
    for (const auto& message : field(claimed, "messages").asArray().values)
        items.push_back(object({{"id", field(*message, "id")}, {"eventId", field(*message, "eventId")}}));
    return object({{"claimToken", field(claimed, "claimToken")}, {"items", array(items)}});
}

void frozen(EventStoreClient& client, const std::string& request, std::int64_t sequence = 0) {
    require(client.hasPending() && client.pendingRequest() == request && client.sequence() == sequence,
        "unresolved request was replaced, cleared, or advanced");
}

void exactReplay(ReplyProxy& proxy, const std::string& op, const std::string& request, std::size_t count = 2) {
    const auto exchanges = proxy.exchanges(op);
    require(exchanges.size() == count, "unexpected IPC attempt count for " + op);
    for (const auto& exchange : exchanges)
        require(exchange.request == request, "retry changed exact request bytes for " + op);
    proxy.checked();
}

void constructorAndOwnership() {
    Directory directory;
    const auto runtimeOptions = directory.options();
    auto options = clientOptions(runtimeOptions);
    const auto before = directory.files();
    {
        EventStoreClient owner(options);
        require(access(runtimeOptions.databasePath.c_str(), F_OK) != 0, "client constructor opened a database");
        require(!owner.hasPending(), "fresh client has pending work");
        rejects([&] { EventStoreClient duplicate(options); });
        auto generation = options;
        generation.identity.configGeneration = "replacement-generation";
        rejects([&] { EventStoreClient duplicate(generation); });
        require(symlink(directory.path.c_str(), (directory.path + "/alias").c_str()) == 0, "directory alias failed");
        auto alias = options;
        alias.socketPath = directory.path + "/alias/runtime.sock";
        rejects([&] { EventStoreClient duplicate(alias); });
        alias.socketPath = directory.path + "/./runtime.sock";
        rejects([&] { EventStoreClient duplicate(alias); });
        alias.socketPath = directory.path + "/other-name.sock";
        rejects([&] { EventStoreClient duplicate(alias); });
        auto other = options;
        other.actorId = "p1";
        EventStoreClient independent(other);
        auto sender = options;
        sender.role = Role::Sender;
        EventStoreClient otherRole(sender);
        rejects([&] { owner.execute(kAppend, appendArgs()); });
        rejects([&] { owner.retry(); });
        rejects([&] { owner.start(); }); // Missing endpoint, no database fallback.
        require(access(runtimeOptions.databasePath.c_str(), F_OK) != 0, "offline start created a database");
    }
    require(directory.files().size() > before.size(), "actor lock files were removed on destruction");
    EventStoreClient released(options);
}

std::string discoverLock(const Directory& directory, const EventStoreClientOptions& options) {
    const auto before = directory.files();
    { EventStoreClient client(options); }
    auto after = directory.files();
    for (const auto& entry : before) after.erase(entry);
    require(after.size() == 1, "constructor must leave exactly one actor lock in its empty directory");
    struct stat info{};
    require(lstat(after.begin()->c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_nlink == 1,
        "actor lock is not a regular single-link file");
    return *after.begin();
}

void lockFileGuards() {
    for (int mode = 0; mode < 3; ++mode) {
        Directory directory;
        const auto options = clientOptions(directory.options());
        const auto lock = discoverLock(directory, options);
        const auto displaced = directory.path + "/displaced";
        if (mode == 0) {
            require(rename(lock.c_str(), displaced.c_str()) == 0 && symlink(displaced.c_str(), lock.c_str()) == 0,
                "symlink lock fixture failed");
            rejects([&] { EventStoreClient client(options); });
        } else if (mode == 1) {
            require(link(lock.c_str(), displaced.c_str()) == 0, "hardlink lock fixture failed");
            rejects([&] { EventStoreClient client(options); });
        } else {
            EventStoreClient owner(options);
            require(rename(lock.c_str(), displaced.c_str()) == 0, "lock rename failed");
            Fd replacement(open(lock.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
            require(replacement.get() >= 0, "lock inode replacement failed");
            rejects([&] { (void)owner.hasPending(); });
            rejects([&] { owner.start(); });
        }
    }
}

void threadAndForkGuards() {
    Directory directory;
    const auto options = clientOptions(directory.options());
    std::unique_ptr<EventStoreClient> owner(new EventStoreClient(options));
    const std::vector<std::function<void()>> calls = {
        [&] { owner->start(); }, [&] { owner->execute(kAppend, appendArgs()); }, [&] { owner->retry(); },
        [&] { owner->discardRejected(); }, [&] { (void)owner->hasPending(); }, [&] { (void)owner->pendingRequest(); },
        [&] { (void)owner->epoch(); }, [&] { (void)owner->sequence(); }, [&] { owner->loadStates(); },
        [&] { owner->receipt(); }
    };
    std::exception_ptr threadFailure;
    std::thread foreign([&] {
        try { for (const auto& action : calls) rejects(action); }
        catch (...) { threadFailure = std::current_exception(); }
    });
    foreign.join();
    if (threadFailure) std::rethrow_exception(threadFailure);
    // Fork only while no runtime/proxy/helper threads exist in this process.
    const auto child = fork();
    require(child >= 0, "fork owner fixture failed");
    if (child == 0) {
        alarm(10);
        try {
            for (const auto& action : calls) rejects(action);
            owner.reset(); // Closing the inherited object must not unlock the parent's flock.
            _exit(0);
        } catch (...) { _exit(1); }
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "inherited client remained usable after fork");
    rejects([&] { EventStoreClient duplicate(options); });
    require(!owner->hasPending(), "foreign thread/fork changed parent state");
    owner.reset();
    EventStoreClient successor(options);
}

void producerLifecycle(Profile profile) {
    Lab lab(profile);
    EventStoreClient client(lab.client());
    require(lab.proxy.exchanges().empty(), "client constructor performed IPC");
    const auto hello = call(lab.options, "Hello");
    require(field(hello, "ok").asBool() && stringField(hello, "backend") == "ipc" &&
        stringField(hello, "schemaVersion") == "1" && stringField(hello, "version") == "1" &&
        stringField(hello, "synchronous") == "2", "Hello missing P1c/FULL contract");
    const auto registration = client.start();
    success(registration, Role::Producer, 1, 0);
    success(client.start(), Role::Producer, 1, 0);
    require(lab.proxy.exchanges("RegisterProducer").size() == 1, "repeated start re-registered actor");
    require(client.loadStates().empty(), "new producer has unexpected baseline");
    const auto reply = client.execute(kAppend, appendArgs());
    success(reply, Role::Producer, 1, 1);
    require(!client.hasPending() && client.pendingRequest().empty() && client.sequence() == 1,
        "successful append did not release single-flight slot");
    require(encode(client.receipt()) == encode(reply), "receipt does not match append response");
    auto states = client.loadStates();
    require(states.size() == 1 && states[0].version == 1 && states[0].state.stateKey == "state-1" &&
        states[0].state.value == 1.25 && states[0].state.active && states[0].state.index == 1 &&
        states[0].state.quality == 0 && states[0].state.sourceTs == 1780000000000LL &&
        states[0].state.alarmType == "threshold" && states[0].state.lifecycle == "observed" &&
        states[0].state.eventType == "change", "client state snapshot lost/changed stored fields");
    require(pendingCount(lab.options) == 1, "append did not persist exactly one event");
    lab.restart();
    require(encode(client.receipt()) == encode(reply), "same-session receipt did not survive runtime restart");
    success(client.execute(kAppend, appendArgs("event-2", 1)), Role::Producer, 1, 2);
    states = client.loadStates();
    require(states.size() == 1 && states[0].version == 2 && pendingCount(lab.options) == 2,
        "restart lost state/sequence continuity");
    lab.proxy.checked();
}

void lostRegistration(Role role) {
    Lab lab;
    const auto operation = role == Role::Producer ? "RegisterProducer" : "RegisterSender";
    const auto receiptOp = role == Role::Producer ? "GetReceipt" : "GetDeliveryReceipt";
    const auto actorKey = role == Role::Producer ? "producerId" : "senderId";
    const auto actor = role == Role::Producer ? "p0" : "s0";
    std::string session;
    {
        EventStoreClient client(lab.client(role));
        lab.proxy.arm(operation, ReplyProxy::Fault::Truncate);
        require(clientError([&] { client.start(); }).outcomeUnknown(), "lost registration reply was marked definite");
        auto exchanges = lab.proxy.exchanges(operation);
        require(exchanges.size() == 1, "start implicitly retried lost registration");
        const auto committed = parse(exchanges[0].runtimeReply);
        success(committed, role, 1, 0);
        session = stringField(committed, "sessionId");
        const auto original = exchanges[0].request;
        require(integerField(field(parse(original), "args"), "expectedEpoch") == 0,
            "first registration did not use original watermark");
        lab.restart();
        const auto recovered = client.start();
        success(recovered, role, 1, 0);
        require(encode(recovered) == encode(committed), "registration retry changed session/epoch/receipt");
        exactReplay(lab.proxy, operation, original);
        success(call(lab.options, receiptOp, object({{actorKey, text(actor)}})), role, 1, 0);
    }
    EventStoreClient replacement(lab.client(role));
    const auto next = replacement.start();
    success(next, role, 2, 0);
    require(stringField(next, "sessionId") != session, "fresh client reused old random session");
    lab.proxy.checked();
}

void lostAppend(bool rejectAfterUnknown) {
    Lab lab;
    EventStoreClient client(lab.client());
    client.start();
    lab.proxy.arm(kAppend, ReplyProxy::Fault::Truncate);
    require(clientError([&] { client.execute(kAppend, appendArgs()); }).outcomeUnknown(),
        "truncated committed append was not classified unknown");
    const auto request = client.pendingRequest();
    frozen(client, request);
    const auto exchanges = lab.proxy.exchanges(kAppend);
    require(exchanges.size() == 1 && exchanges[0].request == request, "pending bytes do not match original wire request");
    success(parse(exchanges[0].runtimeReply), Role::Producer, 1, 1);
    require(pendingCount(lab.options) == 1, "fault did not occur after a committed append");
    const auto attempts = lab.proxy.exchanges().size();
    rejects([&] { client.execute(kAppend, appendArgs("replacement-event")); });
    rejects([&] { client.loadStates(); });
    rejects([&] { client.discardRejected(); });
    frozen(client, request);
    require(lab.proxy.exchanges().size() == attempts, "single-flight rejection unexpectedly performed IPC");
    if (rejectAfterUnknown) {
        lab.proxy.arm(kAppend, ReplyProxy::Fault::Reject);
        const auto error = clientError([&] { client.retry(); });
        require(error.code() == "REJECTED", "proxy definite rejection was not surfaced");
        frozen(client, request);
        rejects([&] { client.discardRejected(); });
        rejects([&] { client.execute(kAppend, appendArgs("new-after-reject")); });
        frozen(client, request);
        require(lab.proxy.exchanges(kAppend).back().runtimeReply.empty(), "injected rejection reached runtime");
    }
    lab.restart();
    const auto recovered = client.retry();
    success(recovered, Role::Producer, 1, 1);
    require(encode(recovered) == encode(parse(exchanges[0].runtimeReply)), "append exact replay changed receipt");
    exactReplay(lab.proxy, kAppend, request, rejectAfterUnknown ? 3 : 2);
    require(!client.hasPending() && client.sequence() == 1 && pendingCount(lab.options) == 1,
        "append retry duplicated event or failed to resolve pending slot");
    const auto states = client.loadStates();
    require(states.size() == 1 && states[0].version == 1, "append retry advanced state version twice");
}

void capacityRejectionAndRecovery(Profile profile) {
    Directory directory;
    auto options = directory.options(profile);
    // Real statvfs capacity gate, without filling the host filesystem.
    options.minFreeBytes = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    auto runtime = std::make_unique<EventStoreRuntime>(options);
    runtime->start();
    ReplyProxy proxy(directory.path + "/proxy.sock", options.socketPath);
    EventStoreClient client(clientOptions(options, Role::Producer, directory.path + "/proxy.sock"));
    client.start();
    const auto error = clientError([&] { client.execute(kAppend, appendArgs("capacity-retry")); });
    require(error.code() == "CAPACITY_REJECTED" && !error.outcomeUnknown(),
        "capacity rejection was not classified as known not_committed");
    const auto original = client.pendingRequest();
    frozen(client, original);
    const auto rejected = proxy.exchanges(kAppend);
    require(rejected.size() == 1 && rejected[0].runtimeReply.find("minFreeBytes reached") != std::string::npos,
        "fixture did not hit real filesystem capacity threshold");
    require(stringField(parse(rejected[0].runtimeReply), "outcome") == "not_committed" &&
        pendingCount(options) == 0, "capacity rejection committed an event");
    rejects([&] { client.execute(kAppend, appendArgs("replacement-after-capacity")); });
    frozen(client, original);
    runtime->stop();
    runtime.reset();
    options.minFreeBytes = 0;
    runtime = std::make_unique<EventStoreRuntime>(options);
    runtime->start();
    success(client.retry(), Role::Producer, 1, 1);
    exactReplay(proxy, kAppend, original);
    require(!client.hasPending() && client.sequence() == 1 && pendingCount(options) == 1,
        "capacity recovery skipped or duplicated retained batch");
    const auto states = client.loadStates();
    require(states.size() == 1 && states[0].version == 1, "capacity retry applied state more than once");
    require(proxy.exchanges("RegisterProducer").size() == 1, "capacity retry unexpectedly registered a new session");
    proxy.checked();
}

void definiteRejectionAndArguments() {
    Lab lab;
    EventStoreClient client(lab.client());
    client.start();
    const auto error = clientError([&] { client.execute(kAppend, appendArgs("bad-cas", 99)); });
    require(error.code() == "CONFLICT_OR_INVALID" && !error.outcomeUnknown(),
        "state CAS conflict missing definite parameter/state rejection code");
    const auto rejectedRequest = client.pendingRequest();
    frozen(client, rejectedRequest);
    require(lab.proxy.exchanges(kAppend).size() == 1 && pendingCount(lab.options) == 0,
        "definite rejection fixture did not reach runtime or committed an event");
    client.discardRejected();
    require(!client.hasPending() && client.sequence() == 0, "definite discard consumed sequence");
    const auto attempts = lab.proxy.exchanges().size();
    for (const auto* forbidden : {"actorId", "producerId", "senderId", "epoch", "sequence", "sessionId"}) {
        auto fields = appendArgs().asObject();
        fields.values.push_back({forbidden, std::make_shared<Json>(text("injected"))});
        rejects([&] { client.execute(kAppend, Json::makeObject(fields)); });
        require(!client.hasPending(), "caller-supplied identity/sequence occupied pending slot");
    }
    rejects([&] { client.execute(kAppend, array()); });
    rejects([&] { client.execute("ClaimBatch", claimArgs()); });
    require(lab.proxy.exchanges().size() == attempts, "invalid business arguments escaped to IPC");
    success(client.execute(kAppend, appendArgs("fixed-cas")), Role::Producer, 1, 1);
    require(pendingCount(lab.options) == 1, "corrected request did not reuse the original next sequence");
    lab.proxy.checked();
}

void senderLostReplies(Profile profile) {
    Lab lab(profile);
    EventStoreClient producer(lab.client());
    producer.start();
    producer.execute(kAppend, appendArgs("delivery-1"));
    EventStoreClient sender(lab.client(Role::Sender));
    success(sender.start(), Role::Sender, 1, 0);
    lab.proxy.arm("ClaimBatch", ReplyProxy::Fault::Truncate);
    require(clientError([&] { sender.execute("ClaimBatch", claimArgs()); }).outcomeUnknown(), "lost Claim reply not unknown");
    const auto claimRequest = sender.pendingRequest();
    frozen(sender, claimRequest);
    const auto firstClaim = lab.proxy.exchanges("ClaimBatch").at(0);
    const auto committedClaim = parse(firstClaim.runtimeReply);
    success(committedClaim, Role::Sender, 1, 1);
    require(stringField(committedClaim, "status") == "CLAIMED" &&
        field(committedClaim, "messages").asArray().values.size() == 1, "Claim fault did not follow a real claim");
    rejects([&] { sender.execute("ReleaseBatch", finishArgs(committedClaim)); });
    rejects([&] { sender.discardRejected(); });
    lab.restart();
    const auto claimed = sender.retry();
    success(claimed, Role::Sender, 1, 1);
    require(encode(claimed) == encode(committedClaim), "Claim retry changed token, lease, payload or receipt");
    exactReplay(lab.proxy, "ClaimBatch", claimRequest);
    require(stringField(*field(claimed, "messages").asArray().values[0], "eventId") == "delivery-1",
        "Claim returned a different event");

    const auto inProgress = clientError([&] { sender.execute("ClaimBatch", claimArgs()); });
    require(inProgress.code() == "BATCH_IN_PROGRESS" && !inProgress.outcomeUnknown(),
        "new Claim during active batch did not return definite BATCH_IN_PROGRESS");
    const auto rejectedClaim = sender.pendingRequest();
    frozen(sender, rejectedClaim, 1);
    const auto rejectedExchange = lab.proxy.exchanges("ClaimBatch").back();
    const auto rejectedReply = parse(rejectedExchange.runtimeReply);
    require(rejectedExchange.request == rejectedClaim && !field(rejectedReply, "ok").asBool() &&
        stringField(rejectedReply, "code") == "BATCH_IN_PROGRESS" &&
        stringField(rejectedReply, "outcome") == "not_committed" &&
        integerField(field(parse(rejectedClaim), "args"), "sequence") == 2,
        "active-batch rejection did not come from real runtime at original next sequence");
    require(encode(sender.receipt()) == encode(claimed) && pendingCount(lab.options) == 1,
        "rejected Claim changed old batch receipt or marked the event sent");
    frozen(sender, rejectedClaim, 1);
    const auto attempts = lab.proxy.exchanges().size();
    rejects([&] { sender.execute("AckBatch", finishArgs(claimed)); });
    frozen(sender, rejectedClaim, 1);
    require(lab.proxy.exchanges().size() == attempts, "ACK replaced rejected Claim before explicit discard");
    sender.discardRejected();
    require(!sender.hasPending() && sender.pendingRequest().empty() && sender.sequence() == 1,
        "definite BATCH_IN_PROGRESS could not be discarded without consuming sequence");

    lab.proxy.arm("AckBatch", ReplyProxy::Fault::Truncate);
    require(clientError([&] { sender.execute("AckBatch", finishArgs(claimed)); }).outcomeUnknown(), "lost ACK reply not unknown");
    const auto ackRequest = sender.pendingRequest();
    frozen(sender, ackRequest, 1);
    require(integerField(field(parse(ackRequest), "args"), "sequence") == 2 &&
        stringField(field(parse(ackRequest), "args"), "claimToken") == stringField(claimed, "claimToken"),
        "ACK after discarded Claim did not reuse next sequence and original batch token");
    const auto committedAck = lab.proxy.exchanges("AckBatch").at(0).runtimeReply;
    require(pendingCount(lab.options) == 0, "ACK fault was not after marking event sent");
    const auto ack = sender.retry();
    success(ack, Role::Sender, 1, 2);
    require(encode(ack) == encode(parse(committedAck)) &&
        stringField(*field(ack, "results").asArray().values.at(0), "status") == "APPLIED",
        "ACK replay re-executed instead of returning the original APPLIED receipt");
    exactReplay(lab.proxy, "AckBatch", ackRequest);
    require(!sender.hasPending() && pendingCount(lab.options) == 0, "ACK recovery left pending/unsent data");
    const auto empty = sender.execute("ClaimBatch", claimArgs());
    success(empty, Role::Sender, 1, 3);
    require(stringField(empty, "status") == "EMPTY" && field(empty, "messages").asArray().values.empty(),
        "ACKed event was claimed again");
    producer.execute(kAppend, appendArgs("delivery-2", 1));
    const auto next = sender.execute("ClaimBatch", claimArgs());
    success(next, Role::Sender, 1, 4);
    const auto released = sender.execute("ReleaseBatch", finishArgs(next));
    success(released, Role::Sender, 1, 5);
    require(stringField(*field(released, "results").asArray().values.at(0), "status") == "APPLIED" &&
        pendingCount(lab.options) == 1, "Release marked event sent or failed");
    const auto reclaimed = sender.execute("ClaimBatch", claimArgs());
    require(stringField(*field(reclaimed, "messages").asArray().values.at(0), "eventId") == "delivery-2",
        "released event was not claimable again");
    sender.execute("AckBatch", finishArgs(reclaimed));
    require(pendingCount(lab.options) == 0, "sender lifecycle leaked an unsent event");
    lab.proxy.checked();
}

void seedPages(EventStoreClient& client, int batchSize = 64) {
    require(batchSize > 0 && batchSize <= 64, "invalid baseline seed batch size");
    for (int first = 0; first < 65; first += batchSize) {
        std::vector<Json> states;
        for (int i = first; i < std::min(first + batchSize, 65); ++i) {
            const auto number = std::to_string(1000 + i);
            states.push_back(state("state-" + number, 0, i));
        }
        client.execute(kAppend, object({{"events", array()}, {"states", array(states)}}));
    }
}

void snapshotSmallFramesAndDeadline() {
    Lab lab(Profile::DeleteFull, 4096);
    auto options = lab.client();
    options.timeoutMs = 1000;
    EventStoreClient client(options);
    client.start();
    seedPages(client, 8);
    const auto states = client.loadStates(65);
    require(states.size() == 65 && states.front().state.stateKey == "state-1000" &&
        states.back().state.stateKey == "state-1064", "small-frame snapshot stopped at a short nonempty page");
    const auto pages = lab.proxy.exchanges("LoadStates");
    require(pages.size() >= 3, "small runtime frame did not force multiple data pages and EOF");
    std::size_t rows = 0;
    std::string cursor;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const auto request = parse(pages[i].request);
        require(stringField(field(request, "args"), "afterKey") == cursor &&
            integerField(field(request, "args"), "limit") == 64,
            "small-frame pagination did not follow last page cursor with limit 64");
        const auto response = parse(pages[i].runtimeReply);
        const auto count = field(response, "states").asArray().values.size();
        require(field(response, "ok").asBool() && pages[i].runtimeReply.size() <= 4096 &&
            integerField(response, "pageCount") == static_cast<std::int64_t>(count),
            "small-frame fixture returned an invalid/unbounded runtime page");
        if (i + 1 == pages.size()) {
            require(count == 0 && stringField(response, "nextKey").empty(), "snapshot omitted terminal empty page");
        } else {
            require(count > 0 && count < 64, "runtime frame did not produce a nonempty short page");
            cursor = stringField(*field(response, "states").asArray().values.back(), "stateKey");
            require(stringField(response, "nextKey") == cursor, "short page cursor mismatched last state");
            rows += count;
        }
    }
    require(rows == 65, "short-page traversal omitted or repeated states");

    // Each delay is below one call budget, but their sum exceeds the entire snapshot budget.
    lab.proxy.delayResponses("LoadStates", 650, 2);
    const auto started = Clock::now();
    const auto error = clientError([&] { (void)client.loadStates(65); });
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    require(error.code() == "TRANSPORT" && !error.outcomeUnknown(), "snapshot deadline did not fail as a read transport error");
    lab.proxy.waitForExchanges("LoadStates", pages.size() + 2);
    const auto delayed = lab.proxy.exchanges("LoadStates");
    require(delayed.size() == pages.size() + 2 && !delayed[pages.size()].clientDisconnected &&
        delayed.back().clientDisconnected && delayed.back().deliveredReply.empty() &&
        field(parse(delayed.back().runtimeReply), "ok").asBool(),
        "shared deadline did not disconnect during second completed-but-delayed page");
    require(elapsed >= 900 && elapsed < 2000, "snapshot did not respect one overall deadline with scheduling margin");
    require(!client.hasPending() && client.sequence() == 9 && client.epoch() == 1,
        "read deadline changed mutation state");
    lab.proxy.checked();
    require(client.loadStates(65).size() == 65 && pendingCount(lab.options) == 0,
        "snapshot could not recover completely after deadline or generated baseline events");
}

void snapshotPagesAndRecovery() {
    snapshotSmallFramesAndDeadline();
    Lab lab;
    {
        EventStoreClient client(lab.client());
        client.start();
        seedPages(client);
        const auto before = lab.proxy.exchanges("LoadStates").size();
        const auto states = client.loadStates(65);
        require(states.size() == 65 && states.front().state.stateKey == "state-1000" &&
            states.back().state.stateKey == "state-1064", "snapshot omitted boundary/tail states");
        std::set<std::string> keys;
        for (const auto& item : states) {
            require(item.version == 1, "baseline version changed during paging");
            keys.insert(item.state.stateKey);
        }
        require(keys.size() == 65 && lab.proxy.exchanges("LoadStates").size() - before >= 2,
            "snapshot did not exercise complete keyset pagination");
        rejects([&] { client.loadStates(64); });
        require(!client.hasPending() && client.sequence() == 2, "snapshot bound failure changed mutation state");
        lab.proxy.arm("LoadStates", ReplyProxy::Fault::Truncate, {}, 1);
        rejects([&] { (void)client.loadStates(65); });
        lab.proxy.checked();
        require(client.loadStates(65).size() == 65 && pendingCount(lab.options) == 0,
            "failed page leaked partial state or baseline generated events");
    }
    lab.restart();
    EventStoreClient recovered(lab.client());
    success(recovered.start(), Role::Producer, 2, 0);
    require(recovered.loadStates(65).size() == 65, "new session failed to restore persisted baseline");
    recovered.execute(kAppend, object({{"events", array()}, {"states", array({state("state-1000", 1, 0)})}}));
    const auto updated = recovered.loadStates(65);
    require(updated.front().version == 2 && updated.back().version == 1, "restored baseline used wrong CAS versions");
    lab.proxy.checked();
}

void snapshotFence() {
    Lab lab;
    EventStoreClient client(lab.client());
    client.start();
    seedPages(client);
    lab.proxy.arm("LoadStates", ReplyProxy::Fault::Rewrite, [&](const std::string& response) {
        const auto takeover = call(lab.options, "RegisterProducer", object({{"producerId", text("p0")},
            {"sessionId", text("snapshot-takeover")}, {"expectedEpoch", text("1")}}));
        success(takeover, Role::Producer, 2, 0);
        return response;
    });
    rejects([&] { (void)client.loadStates(65); });
    lab.proxy.checked();
    require(!client.hasPending() && client.epoch() == 1 && client.sequence() == 2,
        "snapshot takeover silently re-registered or changed local watermark");
    require(lab.proxy.exchanges("RegisterProducer").size() == 1, "snapshot automatically stole actor back");
}

void definiteIdentityConflicts() {
    for (const auto role : {Role::Producer, Role::Sender}) {
        for (const auto* code : {"STALE_EPOCH", "REQUEST_CONFLICT", "STALE_OR_GAPPED_SEQUENCE"}) {
            Lab lab;
            EventStoreClient client(lab.client(role));
            client.start();
            const bool producer = role == Role::Producer;
            const auto* actorKey = producer ? "producerId" : "senderId";
            const auto* actor = producer ? "p0" : "s0";
            const auto* operation = producer ? kAppend : "ClaimBatch";
            const auto* registration = producer ? "RegisterProducer" : "RegisterSender";
            Json remoteReceipt;
            int remoteSequence = 0;
            if (std::string(code) == "STALE_EPOCH") {
                remoteReceipt = call(lab.options, registration, object({{actorKey, text(actor)},
                    {"sessionId", text("definite-takeover")}, {"expectedEpoch", text("1")}}));
                success(remoteReceipt, role, 2, 0);
            } else {
                remoteSequence = std::string(code) == "REQUEST_CONFLICT" ? 1 : 2;
                // Raw IPC intentionally bypasses the client owner to reproduce an external sequence advance.
                for (int sequence = 1; sequence <= remoteSequence; ++sequence) {
                    auto args = producer ? object({{"events", array({event("external-" + std::to_string(sequence))})},
                        {"states", array()}}) : replaceField(claimArgs(), "limit", text("1"));
                    auto fields = args.asObject();
                    fields.values.push_back({actorKey, std::make_shared<Json>(text(actor))});
                    fields.values.push_back({"epoch", std::make_shared<Json>(text("1"))});
                    fields.values.push_back({"sequence", std::make_shared<Json>(decimal(sequence))});
                    remoteReceipt = call(lab.options, operation, Json::makeObject(std::move(fields)));
                    success(remoteReceipt, role, 1, sequence);
                }
            }
            const auto candidate = producer ? appendArgs("blocked-candidate") : claimArgs();
            const auto error = clientError([&] { client.execute(operation, candidate); });
            require(error.code() == code && !error.outcomeUnknown(),
                std::string("first identity/sequence rejection lost structured definite code: ") + code);
            const auto request = client.pendingRequest();
            frozen(client, request);
            const auto exchanges = lab.proxy.exchanges(operation);
            require(exchanges.size() == 1 && exchanges[0].request == request,
                "identity-conflict fixture did not make exactly one client mutation attempt");
            const auto response = parse(exchanges[0].runtimeReply);
            require(!field(response, "ok").asBool() && stringField(response, "code") == code &&
                stringField(response, "outcome") == "not_committed",
                "runtime did not preserve structured identity/sequence conflict");
            const auto attempts = lab.proxy.exchanges().size();
            rejects([&] { client.discardRejected(); });
            const auto replacement = producer ? appendArgs("forbidden-replacement") :
                replaceField(claimArgs(), "limit", text("2"));
            rejects([&] { client.execute(operation, replacement); });
            frozen(client, request);
            require(lab.proxy.exchanges().size() == attempts,
                "first definite identity conflict allowed replacement IPC");
            const auto retried = clientError([&] { client.retry(); });
            require(retried.code() == code && !retried.outcomeUnknown(),
                "exact conflict retry lost original definite structured error");
            rejects([&] { client.discardRejected(); });
            frozen(client, request);
            exactReplay(lab.proxy, operation, request);
            const auto receipt = call(lab.options, producer ? "GetReceipt" : "GetDeliveryReceipt",
                object({{actorKey, text(actor)}}));
            require(encode(receipt) == encode(remoteReceipt) && client.epoch() == 1 &&
                lab.proxy.exchanges(registration).size() == 1 &&
                pendingCount(lab.options) == (producer ? remoteSequence : 0),
                "identity conflict handling stole ownership, advanced receipt, or changed stored events");
            lab.proxy.checked();
        }
    }
}

void pendingEpochFence() {
    definiteIdentityConflicts();
    Lab lab;
    EventStoreClient client(lab.client());
    client.start();
    lab.proxy.arm(kAppend, ReplyProxy::Fault::Truncate);
    require(clientError([&] { client.execute(kAppend, appendArgs()); }).outcomeUnknown(), "fence fixture not unknown");
    const auto request = client.pendingRequest();
    success(call(lab.options, "RegisterProducer", object({{"producerId", text("p0")},
        {"sessionId", text("pending-takeover")}, {"expectedEpoch", text("1")}})), Role::Producer, 2, 0);
    const auto stale = clientError([&] { client.retry(); });
    require(stale.code() == "STALE_EPOCH" && stale.outcomeUnknown(),
        "structured epoch rejection forgot earlier unknown attempt");
    frozen(client, request);
    rejects([&] { client.discardRejected(); });
    rejects([&] { client.execute(kAppend, appendArgs("after-takeover", 1)); });
    frozen(client, request);
    require(client.epoch() == 1 && lab.proxy.exchanges("RegisterProducer").size() == 1 &&
        pendingCount(lab.options) == 1, "stale client automatically registered/replaced a pending event");
    exactReplay(lab.proxy, kAppend, request);
}

void malformedAndWrongReceipts() {
    const std::vector<std::pair<std::string, Json>> changes = {
        {"producerId", text("p1")}, {"sessionId", text("foreign-session")},
        {"epoch", text("2")}, {"sequence", text("2")}
    };
    for (std::size_t i = 0; i <= changes.size(); ++i) {
        Lab lab;
        EventStoreClient client(lab.client());
        client.start();
        lab.proxy.arm(kAppend, ReplyProxy::Fault::Rewrite, [&, i](const std::string& response) {
            return i == changes.size() ? std::string("{broken-json") :
                encode(replaceField(parse(response), changes[i].first, changes[i].second));
        });
        require(clientError([&] { client.execute(kAppend, appendArgs()); }).outcomeUnknown(),
            "malformed/mismatched committed reply treated as definite rejection");
        const auto request = client.pendingRequest();
        frozen(client, request);
        rejects([&] { client.discardRejected(); });
        const auto result = client.retry();
        success(result, Role::Producer, 1, 1);
        exactReplay(lab.proxy, kAppend, request);
        require(pendingCount(lab.options) == 1 && client.loadStates().at(0).version == 1,
            "reply corruption recovery executed mutation twice");
    }
    for (const auto* injectedCode : {"SQLITE_ERROR", "UNRECOGNIZED_ERROR_CODE"}) {
        Lab lab;
        EventStoreClient client(lab.client());
        client.start();
        lab.proxy.arm(kAppend, ReplyProxy::Fault::Rewrite, [injectedCode](const std::string& response) {
            success(parse(response), Role::Producer, 1, 1);
            return encode(object({{"ok", Json::makeBool(false)}, {"code", text(injectedCode)},
                {"outcome", text("not_committed")}, {"message", text("injected")}}));
        });
        const auto invalid = clientError([&] { client.execute(kAppend, appendArgs()); });
        require(invalid.code() == "BAD_RESPONSE" && invalid.outcomeUnknown(),
            std::string("invalid error code/outcome must be BAD_RESPONSE and unknown: ") + injectedCode);
        const auto request = client.pendingRequest();
        frozen(client, request);
        const auto committed = lab.proxy.exchanges(kAppend);
        require(committed.size() == 1 && committed[0].request == request && pendingCount(lab.options) == 1,
            "invalid error envelope was not injected after exactly one real committed Append");
        rejects([&] { client.discardRejected(); });
        frozen(client, request);

        lab.proxy.arm(kAppend, ReplyProxy::Fault::Reject);
        const auto rejected = clientError([&] { client.retry(); });
        require(rejected.code() == "REJECTED" && rejected.outcomeUnknown(),
            "later not_queued rejection erased uncertainty from an invalid error envelope");
        frozen(client, request);
        const auto attempts = lab.proxy.exchanges(kAppend);
        require(attempts.size() == 2 && attempts.back().runtimeReply.empty(),
            "follow-up definite rejection was not injected before runtime forwarding");
        const auto beforeDiscard = lab.proxy.exchanges().size();
        rejects([&] { client.discardRejected(); });
        rejects([&] { client.execute(kAppend, appendArgs("forbidden-after-error-envelope")); });
        frozen(client, request);
        require(lab.proxy.exchanges().size() == beforeDiscard,
            "invalid error envelope plus queue rejection permitted replacement IPC");

        const auto recovered = client.retry();
        success(recovered, Role::Producer, 1, 1);
        require(encode(recovered) == encode(parse(committed[0].runtimeReply)),
            "exact retry after invalid error envelope changed the committed receipt");
        exactReplay(lab.proxy, kAppend, request, 3);
        require(!client.hasPending() && client.pendingRequest().empty() && client.sequence() == 1 &&
            pendingCount(lab.options) == 1, "invalid error envelope recovery duplicated event or left pending work");
        const auto states = client.loadStates();
        require(states.size() == 1 && states[0].version == 1,
            "invalid error envelope recovery reapplied the committed baseline");
        lab.proxy.checked();
    }
}

void helloValidation() {
    const std::vector<std::pair<std::string, Json>> changes = {
        {"backend", text("legacy")}, {"schemaVersion", text("2")}, {"version", text("2")},
        {"storeId", text("foreign-store")}, {"configGeneration", text("foreign-generation")},
        {"synchronous", text("1")}, {"storageProfile", text("wal-normal")}
    };
    for (const auto& change : changes) {
        Lab lab;
        EventStoreClient client(lab.client());
        lab.proxy.arm("Hello", ReplyProxy::Fault::Rewrite, [&](const std::string& response) {
            return encode(replaceField(parse(response), change.first, change.second));
        });
        rejects([&] { client.start(); });
        lab.proxy.checked();
        require(lab.proxy.exchanges("RegisterProducer").empty() && !client.hasPending(),
            "invalid Hello registered actor or queued a mutation: " + change.first);
        success(client.start(), Role::Producer, 1, 0);
    }
    Lab lab;
    auto options = lab.client();
    options.identity.storeId = "wrong-request-store";
    EventStoreClient wrong(options);
    rejects([&] { wrong.start(); });
    require(lab.proxy.exchanges("RegisterProducer").empty(), "wrong request identity reached registration");
    lab.proxy.checked();
}

bool run(const char* name, const std::function<void()>& action) {
    // Isolate SQLite's process-global dynamic API and bound even a deadlocked ownership regression.
    std::cout.flush();
    std::cerr.flush();
    const auto child = fork();
    require(child >= 0, "test runner fork failed");
    if (child == 0) {
        alarm(90);
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
        failed += !run("constructor-offline-actor-role-generation-path-locks", constructorAndOwnership);
        failed += !run("lock-symlink-hardlink-inode-replacement", lockFileGuards);
        failed += !run("thread-fork-ownership-parent-lock-survival", threadAndForkGuards);
        failed += !run("producer-real-ipc-delete-full-restart", [] { producerLifecycle(Profile::DeleteFull); });
        failed += !run("producer-real-ipc-wal-full-restart", [] { producerLifecycle(Profile::WalFull); });
        failed += !run("producer-registration-lost-reply-exact-retry", [] { lostRegistration(Role::Producer); });
        failed += !run("sender-registration-lost-reply-exact-retry", [] { lostRegistration(Role::Sender); });
        failed += !run("append-lost-reply-single-flight-exact-retry", [] { lostAppend(false); });
        failed += !run("unknown-then-not-queued-cannot-discard", [] { lostAppend(true); });
        failed += !run("definite-rejection-discard-business-only-arguments", definiteRejectionAndArguments);
        failed += !run("capacity-rejected-recover-original-request-delete", [] { capacityRejectionAndRecovery(Profile::DeleteFull); });
        failed += !run("capacity-rejected-recover-original-request-wal", [] { capacityRejectionAndRecovery(Profile::WalFull); });
        failed += !run("sender-claim-ack-lost-replies-release-delete", [] { senderLostReplies(Profile::DeleteFull); });
        failed += !run("sender-claim-ack-lost-replies-release-wal", [] { senderLostReplies(Profile::WalFull); });
        failed += !run("snapshot-pages-bound-failed-page-new-session-recovery", snapshotPagesAndRecovery);
        failed += !run("snapshot-mid-page-epoch-fencing", snapshotFence);
        failed += !run("pending-stale-epoch-no-auto-register-or-discard", pendingEpochFence);
        failed += !run("malformed-wrong-actor-session-epoch-sequence-receipts", malformedAndWrongReceipts);
        failed += !run("hello-backend-schema-identity-generation-full-validation", helloValidation);
    } catch (const std::exception& error) {
        std::cerr << "FAIL client-test-runner: " << error.what() << std::endl;
        return 1;
    }
    std::cout << "SUMMARY groups=" << groups << " failed=" << failed << std::endl;
    return failed ? 1 : 0;
}
