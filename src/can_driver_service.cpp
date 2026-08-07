#include "edge_gateway/can_driver_service.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#include "edge_gateway/can_signal_codec.hpp"
#include "edge_gateway/virtual_can_datagram.hpp"
#include "edge_gateway/writeback_service.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace edge_gateway {

namespace {

constexpr int kStoreHeartbeatIntervalMs = 5000;
constexpr int kStoreExpirySweepIntervalMs = 5000;

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

void sleepInterruptibly(const std::atomic<bool>& running, int intervalMs) {
    int remaining = std::max(0, intervalMs);
    while (running.load() && remaining > 0) {
        const auto slice = std::min(100, remaining);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        remaining -= slice;
    }
}

bool isSafeInterfaceName(const std::string& value) {
    if (value.empty() || value.size() > 32) {
        return false;
    }
    for (const auto ch : value) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '-') {
            return false;
        }
    }
    return true;
}

bool isValidCanBitrate(int value) {
    return value >= 10000 && value <= 8000000;
}

bool isValidCanRestartMs(int value) {
    return value >= 0 && value <= 60000;
}

bool isValidCanSamplePoint(double value) {
    return value <= 0.0 || (value >= 0.5 && value <= 0.999);
}

bool usesUdpTestTransport(const CanProtocolConfig& config) {
    return config.transportMode == "udp_test";
}

void validateUdpTestTransport(const CanProtocolConfig& config) {
    if (config.udpListenPort <= 0 || config.udpListenPort > 65535) {
        throw std::invalid_argument("CAN udpListenPort must be between 1 and 65535");
    }
    if (config.udpPeerPort <= 0 || config.udpPeerPort > 65535) {
        throw std::invalid_argument("CAN udpPeerPort must be between 1 and 65535");
    }
#ifndef _WIN32
    in_addr address{};
    if (inet_pton(AF_INET, config.udpBindAddress.c_str(), &address) != 1) {
        throw std::invalid_argument("invalid CAN udpBindAddress: " + config.udpBindAddress);
    }
    if (inet_pton(AF_INET, config.udpPeerAddress.c_str(), &address) != 1) {
        throw std::invalid_argument("invalid CAN udpPeerAddress: " + config.udpPeerAddress);
    }
#endif
}

std::string quoteShellArg(const std::string& value) {
    std::string out = "'";
    for (const auto ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out.push_back(ch);
        }
    }
    out += "'";
    return out;
}

std::vector<DeviceConfig> expandRuntimeConfigs(const DeviceConfig& config) {
    if (config.meters.empty()) {
        return {config};
    }

    std::vector<DeviceConfig> expanded;
    expanded.reserve(config.meters.size());
    for (const auto& logicalDevice : config.meters) {
        if (!logicalDevice.enabled) {
            continue;
        }
        DeviceConfig item = config;
        item.meterCode = logicalDevice.meterCode;
        item.deviceName = logicalDevice.deviceName;
        item.address = logicalDevice.address;
        item.protocol.slave = logicalDevice.slave;
        item.points = logicalDevice.points;
        item.meters = {logicalDevice};
        expanded.push_back(item);
    }
    return expanded;
}

bool isOnlinePoint(const PointDefinition& point) {
    return point.pointCode == "device_online" || point.read.dataType == "device_online";
}

bool hasAllowedValue(const WriteSpec& spec, double value) {
    if (spec.allowedValues.empty()) {
        return true;
    }
    return std::find_if(
        spec.allowedValues.begin(),
        spec.allowedValues.end(),
        [value](double candidate) {
            return std::abs(candidate - value) <= 1e-9;
        }
    ) != spec.allowedValues.end();
}

void validateWriteValue(const PointDefinition& point, double value) {
    if (!point.write.enable) {
        throw std::invalid_argument("point write is disabled");
    }
    if (point.write.minValue && value < *point.write.minValue) {
        throw std::invalid_argument("value below min");
    }
    if (point.write.maxValue && value > *point.write.maxValue) {
        throw std::invalid_argument("value above max");
    }
    if (!hasAllowedValue(point.write, value)) {
        throw std::invalid_argument("value is not in allowedValues");
    }
}

}  // namespace

CanDriverService::CanDriverService(
    DeviceConfig config,
    MemoryPointStore& store,
    std::shared_ptr<IMqttPublisher> mqttPublisher
) : config_(std::move(config)),
    store_(store),
    sqliteWriter_(config_.memoryStore.sqlitePath, config_.memoryStore.sqliteLibraryPath),
    priorityControlLease_(
        config_.mqttDriver.priorityControlLeaseFile,
        config_.protocol.type + ":" + config_.memoryStore.sharedMemoryName
    ),
    mqttPublisher_(std::move(mqttPublisher)) {
    if (config_.protocol.type != "can_socketcan" && config_.protocol.type != "can") {
        throw std::invalid_argument("CanDriver requires protocol.type=can_socketcan");
    }
    initializeRuntimeDevices();
}

CanDriverService::~CanDriverService() {
    stop();
}

void CanDriverService::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }

    configureInterface();
    openSocket();
    publishStatusEvent(
        "started",
        nowMs(),
        std::string(R"("interfaceName":")") + escapeJson(config_.protocol.can.interfaceName) +
            R"(","sharedMemory":")" + escapeJson(config_.memoryStore.sharedMemoryName) + R"(")"
    );
    runStartupWrites();
    receiveThread_ = std::thread(&CanDriverService::receiveLoop, this);
    writebackThread_ = std::thread(&CanDriverService::writebackLoop, this);
    persistThread_ = std::thread(&CanDriverService::persistLoop, this);
}

void CanDriverService::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }
    if (writebackThread_.joinable()) {
        writebackThread_.join();
    }
    if (persistThread_.joinable()) {
        persistThread_.join();
    }
    closeSocket();
    publishStatusEvent("stopped", nowMs());
}

bool CanDriverService::isRunning() const {
    return running_.load();
}

void CanDriverService::initializeRuntimeDevices() {
    auto expandedConfigs = expandRuntimeConfigs(config_);
    if (expandedConfigs.empty()) {
        throw std::invalid_argument("no enabled CAN logical devices configured");
    }

    for (auto& expanded : expandedConfigs) {
        RuntimeDevice device;
        device.config = std::move(expanded);
        if (!device.config.meters.empty()) {
            device.onlineTimeoutMs = device.config.meters.front().onlineTimeoutMs;
            device.onlineFrameIds = device.config.meters.front().onlineFrameIds;
            device.config.meters.clear();
        }
        const auto deviceIndex = runtimeDevices_.size();
        store_.registerPoints(device.config.machineCode, device.config.meterCode, device.config.points);
        for (const auto& point : device.config.points) {
            if (!point.enabled) {
                continue;
            }
            const auto inserted = indexToRuntimePoint_.emplace(point.index, runtimePoints_.size());
            if (!inserted.second) {
                throw std::invalid_argument("duplicate CAN point.index: " + std::to_string(point.index));
            }
            runtimePoints_.push_back(RuntimePoint{deviceIndex, point, 0, false});
        }
        runtimeDevices_.push_back(std::move(device));
        publishOnlinePoint(runtimeDevices_.back(), false, nowMs());
    }
}

void CanDriverService::configureInterface() const {
#ifdef _WIN32
    throw std::runtime_error("SocketCAN is not supported on Windows");
#else
    const auto& can = config_.protocol.can;
    if (usesUdpTestTransport(can)) {
        validateUdpTestTransport(can);
        return;
    }
    if (can.transportMode != "socketcan") {
        throw std::invalid_argument("unsupported CAN transportMode: " + can.transportMode);
    }
    if (!can.manageInterface) {
        return;
    }
    if (!isSafeInterfaceName(can.interfaceName)) {
        throw std::invalid_argument("invalid CAN interfaceName: " + can.interfaceName);
    }
    if (!isValidCanBitrate(can.bitrate)) {
        throw std::invalid_argument("invalid CAN bitrate: " + std::to_string(can.bitrate));
    }
    if (!isValidCanSamplePoint(can.samplePoint)) {
        throw std::invalid_argument("invalid CAN samplePoint");
    }
    if (!isValidCanRestartMs(can.restartMs)) {
        throw std::invalid_argument("invalid CAN restartMs: " + std::to_string(can.restartMs));
    }
    if (can.fdEnabled && !isValidCanBitrate(can.dataBitrate)) {
        throw std::invalid_argument("invalid CAN dataBitrate: " + std::to_string(can.dataBitrate));
    }
    std::ostringstream command;
    const auto iface = quoteShellArg(can.interfaceName);
    command << "ip link set " << iface << " down >/dev/null 2>&1; ";
    command << "ip link set " << iface << " type can bitrate " << can.bitrate;
    if (can.samplePoint > 0.0) {
        command << " sample-point " << can.samplePoint;
    }
    if (can.restartMs > 0) {
        command << " restart-ms " << can.restartMs;
    }
    command << " loopback " << (can.loopback ? "on" : "off");
    command << " listen-only " << (can.listenOnly ? "on" : "off");
    if (can.fdEnabled) {
        command << " fd on dbitrate " << can.dataBitrate;
    }
    command << " && ip link set " << iface << " up";
    const auto rc = std::system(command.str().c_str());
    if (rc != 0) {
        throw std::runtime_error("failed to configure CAN interface: " + can.interfaceName);
    }
#endif
}

void CanDriverService::openSocket() {
#ifdef _WIN32
    throw std::runtime_error("SocketCAN is not supported on Windows");
#else
    std::lock_guard<std::mutex> lock(socketMutex_);
    openSocketLocked();
#endif
}

void CanDriverService::openSocketLocked() {
#ifdef _WIN32
    throw std::runtime_error("SocketCAN is not supported on Windows");
#else
    if (socketFd_ >= 0) {
        return;
    }
    const auto& can = config_.protocol.can;
    if (usesUdpTestTransport(can)) {
        validateUdpTestTransport(can);
        socketFd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (socketFd_ < 0) {
            throw std::runtime_error("failed to open virtual CAN UDP socket");
        }
        int reuse = 1;
        setsockopt(socketFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        const auto receiveBuffer = static_cast<int>(std::min<std::size_t>(
            can.rxQueueSize * (kVirtualCanHeaderSize + kVirtualCanMaxPayloadSize),
            static_cast<std::size_t>(std::numeric_limits<int>::max())
        ));
        const auto sendBuffer = static_cast<int>(std::min<std::size_t>(
            can.txQueueSize * (kVirtualCanHeaderSize + kVirtualCanMaxPayloadSize),
            static_cast<std::size_t>(std::numeric_limits<int>::max())
        ));
        setsockopt(socketFd_, SOL_SOCKET, SO_RCVBUF, &receiveBuffer, sizeof(receiveBuffer));
        setsockopt(socketFd_, SOL_SOCKET, SO_SNDBUF, &sendBuffer, sizeof(sendBuffer));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(can.udpListenPort));
        if (inet_pton(AF_INET, can.udpBindAddress.c_str(), &address.sin_addr) != 1 ||
            bind(socketFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            closeSocketLocked();
            throw std::runtime_error(
                "failed to bind virtual CAN UDP socket: " + can.udpBindAddress + ":" +
                std::to_string(can.udpListenPort)
            );
        }
        return;
    }
    socketFd_ = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (socketFd_ < 0) {
        throw std::runtime_error("failed to open CAN raw socket");
    }
    if (can.fdEnabled) {
#ifdef CAN_RAW_FD_FRAMES
        int enable = 1;
        setsockopt(socketFd_, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enable, sizeof(enable));
#endif
    }

    ifreq ifr{};
    std::snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", can.interfaceName.c_str());
    if (ioctl(socketFd_, SIOCGIFINDEX, &ifr) < 0) {
        closeSocketLocked();
        throw std::runtime_error("failed to resolve CAN interface index: " + can.interfaceName);
    }

    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(socketFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        closeSocketLocked();
        throw std::runtime_error("failed to bind CAN socket: " + can.interfaceName);
    }
#endif
}

void CanDriverService::closeSocket() {
#ifndef _WIN32
    std::lock_guard<std::mutex> lock(socketMutex_);
    closeSocketLocked();
#endif
}

void CanDriverService::closeSocketLocked() {
#ifndef _WIN32
    if (socketFd_ >= 0) {
        ::close(socketFd_);
        socketFd_ = -1;
    }
#endif
}

int CanDriverService::duplicateSocket() const {
#ifdef _WIN32
    return -1;
#else
    std::lock_guard<std::mutex> lock(socketMutex_);
    return socketFd_ >= 0 ? ::dup(socketFd_) : -1;
#endif
}

bool CanDriverService::isInterfaceReady() const {
#ifdef _WIN32
    return false;
#else
    if (usesUdpTestTransport(config_.protocol.can)) {
        return socketFd_ >= 0;
    }
    const auto fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return false;
    }
    ifreq ifr{};
    std::snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", config_.protocol.can.interfaceName.c_str());
    const auto result = ioctl(fd, SIOCGIFFLAGS, &ifr);
    ::close(fd);
    return result == 0 && (ifr.ifr_flags & IFF_UP) != 0;
#endif
}

void CanDriverService::checkInterfaceOnce(std::int64_t nowMsValue) {
    const auto intervalMs = std::max(1, config_.collect.interfaceCheckIntervalMs);
    if (lastInterfaceCheckMs_ > 0 && nowMsValue - lastInterfaceCheckMs_ < intervalMs) {
        return;
    }
    lastInterfaceCheckMs_ = nowMsValue;
    std::lock_guard<std::mutex> lock(socketMutex_);
    if (socketFd_ >= 0 && isInterfaceReady()) {
        return;
    }

    closeSocketLocked();
    configureInterface();
    openSocketLocked();
    publishStatusEvent(
        "interface-recovered",
        nowMsValue,
        std::string("\"interfaceName\":\"") + escapeJson(config_.protocol.can.interfaceName) + "\""
    );
}

std::size_t CanDriverService::processReceiveOnce(int timeoutMs) {
#ifdef _WIN32
    (void)timeoutMs;
    throw std::runtime_error("SocketCAN is not supported on Windows");
#else
    openSocket();
    const auto receiveFd = duplicateSocket();
    if (receiveFd < 0) {
        throw std::runtime_error("failed to duplicate CAN socket");
    }
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(receiveFd, &readSet);
    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    const auto ready = select(receiveFd + 1, &readSet, nullptr, nullptr, &timeout);
    if (ready <= 0) {
        ::close(receiveFd);
        return 0;
    }

    std::uint32_t frameId = 0;
    bool extended = false;
    bool remote = false;
    std::vector<std::uint8_t> payload;
    if (usesUdpTestTransport(config_.protocol.can)) {
        std::uint8_t bytes[kVirtualCanHeaderSize + kVirtualCanMaxPayloadSize]{};
        const auto nbytes = recv(receiveFd, bytes, sizeof(bytes), 0);
        ::close(receiveFd);
        if (nbytes < 0) {
            throw std::runtime_error("failed to read virtual CAN UDP datagram");
        }
        VirtualCanFrame frame;
        if (!decodeVirtualCanDatagram(bytes, static_cast<std::size_t>(nbytes), frame)) {
            return 0;
        }
        frameId = frame.frameId;
        extended = frame.extended;
        remote = frame.remoteRequest;
        payload = std::move(frame.payload);
    } else {
        canfd_frame frame{};
        const auto nbytes = read(receiveFd, &frame, sizeof(frame));
        ::close(receiveFd);
        if (nbytes < 0) {
            throw std::runtime_error("failed to read CAN frame");
        }
        if (nbytes != CAN_MTU && nbytes != CANFD_MTU) {
            return 0;
        }
        frameId = frame.can_id & CAN_EFF_MASK;
        extended = (frame.can_id & CAN_EFF_FLAG) != 0;
        remote = (frame.can_id & CAN_RTR_FLAG) != 0;
        payload.assign(frame.data, frame.data + frame.len);
    }
    if (remote) {
        return 0;
    }
    const auto ts = nowMs();
    std::size_t matched = 0;

    for (auto& device : runtimeDevices_) {
        if (frameBelongsToDevice(device, frameId, extended)) {
            device.lastSeenTs = ts;
            if (!device.online) {
                publishOnlinePoint(device, true, ts);
            }
        }
    }

    for (auto& runtimePoint : runtimePoints_) {
        const auto& point = runtimePoint.point;
        if (!point.read.enable || isOnlinePoint(point)) {
            continue;
        }
        if (!CanSignalCodec::frameMatches(frameId, extended, point.read.can)) {
            continue;
        }
        const auto decoded = CanSignalCodec::decode(payload, point.read);
        runtimePoint.lastSeenTs = ts;
        runtimePoint.stalePublished = false;
        publishPointValue(runtimeDevices_[runtimePoint.deviceIndex], point, decoded, ts);
        ++matched;
    }
    return matched;
#endif
}

std::size_t CanDriverService::processWritebackOnce(std::int64_t nowMsValue) {
    const auto commands = drainScheduledWriteCommands(
        store_,
        &priorityControlLease_,
        nowMsValue,
        config_.memoryStore.writebackBatchSize
    );
    std::size_t processed = 0;
    for (const auto& command : commands) {
        const auto startedAt = nowMs();
        auto writebackResult = beginWritebackResult(command, startedAt);
        const auto pointIt = indexToRuntimePoint_.find(command.index);
        if (pointIt == indexToRuntimePoint_.end()) {
            const auto completedAt = nowMs();
            completeWritebackResult(
                writebackResult,
                false,
                "unknown-index",
                "writeback-skipped",
                completedAt
            );
            store_.recordWritebackResult(writebackResult);
            priorityControlLease_.release(command.cmdId);
            continue;
        }
        const auto& runtimePoint = runtimePoints_[pointIt->second];
        const auto& point = runtimePoint.point;
        try {
            validateWriteValue(point, command.value);
            sendCanWrite(point, command.value);
            publishStatusEvent(
                "writeback-succeeded",
                nowMsValue,
                std::string(R"("meterCode":")") + escapeJson(runtimeDevices_[runtimePoint.deviceIndex].config.meterCode) +
                    R"(","index":)" + std::to_string(command.index) +
                    R"(,"cmdId":")" + escapeJson(command.cmdId) + R"(")"
            );
            const auto completedAt = nowMs();
            completeWritebackResult(
                writebackResult,
                true,
                "ok",
                "writeback-succeeded",
                completedAt
            );
            store_.recordWritebackResult(writebackResult);
            priorityControlLease_.release(command.cmdId);
            ++processed;
        } catch (const std::exception& ex) {
            const auto completedAt = nowMs();
            completeWritebackResult(
                writebackResult,
                false,
                ex.what(),
                "writeback-failed",
                completedAt
            );
            store_.recordWritebackResult(writebackResult);
            publishStatusEvent(
                "writeback-failed",
                nowMsValue,
                std::string(R"("meterCode":")") + escapeJson(runtimeDevices_[runtimePoint.deviceIndex].config.meterCode) +
                    R"(","index":)" + std::to_string(command.index) +
                    R"(,"message":")" + escapeJson(ex.what()) + R"(")"
            );
            priorityControlLease_.release(command.cmdId);
        }
    }
    return processed;
}

std::size_t CanDriverService::flushPersistentOnce() {
    const auto samples = store_.drainPersistentSamples();
    sqliteWriter_.writeSamples(samples);
    return samples.size();
}

void CanDriverService::runStartupWrites() {
    if (config_.startupWrites.empty()) {
        return;
    }

    for (const auto& item : config_.startupWrites) {
        if (!item.enabled || item.pointCode.empty()) {
            continue;
        }

        const auto pointIt = std::find_if(
            runtimePoints_.begin(),
            runtimePoints_.end(),
            [&](const RuntimePoint& runtimePoint) {
                const auto& device = runtimeDevices_[runtimePoint.deviceIndex];
                return runtimePoint.point.pointCode == item.pointCode &&
                    (item.meterCode.empty() || device.config.meterCode == item.meterCode);
            }
        );
        if (pointIt == runtimePoints_.end()) {
            publishStatusEvent(
                "startup-write-skipped",
                nowMs(),
                std::string(R"("pointCode":")") + escapeJson(item.pointCode) +
                    R"(","message":"point-not-found")"
            );
            continue;
        }

        if (item.delayMs > 0) {
            sleepInterruptibly(running_, item.delayMs);
        }

        for (int attempt = 0; attempt < std::max(1, item.repeat) && running_.load(); ++attempt) {
            try {
                validateWriteValue(pointIt->point, item.value);
                sendCanWrite(pointIt->point, item.value);
                publishStatusEvent(
                    "startup-write-succeeded",
                    nowMs(),
                    std::string(R"("pointCode":")") + escapeJson(item.pointCode) +
                        R"(","value":)" + std::to_string(item.value) +
                        R"(,"reason":")" + escapeJson(item.reason) + R"(")"
                );
            } catch (const std::exception& ex) {
                publishStatusEvent(
                    "startup-write-failed",
                    nowMs(),
                    std::string(R"("pointCode":")") + escapeJson(item.pointCode) +
                        R"(","message":")" + escapeJson(ex.what()) + R"(")"
                );
            }

            if (attempt + 1 < std::max(1, item.repeat) && item.repeatIntervalMs > 0) {
                sleepInterruptibly(running_, item.repeatIntervalMs);
            }
        }
    }
}

bool CanDriverService::sendCanWrite(const PointDefinition& point, double value) {
    const auto encoded = CanSignalCodec::encode(value, point.write);
#ifndef _WIN32
    if (usesUdpTestTransport(config_.protocol.can)) {
        VirtualCanFrame frame;
        frame.frameId = encoded.frameId;
        frame.extended = encoded.extended;
        frame.remoteRequest = encoded.remoteRequest;
        frame.fd = config_.protocol.can.fdEnabled || encoded.payload.size() > CAN_MAX_DLEN;
        frame.payload = encoded.payload;
        const auto bytes = encodeVirtualCanDatagram(frame);
        if (bytes.empty() && !frame.payload.empty()) {
            throw std::invalid_argument("virtual CAN payload must be <= 64 bytes");
        }
        sockaddr_in peer{};
        peer.sin_family = AF_INET;
        peer.sin_port = htons(static_cast<std::uint16_t>(config_.protocol.can.udpPeerPort));
        if (inet_pton(AF_INET, config_.protocol.can.udpPeerAddress.c_str(), &peer.sin_addr) != 1) {
            throw std::invalid_argument("invalid CAN udpPeerAddress: " + config_.protocol.can.udpPeerAddress);
        }
        std::lock_guard<std::mutex> lock(socketMutex_);
        openSocketLocked();
        if (sendto(
                socketFd_,
                bytes.data(),
                bytes.size(),
                0,
                reinterpret_cast<sockaddr*>(&peer),
                sizeof(peer)
            ) != static_cast<ssize_t>(bytes.size())) {
            throw std::runtime_error("failed to send virtual CAN UDP datagram");
        }
        return true;
    }
    if (!config_.protocol.can.fdEnabled && encoded.payload.size() > CAN_MAX_DLEN) {
        throw std::invalid_argument("classic CAN payload must be <= 8 bytes");
    }
    std::lock_guard<std::mutex> lock(socketMutex_);
    openSocketLocked();
    if (config_.protocol.can.fdEnabled || encoded.payload.size() > CAN_MAX_DLEN) {
        canfd_frame frame{};
        frame.can_id = encoded.frameId | (encoded.extended ? CAN_EFF_FLAG : 0U);
        frame.len = static_cast<__u8>(encoded.payload.size());
        std::memcpy(frame.data, encoded.payload.data(), encoded.payload.size());
        if (write(socketFd_, &frame, sizeof(frame)) != CANFD_MTU) {
            throw std::runtime_error("failed to send CAN-FD frame");
        }
    } else {
        can_frame frame{};
        frame.can_id = encoded.frameId | (encoded.extended ? CAN_EFF_FLAG : 0U);
        if (encoded.remoteRequest) {
            frame.can_id |= CAN_RTR_FLAG;
        }
        frame.can_dlc = static_cast<__u8>(encoded.payload.size());
        std::memcpy(frame.data, encoded.payload.data(), encoded.payload.size());
        if (write(socketFd_, &frame, sizeof(frame)) != CAN_MTU) {
            throw std::runtime_error("failed to send CAN frame");
        }
    }
#else
    throw std::runtime_error("SocketCAN is not supported on Windows");
#endif
    return true;
}

void CanDriverService::updateOnlineStatus(std::int64_t nowMsValue) {
    for (auto& device : runtimeDevices_) {
        const auto timeoutMs = std::max(1, device.onlineTimeoutMs);
        const auto online = device.lastSeenTs > 0 && nowMsValue - device.lastSeenTs <= timeoutMs;
        std::int64_t refreshMs = std::min(timeoutMs, 30000);
        for (const auto& point : device.config.points) {
            if (isOnlinePoint(point) && point.read.cachePolicy.ttlMs > 0) {
                const auto ttlRefreshMs = std::max<std::int64_t>(250, point.read.cachePolicy.ttlMs / 2);
                refreshMs = std::min(refreshMs, ttlRefreshMs);
            }
        }
        if (online != device.online || device.lastOnlinePublishedTs <= 0 ||
            nowMsValue - device.lastOnlinePublishedTs >= refreshMs) {
            publishOnlinePoint(device, online, nowMsValue);
        }
    }
    updatePointStaleness(nowMsValue);
}

void CanDriverService::updatePointStaleness(std::int64_t nowMsValue) {
    for (auto& runtimePoint : runtimePoints_) {
        const auto& point = runtimePoint.point;
        if (!point.read.enable || isOnlinePoint(point) || runtimePoint.lastSeenTs <= 0 ||
            point.read.can.receiveTimeoutMs <= 0) {
            continue;
        }
        if (nowMsValue - runtimePoint.lastSeenTs <= point.read.can.receiveTimeoutMs) {
            runtimePoint.stalePublished = false;
            continue;
        }
        if (runtimePoint.stalePublished) {
            continue;
        }
        const auto latest = store_.getLatestByIndex(point.index, nowMsValue);
        if (!latest) {
            continue;
        }

        const auto& device = runtimeDevices_[runtimePoint.deviceIndex].config;
        PointValue stale;
        stale.index = point.index;
        stale.machineCode = device.machineCode;
        stale.meterCode = device.meterCode;
        stale.pointCode = point.pointCode;
        stale.pointName = point.name;
        stale.category = point.category;
        stale.unit = point.read.unit;
        stale.value = latest->value;
        stale.quality = 0;
        stale.qualityMsg = "CAN receive timeout";
        stale.ts = nowMsValue;
        stale.expireAt = nowMsValue + std::max<std::int64_t>(1, point.read.cachePolicy.ttlMs);
        stale.stale = true;
        stale.function = 0;
        stale.address = point.address;
        stale.length = 1;
        stale.isStore = false;
        stale.persistIntervalSec = point.persistIntervalSec;
        store_.putLatest(stale);
        runtimePoint.stalePublished = true;
    }
}

void CanDriverService::receiveLoop() {
    std::int64_t lastStoreHeartbeatMs = 0;
    std::int64_t lastExpirySweepMs = 0;
    while (running_.load()) {
        try {
            const auto ts = nowMs();
            checkInterfaceOnce(ts);
            if (lastStoreHeartbeatMs <= 0 || ts - lastStoreHeartbeatMs >= kStoreHeartbeatIntervalMs) {
                store_.heartbeatRegisteredPoints(ts);
                lastStoreHeartbeatMs = ts;
            }
            if (!priorityControlBlocked(ts)) {
                processReceiveOnce(std::max(1, config_.collect.receiveWaitMs));
                const auto completedTs = nowMs();
                updateOnlineStatus(completedTs);
                if (lastExpirySweepMs <= 0 || completedTs - lastExpirySweepMs >= kStoreExpirySweepIntervalMs) {
                    store_.removeExpired(completedTs);
                    lastExpirySweepMs = completedTs;
                }
            } else {
                sleepInterruptibly(running_, std::max(10, config_.collect.receiveWaitMs));
            }
        } catch (const std::exception& ex) {
            publishStatusEvent("receive-failed", nowMs(), std::string(R"("message":")") + escapeJson(ex.what()) + R"(")");
            sleepInterruptibly(running_, 1000);
        }
    }
}

void CanDriverService::writebackLoop() {
    const auto intervalMs = std::max(20, config_.collect.writebackIntervalMs);
    while (running_.load()) {
        try {
            const auto ts = nowMs();
            processWritebackOnce(ts);
        } catch (...) {
        }
        sleepInterruptibly(running_, intervalMs);
    }
}

void CanDriverService::persistLoop() {
    const auto intervalMs = std::max(1000, config_.memoryStore.persistFlushIntervalMs);
    while (running_.load()) {
        try {
            const auto ts = nowMs();
            if (!priorityControlBlocked(ts)) {
                flushPersistentOnce();
            }
        } catch (...) {
        }
        sleepInterruptibly(running_, intervalMs);
    }
}

bool CanDriverService::priorityControlBlocked(std::int64_t nowMsValue) const {
    return priorityControlLease_.isBlocked(nowMsValue);
}

void CanDriverService::publishStatusEvent(
    const std::string& event,
    std::int64_t ts,
    const std::string& detailsJson
) const {
    if (!mqttPublisher_) {
        return;
    }
    std::ostringstream payload;
    payload << "{\"service\":\"can-daemon\",\"event\":\""
            << event
            << "\",\"ts\":"
            << ts;
    if (!detailsJson.empty()) {
        payload << "," << detailsJson;
    }
    payload << "}";
    mqttPublisher_->publishStatusMessage(config_.machineCode, payload.str());
}

void CanDriverService::publishPointValue(
    const RuntimeDevice& device,
    const PointDefinition& point,
    const DecodedValue& decoded,
    std::int64_t ts
) {
    PointValue latest;
    latest.index = point.index;
    latest.machineCode = device.config.machineCode;
    latest.meterCode = device.config.meterCode;
    latest.pointCode = point.pointCode;
    latest.pointName = point.name;
    latest.category = point.category;
    latest.unit = point.read.unit;
    latest.value = decoded.value;
    latest.text = decoded.text;
    latest.rawHex = decoded.rawHex;
    latest.quality = 1;
    latest.qualityMsg = "ok";
    latest.ts = ts;
    latest.expireAt = ts + point.read.cachePolicy.ttlMs;
    latest.function = 0;
    latest.address = point.address;
    latest.length = 1;
    latest.isStore = point.isStore;
    latest.persistIntervalSec = point.persistIntervalSec;
    store_.putLatest(latest);
}

void CanDriverService::publishOnlinePoint(RuntimeDevice& device, bool online, std::int64_t ts) {
    device.online = online;
    device.lastOnlinePublishedTs = ts;
    for (const auto& point : device.config.points) {
        if (!isOnlinePoint(point)) {
            continue;
        }
        DecodedValue decoded;
        decoded.value = online ? 1.0 : 0.0;
        decoded.text = online ? "1" : "0";
        decoded.rawHex = online ? "01" : "00";
        publishPointValue(device, point, decoded, ts);
    }
}

bool CanDriverService::frameBelongsToDevice(const RuntimeDevice& device, std::uint32_t frameId, bool extended) const {
    for (const auto& frame : device.onlineFrameIds) {
        CanSignalSpec spec;
        spec.frameId = frame;
        spec.extended = extended;
        if (CanSignalCodec::frameMatches(frameId, extended, spec)) {
            return true;
        }
    }
    for (const auto& point : device.config.points) {
        if (point.read.enable && CanSignalCodec::frameMatches(frameId, extended, point.read.can)) {
            return true;
        }
    }
    return false;
}

std::int64_t CanDriverService::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

}  // namespace edge_gateway
