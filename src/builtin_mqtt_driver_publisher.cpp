#include "edge_gateway/builtin_mqtt_driver_publisher.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace edge_gateway {

namespace {

constexpr std::uint8_t kPacketConnect = 0x10;
constexpr std::uint8_t kPacketConnAck = 0x20;
constexpr std::uint8_t kPacketPublishQos0 = 0x30;
constexpr std::uint8_t kPacketPublishQos1 = 0x32;
constexpr std::uint8_t kPacketPubAck = 0x40;
constexpr std::uint8_t kPacketSubscribe = 0x82;
constexpr std::uint8_t kPacketSubAck = 0x90;
constexpr std::uint8_t kPacketPingReq = 0xC0;
constexpr std::uint8_t kPacketPingResp = 0xD0;
constexpr std::uint8_t kPacketDisconnect = 0xE0;

std::string scopedTopic(const std::string& topic, const std::string& machineCode) {
    if (topic.empty() || machineCode.empty()) {
        return topic;
    }
    const std::string suffix = "/" + machineCode;
    if (topic.size() >= suffix.size() &&
        topic.compare(topic.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return topic;
    }
    return topic + suffix;
}

struct BrokerEndpoint {
    std::string host = "127.0.0.1";
    int port = 1883;
};

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

std::string escapeJson(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const auto ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

BrokerEndpoint parseBroker(const std::string& broker) {
    BrokerEndpoint endpoint;
    auto address = broker;
    const auto schemePos = address.find("://");
    if (schemePos != std::string::npos) {
        address = address.substr(schemePos + 3);
    }
    const auto colonPos = address.rfind(':');
    if (colonPos != std::string::npos) {
        endpoint.host = address.substr(0, colonPos);
        endpoint.port = std::stoi(address.substr(colonPos + 1));
    } else if (!address.empty()) {
        endpoint.host = address;
    }
    return endpoint;
}

void appendString(std::vector<std::uint8_t>& bytes, const std::string& value) {
    if (value.size() > 0xFFFF) {
        throw std::runtime_error("mqtt string too long");
    }
    bytes.push_back(static_cast<std::uint8_t>((value.size() >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value.size() & 0xFF));
    bytes.insert(bytes.end(), value.begin(), value.end());
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

void appendVarint(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    do {
        std::uint8_t encoded = static_cast<std::uint8_t>(value % 128);
        value /= 128;
        if (value > 0) {
            encoded |= 0x80;
        }
        bytes.push_back(encoded);
    } while (value > 0);
}

std::size_t decodeRemainingLength(const std::vector<std::uint8_t>& packet, std::size_t* headerSize) {
    std::size_t multiplier = 1;
    std::size_t value = 0;
    std::size_t offset = 1;
    std::uint8_t encoded = 0;
    do {
        if (offset >= packet.size()) {
            throw std::runtime_error("mqtt malformed remaining length");
        }
        encoded = packet[offset++];
        value += (encoded & 0x7F) * multiplier;
        multiplier *= 128;
    } while ((encoded & 0x80) != 0);
    *headerSize = offset;
    return value;
}

std::vector<std::uint8_t> buildConnectPacket(const MqttConfig& config, const std::string& clientIdSuffix) {
    std::vector<std::uint8_t> variableHeader;
    std::vector<std::uint8_t> payload;

    if (config.protocolVersion == "mqtt5") {
        appendString(variableHeader, "MQTT");
        variableHeader.push_back(0x05);
        std::uint8_t flags = 0;
        if (config.cleanSession) {
            flags |= 0x02;
        }
        if (!config.username.empty()) {
            flags |= 0x80;
        }
        if (!config.password.empty()) {
            flags |= 0x40;
        }
        variableHeader.push_back(flags);
        variableHeader.push_back(static_cast<std::uint8_t>((config.keepAliveSec >> 8) & 0xFF));
        variableHeader.push_back(static_cast<std::uint8_t>(config.keepAliveSec & 0xFF));

        std::vector<std::uint8_t> properties;
        if (config.sessionExpirySec > 0) {
            properties.push_back(0x11);
            properties.push_back(static_cast<std::uint8_t>((config.sessionExpirySec >> 24) & 0xFF));
            properties.push_back(static_cast<std::uint8_t>((config.sessionExpirySec >> 16) & 0xFF));
            properties.push_back(static_cast<std::uint8_t>((config.sessionExpirySec >> 8) & 0xFF));
            properties.push_back(static_cast<std::uint8_t>(config.sessionExpirySec & 0xFF));
        }
        appendVarint(variableHeader, static_cast<std::uint32_t>(properties.size()));
        variableHeader.insert(variableHeader.end(), properties.begin(), properties.end());
    } else {
        appendString(variableHeader, "MQTT");
        variableHeader.push_back(0x04);
        std::uint8_t flags = 0;
        if (config.cleanSession) {
            flags |= 0x02;
        }
        if (!config.username.empty()) {
            flags |= 0x80;
        }
        if (!config.password.empty()) {
            flags |= 0x40;
        }
        variableHeader.push_back(flags);
        variableHeader.push_back(static_cast<std::uint8_t>((config.keepAliveSec >> 8) & 0xFF));
        variableHeader.push_back(static_cast<std::uint8_t>(config.keepAliveSec & 0xFF));
    }

    appendString(payload, config.clientId + clientIdSuffix);
    if (!config.username.empty()) {
        appendString(payload, config.username);
    }
    if (!config.password.empty()) {
        appendString(payload, config.password);
    }

    std::vector<std::uint8_t> packet;
    packet.push_back(kPacketConnect);
    appendRemainingLength(packet, variableHeader.size() + payload.size());
    packet.insert(packet.end(), variableHeader.begin(), variableHeader.end());
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

std::vector<std::uint8_t> buildPublishPacket(
    const MqttConfig& config,
    const std::string& topic,
    const std::string& payload,
    std::uint16_t packetId
) {
    std::vector<std::uint8_t> variableHeader;
    appendString(variableHeader, topic);
    if (config.qos > 0) {
        variableHeader.push_back(static_cast<std::uint8_t>((packetId >> 8) & 0xFF));
        variableHeader.push_back(static_cast<std::uint8_t>(packetId & 0xFF));
    }
    if (config.protocolVersion == "mqtt5") {
        appendVarint(variableHeader, 0);
    }

    std::vector<std::uint8_t> packet;
    packet.push_back(config.qos > 0 ? kPacketPublishQos1 : kPacketPublishQos0);
    appendRemainingLength(packet, variableHeader.size() + payload.size());
    packet.insert(packet.end(), variableHeader.begin(), variableHeader.end());
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

std::vector<std::uint8_t> buildSubscribePacket(
    const MqttConfig& config,
    const std::string& commandTopic,
    const std::string& otaTopic,
    const std::string& systemMonitorTopic,
    const std::string& diagTopic,
    const std::string& configPullTopic,
    std::uint16_t packetId
) {
    std::vector<std::uint8_t> variableHeader;
    variableHeader.push_back(static_cast<std::uint8_t>((packetId >> 8) & 0xFF));
    variableHeader.push_back(static_cast<std::uint8_t>(packetId & 0xFF));
    if (config.protocolVersion == "mqtt5") {
        appendVarint(variableHeader, 0);
    }

    std::vector<std::uint8_t> payload;
    if (!commandTopic.empty()) {
        appendString(payload, commandTopic);
        payload.push_back(static_cast<std::uint8_t>(config.qos > 0 ? 1 : 0));
    }
    if (!otaTopic.empty()) {
        appendString(payload, otaTopic);
        payload.push_back(static_cast<std::uint8_t>(config.qos > 0 ? 1 : 0));
    }
    if (!systemMonitorTopic.empty()) {
        appendString(payload, systemMonitorTopic);
        payload.push_back(static_cast<std::uint8_t>(config.qos > 0 ? 1 : 0));
    }
    if (!diagTopic.empty()) {
        appendString(payload, diagTopic);
        payload.push_back(static_cast<std::uint8_t>(config.qos > 0 ? 1 : 0));
    }
    if (!configPullTopic.empty()) {
        appendString(payload, configPullTopic);
        payload.push_back(static_cast<std::uint8_t>(config.qos > 0 ? 1 : 0));
    }

    std::vector<std::uint8_t> packet;
    packet.push_back(kPacketSubscribe);
    appendRemainingLength(packet, variableHeader.size() + payload.size());
    packet.insert(packet.end(), variableHeader.begin(), variableHeader.end());
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

std::vector<std::uint8_t> buildPubAckPacket(std::uint16_t packetId, bool mqtt5) {
    std::vector<std::uint8_t> packet;
    packet.push_back(kPacketPubAck);
    if (mqtt5) {
        packet.push_back(0x04);
        packet.push_back(static_cast<std::uint8_t>((packetId >> 8) & 0xFF));
        packet.push_back(static_cast<std::uint8_t>(packetId & 0xFF));
        packet.push_back(0x00);
        packet.push_back(0x00);
    } else {
        packet.push_back(0x02);
        packet.push_back(static_cast<std::uint8_t>((packetId >> 8) & 0xFF));
        packet.push_back(static_cast<std::uint8_t>(packetId & 0xFF));
    }
    return packet;
}

std::vector<std::uint8_t> buildPingReqPacket() {
    return {kPacketPingReq, 0x00};
}

std::vector<std::uint8_t> buildDisconnectPacket() {
    return {kPacketDisconnect, 0x00};
}

void appendPointValueJson(std::ostringstream& out, const StoredPointValue& item) {
    out << "{\"index\":" << item.index
        << ",\"pointCode\":\"" << escapeJson(item.pointCode) << "\""
        << ",\"value\":" << item.value
        << ",\"quality\":" << item.quality
        << ",\"ts\":" << item.ts
        << ",\"expireAt\":" << item.expireAt
        << ",\"stale\":" << (item.stale ? "true" : "false")
        << "}";
}

std::int64_t currentTimeMs();

std::string firstMachineCode(const std::vector<StoredPointValue>& values) {
    for (const auto& item : values) {
        if (!item.machineCode.empty()) {
            return item.machineCode;
        }
    }
    return "";
}

std::vector<std::string> meterCodesFromValues(const std::vector<StoredPointValue>& values) {
    std::vector<std::string> devices;
    for (const auto& item : values) {
        if (item.meterCode.empty()) {
            continue;
        }
        if (std::find(devices.begin(), devices.end(), item.meterCode) == devices.end()) {
            devices.push_back(item.meterCode);
        }
    }
    return devices;
}

std::string encodeValuesJson(const std::vector<StoredPointValue>& values) {
    std::ostringstream out;
    out << "{\"type\":\"telemetry\",\"machineCode\":\"" << escapeJson(firstMachineCode(values)) << "\",\"meters\":[";
    const auto devices = meterCodesFromValues(values);
    for (std::size_t d = 0; d < devices.size(); ++d) {
        if (d > 0) {
            out << ",";
        }
        out << "{\"meterCode\":\"" << escapeJson(devices[d]) << "\",\"values\":[";
        bool firstValue = true;
        for (const auto& item : values) {
            if (item.meterCode != devices[d]) {
                continue;
            }
            if (!firstValue) {
                out << ",";
            }
            appendPointValueJson(out, item);
            firstValue = false;
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}

std::string encodeFullJson(const std::vector<StoredPointValue>& values) {
    std::ostringstream out;
    out << "{\"type\":\"snapshot\",\"machineCode\":\"" << escapeJson(firstMachineCode(values)) << "\",\"meters\":[";
    const auto devices = meterCodesFromValues(values);
    for (std::size_t d = 0; d < devices.size(); ++d) {
        if (d > 0) {
            out << ",";
        }
        out << "{\"meterCode\":\"" << escapeJson(devices[d]) << "\",\"values\":[";
        bool firstValue = true;
        for (const auto& item : values) {
            if (item.meterCode != devices[d]) {
                continue;
            }
            if (!firstValue) {
                out << ",";
            }
            appendPointValueJson(out, item);
            firstValue = false;
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}

std::string randomChunkId() {
    static std::uint64_t counter = 0;
    std::ostringstream out;
    out << currentTimeMs() << "-" << ++counter;
    return out.str();
}

std::string buildRealtimeChunkJson(
    const std::string& type,
    const std::string& machineCode,
    const std::string& chunkId,
    std::size_t chunkIndex,
    std::size_t chunkCount,
    const std::vector<std::pair<std::string, std::vector<std::string>>>& meters
) {
    std::ostringstream out;
    out << "{\"type\":\"" << escapeJson(type) << "\""
        << ",\"machineCode\":\"" << escapeJson(machineCode) << "\""
        << ",\"chunked\":true"
        << ",\"chunkId\":\"" << escapeJson(chunkId) << "\""
        << ",\"chunkIndex\":" << chunkIndex
        << ",\"chunkCount\":" << chunkCount
        << ",\"meters\":[";
    for (std::size_t i = 0; i < meters.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        out << "{\"meterCode\":\"" << escapeJson(meters[i].first) << "\",\"values\":[";
        for (std::size_t j = 0; j < meters[i].second.size(); ++j) {
            if (j > 0) {
                out << ",";
            }
            out << meters[i].second[j];
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}

std::vector<std::string> encodeRealtimeChunks(
    const std::string& type,
    const std::vector<StoredPointValue>& values,
    std::size_t maxPayloadBytes
) {
    const auto limit = std::max<std::size_t>(4096, maxPayloadBytes);
    const auto machineCode = firstMachineCode(values);
    const auto chunkId = randomChunkId();
    std::vector<std::vector<std::pair<std::string, std::vector<std::string>>>> chunks;
    std::vector<std::pair<std::string, std::vector<std::string>>> currentMeters;

    const auto flushCurrent = [&]() {
        if (!currentMeters.empty()) {
            chunks.push_back(currentMeters);
            currentMeters.clear();
        }
    };

    const auto currentSize = [&]() {
        return buildRealtimeChunkJson(type, machineCode, chunkId, 1, 1, currentMeters).size();
    };

    const auto devices = meterCodesFromValues(values);
    for (const auto& meterCode : devices) {
        currentMeters.push_back(std::make_pair(meterCode, std::vector<std::string>()));
        for (const auto& item : values) {
            if (item.meterCode != meterCode) {
                continue;
            }
            std::ostringstream itemJson;
            appendPointValueJson(itemJson, item);
            auto& meter = currentMeters.back();
            meter.second.push_back(itemJson.str());
            if (currentSize() > limit && meter.second.size() > 1) {
                const auto overflow = meter.second.back();
                meter.second.pop_back();
                flushCurrent();
                currentMeters.push_back(std::make_pair(meterCode, std::vector<std::string>{overflow}));
            } else if (currentSize() > limit && meter.second.size() == 1) {
                flushCurrent();
                currentMeters.push_back(std::make_pair(meterCode, std::vector<std::string>()));
            }
        }
        if (currentMeters.back().second.empty()) {
            currentMeters.pop_back();
        }
    }
    flushCurrent();

    if (chunks.empty()) {
        chunks.push_back({});
    }

    std::vector<std::string> payloads;
    payloads.reserve(chunks.size());
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        payloads.push_back(buildRealtimeChunkJson(type, machineCode, chunkId, i + 1, chunks.size(), chunks[i]));
    }
    return payloads;
}

std::string encodeAlarmJson(std::uint32_t index, const StoredPointValue& value, const std::string& alarmType, bool active) {
    std::ostringstream out;
    out << "{\"type\":\"alarm\""
        << ",\"machineCode\":\"" << escapeJson(value.machineCode) << "\""
        << ",\"meterCode\":\"" << escapeJson(value.meterCode) << "\""
        << ",\"index\":" << index
        << ",\"pointCode\":\"" << escapeJson(value.pointCode) << "\""
        << ",\"alarmType\":\"" << escapeJson(alarmType) << "\""
        << ",\"active\":" << (active ? "true" : "false")
        << ",\"value\":" << value.value
        << ",\"quality\":" << value.quality
        << ",\"ts\":" << value.ts
        << ",\"stale\":" << (value.stale ? "true" : "false")
        << "}";
    return out.str();
}

std::string encodeChangeEventJson(const StoredPointValue& value) {
    std::ostringstream out;
    out << "{\"type\":\"change\""
        << ",\"machineCode\":\"" << escapeJson(value.machineCode) << "\""
        << ",\"meterCode\":\"" << escapeJson(value.meterCode) << "\""
        << ",\"index\":" << value.index
        << ",\"pointCode\":\"" << escapeJson(value.pointCode) << "\""
        << ",\"value\":" << value.value
        << ",\"quality\":" << value.quality
        << ",\"ts\":" << value.ts
        << ",\"expireAt\":" << value.expireAt
        << ",\"stale\":" << (value.stale ? "true" : "false")
        << "}";
    return out.str();
}

std::string encodeCommandReplyJson(const MqttCommandReply& reply) {
    std::ostringstream out;
    out << "{\"cmdId\":\"" << escapeJson(reply.cmdId) << "\""
        << ",\"machineCode\":\"" << escapeJson(reply.machineCode) << "\""
        << ",\"meterCode\":\"" << escapeJson(reply.meterCode) << "\""
        << ",\"pointCode\":\"" << escapeJson(reply.pointCode) << "\""
        << ",\"index\":" << reply.index
        << ",\"success\":" << (reply.success ? "true" : "false")
        << ",\"message\":\"" << escapeJson(reply.message) << "\""
        << ",\"ts\":" << reply.ts
        << "}";
    return out.str();
}

std::string encodeOtaReplyJson(const OtaReply& reply) {
    std::ostringstream out;
    out << "{\"jobId\":\"" << escapeJson(reply.jobId) << "\""
        << ",\"machineCode\":\"" << escapeJson(reply.machineCode) << "\""
        << ",\"accepted\":" << (reply.accepted ? "true" : "false")
        << ",\"message\":\"" << escapeJson(reply.message) << "\""
        << ",\"ts\":" << reply.ts
        << "}";
    return out.str();
}

std::string encodeOtaStatusJson(const OtaStatus& status) {
    std::ostringstream out;
    out << "{\"jobId\":\"" << escapeJson(status.jobId) << "\""
        << ",\"machineCode\":\"" << escapeJson(status.machineCode) << "\""
        << ",\"stage\":\"" << escapeJson(status.stage) << "\""
        << ",\"progress\":" << status.progress
        << ",\"message\":\"" << escapeJson(status.message) << "\""
        << ",\"ts\":" << status.ts
        << "}";
    return out.str();
}

void closeSocket(SocketHandle sock) {
    if (sock == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

class SocketGuard {
public:
    explicit SocketGuard(SocketHandle sock) : sock_(sock) {
    }
    ~SocketGuard() {
        closeSocket(sock_);
    }
private:
    SocketHandle sock_;
};

void ensureSocketRuntime() {
#ifdef _WIN32
    static bool initialized = false;
    if (!initialized) {
        WSADATA wsaData{};
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
        initialized = true;
    }
#endif
}

SocketHandle connectTcp(const BrokerEndpoint& endpoint) {
    ensureSocketRuntime();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const auto port = std::to_string(endpoint.port);
    if (getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &result) != 0) {
        throw std::runtime_error("getaddrinfo failed");
    }

    SocketHandle sock = kInvalidSocket;
    for (auto* addr = result; addr != nullptr; addr = addr->ai_next) {
        sock = static_cast<SocketHandle>(socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol));
        if (sock == kInvalidSocket) {
            continue;
        }
        if (connect(sock, addr->ai_addr, static_cast<int>(addr->ai_addrlen)) == 0) {
            break;
        }
        closeSocket(sock);
        sock = kInvalidSocket;
    }
    freeaddrinfo(result);

    if (sock == kInvalidSocket) {
        throw std::runtime_error("mqtt tcp connect failed");
    }
    return sock;
}

void sendAll(SocketHandle sock, const std::vector<std::uint8_t>& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
#ifdef _WIN32
        const auto rc = send(sock, reinterpret_cast<const char*>(bytes.data() + sent), static_cast<int>(bytes.size() - sent), 0);
#else
        const auto rc = send(sock, bytes.data() + sent, bytes.size() - sent, 0);
#endif
        if (rc <= 0) {
            throw std::runtime_error("mqtt send failed");
        }
        sent += static_cast<std::size_t>(rc);
    }
}

std::uint8_t recvByte(SocketHandle sock) {
    std::uint8_t byte = 0;
#ifdef _WIN32
    const auto rc = recv(sock, reinterpret_cast<char*>(&byte), 1, 0);
#else
    const auto rc = recv(sock, &byte, 1, 0);
#endif
    if (rc != 1) {
        throw std::runtime_error("mqtt recv failed");
    }
    return byte;
}

std::vector<std::uint8_t> recvExact(SocketHandle sock, std::size_t len) {
    std::vector<std::uint8_t> bytes(len);
    std::size_t got = 0;
    while (got < len) {
#ifdef _WIN32
        const auto rc = recv(sock, reinterpret_cast<char*>(bytes.data() + got), static_cast<int>(len - got), 0);
#else
        const auto rc = recv(sock, bytes.data() + got, len - got, 0);
#endif
        if (rc <= 0) {
            throw std::runtime_error("mqtt recv payload failed");
        }
        got += static_cast<std::size_t>(rc);
    }
    return bytes;
}

std::vector<std::uint8_t> readPacket(SocketHandle sock) {
    std::vector<std::uint8_t> packet;
    packet.push_back(recvByte(sock));
    std::size_t multiplier = 1;
    std::size_t remaining = 0;
    std::uint8_t encoded = 0;
    do {
        encoded = recvByte(sock);
        packet.push_back(encoded);
        remaining += (encoded & 0x7F) * multiplier;
        multiplier *= 128;
    } while ((encoded & 0x80) != 0);
    const auto body = recvExact(sock, remaining);
    packet.insert(packet.end(), body.begin(), body.end());
    return packet;
}

bool waitReadable(SocketHandle sock, int timeoutMs) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(sock, &readSet);
    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
#ifdef _WIN32
    const auto rc = select(0, &readSet, nullptr, nullptr, &timeout);
#else
    const auto rc = select(sock + 1, &readSet, nullptr, nullptr, &timeout);
#endif
    if (rc < 0) {
        throw std::runtime_error("mqtt select failed");
    }
    return rc > 0;
}

void validateConnAck(const MqttConfig& config, const std::vector<std::uint8_t>& packet) {
    if (packet.empty() || (packet[0] & 0xF0) != kPacketConnAck) {
        throw std::runtime_error("mqtt connack missing");
    }
    if (config.protocolVersion == "mqtt5") {
        if (packet.size() < 4 || packet[3] != 0x00) {
            throw std::runtime_error("mqtt5 connect rejected");
        }
    } else {
        if (packet.size() < 4 || packet[3] != 0x00) {
            throw std::runtime_error("mqtt3 connect rejected");
        }
    }
}

void validatePubAck(const std::vector<std::uint8_t>& packet) {
    if (packet.empty() || (packet[0] & 0xF0) != kPacketPubAck) {
        throw std::runtime_error("mqtt puback missing");
    }
}

void validateSubAck(const std::vector<std::uint8_t>& packet) {
    if (packet.empty() || (packet[0] & 0xF0) != kPacketSubAck) {
        throw std::runtime_error("mqtt suback missing");
    }
}

SocketHandle handleFromInt(std::intptr_t value) {
    return static_cast<SocketHandle>(value);
}

std::intptr_t handleToInt(SocketHandle sock) {
    return static_cast<std::intptr_t>(sock);
}

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::string joinPath(const std::string& left, const std::string& right) {
    if (left.empty()) {
        return right;
    }
    const char tail = left[left.size() - 1];
    if (tail == '/' || tail == '\\') {
        return left + right;
    }
#ifdef _WIN32
    return left + "\\" + right;
#else
    return left + "/" + right;
#endif
}

bool directoryExists(const std::string& path) {
#ifdef _WIN32
    return _access(path.c_str(), 0) == 0;
#else
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

void makeDirectory(const std::string& path) {
    if (path.empty() || directoryExists(path)) {
        return;
    }
    const auto pos = path.find_last_of("/\\");
    if (pos != std::string::npos) {
        makeDirectory(path.substr(0, pos));
    }
#ifdef _WIN32
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0755);
#endif
}

std::uint64_t fileSize(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary | std::ios::ate);
    if (!input.is_open()) {
        return 0;
    }
    return static_cast<std::uint64_t>(input.tellg());
}

void replaceFile(const std::string& from, const std::string& to) {
#ifdef _WIN32
    remove(to.c_str());
#endif
    rename(from.c_str(), to.c_str());
}

char hexDigit(unsigned char value) {
    return static_cast<char>(value < 10 ? ('0' + value) : ('A' + value - 10));
}

std::string hexEncode(const std::string& value) {
    std::string out;
    out.reserve(value.size() * 2);
    for (const unsigned char ch : value) {
        out.push_back(hexDigit((ch >> 4) & 0x0F));
        out.push_back(hexDigit(ch & 0x0F));
    }
    return out;
}

int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + ch - 'a';
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + ch - 'A';
    }
    return -1;
}

bool hexDecode(const std::string& value, std::string* out) {
    if (value.size() % 2 != 0) {
        return false;
    }
    std::string decoded;
    decoded.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
        const int high = hexValue(value[i]);
        const int low = hexValue(value[i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    *out = decoded;
    return true;
}

bool parsePublishPacket(
    const MqttConfig& config,
    const std::vector<std::uint8_t>& packet,
    MqttIncomingMessage* message,
    std::uint16_t* packetId
) {
    if (packet.empty() || (packet[0] & 0xF0) != 0x30) {
        return false;
    }

    std::size_t headerSize = 0;
    decodeRemainingLength(packet, &headerSize);
    std::size_t cursor = headerSize;
    if (cursor + 2 > packet.size()) {
        throw std::runtime_error("mqtt publish topic length missing");
    }
    const auto topicLength = (static_cast<std::size_t>(packet[cursor]) << 8) | packet[cursor + 1];
    cursor += 2;
    if (cursor + topicLength > packet.size()) {
        throw std::runtime_error("mqtt publish topic malformed");
    }
    const std::string topic(
        reinterpret_cast<const char*>(&packet[cursor]),
        reinterpret_cast<const char*>(&packet[cursor + topicLength])
    );
    cursor += topicLength;

    const int qos = (packet[0] >> 1) & 0x03;
    *packetId = 0;
    if (qos > 0) {
        if (cursor + 2 > packet.size()) {
            throw std::runtime_error("mqtt publish packetId missing");
        }
        *packetId = static_cast<std::uint16_t>((packet[cursor] << 8) | packet[cursor + 1]);
        cursor += 2;
    }

    if (config.protocolVersion == "mqtt5") {
        std::size_t propertyMultiplier = 1;
        std::size_t propertyLength = 0;
        std::uint8_t encoded = 0;
        do {
            if (cursor >= packet.size()) {
                throw std::runtime_error("mqtt publish properties malformed");
            }
            encoded = packet[cursor++];
            propertyLength += (encoded & 0x7F) * propertyMultiplier;
            propertyMultiplier *= 128;
        } while ((encoded & 0x80) != 0);
        cursor += propertyLength;
        if (cursor > packet.size()) {
            throw std::runtime_error("mqtt publish properties overflow");
        }
    }

    std::string payload;
    if (cursor < packet.size()) {
        payload.assign(
            reinterpret_cast<const char*>(packet.data() + cursor),
            reinterpret_cast<const char*>(packet.data() + packet.size())
        );
    }

    if (topic == scopedTopic(config.commandRequestTopic, config.topicMachineCode)) {
        message->type = MqttIncomingType::CommandRequest;
    } else if (topic == scopedTopic(config.otaRequestTopic, config.topicMachineCode)) {
        message->type = MqttIncomingType::OtaRequest;
    } else if (topic == scopedTopic(config.systemMonitorRequestTopic, config.topicMachineCode)) {
        message->type = MqttIncomingType::SystemMonitorRequest;
    } else if (topic == scopedTopic(config.diagRequestTopic, config.topicMachineCode)) {
        message->type = MqttIncomingType::DiagRequest;
    } else if (topic == scopedTopic(config.configPullRequestTopic, config.topicMachineCode)) {
        message->type = MqttIncomingType::ConfigPullRequest;
    } else {
        return false;
    }
    message->topic = topic;
    message->payload = payload;
    return true;
}

}  // namespace

BuiltinMqttDriverPublisher::BuiltinMqttDriverPublisher(MqttConfig config)
    : config_(std::move(config)) {
    if (config_.offlineBufferEnabled) {
        try {
            realtimeRing_.reset(new MqttRealtimeRingBuffer(
                config_.offlineRealtimeFile,
                config_.offlineRealtimeFileSizeBytes,
                config_.offlineMaxRealtimeMessageBytes,
                config_.offlineBufferReplayBatchSize
            ));
        } catch (...) {
            realtimeRing_.reset();
        }
        try {
            eventOutbox_.reset(new MqttEventOutbox(
                config_.eventOutboxSqlitePath,
                config_.eventOutboxSqliteLibraryPath,
                config_.eventOutboxRetentionMonths,
                config_.eventOutboxCleanupIntervalHours,
                config_.eventOutboxReplayBatchSize
            ));
        } catch (...) {
            eventOutbox_.reset();
        }
    }
}

BuiltinMqttDriverPublisher::~BuiltinMqttDriverPublisher() {
    flushOfflineBuffer(true);
    closeSubscriber();
}

void BuiltinMqttDriverPublisher::publishFullSnapshot(
    const std::string& topic,
    const std::vector<StoredPointValue>& values
) {
    for (const auto& payload : encodeRealtimeChunks("snapshot", values, config_.maxPayloadBytes)) {
        publishRealtimeJson(topic, payload);
    }
}

void BuiltinMqttDriverPublisher::publishAlarm(
    const std::string& topic,
    std::uint32_t index,
    const StoredPointValue& value,
    const std::string& alarmType,
    bool active
) {
    publishEventJson("alarm", topic, encodeAlarmJson(index, value, alarmType, active), value.ts);
}

void BuiltinMqttDriverPublisher::publishOnDemand(
    const std::string& topic,
    const std::vector<StoredPointValue>& values
) {
    for (const auto& payload : encodeRealtimeChunks("telemetry", values, config_.maxPayloadBytes)) {
        publishRealtimeJson(topic, payload);
    }
}

void BuiltinMqttDriverPublisher::publishChangeEvent(
    const std::string& topic,
    const StoredPointValue& value
) {
    publishEventJson("change", topic, encodeChangeEventJson(value), value.ts);
}

void BuiltinMqttDriverPublisher::publishCommandReply(
    const std::string& topic,
    const MqttCommandReply& reply
) {
    publishJson(topic, encodeCommandReplyJson(reply));
}

void BuiltinMqttDriverPublisher::publishOtaReply(
    const std::string& topic,
    const OtaReply& reply
) {
    publishJson(topic, encodeOtaReplyJson(reply));
}

void BuiltinMqttDriverPublisher::publishOtaStatus(
    const std::string& topic,
    const OtaStatus& status
) {
    publishEventJson("ota_status", topic, encodeOtaStatusJson(status), status.ts);
}

void BuiltinMqttDriverPublisher::publishJsonMessage(
    const std::string& topic,
    const std::string& payload
) {
    publishJson(topic, payload);
}

std::vector<MqttIncomingMessage> BuiltinMqttDriverPublisher::pollIncoming(int timeoutMs) {
    std::vector<MqttIncomingMessage> messages;
    if (config_.commandRequestTopic.empty() &&
        config_.otaRequestTopic.empty() &&
        config_.systemMonitorRequestTopic.empty() &&
        config_.diagRequestTopic.empty() &&
        config_.configPullRequestTopic.empty()) {
        return messages;
    }

    try {
        ensureSubscriberConnected();
        const auto sock = handleFromInt(subscriberSocket_);

        if (!waitReadable(sock, timeoutMs)) {
            const auto now = currentTimeMs();
            if (config_.keepAliveSec > 0 &&
                now - lastSubscriberActivityMs_ >= static_cast<std::int64_t>(config_.keepAliveSec) * 500) {
                sendAll(sock, buildPingReqPacket());
                lastSubscriberActivityMs_ = now;
            }
            return messages;
        }

        while (true) {
            const auto packet = readPacket(sock);
            lastSubscriberActivityMs_ = currentTimeMs();
            const auto type = packet[0] & 0xF0;
            if (type == kPacketPingResp) {
                if (!waitReadable(sock, 0)) {
                    break;
                }
                continue;
            }

            MqttIncomingMessage message;
            std::uint16_t packetId = 0;
            if (parsePublishPacket(config_, packet, &message, &packetId)) {
                messages.push_back(message);
                if (packetId != 0) {
                    sendAll(sock, buildPubAckPacket(packetId, config_.protocolVersion == "mqtt5"));
                }
            }

            if (!waitReadable(sock, 0)) {
                break;
            }
        }
    } catch (...) {
        closeSubscriber();
        throw;
    }

    return messages;
}

void BuiltinMqttDriverPublisher::publishJson(const std::string& topic, const std::string& payload) {
    const std::string scoped = scopedTopic(topic, config_.topicMachineCode);
    if (scoped.empty()) {
        return;
    }
    if (eventOutbox_) {
        try {
            eventOutbox_->replay([this](const std::string& replayTopic, const std::string& replayPayload) {
                sendJsonNow(replayTopic, replayPayload);
            });
            eventOutbox_->cleanupIfDue(currentTimeMs());
        } catch (...) {
        }
    }
    if (config_.offlineBufferEnabled) {
        replayOfflineBuffer();
    }
    try {
        sendJsonNow(scoped, payload);
    } catch (...) {
        enqueueOffline(scoped, payload);
    }
}

void BuiltinMqttDriverPublisher::publishRealtimeJson(const std::string& topic, const std::string& payload) {
    const std::string scoped = scopedTopic(topic, config_.topicMachineCode);
    if (scoped.empty()) {
        return;
    }
    if (eventOutbox_) {
        try {
            eventOutbox_->replay([this](const std::string& replayTopic, const std::string& replayPayload) {
                sendJsonNow(replayTopic, replayPayload);
            });
            eventOutbox_->cleanupIfDue(currentTimeMs());
        } catch (...) {
        }
    }
    if (realtimeRing_) {
        try {
            realtimeRing_->replay([this](const std::string& replayTopic, const std::string& replayPayload) {
                sendJsonNow(replayTopic, replayPayload);
            });
        } catch (...) {
        }
    }
    try {
        sendJsonNow(scoped, payload);
    } catch (...) {
        if (realtimeRing_) {
            realtimeRing_->enqueue(scoped, payload);
        } else {
            enqueueOffline(scoped, payload);
        }
    }
}

void BuiltinMqttDriverPublisher::publishEventJson(
    const std::string& eventType,
    const std::string& topic,
    const std::string& payload,
    std::int64_t eventTs
) {
    const std::string scoped = scopedTopic(topic, config_.topicMachineCode);
    if (scoped.empty()) {
        return;
    }
    if (eventOutbox_) {
        try {
            eventOutbox_->replay([this](const std::string& replayTopic, const std::string& replayPayload) {
                sendJsonNow(replayTopic, replayPayload);
            });
            eventOutbox_->cleanupIfDue(currentTimeMs());
        } catch (...) {
        }
        std::int64_t id = 0;
        try {
            id = eventOutbox_->enqueue(eventType, scoped, payload, eventTs);
        } catch (...) {
            id = 0;
        }
        try {
            sendJsonNow(scoped, payload);
            if (id > 0) {
                eventOutbox_->markSent(id, currentTimeMs());
            }
        } catch (...) {
        }
        return;
    }
    publishJson(scoped, payload);
}

void BuiltinMqttDriverPublisher::sendJsonNow(const std::string& topic, const std::string& payload) {
    const auto endpoint = parseBroker(config_.broker);
    const auto sock = connectTcp(endpoint);
    SocketGuard guard(sock);

    sendAll(sock, buildConnectPacket(config_, "-tx"));
    validateConnAck(config_, readPacket(sock));

    sendAll(sock, buildPublishPacket(config_, topic, payload, nextPacketId_++));
    if (config_.qos > 0) {
        validatePubAck(readPacket(sock));
    }

    sendAll(sock, buildDisconnectPacket());
}

void BuiltinMqttDriverPublisher::enqueueOffline(const std::string& topic, const std::string& payload) {
    if (!config_.offlineBufferEnabled) {
        return;
    }
    offlineMessages_.push_back(OfflineMessage{topic, payload});
    while (offlineMessages_.size() > config_.offlineBufferMaxMemoryMessages &&
           offlineMessages_.size() > config_.offlineBufferFlushBatchSize) {
        flushOfflineBuffer(true);
        if (offlineMessages_.size() <= config_.offlineBufferMaxMemoryMessages) {
            break;
        }
        offlineMessages_.erase(offlineMessages_.begin());
    }
    flushOfflineBuffer(false);
}

void BuiltinMqttDriverPublisher::flushOfflineBuffer(bool force) {
    if (!config_.offlineBufferEnabled || offlineMessages_.empty()) {
        return;
    }
    const auto now = currentTimeMs();
    const auto flushBatch = std::max<std::size_t>(1, config_.offlineBufferFlushBatchSize);
    if (!force &&
        offlineMessages_.size() < flushBatch &&
        now - lastOfflineFlushMs_ < config_.offlineBufferFlushIntervalMs) {
        return;
    }

    makeDirectory(config_.offlineBufferDir);
    const auto path = joinPath(config_.offlineBufferDir, "mqtt_offline_queue.log");
    std::ofstream output(path.c_str(), std::ios::app);
    if (!output.is_open()) {
        return;
    }

    std::size_t count = force ? offlineMessages_.size() : std::min(flushBatch, offlineMessages_.size());
    for (std::size_t i = 0; i < count; ++i) {
        output << hexEncode(offlineMessages_[i].topic)
               << "\t"
               << hexEncode(offlineMessages_[i].payload)
               << "\n";
    }
    output.close();
    offlineMessages_.erase(offlineMessages_.begin(), offlineMessages_.begin() + static_cast<std::ptrdiff_t>(count));
    lastOfflineFlushMs_ = now;

    if (config_.offlineBufferMaxDiskBytes > 0 && fileSize(path) > config_.offlineBufferMaxDiskBytes) {
        std::ifstream input(path.c_str());
        std::vector<std::string> lines;
        std::string line;
        std::uint64_t keptBytes = 0;
        while (std::getline(input, line)) {
            lines.push_back(line);
        }
        std::vector<std::string> kept;
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            keptBytes += static_cast<std::uint64_t>(it->size() + 1);
            if (keptBytes > config_.offlineBufferMaxDiskBytes) {
                break;
            }
            kept.push_back(*it);
        }
        std::ofstream truncated(path.c_str(), std::ios::trunc);
        for (auto it = kept.rbegin(); it != kept.rend(); ++it) {
            truncated << *it << "\n";
        }
    }
}

void BuiltinMqttDriverPublisher::replayOfflineBuffer() {
    if (replayingOffline_ || !config_.offlineBufferEnabled) {
        return;
    }
    replayingOffline_ = true;
    struct ReplayGuard {
        explicit ReplayGuard(bool& value) : value_(value) {}
        ~ReplayGuard() { value_ = false; }
        bool& value_;
    } guard(replayingOffline_);

    const auto path = joinPath(config_.offlineBufferDir, "mqtt_offline_queue.log");
    const auto replayBatch = std::max<std::size_t>(1, config_.offlineBufferReplayBatchSize);
    std::ifstream input(path.c_str());
    if (input.is_open()) {
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(input, line)) {
            lines.push_back(line);
        }
        input.close();

        std::size_t cursor = 0;
        for (; cursor < lines.size() && cursor < replayBatch; ++cursor) {
            const auto tab = lines[cursor].find('\t');
            if (tab == std::string::npos) {
                continue;
            }
            std::string topic;
            std::string payload;
            if (!hexDecode(lines[cursor].substr(0, tab), &topic) ||
                !hexDecode(lines[cursor].substr(tab + 1), &payload)) {
                continue;
            }
            try {
                sendJsonNow(topic, payload);
            } catch (...) {
                break;
            }
        }
        if (cursor > 0) {
            const auto tempPath = path + ".tmp";
            std::ofstream output(tempPath.c_str(), std::ios::trunc);
            for (std::size_t i = cursor; i < lines.size(); ++i) {
                output << lines[i] << "\n";
            }
            output.close();
            replaceFile(tempPath, path);
        }
        if (cursor < lines.size()) {
            return;
        }
    }

    std::size_t sentMemory = 0;
    for (; sentMemory < offlineMessages_.size() && sentMemory < replayBatch; ++sentMemory) {
        try {
            sendJsonNow(offlineMessages_[sentMemory].topic, offlineMessages_[sentMemory].payload);
        } catch (...) {
            break;
        }
    }
    if (sentMemory > 0) {
        offlineMessages_.erase(
            offlineMessages_.begin(),
            offlineMessages_.begin() + static_cast<std::ptrdiff_t>(sentMemory)
        );
    }
}

void BuiltinMqttDriverPublisher::ensureSubscriberConnected() {
    if (subscriberConnected_) {
        return;
    }

    const auto endpoint = parseBroker(config_.broker);
    const auto sock = connectTcp(endpoint);
    try {
        sendAll(sock, buildConnectPacket(config_, "-rx"));
        validateConnAck(config_, readPacket(sock));

        sendAll(
            sock,
                buildSubscribePacket(
                    config_,
                    scopedTopic(config_.commandRequestTopic, config_.topicMachineCode),
                    scopedTopic(config_.otaRequestTopic, config_.topicMachineCode),
                    scopedTopic(config_.systemMonitorRequestTopic, config_.topicMachineCode),
                    scopedTopic(config_.diagRequestTopic, config_.topicMachineCode),
                    scopedTopic(config_.configPullRequestTopic, config_.topicMachineCode),
                    nextPacketId_++
                )
            );
        validateSubAck(readPacket(sock));
    } catch (...) {
        closeSocket(sock);
        throw;
    }

    subscriberSocket_ = handleToInt(sock);
    subscriberConnected_ = true;
    lastSubscriberActivityMs_ = currentTimeMs();
}

void BuiltinMqttDriverPublisher::closeSubscriber() {
    if (!subscriberConnected_) {
        return;
    }

    const auto sock = handleFromInt(subscriberSocket_);
    try {
        sendAll(sock, buildDisconnectPacket());
    } catch (...) {
    }
    closeSocket(sock);
    subscriberSocket_ = 0;
    subscriberConnected_ = false;
    lastSubscriberActivityMs_ = 0;
}

}  // namespace edge_gateway
