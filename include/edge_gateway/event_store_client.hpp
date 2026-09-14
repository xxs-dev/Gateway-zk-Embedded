#pragma once

#include "edge_gateway/event_store.hpp"
#include "edge_gateway/json_value.hpp"

#include <memory>
#include <stdexcept>

namespace edge_gateway {

enum class EventStoreClientRole { Producer, Sender };

struct EventStoreClientOptions {
    EventStoreIdentity identity;
    std::string socketPath;
    std::string actorId;
    EventStoreClientRole role = EventStoreClientRole::Producer;
    int timeoutMs = 3000;
    std::size_t maxFrameBytes = 256 * 1024;
    bool requireLocalJournal = false;
    // Optional for raw protocol clients; mandatory for EventStoreSender.
    // Both must be set together. Exact actor scope assertion, never a selector.
    std::string expectedSenderTargetId;
    std::vector<std::string> expectedSenderEventTypes;
};

class EventStoreClientError : public std::runtime_error {
public:
    EventStoreClientError(std::string code, const std::string& message, bool unknown)
        : std::runtime_error(message), code_(std::move(code)), unknown_(unknown) {}
    const std::string& code() const noexcept { return code_; }
    bool outcomeUnknown() const noexcept { return unknown_; }
private:
    std::string code_;
    bool unknown_;
};

// Laboratory-only, single-thread owner. Does not open SQLite or run network callbacks.
// Construction acquires the actor lock; start() explicitly performs bounded IPC.
class EventStoreClient {
public:
    explicit EventStoreClient(EventStoreClientOptions options);
    ~EventStoreClient();
    EventStoreClient(const EventStoreClient&) = delete;
    EventStoreClient& operator=(const EventStoreClient&) = delete;
    EventStoreClient(EventStoreClient&&) = delete;
    EventStoreClient& operator=(EventStoreClient&&) = delete;

    // Role selects GetReceipt/RegisterProducer or GetDeliveryReceipt/RegisterSender.
    // Retry start with the same object after a lost registration reply.
    json::JsonValue start();
    // args contains operation data only: no actorId, epoch or sequence fields.
    // One unresolved mutation is retained byte-for-byte, including after a timeout.
    json::JsonValue execute(const std::string& operation, const json::JsonValue& args);
    json::JsonValue retry();
    // Only after an explicitly discardable rejection with no uncertain attempt.
    // Otherwise throws without changing pending bytes or sequence.
    void discardRejected();
    bool hasPending() const;
    bool hasPendingRegistration() const;
    std::string pendingRequest() const;
    std::int64_t epoch() const;
    std::int64_t sequence() const;
    // Complete snapshot, empty-page termination, max 16 MiB and one total timeoutMs.
    // Producer only; sender/pending mutation is rejected before any IPC.
    // Any failure discards all partial pages.
    std::vector<EventStoreState> loadStates(std::size_t maxStates = 65536);
    // Diagnostic only: never changes sequence or clears an unresolved mutation.
    json::JsonValue receipt();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace edge_gateway
