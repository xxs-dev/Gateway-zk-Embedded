#pragma once

#include "edge_gateway/mqtt_event_outbox.hpp"
#include "edge_gateway/models.hpp"

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace edge_gateway {

class EventStoreConflict : public std::invalid_argument {
public:
    EventStoreConflict(std::string code, const std::string& message)
        : std::invalid_argument(code + ": " + message), code_(std::move(code)) {}
    const std::string& code() const noexcept { return code_; }
private:
    std::string code_;
};

// Experimental storage contract. It is not a production Outbox backend yet.
struct EventStoreIdentity {
    std::string storeId;
    std::string configGeneration;
};

struct EventStoreProducer {
    std::string producerId;
    std::string sessionId;
    std::int64_t epoch = 0;
    std::int64_t sequence = 0;
    std::string request;
    std::string receipt;
};

struct EventStoreState {
    MqttEventOutbox::EventState state;
    std::int64_t version = 0;
};

struct EventStoreLocalEvent {
    std::string kind; // alarm or change; only alarm is copied to alarm_events.
    AlarmEvent event;
    std::int64_t stateVersion = 0;
    std::string configGeneration;
};

struct EventStoreJournalRow {
    std::int64_t id = 0;
    EventStoreLocalEvent local;
};

class EventHistoryProjection;

struct EventStoreProjectionCursor {
    std::string journalGeneration;
    std::int64_t projectedThrough = 0;
    std::int64_t cleanedThrough = 0;
    std::int64_t journalThrough = 0;
};

struct EventStoreCapacityLimits {
    std::uint64_t minFreeBytes = 0;
    std::uint64_t maxStoreBytes = 0;
    std::string historyPath;
};

struct EventStoreCapacityStatus {
    std::uint64_t availableBytes = 0; // Minimum f_bavail across configured database filesystems.
    std::uint64_t storeBytes = 0; // Logical file lengths: both DBs and SQLite sidecars.
    bool blocked = false;
    std::string reason;
};

struct EventStoreAppend {
    std::string producerId;
    std::int64_t epoch = 0;
    std::int64_t sequence = 0;
    // Exact immutable request bytes, bounded by the protocol. No lossy hash.
    std::string request;
    std::vector<MqttEventOutbox::EventMessage> events;
    std::vector<EventStoreState> states;
    std::vector<EventStoreLocalEvent> localEvents;
};

struct EventStoreSenderConfig {
    std::string senderId;
    std::string targetId;
    std::vector<std::string> eventTypes;
};

struct EventStoreLeaseTime {
    std::string bootId;
    std::int64_t milliseconds = 0;
};

struct EventStoreClaimItem {
    std::int64_t id = 0;
    std::string eventId;
};

struct EventStoreDeliveryRequest {
    std::string operation; // ClaimBatch, AckBatch or ReleaseBatch.
    std::string senderId;
    std::int64_t epoch = 0;
    std::int64_t sequence = 0;
    std::string request;
    std::size_t limit = 8;
    std::size_t maxBytes = 32768;
    std::int64_t leaseMs = 5000;
    std::string claimToken;
    std::vector<EventStoreClaimItem> items;
};

class EventStoreDatabase {
public:
    EventStoreDatabase(MqttEventOutbox& outbox, EventStoreIdentity identity,
        std::vector<std::string> producers, bool readOnly = false,
        std::vector<EventStoreSenderConfig> senders = {},
        std::function<EventStoreLeaseTime()> leaseClock = {});
    EventStoreProducer registerProducer(const std::string& producerId,
        const std::string& sessionId, std::int64_t expectedEpoch);
    EventStoreProducer receipt(const std::string& producerId);
    EventStoreProducer append(const EventStoreAppend& request);
    void setCapacityLimits(EventStoreCapacityLimits limits);
    EventStoreCapacityStatus capacityStatus() const;
    std::vector<EventStoreState> states(const std::string& producerId,
        const std::string& afterKey, std::size_t limit);
    std::string registerSender(const std::string& senderId, const std::string& sessionId,
        std::int64_t expectedEpoch);
    std::string deliveryReceipt(const std::string& senderId);
    std::string deliver(const EventStoreDeliveryRequest& request);
    void setDeliveryResponseLimit(std::size_t maxBytes);
    std::vector<EventStoreJournalRow> readJournal(std::int64_t afterId, std::size_t limit);
    std::string journalGeneration();
    EventStoreProjectionCursor projectionCursor();
    // Internal writer-queue boundary, never exposed as a client operation. The owner
    // must first commit/verify the corresponding history batch on its own connection.
    void commitProjectionCursor(const std::string& generation, std::int64_t expected, std::int64_t through);
    // Writer-queue operations. Projection must remain exclusively owned throughout each call.
    std::int64_t reconcileProjection(EventHistoryProjection& projection);
    void confirmProjection(EventHistoryProjection& projection);
    // Journal retention is intentionally disabled: immutable eventId deduplication must
    // survive retention before deletion can be enabled. No timestamp-only cleanup API.

private:
    friend class EventHistoryProjection;
    const std::string& journalDatabasePath() const;
    void checkProducer(const std::string& producerId) const;
    EventStoreProducer writeTransaction(const std::function<EventStoreProducer()>& operation);
    const EventStoreSenderConfig& checkSender(const std::string& senderId) const;
    MqttEventOutbox& outbox_;
    EventStoreIdentity identity_;
    std::vector<std::string> producers_;
    bool readOnly_;
    std::vector<EventStoreSenderConfig> senders_;
    std::function<EventStoreLeaseTime()> leaseClock_;
    std::size_t deliveryResponseLimit_ = 256 * 1024;
    EventStoreCapacityLimits capacityLimits_;
};

}  // namespace edge_gateway
