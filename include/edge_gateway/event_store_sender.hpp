#pragma once

#include "edge_gateway/event_store_client.hpp"

#include <functional>
#include <memory>

namespace edge_gateway {

struct EventStoreSenderMessage {
    std::int64_t id = 0;
    std::string eventId, eventType, topic, payload;
    std::int64_t eventTs = 0;
};

struct EventStoreSenderCall {
    std::string senderId, claimToken, bootId;
    std::int64_t epoch = 0, claimSequence = 0, leaseUntilMs = 0;
    std::int64_t deadlineMs = 0; // Absolute CLOCK_BOOTTIME, never wall time.
    int timeoutMs = 0;
};

// Synchronous observers: no retained references, background calls or hidden retries.
// Report attempt immediately before PUBLISH, confirmation only after exact PUBACK.
// Previously reported confirmations survive a later exception. Index is into messages.
using EventStoreSenderBatch = std::function<void(const std::vector<EventStoreSenderMessage>&,
    const EventStoreSenderCall&, const std::function<bool()>&,
    const std::function<void(std::size_t)>&, const std::function<void(std::size_t)>&)>;

struct EventStoreSenderOptions {
    // client.expectedSenderTargetId/EventTypes are required and verified before
    // registration/Claim/network against the server's immutable actor scope.
    EventStoreClientOptions client;
    int limit = 16;
    int maxBytes = 32768;
    int leaseMs = 30000;
    int networkTimeoutMs = 3000;
    int completionReserveMs = 100;
    // Required, synchronous, checked before each network call. Exceptions deny.
    std::function<bool()> authorized;
    // Return true only for confirmed delivery of this exact message/call identity.
    // Must obey timeout/deadline, perform no hidden retries, and retain no references.
    // False/exception stops the batch; delivery uncertainty may cause redelivery.
    std::function<bool(const EventStoreSenderMessage&, const EventStoreSenderCall&)> send;
    // Preferred when present. canSend latches denial for the whole batch and must
    // be checked before each new PUBLISH; outstanding ACKs may drain until deadline.
    EventStoreSenderBatch sendBatch;
    // Empty selects Linux CLOCK_BOOTTIME + boot_id. Injection is for tests only.
    std::function<EventStoreLeaseTime()> leaseClock;
};

enum class EventStoreSenderResult { Unauthorized, Empty, Completed };

struct EventStoreSenderProgress {
    EventStoreSenderResult result = EventStoreSenderResult::Unauthorized;
    bool pending = false, healthy = false;
    std::size_t attemptedBytes = 0, ackedCount = 0;
    std::size_t alarmCount = 0, changeCount = 0, otherCount = 0;
    std::string error;
};

// IPC only. Owns its client; construct, run and destroy on one thread, without fork.
// runOnce handles at most one batch. IPC errors propagate; keep this object alive
// and call again even after authorization loss to drain unresolved mutations.
// No worker, SQL fallback, automatic re-registration, or destructor IPC.
class EventStoreSender {
public:
    explicit EventStoreSender(EventStoreSenderOptions options);
    ~EventStoreSender();
    EventStoreSender(const EventStoreSender&) = delete;
    EventStoreSender& operator=(const EventStoreSender&) = delete;
    EventStoreSender(EventStoreSender&&) = delete;
    EventStoreSender& operator=(EventStoreSender&&) = delete;
    EventStoreSenderResult runOnce();
    // Incremental statistics, including on IPC failure. drainOnly is sticky for
    // the current batch and never starts a new Claim. Owner violations throw.
    EventStoreSenderProgress runOnceDetailed(bool drainOnly = false);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace edge_gateway
