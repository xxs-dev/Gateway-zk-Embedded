#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {

class MosquittoMqttDriverPublisher : public IMqttDriverPublisher {
public:
    explicit MosquittoMqttDriverPublisher(MqttConfig config);
    ~MosquittoMqttDriverPublisher() override;

    MosquittoMqttDriverPublisher(const MosquittoMqttDriverPublisher&) = delete;
    MosquittoMqttDriverPublisher& operator=(const MosquittoMqttDriverPublisher&) = delete;

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

private:
    struct Impl;

    void publishJson(const std::string& topic, const std::string& payload);

    MqttConfig config_;
    std::unique_ptr<Impl> impl_;
    std::mutex mutex_;
};

}  // namespace edge_gateway
