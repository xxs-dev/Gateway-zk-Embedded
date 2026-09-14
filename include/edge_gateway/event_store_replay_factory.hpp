#pragma once

#include "edge_gateway/event_store_sender.hpp"
#include "edge_gateway/mqtt_event_replay.hpp"

namespace edge_gateway {

struct EventStoreReplayFactoryOptions {
    EventStoreClientOptions client;
    MqttEventReplayLane lane = MqttEventReplayLane::MainBusiness;
    std::string targetId;
    std::vector<std::string> eventTypes;
    std::function<bool(const EventStoreSenderMessage&, const EventStoreSenderCall&)> send;
    EventStoreSenderBatch sendBatch;
    int leaseMs = 30000;
    int networkTimeoutMs = 3000;
    int completionReserveMs = 100;
    std::function<EventStoreLeaseTime()> leaseClock; // Tests only; empty uses BOOTTIME.
};

// Pure closure creation: no actor lock, registration, IPC or SQLite access.
// The configured senderId must already have this target/types scope in EventStore.
// Verified via GetSenderScope before registration/Claim; old servers fail closed.
// Driver business/management and third-party delivery use separate factory values.
MqttEventReplayFactory makeEventStoreReplayFactory(EventStoreReplayFactoryOptions options);

} // namespace edge_gateway
