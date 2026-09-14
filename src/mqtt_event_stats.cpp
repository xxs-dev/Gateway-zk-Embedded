#include "edge_gateway/mqtt_event_stats.hpp"

#include <stdexcept>

namespace edge_gateway {
namespace {

std::string boundedDiagnostic(const std::string& text) {
    std::string result;
    for (std::size_t i = 0; i < text.size() && result.size() < 512;) {
        const auto first = static_cast<unsigned char>(text[i]);
        if (first < 0x80) { result += text[i++]; continue; }
        unsigned code = 0, minimum = 0;
        std::size_t length = 0;
        if (first >= 0xc2 && first <= 0xdf) { code = first & 31; minimum = 0x80; length = 2; }
        else if (first >= 0xe0 && first <= 0xef) { code = first & 15; minimum = 0x800; length = 3; }
        else if (first >= 0xf0 && first <= 0xf4) { code = first & 7; minimum = 0x10000; length = 4; }
        bool valid = length != 0 && length <= text.size() - i;
        for (std::size_t j = 1; valid && j < length; ++j) {
            const auto next = static_cast<unsigned char>(text[i + j]);
            valid = (next & 0xc0) == 0x80;
            code = (code << 6) | (next & 63);
        }
        valid = valid && code >= minimum && code <= 0x10ffff && !(code >= 0xd800 && code <= 0xdfff);
        // A cache/backend can already have byte-truncated its diagnostic.
        // Replace invalid bytes, but never split a valid code point at the cap.
        if (!valid) { result += '?'; ++i; continue; }
        if (length > 512 - result.size()) break;
        result.append(text, i, length);
        i += length;
    }
    return result;
}

std::string quote(const std::string& text) {
    std::string result = "\"";
    static const char hex[] = "0123456789abcdef";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += static_cast<char>(c);
    }
    return result + '"';
}

std::string array(const std::vector<std::string>& values) {
    std::string result = "[";
    for (const auto& value : values) {
        if (result.size() > 1) result += ',';
        result += quote(value);
    }
    return result + ']';
}

const char* statusName(EventStatsStatus status) {
    switch (status) {
    case EventStatsStatus::NeverSampled: return "never-sampled";
    case EventStatsStatus::Fresh: return "fresh";
    case EventStatsStatus::Error: return "error";
    case EventStatsStatus::Stale: return "stale";
    case EventStatsStatus::Stopped: return "stopped";
    }
    return "error";
}

} // namespace

EventStatsQuery mqttDriverTotalStatsQuery() {
    return {"driver.total", "main-all-events", {"main", EventStatsSelection::All, {}, {}}};
}

EventStatsQuery mqttDriverBusinessStatsQuery() {
    return {"driver.business", "main-business-events", {"main", EventStatsSelection::Only, {"alarm", "change"}, {}}};
}

EventStatsQuery mqttForwarderStatsQuery(const MqttForwardEventConfig& config, bool awaitingDelegation) {
    EventStatsQuery query;
    query.key = awaitingDelegation ? "forwarder.waiting" : "forwarder.enabled";
    query.context = awaitingDelegation ? "awaiting-business-delegation" : "configured-business-topics";
    query.scope.targetId = config.targetId;
    query.scope.selection = EventStatsSelection::Only;
    if (awaitingDelegation || !config.alarmTopic.empty()) query.scope.include.push_back("alarm");
    if (awaitingDelegation || !config.changeTopic.empty()) query.scope.include.push_back("change");
    return query;
}

EventStatsCacheEntry readMqttEventStats(const IEventStatsSource* source, const EventStatsQuery& query,
    const EventStoreIdentity& expectedIdentity) {
    EventStatsCacheEntry result;
    result.query = query;
    result.identity = expectedIdentity;
    result.backend = expectedIdentity.storeId.empty() && expectedIdentity.configGeneration.empty()
        ? EventStatsBackend::Legacy : EventStatsBackend::Ipc;
    if (!source) {
        result.error = "event statistics source unavailable";
        return result;
    }
    try {
        auto sample = source->snapshot(query.key);
        const bool ipc = !expectedIdentity.storeId.empty() || !expectedIdentity.configGeneration.empty();
        if (sample.query.key != query.key || sample.query.context != query.context ||
            !sameEventStatsScope(normalizeEventStatsScope(sample.query.scope), normalizeEventStatsScope(query.scope)) ||
            sample.backend != (ipc ? EventStatsBackend::Ipc : EventStatsBackend::Legacy) ||
            (ipc && (expectedIdentity.storeId.empty() || expectedIdentity.configGeneration.empty())) ||
            sample.identity.storeId != expectedIdentity.storeId ||
            sample.identity.configGeneration != expectedIdentity.configGeneration) {
            throw std::runtime_error("event statistics source identity or scope mismatch");
        }
        if (sample.hasValue && (sample.value.pendingCount < 0 || sample.value.pendingTextUnits < 0 ||
                sample.ageMs < 0 || (sample.value.pendingCount == 0 && sample.value.pendingTextUnits != 0) ||
                (query.scope.selection == EventStatsSelection::Only &&
                 normalizeEventStatsScope(query.scope).include.empty() && sample.value.pendingCount != 0))) {
            throw std::runtime_error("invalid event statistics cached value");
        }
        result = std::move(sample);
        result.query = query;
        result.valid = result.valid && result.hasValue && result.status == EventStatsStatus::Fresh && result.error.empty();
        if (!result.valid && result.status == EventStatsStatus::Fresh) {
            result.status = EventStatsStatus::Error;
            result.error = "inconsistent event statistics freshness";
        }
        result.error = boundedDiagnostic(result.error);
    } catch (const std::exception& ex) {
        result.status = EventStatsStatus::Error;
        result.error = boundedDiagnostic(ex.what());
    } catch (...) {
        result.status = EventStatsStatus::Error;
        result.error = "event statistics snapshot failed";
    }
    return result;
}

std::string mqttEventStatsHealthFields(const EventStatsCacheEntry& entry, MqttStatsHealthSection section) {
    const std::string prefix = section == MqttStatsHealthSection::FullBacklog ? "fullBacklog" : "event";
    const auto key = [&](const char* suffix) { return ",\"" + prefix + suffix + "\":"; };
    return key("PendingCount") + (entry.valid ? std::to_string(entry.value.pendingCount) : "null") +
        key("PendingLastKnownCount") + (entry.hasValue ? std::to_string(entry.value.pendingCount) : "null") +
        key("StatsStatus") + quote(statusName(entry.status)) +
        key("StatsValid") + (entry.valid ? "true" : "false") +
        key("StatsHasValue") + (entry.hasValue ? "true" : "false") +
        key("StatsAgeMs") + (entry.hasValue ? std::to_string(entry.ageMs) : "null") +
        key("StatsSampledAtMs") + (entry.hasValue ? std::to_string(entry.sampledAtUnixMs) : "null") +
        key("StatsError") + quote(entry.error) +
        key("StatsBackend") + quote(entry.backend == EventStatsBackend::Ipc ? "ipc" : "legacy") +
        key("StatsStoreId") + quote(entry.identity.storeId) +
        key("StatsConfigGeneration") + quote(entry.identity.configGeneration) +
        key("StatsKey") + quote(entry.query.key) +
        key("StatsContext") + quote(entry.query.context) +
        key("StatsTargetId") + quote(entry.query.scope.targetId) +
        key("StatsSelection") + quote(entry.query.scope.selection == EventStatsSelection::All ? "all" : "only") +
        key("StatsInclude") + array(entry.query.scope.include) +
        key("StatsExclude") + array(entry.query.scope.exclude);
}

} // namespace edge_gateway
