#include "edge_gateway/event_stats_reader.hpp"
#include "edge_gateway/event_store_runtime.hpp"
#include "edge_gateway/json_value.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace edge_gateway {
namespace {
using Json = json::JsonValue;
using Clock = std::chrono::steady_clock;
using Profile = MqttEventOutbox::StorageProfile;

void bounded(const std::string& value, std::size_t max, bool empty = false) {
    if ((!empty && value.empty()) || value.size() > max || value.find('\0') != std::string::npos) {
        throw std::invalid_argument("invalid bounded event statistics text");
    }
}

void profile(Profile value) {
    if (value != Profile::DeleteFull && value != Profile::WalFull &&
        value != Profile::DeleteNormal && value != Profile::WalNormal) {
        throw std::invalid_argument("invalid statistics storage profile");
    }
}

void validate(const EventStatsReaderOptions& options) {
    if (const auto* legacy = options.legacy()) {
        bounded(legacy->databasePath, 4096);
        bounded(legacy->sqliteLibraryPath, 4096, true);
        if (legacy->databasePath.front() != '/') throw std::invalid_argument("legacy statistics require an absolute database path");
        profile(legacy->profile);
        return;
    }
    const auto& ipc = *options.ipc();
    bounded(ipc.identity.storeId, 96); bounded(ipc.identity.configGeneration, 96);
    bounded(ipc.socketPath, sizeof(sockaddr_un::sun_path) - 1);
    if (ipc.socketPath.front() != '/' || ipc.socketPath.back() == '/' ||
        ipc.timeoutMs < 1 || ipc.timeoutMs > 30000 || ipc.maxFrameBytes < 4096 || ipc.maxFrameBytes > 256 * 1024 ||
        (ipc.profile != Profile::DeleteFull && ipc.profile != Profile::WalFull)) {
        throw std::invalid_argument("invalid IPC statistics endpoint/budget/FULL profile");
    }
}

class Owner {
public:
    void check() const {
        if (pid_ != getpid() || thread_ != std::this_thread::get_id()) {
            throw std::logic_error("event statistics reader cannot cross fork/thread ownership");
        }
    }
private:
    pid_t pid_ = getpid();
    std::thread::id thread_ = std::this_thread::get_id();
};

const Json& field(const Json& root, const char* key) {
    const auto* value = root.find(key);
    if (!value) throw std::invalid_argument(std::string("missing statistics response field: ") + key);
    return *value;
}

void fields(const Json& root, std::initializer_list<const char*> allowed) {
    std::set<std::string> seen;
    for (const auto& value : root.asObject().values) {
        if (!seen.insert(value.key).second || std::none_of(allowed.begin(), allowed.end(),
                [&](const char* key) { return value.key == key; })) {
            throw std::invalid_argument("duplicate/unknown statistics response field");
        }
    }
}

void uniqueKeys(const Json& root) {
    if (root.isArray()) {
        for (const auto& value : root.asArray().values) uniqueKeys(*value);
    } else if (root.isObject()) {
        std::set<std::string> keys;
        for (const auto& value : root.asObject().values) {
            if (!keys.insert(value.key).second) throw std::invalid_argument("duplicate statistics response key");
            uniqueKeys(*value.value);
        }
    }
}

std::int64_t integer(const Json& root, const char* key) {
    const auto& text = field(root, key).asString();
    bounded(text, 20);
    std::size_t consumed = 0;
    const auto value = std::stoll(text, &consumed);
    if (value < 0 || consumed != text.size() || std::to_string(value) != text) {
        throw std::invalid_argument("statistics integer must be nonnegative canonical int64 text");
    }
    return value;
}

std::string quote(const std::string& text) {
    std::string result = "\"";
    static const char hex[] = "0123456789abcdef";
    for (unsigned char ch : text) {
        if (ch == '"' || ch == '\\') { result += '\\'; result += static_cast<char>(ch); }
        else if (ch < 32) { result += "\\u00"; result += hex[ch >> 4]; result += hex[ch & 15]; }
        else result += static_cast<char>(ch);
    }
    return result + '"';
}

std::string array(const std::vector<std::string>& values) {
    std::string result = "[";
    for (const auto& value : values) {
        if (result.size() > 1) result += ',';
        result += quote(value);
    }
    return result + ']';
}

std::vector<std::string> strings(const Json& root, const char* key) {
    const auto& list = field(root, key).asArray().values;
    if (list.size() > 32) throw std::invalid_argument("statistics response type limit");
    std::vector<std::string> result;
    for (const auto& item : list) result.push_back(item->asString());
    return result;
}

class LegacyReader final : public EventStatsReader {
public:
    explicit LegacyReader(LegacyEventStatsOptions options) : options_(std::move(options)) {}
    EventPendingStats read(const EventStatsScope& scope) override {
        owner_.check();
        const auto normalized = normalizeEventStatsScope(scope);
        // A per-call read-only connection cannot silently keep serving an old replaced file.
        MqttEventOutbox box(options_.databasePath, options_.sqliteLibraryPath, 1, 1, 1, 0,
            options_.profile, MqttEventOutbox::AccessMode::ReadOnly);
        const auto mode = box.storageSettings().journalMode;
        const bool wal = options_.profile == Profile::WalFull || options_.profile == Profile::WalNormal;
        if (mode != (wal ? "wal" : "delete")) throw std::runtime_error("legacy statistics journal mode mismatch");
        return box.readPendingStats(normalized);
    }
private:
    LegacyEventStatsOptions options_;
    Owner owner_;
};

class IpcReader final : public EventStatsReader {
public:
    explicit IpcReader(IpcEventStatsOptions options) : options_(std::move(options)) {}
    EventPendingStats read(const EventStatsScope& scope) override {
        owner_.check();
        const auto normalized = normalizeEventStatsScope(scope);
        const auto deadline = Clock::now() + std::chrono::milliseconds(options_.timeoutMs);
        struct stat info{};
        if (lstat(options_.socketPath.c_str(), &info) != 0 || !S_ISSOCK(info.st_mode)) {
            throw std::runtime_error("statistics endpoint is unavailable or not a direct socket");
        }
        const auto hello = call("Hello", "{}", deadline);
        fields(hello, {"ok", "version", "schemaVersion", "backend", "laboratoryOnly", "storeId",
            "configGeneration", "storageProfile", "journalMode", "synchronous", "sqliteVersion",
            "localJournalVersion", "historyProjectionVersion", "historyEnabled",
            "capacityAdmissionVersion", "minFreeBytes", "maxStoreBytes"});
        identity(hello);
        for (const auto* capability : {"localJournalVersion", "historyProjectionVersion", "capacityAdmissionVersion"}) {
            if (hello.find(capability) && field(hello, capability).asString() != "1")
                throw std::invalid_argument("unsupported statistics Hello capability version");
        }
        if (hello.find("historyEnabled")) (void)field(hello, "historyEnabled").asBool();
        for (const auto* bound : {"minFreeBytes", "maxStoreBytes"})
            if (hello.find(bound)) (void)integer(hello, bound);
        const auto args = "{\"targetId\":" + quote(normalized.targetId) + ",\"selection\":" +
            quote(normalized.selection == EventStatsSelection::All ? "all" : "only") +
            ",\"include\":" + array(normalized.include) + ",\"exclude\":" + array(normalized.exclude) + "}";
        const auto response = call("GetScopedStats", args, deadline);
        fields(response, {"ok", "version", "schemaVersion", "backend", "laboratoryOnly", "storeId",
            "configGeneration", "storageProfile", "journalMode", "synchronous", "scope", "pendingCount", "pendingTextUnits"});
        identity(response);
        const auto& echo = field(response, "scope");
        fields(echo, {"targetId", "selection", "include", "exclude"});
        EventStatsScope actual;
        actual.targetId = field(echo, "targetId").asString();
        const auto selection = field(echo, "selection").asString();
        if (selection != "all" && selection != "only") throw std::invalid_argument("invalid statistics selection");
        actual.selection = selection == "all" ? EventStatsSelection::All : EventStatsSelection::Only;
        actual.include = strings(echo, "include"); actual.exclude = strings(echo, "exclude");
        const auto canonical = normalizeEventStatsScope(actual);
        if (!sameEventStatsScope(canonical, normalized) || actual.include != canonical.include || actual.exclude != canonical.exclude) {
            throw std::invalid_argument("statistics response scope mismatch/noncanonical scope");
        }
        EventPendingStats result{integer(response, "pendingCount"), integer(response, "pendingTextUnits")};
        if ((result.pendingCount == 0 && result.pendingTextUnits != 0) ||
            (normalized.selection == EventStatsSelection::Only && normalized.include.empty() && result.pendingCount != 0)) {
            throw std::invalid_argument("inconsistent empty statistics result");
        }
        return result;
    }
private:
    IpcEventStatsOptions options_;
    Owner owner_;

    Json call(const std::string& op, const std::string& args, Clock::time_point deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining < 1) throw std::runtime_error("statistics total deadline exhausted");
        const auto request = "{\"version\":\"1\",\"storeId\":" + quote(options_.identity.storeId) +
            ",\"configGeneration\":" + quote(options_.identity.configGeneration) + ",\"op\":" + quote(op) + ",\"args\":" + args + "}";
        EventStoreRuntimeOptions validation;
        validation.identity = options_.identity; validation.maxFrameBytes = options_.maxFrameBytes;
        validateEventStoreRequest(request, validation);
        const auto response = callEventStore(options_.socketPath, request, static_cast<int>(remaining), options_.maxFrameBytes);
        auto root = json::JsonParser(response, 16, 4096).parse();
        uniqueKeys(root);
        if (!field(root, "ok").asBool()) {
            throw std::runtime_error("statistics request rejected: " + field(root, "code").asString());
        }
        return root;
    }

    void identity(const Json& root) {
        const bool wal = options_.profile == Profile::WalFull;
        if (field(root, "version").asString() != "1" || field(root, "schemaVersion").asString() != "1" ||
            field(root, "backend").asString() != "ipc" || !field(root, "laboratoryOnly").asBool() ||
            field(root, "storeId").asString() != options_.identity.storeId ||
            field(root, "configGeneration").asString() != options_.identity.configGeneration ||
            integer(root, "synchronous") != 2 || field(root, "journalMode").asString() != (wal ? "wal" : "delete") ||
            field(root, "storageProfile").asString() != (wal ? "wal-full" : "delete-full")) {
            throw std::runtime_error("statistics identity/backend/FULL profile mismatch");
        }
    }
};
} // namespace

std::unique_ptr<EventStatsReader> makeEventStatsReader(const EventStatsReaderOptions& options) {
    validate(options);
    if (const auto* legacy = options.legacy()) {
        return std::make_unique<LegacyReader>(*legacy);
    }
    return std::make_unique<IpcReader>(*options.ipc());
}

struct EventStatsCache::Impl {
    explicit Impl(EventStatsCacheOptions value) : options(std::move(value)) {
        validate(options.reader);
        if (options.queries.empty() || options.queries.size() > 16 || options.pollIntervalMs < 10 ||
            options.pollIntervalMs > 3600000 || options.staleAfterMs < 1 || options.staleAfterMs > 3600000) {
            throw std::invalid_argument("invalid statistics cache bounds");
        }
        std::set<std::string> keys;
        for (auto& query : options.queries) {
            bounded(query.key, 96); bounded(query.context, 256, true);
            if (!keys.insert(query.key).second) throw std::invalid_argument("duplicate statistics query key");
            query.scope = normalizeEventStatsScope(std::move(query.scope));
            EventStatsCacheEntry entry;
            entry.query = query;
            if (const auto* ipc = options.reader.ipc()) {
                entry.backend = EventStatsBackend::Ipc; entry.identity = ipc->identity;
            }
            entries.push_back(std::move(entry));
        }
        sampled.resize(entries.size());
    }

    EventStatsCacheOptions options;
    const pid_t pid = getpid();
    mutable std::mutex mutex;
    std::mutex lifecycle;
    std::condition_variable condition;
    std::thread worker;
    bool started = false, stopping = false;
    std::vector<EventStatsCacheEntry> entries;
    std::vector<Clock::time_point> sampled;

    void process() const {
        if (pid != getpid()) throw std::logic_error("statistics cache cannot be used after fork");
    }

    void run() {
        // Both backends are constructed, used and destroyed on this worker only.
        const auto reader = makeEventStatsReader(options.reader);
        for (;;) {
            for (std::size_t i = 0; i < entries.size(); ++i) {
                {
                    std::lock_guard<std::mutex> guard(mutex);
                    if (stopping) return;
                }
                EventPendingStats value;
                std::string error;
                bool succeeded = false;
                try { value = reader->read(options.queries[i].scope); succeeded = true; }
                catch (const std::exception& ex) {
                    error = std::string(ex.what()).substr(0, 512);
                    if (error.empty()) error = "statistics read failed";
                }
                catch (...) { error = "unknown statistics read failure"; }
                const auto now = Clock::now();
                std::lock_guard<std::mutex> guard(mutex);
                if (stopping) return;
                auto& entry = entries[i];
                entry.error = error;
                if (succeeded) {
                    entry.value = value; entry.hasValue = true; entry.status = EventStatsStatus::Fresh;
                    entry.sampledAtUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    sampled[i] = now;
                } else entry.status = EventStatsStatus::Error;
            }
            std::unique_lock<std::mutex> lock(mutex);
            if (condition.wait_for(lock, std::chrono::milliseconds(options.pollIntervalMs), [&] { return stopping; })) return;
        }
    }

    void start() {
        process();
        std::lock_guard<std::mutex> serial(lifecycle);
        std::lock_guard<std::mutex> guard(mutex);
        if (started || stopping) throw std::logic_error("statistics cache start requires a new instance");
        worker = std::thread([this] {
            try { run(); }
            catch (const std::exception& ex) {
                std::lock_guard<std::mutex> guard(mutex);
                for (auto& entry : entries) { entry.status = EventStatsStatus::Error; entry.error = std::string(ex.what()).substr(0, 512); }
            }
            catch (...) {
                std::lock_guard<std::mutex> guard(mutex);
                for (auto& entry : entries) { entry.status = EventStatsStatus::Error; entry.error = "statistics worker failed"; }
            }
        });
        started = true;
    }

    void stop() {
        process();
        std::lock_guard<std::mutex> serial(lifecycle);
        {
            std::lock_guard<std::mutex> guard(mutex);
            stopping = true;
        }
        condition.notify_all();
        if (worker.joinable()) worker.join();
    }
};

EventStatsCache::EventStatsCache(EventStatsCacheOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
EventStatsCache::~EventStatsCache() { impl_->stop(); }
void EventStatsCache::start() { impl_->start(); }
void EventStatsCache::stop() { impl_->stop(); }
EventStatsCacheEntry EventStatsCache::snapshot(const std::string& key) const {
    impl_->process();
    std::lock_guard<std::mutex> guard(impl_->mutex);
    for (std::size_t i = 0; i < impl_->entries.size(); ++i) {
        if (impl_->entries[i].query.key != key) continue;
        auto result = impl_->entries[i];
        if (result.hasValue) result.ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - impl_->sampled[i]).count();
        if (impl_->stopping) result.status = EventStatsStatus::Stopped;
        else if (result.status == EventStatsStatus::Fresh && result.ageMs > impl_->options.staleAfterMs) result.status = EventStatsStatus::Stale;
        result.valid = result.status == EventStatsStatus::Fresh && result.hasValue;
        return result;
    }
    throw std::out_of_range("unknown statistics cache query key");
}

} // namespace edge_gateway
