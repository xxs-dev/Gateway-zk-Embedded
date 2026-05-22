#include "edge_gateway/builtin_mqtt_driver_publisher.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireRejected(const std::vector<std::uint8_t>& packet, const edge_gateway::MqttConfig& config, const std::string& expected) {
    edge_gateway::MqttIncomingMessage message;
    try {
        edge_gateway::BuiltinMqttDriverPublisher::parseIncomingPublishPacket(config, packet, &message);
    } catch (const std::exception& ex) {
        if (std::string(ex.what()).find(expected) != std::string::npos) {
            return;
        }
        throw;
    }
    throw std::runtime_error("mqtt publish packet should be rejected");
}

void appendRemainingLength(std::vector<std::uint8_t>& bytes, std::size_t length) {
    do {
        std::uint8_t encoded = static_cast<std::uint8_t>(length % 128);
        length /= 128;
        if (length > 0) {
            encoded |= 0x80;
        }
        bytes.push_back(encoded);
    } while (length > 0);
}

std::vector<std::uint8_t> publishPacket(const std::string& topic, const std::string& payload) {
    std::vector<std::uint8_t> packet;
    packet.push_back(0x30);
    appendRemainingLength(packet, 2 + topic.size() + payload.size());
    packet.push_back(static_cast<std::uint8_t>((topic.size() >> 8) & 0xFF));
    packet.push_back(static_cast<std::uint8_t>(topic.size() & 0xFF));
    packet.insert(packet.end(), topic.begin(), topic.end());
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

}  // namespace

int main() {
    using namespace edge_gateway;

    MqttConfig config;
    config.topicMachineCode = "GW_TEST";
    config.commandRequestTopic = "edge/command/request";
    config.maxPayloadBytes = 4 * 1024;

    MqttIncomingMessage message;
    const auto ok = publishPacket("edge/command/request/GW_TEST", "{\"cmdId\":\"CMD1\"}");
    require(BuiltinMqttDriverPublisher::parseIncomingPublishPacket(config, ok, &message), "command publish should parse");
    require(message.type == MqttIncomingType::CommandRequest, "command publish type mismatch");
    require(message.topic == "edge/command/request/GW_TEST", "command publish topic mismatch");
    require(message.payload == "{\"cmdId\":\"CMD1\"}", "command publish payload mismatch");

    const auto oversized = publishPacket("edge/command/request/GW_TEST", std::string(512 * 1024 + 1, 'x'));
    requireRejected(oversized, config, "mqtt incoming packet is too large");

    const std::vector<std::uint8_t> malformedRemainingLength = {0x30, 0x80, 0x80, 0x80, 0x80, 0x00};
    requireRejected(malformedRemainingLength, config, "mqtt malformed remaining length");

    std::cout << "builtin_mqtt_driver_publisher_test passed" << std::endl;
    return 0;
}
