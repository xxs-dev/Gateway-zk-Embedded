#include "edge_gateway/can_driver_service.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#include "can_driver_service_internal.hpp"
#include "can_frame_state.hpp"
#include "edge_gateway/can_signal_codec.hpp"
#include "edge_gateway/common/persistent_flush_schedule.hpp"
#include "edge_gateway/virtual_can_datagram.hpp"

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

using namespace can_driver_detail;

namespace {

constexpr int kStoreHeartbeatIntervalMs = 5000;
constexpr int kStoreExpirySweepIntervalMs = 5000;
constexpr std::int64_t kErrorLogIntervalMs = 60000;
// linux/can/error.h: error classes carried in the can_id of an error frame, controller status in data[1].
constexpr std::uint32_t kCanErrCrtl = 0x004U;
constexpr std::uint32_t kCanErrBusOff = 0x040U;
constexpr std::uint32_t kCanErrRestarted = 0x100U;
constexpr std::uint8_t kCanErrCrtlRxPassive = 0x10U;
constexpr std::uint8_t kCanErrCrtlTxPassive = 0x20U;
constexpr std::uint8_t kCanErrCrtlActive = 0x40U;

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

std::string runtimeLabel(const DeviceConfig& device, const PointDefinition& point) {
    return "meter " + device.meterCode + " point " + point.pointCode + " (index " + std::to_string(point.index) + ")";
}

}  // namespace

CanDriverService::CanDriverService(
    DeviceConfig config,
    MemoryPointStore& store,
    std::shared_ptr<IMqttPublisher> mqttPublisher
) : config_(std::move(config)),
    store_(store),
    sqliteWriter_(config_.memoryStore.sqlitePath, config_.memoryStore.sqliteLibraryPath,
                  config_.memoryStore.historyRetentionDays),
    priorityControlLease_(
        config_.mqttDriver.priorityControlLeaseFile,
        config_.protocol.type + ":" + config_.memoryStore.sharedMemoryName
    ),
    powerControlOwnership_(
        config_.mqttDriver.powerControlOwnershipFile,
        "mqtt-forwarder"
    ),
    mqttPublisher_(std::move(mqttPublisher)) {
    if (config_.protocol.type != "can_socketcan") {
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

    // Transmit frames: every write signal of one (frameId, extended) shares a payload mirror.
    struct TxBuild {
        TxFrame frame;
        std::string templateHex;
        std::string owner;
        std::vector<std::string> signals;  // layout signature parts, one per write signal
    };
    std::unordered_map<std::uint64_t, TxBuild> txBuilds;
    const auto wallMs = nowMs();
    const auto monoMs = steadyNowMs();
    for (auto& expanded : expandedConfigs) {
        RuntimeDevice device;
        device.config = std::move(expanded);
        if (!device.config.meters.empty()) {
            device.onlineTimeoutMs = device.config.meters.front().onlineTimeoutMs;
            for (const auto& id : device.config.meters.front().onlineFrameIds) {
                try {
                    device.onlineFrameIds.push_back(CanSignalCodec::parseAnyFrameId(id));
                } catch (const std::exception& ex) {
                    throw std::invalid_argument("CAN meter " + device.config.meterCode + ": onlineFrameIds: " + ex.what());
                }
            }
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
            RuntimePoint runtimePoint;
            runtimePoint.deviceIndex = deviceIndex;
            runtimePoint.point = point;
            const auto label = runtimeLabel(device.config, point);
            try {
                if (isInterfacePoint(point)) {
                    if (point.read.enable) {
                        interfacePoints_.push_back(runtimePoints_.size());
                    }
                } else if (!isOnlinePoint(point)) {
                    if (point.read.enable) {
                        const auto& can = point.read.can;
                        runtimePoint.readSignal = true;
                        runtimePoint.readKey = CanSignalCodec::frameKey(
                            CanSignalCodec::parseFrameId(can.frameId, can.extended), can.extended);
                        readPointsByFrame_[runtimePoint.readKey].push_back(runtimePoints_.size());
                        if (std::find(device.signalFrameKeys.begin(), device.signalFrameKeys.end(),
                                      runtimePoint.readKey) == device.signalFrameKeys.end()) {
                            device.signalFrameKeys.push_back(runtimePoint.readKey);
                        }
                    }
                    if (point.write.enable) {
                        const auto& can = point.write.can;
                        const auto frameId = CanSignalCodec::parseFrameId(can.frameId, can.extended);
                        runtimePoint.writeSignal = true;
                        runtimePoint.writeKey = CanSignalCodec::frameKey(frameId, can.extended);
                        auto build = txBuilds.emplace(runtimePoint.writeKey, TxBuild());
                        auto& tx = build.first->second;
                        if (build.second) {
                            tx.owner = label;
                            tx.frame.frameId = frameId;
                            tx.frame.extended = can.extended;
                            tx.frame.fd = can.fd;
                            tx.frame.remoteRequest = can.remoteRequest;
                            tx.frame.templateSafe = can.templateSafe;
                            tx.frame.payload = CanSignalCodec::templatePayload(can);
                        } else if (tx.frame.payload.size() != static_cast<std::size_t>(std::max(0, can.dlc)) ||
                                   tx.frame.fd != can.fd || tx.frame.remoteRequest != can.remoteRequest ||
                                   tx.frame.templateSafe != can.templateSafe) {
                            throw std::invalid_argument("frame " + can.frameId + " is also written by " + tx.owner +
                                                        " with a different dlc/fd/remoteRequest/templateSafe");
                        }
                        if (!can.payloadTemplate.empty()) {
                            if (!tx.templateHex.empty() && tx.templateHex != can.payloadTemplate) {
                                throw std::invalid_argument("payloadTemplate differs from " + tx.owner);
                            }
                            tx.templateHex = can.payloadTemplate;
                            tx.frame.payload = CanSignalCodec::templatePayload(can);
                        }
                        if (can.pulse) {
                            if (point.write.dataType == "float32") {
                                throw std::invalid_argument("write.can.pulse needs a bool or integer signal");
                            }
                            // The idle value must encode (range checked here as well as at load time).
                            auto scratch = tx.frame.payload;
                            CanSignalCodec::encodeInto(scratch, can.pulseIdleValue, point.write, point.read.signedFlag);
                            tx.frame.pulsePoints.push_back(runtimePoints_.size());
                        } else {
                            tx.frame.signalPoints.push_back(runtimePoints_.size());
                        }
                        std::ostringstream signature;
                        signature << point.index << ':' << can.byteOffset << '/' << can.bitOffset << '/'
                                  << CanSignalCodec::signalBitLength(can, point.write.dataType) << '/' << can.endian
                                  << '/' << point.write.dataType << '/' << point.write.scale << '/'
                                  << point.write.offset << '/' << point.read.signedFlag << '/' << can.pulse << '/'
                                  << can.pulseIdleValue;
                        tx.signals.push_back(signature.str());
                    }
                }
            } catch (const std::exception& ex) {
                throw std::invalid_argument("CAN " + label + ": " + ex.what());
            }
            runtimePoints_.push_back(runtimePoint);
        }
        runtimeDevices_.push_back(std::move(device));
        publishOnlinePoint(runtimeDevices_.back(), false, wallMs, monoMs);
    }
    for (auto& entry : txBuilds) {
        auto& build = entry.second;
        std::sort(build.signals.begin(), build.signals.end());
        std::ostringstream signature;
        signature << "dlc=" << build.frame.payload.size() << ";fd=" << build.frame.fd << ";rtr="
                  << build.frame.remoteRequest << ";template=" << build.templateHex << ";safe="
                  << build.frame.templateSafe;
        for (const auto& signal : build.signals) {
            signature << ';' << signal;
        }
        build.frame.layout = can_frame_state::layoutHash(signature.str());
        txFrames_.emplace(entry.first, build.frame);
    }
    for (auto& entry : txFrames_) {
        // A template must not carry a pulse in its active state.
        storeSentPayloadLocked(entry.second, entry.second.payload);
    }
    frameStatePath_ = config_.protocol.can.frameStateFile;
    if (frameStatePath_.empty()) {
        const auto& dedup = config_.memoryStore.controlDedupPath;
        const auto slash = dedup.find_last_of('/');
        frameStatePath_ = (slash == std::string::npos ? std::string(".") : dedup.substr(0, slash)) +
            "/can-frame-state-" + config_.protocol.can.interfaceName + ".json";
    }
    loadFrameStates();
    publishInterfacePoints(wallMs, monoMs);
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
        if (setsockopt(socketFd_, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enable, sizeof(enable)) != 0) {
            std::cerr << "CAN FD frames could not be enabled on " << can.interfaceName << std::endl;
        }
#endif
    }
#ifdef CAN_RAW_ERR_FILTER
    // Error frames report bus-off, error-passive and controller restarts (interface_online).
    can_err_mask_t errorMask = kCanErrBusOff | kCanErrCrtl | kCanErrRestarted;
    if (setsockopt(socketFd_, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &errorMask, sizeof(errorMask)) != 0) {
        std::cerr << "CAN error frames could not be enabled on " << can.interfaceName << std::endl;
    }
#endif

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

CanDriverService::LinkState CanDriverService::linkStateLocked() const {
#ifdef _WIN32
    return LinkState::Down;
#else
    if (socketFd_ < 0) {
        return LinkState::Down;
    }
    if (usesUdpTestTransport(config_.protocol.can)) {
        return LinkState::Ready;
    }
    const auto fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return LinkState::Down;
    }
    ifreq ifr{};
    std::snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", config_.protocol.can.interfaceName.c_str());
    const auto result = ioctl(fd, SIOCGIFFLAGS, &ifr);
    ::close(fd);
    if (result != 0 || (ifr.ifr_flags & IFF_UP) == 0) {
        return LinkState::Down;
    }
    // A CAN controller in bus-off drops its carrier (IFF_RUNNING) until it restarts.
    return (ifr.ifr_flags & IFF_RUNNING) != 0 ? LinkState::Ready : LinkState::NoCarrier;
#endif
}

void CanDriverService::checkInterfaceOnce(std::int64_t wallMs, std::int64_t monoMs) {
    const auto intervalMs = std::max(1, config_.collect.interfaceCheckIntervalMs);
    if (lastInterfaceCheckMono_ > 0 && monoMs - lastInterfaceCheckMono_ < intervalMs) {
        return;
    }
    lastInterfaceCheckMono_ = monoMs;
    auto state = LinkState::Down;
    std::string reason;
    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        try {
            state = linkStateLocked();
            if (state == LinkState::NoCarrier && lastBusActivityMono_ > 0 &&
                monoMs - lastBusActivityMono_ < 2 * static_cast<std::int64_t>(intervalMs)) {
                // The carrier flag follows a controller restart with a delay; traffic proves the link.
                state = LinkState::Ready;
            }
            if (state == LinkState::NoCarrier) {
                // Bus-off (or no transceiver link): the driver never forces a recovery. The
                // controller restarts on its own only when restartMs > 0; with restartMs = 0 it
                // stays bus-off until someone runs `ip link set <if> type can restart`.
                linkDownSeen_ = true;
            }
            if (state == LinkState::Down) {
                // Interface missing or administratively down: reopen the socket; with
                // manageInterface the driver also reconfigures and brings the interface up.
                linkDownSeen_ = true;
                closeSocketLocked();
                configureInterface();
                openSocketLocked();
                state = linkStateLocked();
            }
        } catch (const std::exception& ex) {
            state = LinkState::Down;
            reason = ex.what();
        }
    }
    if (state == LinkState::Ready && linkDownSeen_) {
        // The link came back (restart or reconfiguration): the controller left bus-off.
        linkDownSeen_ = false;
        busOff_ = false;
    }
    if (reason.empty()) {
        reason = state == LinkState::Down ? "interface down" : state == LinkState::NoCarrier ? "no carrier (bus-off)"
            : busOff_ ? "bus-off" : "ok";
    }
    setInterfaceOnline(state == LinkState::Ready && !busOff_, reason, wallMs, monoMs);
}

void CanDriverService::handleErrorFrame(std::uint32_t errorClass, const std::vector<std::uint8_t>& data,
                                        std::int64_t wallMs, std::int64_t monoMs) {
    const auto details = std::string(R"("interfaceName":")") + escapeJson(config_.protocol.can.interfaceName) + "\"";
    if ((errorClass & kCanErrCrtl) != 0 && data.size() > 1) {
        const auto status = data[1];
        if ((status & (kCanErrCrtlRxPassive | kCanErrCrtlTxPassive)) != 0 && !errorPassive_) {
            // Still communicating, but the controller counts many errors: report, stay online.
            errorPassive_ = true;
            publishStatusEvent("bus-error-passive", wallMs, details);
            std::cerr << "CAN " << config_.protocol.can.interfaceName << " error-passive" << std::endl;
        }
        if ((status & kCanErrCrtlActive) != 0 && errorPassive_) {
            errorPassive_ = false;
            publishStatusEvent("bus-error-active", wallMs, details);
        }
    }
    if ((errorClass & kCanErrBusOff) != 0) {
        if (!busOff_) {
            busOff_ = true;
            publishStatusEvent("bus-off", wallMs, details);
            std::cerr << "CAN " << config_.protocol.can.interfaceName << " bus-off" << std::endl;
        }
        setInterfaceOnline(false, "bus-off", wallMs, monoMs);
    }
    if ((errorClass & kCanErrRestarted) != 0) {
        lastBusActivityMono_ = monoMs;
        busOff_ = false;
        errorPassive_ = false;
        publishStatusEvent("bus-restarted", wallMs, details);
        setInterfaceOnline(true, "controller restarted", wallMs, monoMs);
    }
}

void CanDriverService::setInterfaceOnline(bool online, const std::string& reason, std::int64_t wallMs,
                                          std::int64_t monoMs) {
    interfaceFault_.store(!online);
    if (online == interfaceOnline_) {
        return;
    }
    interfaceOnline_ = online;
    publishStatusEvent(
        online ? "interface-online" : "interface-offline",
        wallMs,
        std::string(R"("interfaceName":")") + escapeJson(config_.protocol.can.interfaceName) +
            R"(","reason":")" + escapeJson(reason) + R"(")"
    );
    std::cerr << "CAN interface " << config_.protocol.can.interfaceName << (online ? " online" : " offline")
              << " reason=" << reason << std::endl;
    if (!online) {
        // Nothing can be received now: every device is offline and every signal keeps its last
        // value as stale/bad quality until a new frame arrives.
        for (auto& device : runtimeDevices_) {
            device.lastSeenMono = 0;
            publishOnlinePoint(device, false, wallMs, monoMs);
        }
        for (auto& runtimePoint : runtimePoints_) {
            if (!runtimePoint.readSignal) {
                continue;
            }
            try {
                markPointBad(runtimePoint, "CAN interface offline: " + reason, true, wallMs);
            } catch (const std::exception& ex) {
                logPointError(runtimePoint, std::string("publish failed: ") + ex.what(), monoMs);
            }
            runtimePoint.lastSeenMono = 0;
            runtimePoint.stalePublished = true;
        }
    }
    publishInterfacePoints(wallMs, monoMs);
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
    bool error = false;
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
        error = frame.error;
        payload = std::move(frame.payload);
    } else {
        canfd_frame frame{};
        const auto nbytes = read(receiveFd, &frame, sizeof(frame));
        ::close(receiveFd);
        if (nbytes < 0) {
            throw std::runtime_error("failed to read CAN frame: " + std::string(std::strerror(errno)));
        }
        if (nbytes != CAN_MTU && nbytes != CANFD_MTU) {
            return 0;
        }
        error = (frame.can_id & CAN_ERR_FLAG) != 0;
        frameId = error ? (frame.can_id & CAN_ERR_MASK) : (frame.can_id & CAN_EFF_MASK);
        extended = (frame.can_id & CAN_EFF_FLAG) != 0;
        remote = (frame.can_id & CAN_RTR_FLAG) != 0;
        payload.assign(frame.data, frame.data + std::min<std::size_t>(frame.len, CANFD_MAX_DLEN));
    }
    const auto wallMs = nowMs();
    const auto monoMs = steadyNowMs();
    if (error) {
        handleErrorFrame(frameId, payload, wallMs, monoMs);
        return 0;
    }
    if (remote) {
        return 0;
    }
    std::size_t matched = 0;
    applyDataFrame(frameId, extended, payload, wallMs, monoMs, matched);
    return matched;
#endif
}

void CanDriverService::applyDataFrame(std::uint32_t frameId, bool extended, const std::vector<std::uint8_t>& payload,
                                      std::int64_t wallMs, std::int64_t monoMs, std::size_t& matched) {
    if (!interfaceOnline_ || busOff_) {
        // A received frame proves the controller is active (bus-off over even if the restart
        // error frame was missed).
        busOff_ = false;
        setInterfaceOnline(true, "frame received", wallMs, monoMs);
    }
    lastBusActivityMono_ = monoMs;
    const auto key = CanSignalCodec::frameKey(frameId, extended);
    for (auto& device : runtimeDevices_) {
        const bool belongs =
            std::find(device.onlineFrameIds.begin(), device.onlineFrameIds.end(), frameId) != device.onlineFrameIds.end() ||
            std::find(device.signalFrameKeys.begin(), device.signalFrameKeys.end(), key) != device.signalFrameKeys.end();
        if (belongs) {
            device.lastSeenMono = monoMs;
            if (!device.online) {
                publishOnlinePoint(device, true, wallMs, monoMs);
            }
        }
    }
    const auto points = readPointsByFrame_.find(key);
    if (points == readPointsByFrame_.end()) {
        return;
    }
    for (const auto pointIndex : points->second) {
        auto& runtimePoint = runtimePoints_[pointIndex];
        // One signal failing (short frame, invalid raw value, store error) never affects the others.
        try {
            runtimePoint.lastSeenMono = monoMs;
            runtimePoint.stalePublished = false;
            try {
                const auto decoded = CanSignalCodec::decode(payload, runtimePoint.point.read);
                publishPointValue(runtimeDevices_[runtimePoint.deviceIndex], runtimePoint.point, decoded, wallMs);
                ++matched;
            } catch (const CanInvalidRawValue& ex) {
                markPointBad(runtimePoint, ex.what(), false, wallMs);
            } catch (const std::exception& ex) {
                logPointError(runtimePoint, std::string("decode failed: ") + ex.what() +
                              " payloadBytes=" + std::to_string(payload.size()), monoMs);
                markPointBad(runtimePoint, ex.what(), false, wallMs);
            }
        } catch (const std::exception& ex) {
            logPointError(runtimePoint, std::string("publish failed: ") + ex.what(), monoMs);
        }
    }
}

void CanDriverService::markPointBad(RuntimePoint& runtimePoint, const std::string& message, bool stale,
                                    std::int64_t wallMs) {
    const auto& point = runtimePoint.point;
    const auto latest = store_.getLatestByIndex(point.index, wallMs);
    if (!latest && stale) {
        return;
    }
    const auto& device = runtimeDevices_[runtimePoint.deviceIndex].config;
    PointValue value;
    value.index = point.index;
    value.machineCode = device.machineCode;
    value.meterCode = device.meterCode;
    value.pointCode = point.pointCode;
    value.pointName = point.name;
    value.category = point.category;
    value.unit = point.read.unit;
    // The last good value is kept (never replaced by 0); quality says it is not current.
    value.value = latest ? latest->value : 0.0;
    value.text = message;
    value.quality = 0;
    value.qualityMsg = message;
    value.ts = wallMs;
    value.expireAt = wallMs + std::max<std::int64_t>(1, point.read.cachePolicy.ttlMs);
    value.stale = stale;
    value.function = 0;
    value.address = point.address;
    value.length = 1;
    value.isStore = false;  // never written to the history database
    value.persistOnChange = point.persistOnChange;
    value.persistIntervalSec = point.persistIntervalSec;
    store_.putLatest(value);
}

void CanDriverService::logPointError(RuntimePoint& runtimePoint, const std::string& message, std::int64_t monoMs) {
    if (runtimePoint.lastErrorLogMono > 0 && monoMs - runtimePoint.lastErrorLogMono < kErrorLogIntervalMs) {
        ++runtimePoint.suppressedErrors;
        return;
    }
    std::cerr << "CAN signal error " << runtimeLabel(runtimeDevices_[runtimePoint.deviceIndex].config, runtimePoint.point)
              << " " << message;
    if (runtimePoint.suppressedErrors > 0) {
        std::cerr << " (" << runtimePoint.suppressedErrors << " more in the last minute)";
    }
    std::cerr << std::endl;
    runtimePoint.lastErrorLogMono = std::max<std::int64_t>(1, monoMs);
    runtimePoint.suppressedErrors = 0;
}

std::size_t CanDriverService::flushPersistentOnce() {
    std::lock_guard<std::mutex> lock(persistentFlushMutex_);
    const auto immediateGeneration = store_.immediatePersistentGeneration();
    // Bounded batches (see GatewayDaemon::flushPersistentOnce).
    const auto batchSize = std::max<std::size_t>(1, config_.memoryStore.persistFlushBatchSize);
    std::size_t flushed = 0;
    for (std::size_t batch = 0; batch < 64; ++batch) {
        const auto samples = store_.peekPersistentSamples(batchSize);
        if (samples.empty()) {
            break;
        }
        sqliteWriter_.writeSamples(samples);
        store_.acknowledgePersistentSamples(samples.back().sequence);
        flushed += samples.size();
        if (samples.size() < batchSize) {
            break;
        }
    }
    flushedImmediatePersistentGeneration_.store(immediateGeneration, std::memory_order_release);
    return flushed;
}

void CanDriverService::flushImmediatePersistentSamples() {
    const auto requested = store_.immediatePersistentGeneration();
    if (requested == flushedImmediatePersistentGeneration_.load(std::memory_order_acquire)) {
        return;
    }
    try {
        flushPersistentOnce();
    } catch (const std::exception& ex) {
        std::cerr << "CAN immediate persistent flush failed"
                  << " sharedMemory=" << config_.memoryStore.sharedMemoryName
                  << " error=" << ex.what()
                  << std::endl;
    }
}

void CanDriverService::updateOnlineStatus(std::int64_t wallMs, std::int64_t monoMs) {
    for (auto& device : runtimeDevices_) {
        const auto timeoutMs = std::max(1, device.onlineTimeoutMs);
        const auto online = device.lastSeenMono > 0 && monoMs - device.lastSeenMono <= timeoutMs;
        std::int64_t refreshMs = std::min(timeoutMs, 30000);
        for (const auto& point : device.config.points) {
            if (isOnlinePoint(point) && point.read.cachePolicy.ttlMs > 0) {
                refreshMs = std::min(refreshMs, std::max<std::int64_t>(250, point.read.cachePolicy.ttlMs / 2));
            }
        }
        if (online != device.online || device.lastOnlinePublishedMono <= 0 ||
            monoMs - device.lastOnlinePublishedMono >= refreshMs) {
            publishOnlinePoint(device, online, wallMs, monoMs);
        }
    }
    if (!interfacePoints_.empty()) {
        std::int64_t refreshMs = 30000;
        for (const auto pointIndex : interfacePoints_) {
            const auto ttlMs = runtimePoints_[pointIndex].point.read.cachePolicy.ttlMs;
            if (ttlMs > 0) {
                refreshMs = std::min(refreshMs, std::max<std::int64_t>(250, ttlMs / 2));
            }
        }
        if (monoMs - lastInterfacePublishedMono_ >= refreshMs) {
            publishInterfacePoints(wallMs, monoMs);
        }
    }
    updatePointStaleness(wallMs, monoMs);
}

void CanDriverService::updatePointStaleness(std::int64_t wallMs, std::int64_t monoMs) {
    for (auto& runtimePoint : runtimePoints_) {
        const auto timeoutMs = runtimePoint.point.read.can.receiveTimeoutMs;
        if (!runtimePoint.readSignal || runtimePoint.lastSeenMono <= 0 || timeoutMs <= 0) {
            continue;
        }
        if (monoMs - runtimePoint.lastSeenMono <= timeoutMs) {
            runtimePoint.stalePublished = false;
            continue;
        }
        if (runtimePoint.stalePublished) {
            continue;
        }
        try {
            markPointBad(runtimePoint, "CAN receive timeout", true, wallMs);
        } catch (const std::exception& ex) {
            logPointError(runtimePoint, std::string("publish failed: ") + ex.what(), monoMs);
        }
        runtimePoint.stalePublished = true;
    }
}

void CanDriverService::receiveLoop() {
    std::int64_t lastStoreHeartbeatMono = 0;
    std::int64_t lastExpirySweepMono = 0;
    std::int64_t lastFailureReportMono = 0;
    while (running_.load()) {
        try {
            checkInterfaceOnce(nowMs(), steadyNowMs());
            if (lastStoreHeartbeatMono <= 0 || steadyNowMs() - lastStoreHeartbeatMono >= kStoreHeartbeatIntervalMs) {
                store_.heartbeatRegisteredPoints(nowMs());
                lastStoreHeartbeatMono = steadyNowMs();
            }
            // Receiving never pauses for a priority-control lease: CAN reception shares no bus with
            // the priority write, and alarms (e.g. fire detectors) must keep updating.
            processReceiveOnce(std::max(1, config_.collect.receiveWaitMs));
            const auto completedWall = nowMs();
            const auto completedMono = steadyNowMs();
            updateOnlineStatus(completedWall, completedMono);
            flushImmediatePersistentSamples();
            if (lastExpirySweepMono <= 0 || completedMono - lastExpirySweepMono >= kStoreExpirySweepIntervalMs) {
                store_.removeExpired(completedWall);
                lastExpirySweepMono = completedMono;
            }
        } catch (const std::exception& ex) {
            const auto monoMs = steadyNowMs();
            if (lastFailureReportMono <= 0 || monoMs - lastFailureReportMono >= kErrorLogIntervalMs) {
                lastFailureReportMono = monoMs;
                publishStatusEvent("receive-failed", nowMs(), std::string(R"("message":")") + escapeJson(ex.what()) + R"(")");
                std::cerr << "CAN receive failed: " << ex.what() << std::endl;
            }
            try {
                updateOnlineStatus(nowMs(), steadyNowMs());
            } catch (const std::exception&) {
            }
            sleepInterruptibly(running_, 1000);
        }
    }
}

void CanDriverService::writebackLoop() {
    const auto intervalMs = std::max(20, config_.collect.writebackIntervalMs);
    std::int64_t lastFailureReportMono = 0;
    while (running_.load()) {
        try {
            processWritebackOnce(nowMs());
        } catch (const std::exception& ex) {
            const auto monoMs = steadyNowMs();
            if (lastFailureReportMono <= 0 || monoMs - lastFailureReportMono >= kErrorLogIntervalMs) {
                lastFailureReportMono = monoMs;
                std::cerr << "CAN writeback failed: " << ex.what() << std::endl;
            }
        }
        sleepInterruptibly(running_, intervalMs);
    }
}

void CanDriverService::persistLoop() {
    PersistentFlushSchedule schedule(config_.memoryStore.persistFlushIntervalMs);
    while (running_.load()) {
        try {
            const auto ts = nowMs();
            // History flushes yield to an active priority control (shared storage I/O).
            if (!priorityControlBlocked(ts)) {
                const auto stats = store_.getStats();
                if (schedule.shouldFlush(stats.persistentCount, stats.persistentConfiguredLimit)) {
                    flushPersistentOnce();
                    schedule.succeeded();
                }
            }
        } catch (...) {
            schedule.failed();
        }
        try {
            std::unique_lock<std::mutex> lock(persistentFlushMutex_, std::try_to_lock);
            if (lock.owns_lock() && running_.load() && !priorityControlBlocked(nowMs())) {
                sqliteWriter_.cleanupExpiredSamples(nowMs());
            }
        } catch (const std::exception& ex) {
            std::cerr << "CAN point history cleanup failed: " << ex.what() << std::endl;
        }
        sleepInterruptibly(running_, PersistentFlushSchedule::kCheckIntervalMs);
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
    try {
        mqttPublisher_->publishStatusMessage(config_.machineCode, payload.str());
    } catch (const std::exception& ex) {
        std::cerr << "CAN status publish failed"
                  << " event=" << event
                  << " error=" << ex.what()
                  << std::endl;
    } catch (...) {
        std::cerr << "CAN status publish failed"
                  << " event=" << event
                  << " error=unknown"
                  << std::endl;
    }
}

void CanDriverService::publishPointValue(
    const RuntimeDevice& device,
    const PointDefinition& point,
    const DecodedValue& decoded,
    std::int64_t ts,
    int quality,
    const std::string& qualityMsg
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
    latest.quality = quality;
    latest.qualityMsg = qualityMsg;
    latest.ts = ts;
    latest.expireAt = ts + point.read.cachePolicy.ttlMs;
    latest.function = 0;
    latest.address = point.address;
    latest.length = 1;
    latest.isStore = point.isStore;
    latest.persistOnChange = point.persistOnChange;
    latest.persistIntervalSec = point.persistIntervalSec;
    store_.putLatest(latest);
}

void CanDriverService::publishOnlinePoint(RuntimeDevice& device, bool online, std::int64_t wallMs, std::int64_t monoMs) {
    device.online = online;
    device.lastOnlinePublishedMono = monoMs;
    for (const auto& point : device.config.points) {
        if (!point.enabled || !isOnlinePoint(point)) {
            continue;
        }
        DecodedValue decoded;
        decoded.value = online ? 1.0 : 0.0;
        decoded.text = online ? "1" : "0";
        decoded.rawHex = online ? "01" : "00";
        publishPointValue(device, point, decoded, wallMs);
    }
}

void CanDriverService::publishInterfacePoints(std::int64_t wallMs, std::int64_t monoMs) {
    lastInterfacePublishedMono_ = monoMs;
    for (const auto pointIndex : interfacePoints_) {
        const auto& runtimePoint = runtimePoints_[pointIndex];
        DecodedValue decoded;
        decoded.value = interfaceOnline_ ? 1.0 : 0.0;
        decoded.text = interfaceOnline_ ? "1" : "0";
        decoded.rawHex = interfaceOnline_ ? "01" : "00";
        publishPointValue(runtimeDevices_[runtimePoint.deviceIndex], runtimePoint.point, decoded, wallMs);
    }
}

std::int64_t CanDriverService::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::int64_t CanDriverService::steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

}  // namespace edge_gateway
