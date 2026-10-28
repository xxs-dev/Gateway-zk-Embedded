// CAN transmit side: pending writes, startup writes and the per-frame payload mirror.
#include "edge_gateway/can_driver_service.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

#include "can_driver_service_internal.hpp"
#include "edge_gateway/can_signal_codec.hpp"
#include "edge_gateway/cluster_write_authorization.hpp"
#include "edge_gateway/control_dedup_store.hpp"
#include "edge_gateway/ems_control_mode.hpp"
#include "edge_gateway/virtual_can_datagram.hpp"
#include "edge_gateway/writeback_service.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <linux/can.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace edge_gateway {

using namespace can_driver_detail;

// A write that passed every check and waits to be sent together with the other signals of its frame.
struct CanDriverService::StagedWrite {
    PendingWriteCommand command;
    WritebackResultRecord result;
    std::int64_t startedAt = 0;
    std::int64_t commandNowMs = 0;
    bool hasPoint = false;
    std::size_t pointIndex = 0;
    bool claimed = false;  // durable control reserved in the dedup ledger
    std::shared_ptr<void> modePermit;
};

std::size_t CanDriverService::processWritebackOnce(std::int64_t nowMsValue) {
    const auto entryTime = std::chrono::steady_clock::now();
    const auto commands = drainScheduledWriteCommands(
        store_,
        &priorityControlLease_,
        nowMsValue,
        config_.memoryStore.writebackBatchSize
    );
    std::size_t processed = 0;
    const auto maxAgeMs = config_.memoryStore.pendingWriteMaxAgeMs;
    const auto currentTime = [&] {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - entryTime).count();
        return nowMsValue > std::numeric_limits<std::int64_t>::max() - elapsed
            ? std::numeric_limits<std::int64_t>::max() : nowMsValue + elapsed;
    };

    // Consecutive writes to different signals of the same frame are merged into one frame; a second
    // write of a signal already in the group, or a write to another frame, sends the group first, so
    // every commanded value reaches the bus in order.
    std::vector<StagedWrite> group;
    const auto flushGroup = [&] {
        if (group.empty()) {
            return;
        }
        flushStagedWrites(group);
        for (auto& staged : group) {
            finishWrite(staged, processed);
        }
        group.clear();
    };

    for (const auto& command : commands) {
        StagedWrite staged;
        staged.command = command;
        staged.startedAt = nowMs();
        staged.commandNowMs = currentTime();
        staged.result = beginWritebackResult(command, staged.startedAt);
        const auto pointIt = indexToRuntimePoint_.find(command.index);
        staged.hasPoint = pointIt != indexToRuntimePoint_.end();
        staged.pointIndex = staged.hasPoint ? pointIt->second : 0;

        if (isPendingWriteExpired(command, pendingWriteMonotonicNowMs(), maxAgeMs)) {
            const auto expire = [&] {
                auto receipt = beginWritebackResult(command, staged.startedAt);
                completeWritebackResult(receipt, false, "pending write expired before execution", "expired", nowMs());
                return receipt;
            };
            if (command.durableControl && staged.hasPoint) {
                try {
                    const auto& device = runtimeDevices_[runtimePoints_[staged.pointIndex].deviceIndex].config;
                    ControlDedupStore dedup(config_.memoryStore.controlDedupPath);
                    staged.result = dedup.dispatch(device.machineCode, device.meterCode, command, staged.startedAt, expire);
                } catch (const std::exception& ex) {
                    completeWritebackResult(staged.result, false, ex.what(), "expired", nowMs());
                }
            } else {
                staged.result = expire();
            }
            finishWrite(staged, processed);
            continue;
        }
        const auto authorization = powerControlOwnership_.authorize(
            command.index, command.source, command.controlGeneration, command.highPriority, staged.commandNowMs);
        if (!authorization.allowed) {
            completeWritebackResult(staged.result, false, authorization.message, "control-rejected", nowMs());
            finishWrite(staged, processed);
            continue;
        }
        if (!staged.hasPoint) {
            completeWritebackResult(staged.result, false, "unknown-index", "writeback-skipped", nowMs());
            finishWrite(staged, processed);
            continue;
        }
        if (clusterAuthorizationRequired(config_.emsCluster, command)) {
            completeWritebackResult(staged.result, false, "cluster physical write is unsupported by CAN",
                                    "control-rejected", nowMs());
            finishWrite(staged, processed);
            continue;
        }
        const auto& runtimePoint = runtimePoints_[staged.pointIndex];
        const auto& point = runtimePoint.point;
        if (!group.empty()) {
            const auto& head = runtimePoints_[group.front().pointIndex];
            const bool sameSignalQueued = std::find_if(group.begin(), group.end(), [&](const StagedWrite& other) {
                return other.command.index == command.index;
            }) != group.end();
            if (!runtimePoint.writeSignal || runtimePoint.writeKey != head.writeKey || sameSignalQueued) {
                flushGroup();
            }
        }

        // Checks that need no I/O: limits, and that the value encodes into the signal.
        try {
            validateWriteValue(point, command.value);
            if (!runtimePoint.writeSignal) {
                throw std::invalid_argument("point has no write.can signal");
            }
            std::vector<std::uint8_t> scratch;
            {
                std::lock_guard<std::mutex> lock(socketMutex_);
                scratch = txFrames_.at(runtimePoint.writeKey).payload;
            }
            CanSignalCodec::encodeInto(scratch, command.value, point.write, point.read.signedFlag);
        } catch (const std::exception& ex) {
            completeWritebackResult(staged.result, false, ex.what(), "writeback-failed", nowMs());
            finishWrite(staged, processed);
            continue;
        }
        if (command.durableControl) {
            try {
                const auto& device = runtimeDevices_[runtimePoint.deviceIndex].config;
                ControlDedupStore dedup(config_.memoryStore.controlDedupPath);
                auto claim = dedup.claim(device.machineCode, device.meterCode, command, staged.startedAt);
                if (!claim.owner) {
                    staged.result = claim.result;
                    finishWrite(staged, processed);
                    continue;
                }
                staged.claimed = true;
            } catch (const std::exception& ex) {
                completeWritebackResult(staged.result, false, ex.what(), "writeback-failed", nowMs());
                finishWrite(staged, processed);
                continue;
            }
        }
        // Last checks right before the physical send: EMS mode permit (held until sent) and age.
        bool expiredBeforeSend = false;
        try {
            if (command.platformControl) {
                staged.modePermit = EmsControlMode(config_.memoryStore.emsControlModeFile).dispatchPermit(command);
            }
            if (isPendingWriteExpired(command, pendingWriteMonotonicNowMs(), maxAgeMs)) {
                expiredBeforeSend = true;
                throw std::runtime_error("pending write expired before physical send");
            }
        } catch (const std::exception& ex) {
            // A reserved durable control stays reserved (outcome unknown), as ControlDedupStore::dispatch does.
            completeWritebackResult(staged.result, false, ex.what(),
                                    staged.claimed ? "writeback-timeout" : expiredBeforeSend ? "expired" : "writeback-failed",
                                    nowMs());
            finishWrite(staged, processed);
            continue;
        }
        group.push_back(std::move(staged));
    }
    flushGroup();
    flushImmediatePersistentSamples();
    return processed;
}

std::size_t CanDriverService::flushStagedWrites(std::vector<StagedWrite>& group) {
    const auto key = runtimePoints_[group.front().pointIndex].writeKey;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        auto& frame = txFrames_.at(key);
        auto payload = frame.payload;
        try {
            for (const auto& staged : group) {
                const auto& point = runtimePoints_[staged.pointIndex].point;
                CanSignalCodec::encodeInto(payload, staged.command.value, point.write, point.read.signedFlag);
            }
            openSocketLocked();
            transmitLocked(frame, payload);
            frame.payload = payload;  // the mirror is what was last put on the bus
        } catch (const std::exception& ex) {
            error = ex.what();
        }
    }
    const auto completedAt = nowMs();
    for (auto& staged : group) {
        staged.modePermit.reset();
        if (!error.empty()) {
            // Not sent; a reserved durable control is reported like ControlDedupStore::dispatch does.
            completeWritebackResult(staged.result, false, error,
                                    staged.claimed ? "writeback-timeout" : "writeback-failed", completedAt);
            continue;
        }
        // Success means the kernel queued the frame; CAN has no acknowledgement from the receiver.
        auto receipt = beginWritebackResult(staged.command, staged.startedAt);
        completeWritebackResult(receipt, true, "ok", "writeback-succeeded", completedAt);
        if (staged.claimed) {
            const auto& device = runtimeDevices_[runtimePoints_[staged.pointIndex].deviceIndex].config;
            try {
                ControlDedupStore(config_.memoryStore.controlDedupPath).finish(device.machineCode, device.meterCode, receipt);
            } catch (const std::exception& ex) {
                receipt = beginWritebackResult(staged.command, staged.startedAt);
                completeWritebackResult(receipt, false, ex.what(), "writeback-timeout", nowMs());
            }
        }
        staged.result = receipt;
    }
    return group.size();
}

void CanDriverService::finishWrite(StagedWrite& staged, std::size_t& processed) {
    const auto& command = staged.command;
    store_.recordWritebackResult(staged.result);
    if (staged.result.stage != "writeback-skipped") {
        std::string details;
        if (staged.hasPoint) {
            details += std::string(R"("meterCode":")") +
                escapeJson(runtimeDevices_[runtimePoints_[staged.pointIndex].deviceIndex].config.meterCode) + R"(",)";
        }
        details += std::string(R"("index":)") + std::to_string(command.index) +
            R"(,"cmdId":")" + escapeJson(command.cmdId) + R"(")";
        if (!staged.result.success) {
            details += R"(,"message":")" + escapeJson(staged.result.message) + R"(")";
        }
        publishStatusEvent(staged.result.stage, staged.commandNowMs, details);
    }
    if (command.highPriority) {
        priorityControlLease_.release(command.cmdId);
    }
    if (staged.result.success) {
        ++processed;
    }
}

void CanDriverService::runStartupWrites() {
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
                sendSingleWrite(*pointIt, item.value);
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

void CanDriverService::sendSingleWrite(const RuntimePoint& runtimePoint, double value) {
    if (!runtimePoint.writeSignal) {
        throw std::invalid_argument("point has no write.can signal");
    }
    std::lock_guard<std::mutex> lock(socketMutex_);
    auto& frame = txFrames_.at(runtimePoint.writeKey);
    auto payload = frame.payload;
    CanSignalCodec::encodeInto(payload, value, runtimePoint.point.write, runtimePoint.point.read.signedFlag);
    openSocketLocked();
    transmitLocked(frame, payload);
    frame.payload = payload;
}

void CanDriverService::transmitLocked(const TxFrame& frame, const std::vector<std::uint8_t>& payload) {
#ifndef _WIN32
    if (usesUdpTestTransport(config_.protocol.can)) {
        VirtualCanFrame datagram;
        datagram.frameId = frame.frameId;
        datagram.extended = frame.extended;
        datagram.remoteRequest = frame.remoteRequest;
        datagram.fd = frame.fd;
        datagram.payload = payload;
        const auto bytes = encodeVirtualCanDatagram(datagram);
        if (bytes.empty()) {
            throw std::invalid_argument("virtual CAN payload must be <= 64 bytes");
        }
        sockaddr_in peer{};
        peer.sin_family = AF_INET;
        peer.sin_port = htons(static_cast<std::uint16_t>(config_.protocol.can.udpPeerPort));
        if (inet_pton(AF_INET, config_.protocol.can.udpPeerAddress.c_str(), &peer.sin_addr) != 1) {
            throw std::invalid_argument("invalid CAN udpPeerAddress: " + config_.protocol.can.udpPeerAddress);
        }
        if (sendto(socketFd_, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) !=
            static_cast<ssize_t>(bytes.size())) {
            throw std::runtime_error("failed to send virtual CAN UDP datagram");
        }
        return;
    }
    const auto canId = frame.frameId | (frame.extended ? CAN_EFF_FLAG : 0U);
    if (frame.fd) {
        canfd_frame wire{};
        wire.can_id = canId;
        wire.len = static_cast<__u8>(payload.size());
        std::memcpy(wire.data, payload.data(), payload.size());
        if (write(socketFd_, &wire, sizeof(wire)) != CANFD_MTU) {
            throw std::runtime_error("failed to send CAN FD frame");
        }
        return;
    }
    if (payload.size() > CAN_MAX_DLEN) {
        throw std::invalid_argument("classic CAN payload must be <= 8 bytes");
    }
    can_frame wire{};
    wire.can_id = canId | (frame.remoteRequest ? CAN_RTR_FLAG : 0U);
    wire.can_dlc = static_cast<__u8>(payload.size());
    std::memcpy(wire.data, payload.data(), payload.size());
    if (write(socketFd_, &wire, sizeof(wire)) != CAN_MTU) {
        throw std::runtime_error("failed to send CAN frame");
    }
#else
    (void)frame;
    (void)payload;
    throw std::runtime_error("SocketCAN is not supported on Windows");
#endif
}

}  // namespace edge_gateway
