#include "edge_gateway/builtin_mqtt_driver_publisher.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <limits>
#include <mutex>
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

std::string packetPayload(const std::vector<std::uint8_t>& packet) {
    std::size_t cursor = 1;
    while (cursor < packet.size() && (packet[cursor++] & 0x80) != 0) {
    }
    if (cursor + 2 > packet.size()) {
        return {};
    }
    const auto topicLength = (static_cast<std::size_t>(packet[cursor]) << 8) | packet[cursor + 1];
    cursor += 2 + topicLength;
    if (((packet[0] >> 1) & 0x03) > 0) {
        cursor += 2;
    }
    if (cursor > packet.size()) {
        return {};
    }
    return std::string(packet.begin() + static_cast<std::ptrdiff_t>(cursor), packet.end());
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
        std::string payload;
        int qos = 0;
    };

    explicit TestMqttBroker(int expectedPublishes, int requestedPort = 0)
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
        addr.sin_port = htons(static_cast<std::uint16_t>(requestedPort));
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
        const auto captured = messages();
        std::vector<std::string> result;
        result.reserve(captured.size());
        for (const auto& message : captured) {
            result.push_back(message.topic);
        }
        return result;
    }

    std::vector<PublishedMessage> messages() const {
        std::lock_guard<std::mutex> lock(messagesMutex_);
        return messages_;
    }

    int publishCount() const {
        return publishCount_.load();
    }

private:
    void run() {
        while (!stop_.load() && publishCount_.load() < expectedPublishes_) {
            const int fd = accept(listenFd_, nullptr, nullptr);
            if (fd < 0) {
                continue;
            }
            try {
                const auto connect = readMqttPacket(fd);
                require(!connect.empty() && (connect[0] & 0xF0) == 0x10, "test broker expected connect");
                const std::uint8_t connAck[] = {0x20, 0x02, 0x00, 0x00};
                send(fd, connAck, sizeof(connAck), 0);
                while (!stop_.load() && publishCount_.load() < expectedPublishes_) {
                    const auto publish = readMqttPacket(fd);
                    if (publish.empty() || (publish[0] & 0xF0) == 0xE0) {
                        break;
                    }
                    require((publish[0] & 0xF0) == 0x30, "test broker expected publish");
                    const auto qos = static_cast<int>((publish[0] >> 1) & 0x03);
                    {
                        std::lock_guard<std::mutex> lock(messagesMutex_);
                        messages_.push_back(PublishedMessage{packetTopic(publish), packetPayload(publish), qos});
                    }
                    publishCount_.fetch_add(1);
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
    std::atomic<int> publishCount_ {0};
    std::thread thread_;
    mutable std::mutex messagesMutex_;
    std::vector<PublishedMessage> messages_;
};

int unusedTcpPort() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    require(fd >= 0, "unused port socket failed");
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    require(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "unused port bind failed");
    socklen_t len = sizeof(addr);
    require(getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0, "unused port lookup failed");
    const int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

class ReconnectMqttBroker {
public:
    ReconnectMqttBroker() {
        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        require(listenFd_ >= 0, "reconnect broker socket failed");
        int opt = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        require(bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0,
                "reconnect broker bind failed");
        socklen_t len = sizeof(addr);
        require(getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0,
                "reconnect broker getsockname failed");
        port_ = ntohs(addr.sin_port);
        require(listen(listenFd_, 4) == 0, "reconnect broker listen failed");
        thread_ = std::thread([this]() { run(); });
    }

    ~ReconnectMqttBroker() {
        stop_.store(true);
        const int rx = rxFd_.exchange(-1);
        if (rx >= 0) {
            shutdown(rx, SHUT_RDWR);
            close(rx);
        }
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

    int publishCount() const {
        return publishCount_.load();
    }

    int closedTxCount() const {
        return closedTxCount_.load();
    }

private:
    static std::size_t bodyOffset(const std::vector<std::uint8_t>& packet) {
        std::size_t cursor = 1;
        while (cursor < packet.size() && (packet[cursor++] & 0x80) != 0) {
        }
        return cursor;
    }

    static bool isRxConnect(const std::vector<std::uint8_t>& packet) {
        const std::string bytes(packet.begin(), packet.end());
        return bytes.find("-rx") != std::string::npos;
    }

    static void sendConnAck(int fd) {
        const std::uint8_t connAck[] = {0x20, 0x02, 0x00, 0x00};
        send(fd, connAck, sizeof(connAck), 0);
    }

    static void sendSubAck(int fd, const std::vector<std::uint8_t>& subscribe) {
        const auto body = bodyOffset(subscribe);
        require(body + 2 <= subscribe.size(), "reconnect broker subscribe id missing");
        const std::uint8_t subAck[] = {
            0x90, 0x03, subscribe[body], subscribe[body + 1], 0x01
        };
        send(fd, subAck, sizeof(subAck), 0);
    }

    void run() {
        while (!stop_.load() && publishCount_.load() < 2) {
            const int fd = accept(listenFd_, nullptr, nullptr);
            if (fd < 0) {
                continue;
            }
            try {
                const auto connect = readMqttPacket(fd);
                require(!connect.empty() && (connect[0] & 0xF0) == 0x10,
                        "reconnect broker expected connect");
                sendConnAck(fd);
                if (isRxConnect(connect)) {
                    const auto subscribe = readMqttPacket(fd);
                    require(!subscribe.empty() && (subscribe[0] & 0xF0) == 0x80,
                            "reconnect broker expected subscribe");
                    sendSubAck(fd, subscribe);
                    rxFd_.store(fd);
                    continue;
                }

                const auto publish = readMqttPacket(fd);
                require(!publish.empty() && (publish[0] & 0xF0) == 0x30,
                        "reconnect broker expected publish");
                const auto id = packetId(publish);
                if (((publish[0] >> 1) & 0x03) == 1) {
                    sendPubAck(fd, id);
                }
                publishCount_.fetch_add(1);
            } catch (...) {
            }
            shutdown(fd, SHUT_RDWR);
            close(fd);
            closedTxCount_.fetch_add(1);
        }
    }

    int listenFd_ = -1;
    int port_ = 0;
    std::atomic<int> rxFd_ {-1};
    std::atomic<int> publishCount_ {0};
    std::atomic<int> closedTxCount_ {0};
    std::atomic<bool> stop_ {false};
    std::thread thread_;
};

void testClosedTxConnectionReconnectsBeforeNextPublish() {
    ReconnectMqttBroker broker;
    edge_gateway::MqttConfig config;
    config.broker = std::string("tcp://127.0.0.1:") + std::to_string(broker.port());
    config.clientId = "GW_RECONNECT";
    config.topicMachineCode = "GW_RECONNECT";
    config.commandRequestTopic = "edge/command/request";
    config.statusTopic = "edge/status";
    config.qos = 1;
    config.offlineBufferEnabled = false;

    edge_gateway::BuiltinMqttDriverPublisher publisher(config);
    publisher.pollIncoming(0);
    publisher.publishJsonMessage(config.statusTopic, "{\"n\":1}");

    for (int attempt = 0; attempt < 100 && broker.closedTxCount() < 1; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(broker.publishCount() == 1, "reconnect broker did not receive first publish");
    require(broker.closedTxCount() == 1, "reconnect broker did not close first TX connection");

    // Publish-only services call maintain without subscribing to command topics.
    publisher.maintain();
    publisher.publishJsonMessage(config.statusTopic, "{\"n\":2}");

    for (int attempt = 0; attempt < 100 && broker.publishCount() < 2; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(broker.publishCount() == 2, "publisher did not reconnect before second publish");
}

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

void testOfflinePublishSurvivesRestartAndReplaysInOrder() {
    const auto dir = std::string("/tmp/gateway_mqtt_restart_replay_test_") + std::to_string(getpid());
    const auto mkdirCommand = std::string("mkdir -p ") + dir;
    require(std::system(mkdirCommand.c_str()) == 0, "restart replay mkdir failed");
    const auto queuePath = dir + "/mqtt_offline_queue.log";
    const auto port = unusedTcpPort();

    edge_gateway::MqttConfig config;
    config.broker = std::string("tcp://127.0.0.1:") + std::to_string(port);
    config.clientId = "GW_RESTART_REPLAY";
    config.topicMachineCode = "GW_RESTART_REPLAY";
    config.statusTopic = "edge/status";
    config.qos = 1;
    config.offlineBufferEnabled = true;
    config.offlineBufferDir = dir;
    config.offlineBufferMaxMemoryMessages = 1;
    config.offlineBufferFlushBatchSize = 1;
    config.offlineBufferFlushIntervalMs = 0;
    config.offlineBufferReplayBatchSize = 10;
    config.offlineRealtimeFile = dir + "/realtime_ring.dat";
    config.eventOutboxSqlitePath = dir + "/event_outbox.db";

    {
        edge_gateway::BuiltinMqttDriverPublisher publisher(config);
        publisher.publishJsonMessage(config.statusTopic, "{\"sequence\":1}");
        publisher.publishJsonMessage(config.statusTopic, "{\"sequence\":2}");
    }

    std::ifstream queued(queuePath.c_str());
    std::vector<std::string> queuedLines;
    std::string line;
    while (std::getline(queued, line)) {
        queuedLines.push_back(line);
    }
    require(queuedLines.size() == 2, "offline publishes were not persisted before restart");

    {
        TestMqttBroker broker(3, port);
        edge_gateway::BuiltinMqttDriverPublisher restarted(config);
        restarted.publishJsonMessage(config.statusTopic, "{\"sequence\":3}");
        for (int attempt = 0; attempt < 100 && broker.publishCount() < 3; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const auto messages = broker.messages();
        require(messages.size() == 3, "restarted publisher did not replay all persisted messages");
        require(messages[0].payload == "{\"sequence\":1}", "offline replay changed first message order");
        require(messages[1].payload == "{\"sequence\":2}", "offline replay changed second message order");
        require(messages[2].payload == "{\"sequence\":3}", "current message must follow offline replay");
    }

    std::ifstream remaining(queuePath.c_str());
    require(!remaining.is_open() || remaining.peek() == std::ifstream::traits_type::eof(),
        "offline queue must be empty after successful replay");
    std::remove(queuePath.c_str());
    std::remove((queuePath + ".lock").c_str());
    std::remove((queuePath + ".tmp").c_str());
    std::remove((dir + "/realtime_ring.dat").c_str());
    std::remove((dir + "/event_outbox.db").c_str());
    std::remove((dir + "/event_outbox.db-shm").c_str());
    std::remove((dir + "/event_outbox.db-wal").c_str());
    rmdir(dir.c_str());
}

void testRealtimeRingSurvivesRestartAndReplaysBeforeCurrentSnapshot() {
    const auto dir = std::string("/tmp/gateway_mqtt_realtime_replay_test_") + std::to_string(getpid());
    const auto mkdirCommand = std::string("mkdir -p ") + dir;
    require(std::system(mkdirCommand.c_str()) == 0, "realtime replay mkdir failed");
    const auto port = unusedTcpPort();

    edge_gateway::MqttConfig config;
    config.broker = std::string("tcp://127.0.0.1:") + std::to_string(port);
    config.clientId = "GW_REALTIME_REPLAY";
    config.topicMachineCode = "GW_REALTIME_REPLAY";
    config.fullTelemetryTopic = "edge/telemetry/full";
    config.qos = 1;
    config.offlineBufferEnabled = true;
    config.offlineBufferDir = dir;
    config.offlineBufferReplayBatchSize = 10;
    config.offlineRealtimeFile = dir + "/realtime_ring.dat";
    config.offlineRealtimeFileSizeBytes = 64 * 1024;
    config.offlineMaxRealtimeMessageBytes = 4096;
    config.eventOutboxSqlitePath = dir + "/event_outbox.db";

    edge_gateway::StoredPointValue point;
    point.index = 910001;
    point.machineCode = config.topicMachineCode;
    point.meterCode = "SIM_METER_01";
    point.pointCode = "POWER";
    point.value = 11.5;
    point.ts = 1000;
    {
        edge_gateway::BuiltinMqttDriverPublisher publisher(config);
        publisher.publishFullSnapshot(config.fullTelemetryTopic, {point}, "object");
    }

    std::ifstream ring(config.offlineRealtimeFile.c_str(), std::ios::binary | std::ios::ate);
    require(ring.is_open() && ring.tellg() > 0, "offline realtime snapshot was not persisted");

    {
        TestMqttBroker broker(2, port);
        point.value = 22.5;
        point.ts = 2000;
        edge_gateway::BuiltinMqttDriverPublisher restarted(config);
        restarted.publishFullSnapshot(config.fullTelemetryTopic, {point}, "object");
        for (int attempt = 0; attempt < 100 && broker.publishCount() < 2; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const auto messages = broker.messages();
        require(messages.size() == 2, "restarted publisher did not replay realtime snapshot");
        require(messages[0].payload.find("\"value\":11.5") != std::string::npos,
            "persisted realtime snapshot was not replayed first");
        require(messages[1].payload.find("\"value\":22.5") != std::string::npos,
            "current realtime snapshot did not follow replay");
    }

    std::remove(config.offlineRealtimeFile.c_str());
    std::remove((dir + "/mqtt_offline_queue.log").c_str());
    std::remove((dir + "/mqtt_offline_queue.log.lock").c_str());
    std::remove((dir + "/event_outbox.db").c_str());
    std::remove((dir + "/event_outbox.db-shm").c_str());
    std::remove((dir + "/event_outbox.db-wal").c_str());
    rmdir(dir.c_str());
}

void testEventOutboxSurvivesRestartAndClearsAfterReplay() {
    const auto dir = std::string("/tmp/gateway_mqtt_event_replay_test_") + std::to_string(getpid());
    const auto mkdirCommand = std::string("mkdir -p ") + dir;
    require(std::system(mkdirCommand.c_str()) == 0, "event replay mkdir failed");
    const auto port = unusedTcpPort();

    edge_gateway::MqttConfig config;
    config.broker = std::string("tcp://127.0.0.1:") + std::to_string(port);
    config.clientId = "GW_EVENT_REPLAY";
    config.topicMachineCode = "GW_EVENT_REPLAY";
    config.changeEventTopic = "edge/event/change";
    config.qos = 1;
    config.offlineBufferEnabled = true;
    config.offlineBufferDir = dir;
    config.offlineBufferReplayBatchSize = 10;
    config.offlineRealtimeFile = dir + "/realtime_ring.dat";
    config.offlineRealtimeFileSizeBytes = 64 * 1024;
    config.eventOutboxSqlitePath = dir + "/event_outbox.db";
    config.eventOutboxReplayBatchSize = 10;

    edge_gateway::StoredPointValue point;
    point.index = 910002;
    point.machineCode = config.topicMachineCode;
    point.meterCode = "SIM_METER_01";
    point.pointCode = "RUN_STATE";
    point.value = 1;
    point.ts = 1000;
    {
        edge_gateway::BuiltinMqttDriverPublisher publisher(config);
        publisher.publishChangeEvent(config.changeEventTopic, point);
    }
    {
        edge_gateway::MqttEventOutbox outbox(
            config.eventOutboxSqlitePath,
            config.eventOutboxSqliteLibraryPath,
            config.eventOutboxRetentionMonths,
            config.eventOutboxCleanupIntervalHours,
            config.eventOutboxReplayBatchSize,
            config.eventOutboxMaxDiskBytes);
        require(outbox.pendingCount() == 1, "offline event was not persisted in SQLite outbox");
    }

    {
        TestMqttBroker broker(2, port);
        point.value = 2;
        point.ts = 2000;
        edge_gateway::BuiltinMqttDriverPublisher restarted(config);
        restarted.publishChangeEvent(config.changeEventTopic, point);
        for (int attempt = 0; attempt < 100 && broker.publishCount() < 2; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const auto messages = broker.messages();
        require(messages.size() == 2, "restarted publisher did not replay event outbox");
        require(messages[0].payload.find("\"value\":1") != std::string::npos,
            "persisted event was not replayed first");
        require(messages[1].payload.find("\"value\":2") != std::string::npos,
            "current event did not follow replay");
    }
    {
        edge_gateway::MqttEventOutbox outbox(
            config.eventOutboxSqlitePath,
            config.eventOutboxSqliteLibraryPath,
            config.eventOutboxRetentionMonths,
            config.eventOutboxCleanupIntervalHours,
            config.eventOutboxReplayBatchSize,
            config.eventOutboxMaxDiskBytes);
        require(outbox.pendingCount() == 0, "event outbox must be empty after successful replay");
    }

    std::remove(config.offlineRealtimeFile.c_str());
    std::remove((dir + "/mqtt_offline_queue.log").c_str());
    std::remove((dir + "/mqtt_offline_queue.log.lock").c_str());
    std::remove(config.eventOutboxSqlitePath.c_str());
    std::remove((config.eventOutboxSqlitePath + "-shm").c_str());
    std::remove((config.eventOutboxSqlitePath + "-wal").c_str());
    rmdir(dir.c_str());
}

void testControlTopicsUseQos2() {
    TestMqttBroker broker(4);
    edge_gateway::MqttConfig config;
    config.broker = std::string("tcp://127.0.0.1:") + std::to_string(broker.port());
    config.clientId = "GW_TEST";
    config.topicMachineCode = "GW_TEST";
    config.qos = 1;
    config.controlQos = 2;
    config.statusTopic = "edge/status";
    config.commandReplyTopic = "edge/command/reply";
    config.recordingReplyTopic = "edge/recording/reply";
    config.recordingStatusTopic = "edge/recording/status";
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
        publisher.publishJsonMessage(config.recordingReplyTopic, "{\"stage\":\"accepted\"}");
        publisher.publishJsonMessage(config.recordingStatusTopic, "{\"stage\":\"uploading\"}");
    }

    const auto messages = broker.messages();
    require(messages.size() == 4, "test broker should capture four publishes");
    require(messages[0].topic == "edge/command/reply/GW_TEST", "command reply topic mismatch");
    require(messages[0].qos == 2, "command reply should use qos2");
    require(messages[1].topic == "edge/status/GW_TEST", "status topic mismatch");
    require(messages[1].qos == 1, "status should keep normal qos");
    require(messages[2].topic == "edge/recording/reply/GW_TEST", "recording reply topic mismatch");
    require(messages[2].qos == 2, "recording reply should use qos2");
    require(messages[3].topic == "edge/recording/status/GW_TEST", "recording status topic mismatch");
    require(messages[3].qos == 2, "recording status should use qos2");
}

void testFullTelemetryTopicScopingMode() {
    TestMqttBroker broker(2);
    edge_gateway::MqttConfig config;
    config.broker = std::string("tcp://127.0.0.1:") + std::to_string(broker.port());
    config.clientId = "GW_FULL_TOPIC";
    config.topicMachineCode = "GW_FULL_TOPIC";
    config.fullTelemetryTopic = "third/site/full";
    config.offlineBufferEnabled = false;

    edge_gateway::StoredPointValue point;
    point.index = 1001;
    point.value = 1.0;
    point.ts = 1000;
    {
        edge_gateway::BuiltinMqttDriverPublisher publisher(config);
        publisher.publishFullSnapshot(config.fullTelemetryTopic, {point}, "object");
    }
    config.clientId = "GW_FULL_TOPIC_EXACT";
    config.fullTelemetryTopicMachineScoped = false;
    {
        edge_gateway::BuiltinMqttDriverPublisher publisher(config);
        publisher.publishFullSnapshot(config.fullTelemetryTopic, {point}, "object");
    }

    const auto messages = broker.messages();
    require(messages.size() == 2, "test broker should capture both full telemetry publishes");
    require(
        messages[0].topic == "third/site/full/GW_FULL_TOPIC",
        "full telemetry topic must remain machine-scoped by default"
    );
    require(
        messages[1].topic == "third/site/full",
        "explicit exact full telemetry topic must not append machineCode"
    );
}

void testNonFinitePointValuesProduceValidJson() {
    TestMqttBroker broker(5);
    edge_gateway::MqttConfig config;
    config.broker = std::string("tcp://127.0.0.1:") + std::to_string(broker.port());
    config.clientId = "GW_FINITE_JSON";
    config.topicMachineCode = "GW_FINITE_JSON";
    config.fullTelemetryTopic = "edge/telemetry/full";
    config.changeEventTopic = "edge/event/change";
    config.alarmTopic = "edge/alarm";
    config.commandReplyTopic = "edge/command/reply";
    config.offlineBufferEnabled = false;

    edge_gateway::StoredPointValue point;
    point.index = 1001;
    point.machineCode = config.topicMachineCode;
    point.meterCode = "SIM_METER_01";
    point.pointCode = "POWER";
    point.value = std::numeric_limits<double>::quiet_NaN();
    point.ts = 1000;

    edge_gateway::MqttCommandReply reply;
    reply.cmdId = "CMD_NON_FINITE";
    reply.machineCode = config.topicMachineCode;
    reply.value = std::numeric_limits<double>::infinity();

    edge_gateway::BuiltinMqttDriverPublisher publisher(config);
    publisher.publishFullSnapshot(config.fullTelemetryTopic, {point}, "compactArray");
    publisher.publishFullSnapshot(config.fullTelemetryTopic, {point}, "object");
    publisher.publishChangeEvent(config.changeEventTopic, point);
    publisher.publishAlarm(config.alarmTopic, point.index, point, "high", true);
    publisher.publishCommandReply(config.commandReplyTopic, reply);

    const auto messages = broker.messages();
    require(messages.size() == 5, "test broker should capture all non-finite JSON cases");
    for (const auto& message : messages) {
        require(message.payload.find("nan") == std::string::npos, "JSON must not contain nan");
        require(message.payload.find("inf") == std::string::npos, "JSON must not contain inf");
    }
    require(messages[0].payload.find("[1001,\"POWER\",null,") != std::string::npos,
        "compact point value must encode non-finite numbers as null");
    for (std::size_t index = 1; index < messages.size(); ++index) {
        require(messages[index].payload.find("\"value\":null") != std::string::npos,
            "object payload value must encode non-finite numbers as null");
    }
}

void testUnreachableBrokerHonorsConnectTimeout() {
    edge_gateway::MqttConfig config;
    config.broker = "tcp://192.0.2.1:1883";
    config.clientId = "GW_CONNECT_TIMEOUT";
    config.statusTopic = "edge/status";
    config.connectTimeoutMs = 100;
    config.offlineBufferEnabled = false;

    const auto startedAt = std::chrono::steady_clock::now();
    {
        edge_gateway::BuiltinMqttDriverPublisher publisher(config);
        publisher.publishJsonMessage(config.statusTopic, "{\"ok\":true}");
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt
    ).count();
    require(elapsedMs < 1000, "MQTT connect must honor connectTimeoutMs");
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
    require(!message.retained, "normal command publish must not be retained");

    auto retainedPublish = ok;
    retainedPublish[0] = static_cast<std::uint8_t>(retainedPublish[0] | 0x01);
    require(
        BuiltinMqttDriverPublisher::parseIncomingPublishPacket(config, retainedPublish, &message),
        "retained command publish should parse"
    );
    require(message.type == MqttIncomingType::CommandRequest, "retained command publish type mismatch");
    require(message.topic == "edge/command/request/GW_TEST", "retained command publish topic mismatch");
    require(message.payload == "{\"cmdId\":\"CMD1\"}", "retained command publish payload mismatch");
    require(message.retained, "retained command publish flag mismatch");

    MqttConfig exactTopicConfig = config;
    exactTopicConfig.commandRequestTopic = "cnz_data/cmdRequest/whzn005";
    exactTopicConfig.commandRequestTopicMachineScoped = false;
    const auto exactTopicPacket = publishPacket(
        "cnz_data/cmdRequest/whzn005",
        "{\"type\":\"1\"}"
    );
    require(
        BuiltinMqttDriverPublisher::parseIncomingPublishPacket(
            exactTopicConfig,
            exactTopicPacket,
            &message
        ),
        "exact third-party command topic should parse without a machineCode suffix"
    );
    const auto incorrectlyScopedPacket = publishPacket(
        "cnz_data/cmdRequest/whzn005/GW_TEST",
        "{\"type\":\"1\"}"
    );
    require(
        !BuiltinMqttDriverPublisher::parseIncomingPublishPacket(
            exactTopicConfig,
            incorrectlyScopedPacket,
            &message
        ),
        "exact third-party command topic must not accept an appended machineCode"
    );

    config.recordingRequestTopic = "edge/recording/request";
    const auto recordingRequest = publishPacket(
        "edge/recording/request/GW_TEST",
        "{\"requestId\":\"REC1\",\"action\":\"list\"}"
    );
    require(BuiltinMqttDriverPublisher::parseIncomingPublishPacket(config, recordingRequest, &message),
        "recording request publish should parse");
    require(message.type == MqttIncomingType::RecordingRequest,
        "recording request publish type mismatch");

    config.recordingAckTopic = "edge/recording/ack";
    const auto recordingAck = publishPacket(
        "edge/recording/ack/GW_TEST",
        "{\"requestId\":\"REC1\",\"status\":\"completed\"}"
    );
    require(BuiltinMqttDriverPublisher::parseIncomingPublishPacket(config, recordingAck, &message),
        "recording ACK publish should parse");
    require(message.type == MqttIncomingType::RecordingAck,
        "recording ACK publish type mismatch");

    const auto oversized = publishPacket("edge/command/request/GW_TEST", std::string(512 * 1024 + 1, 'x'));
    requireRejected(oversized, config, "mqtt incoming packet is too large");

    const std::vector<std::uint8_t> malformedRemainingLength = {0x30, 0x80, 0x80, 0x80, 0x80, 0x00};
    requireRejected(malformedRemainingLength, config, "mqtt malformed remaining length");

#ifndef _WIN32
    testOfflineReplayRemovesSentRecords();
    testOfflinePublishSurvivesRestartAndReplaysInOrder();
    testRealtimeRingSurvivesRestartAndReplaysBeforeCurrentSnapshot();
    testEventOutboxSurvivesRestartAndClearsAfterReplay();
    testControlTopicsUseQos2();
    testFullTelemetryTopicScopingMode();
    testNonFinitePointValuesProduceValidJson();
    testClosedTxConnectionReconnectsBeforeNextPublish();
    testUnreachableBrokerHonorsConnectTimeout();
#endif

    std::cout << "builtin_mqtt_driver_publisher_test passed" << std::endl;
    return 0;
}
