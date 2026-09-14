#include "edge_gateway/builtin_mqtt_driver_publisher.hpp"
#include "edge_gateway/event_store_clock.hpp"

#include <algorithm>
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
#include <sys/select.h>
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

bool socketReadableWithin(int fd, int timeoutMs) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(fd, &readSet);
    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    return select(fd + 1, &readSet, nullptr, nullptr, &timeout) > 0;
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
        std::uint16_t packetId = 0;
    };

    explicit TestMqttBroker(
        int expectedPublishes,
        int requestedPort = 0,
        bool deferQos1Acks = false,
        bool reverseDeferredQos1Acks = false,
        std::size_t deferredQos1AckWindow = 0
    ) : expectedPublishes_(expectedPublishes),
        deferQos1Acks_(deferQos1Acks),
        reverseDeferredQos1Acks_(reverseDeferredQos1Acks),
        deferredQos1AckWindow_(deferredQos1AckWindow) {
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

    bool qos1WindowViolated() const {
        return qos1WindowViolated_.load();
    }

private:
    void run() {
        while (!stop_.load() && publishCount_.load() < expectedPublishes_) {
            const int fd = accept(listenFd_, nullptr, nullptr);
            if (fd < 0) {
                continue;
            }
            try {
                std::vector<std::uint16_t> deferredQos1PacketIds;
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
                    const auto id = packetId(publish);
                    {
                        std::lock_guard<std::mutex> lock(messagesMutex_);
                        messages_.push_back(PublishedMessage{
                            packetTopic(publish),
                            packetPayload(publish),
                            qos,
                            id
                        });
                    }
                    publishCount_.fetch_add(1);
                    if (qos == 1) {
                        if (deferQos1Acks_) {
                            deferredQos1PacketIds.push_back(id);
                            const bool windowReady = deferredQos1AckWindow_ > 0 &&
                                deferredQos1PacketIds.size() >= deferredQos1AckWindow_;
                            if (windowReady && publishCount_.load() < expectedPublishes_ &&
                                socketReadableWithin(fd, 20)) {
                                qos1WindowViolated_.store(true);
                            }
                            if (windowReady || publishCount_.load() >= expectedPublishes_) {
                                if (reverseDeferredQos1Acks_) {
                                    for (auto ack = deferredQos1PacketIds.rbegin();
                                         ack != deferredQos1PacketIds.rend(); ++ack) {
                                        sendPubAck(fd, *ack);
                                    }
                                } else {
                                    for (const auto deferredId : deferredQos1PacketIds) {
                                        sendPubAck(fd, deferredId);
                                    }
                                }
                                deferredQos1PacketIds.clear();
                            }
                        } else {
                            sendPubAck(fd, id);
                        }
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
    bool deferQos1Acks_ = false;
    bool reverseDeferredQos1Acks_ = false;
    std::size_t deferredQos1AckWindow_ = 0;
    int listenFd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_ {false};
    std::atomic<int> publishCount_ {0};
    std::atomic<bool> qos1WindowViolated_ {false};
    std::thread thread_;
    mutable std::mutex messagesMutex_;
    std::vector<PublishedMessage> messages_;
};

class RejectingAckMqttBroker {
public:
    enum class Mode {
        WrongPacketId,
        Mqtt5Rejected
    };

    explicit RejectingAckMqttBroker(Mode mode)
        : mode_(mode) {
        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        require(listenFd_ >= 0, "rejecting broker socket failed");
        int opt = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        require(bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0,
                "rejecting broker bind failed");
        socklen_t len = sizeof(addr);
        require(getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0,
                "rejecting broker getsockname failed");
        port_ = ntohs(addr.sin_port);
        require(listen(listenFd_, 1) == 0, "rejecting broker listen failed");
        thread_ = std::thread([this]() { run(); });
    }

    ~RejectingAckMqttBroker() {
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

private:
    void run() {
        const int fd = accept(listenFd_, nullptr, nullptr);
        if (fd < 0) {
            return;
        }
        try {
            const auto connect = readMqttPacket(fd);
            require(!connect.empty() && (connect[0] & 0xF0) == 0x10,
                    "rejecting broker expected connect");
            if (mode_ == Mode::Mqtt5Rejected) {
                const std::uint8_t connAck[] = {0x20, 0x03, 0x00, 0x00, 0x00};
                send(fd, connAck, sizeof(connAck), 0);
            } else {
                const std::uint8_t connAck[] = {0x20, 0x02, 0x00, 0x00};
                send(fd, connAck, sizeof(connAck), 0);
            }

            const auto publish = readMqttPacket(fd);
            require(!publish.empty() && (publish[0] & 0xF0) == 0x30,
                    "rejecting broker expected publish");
            const auto id = packetId(publish);
            if (mode_ == Mode::WrongPacketId) {
                sendPubAck(fd, static_cast<std::uint16_t>(id + 1));
            } else {
                const std::uint8_t rejected[] = {
                    0x40,
                    0x04,
                    static_cast<std::uint8_t>((id >> 8) & 0xFF),
                    static_cast<std::uint8_t>(id & 0xFF),
                    0x80,
                    0x00
                };
                send(fd, rejected, sizeof(rejected), 0);
            }
        } catch (...) {
        }
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }

    Mode mode_;
    int listenFd_ = -1;
    int port_ = 0;
    std::thread thread_;
};

class SlowAckMqttBroker {
public:
    explicit SlowAckMqttBroker(std::chrono::milliseconds byteDelay)
        : byteDelay_(byteDelay) {
        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        require(listenFd_ >= 0, "slow ACK broker socket failed");
        int opt = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        require(bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0,
                "slow ACK broker bind failed");
        socklen_t len = sizeof(addr);
        require(getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0,
                "slow ACK broker getsockname failed");
        port_ = ntohs(addr.sin_port);
        require(listen(listenFd_, 1) == 0, "slow ACK broker listen failed");
        thread_ = std::thread([this]() { run(); });
    }

    ~SlowAckMqttBroker() {
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

private:
    void run() {
        const int fd = accept(listenFd_, nullptr, nullptr);
        if (fd < 0) {
            return;
        }
        try {
            const auto connect = readMqttPacket(fd);
            require(!connect.empty() && (connect[0] & 0xF0) == 0x10,
                    "slow ACK broker expected connect");
            const std::uint8_t connAck[] = {0x20, 0x02, 0x00, 0x00};
            send(fd, connAck, sizeof(connAck), MSG_NOSIGNAL);

            const auto publish = readMqttPacket(fd);
            require(!publish.empty() && (publish[0] & 0xF0) == 0x30,
                    "slow ACK broker expected publish");
            const auto id = packetId(publish);
            const std::uint8_t pubAck[] = {
                0x40,
                0x02,
                static_cast<std::uint8_t>((id >> 8) & 0xFF),
                static_cast<std::uint8_t>(id & 0xFF)
            };
            for (const auto byte : pubAck) {
                std::this_thread::sleep_for(byteDelay_);
                if (send(fd, &byte, 1, MSG_NOSIGNAL) != 1) {
                    break;
                }
            }
        } catch (...) {
        }
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }

    std::chrono::milliseconds byteDelay_;
    int listenFd_ = -1;
    int port_ = 0;
    std::thread thread_;
};

void requireReliablePublishRejected(
    RejectingAckMqttBroker::Mode mode,
    const std::string& protocolVersion,
    const std::string& expectedError
) {
    RejectingAckMqttBroker broker(mode);
    edge_gateway::MqttConfig config;
    config.broker = std::string("tcp://127.0.0.1:") + std::to_string(broker.port());
    config.clientId = "GW_ACK_REJECT";
    config.topicMachineCode = "GW_ACK_REJECT";
    config.statusTopic = "edge/status";
    config.protocolVersion = protocolVersion;
    config.qos = 1;
    config.offlineBufferEnabled = false;

    edge_gateway::BuiltinMqttDriverPublisher publisher(config);
    try {
        publisher.publishReliableJsonMessage(config.statusTopic, "{\"ok\":true}");
    } catch (const std::exception& ex) {
        require(std::string(ex.what()).find(expectedError) != std::string::npos,
                "reliable publish failed with unexpected ACK error");
        return;
    }
    throw std::runtime_error("reliable publish must reject invalid ACK");
}

void testReliablePublishRejectsWrongPacketId() {
    requireReliablePublishRejected(
        RejectingAckMqttBroker::Mode::WrongPacketId,
        "mqtt3",
        "packet id mismatch"
    );
}

void testReliablePublishRejectsMqtt5FailureReason() {
    requireReliablePublishRejected(
        RejectingAckMqttBroker::Mode::Mqtt5Rejected,
        "mqtt5",
        "puback rejected"
    );
}

void testReliablePublishHasAbsoluteAckDeadline() {
    SlowAckMqttBroker broker(std::chrono::milliseconds(80));
    edge_gateway::MqttConfig config;
    config.broker = std::string("tcp://127.0.0.1:") + std::to_string(broker.port());
    config.clientId = "GW_SLOW_ACK";
    config.topicMachineCode = "GW_SLOW_ACK";
    config.statusTopic = "edge/status";
    config.qos = 1;
    config.connectTimeoutMs = 120;
    config.offlineBufferEnabled = false;

    const auto startedAt = std::chrono::steady_clock::now();
    bool timedOut = false;
    try {
        edge_gateway::BuiltinMqttDriverPublisher publisher(config);
        publisher.publishReliableJsonMessage(config.statusTopic, "{\"ok\":true}");
    } catch (const std::exception& ex) {
        timedOut = std::string(ex.what()).find("deadline") != std::string::npos;
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt
    ).count();
    require(timedOut, "slow PUBACK must fail at the reliable publish deadline");
    require(elapsedMs < 300, "slow PUBACK must not reset the timeout for every received byte");
}

class LeasedScriptBroker {
public:
    explicit LeasedScriptBroker(std::function<void(int)> script, int connections = 1) {
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        require(listener_ >= 0, "leased broker socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "leased broker bind failed");
        socklen_t length = sizeof(address);
        require(getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0,
            "leased broker getsockname failed");
        port_ = ntohs(address.sin_port);
        require(listen(listener_, 1) == 0, "leased broker listen failed");
        worker_ = std::thread([this, script, connections] {
          for (int connection = 0; connection < connections; ++connection) {
            const auto fd = accept(listener_, nullptr, nullptr);
            if (fd < 0) return;
            timeval timeout{2, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            try { script(fd); } catch (...) { failure_ = std::current_exception(); }
            shutdown(fd, SHUT_RDWR);
            close(fd);
            if (failure_) return;
          }
        });
    }
    ~LeasedScriptBroker() {
        shutdown(listener_, SHUT_RDWR);
        close(listener_);
        if (worker_.joinable()) worker_.join();
    }
    int port() const { return port_; }
    void checked() {
        if (worker_.joinable()) worker_.join();
        if (failure_) std::rethrow_exception(failure_);
    }
private:
    int listener_ = -1, port_ = 0;
    std::thread worker_;
    std::exception_ptr failure_;
};

void testLeasedBatchWindowAndExactTopics() {
    TestMqttBroker broker(16, 0, true, true, 16);
    edge_gateway::MqttConfig config;
    config.broker = "tcp://127.0.0.1:" + std::to_string(broker.port());
    config.clientId = "leased-window"; config.topicMachineCode = "must-not-append";
    config.offlineBufferEnabled = false;
    config.qos = 0;
    edge_gateway::BuiltinMqttDriverPublisher publisher(config, edge_gateway::MqttPublisherMode::TxOnly,
        edge_gateway::MqttEventOutboxOwnership::External);
    std::vector<edge_gateway::MqttJsonMessage> messages;
    for (int i = 0; i < 16; ++i) messages.push_back({"exact/topic/" + std::to_string(i), std::to_string(i)});
    std::vector<std::size_t> attempted, confirmed;
    const auto now = edge_gateway::readEventStoreLeaseTime();
    try {
        publisher.publishLeasedEvents(messages, now.bootId, now.milliseconds + 2000, [] { return true; },
            [&](auto i) { attempted.push_back(i); }, [&](auto i) { confirmed.push_back(i); });
    } catch (...) {
        if (attempted.size() != 16) throw std::runtime_error(
            "leased batch stalled before 16-message ACK barrier; attempts=" + std::to_string(attempted.size()));
        throw;
    }
    require(attempted.size() == 16 && confirmed.size() == 16 && confirmed.front() == 15,
        "leased window was serial or PUBACK mapping changed");
    require(!broker.qos1WindowViolated(), "leased batch exceeded inflight cap");
    const auto captured = broker.messages();
    for (std::size_t i = 0; i < messages.size(); ++i)
        require(captured[i].topic == messages[i].topic && captured[i].payload == messages[i].payload &&
            captured[i].qos == 1 && captured[i].packetId != 0, "leased batch rewrote topic/payload/QoS/id");
    std::sort(confirmed.begin(), confirmed.end());
    for (std::size_t i = 0; i < confirmed.size(); ++i) require(confirmed[i] == i, "leased ACK index duplicated");
}

// 0=disconnect, 1=duplicate, 2=unknown, 3=malformed, 4=deny+drain,
// 5=deadline with outstanding ACKs, 6=MQTT5 failure after successful ACK,
// 7=deny during initial fill after four attempts.
void testLeasedBatchPartial(int mode) {
    const bool mqtt5 = mode == 6;
    LeasedScriptBroker broker([&](int fd) {
        require((readMqttPacket(fd).at(0) & 0xf0) == 0x10, "leased script expected CONNECT");
        const std::vector<std::uint8_t> connack = mqtt5 ? std::vector<std::uint8_t>{0x20, 3, 0, 0, 0} :
            std::vector<std::uint8_t>{0x20, 2, 0, 0};
        send(fd, connack.data(), connack.size(), MSG_NOSIGNAL);
        std::vector<std::uint16_t> ids;
        for (int i = 0; i < (mqtt5 ? 1 : mode == 7 ? 4 : 16); ++i) ids.push_back(packetId(readMqttPacket(fd)));
        require(!socketReadableWithin(fd, 20), "leased window limit exceeded before first ACK");
        if (mode == 5) {
            // Wait for client deadline closure, not an unbounded test sleep.
            char byte;
            require(recv(fd, &byte, 1, 0) == 0, "deadline did not close transport");
            return;
        }
        sendPubAck(fd, ids.back());
        if (mode == 4 || mode == 7) {
            for (int i = static_cast<int>(ids.size()) - 2; i >= 0; --i) sendPubAck(fd, ids[i]);
            require(!socketReadableWithin(fd, 50), "denied batch refilled a slot");
            return;
        }
        if (mqtt5) {
            const auto next = readMqttPacket(fd);
            require((next.at(0) & 0xf0) == 0x30, "MQTT5 window did not refill after one ACK");
            const auto id = packetId(next);
            const std::uint8_t rejected[] = {0x40, 4, static_cast<std::uint8_t>(id >> 8),
                static_cast<std::uint8_t>(id), 0x80, 0};
            send(fd, rejected, sizeof(rejected), MSG_NOSIGNAL);
        } else {
            sendPubAck(fd, ids[2]);
            if (mode == 0) return;
            if (mode == 1) sendPubAck(fd, ids[2]);
            if (mode == 2) sendPubAck(fd, 65535);
            if (mode == 3) {
                const std::uint8_t malformed[] = {0x41, 2, 0, 1};
                send(fd, malformed, sizeof(malformed), MSG_NOSIGNAL);
            }
        }
        char byte;
        require(recv(fd, &byte, 1, 0) == 0, "bad ACK did not close transport");
    });
    edge_gateway::MqttConfig config;
    config.broker = "tcp://127.0.0.1:" + std::to_string(broker.port());
    config.clientId = "leased-partial"; config.offlineBufferEnabled = false;
    if (mqtt5) config.protocolVersion = "mqtt5";
    edge_gateway::BuiltinMqttDriverPublisher publisher(config, edge_gateway::MqttPublisherMode::TxOnly,
        edge_gateway::MqttEventOutboxOwnership::External);
    std::vector<edge_gateway::MqttJsonMessage> messages(16, {"exact/topic", "payload"});
    std::vector<std::size_t> attempts, confirmations;
    bool allowed = true, failed = false;
    const auto now = edge_gateway::readEventStoreLeaseTime();
    const auto start = std::chrono::steady_clock::now();
    try {
        publisher.publishLeasedEvents(messages, now.bootId, now.milliseconds + (mode == 5 ? 150 : 1500),
            [&] { return allowed; }, [&](auto i) {
                attempts.push_back(i);
                if (mode == 7 && attempts.size() == 4) allowed = false;
            }, [&](auto i) {
                confirmations.push_back(i);
                if (mode == 4) allowed = false;
            });
    } catch (...) { failed = true; }
    require(failed == (mode != 4 && mode != 7), "leased transport failure status wrong");
    if (mode < 4) require(confirmations == std::vector<std::size_t>{15, 2} && attempts.size() == 16,
        "partial ACK identities lost or wrong after failure");
    if (mode == 4) require(confirmations.size() == 16 && attempts.size() == 16,
        "authority denial did not drain exact inflight set");
    if (mode == 5) require(confirmations.empty() && attempts.size() == 16 &&
        std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "batch deadline reset per packet");
    if (mode == 6) require(confirmations == std::vector<std::size_t>{0} && attempts.size() == 2,
        "MQTT5 rejected ACK counted or receive window exceeded");
    if (mode == 7) require(confirmations == std::vector<std::size_t>{3, 2, 1, 0} && attempts.size() == 4,
        "authority denial during fill sent new messages or lost confirmations");
    broker.checked();
}

void testLeasedBatchWrapReconnect(bool alreadyWrapped) {
    const int warmup = alreadyWrapped ? 65540 : 65530;
    int connections = 0;
    LeasedScriptBroker broker([&](int fd) {
        require((readMqttPacket(fd).at(0) & 0xf0) == 0x10, "wrap broker expected CONNECT");
        const std::uint8_t connack[] = {0x20, 2, 0, 0};
        send(fd, connack, sizeof(connack), MSG_NOSIGNAL);
        if (++connections == 1) {
            for (int i = 0; i < warmup; ++i) (void)readMqttPacket(fd);
            char byte;
            require(recv(fd, &byte, 1, 0) == 0, "leased batch reused pre-wrap connection");
        } else {
            std::vector<std::uint16_t> ids;
            for (int i = 0; i < 16; ++i) {
                const auto id = packetId(readMqttPacket(fd));
                require(id != 0 && std::find(ids.begin(), ids.end(), id) == ids.end(), "wrap duplicated packet id");
                ids.push_back(id);
                sendPubAck(fd, id);
            }
        }
    }, 2);
    edge_gateway::MqttConfig config;
    config.broker = "tcp://127.0.0.1:" + std::to_string(broker.port());
    config.clientId = "leased-wrap"; config.offlineBufferEnabled = false; config.qos = 0;
    edge_gateway::BuiltinMqttDriverPublisher publisher(config, edge_gateway::MqttPublisherMode::TxOnly,
        edge_gateway::MqttEventOutboxOwnership::External);
    for (int i = 0; i < warmup; ++i) publisher.publishReliableJsonMessage("test/warmup", "x");
    const auto now = edge_gateway::readEventStoreLeaseTime();
    std::size_t confirmed = 0;
    publisher.publishLeasedEvents(std::vector<edge_gateway::MqttJsonMessage>(16, {"exact", "x"}),
        now.bootId, now.milliseconds + 1500, [] { return true; }, [](auto) {}, [&](auto) { ++confirmed; });
    require(confirmed == 16, "wrap lost confirmations");
    broker.checked();
    require(connections == 2, "wrap failed to reconnect before batch");
}

void testLeasedBatchInputGates() {
    edge_gateway::MqttConfig config;
    config.broker = "tcp://127.0.0.1:1"; config.offlineBufferEnabled = false;
    edge_gateway::BuiltinMqttDriverPublisher publisher(config, edge_gateway::MqttPublisherMode::TxOnly,
        edge_gateway::MqttEventOutboxOwnership::External);
    const auto now = edge_gateway::readEventStoreLeaseTime();
    int attempts = 0, confirmations = 0;
    for (int mode = 0; mode < 5; ++mode) {
        auto messages = std::vector<edge_gateway::MqttJsonMessage>(mode == 0 ? 17 : 1, {"exact", "x"});
        if (mode == 1) messages[0].payload.assign(32768, 'x');
        bool failed = false;
        try {
            publisher.publishLeasedEvents(messages, mode == 2 ? "foreign-boot" : now.bootId,
                mode == 3 ? now.milliseconds : now.milliseconds + 1000, [mode] { return mode != 4; },
                [&](auto) { ++attempts; }, [&](auto) { ++confirmations; });
        } catch (...) { failed = true; }
        require(failed == (mode != 4) && attempts == 0 && confirmations == 0, "invalid batch reached network");
    }
}

void testLeasedBatchRejectsPersistentSession() {
    for (int mode = 0; mode < 4; ++mode) {
        edge_gateway::MqttConfig config;
        config.broker = "tcp://127.0.0.1:1";
        config.offlineBufferEnabled = false;
        config.cleanSession = mode >= 2;
        if (mode != 0) config.protocolVersion = "mqtt5";
        if (mode >= 2) config.sessionExpirySec = mode == 2 ? 60 : -1;
        edge_gateway::BuiltinMqttDriverPublisher publisher(config, edge_gateway::MqttPublisherMode::TxOnly,
            edge_gateway::MqttEventOutboxOwnership::External);
        int callbacks = 0;
        const auto now = edge_gateway::readEventStoreLeaseTime();
        bool rejected = false;
        try {
            publisher.publishLeasedEvents({{"exact", "x"}}, now.bootId, now.milliseconds + 1000,
                [&] { ++callbacks; return true; }, [&](auto) { ++callbacks; }, [&](auto) { ++callbacks; });
        } catch (const std::invalid_argument& ex) {
            rejected = std::string(ex.what()) == "leased MQTT batch requires a nonpersistent session";
        }
        require(rejected && callbacks == 0, "persistent batch did not fail before authority/network/observers");
    }
}

void testReliableBatchPipelinesQos1Publishes() {
    constexpr int kBatchSize = 8;
    TestMqttBroker broker(kBatchSize, 0, true, true);
    edge_gateway::MqttConfig config;
    config.broker = std::string("tcp://127.0.0.1:") + std::to_string(broker.port());
    config.clientId = "GW_BATCH_ACK";
    config.topicMachineCode = "GW_BATCH_ACK";
    config.statusTopic = "edge/status";
    config.qos = 1;
    config.connectTimeoutMs = 1000;
    config.offlineBufferEnabled = false;

    std::vector<edge_gateway::MqttJsonMessage> messages;
    for (int index = 0; index < kBatchSize; ++index) {
        messages.push_back(edge_gateway::MqttJsonMessage{
            config.statusTopic,
            std::string("{\"index\":") + std::to_string(index) + "}"
        });
    }
    edge_gateway::BuiltinMqttDriverPublisher publisher(config);
    publisher.publishReliableJsonMessages(messages);

    const auto published = broker.messages();
    require(published.size() == kBatchSize,
        "reliable batch did not pipeline every QoS1 publish before ACK wait");
    for (int index = 0; index < kBatchSize; ++index) {
        require(published[static_cast<std::size_t>(index)].qos == 1,
            "reliable batch changed QoS1 delivery");
        require(published[static_cast<std::size_t>(index)].payload == messages[index].payload,
            "reliable batch changed publish order or payload");
    }
}

void testReliableBatchCapsQos1InFlightWindow() {
    constexpr int kBatchSize = 20;
    constexpr std::size_t kExpectedWindow = 8;
    TestMqttBroker broker(kBatchSize, 0, true, true, kExpectedWindow);
    edge_gateway::MqttConfig config;
    config.broker = std::string("tcp://127.0.0.1:") + std::to_string(broker.port());
    config.clientId = "GW_BATCH_WINDOW";
    config.topicMachineCode = "GW_BATCH_WINDOW";
    config.statusTopic = "edge/status";
    config.qos = 1;
    config.connectTimeoutMs = 2000;
    config.offlineBufferEnabled = false;

    std::vector<edge_gateway::MqttJsonMessage> messages;
    for (int index = 0; index < kBatchSize; ++index) {
        messages.push_back(edge_gateway::MqttJsonMessage{
            config.statusTopic,
            std::string("{\"index\":") + std::to_string(index) + "}"
        });
    }
    edge_gateway::BuiltinMqttDriverPublisher publisher(config);
    publisher.publishReliableJsonMessages(messages);

    require(broker.messages().size() == kBatchSize,
        "bounded reliable batch lost a QoS1 publish");
    require(!broker.qos1WindowViolated(),
        "MQTT3 reliable batch exceeded the eight-publish in-flight window");
}

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

void testManagementOnlyPublisherNeverConsumesBusinessOutbox() {
    const auto dir = std::string("/tmp/gateway_mqtt_management_only_test_") +
        std::to_string(getpid());
    require(std::system((std::string("mkdir -p ") + dir).c_str()) == 0,
        "management-only test mkdir failed");
    const auto port = unusedTcpPort();

    edge_gateway::MqttConfig config;
    config.broker = std::string("tcp://127.0.0.1:") + std::to_string(port);
    config.clientId = "GW_MANAGEMENT_ONLY";
    config.topicMachineCode = "GW_MANAGEMENT_ONLY";
    config.statusTopic = "edge/status";
    config.offlineBufferEnabled = true;
    config.offlineBufferDir = dir;
    config.offlineRealtimeFile = dir + "/realtime_ring.dat";
    config.offlineRealtimeFileSizeBytes = 64 * 1024;
    config.eventOutboxSqlitePath = dir + "/event_outbox.db";

    {
        edge_gateway::MqttEventOutbox outbox(
            config.eventOutboxSqlitePath,
            config.eventOutboxSqliteLibraryPath,
            config.eventOutboxRetentionMonths,
            config.eventOutboxCleanupIntervalHours,
            config.eventOutboxReplayBatchSize,
            config.eventOutboxMaxDiskBytes
        );
        require(outbox.enqueue("alarm", "edge/alarm/GW_MANAGEMENT_ONLY", "{\"active\":true}", 1000) > 0,
            "failed to seed business outbox row");
    }

    {
        TestMqttBroker broker(1, port);
        edge_gateway::BuiltinMqttDriverPublisher publisher(
            config,
            edge_gateway::MqttPublisherMode::Bidirectional,
            edge_gateway::MqttEventOutboxOwnership::ManagementOnly
        );
        publisher.publishJsonMessage(config.statusTopic, "{\"ok\":true}");
        const auto messages = broker.messages();
        require(messages.size() == 1 &&
                messages.front().topic == "edge/status/GW_MANAGEMENT_ONLY",
            "management-only publisher must send management data without replaying business events");
    }
    {
        edge_gateway::MqttEventOutbox outbox(
            config.eventOutboxSqlitePath,
            config.eventOutboxSqliteLibraryPath,
            config.eventOutboxRetentionMonths,
            config.eventOutboxCleanupIntervalHours,
            config.eventOutboxReplayBatchSize,
            config.eventOutboxMaxDiskBytes
        );
        require(outbox.pendingCount("main") == 1,
            "management-only publisher acknowledged a business event it does not own");
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

void testPacketIdentifierSkipsZeroAfterWrap() {
    constexpr int kPublishCount = 65536;
    TestMqttBroker broker(kPublishCount);
    edge_gateway::MqttConfig config;
    config.broker = std::string("tcp://127.0.0.1:") + std::to_string(broker.port());
    config.clientId = "GW_PACKET_ID_WRAP";
    config.topicMachineCode = "GW_PACKET_ID_WRAP";
    config.statusTopic = "edge/status";
    config.qos = 1;
    config.offlineBufferEnabled = false;

    edge_gateway::BuiltinMqttDriverPublisher publisher(config);
    for (int i = 0; i < kPublishCount; ++i) {
        publisher.publishReliableJsonMessage(config.statusTopic, "{}");
    }

    const auto messages = broker.messages();
    require(messages.size() == kPublishCount, "packet-id wrap test lost a publish");
    require(
        std::all_of(messages.begin(), messages.end(), [](const auto& message) {
            return message.packetId != 0;
        }),
        "MQTT packet identifier must never be zero"
    );
    require(
        messages[65534].packetId == 65535 && messages[65535].packetId == 1,
        "MQTT packet identifier must wrap from 65535 to 1"
    );
}

void testFullTelemetryTopicScopingMode() {
    TestMqttBroker broker(6);
    edge_gateway::MqttConfig config;
    config.broker = std::string("tcp://127.0.0.1:") + std::to_string(broker.port());
    config.clientId = "GW_FULL_TOPIC";
    config.topicMachineCode = "GW_FULL_TOPIC";
    config.fullTelemetryTopic = "third/site/full";
    config.changeEventTopic = "third/site/change";
    config.alarmTopic = "third/site/alarm";
    config.offlineBufferEnabled = false;

    edge_gateway::StoredPointValue point;
    point.index = 1001;
    point.value = 1.0;
    point.ts = 1000;
    {
        edge_gateway::BuiltinMqttDriverPublisher publisher(config);
        publisher.publishFullSnapshot(config.fullTelemetryTopic, {point}, "object");
        publisher.publishChangeEvent(config.changeEventTopic, point);
        publisher.publishAlarm(config.alarmTopic, point.index, point, "high", true);
    }
    config.clientId = "GW_FULL_TOPIC_EXACT";
    config.fullTelemetryTopicMachineScoped = false;
    config.changeEventTopicMachineScoped = false;
    config.alarmTopicMachineScoped = false;
    {
        edge_gateway::BuiltinMqttDriverPublisher publisher(config);
        publisher.publishFullSnapshot(config.fullTelemetryTopic, {point}, "object");
        publisher.publishChangeEvent(config.changeEventTopic, point);
        publisher.publishAlarm(config.alarmTopic, point.index, point, "high", true);
    }

    const auto messages = broker.messages();
    require(messages.size() == 6, "test broker should capture scoped and exact telemetry events");
    require(
        messages[0].topic == "third/site/full/GW_FULL_TOPIC",
        "full telemetry topic must remain machine-scoped by default"
    );
    require(
        messages[1].topic == "third/site/change/GW_FULL_TOPIC" &&
            messages[2].topic == "third/site/alarm/GW_FULL_TOPIC",
        "change and alarm topics must remain machine-scoped by default"
    );
    require(
        messages[3].topic == "third/site/full",
        "explicit exact full telemetry topic must not append machineCode"
    );
    require(
        messages[4].topic == "third/site/change" &&
            messages[5].topic == "third/site/alarm",
        "explicit exact change and alarm topics must not append machineCode"
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

void testRealtimeSessionIdIsPresentInEveryFormatChunkAndAbsentFromFull() {
    TestMqttBroker broker(16);
    edge_gateway::MqttConfig config;
    config.broker = std::string("tcp://127.0.0.1:") + std::to_string(broker.port());
    config.clientId = "GW_REALTIME_SESSION";
    config.topicMachineCode = "GW_TEST";
    config.realtimeTelemetryTopic = "edge/telemetry/realtime";
    config.fullTelemetryTopic = "edge/telemetry/full";
    config.qos = 1;
    config.offlineBufferEnabled = false;
    config.maxPayloadBytes = 4096;

    std::vector<edge_gateway::StoredPointValue> values;
    for (std::uint32_t i = 0; i < 10; ++i) {
        edge_gateway::StoredPointValue value;
        value.index = 1000 + i;
        value.machineCode = "GW_TEST";
        value.meterCode = "METER_A";
        value.pointCode = std::string(1000, static_cast<char>('A' + i));
        value.value = static_cast<double>(i);
        value.quality = 0;
        value.ts = 1770000000000LL;
        value.expireAt = 1770000600000LL;
        values.push_back(value);
    }

    {
        edge_gateway::BuiltinMqttDriverPublisher publisher(config);
        publisher.publishRealtime(config.realtimeTelemetryTopic, values, "compactArray", "SESSION_COMPACT");
        publisher.publishRealtime(config.realtimeTelemetryTopic, values, "object", "SESSION_OBJECT");
        publisher.publishFullSnapshot(config.fullTelemetryTopic, values, "compactArray");
        publisher.publishFullSnapshot(config.fullTelemetryTopic, values, "object");
    }

    const auto messages = broker.messages();
    require(messages.size() == 16, "chunk fixture should produce four chunks for each publication");
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const bool realtime = i < 8;
        require(messages[i].payload.size() <= config.maxPayloadBytes,
            "session metadata must be included in chunk size accounting");
        require(
            messages[i].topic == (realtime ? "edge/telemetry/realtime/GW_TEST" : "edge/telemetry/full/GW_TEST"),
            "realtime/full topic must remain unchanged"
        );
        if (i < 4) {
            require(
                messages[i].payload.find("\"machineCode\":\"GW_TEST\",\"sessionId\":\"SESSION_COMPACT\"") != std::string::npos,
                "every compact realtime chunk should contain the original top-level sessionId"
            );
            require(messages[i].payload.find("\"values\":[[") != std::string::npos,
                "compact realtime chunk should keep compact array values");
        } else if (i < 8) {
            require(
                messages[i].payload.find("\"machineCode\":\"GW_TEST\",\"sessionId\":\"SESSION_OBJECT\"") != std::string::npos,
                "every object realtime chunk should contain the original top-level sessionId"
            );
            require(messages[i].payload.find("\"values\":[{") != std::string::npos,
                "object realtime chunk should keep structured values");
        } else {
            require(messages[i].payload.find("\"sessionId\"") == std::string::npos,
                "full payload must not fabricate a realtime sessionId");
        }
    }
}

void testRealtimeRingReplayPreservesSerializedSessionId() {
    const auto dir = std::string("/tmp/gateway_mqtt_realtime_session_test_") + std::to_string(getpid());
    require(std::system((std::string("mkdir -p ") + dir).c_str()) == 0, "realtime ring test mkdir failed");
    const auto ringPath = dir + "/realtime_ring.dat";
    const std::string topic = "edge/telemetry/realtime/GW_TEST";
    const std::string payload =
        "{\"type\":\"telemetry\",\"machineCode\":\"GW_TEST\",\"sessionId\":\"SESSION_REPLAY\",\"meters\":[]}";

    edge_gateway::MqttRealtimeRingBuffer ring(ringPath, 16ULL * 1024ULL * 1024ULL, 1024U * 1024U, 10);
    ring.enqueue(topic, payload);
    std::string replayedTopic;
    std::string replayedPayload;
    const auto replayed = ring.replay([&](const std::string& valueTopic, const std::string& valuePayload) {
        replayedTopic = valueTopic;
        replayedPayload = valuePayload;
    });
    require(replayed == 1, "realtime ring should replay the queued payload once");
    require(replayedTopic == topic, "realtime ring replay should preserve the scoped topic");
    require(replayedPayload == payload, "realtime ring replay should preserve serialized sessionId exactly");
    require(ring.replay([](const std::string&, const std::string&) {}) == 0,
        "realtime ring should remove a successfully replayed payload");

    std::remove(ringPath.c_str());
    rmdir(dir.c_str());
}

void testRealtimeLegacyEscapingAndTxOnlyIsolation() {
    TestMqttBroker broker(4);
    edge_gateway::MqttConfig config;
    config.broker = "tcp://127.0.0.1:" + std::to_string(broker.port());
    config.topicMachineCode = "GW_TEST";
    config.clientId = "GW_SESSION_COMPAT";
    config.realtimeTelemetryTopic = "edge/telemetry/realtime";
    config.fullTelemetryTopic = "edge/telemetry/full";
    config.offlineBufferEnabled = false;
    config.qos = 1;
    {
        edge_gateway::BuiltinMqttDriverPublisher publisher(config);
        publisher.publishOnDemand(config.realtimeTelemetryTopic, {}, "compactArray");
        publisher.publishRealtime(config.realtimeTelemetryTopic, {}, "object", "A\"\\\nB");
    }
    {
        edge_gateway::BuiltinMqttDriverPublisher publisher(config, edge_gateway::MqttPublisherMode::TxOnly);
        publisher.publishRealtime(config.realtimeTelemetryTopic, {}, "object", "MUST_NOT_PUBLISH");
        publisher.publishOnDemand(config.realtimeTelemetryTopic, {}, "compactArray");
        publisher.publishFullSnapshot(config.fullTelemetryTopic, {}, "object");
        publisher.publishFullSnapshot(config.fullTelemetryTopic, {}, "compactArray");
    }
    const auto messages = broker.messages();
    require(messages.size() == 4, "TX-only must suppress both realtime entry points while allowing full");
    require(messages[0].payload.find("\"sessionId\"") == std::string::npos,
        "legacy one-shot must not fabricate a sessionId");
    require(messages[1].payload.find(R"("sessionId":"A\"\\\nB")") != std::string::npos,
        "sessionId must retain JSON escaping");
    require(messages[1].payload.find("\"machineCode\":\"GW_TEST\"") != std::string::npos,
        "empty realtime response must keep fallback machine identity");
    for (std::size_t i = 2; i < messages.size(); ++i) {
        require(messages[i].topic == "edge/telemetry/full/GW_TEST", "TX-only must keep full topic");
        require(messages[i].payload.find("\"sessionId\"") == std::string::npos,
            "TX-only full must remain unscoped by session");
        require(messages[i].payload.find("\"type\":\"snapshot\"") != std::string::npos,
            "TX-only full must remain a snapshot");
    }
    std::cout << "Builtin session chunks, formats, legacy, escaping, TX-only/full isolation passed\n";
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
    testManagementOnlyPublisherNeverConsumesBusinessOutbox();
    testControlTopicsUseQos2();
    testPacketIdentifierSkipsZeroAfterWrap();
    testFullTelemetryTopicScopingMode();
    testNonFinitePointValuesProduceValidJson();
    testClosedTxConnectionReconnectsBeforeNextPublish();
    testUnreachableBrokerHonorsConnectTimeout();
    testReliablePublishRejectsWrongPacketId();
    testReliablePublishRejectsMqtt5FailureReason();
    testReliablePublishHasAbsoluteAckDeadline();
    testReliableBatchPipelinesQos1Publishes();
    testReliableBatchCapsQos1InFlightWindow();
    testLeasedBatchWindowAndExactTopics();
    for (int mode = 0; mode < 8; ++mode) testLeasedBatchPartial(mode);
    testLeasedBatchWrapReconnect(false);
    testLeasedBatchWrapReconnect(true);
    testLeasedBatchInputGates();
    testLeasedBatchRejectsPersistentSession();
    testRealtimeSessionIdIsPresentInEveryFormatChunkAndAbsentFromFull();
    testRealtimeRingReplayPreservesSerializedSessionId();
    testRealtimeLegacyEscapingAndTxOnlyIsolation();
#endif

    std::cout << "builtin_mqtt_driver_publisher_test passed" << std::endl;
    return 0;
}
