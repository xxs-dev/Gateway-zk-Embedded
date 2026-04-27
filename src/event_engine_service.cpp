#include "edge_gateway/event_engine_service.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace edge_gateway {

namespace {

std::string escapeJson(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    escaped += '?';
                } else {
                    escaped += ch;
                }
                break;
        }
    }
    return escaped;
}

void sleepInterruptibly(const std::atomic<bool>& running, int intervalMs) {
    int remaining = std::max(0, intervalMs);
    while (running.load() && remaining > 0) {
        const int slice = std::min(remaining, 50);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        remaining -= slice;
    }
}

}  // namespace

EventEngineService::EventEngineService(
    EventEngineConfig eventConfig,
    MqttConfig mqttConfig,
    std::vector<DeviceConfig> deviceConfigs,
    PointStoreRouter& router,
    std::vector<MemoryPointStore*> stores,
    std::shared_ptr<IMqttDriverPublisher> publisher,
    std::unique_ptr<MqttEventOutbox> eventOutbox,
    std::unique_ptr<SqliteAlarmWriter> alarmWriter
)
    : eventConfig_(std::move(eventConfig)),
      mqttConfig_(std::move(mqttConfig)),
      alarmService_(std::move(deviceConfigs)),
      router_(router),
      stores_(std::move(stores)),
      publisher_(std::move(publisher)),
      eventOutbox_(std::move(eventOutbox)),
      alarmWriter_(std::move(alarmWriter)) {
    if (!publisher_) {
        throw std::invalid_argument("event engine publisher is null");
    }
}

EventEngineService::~EventEngineService() {
    stop();
}

void EventEngineService::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    thread_ = std::thread(&EventEngineService::loop, this);
}

void EventEngineService::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool EventEngineService::isRunning() const {
    return running_.load();
}

void EventEngineService::runOnce(std::int64_t nowMs) {
    std::vector<StoredPointValue> values;
    const std::size_t limit = std::max<std::size_t>(1, eventConfig_.updateDrainBatchSize);

    for (auto* store : stores_) {
        if (store == nullptr) {
            continue;
        }
        const auto updates = store->drainPointUpdates(limit);
        for (const auto& update : updates) {
            auto value = buildValueFromUpdate(update);
            if (value) {
                values.push_back(*value);
            }
        }
    }

    if (!values.empty()) {
        evaluateValues(values, nowMs);
        return;
    }

    const int fallbackMs = std::max(1000, eventConfig_.scanFallbackIntervalMs);
    if (lastFallbackScanMs_ == 0 || nowMs - lastFallbackScanMs_ >= fallbackMs) {
        evaluateValues(router_.getAllLatest(nowMs), nowMs);
        lastFallbackScanMs_ = nowMs;
    }
}

void EventEngineService::loop() {
    const auto intervalMs = std::max(20, eventConfig_.scanIntervalMs);
    while (running_.load()) {
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        try {
            runOnce(nowMs);
        } catch (const std::exception& ex) {
            std::cerr << "event engine scan failed error=" << ex.what() << std::endl;
            publishStatusEvent(
                "event-engine-scan-failed",
                nowMs,
                std::string(R"("message":")") + ex.what() + R"(")"
            );
        }
        sleepInterruptibly(running_, intervalMs);
    }
}

Optional<StoredPointValue> EventEngineService::buildValueFromUpdate(const PointUpdateRecord& update) const {
    const auto route = router_.routeByIndex(update.index);
    if (!route) {
        return NullOpt;
    }

    StoredPointValue value;
    value.index = update.index;
    value.machineCode = route->machineCode;
    value.meterCode = route->meterCode;
    value.pointCode = route->pointCode;
    value.value = update.value;
    value.quality = update.quality;
    value.ts = update.ts;
    value.expireAt = update.expireAt;
    value.stale = false;
    return value;
}

void EventEngineService::publishOrEnqueueEvent(
    const std::string& eventType,
    const std::string& topic,
    const std::string& payload,
    std::int64_t eventTs
) {
    if (topic.empty()) {
        return;
    }
    if (eventConfig_.publishMode == "mqtt_driver_outbox") {
        if (eventOutbox_) {
            eventOutbox_->enqueue(eventType, topic, payload, eventTs);
        }
        return;
    }
    publisher_->publishJsonMessage(topic, payload);
}

std::string EventEngineService::encodeAlarmPayload(const AlarmEvent& event) {
    std::ostringstream out;
    out << "{\"type\":\"alarm\""
        << ",\"machineCode\":\"" << escapeJson(event.machineCode) << "\""
        << ",\"meterCode\":\"" << escapeJson(event.meterCode) << "\""
        << ",\"index\":" << event.index
        << ",\"pointCode\":\"" << escapeJson(event.pointCode) << "\""
        << ",\"alarmType\":\"" << escapeJson(event.alarmType) << "\""
        << ",\"active\":" << (event.active ? "true" : "false")
        << ",\"value\":" << event.value
        << ",\"quality\":" << event.quality
        << ",\"ts\":" << event.ts
        << ",\"stale\":" << (event.stale ? "true" : "false")
        << "}";
    return out.str();
}

std::string EventEngineService::encodeChangePayload(const StoredPointValue& value) {
    std::ostringstream out;
    out << "{\"type\":\"change\""
        << ",\"machineCode\":\"" << escapeJson(value.machineCode) << "\""
        << ",\"meterCode\":\"" << escapeJson(value.meterCode) << "\""
        << ",\"index\":" << value.index
        << ",\"pointCode\":\"" << escapeJson(value.pointCode) << "\""
        << ",\"value\":" << value.value
        << ",\"quality\":" << value.quality
        << ",\"ts\":" << value.ts
        << ",\"expireAt\":" << value.expireAt
        << ",\"stale\":" << (value.stale ? "true" : "false")
        << "}";
    return out.str();
}

void EventEngineService::evaluateValues(const std::vector<StoredPointValue>& values, std::int64_t nowMs) {
    if (values.empty()) {
        return;
    }
    processChanges(values, nowMs);
    processAlarms(values);
}

void EventEngineService::processAlarms(const std::vector<StoredPointValue>& values) {
    const auto events = alarmService_.evaluate(values);
    if (events.empty()) {
        return;
    }

    std::vector<AlarmEvent> persistentEvents;
    persistentEvents.reserve(events.size());

    for (const auto& event : events) {
        publishOrEnqueueEvent(
            "alarm",
            mqttConfig_.alarmTopic,
            encodeAlarmPayload(event),
            event.ts
        );

        if (!event.persistValue.empty()) {
            persistentEvents.push_back(event);
        }
    }

    if (alarmWriter_ && !persistentEvents.empty()) {
        alarmWriter_->writeEvents(persistentEvents);
        publishStatusEvent(
            "alarm-persisted",
            persistentEvents.front().ts,
            std::string(R"("count":)") + std::to_string(persistentEvents.size())
        );
    }
}

void EventEngineService::processChanges(const std::vector<StoredPointValue>& values, std::int64_t nowMs) {
    std::unordered_map<std::uint32_t, StoredPointValue> pendingByIndex;
    std::vector<std::uint32_t> publishOrder;
    pendingByIndex.reserve(values.size());

    for (const auto& value : values) {
        const auto route = router_.routeByIndex(value.index);
        if (!route || !route->reportOnChange) {
            continue;
        }

        auto& state = changeStates_[value.index];
        if (!state.initialized) {
            state.value = value.value;
            state.quality = value.quality;
            state.ts = value.ts;
            state.initialized = true;
            continue;
        }

        if (state.value == value.value) {
            state.quality = value.quality;
            state.ts = value.ts;
            continue;
        }

        state.value = value.value;
        state.quality = value.quality;
        state.ts = value.ts;
        if (pendingByIndex.find(value.index) == pendingByIndex.end()) {
            publishOrder.push_back(value.index);
        }
        pendingByIndex[value.index] = value;
    }

    std::size_t published = 0;
    for (const auto index : publishOrder) {
        const auto it = pendingByIndex.find(index);
        if (it == pendingByIndex.end()) {
            continue;
        }
        publishOrEnqueueEvent(
            "change",
            mqttConfig_.changeEventTopic,
            encodeChangePayload(it->second),
            it->second.ts
        );
        ++published;
    }

    if (published > 0) {
        publishStatusEvent(
            "report-on-change",
            nowMs,
            std::string(R"("valueCount":)") + std::to_string(published)
        );
    }
}

void EventEngineService::publishStatusEvent(
    const std::string& event,
    std::int64_t ts,
    const std::string& detailsJson
) const {
    if (mqttConfig_.statusTopic.empty()) {
        return;
    }
    std::ostringstream payload;
    payload << "{\"service\":\"event-engine\",\"event\":\""
            << event
            << "\",\"ts\":"
            << ts;
    if (!detailsJson.empty()) {
        payload << "," << detailsJson;
    }
    payload << "}";
    publisher_->publishJsonMessage(mqttConfig_.statusTopic, payload.str());
}

}  // namespace edge_gateway
