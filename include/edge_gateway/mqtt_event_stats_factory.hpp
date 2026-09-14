#pragma once

#include "edge_gateway/mqtt_event_stats.hpp"

#include <memory>

namespace edge_gateway {

// Call after the legacy writer has opened the same configured database.
// Starts a read-only worker; failure is returned as an error-only source.
std::unique_ptr<IEventStatsSource> makeLegacyMqttEventStatsSource(
    const MqttConfig& config, std::vector<EventStatsQuery> queries);

} // namespace edge_gateway
