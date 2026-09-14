#include "edge_gateway/event_store_client.hpp"
#include "edge_gateway/event_store_runtime.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace edge_gateway {
namespace {
using Json = json::JsonValue;
using Clock = std::chrono::steady_clock;

const Json& field(const Json& root, const char* key) {
    const auto* value = root.find(key);
    if (!value) throw std::invalid_argument(std::string("missing event store response field: ") + key);
    return *value;
}

std::int64_t integer(const Json& root, const char* key, std::int64_t minimum = 0) {
    const auto& value = field(root, key).asString();
    if (value.empty() || value.size() > 20) throw std::invalid_argument("invalid event store integer");
    std::size_t consumed = 0;
    const auto result = std::stoll(value, &consumed);
    if (consumed != value.size() || std::to_string(result) != value || result < minimum) {
        throw std::invalid_argument("noncanonical event store integer");
    }
    return result;
}

void identifier(const std::string& value) {
    if (value.empty() || value.size() > 96 || !std::all_of(value.begin(), value.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '.' || c == ':' || c == '-' || c == '_';
        })) throw std::invalid_argument("invalid event store client identifier");
}

std::string quote(const std::string& text) {
    std::string result = "\"";
    static const char hex[] = "0123456789abcdef";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += static_cast<char>(c);
    }
    return result + '"';
}

std::string serialize(const Json& value, std::size_t limit) {
    std::string result;
    std::size_t nodes = 0;
    const auto add = [&](const std::string& part) {
        if (part.size() > limit - result.size()) throw std::length_error("event store client frame limit");
        result += part;
    };
    std::function<void(const Json&, std::size_t)> visit = [&](const Json& item, std::size_t depth) {
        if (depth > 16 || ++nodes > 4096) throw std::length_error("event store client JSON limit");
        if (item.isNull()) add("null");
        else if (item.isBool()) add(item.asBool() ? "true" : "false");
        else if (item.isString()) {
            if (item.asString().size() > limit) throw std::length_error("event store client string limit");
            add(quote(item.asString()));
        } else if (item.isNumber()) {
            if (!std::isfinite(item.asNumber())) throw std::invalid_argument("nonfinite event store value");
            std::ostringstream number;
            number.imbue(std::locale::classic());
            number << std::setprecision(17) << item.asNumber();
            add(number.str());
        } else if (item.isArray()) {
            add("[");
            bool first = true;
            for (const auto& child : item.asArray().values) {
                if (!child) throw std::invalid_argument("null JSON array member");
                if (!first) add(",");
                first = false;
                visit(*child, depth + 1);
            }
            add("]");
        } else {
            add("{");
            std::set<std::string> keys;
            for (const auto& child : item.asObject().values) {
                if (child.key.size() > limit || !child.value || !keys.insert(child.key).second) {
                    throw std::invalid_argument("duplicate or invalid JSON object member");
                }
                if (keys.size() > 1) add(",");
                add(quote(child.key)); add(":"); visit(*child.value, depth + 1);
            }
            add("}");
        }
    };
    visit(value, 0);
    return result;
}

std::string hexName(const std::string& value) {
    static const char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned char c : value) { result += hex[c >> 4]; result += hex[c & 15]; }
    return result;
}

class Descriptor {
public:
    explicit Descriptor(int fd = -1) : fd_(fd) {}
    ~Descriptor() { if (fd_ >= 0) ::close(fd_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return fd_; }
    void reset(int fd) { if (fd_ >= 0) ::close(fd_); fd_ = fd; }
private:
    int fd_;
};

int lockDirectory(int parent, const std::string& name) {
    if (mkdirat(parent, name.c_str(), 0700) != 0 && errno != EEXIST) {
        throw std::runtime_error("cannot create event store client lock directory");
    }
    const int fd = openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat info{};
    if (fd < 0 || fstat(fd, &info) != 0 || info.st_uid != geteuid() || (info.st_mode & 0022)) {
        if (fd >= 0) ::close(fd);
        throw std::runtime_error("unsafe event store client lock directory");
    }
    return fd;
}
} // namespace

struct EventStoreClient::Impl {
    explicit Impl(EventStoreClientOptions value) : options(std::move(value)), pid(getpid()), thread(std::this_thread::get_id()) {
        identifier(options.identity.storeId); identifier(options.identity.configGeneration); identifier(options.actorId);
        if (options.expectedSenderTargetId.empty() != options.expectedSenderEventTypes.empty() ||
            (producer() && !options.expectedSenderTargetId.empty()))
            throw std::invalid_argument("invalid expected sender scope");
        if (!options.expectedSenderTargetId.empty()) {
            identifier(options.expectedSenderTargetId);
            std::set<std::string> types;
            for (const auto& type : options.expectedSenderEventTypes) {
                if (type.empty() || type.size() > 64 || type.find('\0') != std::string::npos || !types.insert(type).second)
                    throw std::invalid_argument("invalid expected sender event types");
            }
            if (types.size() > 16) throw std::invalid_argument("invalid expected sender event types");
        }
        if (options.timeoutMs < 1 || options.timeoutMs > 30000 || options.maxFrameBytes < 4096 ||
            options.maxFrameBytes > 256 * 1024 || options.socketPath.empty() || options.socketPath.front() != '/' ||
            options.socketPath.find('\0') != std::string::npos || options.socketPath.size() >= sizeof(sockaddr_un::sun_path)) {
            throw std::invalid_argument("invalid event store client options");
        }
        const auto slash = options.socketPath.find_last_of('/');
        const auto name = options.socketPath.substr(slash + 1);
        if (name.empty() || name == "." || name == "..") throw std::invalid_argument("invalid socket filename");
        char* resolved = realpath(options.socketPath.substr(0, slash ? slash : 1).c_str(), nullptr);
        if (!resolved) throw std::runtime_error("event store socket directory is unavailable");
        directory = resolved;
        std::free(resolved);
        options.socketPath = directory + (directory == "/" ? "" : "/") + name;
        struct stat socketInfo{};
        if (lstat(options.socketPath.c_str(), &socketInfo) == 0 && !S_ISSOCK(socketInfo.st_mode)) {
            throw std::runtime_error("event store endpoint must be a socket, not an alias");
        }
        Descriptor parent(open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (parent.get() < 0) throw std::runtime_error("cannot open socket directory");
        Descriptor shared(lockDirectory(parent.get(), ".event-store-clients"));
        const auto store = hexName(options.identity.storeId);
        lockDir.reset(lockDirectory(shared.get(), store));
        lockDirPath = directory + "/.event-store-clients/" + store;
        lockName = (producer() ? "producer-" : "sender-") + hexName(options.actorId) + ".lock";
        lock.reset(openat(lockDir.get(), lockName.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (lock.get() < 0) throw std::runtime_error("cannot open event store actor lock");
        verifyLock();
        if (flock(lock.get(), LOCK_EX | LOCK_NB) != 0) throw std::runtime_error("event store actor already owned");
        verifyLock();
        std::ifstream random("/proc/sys/kernel/random/uuid");
        if (!std::getline(random, session)) throw std::runtime_error("cannot generate event store session identity");
        identifier(session);
    }

    EventStoreClientOptions options;
    pid_t pid;
    std::thread::id thread;
    Descriptor lockDir;
    Descriptor lock;
    std::string directory, lockDirPath, lockName, session;
    std::string registration, pending, pendingOperation;
    std::int64_t expectedEpoch = 0, currentEpoch = 0, currentSequence = 0;
    bool started = false, uncertain = false, rejected = false;
    Clock::time_point deadline;

    bool producer() const { return options.role == EventStoreClientRole::Producer; }
    const char* idKey() const { return producer() ? "producerId" : "senderId"; }
    const char* receiptOp() const { return producer() ? "GetReceipt" : "GetDeliveryReceipt"; }

    void verifyLock() const {
        struct stat opened{}, current{}, openedDir{}, currentDir{};
        if (fstat(lock.get(), &opened) != 0 || fstatat(lockDir.get(), lockName.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(opened.st_mode) || !S_ISREG(current.st_mode) || opened.st_nlink != 1 || opened.st_uid != geteuid() ||
            (opened.st_mode & 0022) || opened.st_ino != current.st_ino || opened.st_dev != current.st_dev ||
            fstat(lockDir.get(), &openedDir) != 0 || lstat(lockDirPath.c_str(), &currentDir) != 0 ||
            !S_ISDIR(currentDir.st_mode) || openedDir.st_ino != currentDir.st_ino || openedDir.st_dev != currentDir.st_dev) {
            throw std::runtime_error("event store actor lock was replaced or is unsafe");
        }
    }

    void owner() const {
        if (getpid() != pid || std::this_thread::get_id() != thread) {
            throw std::logic_error("event store client cannot cross fork/thread ownership");
        }
        verifyLock();
    }

    void begin() { owner(); deadline = Clock::now() + std::chrono::milliseconds(options.timeoutMs); }

    std::string wire(const std::string& op, const std::string& args) const {
        const auto result = "{\"version\":\"1\",\"storeId\":" + quote(options.identity.storeId) +
            ",\"configGeneration\":" + quote(options.identity.configGeneration) + ",\"op\":" + quote(op) + ",\"args\":" + args + "}";
        if (result.size() > options.maxFrameBytes) throw std::length_error("event store client frame limit");
        return result;
    }

    Json call(const std::string& request, bool mutation) {
        owner();
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining < 1) throw EventStoreClientError("DEADLINE", "event store client call budget exhausted", false);
        std::string response;
        try { response = callEventStore(options.socketPath, request, static_cast<int>(remaining), options.maxFrameBytes); }
        catch (const EventStoreTransportError& ex) {
            throw EventStoreClientError("TRANSPORT", ex.what(), mutation && ex.outcomeUnknown());
        }
        Json root;
        try {
            root = json::JsonParser(response, 16, 4096).parse();
            (void)serialize(root, options.maxFrameBytes); // Also rejects duplicate response keys.
            if (field(root, "ok").asBool()) return root;
            const auto code = field(root, "code").asString();
            const auto outcome = field(root, "outcome").asString();
            const auto message = field(root, "message").asString();
            const std::set<std::string> conflictCodes{"CONFLICT_OR_INVALID", "STALE_EPOCH", "REGISTER_CONFLICT",
                "REQUEST_CONFLICT", "STALE_OR_GAPPED_SEQUENCE", "BATCH_IN_PROGRESS", "READ_LIMIT", "JOURNAL_CONFLICT",
                "CAPACITY_REJECTED"};
            const std::set<std::string> unknownCodes{"SQLITE_ERROR", "STORE_ERROR", "RESPONSE_TOO_LARGE"};
            const bool valid = (code == "REJECTED" && outcome == "not_queued") ||
                (conflictCodes.count(code) && outcome == "not_committed") ||
                (unknownCodes.count(code) && outcome == "unknown_reconcile_receipt");
            if (!valid) {
                throw std::invalid_argument("inconsistent event store error outcome");
            }
            throw EventStoreClientError(code, message, mutation && outcome != "not_queued" && outcome != "not_committed");
        } catch (const EventStoreClientError&) { throw; }
        catch (const std::exception& ex) { throw EventStoreClientError("BAD_RESPONSE", ex.what(), mutation); }
    }

    Json readReceipt(bool requireCurrent) {
        auto root = call(wire(receiptOp(), "{" + quote(idKey()) + ":" + quote(options.actorId) + "}"), false);
        if (const auto* status = root.find("status")) {
            if (status->asString() == "IN_PROGRESS") throw EventStoreClientError("IN_PROGRESS", "event store request still in progress", false);
        }
        if (field(root, idKey()).asString() != options.actorId) throw std::runtime_error("event store receipt actor mismatch");
        (void)field(root, "sessionId").asString();
        (void)integer(root, "epoch"); (void)integer(root, "sequence");
        if (requireCurrent && (field(root, "sessionId").asString() != session || integer(root, "epoch") != currentEpoch)) {
            throw EventStoreClientError("FENCED", "event store client epoch was replaced", !pending.empty());
        }
        return root;
    }

    void match(const Json& root, std::int64_t epoch, std::int64_t sequence) {
        if (field(root, idKey()).asString() != options.actorId || field(root, "sessionId").asString() != session ||
            integer(root, "epoch") != epoch || integer(root, "sequence") != sequence) {
            throw std::invalid_argument("event store mutation receipt identity/sequence mismatch");
        }
    }

    void verifySenderScope() {
        if (options.expectedSenderTargetId.empty()) return;
        const auto root = call(wire("GetSenderScope", "{\"senderId\":" + quote(options.actorId) + "}"), false);
        if (field(root, "scopeVersion").asString() != "1" ||
            field(root, "storeId").asString() != options.identity.storeId ||
            field(root, "configGeneration").asString() != options.identity.configGeneration ||
            field(root, "senderId").asString() != options.actorId ||
            field(root, "targetId").asString() != options.expectedSenderTargetId)
            throw std::runtime_error("event store sender scope identity/target mismatch");
        const auto& values = field(root, "eventTypes").asArray().values;
        std::set<std::string> types;
        if (values.empty() || values.size() > 16) throw std::runtime_error("invalid event store sender scope types");
        for (const auto& value : values) {
            if (!types.insert(value->asString()).second) throw std::runtime_error("duplicate event store sender scope type");
        }
        if (types != std::set<std::string>(options.expectedSenderEventTypes.begin(), options.expectedSenderEventTypes.end()))
            throw std::runtime_error("event store sender scope event types mismatch");
    }

    Json start() {
        begin();
        if (started) return readReceipt(true);
        const auto hello = call(wire("Hello", "{}"), false);
        if (options.requireLocalJournal &&
            (!hello.find("localJournalVersion") || field(hello, "localJournalVersion").asString() != "1"))
            throw std::runtime_error("event store reliable local journal capability is required");
        if (field(hello, "version").asString() != "1" || field(hello, "schemaVersion").asString() != "1" ||
            field(hello, "backend").asString() != "ipc" || !field(hello, "laboratoryOnly").asBool() ||
            field(hello, "storeId").asString() != options.identity.storeId ||
            field(hello, "configGeneration").asString() != options.identity.configGeneration ||
            integer(hello, "synchronous") != 2) throw std::runtime_error("event store Hello identity/backend/durability mismatch");
        const auto profile = field(hello, "storageProfile").asString();
        const auto mode = field(hello, "journalMode").asString();
        if (!((profile == "wal-full" && mode == "wal") || (profile == "delete-full" && mode == "delete"))) {
            throw std::runtime_error("event store Hello profile mismatch");
        }
        if (registration.empty()) {
            // Scope is immutable in the server registry. Check before any
            // registration side effect; never block an uncertain registration's
            // exact replay on a new, unrelated scope read.
            verifySenderScope();
            const auto prior = readReceipt(false);
            expectedEpoch = integer(prior, "epoch");
            if (expectedEpoch == std::numeric_limits<std::int64_t>::max()) throw std::overflow_error("event store epoch exhausted");
            registration = wire(producer() ? "RegisterProducer" : "RegisterSender", "{" + quote(idKey()) + ":" +
                quote(options.actorId) + ",\"sessionId\":" + quote(session) + ",\"expectedEpoch\":" + quote(std::to_string(expectedEpoch)) + "}");
        }
        auto root = call(registration, true);
        try { match(root, expectedEpoch + 1, 0); }
        catch (const std::exception& ex) { throw EventStoreClientError("BAD_RESPONSE", ex.what(), true); }
        currentEpoch = expectedEpoch + 1;
        currentSequence = 0;
        started = true;
        registration.clear();
        return root;
    }

    std::string request(const std::string& op, const Json& args) {
        std::set<std::string> allowed;
        if (producer() && op == "AppendEventsAndStates") {
            allowed = {"events", "states"};
            if (args.find("localEvents")) allowed.insert("localEvents");
        }
        else if (!producer() && op == "ClaimBatch") allowed = {"limit", "maxBytes", "leaseMs"};
        else if (!producer() && (op == "AckBatch" || op == "ReleaseBatch")) allowed = {"claimToken", "items"};
        else throw std::invalid_argument("operation is not allowed for this event store client role");
        if (args.asObject().values.size() != allowed.size()) throw std::invalid_argument("event store operation fields mismatch");
        for (const auto& entry : args.asObject().values) {
            if (!allowed.erase(entry.key)) throw std::invalid_argument("invalid/reserved event store operation field");
        }
        const auto data = serialize(args, options.maxFrameBytes);
        if (currentSequence == std::numeric_limits<std::int64_t>::max()) throw std::overflow_error("event store sequence exhausted");
        const auto result = wire(op, "{" + quote(idKey()) + ":" + quote(options.actorId) + ",\"epoch\":" + quote(std::to_string(currentEpoch)) +
            ",\"sequence\":" + quote(std::to_string(currentSequence + 1)) + "," + data.substr(1));
        EventStoreRuntimeOptions validation;
        validation.identity = options.identity;
        validation.maxFrameBytes = options.maxFrameBytes;
        if (producer()) validation.producers.push_back(options.actorId);
        else validation.senders.push_back({options.actorId, "main", {}});
        validateEventStoreRequest(result, validation);
        return result;
    }

    Json sendPending() {
        rejected = false;
        try {
            auto root = call(pending, true);
            try {
                match(root, currentEpoch, currentSequence + 1);
                const auto original = json::JsonParser(pending, 16, 4096).parse();
                const auto& args = field(original, "args");
                if (producer()) {
                    const auto& receipt = field(root, "receipt");
                    if (!field(receipt, "committed").asBool() || integer(receipt, "sequence") != currentSequence + 1) {
                        throw std::invalid_argument("event store append lacks committed receipt");
                    }
                    const auto& ids = field(receipt, "ids").asArray().values;
                    if (ids.size() != field(args, "events").asArray().values.size()) {
                        throw std::invalid_argument("event store append receipt count mismatch");
                    }
                    std::set<std::int64_t> unique;
                    for (const auto& id : ids) {
                        Json::Object wrapper;
                        wrapper.values.push_back({"id", id});
                        if (!unique.insert(integer(Json::makeObject(std::move(wrapper)), "id", 1)).second) {
                            throw std::invalid_argument("event store append receipt duplicate row");
                        }
                    }
                    const auto* local = args.find("localEvents");
                    if (local && !local->asArray().values.empty()) {
                        const auto& journalIds = field(receipt, "journalIds").asArray().values;
                        if (journalIds.size() != local->asArray().values.size())
                            throw std::invalid_argument("event store journal receipt count mismatch");
                        std::set<std::int64_t> journalUnique;
                        for (const auto& id : journalIds) {
                            Json::Object wrapper;
                            wrapper.values.push_back({"id", id});
                            if (!journalUnique.insert(integer(Json::makeObject(std::move(wrapper)), "id", 1)).second)
                                throw std::invalid_argument("event store duplicate journal receipt");
                        }
                    }
                } else {
                    const auto status = field(root, "status").asString();
                    if (pendingOperation == "ClaimBatch") {
                        if (status != "CLAIMED" && status != "EMPTY" && status != "EXPIRED" && status != "REVOKED") {
                            throw std::invalid_argument("event store claim status mismatch");
                        }
                        const auto& messages = field(root, "messages").asArray().values;
                        if (status != "CLAIMED") {
                            if (!messages.empty()) throw std::invalid_argument("inactive event store claim contains payload");
                        } else {
                            if (messages.empty() || messages.size() > static_cast<std::size_t>(integer(args, "limit", 1)) ||
                                field(root, "claimToken").asString().empty() || field(root, "claimToken").asString().size() > 256 ||
                                field(root, "bootId").asString().empty() || field(root, "bootId").asString().size() > 96) {
                                throw std::invalid_argument("event store claim metadata mismatch");
                            }
                            (void)integer(root, "leaseUntilMs", 1);
                            std::set<std::int64_t> unique;
                            std::size_t bytes = 0;
                            for (const auto& message : messages) {
                                if (!unique.insert(integer(*message, "id", 1)).second || field(*message, "eventId").asString().empty() ||
                                    field(*message, "eventId").asString().size() > 256 || field(*message, "eventType").asString().empty()) {
                                    throw std::invalid_argument("invalid event store claim row");
                                }
                                const auto& topic = field(*message, "topic").asString();
                                if (topic.empty() || topic.size() > 4096) throw std::invalid_argument("invalid claim topic");
                                bytes += topic.size() + field(*message, "payload").asString().size();
                                (void)integer(*message, "eventTs", 1);
                            }
                            if (bytes > static_cast<std::size_t>(integer(args, "maxBytes", 1))) throw std::invalid_argument("event store claim byte limit mismatch");
                        }
                    } else {
                        if (status != "FINISHED") throw std::invalid_argument("event store finish status mismatch");
                        const auto& results = field(root, "results").asArray().values;
                        const auto& items = field(args, "items").asArray().values;
                        if (results.size() != items.size()) throw std::invalid_argument("event store finish count mismatch");
                        const std::set<std::string> statuses{"APPLIED", "ALREADY_ACKED", "ALREADY_RELEASED", "EXPIRED", "REVOKED", "NOT_CLAIMED"};
                        for (std::size_t i = 0; i < results.size(); ++i) {
                            if (integer(*results[i], "id", 1) != integer(*items[i], "id", 1) ||
                                field(*results[i], "eventId").asString() != field(*items[i], "eventId").asString() ||
                                !statuses.count(field(*results[i], "status").asString())) {
                                throw std::invalid_argument("event store finish item mismatch");
                            }
                        }
                    }
                }
            } catch (const std::exception& ex) { throw EventStoreClientError("BAD_RESPONSE", ex.what(), true); }
            ++currentSequence;
            pending.clear(); pendingOperation.clear(); uncertain = false;
            return root;
        } catch (const EventStoreClientError& ex) {
            uncertain = uncertain || ex.outcomeUnknown();
            rejected = !uncertain && (ex.code() == "REJECTED" || ex.code() == "CONFLICT_OR_INVALID" || ex.code() == "BATCH_IN_PROGRESS");
            // A later queue rejection does not resolve an earlier timed-out attempt.
            throw EventStoreClientError(ex.code(), ex.what(), uncertain);
        }
    }

    Json execute(const std::string& op, const Json& args) {
        begin();
        if (!started) throw std::logic_error("event store client must start before execute");
        const auto next = request(op, args);
        if (!pending.empty() && pending != next) throw std::logic_error("event store unresolved request cannot be replaced");
        if (pending.empty()) { pending = next; pendingOperation = op; uncertain = false; }
        return sendPending();
    }

    std::vector<EventStoreState> loadStates(std::size_t maxStates) {
        begin();
        if (!started || !producer() || !pending.empty()) throw std::logic_error("event store baseline requires an idle producer");
        if (!maxStates || maxStates > 65536) throw std::invalid_argument("invalid event store baseline limit");
        const auto prior = readReceipt(true);
        if (integer(prior, "sequence") != currentSequence) throw std::runtime_error("event store baseline sequence mismatch");
        std::vector<EventStoreState> states;
        std::string cursor;
        std::size_t bytes = 0;
        for (;;) {
            const auto page = call(wire("LoadStates", "{\"producerId\":" + quote(options.actorId) +
                ",\"afterKey\":" + quote(cursor) + ",\"limit\":\"64\"}"), false);
            const auto& rows = field(page, "states").asArray().values;
            if (rows.size() > 64 || integer(page, "pageCount") != static_cast<std::int64_t>(rows.size())) {
                throw std::runtime_error("event store invalid state page count");
            }
            const auto next = field(page, "nextKey").asString();
            if (rows.empty()) {
                if (!next.empty()) throw std::runtime_error("event store empty page has cursor");
                break;
            }
            for (const auto& row : rows) {
                if (states.size() >= maxStates) throw std::length_error("event store baseline count limit");
                bytes += serialize(*row, options.maxFrameBytes).size() + sizeof(EventStoreState);
                if (bytes > 16 * 1024 * 1024) throw std::length_error("event store baseline byte limit");
                EventStoreState item;
                auto& state = item.state;
                state.stateKey = field(*row, "stateKey").asString();
                if (state.stateKey <= cursor || state.stateKey.size() > 256) throw std::runtime_error("event store state cursor does not advance");
                cursor = state.stateKey;
                state.eventType = field(*row, "eventType").asString();
                const auto index = integer(*row, "index");
                if (index > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("event store state index overflow");
                state.index = static_cast<std::uint32_t>(index);
                state.alarmType = field(*row, "alarmType").asString();
                state.active = field(*row, "active").asBool();
                state.value = field(*row, "value").asNumber();
                if (!std::isfinite(state.value)) throw std::runtime_error("event store state value is not finite");
                const auto quality = integer(*row, "quality", std::numeric_limits<int>::min());
                if (quality > std::numeric_limits<int>::max()) throw std::runtime_error("event store state quality overflow");
                state.quality = static_cast<int>(quality);
                state.sourceTs = integer(*row, "sourceTs");
                state.lifecycle = field(*row, "lifecycle").asString();
                item.version = integer(*row, "version");
                states.push_back(std::move(item));
            }
            if (cursor != next) throw std::runtime_error("event store page cursor mismatch");
        }
        if (integer(readReceipt(true), "sequence") != currentSequence) throw std::runtime_error("event store baseline changed while reading");
        return states;
    }
};

EventStoreClient::EventStoreClient(EventStoreClientOptions options) : impl_(new Impl(std::move(options))) {}
EventStoreClient::~EventStoreClient() = default;
Json EventStoreClient::start() { return impl_->start(); }
Json EventStoreClient::execute(const std::string& op, const Json& args) { return impl_->execute(op, args); }
Json EventStoreClient::retry() {
    impl_->begin();
    if (!impl_->started || impl_->pending.empty()) throw std::logic_error("no pending event store request");
    return impl_->sendPending();
}
void EventStoreClient::discardRejected() {
    impl_->owner();
    if (impl_->pending.empty() || !impl_->rejected || impl_->uncertain) throw std::logic_error("cannot discard unresolved event store request");
    impl_->pending.clear(); impl_->pendingOperation.clear(); impl_->rejected = false;
}
bool EventStoreClient::hasPending() const { impl_->owner(); return !impl_->pending.empty(); }
bool EventStoreClient::hasPendingRegistration() const { impl_->owner(); return !impl_->registration.empty(); }
std::string EventStoreClient::pendingRequest() const { impl_->owner(); return impl_->pending; }
std::int64_t EventStoreClient::epoch() const { impl_->owner(); return impl_->currentEpoch; }
std::int64_t EventStoreClient::sequence() const { impl_->owner(); return impl_->currentSequence; }
std::vector<EventStoreState> EventStoreClient::loadStates(std::size_t maxStates) { return impl_->loadStates(maxStates); }
Json EventStoreClient::receipt() { impl_->begin(); return impl_->readReceipt(impl_->started); }

} // namespace edge_gateway
