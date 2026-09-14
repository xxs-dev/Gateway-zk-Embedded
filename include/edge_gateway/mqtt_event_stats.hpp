#pragma once

#include "edge_gateway/event_stats_source.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {

EventStatsQuery mqttDriverTotalStatsQuery();
EventStatsQuery mqttDriverBusinessStatsQuery();
EventStatsQuery mqttForwarderStatsQuery(const MqttForwardEventConfig& config, bool awaitingDelegation);

// Empty identity selects Legacy; IPC requires an explicit expected store and
// generation. Missing/invalid data never triggers SQL fallback.
EventStatsCacheEntry readMqttEventStats(const IEventStatsSource* source, const EventStatsQuery& query,
    const EventStoreIdentity& expectedIdentity = {});

// JSON fields, including their leading comma. Invalid current count is null;
// a previous successful value is separately identified as diagnostic history.
enum class MqttStatsHealthSection { Events, FullBacklog };
std::string mqttEventStatsHealthFields(const EventStatsCacheEntry& entry,
    MqttStatsHealthSection section = MqttStatsHealthSection::Events);

} // namespace edge_gateway
