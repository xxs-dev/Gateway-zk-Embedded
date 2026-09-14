#include "edge_gateway/event_engine_service.hpp"
#include <cmath>

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

std::string normalizeAlarmType(const std::string& type) {
    if (type == "high" || type == "HIGH") {
        return "high";
    }
    if (type == "low" || type == "LOW") {
        return "low";
    }
    throw std::invalid_argument("unsupported alarm rule type: " + type);
}

bool isAlarmActive(const AlarmRuleConfig& rule, double value) {
    return rule.type == "high" ? value >= rule.threshold : value <= rule.threshold;
}

std::string changeStateKey(std::uint32_t index) {
    return "change:" + std::to_string(index);
}

std::string alarmStateKey(std::uint32_t index, const std::string& alarmType) {
    return "alarm:" + std::to_string(index) + ":" + alarmType;
}

std::uint64_t lifecycleGeneration(const std::string& lifecycle) {
    const auto separator = lifecycle.find_last_of(':');
    if (separator == std::string::npos || separator + 1 >= lifecycle.size()) {
        return 0;
    }
    try {
        return static_cast<std::uint64_t>(std::stoull(lifecycle.substr(separator + 1)));
    } catch (const std::exception&) {
        return 0;
    }
}

std::string scopedTopic(
    const std::string& topic,
    bool machineScoped,
    const std::string& machineCode
) {
    if (!machineScoped || topic.empty() || machineCode.empty()) {
        return topic;
    }
    const std::string suffix = "/" + machineCode;
    if (topic.size() >= suffix.size() &&
        topic.compare(topic.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return topic;
    }
    return topic + suffix;
}

std::string changeEventId(
    const StoredPointValue& value,
    std::uint64_t generation
) {
    return "change:v1:" + value.machineCode + ":" + std::to_string(value.index) + ":" +
        std::to_string(generation);
}

std::string alarmEventId(
    const AlarmEvent& event,
    std::uint64_t generation
) {
    return "alarm:v1:" + event.machineCode + ":" + std::to_string(event.index) + ":" +
        event.alarmType + ":" + std::to_string(generation);
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
    std::unique_ptr<SqliteAlarmWriter> alarmWriter,
    MqttForwardConfig mqttForwardConfig,
    std::shared_ptr<IEventCommitSink> commitSink,
    std::string eventStoreGeneration
)
    : eventConfig_(std::move(eventConfig)),
      mqttConfig_(std::move(mqttConfig)),
      mqttForwardConfig_(std::move(mqttForwardConfig)),
      router_(router),
      stores_(std::move(stores)),
      publisher_(std::move(publisher)),
      eventOutbox_(std::move(eventOutbox)),
      alarmWriter_(std::move(alarmWriter)),
      commitSink_(std::move(commitSink)), eventStoreGeneration_(std::move(eventStoreGeneration)) {
    if (commitSink_ && (eventOutbox_ || alarmWriter_ || eventStoreGeneration_.empty()))
        throw std::invalid_argument("IPC event engine forbids legacy writers and requires generation");
    if (!publisher_) {
        throw std::invalid_argument("event engine publisher is null");
    }
    for (const auto& config : deviceConfigs) {
        if (!config.meters.empty()) {
            for (const auto& meter : config.meters) {
                appendAlarmBindings(config.machineCode, meter.meterCode, meter.points);
            }
        }
        if (!config.points.empty()) {
            appendAlarmBindings(config.machineCode, config.meterCode, config.points);
        }
    }
    if (mqttForwardConfig_.events.enabled) {
        if (!commitSink_ && (eventConfig_.publishMode != "mqtt_driver_outbox" || !eventOutbox_)) {
            throw std::invalid_argument(
                "mqttForward.events requires eventEngine.publishMode=mqtt_driver_outbox"
            );
        }
        const auto& indexes = mqttForwardConfig_.events.pointIndexes.empty()
            ? mqttForwardConfig_.pointIndexes
            : mqttForwardConfig_.events.pointIndexes;
        forwardEventIndexes_.insert(indexes.begin(), indexes.end());
        for (const auto& mapping : mqttForwardConfig_.legacyTelemetryPointMappings) {
            legacyMappingsByIndex_[mapping.index] = mapping;
        }
    }
    restoreEventStates();
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
    if (commitSink_ && !prepareCommitSink(nowMs)) return;
    if (commitSink_ && !processPendingInputs(nowMs)) return;
    flushPendingAlarmPersistence(nowMs);
    std::vector<StoredPointValue> values;
    const std::size_t limit = std::max<std::size_t>(1, eventConfig_.updateDrainBatchSize);
    std::size_t sequenceGapCount = 0;
    bool updateBacklog = false;

    const auto firstStore = commitSink_ && !stores_.empty() ? nextStoreDrain_ % stores_.size() : 0;
    for (std::size_t offset = 0; offset < stores_.size(); ++offset) {
        const auto position = (firstStore + offset) % stores_.size();
        auto* store = stores_[position];
        if (store == nullptr) {
            continue;
        }
        if (commitSink_ && values.size() >= limit) { updateBacklog = true; break; }
        if (commitSink_) nextStoreDrain_ = (position + 1) % stores_.size();
        std::vector<PointUpdateRecord> updates;
        try {
            updates = store->drainPointUpdates(commitSink_ ? limit - values.size() : limit);
        } catch (const std::exception& ex) {
            std::cerr << "event engine store drain failed error=" << ex.what() << std::endl;
            publishStatusEvent(
                "point-update-drain-failed",
                nowMs,
                std::string(R"("message":")") + escapeJson(ex.what()) + R"(")"
            );
            continue;
        }
        auto& lastSequence = lastUpdateSequenceByStore_[store];
        for (const auto& update : updates) {
            if ((lastSequence == 0 && update.sequence > 1) ||
                (lastSequence > 0 && update.sequence != lastSequence + 1)) {
                ++sequenceGapCount;
            }
            lastSequence = update.sequence;
            auto value = buildValueFromUpdate(update);
            if (value) {
                values.push_back(*value);
            }
        }
        try {
            updateBacklog = updateBacklog || store->getStats().pointUpdateCount > 0;
        } catch (const std::exception& ex) {
            updateBacklog = true;
            std::cerr << "event engine store backlog check failed error=" << ex.what() << std::endl;
        }
    }

    if (!values.empty()) {
        if (commitSink_) {
            pendingInputs_ = std::move(values);
            if (!processPendingInputs(nowMs)) return;
        } else evaluateValues(values, nowMs);
    }

    const int fallbackMs = std::max(1000, eventConfig_.scanFallbackIntervalMs);
    const bool fallbackDue = lastFallbackScanMs_ == 0 || nowMs - lastFallbackScanMs_ >= fallbackMs;
    if (sequenceGapCount > 0) {
        publishStatusEvent(
            "point-update-sequence-gap",
            nowMs,
            std::string(R"("gapCount":)") + std::to_string(sequenceGapCount)
        );
    }
    if ((fallbackDue || sequenceGapCount > 0) && !updateBacklog) {
        auto candidate = router_.getAllLatest(nowMs);
        if (!router_.usesOnlyStores(stores_)) return;
        // Capture before rechecking: putLatest writes latest and FIFO under one
        // lock. Later enqueues cannot be in this candidate; earlier ones must
        // drain first. The pre-evaluation backlog sample is no longer current.
        for (auto* store : stores_) {
            if (!store) continue;
            try {
                if (store->getStats().pointUpdateCount > 0) return;
            } catch (const std::exception& ex) {
                std::cerr << "event engine fallback backlog check failed error=" << ex.what() << std::endl;
                return;
            }
        }
        if (commitSink_) {
            pendingInputs_ = std::move(candidate);
            if (!processPendingInputs(nowMs)) return;
        } else evaluateValues(candidate, nowMs);
        lastFallbackScanMs_ = nowMs;
    }
}

void EventEngineService::loop() {
    const auto intervalMs = std::max(10, eventConfig_.scanIntervalMs);
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
                std::string(R"("message":")") + escapeJson(ex.what()) + R"(")"
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

void EventEngineService::appendAlarmBindings(
    const std::string& machineCode,
    const std::string& meterCode,
    const std::vector<PointDefinition>& points
) {
    for (const auto& point : points) {
        for (const auto& configuredRule : point.alarms) {
            AlarmBinding binding;
            binding.index = point.index;
            binding.rule = configuredRule;
            binding.rule.type = normalizeAlarmType(configuredRule.type);
            binding.machineCode = machineCode;
            binding.meterCode = meterCode;
            binding.pointCode = point.pointCode;
            binding.stateKey = alarmStateKey(point.index, binding.rule.type);
            alarmBindingsByIndex_[point.index].push_back(std::move(binding));
        }
    }
}

void EventEngineService::restoreEventStates() {
    if (!eventOutbox_) {
        return;
    }
    applyEventStates(eventOutbox_->loadStates());
}

void EventEngineService::applyEventStates(const std::vector<MqttEventOutbox::EventState>& states) {
    for (const auto& persisted : states) {
        if (persisted.eventType == "change") {
            const auto route = router_.routeByIndex(persisted.index);
            if (!route || !route->reportOnChange) {
                continue;
            }
            ChangeState state;
            state.value = persisted.value;
            state.quality = persisted.quality;
            state.ts = persisted.sourceTs;
            state.generation = lifecycleGeneration(persisted.lifecycle);
            state.initialized = true;
            changeStates_[persisted.index] = state;
            continue;
        }
        if (persisted.eventType != "alarm") {
            continue;
        }
        std::string normalizedType;
        try {
            normalizedType = normalizeAlarmType(persisted.alarmType);
        } catch (const std::exception&) {
            continue;
        }
        const auto bindings = alarmBindingsByIndex_.find(persisted.index);
        if (bindings == alarmBindingsByIndex_.end()) {
            continue;
        }
        const auto binding = std::find_if(
            bindings->second.begin(),
            bindings->second.end(),
            [&](const AlarmBinding& candidate) { return candidate.rule.type == normalizedType; }
        );
        if (binding == bindings->second.end()) {
            continue;
        }
        AlarmLifecycleState state;
        state.active = persisted.active;
        state.value = persisted.value;
        state.quality = persisted.quality;
        state.ts = persisted.sourceTs;
        state.generation = lifecycleGeneration(persisted.lifecycle);
        state.initialized = true;
        alarmStates_[binding->stateKey] = state;
    }
}

bool EventEngineService::prepareCommitSink(std::int64_t nowMs) {
    std::vector<MqttEventOutbox::EventState> restored;
    if (commitSink_->takeBaseline(restored)) {
        changeStates_.clear(); alarmStates_.clear();
        applyEventStates(restored);
        rebaseStates_.clear(); rebaseOffset_ = 0;
        if (commitSink_->status().gaps > 0 && pendingInputs_.empty()) {
            // A gap gives no evidence of intermediate transitions. Commit only the
            // current baseline, preserving lifecycle counters for subsequent events.
            for (const auto& value : router_.getAllLatest(nowMs)) {
                if (value.stale || value.quality != 1 || !std::isfinite(value.value) || value.ts <= 0) continue;
                const auto route = router_.routeByIndex(value.index);
                if (route && route->reportOnChange) {
                    MqttEventOutbox::EventState s;
                    s.stateKey = changeStateKey(value.index); s.eventType = "change";
                    s.index = value.index; s.value = value.value; s.quality = value.quality;
                    s.sourceTs = value.ts;
                    s.lifecycle = "value:" + std::to_string(changeStates_[value.index].generation);
                    rebaseStates_.push_back(std::move(s));
                }
                const auto bindings = alarmBindingsByIndex_.find(value.index);
                if (bindings == alarmBindingsByIndex_.end()) continue;
                for (const auto& binding : bindings->second) {
                    MqttEventOutbox::EventState s;
                    s.stateKey = binding.stateKey; s.eventType = "alarm"; s.index = value.index;
                    s.alarmType = binding.rule.type; s.active = isAlarmActive(binding.rule, value.value);
                    s.value = value.value; s.quality = value.quality; s.sourceTs = value.ts;
                    s.lifecycle = std::string(s.active ? "raised:" : "cleared:") +
                        std::to_string(alarmStates_[binding.stateKey].generation);
                    rebaseStates_.push_back(std::move(s));
                }
            }
        }
        commitSink_->beginBaselineUpdate(rebaseStates_.size());
    }
    if (commitSink_->status().phase == EventCommitPhase::Rebase) {
        while (rebaseOffset_ < rebaseStates_.size()) {
            const auto end = std::min(rebaseStates_.size(), rebaseOffset_ + 32);
            std::vector<MqttEventOutbox::EventState> batch(rebaseStates_.begin() + rebaseOffset_,
                rebaseStates_.begin() + end);
            if (!commitSink_->trySubmitBaseline(batch)) return false;
            applyEventStates(batch); rebaseOffset_ = end;
        }
        if (!commitSink_->resumeAfterBaseline()) return false;
        rebaseStates_.clear(); rebaseOffset_ = 0;
    }
    return commitSink_->status().phase == EventCommitPhase::Normal;
}

bool EventEngineService::publishOrEnqueueEvents(
    const std::vector<MqttEventOutbox::EventMessage>& events,
    const std::vector<MqttEventOutbox::EventState>& states,
    const std::vector<EventStoreLocalEvent>& localEvents
) {
    if (events.empty() && states.empty() && localEvents.empty()) return true;
    if (collectingCommit_) {
        stagedEvents_.insert(stagedEvents_.end(), events.begin(), events.end());
        stagedStates_.insert(stagedStates_.end(), states.begin(), states.end());
        stagedLocalEvents_.insert(stagedLocalEvents_.end(), localEvents.begin(), localEvents.end());
        return true;
    }
    if (commitSink_) return commitSink_->trySubmit(events, states, localEvents);
    if (eventOutbox_) {
        // One outer transaction removes the crash gap between primary and third-party
        // fanout; per-target savepoints keep optional target failures isolated.
        const auto result = eventOutbox_->enqueueFanoutWithStates(events, states);
        for (const auto& targetId : result.failedTargetIds) {
            std::cerr << "event engine forward enqueue failed target="
                      << targetId << " error=isolated target transaction rolled back"
                      << std::endl;
        }
        return true;
    }
    for (const auto& event : events) {
        if ((event.targetId.empty() || event.targetId == "main") && !event.topic.empty()) {
            publisher_->publishJsonMessage(event.topic, event.payload);
        }
    }
    return true;
}

bool EventEngineService::forwardsIndex(std::uint32_t index) const {
    return mqttForwardConfig_.events.enabled &&
        forwardEventIndexes_.find(index) != forwardEventIndexes_.end();
}

bool EventEngineService::mapForwardValue(StoredPointValue& value) const {
    if (mqttForwardConfig_.payloadFormat != "legacy") {
        return true;
    }
    const auto mapping = legacyMappingsByIndex_.find(value.index);
    if (mapping == legacyMappingsByIndex_.end()) {
        return !(mqttForwardConfig_.legacyTelemetryMappedOnly &&
            !legacyMappingsByIndex_.empty());
    }
    value.meterCode = mapping->second.meterCode;
    value.pointCode = mapping->second.pointCode;
    return true;
}

bool EventEngineService::mapForwardAlarm(AlarmEvent& event) const {
    if (mqttForwardConfig_.payloadFormat != "legacy") {
        return true;
    }
    const auto mapping = legacyMappingsByIndex_.find(event.index);
    if (mapping == legacyMappingsByIndex_.end()) {
        return !(mqttForwardConfig_.legacyTelemetryMappedOnly &&
            !legacyMappingsByIndex_.empty());
    }
    event.meterCode = mapping->second.meterCode;
    event.pointCode = mapping->second.pointCode;
    return true;
}

void EventEngineService::appendChangeFanout(
    const StoredPointValue& value,
    std::uint64_t generation,
    std::vector<MqttEventOutbox::EventMessage>& events
) const {
    const auto eventId = changeEventId(value, generation);
    if ((!commitSink_ || mqttConfig_.enabled) && !mqttConfig_.changeEventTopic.empty()) {
        MqttEventOutbox::EventMessage main;
        main.eventType = "change";
        main.topic = mqttConfig_.changeEventTopic;
        main.payload = encodeChangePayload(value, eventId);
        main.eventTs = value.ts;
        main.eventId = eventId;
        main.targetId = "main";
        events.push_back(std::move(main));
    }
    if ((commitSink_ && !mqttForwardConfig_.enabled) || !forwardsIndex(value.index) || mqttForwardConfig_.events.changeTopic.empty()) {
        return;
    }
    auto forwarded = value;
    if (!mapForwardValue(forwarded)) {
        return;
    }
    MqttEventOutbox::EventMessage thirdParty;
    thirdParty.eventType = "change";
    thirdParty.topic = scopedTopic(
        mqttForwardConfig_.events.changeTopic,
        mqttForwardConfig_.events.changeTopicMachineScoped,
        value.machineCode
    );
    thirdParty.payload = encodeChangePayload(forwarded, eventId);
    thirdParty.eventTs = value.ts;
    thirdParty.eventId = eventId;
    thirdParty.targetId = mqttForwardConfig_.events.targetId;
    events.push_back(std::move(thirdParty));
}

void EventEngineService::appendAlarmFanout(
    const AlarmEvent& event,
    std::uint64_t generation,
    std::vector<MqttEventOutbox::EventMessage>& events
) const {
    const auto eventId = event.eventId.empty() ? alarmEventId(event, generation) : event.eventId;
    if ((!commitSink_ || mqttConfig_.enabled) && !mqttConfig_.alarmTopic.empty()) {
        MqttEventOutbox::EventMessage main;
        main.eventType = "alarm";
        main.topic = mqttConfig_.alarmTopic;
        main.payload = encodeAlarmPayload(event, eventId);
        main.eventTs = event.ts;
        main.eventId = eventId;
        main.targetId = "main";
        events.push_back(std::move(main));
    }
    if ((commitSink_ && !mqttForwardConfig_.enabled) || !forwardsIndex(event.index) || mqttForwardConfig_.events.alarmTopic.empty()) {
        return;
    }
    auto forwarded = event;
    if (!mapForwardAlarm(forwarded)) {
        return;
    }
    MqttEventOutbox::EventMessage thirdParty;
    thirdParty.eventType = "alarm";
    thirdParty.topic = scopedTopic(
        mqttForwardConfig_.events.alarmTopic,
        mqttForwardConfig_.events.alarmTopicMachineScoped,
        event.machineCode
    );
    thirdParty.payload = encodeAlarmPayload(forwarded, eventId);
    thirdParty.eventTs = event.ts;
    thirdParty.eventId = eventId;
    thirdParty.targetId = mqttForwardConfig_.events.targetId;
    events.push_back(std::move(thirdParty));
}

std::string EventEngineService::encodeAlarmPayload(
    const AlarmEvent& event,
    const std::string& eventId
) {
    std::ostringstream out;
    out << "{\"schemaVersion\":\"2.0\""
        << ",\"eventId\":\"" << escapeJson(eventId) << "\""
        << ",\"type\":\"alarm\""
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

std::string EventEngineService::encodeChangePayload(
    const StoredPointValue& value,
    const std::string& eventId
) {
    std::ostringstream out;
    out << "{\"schemaVersion\":\"2.0\""
        << ",\"eventId\":\"" << escapeJson(eventId) << "\""
        << ",\"type\":\"change\""
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
    processAlarms(values, nowMs);
}

bool EventEngineService::evaluateIpcValue(const StoredPointValue& value, std::int64_t nowMs,
    std::size_t& acceptedChanges) {
    if (commitSink_->status().phase != EventCommitPhase::Normal) return false;
    // Invalid measurements cannot prove either a physical alarm or its recovery.
    if (value.stale || value.quality != 1) return true;
    if (!std::isfinite(value.value) || value.ts <= 0) {
        ++rejectedInputCount_;
        publishStatusEvent("input-rejected", nowMs, std::string(R"("index":)") +
            std::to_string(value.index) + R"(,"reason":"invalid measurement or timestamp")");
        return true;
    }
    stagedEvents_.clear(); stagedStates_.clear(); stagedLocalEvents_.clear();
    stagedChangeStates_.clear(); stagedAlarmStates_.clear(); stagedChangeCount_ = 0;
    collectingCommit_ = true;
    try { evaluateValues({value}, nowMs); }
    catch (...) { collectingCommit_ = false; throw; }
    collectingCommit_ = false;
    if (!publishOrEnqueueEvents(stagedEvents_, stagedStates_, stagedLocalEvents_)) return false;
    for (const auto& state : stagedChangeStates_) changeStates_[state.first] = state.second;
    for (const auto& state : stagedAlarmStates_) alarmStates_[state.first] = state.second;
    acceptedChanges += stagedChangeCount_;
    return true;
}

bool EventEngineService::processPendingInputs(std::int64_t nowMs) {
    struct ChangeReport {
        EventEngineService& service;
        std::int64_t nowMs;
        std::size_t count = 0;
        ~ChangeReport() {
            if (!count) return;
            // Flush the accepted prefix on every exit. Diagnostic failure must
            // never roll back candidate states or repeat accepted input.
            try {
                service.publishStatusEvent("report-on-change", nowMs,
                    std::string(R"("valueCount":)") + std::to_string(count));
            } catch (...) {}
        }
    } report{*this, nowMs};
    pendingInputCount_ = pendingInputs_.size() - pendingInputOffset_;
    while (pendingInputOffset_ < pendingInputs_.size()) {
        if (!evaluateIpcValue(pendingInputs_[pendingInputOffset_], nowMs, report.count)) {
            const auto status = commitSink_->status();
            if (status.phase != EventCommitPhase::Normal) return false;
            // A normal-phase refusal is local batch validation, not backpressure.
            // Quarantine this sample without advancing its candidate state.
            ++rejectedInputCount_;
            std::cerr << "event engine input rejected index=" << pendingInputs_[pendingInputOffset_].index
                      << " error=" << status.error << std::endl;
        }
        ++pendingInputOffset_;
        pendingInputCount_ = pendingInputs_.size() - pendingInputOffset_;
    }
    pendingInputs_.clear(); pendingInputOffset_ = 0;
    return true;
}

bool EventEngineService::drainPendingInputs(int timeoutMs) {
    if (!commitSink_) return true;
    if (running_.load()) throw std::logic_error("stop detection before draining pending inputs");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do {
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (prepareCommitSink(nowMs) && processPendingInputs(nowMs)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return pendingInputs_.empty();
}

void EventEngineService::processAlarms(
    const std::vector<StoredPointValue>& values,
    std::int64_t nowMs
) {
    std::vector<AlarmEvent> persistentEvents;
    std::vector<EventStoreLocalEvent> localEvents;
    std::vector<MqttEventOutbox::EventMessage> mqttEvents;
    std::unordered_map<std::string, AlarmLifecycleState> nextStates;
    std::unordered_map<std::string, const AlarmBinding*> dirtyBindings;

    for (const auto& value : values) {
        const auto bindings = alarmBindingsByIndex_.find(value.index);
        if (bindings == alarmBindingsByIndex_.end()) {
            continue;
        }
        for (const auto& binding : bindings->second) {
            auto next = nextStates.find(binding.stateKey);
            AlarmLifecycleState state;
            if (next != nextStates.end()) {
                state = next->second;
            } else {
                const auto current = alarmStates_.find(binding.stateKey);
                if (current != alarmStates_.end()) {
                    state = current->second;
                }
            }
            if (state.initialized && value.ts < state.ts) {
                continue;
            }

            const bool activeNow = isAlarmActive(binding.rule, value.value);
            bool emitEvent = false;
            if (!state.initialized) {
                state.initialized = true;
                state.active = activeNow;
                dirtyBindings[binding.stateKey] = &binding;
                if (activeNow) {
                    ++state.generation;
                    emitEvent = true;
                }
            } else if (state.active != activeNow) {
                state.active = activeNow;
                ++state.generation;
                dirtyBindings[binding.stateKey] = &binding;
                emitEvent = activeNow || binding.rule.reportRecovery;
            }
            state.value = value.value;
            state.quality = value.quality;
            state.ts = value.ts;
            nextStates[binding.stateKey] = state;

            if (!emitEvent) {
                continue;
            }
            AlarmEvent event;
            event.index = value.index;
            event.machineCode = binding.machineCode;
            event.meterCode = binding.meterCode;
            event.pointCode = binding.pointCode;
            event.alarmType = binding.rule.type;
            event.active = activeNow;
            event.threshold = binding.rule.threshold;
            event.value = value.value;
            event.quality = value.quality;
            event.ts = value.ts;
            event.stale = value.stale;
            event.persistValue = binding.rule.persistValue;
            event.eventId = alarmEventId(event, state.generation);
            if (commitSink_) localEvents.push_back({"alarm", event,
                static_cast<std::int64_t>(state.generation), eventStoreGeneration_});
            appendAlarmFanout(event, state.generation, mqttEvents);
            if (!event.persistValue.empty()) {
                persistentEvents.push_back(event);
            }
        }
    }

    if (alarmWriter_ && !persistentEvents.empty()) {
        pendingAlarmPersistence_.insert(
            pendingAlarmPersistence_.end(),
            persistentEvents.begin(),
            persistentEvents.end()
        );
        flushPendingAlarmPersistence(nowMs);
    }

    std::vector<MqttEventOutbox::EventState> persistedStates;
    persistedStates.reserve(dirtyBindings.size());
    for (const auto& dirty : dirtyBindings) {
        const auto state = nextStates.find(dirty.first);
        if (state == nextStates.end()) {
            continue;
        }
        MqttEventOutbox::EventState persisted;
        persisted.stateKey = dirty.first;
        persisted.eventType = "alarm";
        persisted.index = dirty.second->index;
        persisted.alarmType = dirty.second->rule.type;
        persisted.active = state->second.active;
        persisted.value = state->second.value;
        persisted.quality = state->second.quality;
        persisted.sourceTs = state->second.ts;
        persisted.lifecycle = std::string(state->second.active ? "raised:" : "cleared:") +
            std::to_string(state->second.generation);
        persistedStates.push_back(std::move(persisted));
    }

    if (!publishOrEnqueueEvents(mqttEvents, persistedStates, localEvents)) return;
    if (collectingCommit_) { stagedAlarmStates_ = std::move(nextStates); return; }
    for (const auto& state : nextStates) {
        alarmStates_[state.first] = state.second;
    }
}

void EventEngineService::flushPendingAlarmPersistence(std::int64_t nowMs) {
    if (!alarmWriter_ || pendingAlarmPersistence_.empty() ||
        (nextAlarmPersistenceAttemptMs_ > 0 && nowMs < nextAlarmPersistenceAttemptMs_)) {
        return;
    }
    try {
        alarmWriter_->writeEvents(pendingAlarmPersistence_);
        const auto persisted = pendingAlarmPersistence_.size();
        pendingAlarmPersistence_.clear();
        nextAlarmPersistenceAttemptMs_ = 0;
        publishStatusEvent(
            "alarm-persisted",
            nowMs,
            std::string(R"("count":)") + std::to_string(persisted)
        );
    } catch (const std::exception& ex) {
        nextAlarmPersistenceAttemptMs_ = nowMs + 1000;
        std::cerr << "event engine alarm persistence failed error=" << ex.what() << std::endl;
        publishStatusEvent(
            "alarm-persist-failed",
            nowMs,
            std::string(R"("pendingCount":)") + std::to_string(pendingAlarmPersistence_.size()) +
                R"(,"message":")" + escapeJson(ex.what()) + R"(")"
        );
    }
}

void EventEngineService::processChanges(const std::vector<StoredPointValue>& values, std::int64_t nowMs) {
    if (eventConfig_.deliveryMode == "periodic") {
        return;
    }

    std::unordered_map<std::uint32_t, ChangeState> nextStates;
    std::unordered_set<std::uint32_t> dirtyIndexes;
    std::vector<MqttEventOutbox::EventMessage> mqttEvents;
    std::vector<EventStoreLocalEvent> localEvents;
    std::size_t transitionCount = 0;

    for (const auto& value : values) {
        const auto route = router_.routeByIndex(value.index);
        if (!route || !route->reportOnChange) {
            continue;
        }

        ChangeState state;
        const auto pendingState = nextStates.find(value.index);
        if (pendingState != nextStates.end()) {
            state = pendingState->second;
        } else {
            const auto currentState = changeStates_.find(value.index);
            if (currentState != changeStates_.end()) {
                state = currentState->second;
            }
        }
        if (state.initialized && value.ts < state.ts) {
            continue;
        }
        if (!state.initialized) {
            state.value = value.value;
            state.quality = value.quality;
            state.ts = value.ts;
            state.initialized = true;
            nextStates[value.index] = state;
            dirtyIndexes.insert(value.index);
            continue;
        }

        if (state.value == value.value) {
            state.quality = value.quality;
            state.ts = value.ts;
            nextStates[value.index] = state;
            continue;
        }

        state.value = value.value;
        state.quality = value.quality;
        state.ts = value.ts;
        ++state.generation;
        nextStates[value.index] = state;
        dirtyIndexes.insert(value.index);
        appendChangeFanout(value, state.generation, mqttEvents);
        if (commitSink_) {
            AlarmEvent event;
            event.index = value.index; event.machineCode = value.machineCode;
            event.meterCode = value.meterCode; event.pointCode = value.pointCode;
            event.value = value.value; event.quality = value.quality;
            event.ts = value.ts; event.stale = value.stale;
            event.eventId = changeEventId(value, state.generation);
            localEvents.push_back({"change", event, static_cast<std::int64_t>(state.generation),
                eventStoreGeneration_});
        }
        ++transitionCount;
    }

    std::vector<MqttEventOutbox::EventState> persistedStates;
    persistedStates.reserve(dirtyIndexes.size());
    for (const auto index : dirtyIndexes) {
        const auto state = nextStates.find(index);
        if (state == nextStates.end()) {
            continue;
        }
        MqttEventOutbox::EventState persisted;
        persisted.stateKey = changeStateKey(index);
        persisted.eventType = "change";
        persisted.index = index;
        persisted.value = state->second.value;
        persisted.quality = state->second.quality;
        persisted.sourceTs = state->second.ts;
        persisted.lifecycle = "value:" + std::to_string(state->second.generation);
        persistedStates.push_back(std::move(persisted));
    }

    if (!publishOrEnqueueEvents(mqttEvents, persistedStates, localEvents)) return;
    if (collectingCommit_) {
        stagedChangeStates_ = std::move(nextStates); stagedChangeCount_ = transitionCount; return;
    }
    for (const auto& state : nextStates) {
        changeStates_[state.first] = state.second;
    }

    if (transitionCount > 0) {
        publishStatusEvent(
            "report-on-change",
            nowMs,
            std::string(R"("valueCount":)") + std::to_string(transitionCount)
        );
    }
}

void EventEngineService::publishStatusEvent(
    const std::string& event,
    std::int64_t ts,
    const std::string& detailsJson
) const {
    if (!mqttConfig_.enabled || mqttConfig_.statusTopic.empty()) {
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
    try {
        publisher_->publishJsonMessage(mqttConfig_.statusTopic, payload.str());
    } catch (const std::exception& ex) {
        std::cerr << "event engine status publish failed event=" << event
                  << " error=" << ex.what() << std::endl;
    } catch (...) {
        std::cerr << "event engine status publish failed event=" << event
                  << " error=unknown" << std::endl;
    }
}

}  // namespace edge_gateway
