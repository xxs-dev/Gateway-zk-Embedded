#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "edge_gateway/models.hpp"
#include "edge_gateway/cluster_write_authorization.hpp"
#include <stdexcept>

namespace edge_gateway {

struct MqttJsonMessage {
    std::string topic;
    std::string payload;
};

class IModbusClient {
public:
    virtual ~IModbusClient() = default;

    virtual void beginPriorityWrite() {
    }

    virtual void endPriorityWrite() {
    }

    virtual std::vector<std::uint16_t> readCoils(int slave, int start, int count) = 0;
    virtual std::vector<std::uint16_t> readDiscreteInputs(int slave, int start, int count) = 0;
    virtual std::vector<std::uint16_t> readHoldingRegisters(int slave, int start, int count) = 0;
    virtual std::vector<std::uint16_t> readInputRegisters(int slave, int start, int count) = 0;

    virtual void writeSingleCoil(int slave, int address, bool value) = 0;
    virtual void writeSingleCoil(int slave, int address, bool value, const BeforePhysicalWrite& beforeWrite) {
        if (beforeWrite) throw std::runtime_error("guarded coil write is unsupported");
        writeSingleCoil(slave, address, value);
    }
    virtual void writeSingleRegister(int slave, int address, std::uint16_t value, const BeforePhysicalWrite& beforeWrite) {
        if (beforeWrite) throw std::runtime_error("guarded register write is unsupported");
        writeSingleRegister(slave, address, value);
    }
    virtual void writeMultipleRegisters(int slave, int address, const std::vector<std::uint16_t>& values,
                                        const BeforePhysicalWrite& beforeWrite) {
        if (beforeWrite) throw std::runtime_error("guarded register write is unsupported");
        writeMultipleRegisters(slave, address, values);
    }
    virtual void writeSingleRegister(int slave, int address, std::uint16_t value) = 0;
    virtual void writeMultipleRegisters(
        int slave,
        int address,
        const std::vector<std::uint16_t>& values
    ) = 0;
};

class IGpioPort {
public:
    virtual ~IGpioPort() = default;

    virtual void exportGpio(int gpio) = 0;
    virtual void setDirection(int gpio, const std::string& direction) = 0;
    virtual bool readValue(int gpio) = 0;
    virtual void writeValue(int gpio, bool high) = 0;
};

class IMqttPublisher {
public:
    virtual ~IMqttPublisher() = default;

    virtual void publishTelemetry(
        const std::string& machineCode,
        const std::vector<PointValue>& values
    ) = 0;

    virtual void publishCommandResult(const CommandResult& result) = 0;

    virtual void publishStatusMessage(
        const std::string& machineCode,
        const std::string& payload
    ) = 0;
};

class IMqttDriverPublisher {
public:
    virtual ~IMqttDriverPublisher() = default;

    virtual void publishFullSnapshot(
        const std::string& topic,
        const std::vector<StoredPointValue>& values,
        const std::string& valueFormat
    ) = 0;

    virtual void publishAlarm(
        const std::string& topic,
        std::uint32_t index,
        const StoredPointValue& value,
        const std::string& alarmType,
        bool active
    ) = 0;

    virtual void publishOnDemand(
        const std::string& topic,
        const std::vector<StoredPointValue>& values,
        const std::string& valueFormat
    ) = 0;

    virtual void publishRealtime(
        const std::string& topic,
        const std::vector<StoredPointValue>& values,
        const std::string& valueFormat,
        const std::string& sessionId
    ) {
        (void)sessionId;
        publishOnDemand(topic, values, valueFormat);
    }

    virtual void publishChangeEvent(
        const std::string& topic,
        const StoredPointValue& value
    ) = 0;

    virtual void publishCommandReply(
        const std::string& topic,
        const MqttCommandReply& reply
    ) = 0;

    virtual void publishOtaReply(
        const std::string& topic,
        const OtaReply& reply
    ) = 0;

    virtual void publishOtaStatus(
        const std::string& topic,
        const OtaStatus& status
    ) = 0;

    virtual void publishJsonMessage(
        const std::string& topic,
        const std::string& payload
    ) = 0;
    virtual void publishReliableJsonMessage(
        const std::string& topic,
        const std::string& payload
    ) {
        publishJsonMessage(topic, payload);
    }
    virtual void publishReliableJsonMessages(const std::vector<MqttJsonMessage>& messages) {
        for (const auto& message : messages) {
            publishReliableJsonMessage(message.topic, message.payload);
        }
    }

    virtual void maintain() {
    }

    // Verifies that the publish connection can complete a broker round trip.
    // Implementations should throw when the broker cannot acknowledge it.
    virtual void probeConnection() {
    }

    virtual std::vector<MqttIncomingMessage> pollIncoming(int timeoutMs) = 0;
};

}  // namespace edge_gateway
