#include "edge_gateway/event_store_stats.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace edge_gateway {
namespace {

void validateText(const std::string& text) {
    if (text.empty() || text.size() > 96 || text.find('\0') != std::string::npos) {
        throw std::invalid_argument("stats target/type must contain 1..96 bytes and no NUL");
    }
}

void normalizeTypes(std::vector<std::string>& types) {
    if (types.size() > 32) throw std::invalid_argument("stats type list exceeds 32 entries");
    for (const auto& type : types) validateText(type);
    std::sort(types.begin(), types.end());
    types.erase(std::unique(types.begin(), types.end()), types.end());
}

} // namespace

EventStatsScope normalizeEventStatsScope(EventStatsScope scope) {
    validateText(scope.targetId);
    if (scope.selection != EventStatsSelection::All && scope.selection != EventStatsSelection::Only) {
        throw std::invalid_argument("invalid stats selection");
    }
    if (scope.selection == EventStatsSelection::All && !scope.include.empty()) {
        throw std::invalid_argument("all stats selection requires empty include");
    }
    normalizeTypes(scope.include);
    normalizeTypes(scope.exclude);
    return scope;
}

bool sameEventStatsScope(const EventStatsScope& a, const EventStatsScope& b) {
    const auto left = normalizeEventStatsScope(a);
    const auto right = normalizeEventStatsScope(b);
    return left.targetId == right.targetId && left.selection == right.selection &&
        left.include == right.include && left.exclude == right.exclude;
}

} // namespace edge_gateway
