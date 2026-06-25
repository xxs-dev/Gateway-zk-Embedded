#include "edge_gateway/can_driver_service.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#include "edge_gateway/can_signal_codec.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace edge_gateway {

namespace {

std::int64_t nonNegativeDuration(std::int64_t endMs, std::int64_t startMs) {
    if (endMs <= 0 || startMs <= 0 || endMs < startMs) {
        return 0;
    }
    return endMs - startMs;
}

std::int64_t edgeTotalElapsed(const WritebackResultRecord& result) {
    return std::max(result.edgeElapsedMs, result.queueDelayMs + result.deviceWriteMs);
}

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
            runtimePoints_.push_back(RuntimePoint{deviceIndex, point});
        }
        runtimeDevices_.push_back(std::move(device));
    }
}

void CanDriverService::configureInterface() const {
#ifdef _WIN32
    throw std::runtime_error("SocketCAN is not supported on Windows");
#else
    const auto& can = config_.protocol.can;
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
    if (socketFd_ >= 0) {
        return;
    }
    const auto& can = config_.protocol.can;
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
        closeSocket();
        throw std::runtime_error("failed to resolve CAN interface index: " + can.interfaceName);
    }

    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(socketFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        closeSocket();
        throw std::runtime_error("failed to bind CAN socket: " + can.interfaceName);
    }
#endif
}

void CanDriverService::closeSocket() {
#ifndef _WIN32
    if (socketFd_ >= 0) {
        ::close(socketFd_);
        socketFd_ = -1;
    }
#endif
}

std::size_t CanDriverService::processReceiveOnce(int timeoutMs) {
#ifdef _WIN32
    (void)timeoutMs;
    throw std::runtime_error("SocketCAN is not supported on Windows");
#else
    if (socketFd_ < 0) {
        openSocket();
    }
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socketFd_, &readSet);
    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    const auto ready = select(socketFd_ + 1, &readSet, nullptr, nullptr, &timeout);
    if (ready <= 0) {
        return 0;
    }

    canfd_frame frame{};
    const auto nbytes = read(socketFd_, &frame, sizeof(frame));
    if (nbytes < 0) {
        throw std::runtime_error("failed to read CAN frame");
    }
    if (nbytes != CAN_MTU && nbytes != CANFD_MTU) {
        return 0;
    }

    const auto frameId = frame.can_id & CAN_EFF_MASK;
    const bool extended = (frame.can_id & CAN_EFF_FLAG) != 0;
    const bool remote = (frame.can_id & CAN_RTR_FLAG) != 0;
    if (remote) {
        return 0;
    }
    const auto length = nbytes == CANFD_MTU ? frame.len : frame.len;
    const std::vector<std::uint8_t> payload(frame.data, frame.data + length);
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

    for (const auto& runtimePoint : runtimePoints_) {
        const auto& point = runtimePoint.point;
        if (!point.read.enable || isOnlinePoint(point)) {
            continue;
        }
        if (!CanSignalCodec::frameMatches(frameId, extended, point.read.can)) {
            continue;
        }
        const auto decoded = CanSignalCodec::decode(payload, point.read);
        publishPointValue(runtimeDevices_[runtimePoint.deviceIndex], point, decoded, ts);
        ++matched;
    }
    return matched;
#endif
}

std::size_t CanDriverService::processWritebackOnce(std::int64_t nowMsValue) {
    const auto activeLease = priorityControlLease_.activeLease(nowMsValue);
    const auto commands = activeLease
        ? store_.drainPendingWriteCommandsByCmdId(activeLease->cmdId, config_.memoryStore.writebackBatchSize)
        : store_.drainPendingWriteCommands(config_.memoryStore.writebackBatchSize);
    std::size_t processed = 0;
    for (const auto& command : commands) {
        const auto startedAt = nowMs();
        const auto acceptedAt = command.acceptedAt > 0 ? command.acceptedAt : command.ts;
        WritebackResultRecord writebackResult;
        writebackResult.cmdId = command.cmdId;
        writebackResult.index = command.index;
        writebackResult.value = command.value;
        writebackResult.highPriority = command.highPriority;
        writebackResult.requestedAt = command.ts;
        writebackResult.acceptedAt = acceptedAt;
        writebackResult.startedAt = startedAt;
        writebackResult.queueDelayMs = nonNegativeDuration(startedAt, acceptedAt);
        const auto pointIt = indexToRuntimePoint_.find(command.index);
        if (pointIt == indexToRuntimePoint_.end()) {
            const auto completedAt = nowMs();
            writebackResult.completedAt = completedAt;
            writebackResult.success = false;
            writebackResult.message = "unknown-index";
            writebackResult.stage = "writeback-skipped";
            writebackResult.deviceWriteMs = nonNegativeDuration(completedAt, startedAt);
            writebackResult.edgeElapsedMs = nonNegativeDuration(completedAt, acceptedAt);
            writebackResult.totalElapsedMs = edgeTotalElapsed(writebackResult);
            store_.recordWritebackResult(writebackResult);
            priorityControlLease_.release(command.cmdId);
            continue;
        }
        const auto& runtimePoint = runtimePoints_[pointIt->second];
        const auto& point = runtimePoint.point;
        try {
            validateWriteValue(point, command.value);
            const auto encoded = CanSignalCodec::encode(command.value, point.write);
#ifndef _WIN32
            if (socketFd_ < 0) {
                openSocket();
            }
            if (!config_.protocol.can.fdEnabled && encoded.payload.size() > CAN_MAX_DLEN) {
                throw std::invalid_argument("classic CAN payload must be <= 8 bytes");
            }
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
            publishStatusEvent(
                "writeback-succeeded",
                nowMsValue,
                std::string(R"("meterCode":")") + escapeJson(runtimeDevices_[runtimePoint.deviceIndex].config.meterCode) +
                    R"(","index":)" + std::to_string(command.index) +
                    R"(,"cmdId":")" + escapeJson(command.cmdId) + R"(")"
            );
            const auto completedAt = nowMs();
            writebackResult.completedAt = completedAt;
            writebackResult.success = true;
            writebackResult.message = "ok";
            writebackResult.stage = "writeback-succeeded";
            writebackResult.deviceWriteMs = nonNegativeDuration(completedAt, startedAt);
            writebackResult.edgeElapsedMs = nonNegativeDuration(completedAt, acceptedAt);
            writebackResult.totalElapsedMs = edgeTotalElapsed(writebackResult);
            store_.recordWritebackResult(writebackResult);
            priorityControlLease_.release(command.cmdId);
            ++processed;
        } catch (const std::exception& ex) {
            const auto completedAt = nowMs();
            writebackResult.completedAt = completedAt;
            writebackResult.success = false;
            writebackResult.message = ex.what();
            writebackResult.stage = "writeback-failed";
            writebackResult.deviceWriteMs = nonNegativeDuration(completedAt, startedAt);
            writebackResult.edgeElapsedMs = nonNegativeDuration(completedAt, acceptedAt);
            writebackResult.totalElapsedMs = edgeTotalElapsed(writebackResult);
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

void CanDriverService::updateOnlineStatus(std::int64_t nowMsValue) {
    for (auto& device : runtimeDevices_) {
        const auto timeoutMs = std::max(1000, device.onlineTimeoutMs);
        const auto online = device.lastSeenTs > 0 && nowMsValue - device.lastSeenTs <= timeoutMs;
        if (online != device.online) {
            publishOnlinePoint(device, online, nowMsValue);
        }
    }
}

void CanDriverService::receiveLoop() {
    while (running_.load()) {
        try {
            const auto ts = nowMs();
            store_.heartbeatRegisteredPoints(ts);
            if (!priorityControlBlocked(ts)) {
                processReceiveOnce(std::max(50, config_.collect.defaultIntervalMs));
                updateOnlineStatus(ts);
                store_.removeExpired(ts);
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
            store_.heartbeatRegisteredPoints(ts);
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
