#include "edge_gateway/builtin_mqtt_driver_publisher.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

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

#ifndef _WIN32
std::string hexEncodeForTest(const std::string& value) {
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 2);
    for (const unsigned char ch : value) {
        out.push_back(digits[(ch >> 4) & 0x0F]);
        out.push_back(digits[ch & 0x0F]);
    }
    return out;
}

std::vector<std::uint8_t> readMqttPacket(int fd) {
    std::vector<std::uint8_t> packet;
    std::uint8_t byte = 0;
    if (recv(fd, &byte, 1, MSG_WAITALL) != 1) {
        throw std::runtime_error("test broker failed to read packet type");
    }
    packet.push_back(byte);
    std::size_t multiplier = 1;
    std::size_t remaining = 0;
    do {
        if (recv(fd, &byte, 1, MSG_WAITALL) != 1) {
            throw std::runtime_error("test broker failed to read remaining length");
        }
        packet.push_back(byte);
        remaining += (byte & 0x7F) * multiplier;
        multiplier *= 128;
    } while ((byte & 0x80) != 0);
    const auto headerSize = packet.size();
    packet.resize(headerSize + remaining);
    if (remaining > 0 &&
        recv(fd, packet.data() + headerSize, remaining, MSG_WAITALL) != static_cast<ssize_t>(remaining)) {
        throw std::runtime_error("test broker failed to read packet body");
    }
    return packet;
}

std::string packetTopic(const std::vector<std::uint8_t>& packet) {
    std::size_t cursor = 1;
    while (cursor < packet.size() && (packet[cursor++] & 0x80) != 0) {
    }
    if (cursor + 2 > packet.size()) {
        return {};
    }
    const auto length = (static_cast<std::size_t>(packet[cursor]) << 8) | packet[cursor + 1];
    cursor += 2;
    if (cursor + length > packet.size()) {
        return {};
    }
    return std::string(
        reinterpret_cast<const char*>(packet.data() + cursor),
        reinterpret_cast<const char*>(packet.data() + cursor + length)
    );
}

std::uint16_t packetId(const std::vector<std::uint8_t>& packet) {
    std::size_t cursor = 1;
    while (cursor < packet.size() && (packet[cursor++] & 0x80) != 0) {
    }
    if (cursor + 2 > packet.size()) {
        return 0;
    }
    const auto length = (static_cast<std::size_t>(packet[cursor]) << 8) | packet[cursor + 1];
    cursor += 2 + length;
    if (cursor + 2 > packet.size()) {
        return 0;
    }
    return static_cast<std::uint16_t>((packet[cursor] << 8) | packet[cursor + 1]);
}

std::uint16_t ackPacketId(const std::vector<std::uint8_t>& packet) {
    if (packet.size() < 4) {
        return 0;
    }
    return static_cast<std::uint16_t>((packet[2] << 8) | packet[3]);
}

void sendPubAck(int fd, std::uint16_t id) {
    const std::uint8_t pubAck[] = {
        0x40,
        0x02,
        static_cast<std::uint8_t>((id >> 8) & 0xFF),
        static_cast<std::uint8_t>(id & 0xFF)
    };
    send(fd, pubAck, sizeof(pubAck), 0);
}

void sendPubRec(int fd, std::uint16_t id) {
    const std::uint8_t pubRec[] = {
        0x50,
        0x02,
        static_cast<std::uint8_t>((id >> 8) & 0xFF),
        static_cast<std::uint8_t>(id & 0xFF)
    };
    send(fd, pubRec, sizeof(pubRec), 0);
}

void sendPubComp(int fd, std::uint16_t id) {
    const std::uint8_t pubComp[] = {
        0x70,
        0x02,
        static_cast<std::uint8_t>((id >> 8) & 0xFF),
        static_cast<std::uint8_t>(id & 0xFF)
    };
    send(fd, pubComp, sizeof(pubComp), 0);
}

class TestMqttBroker {
public:
    struct PublishedMessage {
        std::string topic;
        int qos = 0;
    };

    explicit TestMqttBroker(int expectedPublishes)
        : expectedPublishes_(expectedPublishes) {
        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) {
            throw std::runtime_error("test broker socket failed");
        }
        int opt = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            throw std::runtime_error("test broker bind failed");
        }
        socklen_t len = sizeof(addr);
        if (getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            throw std::runtime_error("test broker getsockname failed");
        }
        port_ = ntohs(addr.sin_port);
        if (listen(listenFd_, 16) != 0) {
            throw std::runtime_error("test broker listen failed");
        }
        thread_ = std::thread([this]() { run(); });
    }

    ~TestMqttBroker() {
        stop_.store(true);
        if (listenFd_ >= 0) {
            shutdown(listenFd_, SHUT_RDWR);
            close(listenFd_);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    int port() const {
        return port_;
    }

    std::vector<std::string> topics() const {
        std::vector<std::string> result;
        result.reserve(messages_.size());
        for (const auto& message : messages_) {
            result.push_back(message.topic);
        }
        return result;
    }

    std::vector<PublishedMessage> messages() const {
        return messages_;
    }

private:
    void run() {
        while (!stop_.load() && static_cast<int>(messages_.size()) < expectedPublishes_) {
            const int fd = accept(listenFd_, nullptr, nullptr);
            if (fd < 0) {
                continue;
            }
            try {
                const auto connect = readMqttPacket(fd);
                require(!connect.empty() && (connect[0] & 0xF0) == 0x10, "test broker expected connect");
                const std::uint8_t connAck[] = {0x20, 0x02, 0x00, 0x00};
                send(fd, connAck, sizeof(connAck), 0);
                while (!stop_.load() && static_cast<int>(messages_.size()) < expectedPublishes_) {
                    const auto publish = readMqttPacket(fd);
                    if (publish.empty() || (publish[0] & 0xF0) == 0xE0) {
                        break;
                    }
                    require((publish[0] & 0xF0) == 0x30, "test broker expected publish");
                    const auto qos = static_cast<int>((publish[0] >> 1) & 0x03);
                    messages_.push_back(PublishedMessage{packetTopic(publish), qos});
                    const auto id = packetId(publish);
                    if (qos == 1) {
                        sendPubAck(fd, id);
                    } else if (qos == 2) {
                        sendPubRec(fd, id);
                        const auto pubRel = readMqttPacket(fd);
                        require(!pubRel.empty() && pubRel[0] == 0x62, "test broker expected pubrel");
                        require(ackPacketId(pubRel) == id, "test broker pubrel id mismatch");
                        sendPubComp(fd, id);
                    }
                }
            } catch (...) {
            }
            close(fd);
        }
    }

    int expectedPublishes_;
    int listenFd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_ {false};
    std::thread thread_;
    std::vector<PublishedMessage> messages_;
};

void testOfflineReplayRemovesSentRecords() {
    const auto dir = std::string("/tmp/gateway_mqtt_offline_test_") + std::to_string(getpid());
    const auto mkdirCommand = std::string("mkdir -p ") + dir;
    require(std::system(mkdirCommand.c_str()) == 0, "test mkdir failed");
    const auto queuePath = dir + "/mqtt_offline_queue.log";
    {
        std::ofstream queue(queuePath.c_str(), std::ios::trunc);
        queue << hexEncodeForTest("edge/status/GW_TEST") << "\t" << hexEncodeForTest("{\"n\":1}") << "\n";
        queue << hexEncodeForTest("edge/status/GW_TEST") << "\t" << hexEncodeForTest("{\"n\":2}") << "\n";
        queue << hexEncodeForTest("edge/status/GW_TEST") << "\t" << hexEncodeForTest("{\"n\":3}") << "\n";
    }

    TestMqttBroker broker(3);
    edge_gateway::MqttConfig config;
    config.broker = std::string("tcp://127.0.0.1:") + std::to_string(broker.port());
    config.clientId = "GW_TEST";
    config.topicMachineCode = "GW_TEST";
    config.qos = 1;
    config.offlineBufferEnabled = true;
    config.offlineBufferDir = dir;
    config.offlineBufferReplayBatchSize = 2;
    config.offlineBufferFlushBatchSize = 1;
    config.offlineBufferFlushIntervalMs = 0;
    config.offlineRealtimeFile = dir + "/realtime_ring.dat";
    config.eventOutboxSqlitePath = dir + "/event_outbox.db";

    {
        edge_gateway::BuiltinMqttDriverPublisher publisher(config);
        publisher.publishJsonMessage("edge/status", "{\"current\":true}");
    }

    std::ifstream remaining(queuePath.c_str());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(remaining, line)) {
        lines.push_back(line);
    }
    require(lines.size() == 1, "offline replay should keep only unsent records");
    require(lines[0].find(hexEncodeForTest("{\"n\":3}")) != std::string::npos, "offline replay kept wrong record");
    std::remove(queuePath.c_str());
    std::remove((dir + "/realtime_ring.dat").c_str());
    std::remove((dir + "/event_outbox.db").c_str());
    std::remove((queuePath + ".lock").c_str());
    rmdir(dir.c_str());
}

void testControlTopicsUseQos2() {
    TestMqttBroker broker(2);
    edge_gateway::MqttConfig config;
    config.broker = std::string("tcp://127.0.0.1:") + std::to_string(broker.port());
    config.clientId = "GW_TEST";
    config.topicMachineCode = "GW_TEST";
    config.qos = 1;
    config.controlQos = 2;
    config.statusTopic = "edge/status";
    config.commandReplyTopic = "edge/command/reply";
    config.offlineBufferEnabled = false;

    {
        edge_gateway::BuiltinMqttDriverPublisher publisher(config);
        edge_gateway::MqttCommandReply reply;
        reply.cmdId = "CMD1";
        reply.machineCode = "GW_TEST";
        reply.success = true;
        reply.message = "accepted";
        publisher.publishCommandReply(config.commandReplyTopic, reply);
        publisher.publishJsonMessage(config.statusTopic, "{\"ok\":true}");
    }

    const auto messages = broker.messages();
    require(messages.size() == 2, "test broker should capture two publishes");
    require(messages[0].topic == "edge/command/reply/GW_TEST", "command reply topic mismatch");
    require(messages[0].qos == 2, "command reply should use qos2");
    require(messages[1].topic == "edge/status/GW_TEST", "status topic mismatch");
    require(messages[1].qos == 1, "status should keep normal qos");
}
#endif

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

#ifndef _WIN32
    testOfflineReplayRemovesSentRecords();
    testControlTopicsUseQos2();
#endif

    std::cout << "builtin_mqtt_driver_publisher_test passed" << std::endl;
    return 0;
}
