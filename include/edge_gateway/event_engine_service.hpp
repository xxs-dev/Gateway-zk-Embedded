#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include "edge_gateway/alarm_service.hpp"
#include "edge_gateway/interfaces.hpp"
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
        std::unique_ptr<SqliteAlarmWriter> alarmWriter = nullptr
    );
    ~EventEngineService();

    EventEngineService(const EventEngineService&) = delete;
    EventEngineService& operator=(const EventEngineService&) = delete;

    void start();
    void stop();
    bool isRunning() const;
    void runOnce(std::int64_t nowMs);

private:
    struct ChangeState {
        double value = 0.0;
        int quality = 1;
        std::int64_t ts = 0;
        bool initialized = false;
    };

    void loop();
    void evaluateValues(const std::vector<StoredPointValue>& values, std::int64_t nowMs);
    Optional<StoredPointValue> buildValueFromUpdate(const PointUpdateRecord& update) const;
    void publishOrEnqueueEvent(
        const std::string& eventType,
        const std::string& topic,
        const std::string& payload,
        std::int64_t eventTs
    );
    static std::string encodeAlarmPayload(const AlarmEvent& event);
    static std::string encodeChangePayload(const StoredPointValue& value);
    void processAlarms(const std::vector<StoredPointValue>& values);
    void processChanges(const std::vector<StoredPointValue>& values, std::int64_t nowMs);
    void publishStatusEvent(
        const std::string& event,
        std::int64_t ts,
        const std::string& detailsJson = std::string()
    ) const;

    EventEngineConfig eventConfig_;
    MqttConfig mqttConfig_;
    AlarmService alarmService_;
    PointStoreRouter& router_;
    std::vector<MemoryPointStore*> stores_;
    std::shared_ptr<IMqttDriverPublisher> publisher_;
    std::unique_ptr<MqttEventOutbox> eventOutbox_;
    std::unique_ptr<SqliteAlarmWriter> alarmWriter_;
    std::unordered_map<std::uint32_t, ChangeState> changeStates_;
    std::int64_t lastFallbackScanMs_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace edge_gateway
