#include "edge_gateway/event_store_sender.hpp"
#include "edge_gateway/event_store_clock.hpp"

#include <thread>
#include <unistd.h>

namespace edge_gateway {
namespace {
using Json = json::JsonValue;
const Json& field(const Json& value, const char* key) {
    const auto* result = value.find(key);
    if (!result) throw std::runtime_error(std::string("missing sender field: ") + key);
    return *result;
}
std::int64_t number(const Json& value, const char* key) {
    const auto& raw = field(value, key).asString();
    std::size_t used = 0;
    const auto result = std::stoll(raw, &used);
    if (result <= 0 || used != raw.size() || std::to_string(result) != raw)
        throw std::runtime_error("invalid sender receipt integer");
    return result;
}
Json object(std::initializer_list<std::pair<std::string, Json>> fields) {
    Json::Object result;
    for (const auto& entry : fields)
        result.values.push_back({entry.first, std::make_shared<Json>(entry.second)});
    return Json::makeObject(std::move(result));
}
Json decimal(std::int64_t n) { return Json::makeString(std::to_string(n)); }
EventStoreSenderOptions validate(EventStoreSenderOptions options) {
    if (options.client.role != EventStoreClientRole::Sender || !options.authorized || (!options.send && !options.sendBatch) ||
        options.client.expectedSenderTargetId.empty() || options.client.expectedSenderEventTypes.empty() ||
        options.limit < 1 || options.limit > 16 || options.maxBytes < 1 || options.maxBytes > 32768 ||
        options.client.maxFrameBytes <= 65536 ||
        static_cast<std::size_t>(options.maxBytes) > (options.client.maxFrameBytes - 65536) / 6 ||
        options.leaseMs < 100 || options.leaseMs > 30000 || options.networkTimeoutMs < 1 ||
        options.completionReserveMs < 1 || options.completionReserveMs >= options.leaseMs ||
        options.networkTimeoutMs > options.leaseMs - options.completionReserveMs)
        throw std::invalid_argument("invalid event store sender options");
    if (!options.leaseClock) options.leaseClock = readEventStoreLeaseTime;
    return options;
}
} // namespace

struct EventStoreSender::Impl {
    enum class Phase { Start, Idle, Claim, Network, Ack, Release };
    EventStoreSenderOptions options;
    EventStoreClient client;
    const pid_t pid = getpid();
    const std::thread::id thread = std::this_thread::get_id();
    bool running = false, startAttempted = false;
    Phase phase = Phase::Start;
    Json claim;
    std::vector<EventStoreSenderMessage> messages;
    EventStoreSenderCall call;
    std::vector<bool> attempted, confirmed;
    std::vector<std::size_t> ackIndexes, releaseIndexes;
    bool draining = false, batchHealthy = true;
    std::string batchError;
    EventStoreSenderProgress progress;

    explicit Impl(EventStoreSenderOptions value) : options(validate(std::move(value))), client(options.client) {}
    bool authorized() noexcept {
        if (draining) return false;
        try { return options.authorized(); } catch (...) { return false; }
    }
    void unhealthy(const char* reason) {
        batchHealthy = false;
        if (batchError.empty()) batchError = reason;
    }
    bool pending() const {
        return (phase == Phase::Start && startAttempted) ||
            (phase != Phase::Start && phase != Phase::Idle);
    }
    void owner() const {
        if (getpid() != pid || std::this_thread::get_id() != thread || running)
            throw std::logic_error("event store sender owner/reentrancy violation");
    }
    bool budget(bool prepare) noexcept {
        try {
            const auto now = options.leaseClock();
            if (now.bootId != call.bootId || now.milliseconds < 0 || now.milliseconds >= call.leaseUntilMs)
                return false;
            const auto remaining = call.leaseUntilMs - now.milliseconds;
            if (prepare) {
                if (remaining < static_cast<std::int64_t>(options.networkTimeoutMs) + options.completionReserveMs)
                    return false;
                call.timeoutMs = options.networkTimeoutMs;
                call.deadlineMs = now.milliseconds + options.networkTimeoutMs;
                return true;
            }
            return now.milliseconds < call.deadlineMs;
        } catch (...) { return false; }
    }
    Json mutation(const char* op, const Json& args) {
        // The client reconciles and replays the original bytes, even when denied.
        return client.hasPending() ? client.retry() : client.execute(op, args);
    }
    Json finishArgs(const std::vector<std::size_t>& indexes) const {
        Json::Array items;
        for (auto i : indexes)
            items.values.push_back(std::make_shared<Json>(object({{"id", decimal(messages[i].id)},
                {"eventId", Json::makeString(messages[i].eventId)}})));
        return object({{"claimToken", Json::makeString(call.claimToken)},
            {"items", Json::makeArray(std::move(items))}});
    }
    void decode() {
        // Parse all rows before any callback; a decoding failure cannot resend a prefix.
        std::vector<EventStoreSenderMessage> parsed;
        EventStoreSenderCall context;
        context.senderId = options.client.actorId;
        context.epoch = client.epoch();
        context.claimSequence = client.sequence();
        context.claimToken = field(claim, "claimToken").asString();
        context.bootId = field(claim, "bootId").asString();
        context.leaseUntilMs = number(claim, "leaseUntilMs");
        for (const auto& row : field(claim, "messages").asArray().values) {
            EventStoreSenderMessage message;
            message.id = number(*row, "id");
            message.eventId = field(*row, "eventId").asString();
            message.eventType = field(*row, "eventType").asString();
            message.topic = field(*row, "topic").asString();
            message.payload = field(*row, "payload").asString();
            message.eventTs = number(*row, "eventTs");
            parsed.push_back(std::move(message));
        }
        messages = std::move(parsed);
        call = std::move(context);
        attempted.assign(messages.size(), false);
        confirmed.assign(messages.size(), false);
        ackIndexes.clear();
        releaseIndexes.clear();
    }
    EventStoreSenderResult run(bool drainOnly = false) {
        owner();
        progress = {};
        if (phase == Phase::Idle || (phase == Phase::Start && !startAttempted)) {
            draining = false;
            batchHealthy = true;
            batchError.clear();
        }
        draining = draining || drainOnly;
        if (draining) unhealthy("sender draining without network authority");
        running = true;
        struct Guard { bool& flag; ~Guard() { flag = false; } } guard{running};
        if (phase == Phase::Start) {
            if (!startAttempted && (draining || !authorized())) return EventStoreSenderResult::Unauthorized;
            startAttempted = true;
            try { client.start(); }
            catch (...) {
                // A failed read/scope assertion issued no mutation. Let the
                // service relinquish this actor; unknown registration still drains.
                startAttempted = client.hasPendingRegistration();
                throw;
            }
            phase = Phase::Idle;
        }
        if (phase == Phase::Idle) {
            if (!authorized()) return EventStoreSenderResult::Unauthorized;
            phase = Phase::Claim;
        }
        if (phase == Phase::Claim) {
            if (draining && !client.hasPending()) {
                phase = Phase::Idle;
                return EventStoreSenderResult::Unauthorized;
            }
            claim = mutation("ClaimBatch", object({{"limit", decimal(options.limit)},
                {"maxBytes", decimal(options.maxBytes)}, {"leaseMs", decimal(options.leaseMs)}}));
            if (field(claim, "status").asString() != "CLAIMED") {
                if (field(claim, "status").asString() != "EMPTY") unhealthy("claim expired or revoked");
                phase = Phase::Idle;
                return EventStoreSenderResult::Empty;
            }
            phase = Phase::Network;
        }
        if (phase == Phase::Network) {
            decode();
            // Move out of Network before invoking user code. No later IPC failure
            // or callback exception can take this batch back into network delivery.
            phase = Phase::Ack;
            if (options.sendBatch) {
                bool denied = !authorized() || !budget(true);
                const auto canSend = [&]() {
                    if (!denied && (!authorized() || !budget(false))) denied = true;
                    if (denied) unhealthy("sender authority or lease budget unavailable");
                    return !denied;
                };
                const auto onAttempt = [&](std::size_t index) {
                    if (index >= messages.size() || attempted[index] || !canSend())
                        throw std::runtime_error("invalid or unauthorized batch attempt");
                    attempted[index] = true;
                    progress.attemptedBytes += messages[index].topic.size() + messages[index].payload.size();
                };
                const auto onConfirmed = [&](std::size_t index) {
                    if (index >= messages.size() || !attempted[index] || confirmed[index] || !budget(false))
                        throw std::runtime_error("invalid batch confirmation identity");
                    confirmed[index] = true;
                };
                if (canSend()) {
                    try { options.sendBatch(messages, call, canSend, onAttempt, onConfirmed); }
                    catch (...) { unhealthy("batch transport failed after partial delivery"); }
                    (void)canSend();
                }
            } else for (std::size_t index = 0; index < messages.size(); ++index) {
                const auto& message = messages[index];
                if (!authorized() || !budget(true)) {
                    unhealthy("sender authority or lease budget unavailable");
                    break;
                }
                progress.attemptedBytes += message.topic.size() + message.payload.size();
                bool delivered = false;
                try { delivered = options.send(message, call); } catch (...) {}
                confirmed[index] = delivered;
                const bool stillAuthorized = authorized();
                const bool withinDeadline = budget(false);
                if (!delivered || !stillAuthorized || !withinDeadline) {
                    unhealthy("network unconfirmed or sender authority/deadline lost");
                    break;
                }
            }
            // Stable claim order, independent of PUBACK arrival order. Keep these
            // sets unchanged across unknown IPC outcomes and drain-only retries.
            for (std::size_t index = 0; index < messages.size(); ++index) {
                if (confirmed[index]) ackIndexes.push_back(index);
                else releaseIndexes.push_back(index);
            }
            if (!releaseIndexes.empty()) unhealthy("network delivery unconfirmed");
        }
        if (phase == Phase::Ack) {
            if (!ackIndexes.empty()) {
                const auto receipt = mutation("AckBatch", finishArgs(ackIndexes));
                // EventStoreClient validates count and ordered id/eventId pairs
                // against this exact request before it clears pending bytes.
                const auto& results = field(receipt, "results").asArray().values;
                for (std::size_t i = 0; i < results.size(); ++i) {
                    const auto& status = field(*results[i], "status").asString();
                    if (status == "APPLIED" || status == "ALREADY_ACKED") {
                        ++progress.ackedCount;
                        if (messages[ackIndexes[i]].eventType == "alarm") ++progress.alarmCount;
                        else if (messages[ackIndexes[i]].eventType == "change") ++progress.changeCount;
                        else ++progress.otherCount;
                    } else unhealthy("storage ACK expired, revoked or not applied");
                }
            }
            phase = Phase::Release;
        }
        if (phase == Phase::Release) {
            if (!releaseIndexes.empty()) (void)mutation("ReleaseBatch", finishArgs(releaseIndexes));
            messages.clear();
            claim = Json{};
            phase = Phase::Idle;
        }
        return EventStoreSenderResult::Completed;
    }
    EventStoreSenderProgress detailed(bool drainOnly) {
        owner();
        try {
            progress.result = run(drainOnly);
            progress.pending = pending();
            progress.healthy = !progress.pending && batchHealthy &&
                progress.result != EventStoreSenderResult::Unauthorized;
            progress.error = batchError;
        } catch (const std::exception& ex) {
            progress.pending = pending();
            progress.healthy = false;
            progress.error = ex.what();
        } catch (...) {
            progress.pending = pending();
            progress.healthy = false;
            progress.error = "sender IPC operation failed";
        }
        return progress;
    }
};

EventStoreSender::EventStoreSender(EventStoreSenderOptions options) : impl_(new Impl(std::move(options))) {}
EventStoreSender::~EventStoreSender() = default;
EventStoreSenderResult EventStoreSender::runOnce() { return impl_->run(); }
EventStoreSenderProgress EventStoreSender::runOnceDetailed(bool drainOnly) { return impl_->detailed(drainOnly); }

} // namespace edge_gateway
