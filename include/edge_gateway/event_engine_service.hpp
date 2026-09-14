#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/event_commit_sink.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"
#include "edge_gateway/mqtt_event_outbox.hpp"
#include "edge_gateway/point_store_router.hpp"
#include "edge_gateway/sqlite_alarm_writer.hpp"

namespace edge_gateway {

class EventEngineService {
public:
    EventEngineService(
        EventEngineConfig eventConfig,
        MqttConfig mqttConfig,
        std::vector<DeviceConfig> deviceConfigs,
        PointStoreRouter& router,
        std::vector<MemoryPointStore*> stores,
        std::shared_ptr<IMqttDriverPublisher> publisher,
        std::unique_ptr<MqttEventOutbox> eventOutbox = nullptr,
        std::unique_ptr<SqliteAlarmWriter> alarmWriter = nullptr,
        MqttForwardConfig mqttForwardConfig = MqttForwardConfig(),
        std::shared_ptr<IEventCommitSink> commitSink = nullptr,
        std::string eventStoreGeneration = std::string()
    );
    ~EventEngineService();

    EventEngineService(const EventEngineService&) = delete;
    EventEngineService& operator=(const EventEngineService&) = delete;

    void start();
    void stop();
    bool isRunning() const;
    void runOnce(std::int64_t nowMs);
    bool drainPendingInputs(int timeoutMs);
    std::uint64_t rejectedInputCount() const { return rejectedInputCount_.load(); }
    std::uint64_t pendingInputCount() const { return pendingInputCount_.load(); }

private:
    struct ChangeState {
        double value = 0.0;
        int quality = 1;
        std::int64_t ts = 0;
        std::uint64_t generation = 0;
        bool initialized = false;
    };

    struct AlarmBinding {
        std::uint32_t index = 0;
        AlarmRuleConfig rule;
        std::string machineCode;
        std::string meterCode;
        std::string pointCode;
        std::string stateKey;
    };

    struct AlarmLifecycleState {
        bool active = false;
        double value = 0.0;
        int quality = 1;
        std::int64_t ts = 0;
        std::uint64_t generation = 0;
        bool initialized = false;
    };

    void loop();
    void evaluateValues(const std::vector<StoredPointValue>& values, std::int64_t nowMs);
    bool evaluateIpcValue(const StoredPointValue& value, std::int64_t nowMs, std::size_t& acceptedChanges);
    bool processPendingInputs(std::int64_t nowMs);
    Optional<StoredPointValue> buildValueFromUpdate(const PointUpdateRecord& update) const;
    void appendAlarmBindings(
        const std::string& machineCode,
        const std::string& meterCode,
        const std::vector<PointDefinition>& points
    );
    void restoreEventStates();
    void applyEventStates(const std::vector<MqttEventOutbox::EventState>& states);
    bool prepareCommitSink(std::int64_t nowMs);
    bool publishOrEnqueueEvents(
        const std::vector<MqttEventOutbox::EventMessage>& events,
        const std::vector<MqttEventOutbox::EventState>& states,
        const std::vector<EventStoreLocalEvent>& localEvents = {}
    );
    void appendChangeFanout(
        const StoredPointValue& value,
        std::uint64_t generation,
        std::vector<MqttEventOutbox::EventMessage>& events
    ) const;
    void appendAlarmFanout(
        const AlarmEvent& event,
        std::uint64_t generation,
        std::vector<MqttEventOutbox::EventMessage>& events
    ) const;
    bool mapForwardValue(StoredPointValue& value) const;
    bool mapForwardAlarm(AlarmEvent& event) const;
    bool forwardsIndex(std::uint32_t index) const;
    static std::string encodeAlarmPayload(
        const AlarmEvent& event,
        const std::string& eventId
    );
    static std::string encodeChangePayload(
        const StoredPointValue& value,
        const std::string& eventId
    );
    void processAlarms(const std::vector<StoredPointValue>& values, std::int64_t nowMs);
    void flushPendingAlarmPersistence(std::int64_t nowMs);
    void processChanges(const std::vector<StoredPointValue>& values, std::int64_t nowMs);
    void publishStatusEvent(
        const std::string& event,
        std::int64_t ts,
        const std::string& detailsJson = std::string()
    ) const;

    EventEngineConfig eventConfig_;
    MqttConfig mqttConfig_;
    MqttForwardConfig mqttForwardConfig_;
    PointStoreRouter& router_;
    std::vector<MemoryPointStore*> stores_;
    std::shared_ptr<IMqttDriverPublisher> publisher_;
    std::unique_ptr<MqttEventOutbox> eventOutbox_;
    std::unique_ptr<SqliteAlarmWriter> alarmWriter_;
    std::shared_ptr<IEventCommitSink> commitSink_;
    std::string eventStoreGeneration_;
    std::vector<MqttEventOutbox::EventState> rebaseStates_;
    std::size_t rebaseOffset_ = 0;
    std::vector<StoredPointValue> pendingInputs_;
    std::size_t pendingInputOffset_ = 0;
    std::size_t nextStoreDrain_ = 0;
    std::atomic<std::uint64_t> rejectedInputCount_{0};
    std::atomic<std::uint64_t> pendingInputCount_{0};
    bool collectingCommit_ = false;
    std::vector<MqttEventOutbox::EventMessage> stagedEvents_;
    std::vector<MqttEventOutbox::EventState> stagedStates_;
    std::vector<EventStoreLocalEvent> stagedLocalEvents_;
    std::unordered_map<std::uint32_t, ChangeState> stagedChangeStates_;
    std::unordered_map<std::string, AlarmLifecycleState> stagedAlarmStates_;
    std::size_t stagedChangeCount_ = 0;
    std::vector<AlarmEvent> pendingAlarmPersistence_;
    std::unordered_map<std::uint32_t, ChangeState> changeStates_;
    std::unordered_map<std::uint32_t, std::vector<AlarmBinding>> alarmBindingsByIndex_;
    std::unordered_map<std::string, AlarmLifecycleState> alarmStates_;
    std::unordered_set<std::uint32_t> forwardEventIndexes_;
    std::unordered_map<std::uint32_t, LegacyTelemetryPointMapping> legacyMappingsByIndex_;
    std::unordered_map<MemoryPointStore*, std::uint64_t> lastUpdateSequenceByStore_;
    std::int64_t lastFallbackScanMs_ = 0;
    std::int64_t nextAlarmPersistenceAttemptMs_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace edge_gateway
