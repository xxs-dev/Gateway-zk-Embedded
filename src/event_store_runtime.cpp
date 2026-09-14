#include "edge_gateway/event_store_runtime.hpp"
#include "edge_gateway/event_history_projection.hpp"
#include "edge_gateway/json_value.hpp"
#include "edge_gateway/sqlite_error.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <future>
#include <iomanip>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace edge_gateway {
namespace {
using Json = json::JsonValue;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kControlQueue = 0;
constexpr std::size_t kAlarmQueue = 1;
constexpr std::size_t kNormalQueue = 2;
constexpr std::size_t kClaimQueue = 3;
constexpr std::size_t kMaintenanceQueue = 4;
constexpr std::size_t kReadQueue = 5;
constexpr std::size_t kQueueCount = 6;

class Fd {
public:
    explicit Fd(int value = -1) : value_(value) {}
    ~Fd() { if (value_ >= 0) ::close(value_); }
    Fd(Fd&& other) noexcept : value_(other.value_) { other.value_ = -1; }
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) { if (value_ >= 0) ::close(value_); value_ = other.value_; other.value_ = -1; }
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return value_; }
private:
    int value_;
};

std::string quote(const std::string& value) {
    std::string result = "\"";
    const char* hex = "0123456789abcdef";
    for (unsigned char ch : value) {
        if (ch == '\"' || ch == '\\') { result += '\\'; result += static_cast<char>(ch); }
        else if (ch < 32) { result += "\\u00"; result += hex[ch >> 4]; result += hex[ch & 15]; }
        else result += static_cast<char>(ch);
    }
    result += '\"';
    return result;
}

void fields(const Json& value, std::initializer_list<const char*> allowed) {
    if (!value.isObject()) throw std::invalid_argument("expected JSON object");
    std::set<std::string> seen;
    for (const auto& entry : value.asObject().values) {
        if (!seen.insert(entry.key).second || std::none_of(allowed.begin(), allowed.end(),
                [&](const char* key) { return entry.key == key; })) {
            throw std::invalid_argument("duplicate or unknown JSON field");
        }
    }
}

const Json& required(const Json& value, const char* key) {
    const auto* result = value.find(key);
    if (!result) throw std::invalid_argument(std::string("missing field: ") + key);
    return *result;
}

std::string string(const Json& value, const char* key, std::size_t max = 256 * 1024) {
    const auto& result = required(value, key).asString();
    if (result.size() > max || result.find('\0') != std::string::npos) throw std::invalid_argument("invalid text field");
    return result;
}

void nonemptyText(const std::string& value, std::size_t max) {
    if (value.empty() || value.size() > max || value.find('\0') != std::string::npos) {
        throw std::invalid_argument("text must be nonempty, bounded and contain no NUL");
    }
}

std::string nonemptyString(const Json& value, const char* key, std::size_t max) {
    auto result = string(value, key, max);
    nonemptyText(result, max);
    return result;
}

std::int64_t integer(const Json& value, const char* key, std::int64_t minimum = 0) {
    const auto text = string(value, key, 20);
    if (text.empty()) throw std::invalid_argument("empty decimal integer");
    std::size_t consumed = 0;
    std::int64_t number;
    try { number = std::stoll(text, &consumed, 10); }
    catch (...) { throw std::invalid_argument("decimal integer out of range"); }
    if (consumed != text.size() || std::to_string(number) != text || number < minimum) {
        throw std::invalid_argument("integer must be a canonical decimal string");
    }
    return number;
}

std::string frame(const std::string& payload, std::size_t maxBytes) {
    if (payload.empty() || payload.size() > maxBytes) throw std::length_error("event store frame limit exceeded");
    const auto size = static_cast<std::uint32_t>(payload.size());
    std::string data(4, '\0');
    for (int i = 0; i < 4; ++i) data[i] = static_cast<char>(size >> (24 - 8 * i));
    data += payload;
    return data;
}

std::size_t frameLength(const char* data) {
    std::uint32_t result = 0;
    for (int i = 0; i < 4; ++i) result = (result << 8) | static_cast<unsigned char>(data[i]);
    return result;
}

sockaddr_un address(const std::string& path) {
    sockaddr_un result{};
    result.sun_family = AF_UNIX;
    if (path.empty() || path.front() != '/' || path.size() >= sizeof(result.sun_path) || path.find('\0') != std::string::npos) {
        throw std::invalid_argument("invalid absolute event store socket path");
    }
    std::memcpy(result.sun_path, path.c_str(), path.size() + 1);
    return result;
}

std::string error(const char* code, const std::string& message, const char* outcome) {
    return "{\"ok\":false,\"code\":" + quote(code) + ",\"outcome\":" + quote(outcome) +
        ",\"message\":" + quote(message.substr(0, 512)) + "}";
}

std::string producerJson(const EventStoreProducer& p) {
    return "{\"ok\":true,\"producerId\":" + quote(p.producerId) + ",\"sessionId\":" + quote(p.sessionId) +
        ",\"epoch\":\"" + std::to_string(p.epoch) + "\",\"sequence\":\"" + std::to_string(p.sequence) +
        "\",\"receipt\":" + (p.receipt.empty() ? "null" : p.receipt) + "}";
}

struct Request {
    std::string op;
    std::string producer;
    std::string sender;
    std::string session;
    std::string after;
    std::string target;
    EventStatsScope statsScope;
    std::int64_t epoch = 0;
    std::size_t limit = 32;
    EventStoreAppend append;
    EventStoreDeliveryRequest delivery;
    std::size_t category = kControlQueue;
};

Request decode(const std::string& body, const EventStoreRuntimeOptions& options) {
    const auto root = json::JsonParser(body, 16, 4096).parse();
    fields(root, {"version", "storeId", "configGeneration", "op", "args"});
    if (integer(root, "version") != 1 || string(root, "storeId", 96) != options.identity.storeId ||
        string(root, "configGeneration", 96) != options.identity.configGeneration) {
        throw std::invalid_argument("event store protocol/identity/generation mismatch");
    }
    Request request;
    request.op = string(root, "op", 64);
    const auto& args = required(root, "args");
    if (request.op == "Hello") {
        fields(args, {});
    } else if (request.op == "GetHistoryProjectionStatus") {
        fields(args, {});
        request.category = kReadQueue;
    } else if (request.op == "GetCapacityStatus") {
        fields(args, {});
        request.category = kReadQueue;
    } else if (request.op == "RegisterProducer") {
        fields(args, {"producerId", "sessionId", "expectedEpoch"});
        request.producer = string(args, "producerId", 96);
        request.session = string(args, "sessionId", 96);
        request.epoch = integer(args, "expectedEpoch");
    } else if (request.op == "GetReceipt") {
        fields(args, {"producerId"});
        request.producer = string(args, "producerId", 96);
    } else if (request.op == "RegisterSender") {
        fields(args, {"senderId", "sessionId", "expectedEpoch"});
        request.sender = nonemptyString(args, "senderId", 96);
        request.session = nonemptyString(args, "sessionId", 96);
        request.epoch = integer(args, "expectedEpoch");
    } else if (request.op == "GetDeliveryReceipt") {
        fields(args, {"senderId"});
        request.sender = nonemptyString(args, "senderId", 96);
    } else if (request.op == "GetSenderScope") {
        fields(args, {"senderId"});
        request.sender = nonemptyString(args, "senderId", 96);
        request.category = kReadQueue;
    } else if (request.op == "ClaimBatch" || request.op == "AckBatch" || request.op == "ReleaseBatch") {
        if (request.op == "ClaimBatch") {
            fields(args, {"senderId", "epoch", "sequence", "limit", "maxBytes", "leaseMs"});
        } else {
            fields(args, {"senderId", "epoch", "sequence", "claimToken", "items"});
        }
        request.sender = nonemptyString(args, "senderId", 96);
        auto& delivery = request.delivery;
        delivery.operation = request.op;
        delivery.senderId = request.sender;
        delivery.epoch = integer(args, "epoch", 1);
        delivery.sequence = integer(args, "sequence", 1);
        delivery.request = body;
        if (request.op == "ClaimBatch") {
            const auto limit = integer(args, "limit", 1);
            const auto maxBytes = integer(args, "maxBytes", 1);
            delivery.leaseMs = integer(args, "leaseMs", 100);
            if (limit > 16 || maxBytes > 32768 || delivery.leaseMs > 30000) {
                throw std::invalid_argument("ClaimBatch requires limit 1..16, maxBytes 1..32768, leaseMs 100..30000");
            }
            // Reserve receipt/envelope overhead and worst-case JSON escaping before a claim can COMMIT.
            constexpr std::size_t overhead = 65536;
            constexpr std::size_t expansion = 6;
            if (options.maxFrameBytes <= overhead) {
                throw std::invalid_argument("ClaimBatch requires maxFrameBytes > 65536 for a recoverable response");
            }
            const auto responseBudget = (options.maxFrameBytes - overhead) / expansion;
            if (static_cast<std::size_t>(maxBytes) > responseBudget) {
                throw std::invalid_argument("ClaimBatch maxBytes exceeds response frame budget (maximum " +
                    std::to_string(responseBudget) + ")");
            }
            delivery.limit = static_cast<std::size_t>(limit);
            delivery.maxBytes = static_cast<std::size_t>(maxBytes);
            request.category = kClaimQueue;
        } else {
            delivery.claimToken = nonemptyString(args, "claimToken", 256);
            const auto& items = required(args, "items").asArray().values;
            if (items.empty() || items.size() > 16) throw std::invalid_argument("delivery items must contain 1..16 entries");
            for (const auto& value : items) {
                fields(*value, {"id", "eventId"});
                EventStoreClaimItem item;
                item.id = integer(*value, "id", 1);
                item.eventId = nonemptyString(*value, "eventId", 256);
                delivery.items.push_back(std::move(item));
            }
        }
    } else if (request.op == "LoadStates") {
        fields(args, {"producerId", "afterKey", "limit"});
        request.producer = string(args, "producerId", 96);
        request.after = string(args, "afterKey", 256);
        const auto limit = integer(args, "limit", 1);
        if (limit > 64) throw std::invalid_argument("state page limit exceeds 64");
        request.limit = static_cast<std::size_t>(limit);
        request.category = kReadQueue;
    } else if (request.op == "GetStats") {
        fields(args, {"targetId"});
        request.target = string(args, "targetId", 96);
        if (request.target.empty()) throw std::invalid_argument("targetId is empty");
        request.category = kReadQueue;
    } else if (request.op == "GetScopedStats") {
        fields(args, {"targetId", "selection", "include", "exclude"});
        auto& scope = request.statsScope;
        scope.targetId = string(args, "targetId", 96);
        const auto selection = string(args, "selection", 4);
        if (selection == "all") scope.selection = EventStatsSelection::All;
        else if (selection == "only") scope.selection = EventStatsSelection::Only;
        else throw std::invalid_argument("stats selection must be all or only");
        for (const auto& value : required(args, "include").asArray().values) {
            scope.include.push_back(value->asString());
        }
        for (const auto& value : required(args, "exclude").asArray().values) {
            scope.exclude.push_back(value->asString());
        }
        scope = normalizeEventStatsScope(std::move(scope));
        request.category = kReadQueue;
    } else if (request.op == "AppendEventsAndStates") {
        fields(args, {"producerId", "epoch", "sequence", "events", "states", "localEvents"});
        request.producer = string(args, "producerId", 96);
        auto& append = request.append;
        append.producerId = request.producer;
        append.epoch = integer(args, "epoch", 1);
        append.sequence = integer(args, "sequence", 1);
        append.request = body;
        const auto& events = required(args, "events").asArray().values;
        const auto& states = required(args, "states").asArray().values;
        const auto* localValue = args.find("localEvents");
        const std::size_t localCount = localValue ? localValue->asArray().values.size() : 0;
        if (events.size() > 64 || states.size() > 64 || localCount > 64 ||
            (events.empty() && states.empty() && localCount == 0)) {
            throw std::invalid_argument("append must contain 1..64 events or baseline states");
        }
        request.category = kNormalQueue;
        for (const auto& value : events) {
            fields(*value, {"eventId", "targetId", "eventType", "topic", "payload", "eventTs"});
            MqttEventOutbox::EventMessage event;
            event.eventId = string(*value, "eventId", 256);
            event.targetId = string(*value, "targetId", 96);
            if (event.targetId.empty()) throw std::invalid_argument("targetId must be explicit and nonempty");
            event.eventType = string(*value, "eventType", 96);
            event.topic = string(*value, "topic", 4096);
            event.payload = string(*value, "payload");
            event.eventTs = integer(*value, "eventTs", 1);
            if (event.eventType == "alarm") request.category = kAlarmQueue;
            append.events.push_back(std::move(event));
        }
        for (const auto& value : states) {
            fields(*value, {"stateKey", "eventType", "index", "alarmType", "active", "value", "quality", "sourceTs", "lifecycle", "expectedVersion"});
            EventStoreState item;
            auto& state = item.state;
            state.stateKey = string(*value, "stateKey", 256);
            state.eventType = string(*value, "eventType", 96);
            const auto index = integer(*value, "index");
            if (index > std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument("point index out of range");
            state.index = static_cast<std::uint32_t>(index);
            state.alarmType = string(*value, "alarmType", 256);
            state.active = required(*value, "active").asBool();
            state.value = required(*value, "value").asNumber();
            if (!std::isfinite(state.value)) throw std::invalid_argument("nonfinite state value");
            const auto quality = integer(*value, "quality", std::numeric_limits<int>::min());
            if (quality > std::numeric_limits<int>::max()) throw std::invalid_argument("quality out of range");
            state.quality = static_cast<int>(quality);
            state.sourceTs = integer(*value, "sourceTs");
            state.lifecycle = string(*value, "lifecycle", 4096);
            item.version = integer(*value, "expectedVersion");
            append.states.push_back(std::move(item));
        }
        if (localValue) for (const auto& value : localValue->asArray().values) {
            fields(*value, {"kind", "eventId", "index", "machineCode", "meterCode", "pointCode",
                "alarmType", "active", "threshold", "value", "quality", "ts", "stale",
                "persistValue", "stateVersion", "configGeneration"});
            EventStoreLocalEvent local;
            local.kind = nonemptyString(*value, "kind", 16);
            local.configGeneration = nonemptyString(*value, "configGeneration", 96);
            local.stateVersion = integer(*value, "stateVersion");
            auto& event = local.event;
            event.eventId = nonemptyString(*value, "eventId", 256);
            const auto index = integer(*value, "index");
            if (index > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("local event index out of range");
            event.index = static_cast<std::uint32_t>(index);
            event.machineCode = string(*value, "machineCode", 256);
            event.meterCode = string(*value, "meterCode", 256);
            event.pointCode = string(*value, "pointCode", 256);
            event.alarmType = string(*value, "alarmType", 256);
            event.active = required(*value, "active").asBool();
            event.threshold = required(*value, "threshold").asNumber();
            event.value = required(*value, "value").asNumber();
            if (!std::isfinite(event.threshold) || !std::isfinite(event.value))
                throw std::invalid_argument("nonfinite local event value");
            const auto quality = integer(*value, "quality", std::numeric_limits<int>::min());
            if (quality > std::numeric_limits<int>::max()) throw std::invalid_argument("local quality out of range");
            event.quality = static_cast<int>(quality);
            event.ts = integer(*value, "ts", 1);
            event.stale = required(*value, "stale").asBool();
            event.persistValue = string(*value, "persistValue", 4096);
            if (local.kind == "alarm") request.category = kAlarmQueue;
            append.localEvents.push_back(std::move(local));
        }
    } else throw std::invalid_argument("unsupported laboratory event store operation");
    if (!request.producer.empty() && std::find(options.producers.begin(), options.producers.end(), request.producer) == options.producers.end()) {
        throw std::invalid_argument("producer is not configured");
    }
    if (!request.sender.empty() && std::none_of(options.senders.begin(), options.senders.end(),
            [&](const EventStoreSenderConfig& sender) { return sender.senderId == request.sender; })) {
        throw std::invalid_argument("sender is not configured");
    }
    return request;
}

std::string statsScopeJson(const EventStatsScope& scope) {
    const auto typesJson = [](const std::vector<std::string>& types) {
        std::string result = "[";
        for (const auto& type : types) {
            if (result.size() > 1) result += ',';
            result += quote(type);
        }
        return result + ']';
    };
    return "{\"targetId\":" + quote(scope.targetId) + ",\"selection\":" +
        quote(scope.selection == EventStatsSelection::All ? "all" : "only") +
        ",\"include\":" + typesJson(scope.include) + ",\"exclude\":" + typesJson(scope.exclude) + "}";
}

std::string statesJson(const std::vector<EventStoreState>& states, std::size_t maxBytes) {
    std::string items;
    std::string cursor;
    std::size_t count = 0;
    for (const auto& item : states) {
        const auto& s = item.state;
        std::ostringstream row;
        row.imbue(std::locale::classic());
        row << std::setprecision(17) << "{\"stateKey\":" << quote(s.stateKey) << ",\"eventType\":" << quote(s.eventType)
            << ",\"index\":\"" << s.index << "\",\"alarmType\":" << quote(s.alarmType)
            << ",\"active\":" << (s.active ? "true" : "false") << ",\"value\":" << s.value
            << ",\"quality\":\"" << s.quality << "\",\"sourceTs\":\"" << s.sourceTs
            << "\",\"lifecycle\":" << quote(s.lifecycle) << ",\"version\":\"" << item.version << "\"}";
        if (items.size() + row.str().size() + quote(s.stateKey).size() + 128 > maxBytes) break;
        if (count++) items += ',';
        items += row.str();
        cursor = s.stateKey;
    }
    if (!states.empty() && count == 0) throw std::length_error("state exceeds response frame limit");
    return "{\"ok\":true,\"states\":[" + items + "],\"nextKey\":" + quote(cursor) +
        ",\"pageCount\":\"" + std::to_string(count) + "\"}";
}

std::string canonicalFile(const std::string& path) {
    char* resolved = ::realpath(path.c_str(), nullptr);
    if (!resolved) throw std::runtime_error("cannot resolve event store file");
    std::string result(resolved);
    std::free(resolved);
    return result;
}

void verifyFile(int fd, const std::string& path) {
    struct stat opened{}, current{};
    if (fstat(fd, &opened) != 0 || stat(path.c_str(), &current) != 0 ||
        !S_ISREG(opened.st_mode) || opened.st_nlink != 1 ||
        opened.st_dev != current.st_dev || opened.st_ino != current.st_ino) {
        throw std::runtime_error("event store file was replaced, is hard-linked or is not regular");
    }
}

std::string canonicalFutureFile(const std::string& path) {
    if (path.empty() || path.front() != '/' || path.find('\0') != std::string::npos)
        throw std::invalid_argument("storage paths must be absolute");
    const auto slash = path.find_last_of('/');
    const auto name = path.substr(slash + 1);
    if (name.empty() || name == "." || name == "..") throw std::invalid_argument("invalid storage filename");
    auto parent = canonicalFile(slash == 0 ? "/" : path.substr(0, slash));
    return parent + (parent == "/" ? "" : "/") + name;
}

void validateHistoryPaths(const EventStoreRuntimeOptions& options) {
    const auto source = canonicalFutureFile(options.databasePath);
    const auto history = canonicalFutureFile(options.historyPath);
    const auto socket = canonicalFutureFile(options.socketPath);
    std::vector<std::string> paths{source, source + "-wal", source + "-shm", source + "-journal",
        history, history + "-wal", history + "-shm", history + "-journal", socket, socket + ".lock"};
    if (!options.sqliteLibraryPath.empty()) paths.push_back(canonicalFile(options.sqliteLibraryPath));
    for (std::size_t i = 0; i < paths.size(); ++i) {
        struct stat a{};
        const bool exists = lstat(paths[i].c_str(), &a) == 0;
        if (exists && S_ISLNK(a.st_mode)) throw std::invalid_argument("storage path must not be a symlink");
        for (std::size_t j = 0; j < i; ++j) {
            struct stat b{};
            if (paths[i] == paths[j] || (exists && stat(paths[j].c_str(), &b) == 0 &&
                a.st_dev == b.st_dev && a.st_ino == b.st_ino))
                throw std::invalid_argument("event store configured paths physically alias");
        }
    }
}
}  // namespace

struct EventStoreRuntime::Impl {
    struct Job {
        Request request;
        std::string response;
        std::atomic<bool> complete{false};
        Clock::time_point queued = Clock::now();
        std::shared_ptr<void> reservation;
        std::function<void(EventStoreDatabase&)> internal;
    };
    struct Peer {
        explicit Peer(int value, int timeout) : fd(value), deadline(Clock::now() + std::chrono::milliseconds(timeout)) {}
        Fd fd;
        Clock::time_point deadline;
        std::string input;
        std::string output;
        std::size_t expected = 0;
        std::size_t sent = 0;
        std::shared_ptr<void> reservation;
        std::shared_ptr<Job> job;
    };
    explicit Impl(EventStoreRuntimeOptions value) : options(std::move(value)) {}
    EventStoreRuntimeOptions options;
    Fd databaseLock;
    Fd socketLock;
    Fd listener;
    Fd historyGuard;
    std::string canonicalDatabase;
    std::string canonicalHistory;
    std::atomic<bool> stopping{false};
    std::atomic<bool> isReady{false};
    std::mutex mutex;
    std::condition_variable work;
    std::array<std::deque<std::shared_ptr<Job>>, kQueueCount> queues;
    std::thread writer;
    std::thread reader;
    std::thread io;
    std::thread historyWorker;
    mutable std::mutex historyMutex;
    EventStoreHistoryStatus history;
    std::atomic<std::uint64_t> journalWake{0};
    std::shared_ptr<void> historyReservation;
    std::size_t usedBytes = 0;
    std::size_t usedJobs = 0;
    std::size_t turn = 0;
    bool deleteSqlActive = false;
    bool deleteReaderTurn = false;
    bool ownsSocket = false;
    struct stat ownedSocket{};

    std::shared_ptr<void> reserve(std::size_t bytes) {
        std::lock_guard<std::mutex> lock(mutex);
        if (usedJobs >= 128 || bytes > options.memoryBudgetBytes - usedBytes) return {};
        usedBytes += bytes;
        ++usedJobs;
        return std::shared_ptr<void>(new char, [this, bytes](void* p) {
            delete static_cast<char*>(p);
            std::lock_guard<std::mutex> lock(mutex);
            usedBytes -= bytes;
            --usedJobs;
        });
    }

    std::shared_ptr<Job> next(bool reading) {
        std::unique_lock<std::mutex> lock(mutex);
        work.wait(lock, [&] {
            if (stopping) return true;
            const bool reads = !queues[kReadQueue].empty();
            const bool writes = std::any_of(queues.begin(), queues.begin() + kReadQueue,
                [](const std::deque<std::shared_ptr<Job>>& queue) { return !queue.empty(); });
            if (!(reading ? reads : writes)) return false;
            if (options.profile == MqttEventOutbox::StorageProfile::WalFull) return true;
            if (deleteSqlActive) return false;
            return !(reads && writes) || reading == deleteReaderTurn;
        });
        if (stopping) return {};
        std::size_t category = kReadQueue;
        if (!reading) {
            static const std::array<std::size_t, 11> schedule{{
                kControlQueue, kAlarmQueue, kNormalQueue, kClaimQueue,
                kControlQueue, kAlarmQueue, kNormalQueue, kClaimQueue, kControlQueue, kAlarmQueue, kMaintenanceQueue}};
            const auto now = Clock::now();
            auto oldest = now;
            bool aged = false;
            for (std::size_t i = 0; i < kReadQueue; ++i) {
                if (!queues[i].empty() && queues[i].front()->queued < now - std::chrono::milliseconds(250) &&
                    queues[i].front()->queued < oldest) {
                    category = i; oldest = queues[i].front()->queued; aged = true;
                }
            }
            if (!aged) for (std::size_t i = 0; i < schedule.size(); ++i) {
                category = schedule[turn++ % schedule.size()];
                if (!queues[category].empty()) break;
            }
        }
        auto job = queues[category].front();
        queues[category].pop_front();
        if (options.profile == MqttEventOutbox::StorageProfile::DeleteFull) {
            // DELETE cannot overlap readers with COMMIT; alternate when both sides are busy.
            deleteSqlActive = true;
            deleteReaderTurn = !reading;
        }
        return job;
    }

    bool pendingAppend(const std::string& producer) {
        std::lock_guard<std::mutex> lock(mutex);
        for (std::size_t category : {kAlarmQueue, kNormalQueue}) for (const auto& job : queues[category]) {
            if (job->request.producer == producer) return true;
        }
        return false;
    }

    bool pendingDelivery(const std::string& sender) {
        std::lock_guard<std::mutex> lock(mutex);
        for (std::size_t category = 0; category < kReadQueue; ++category) for (const auto& job : queues[category]) {
            if (job->request.sender == sender && job->request.op != "GetDeliveryReceipt") return true;
        }
        return false;
    }

    EventStoreHistoryStatus historyStatus() const {
        std::lock_guard<std::mutex> lock(historyMutex);
        return history;
    }

    void historyState(const std::string& state, std::int64_t projected, std::int64_t observed) {
        std::lock_guard<std::mutex> lock(historyMutex);
        history.state = state;
        history.projectedThrough = projected;
        history.observedJournalThrough = observed;
        history.lastError.clear();
    }

    void verifyHistoryFiles() {
        verifyFile(historyGuard.get(), canonicalHistory);
        verifyFile(historyGuard.get(), options.historyPath);
        verifyFile(databaseLock.get(), canonicalDatabase);
        verifyFile(databaseLock.get(), options.databasePath);
        if (canonicalFile(options.historyPath) != canonicalHistory || canonicalFile(options.databasePath) != canonicalDatabase)
            throw std::runtime_error("projection configured physical path changed");
        struct stat file{};
        if (lstat(options.historyPath.c_str(), &file) || !S_ISREG(file.st_mode))
            throw std::runtime_error("projection configured physical path replaced");
    }

    template<class Result, class Action> Result historyCall(bool reading, Action action) {
        auto result = std::make_shared<Result>();
        auto failure = std::make_shared<std::exception_ptr>();
        auto job = std::make_shared<Job>();
        job->request.category = reading ? kReadQueue : kMaintenanceQueue;
        job->internal = [action, result, failure](EventStoreDatabase& store) {
            try { *result = action(store); }
            catch (...) { *failure = std::current_exception(); }
        };
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) throw std::runtime_error("projection stopping");
            queues[job->request.category].push_back(job);
        }
        work.notify_all();
        std::unique_lock<std::mutex> lock(mutex);
        work.wait(lock, [&] { return stopping || job->complete.load(std::memory_order_acquire); });
        if (stopping && !job->complete.load(std::memory_order_acquire))
            throw std::runtime_error("projection stopping");
        if (*failure) std::rethrow_exception(*failure);
        if (!job->response.empty()) throw std::runtime_error("projection source job failed: " + job->response);
        return *result;
    }

    void acknowledgeHistory(const std::string& generation, std::int64_t expected, std::int64_t through) {
        historyCall<int>(false, [this, generation, expected, through](EventStoreDatabase& store) {
            // Only filesystem checks and the source CAS run on the writer. No history SQL.
            verifyHistoryFiles();
            store.commitProjectionCursor(generation, expected, through);
            return 0;
        });
    }

    void projectHistory(std::promise<void> initialized) {
        bool announced = false;
        int retryMs = 100;
        std::int64_t verifiedPrefix = 0;
        std::int64_t observedHistoryThrough = 0;
        std::int64_t observedSourceThrough = 0;
        while (!stopping) {
            try {
                verifyHistoryFiles();
                // This connection only supplies startup identity to the projection. All
                // subsequent source pages use the scheduled read worker, also in DELETE mode.
                std::unique_ptr<EventHistoryProjection> ownedProjection;
                {
                    MqttEventOutbox box(canonicalDatabase, options.sqliteLibraryPath, 12, 24, 100, 0,
                        options.profile, MqttEventOutbox::AccessMode::ReadOnly);
                    EventStoreDatabase identityReader(box, options.identity, options.producers, true, options.senders);
                    ownedProjection.reset(new EventHistoryProjection(identityReader, options.historyPath));
                }
                auto& projection = *ownedProjection;
                const auto source = historyCall<EventStoreProjectionCursor>(true,
                    [](EventStoreDatabase& store) { return store.projectionCursor(); });
                const auto historyThrough = projection.watermark();
                if (source.cleanedThrough != 0 || historyThrough > source.journalThrough)
                    throw std::runtime_error("history/source backup mismatch or cleaned journal requires manual recovery");
                // A BUSY retry may reuse only this process's verified immutable prefix.
                // Physical replacement is rejected above; an in-place backup rollback
                // invalidates the prefix even when it retains the same generation.
                if (historyThrough < observedHistoryThrough || source.projectedThrough < observedSourceThrough ||
                    source.journalThrough < verifiedPrefix) verifiedPrefix = 0;
                observedHistoryThrough = historyThrough;
                observedSourceThrough = source.projectedThrough;
                if (!announced) { initialized.set_value(); announced = true; }
                historyState("recovering", source.projectedThrough, source.journalThrough);
                const auto recoverThrough = std::min(source.projectedThrough, historyThrough);
                std::int64_t cursor = std::min(verifiedPrefix, recoverThrough);
                while (!stopping && cursor < recoverThrough) {
                    const auto count = std::min<std::int64_t>(options.historyBatchSize, recoverThrough - cursor);
                    const auto rows = historyCall<std::vector<EventStoreJournalRow>>(true,
                        [cursor, count](EventStoreDatabase& store) { return store.readJournal(cursor, static_cast<std::size_t>(count)); });
                    if (rows.empty()) throw std::runtime_error("journal gap in background recovery");
                    const auto verified = projection.auditRows(rows, cursor);
                    {
                        std::lock_guard<std::mutex> lock(historyMutex);
                        history.auditedRows += static_cast<std::uint64_t>(verified - cursor);
                    }
                    cursor = verified;
                    verifiedPrefix = cursor;
                    if (verified != rows.back().id) break;
                }
                acknowledgeHistory(source.journalGeneration, source.projectedThrough, cursor);
                observedSourceThrough = cursor;
                historyState("running", cursor, source.journalThrough);
                while (!stopping) {
                    verifyHistoryFiles();
                    const auto observedWake = journalWake.load();
                    struct Page { EventStoreProjectionCursor cursor; std::vector<EventStoreJournalRow> rows; };
                    const auto limit = options.historyBatchSize;
                    const auto page = historyCall<Page>(true, [cursor, limit](EventStoreDatabase& store) {
                        return Page{store.projectionCursor(), store.readJournal(cursor, limit)};
                    });
                    if (page.cursor.journalGeneration != source.journalGeneration || page.cursor.projectedThrough != cursor)
                        throw std::runtime_error("source projection cursor changed outside history owner");
                    if (page.rows.empty()) {
                        historyState("idle", cursor, page.cursor.journalThrough);
                        retryMs = 100;
                        std::unique_lock<std::mutex> lock(mutex);
                        work.wait_for(lock, std::chrono::milliseconds(options.historyPollIntervalMs),
                            [&] { return stopping || journalWake.load() != observedWake; });
                        continue;
                    }
                    // The read job has completed and its source statements are finalized.
                    // History BEGIN/COMMIT happens only here, without the DELETE source gate.
                    const auto through = projection.projectRows(page.rows, cursor);
                    verifiedPrefix = std::max(verifiedPrefix, through);
                    observedHistoryThrough = std::max(observedHistoryThrough, through);
                    acknowledgeHistory(source.journalGeneration, cursor, through);
                    cursor = through;
                    observedSourceThrough = cursor;
                    retryMs = 100;
                    historyState("running", cursor, std::max(cursor, page.cursor.journalThrough));
                }
            } catch (const std::exception& ex) {
                const auto* sqlite = dynamic_cast<const SqliteError*>(&ex);
                if (!sqlite || !sqlite->isBusy()) verifiedPrefix = 0;
                if (!announced) {
                    try { initialized.set_exception(std::current_exception()); } catch (...) {}
                    return;
                }
                if (stopping) break;
                {
                    std::lock_guard<std::mutex> lock(historyMutex);
                    history.state = "error";
                    history.lastError = std::string(ex.what()).substr(0, 512);
                    ++history.failures;
                }
                std::unique_lock<std::mutex> lock(mutex);
                work.wait_for(lock, std::chrono::milliseconds(retryMs), [&] { return stopping.load(); });
                retryMs = std::min(2000, retryMs * 2);
            }
        }
        if (!announced) { try { initialized.set_exception(std::make_exception_ptr(std::runtime_error("projection stopped during initialization"))); } catch (...) {} }
        std::lock_guard<std::mutex> lock(historyMutex);
        history.state = "stopped";
    }

    void worker(bool reading, std::promise<void> initialized) {
        try {
            MqttEventOutbox box(canonicalDatabase, options.sqliteLibraryPath, 12, 24, 100, 0,
                options.profile, reading ? MqttEventOutbox::AccessMode::ReadOnly : MqttEventOutbox::AccessMode::ReadWrite);
            EventStoreDatabase store(box, options.identity, options.producers, reading, options.senders);
            store.setDeliveryResponseLimit(options.maxFrameBytes);
            store.setCapacityLimits({options.minFreeBytes, options.maxStoreBytes, options.historyPath});
            initialized.set_value();
            while (auto job = next(reading)) {
                try {
                    verifyFile(databaseLock.get(), canonicalDatabase);
                    const auto& request = job->request;
                    if (job->internal) {
                        job->internal(store);
                    } else if (request.op == "Hello") {
                        const auto settings = box.storageSettings();
                        job->response = "{\"ok\":true,\"laboratoryOnly\":true,\"version\":\"1\",\"backend\":\"ipc\",\"schemaVersion\":\"1\",\"localJournalVersion\":\"1\",\"storeId\":" +
                            quote(options.identity.storeId) + ",\"configGeneration\":" + quote(options.identity.configGeneration) +
                            ",\"sqliteVersion\":" + quote(settings.sqliteVersion) + ",\"journalMode\":" + quote(settings.journalMode) +
                            ",\"synchronous\":\"" + std::to_string(settings.synchronous) + "\",\"storageProfile\":" +
                            quote(options.profile == MqttEventOutbox::StorageProfile::WalFull ? "wal-full" : "delete-full") +
                            ",\"historyProjectionVersion\":\"1\",\"historyEnabled\":" + (options.historyPath.empty() ? "false" : "true") +
                            ",\"capacityAdmissionVersion\":\"1\",\"minFreeBytes\":\"" + std::to_string(options.minFreeBytes) +
                            "\",\"maxStoreBytes\":\"" + std::to_string(options.maxStoreBytes) + "\"}";
                    } else if (request.op == "GetCapacityStatus") {
                        const auto capacity = store.capacityStatus();
                        job->response = "{\"ok\":true,\"blocked\":" + std::string(capacity.blocked ? "true" : "false") +
                            ",\"availableBytes\":\"" + std::to_string(capacity.availableBytes) +
                            "\",\"storeBytes\":\"" + std::to_string(capacity.storeBytes) +
                            "\",\"minFreeBytes\":\"" + std::to_string(options.minFreeBytes) +
                            "\",\"maxStoreBytes\":\"" + std::to_string(options.maxStoreBytes) +
                            "\",\"reason\":" + quote(capacity.reason) + "}";
                    } else if (request.op == "GetHistoryProjectionStatus") {
                        const auto status = historyStatus();
                        job->response = "{\"ok\":true,\"enabled\":" + std::string(status.enabled ? "true" : "false") +
                            ",\"state\":" + quote(status.state) + ",\"projectedThrough\":\"" + std::to_string(status.projectedThrough) +
                            "\",\"observedJournalThrough\":\"" + std::to_string(status.observedJournalThrough) +
                            "\",\"failures\":\"" + std::to_string(status.failures) +
                            "\",\"auditedRows\":\"" + std::to_string(status.auditedRows) + "\",\"lastError\":" + quote(status.lastError) + "}";
                    } else if (request.op == "RegisterProducer") {
                        job->response = producerJson(store.registerProducer(request.producer, request.session, request.epoch));
                    } else if (request.op == "AppendEventsAndStates") {
                        job->response = producerJson(store.append(request.append));
                        if (!request.append.localEvents.empty()) ++journalWake;
                    } else if (request.op == "GetReceipt") {
                        job->response = pendingAppend(request.producer) ?
                            "{\"ok\":true,\"status\":\"IN_PROGRESS\"}" : producerJson(store.receipt(request.producer));
                    } else if (request.op == "RegisterSender") {
                        job->response = store.registerSender(request.sender, request.session, request.epoch);
                    } else if (request.op == "GetSenderScope") {
                        const auto scope = std::find_if(options.senders.begin(), options.senders.end(),
                            [&](const EventStoreSenderConfig& value) { return value.senderId == request.sender; });
                        if (scope == options.senders.end()) throw std::invalid_argument("sender is not configured");
                        std::string types;
                        for (const auto& type : scope->eventTypes) {
                            if (!types.empty()) types += ',';
                            types += quote(type);
                        }
                        job->response = "{\"ok\":true,\"scopeVersion\":\"1\",\"storeId\":" + quote(options.identity.storeId) +
                            ",\"configGeneration\":" + quote(options.identity.configGeneration) +
                            ",\"senderId\":" + quote(scope->senderId) + ",\"targetId\":" + quote(scope->targetId) +
                            ",\"eventTypes\":[" + types + "]}";
                    } else if (request.op == "GetDeliveryReceipt") {
                        job->response = pendingDelivery(request.sender) ?
                            "{\"ok\":true,\"status\":\"IN_PROGRESS\"}" : store.deliveryReceipt(request.sender);
                    } else if (request.op == "ClaimBatch" || request.op == "AckBatch" || request.op == "ReleaseBatch") {
                        job->response = store.deliver(request.delivery);
                    } else if (request.op == "LoadStates") {
                        job->response = statesJson(store.states(request.producer, request.after, request.limit), options.maxFrameBytes);
                    } else if (request.op == "GetStats") {
                        job->response = "{\"ok\":true,\"pendingCount\":\"" + std::to_string(box.pendingCount(request.target)) + "\"}";
                    } else if (request.op == "GetScopedStats") {
                        const auto stats = box.readPendingStats(request.statsScope);
                        const auto settings = box.storageSettings();
                        const bool wal = options.profile == MqttEventOutbox::StorageProfile::WalFull;
                        if (settings.journalMode != (wal ? "wal" : "delete") || settings.synchronous != 2) {
                            throw std::runtime_error("scoped stats storage profile mismatch");
                        }
                        job->response = "{\"ok\":true,\"version\":\"1\",\"schemaVersion\":\"1\","
                            "\"backend\":\"ipc\",\"laboratoryOnly\":true,\"storeId\":" + quote(options.identity.storeId) +
                            ",\"configGeneration\":" + quote(options.identity.configGeneration) +
                            ",\"storageProfile\":" + quote(wal ? "wal-full" : "delete-full") +
                            ",\"journalMode\":" + quote(settings.journalMode) +
                            ",\"synchronous\":\"" + std::to_string(settings.synchronous) + "\"" +
                            ",\"scope\":" + statsScopeJson(request.statsScope) +
                            ",\"pendingCount\":\"" + std::to_string(stats.pendingCount) +
                            "\",\"pendingTextUnits\":\"" + std::to_string(stats.pendingTextUnits) + "\"}";
                    }
                } catch (const EventStoreConflict& ex) {
                    job->response = error(ex.code().c_str(), ex.what(), "not_committed");
                } catch (const std::invalid_argument& ex) {
                    job->response = error("CONFLICT_OR_INVALID", ex.what(), "not_committed");
                } catch (const SqliteError& ex) {
                    job->response = error("SQLITE_ERROR", ex.what(), job->request.op == "GetScopedStats" ?
                        "not_committed" : "unknown_reconcile_receipt");
                } catch (const std::length_error& ex) {
                    job->response = job->request.category == kReadQueue ?
                        error("READ_LIMIT", ex.what(), "not_committed") :
                        error("STORE_ERROR", ex.what(), "unknown_reconcile_receipt");
                } catch (const std::exception& ex) {
                    job->response = error("STORE_ERROR", ex.what(), job->request.op == "GetScopedStats" ?
                        "not_committed" : "unknown_reconcile_receipt");
                }
                if (job->response.size() > options.maxFrameBytes) job->response = error("RESPONSE_TOO_LARGE", "response exceeded limit", "unknown_reconcile_receipt");
                job->complete.store(true, std::memory_order_release);
                if (options.profile == MqttEventOutbox::StorageProfile::DeleteFull) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        deleteSqlActive = false;
                    }
                    work.notify_all();
                }
                work.notify_all();
            }
        } catch (...) {
            try { initialized.set_exception(std::current_exception()); } catch (...) {}
            stopping = true;
            isReady = false;
            work.notify_all();
        }
    }

    void reject(Peer& peer, const char* message) {
        peer.output = frame(error("REJECTED", message, "not_queued"), options.maxFrameBytes);
        peer.input.clear();
        peer.deadline = Clock::now() + std::chrono::milliseconds(options.ioTimeoutMs);
    }

    bool receive(Peer& peer) {
        char data[8192];
        const auto remaining = peer.expected ? peer.expected + 4 - peer.input.size() : 4 - peer.input.size();
        const auto count = recv(peer.fd.get(), data, std::min(sizeof(data), remaining), 0);
        if (count == 0) return false;
        if (count < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        peer.input.append(data, static_cast<std::size_t>(count));
        if (!peer.expected && peer.input.size() == 4) {
            peer.expected = frameLength(peer.input.data());
            if (!peer.expected || peer.expected > options.maxFrameBytes) { reject(peer, "frame length out of range"); return true; }
            // Includes decoded objects, SQLite request copies, output and framing buffers.
            const auto charge = peer.expected * 32 + options.maxFrameBytes * 2 + 65536;
            peer.reservation = reserve(charge);
            if (!peer.reservation) { reject(peer, "bounded event store queue is full"); return true; }
            peer.input.reserve(peer.expected + 4);
        }
        if (peer.expected && peer.input.size() == peer.expected + 4) {
            try {
                auto job = std::make_shared<Job>();
                job->request = decode(peer.input.substr(4), options);
                job->reservation = std::move(peer.reservation);
                peer.input.clear();
                peer.input.shrink_to_fit();
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    queues[job->request.category].push_back(job);
                }
                peer.job = std::move(job);
                peer.deadline = Clock::now() + std::chrono::milliseconds(options.ioTimeoutMs);
                work.notify_all();
            } catch (const std::exception& ex) { reject(peer, ex.what()); }
        }
        return true;
    }

    void ioLoop() {
        std::vector<Peer> peers;
        try {
            while (!stopping) {
                std::vector<pollfd> sockets;
                sockets.push_back({listener.get(), POLLIN, 0});
                for (auto& peer : peers) {
                    if (peer.job && peer.job->complete.load(std::memory_order_acquire) && peer.output.empty()) {
                        peer.output = frame(peer.job->response, options.maxFrameBytes);
                        peer.deadline = Clock::now() + std::chrono::milliseconds(options.ioTimeoutMs);
                    }
                    sockets.push_back({peer.fd.get(), static_cast<short>(peer.output.empty() ? (peer.job ? 0 : POLLIN) : POLLOUT), 0});
                }
                const int rc = poll(sockets.data(), sockets.size(), 10);
                if (rc < 0) { if (errno == EINTR) continue; throw std::runtime_error("event store poll failed"); }
                for (std::size_t i = peers.size(); i > 0; --i) {
                    auto& peer = peers[i - 1];
                    const auto revents = sockets[i].revents;
                    bool keep = !(revents & (POLLERR | POLLNVAL)) && Clock::now() < peer.deadline;
                    if ((revents & POLLHUP) && !(revents & POLLIN)) keep = false;
                    if (keep && (revents & POLLIN) && !peer.job && peer.output.empty()) keep = receive(peer);
                    if (keep && (revents & POLLOUT) && !peer.output.empty()) {
                        const auto sent = send(peer.fd.get(), peer.output.data() + peer.sent, peer.output.size() - peer.sent, MSG_NOSIGNAL);
                        if (sent > 0) peer.sent += static_cast<std::size_t>(sent);
                        else if (sent == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) keep = false;
                        if (peer.sent == peer.output.size()) keep = false;
                    }
                    if (!keep) peers.erase(peers.begin() + static_cast<std::ptrdiff_t>(i - 1));
                }
                if (sockets[0].revents & POLLIN) {
                    for (int attempt = 0; attempt < 16; ++attempt) {
                        const int fd = accept4(listener.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                        if (fd < 0) break;
                        struct ucred credentials{};
                        socklen_t size = sizeof(credentials);
                        if (peers.size() >= options.maxPeers || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 ||
                            credentials.uid != geteuid()) { close(fd); continue; }
                        peers.emplace_back(fd, options.ioTimeoutMs);
                    }
                }
            }
        } catch (...) { stopping = true; }
        isReady = false;
        work.notify_all();
    }

    void stop() {
        stopping = true;
        isReady = false;
        work.notify_all();
        if (io.joinable()) io.join();
        if (writer.joinable()) writer.join();
        if (reader.joinable()) reader.join();
        if (historyWorker.joinable()) historyWorker.join();
        // Destroy reservations without holding the mutex their deleters acquire.
        for (auto& queue : queues) queue.clear();
        historyReservation.reset();
        deleteSqlActive = false;
        deleteReaderTurn = false;
        listener = Fd();
        if (ownsSocket) {
            struct stat current{};
            if (lstat(options.socketPath.c_str(), &current) == 0 && current.st_dev == ownedSocket.st_dev &&
                current.st_ino == ownedSocket.st_ino) unlink(options.socketPath.c_str());
            ownsSocket = false;
        }
        socketLock = Fd();
        databaseLock = Fd();
        historyGuard = Fd();
    }

    void start() {
        if (writer.joinable() || io.joinable()) throw std::logic_error("event store already started");
        if (options.profile != MqttEventOutbox::StorageProfile::DeleteFull &&
            options.profile != MqttEventOutbox::StorageProfile::WalFull) {
            throw std::invalid_argument("laboratory event store requires FULL durability");
        }
        if (options.maxPeers == 0 || options.maxPeers > 16 || options.maxFrameBytes < 4096 || options.maxFrameBytes > 256 * 1024 ||
            options.memoryBudgetBytes < options.maxFrameBytes * 34 + 65536 || options.memoryBudgetBytes > 64 * 1024 * 1024 ||
            options.ioTimeoutMs < 100 || options.ioTimeoutMs > 30000) throw std::invalid_argument("invalid bounded event store options");
        constexpr std::size_t historyBudget = 4 * 1024 * 1024;
        if (options.historyBatchSize == 0 || options.historyBatchSize > 64 ||
            options.historyPollIntervalMs < 10 || options.historyPollIntervalMs > 5000)
            throw std::invalid_argument("historyBatchSize must be 1..64 and historyPollIntervalMs 10..5000");
        if (!options.historyPath.empty() && options.memoryBudgetBytes < options.maxFrameBytes * 34 + 65536 + historyBudget)
            throw std::invalid_argument("history projection requires an additional 4 MiB memory reservation");
        stopping = false;
        const auto endpoint = address(options.socketPath);
        if (options.databasePath.empty() || options.databasePath.front() != '/' || options.databasePath.find('\0') != std::string::npos) {
            throw std::invalid_argument("event store database must use an absolute path");
        }
        try {
            {
                std::lock_guard<std::mutex> lock(historyMutex);
                history = EventStoreHistoryStatus{};
                history.enabled = !options.historyPath.empty();
                history.state = history.enabled ? "starting" : "disabled";
            }
            if (!options.historyPath.empty()) validateHistoryPaths(options);
            databaseLock = Fd(open(options.databasePath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
            if (databaseLock.get() < 0) throw std::runtime_error("cannot open event store lock file");
            canonicalDatabase = canonicalFile(options.databasePath);
            verifyFile(databaseLock.get(), canonicalDatabase);
            if (flock(databaseLock.get(), LOCK_EX | LOCK_NB) != 0) throw std::runtime_error("event store database already has a writer");
            if (!options.historyPath.empty()) {
                canonicalHistory = canonicalFutureFile(options.historyPath);
                historyGuard = Fd(open(canonicalHistory.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
                if (historyGuard.get() < 0) throw std::runtime_error("cannot open history physical identity guard");
                verifyHistoryFiles();
                historyReservation = reserve(historyBudget);
                if (!historyReservation) throw std::runtime_error("cannot reserve bounded history workspace");
            }
            const auto socketLockPath = options.socketPath + ".lock";
            socketLock = Fd(open(socketLockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
            if (socketLock.get() < 0) throw std::runtime_error("cannot open event store endpoint lock");
            verifyFile(socketLock.get(), socketLockPath);
            if (flock(socketLock.get(), LOCK_EX | LOCK_NB) != 0) throw std::runtime_error("event store endpoint already in use");
            struct stat dbStat{};
            if (fstat(databaseLock.get(), &dbStat) != 0) throw std::runtime_error("cannot stat event store database");
            if (dbStat.st_size > 0) {
                // Validate the laboratory stamp before a writer can issue DDL or PRAGMA journal_mode.
                MqttEventOutbox probe(canonicalDatabase, options.sqliteLibraryPath, 12, 24, 100, 0,
                    options.profile, MqttEventOutbox::AccessMode::ReadOnly);
                EventStoreDatabase existing(probe, options.identity, options.producers, true, options.senders);
            }
            std::promise<void> initialized;
            auto wait = initialized.get_future();
            writer = std::thread([this, initialized = std::move(initialized)]() mutable { worker(false, std::move(initialized)); });
            wait.get();
            std::promise<void> readInitialized;
            auto readWait = readInitialized.get_future();
            reader = std::thread([this, initialized = std::move(readInitialized)]() mutable { worker(true, std::move(initialized)); });
            readWait.get();
            if (!options.historyPath.empty()) {
                std::promise<void> historyInitialized;
                auto historyWait = historyInitialized.get_future();
                historyWorker = std::thread([this, initialized = std::move(historyInitialized)]() mutable {
                    projectHistory(std::move(initialized));
                });
                historyWait.get();
            }
            struct stat socketStat{};
            if (lstat(options.socketPath.c_str(), &socketStat) == 0) {
                if (!S_ISSOCK(socketStat.st_mode) || socketStat.st_uid != geteuid()) throw std::runtime_error("refusing to remove non-owned/non-socket endpoint");
                Fd probe(socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
                if (probe.get() < 0) throw std::runtime_error("cannot probe old endpoint");
                const int rc = connect(probe.get(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint));
                if (rc == 0 || errno != ECONNREFUSED) throw std::runtime_error("existing endpoint may still be active");
                if (unlink(options.socketPath.c_str()) != 0) throw std::runtime_error("cannot remove stale endpoint");
            } else if (errno != ENOENT) throw std::runtime_error("cannot inspect event store endpoint");
            listener = Fd(socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
            if (listener.get() < 0 || bind(listener.get(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
                throw std::runtime_error("cannot bind event store endpoint");
            }
            ownsSocket = true;
            if (lstat(options.socketPath.c_str(), &ownedSocket) != 0) throw std::runtime_error("cannot identify bound event store socket");
            if (chmod(options.socketPath.c_str(), 0660) != 0 || listen(listener.get(), 16) != 0) throw std::runtime_error("cannot listen on event store endpoint");
            isReady = true;
            io = std::thread([this] { ioLoop(); });
        } catch (...) { stop(); throw; }
    }
};

EventStoreRuntime::EventStoreRuntime(EventStoreRuntimeOptions options) : impl_(new Impl(std::move(options))) {}
EventStoreRuntime::~EventStoreRuntime() { impl_->stop(); }
void EventStoreRuntime::start() { impl_->start(); }
void EventStoreRuntime::stop() { impl_->stop(); }
bool EventStoreRuntime::ready() const { return impl_->isReady; }
EventStoreHistoryStatus EventStoreRuntime::historyStatus() const { return impl_->historyStatus(); }

void validateEventStoreRequest(const std::string& request, const EventStoreRuntimeOptions& options) {
    if (request.empty() || request.size() > options.maxFrameBytes) throw std::length_error("event store frame limit");
    (void)decode(request, options);
}

std::string callEventStore(const std::string& socketPath, const std::string& request, int timeoutMs, std::size_t maxFrameBytes) {
    if (timeoutMs < 1 || timeoutMs > 30000 || maxFrameBytes > 256 * 1024) throw std::invalid_argument("invalid event store call bounds");
    const auto endpoint = address(socketPath);
    const auto outgoing = frame(request, maxFrameBytes);
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    Fd fd(socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    bool unknown = false;
    const auto fail = [&](const char* message) { throw EventStoreTransportError(message, unknown); };
    if (fd.get() < 0) fail("cannot create event store socket");
    const auto waitFor = [&](short events) {
        for (;;) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (remaining <= 0) fail("event store absolute deadline exceeded; reconcile any submitted request");
            pollfd item{fd.get(), events, 0};
            const int rc = poll(&item, 1, static_cast<int>(remaining));
            if (rc < 0 && errno == EINTR) continue;
            if (rc <= 0 || (item.revents & POLLNVAL)) fail("event store connection failed or timed out");
            // A rejected frame can leave unread request bytes: consume the buffered reply before RST/HUP.
            if (item.revents & events) return;
            if (item.revents & POLLERR) fail("event store connection failed");
            if (item.revents & POLLHUP) fail("event store disconnected; reconcile any submitted request");
        }
    };
    if (connect(fd.get(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
        if (errno != EINPROGRESS && errno != EAGAIN) fail("event store unavailable; local SQL fallback is forbidden");
        waitFor(POLLOUT);
        int result = 0;
        socklen_t size = sizeof(result);
        if (getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &result, &size) != 0 || result != 0) fail("event store connect failed");
    }
    struct ucred peer{};
    socklen_t peerSize = sizeof(peer);
    if (getsockopt(fd.get(), SOL_SOCKET, SO_PEERCRED, &peer, &peerSize) != 0 ||
        peerSize != sizeof(peer) || (peer.uid != geteuid() && peer.uid != 0)) {
        fail("event store peer identity is not authorized");
    }
    for (std::size_t sent = 0; sent < outgoing.size();) {
        waitFor(POLLOUT);
        const auto count = send(fd.get(), outgoing.data() + sent, outgoing.size() - sent, MSG_NOSIGNAL);
        if (count > 0) { unknown = true; sent += static_cast<std::size_t>(count); }
        else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) fail("event store send failed");
    }
    std::string response;
    std::size_t wanted = 4;
    while (response.size() < wanted) {
        waitFor(POLLIN);
        char buffer[8192];
        const auto count = recv(fd.get(), buffer, std::min(sizeof(buffer), wanted - response.size()), 0);
        if (count > 0) response.append(buffer, static_cast<std::size_t>(count));
        else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) fail("event store response lost; reconcile receipt");
        if (response.size() == 4 && wanted == 4) {
            const auto length = frameLength(response.data());
            if (!length || length > maxFrameBytes) fail("event store response frame too large");
            wanted += length;
        }
    }
    return response.substr(4);
}

EventStoreRuntimeOptions loadEventStoreLabConfig(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open laboratory event store config");
    std::string text(65537, '\0');
    file.read(&text[0], static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(file.gcount()));
    if (text.size() > 65536) throw std::invalid_argument("laboratory config too large");
    const auto root = json::JsonParser(text, 8, 2048).parse();
    fields(root, {"laboratoryOnly", "storeId", "configGeneration", "databasePath", "socketPath", "sqliteLibraryPath", "storageProfile", "producers", "senders",
        "historyPath", "historyBatchSize", "historyPollIntervalMs", "minFreeBytes", "maxStoreBytes"});
    if (!required(root, "laboratoryOnly").asBool()) throw std::invalid_argument("P1a is laboratory-only; production backend is not available");
    EventStoreRuntimeOptions options;
    options.identity.storeId = string(root, "storeId", 96);
    options.identity.configGeneration = string(root, "configGeneration", 96);
    options.databasePath = string(root, "databasePath", 4096);
    options.socketPath = string(root, "socketPath", 100);
    options.sqliteLibraryPath = string(root, "sqliteLibraryPath", 4096);
    if (root.find("historyPath")) options.historyPath = string(root, "historyPath", 4096);
    const auto configInteger = [&](const char* key, int fallback, int minimum, int maximum) {
        const auto* field = root.find(key);
        if (!field) return fallback;
        const auto value = field->asNumber();
        if (!std::isfinite(value) || value < minimum || value > maximum || std::floor(value) != value)
            throw std::invalid_argument(std::string("invalid bounded config integer: ") + key);
        return static_cast<int>(value);
    };
    options.historyBatchSize = static_cast<std::size_t>(configInteger("historyBatchSize", 32, 1, 64));
    options.historyPollIntervalMs = configInteger("historyPollIntervalMs", 200, 10, 5000);
    const auto configBytes = [&](const char* key) -> std::uint64_t {
        const auto* field = root.find(key);
        if (!field) return 0;
        const auto value = field->asNumber();
        if (!std::isfinite(value) || value < 0 || value > 9007199254740991.0 || std::floor(value) != value)
            throw std::invalid_argument(std::string("invalid exact byte count: ") + key);
        return static_cast<std::uint64_t>(value);
    };
    options.minFreeBytes = configBytes("minFreeBytes");
    options.maxStoreBytes = configBytes("maxStoreBytes");
    const auto profile = string(root, "storageProfile", 32);
    if (profile == "delete-full") options.profile = MqttEventOutbox::StorageProfile::DeleteFull;
    else if (profile == "wal-full") options.profile = MqttEventOutbox::StorageProfile::WalFull;
    else throw std::invalid_argument("laboratory event store requires explicit delete-full or wal-full");
    for (const auto& item : required(root, "producers").asArray().values) options.producers.push_back(item->asString());
    if (const auto* senders = root.find("senders")) {
        const auto& entries = senders->asArray().values;
        if (entries.size() > 64) throw std::invalid_argument("at most 64 senders are allowed");
        for (const auto& item : entries) {
            fields(*item, {"senderId", "targetId", "eventTypes"});
            EventStoreSenderConfig sender;
            sender.senderId = nonemptyString(*item, "senderId", 96);
            sender.targetId = nonemptyString(*item, "targetId", 96);
            const auto& types = required(*item, "eventTypes").asArray().values;
            if (types.size() > 16) throw std::invalid_argument("sender allows at most 16 eventTypes");
            for (const auto& type : types) {
                nonemptyText(type->asString(), 96);
                sender.eventTypes.push_back(type->asString());
            }
            options.senders.push_back(std::move(sender));
        }
    }
    return options;
}

}  // namespace edge_gateway
