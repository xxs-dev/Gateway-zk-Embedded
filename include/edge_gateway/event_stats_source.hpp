#pragma once

#include "edge_gateway/event_store.hpp"

namespace edge_gateway {

struct EventStatsQuery {
    std::string key;
    std::string context;
    EventStatsScope scope;
};

enum class EventStatsStatus { NeverSampled, Fresh, Error, Stale, Stopped };
enum class EventStatsBackend { Legacy, Ipc };

struct EventStatsCacheEntry {
    EventStatsQuery query;
    EventStatsBackend backend = EventStatsBackend::Legacy;
    EventStoreIdentity identity;
    EventStatsStatus status = EventStatsStatus::NeverSampled;
    bool hasValue = false;
    bool valid = false;
    EventPendingStats value;
    std::int64_t sampledAtUnixMs = 0;
    std::int64_t ageMs = 0; // Only meaningful with hasValue; based on steady_clock.
    std::string error;
};

// Concurrent snapshot calls must only read memory, never SQL or IPC.
// The owner controls sampling lifetime; destruction must finish any worker.
class IEventStatsSource {
public:
    virtual ~IEventStatsSource() = default;
    virtual EventStatsCacheEntry snapshot(const std::string& key) const = 0;
};

} // namespace edge_gateway
