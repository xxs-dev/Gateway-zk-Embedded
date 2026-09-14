#pragma once

#include <chrono>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <functional>

#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/models.hpp"
#include "edge_gateway/mqtt_event_outbox.hpp"
#include "edge_gateway/mqtt_realtime_ring_buffer.hpp"

namespace edge_gateway {

enum class MqttPublisherMode {
    Bidirectional,
    TxOnly
};

enum class MqttEventOutboxOwnership {
    PublisherManaged,
    ManagementOnly,
    External
};

class BuiltinMqttDriverPublisher : public IMqttDriverPublisher {
public:
    explicit BuiltinMqttDriverPublisher(
        MqttConfig config,
        MqttPublisherMode mode = MqttPublisherMode::Bidirectional,
        MqttEventOutboxOwnership eventOutboxOwnership =
            MqttEventOutboxOwnership::PublisherManaged,
        std::function<void(const std::string&, const std::string&, const std::string&, std::int64_t)>
            durableEventSubmit = {}
    );
    ~BuiltinMqttDriverPublisher() override;

    BuiltinMqttDriverPublisher(const BuiltinMqttDriverPublisher&) = delete;
    BuiltinMqttDriverPublisher& operator=(const BuiltinMqttDriverPublisher&) = delete;

    void publishFullSnapshot(
        const std::string& topic,
        const std::vector<StoredPointValue>& values,
        const std::string& valueFormat
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
        const std::vector<StoredPointValue>& values,
        const std::string& valueFormat
    ) override;

    void publishRealtime(
        const std::string& topic,
        const std::vector<StoredPointValue>& values,
        const std::string& valueFormat,
        const std::string& sessionId
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
    void publishReliableJsonMessage(
        const std::string& topic,
        const std::string& payload
    ) override;
    // IPC event sender only: lock acquisition and MQTT QoS1 share this BOOTTIME budget.
    void publishLeasedEvent(const std::string& topic, const std::string& payload,
        const std::string& bootId, std::int64_t deadlineMs);
    // Exact topics; <=16 messages / 32768 topic+payload bytes. MQTT3 window 8,
    // MQTT5 window 1 until CONNACK Receive Maximum is supported. No retries.
    // Requires cleanSession and, for MQTT5, zero sessionExpirySec; no resume protocol.
    // Observer confirmations survive transport exceptions. Callbacks are synchronous.
    void publishLeasedEvents(const std::vector<MqttJsonMessage>& messages,
        const std::string& bootId, std::int64_t deadlineMs,
        const std::function<bool()>& canSend, const std::function<void(std::size_t)>& onAttempt,
        const std::function<void(std::size_t)>& onConfirmed);
    void publishReliableJsonMessages(const std::vector<MqttJsonMessage>& messages) override;

    void maintain() override;
    void probeConnection() override;

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
    void sendJsonNow(const std::string& topic, const std::string& payload, int qos);
    void sendJsonOnTxConnection(const std::string& topic, const std::string& payload, int qos);
    void sendQos1BatchOnTxConnection(const std::vector<MqttJsonMessage>& messages);
    int qosForTopic(const std::string& scopedTopic) const;
    void enqueueOffline(const std::string& topic, const std::string& payload);
    void flushOfflineBuffer(bool force);
    void replayOfflineBuffer();
    void ensureTxConnected(const std::chrono::steady_clock::time_point& deadline);
    void maintainTxConnection();
    void maintainTxConnection(const std::chrono::steady_clock::time_point& deadline);
    void closeTx(bool graceful = true);
    void ensureSubscriberConnected();
    void closeSubscriber(bool graceful = true);
    std::uint16_t nextPacketIdentifier();
    std::string scopedPublishTopic(const std::string& topic) const;

    struct OfflineMessage {
        std::string topic;
        std::string payload;
    };

    MqttConfig config_;
    MqttPublisherMode mode_ = MqttPublisherMode::Bidirectional;
    MqttEventOutboxOwnership eventOutboxOwnership_ =
        MqttEventOutboxOwnership::PublisherManaged;
    std::unique_ptr<MqttConnectionHandle> txConnection_;
    bool txConnected_ = false;
    std::int64_t lastTxActivityMs_ = 0;
    std::unique_ptr<MqttConnectionHandle> subscriberConnection_;
    bool subscriberConnected_ = false;
    std::uint16_t nextPacketId_ = 1;
    std::uint64_t packetIdGeneration_ = 0, txPacketIdGeneration_ = 0;
    std::int64_t lastSubscriberActivityMs_ = 0;
    std::vector<OfflineMessage> offlineMessages_;
    std::int64_t lastOfflineFlushMs_ = 0;
    bool replayingOffline_ = false;
    std::unique_ptr<MqttRealtimeRingBuffer> realtimeRing_;
    std::unique_ptr<MqttEventOutbox> eventOutbox_;
    std::function<void(const std::string&, const std::string&, const std::string&, std::int64_t)>
        durableEventSubmit_;
    std::mutex mutex_;
};

}  // namespace edge_gateway
