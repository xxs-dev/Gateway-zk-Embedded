#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/models.hpp"
#include "edge_gateway/mqtt_event_outbox.hpp"
#include "edge_gateway/mqtt_realtime_ring_buffer.hpp"

namespace edge_gateway {

class BuiltinMqttDriverPublisher : public IMqttDriverPublisher {
public:
    explicit BuiltinMqttDriverPublisher(MqttConfig config);
    ~BuiltinMqttDriverPublisher() override;

    BuiltinMqttDriverPublisher(const BuiltinMqttDriverPublisher&) = delete;
    BuiltinMqttDriverPublisher& operator=(const BuiltinMqttDriverPublisher&) = delete;

    void publishFullSnapshot(
        const std::string& topic,
        const std::vector<StoredPointValue>& values
    ) override;

    void publishAlarm(
        const std::string& topic,
        std::uint32_t index,
        const StoredPointValue& value,
        const std::string& alarmType,
        bool active
    ) override;

    void publishOnDemand(
        const std::string& topic,
        const std::vector<StoredPointValue>& values
    ) override;

    void publishChangeEvent(
        const std::string& topic,
        const StoredPointValue& value
    ) override;

    void publishCommandReply(
        const std::string& topic,
        const MqttCommandReply& reply
    ) override;

    void publishOtaReply(
        const std::string& topic,
        const OtaReply& reply
    ) override;

    void publishOtaStatus(
        const std::string& topic,
        const OtaStatus& status
    ) override;

    void publishJsonMessage(
        const std::string& topic,
        const std::string& payload
    ) override;

    std::vector<MqttIncomingMessage> pollIncoming(int timeoutMs) override;

    static bool parseIncomingPublishPacket(
        const MqttConfig& config,
        const std::vector<std::uint8_t>& packet,
        MqttIncomingMessage* message
    );

private:
    struct MqttConnectionHandle;

    void publishJson(const std::string& topic, const std::string& payload);
    void publishRealtimeJson(const std::string& topic, const std::string& payload);
    void publishEventJson(const std::string& eventType, const std::string& topic, const std::string& payload, std::int64_t eventTs);
    void sendJsonNow(const std::string& topic, const std::string& payload);
    void enqueueOffline(const std::string& topic, const std::string& payload);
    void flushOfflineBuffer(bool force);
    void replayOfflineBuffer();
    void ensureSubscriberConnected();
    void closeSubscriber();

    struct OfflineMessage {
        std::string topic;
        std::string payload;
    };

    MqttConfig config_;
    std::unique_ptr<MqttConnectionHandle> subscriberConnection_;
    bool subscriberConnected_ = false;
    std::uint16_t nextPacketId_ = 1;
    std::int64_t lastSubscriberActivityMs_ = 0;
    std::vector<OfflineMessage> offlineMessages_;
    std::int64_t lastOfflineFlushMs_ = 0;
    bool replayingOffline_ = false;
    std::unique_ptr<MqttRealtimeRingBuffer> realtimeRing_;
    std::unique_ptr<MqttEventOutbox> eventOutbox_;
    std::mutex mutex_;
};

}  // namespace edge_gateway
