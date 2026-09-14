#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace edge_gateway {

enum class EventStatsSelection { All, Only };

struct EventStatsScope {
    std::string targetId;
    EventStatsSelection selection = EventStatsSelection::All;
    std::vector<std::string> include;
    std::vector<std::string> exclude;
};

// Canonicalizes bounded type lists. Only + empty include means no events, not all.
EventStatsScope normalizeEventStatsScope(EventStatsScope scope);
bool sameEventStatsScope(const EventStatsScope& a, const EventStatsScope& b);

struct EventPendingStats {
    std::int64_t pendingCount = 0;
    // Existing SQLite length(TEXT) counters: not UTF-8 bytes or a disk budget.
    std::int64_t pendingTextUnits = 0;
};

} // namespace edge_gateway
