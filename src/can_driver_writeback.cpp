// CAN transmit side: pending writes, startup writes and the per-frame payload mirror.
#include "edge_gateway/can_driver_service.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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
#include "edge_gateway/cluster_write_authorization.hpp"
#include "edge_gateway/control_dedup_store.hpp"
#include "edge_gateway/ems_control_mode.hpp"
#include "edge_gateway/virtual_can_datagram.hpp"
#include "edge_gateway/writeback_outcome.hpp"
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

namespace {

std::string frameName(std::uint32_t frameId) {
    std::ostringstream id;
    id << "0x" << std::uppercase << std::hex << frameId;
    return id.str();
}

std::string number(double value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

bool integerSignal(const std::string& dataType) {
    return dataType != "float32" && dataType != "float64";
}

}  // namespace

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
            requireInterfaceUsable();
            std::vector<std::uint8_t> scratch;
            {
                std::lock_guard<std::mutex> lock(socketMutex_);
                scratch = txFrames_.at(runtimePoint.writeKey).payload;
            }
            CanSignalCodec::encodeInto(scratch, command.value, point.write, point.read.signedFlag);
        } catch (const std::exception& ex) {
            // Refused before anything was sent: a definite failure, recorded in the control ledger
            // like any executed result so a replay of the cmdId reports the same.
            const std::string error = ex.what();
            const auto refuse = [&] {
                auto receipt = beginWritebackResult(command, staged.startedAt);
                completeWritebackResult(receipt, false, error, kWritebackFailedStage, nowMs());
                return receipt;
            };
            staged.result = refuse();
            if (command.durableControl) {
                try {
                    const auto& device = runtimeDevices_[runtimePoint.deviceIndex].config;
                    staged.result = ControlDedupStore(config_.memoryStore.controlDedupPath)
                        .dispatch(device.machineCode, device.meterCode, command, staged.startedAt, refuse);
                } catch (const std::exception& ledgerError) {
                    completeWritebackResult(staged.result, false, error + "; " + ledgerError.what(),
                                            kWritebackFailedStage, nowMs());
                }
            }
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
            // Nothing was sent: a definite result, recorded in the control ledger when reserved.
            completeNotSent(staged, ex.what(), expiredBeforeSend ? "expired" : "writeback-failed");
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
    std::vector<std::pair<std::size_t, double>> writes;
    for (const auto& staged : group) {
        writes.emplace_back(staged.pointIndex, staged.command.value);
    }
    std::string note;
    std::int64_t sentAtWallMs = 0;
    const auto error = transmitJournaled(key, writes, note, sentAtWallMs);
    const auto sentSteadyMs = steadyNowMs();
    std::string frame;
    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        frame = frameName(txFrames_.at(key).frameId);
    }
    for (auto& staged : group) {
        staged.modePermit.reset();
        if (!error.empty()) {
            completeNotSent(staged, error, "writeback-failed");
            continue;
        }
        auto receipt = sentReceipt(staged, frame, note, sentAtWallMs, sentSteadyMs);
        if (staged.claimed) {
            const auto& device = runtimeDevices_[runtimePoints_[staged.pointIndex].deviceIndex].config;
            try {
                ControlDedupStore(config_.memoryStore.controlDedupPath).finish(device.machineCode, device.meterCode, receipt);
            } catch (const std::exception& ex) {
                receipt = beginWritebackResult(staged.command, staged.startedAt);
                completeWritebackResult(receipt, false,
                    std::string("CAN frame sent but the control ledger result was not recorded: ") + ex.what(),
                    kWritebackUnconfirmedStage, nowMs());
                receipt.outcomeUnknown = true;
            }
        }
        staged.result = receipt;
    }
    return group.size();
}

WritebackResultRecord CanDriverService::sentReceipt(const StagedWrite& staged, const std::string& frame,
                                                   const std::string& note, std::int64_t sentAtWallMs,
                                                   std::int64_t sentSteadyMs) {
    const auto& point = runtimePoints_[staged.pointIndex].point;
    const auto suffix = note.empty() ? std::string() : "; " + note;
    auto receipt = beginWritebackResult(staged.command, staged.startedAt);
    const auto feedbackIndex = point.write.verifyIndex;
    if (feedbackIndex == 0) {
        // SocketCAN reports only that the kernel queued the frame: no receiver acknowledges it, and the
        // frame may still be lost (bus-off, no ACK). Never reported as a confirmed action.
        completeWritebackResult(receipt, true, "submitted: CAN frame " + frame + " queued by the kernel; no device "
                                "feedback configured" + suffix, kWritebackSubmittedStage, nowMs());
        return receipt;
    }
    // Device feedback signal (write.verifyIndex): only a good, current value stored after the send
    // counts. It must equal the written value within half a step of the write and feedback scaling.
    double step = integerSignal(point.write.dataType) ? std::abs(point.write.scale) : 0.0;
    const auto local = indexToRuntimePoint_.find(feedbackIndex);
    if (local != indexToRuntimePoint_.end()) {
        const auto& read = runtimePoints_[local->second].point.read;
        if (integerSignal(read.dataType)) step = std::max(step, std::abs(read.scale));
    }
    const auto tolerance = step / 2.0 + 1e-6;
    const auto expected = staged.command.value;
    const auto timeoutMs = std::max(1, point.write.verifyTimeoutMs);
    const auto label = "feedback index " + std::to_string(feedbackIndex);
    bool haveValue = false;
    double lastValue = 0.0;
    std::string lastError = "no value";
    while (true) {
        const auto stored = store_.getLatestByIndex(feedbackIndex, nowMs());
        if (!stored) {
            lastError = "no value";
        } else if (stored->stale || stored->quality != 1) {
            lastError = "stale or bad quality";
        } else if (stored->ts <= sentAtWallMs) {
            lastError = "no value received since the send";
        } else {
            haveValue = true;
            lastValue = stored->value;
            if (std::abs(lastValue - expected) <= tolerance) {
                break;
            }
        }
        if (steadyNowMs() - sentSteadyMs >= timeoutMs) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto completedAt = nowMs();
    if (haveValue && std::abs(lastValue - expected) <= tolerance) {
        completeWritebackResult(receipt, true, "ok: CAN frame " + frame + " sent; " + label + "=" + number(lastValue) +
                                " confirms" + suffix, kWritebackSucceededStage, completedAt, true, true);
    } else if (haveValue) {
        // Sent, and the device reports another value: it may or may not have acted. Never re-sent.
        completeWritebackResult(receipt, false, "feedback mismatch: CAN frame " + frame + " sent, " + label + "=" +
                                number(lastValue) + ", expected " + number(expected) + " (" + std::to_string(timeoutMs) +
                                " ms); not re-sent" + suffix, kWritebackMismatchStage, completedAt, true, false);
        receipt.feedbackMismatch = true;
    } else {
        completeWritebackResult(receipt, false, "outcome unknown: CAN frame " + frame + " sent, " + label +
                                " no new value in " + std::to_string(timeoutMs) + " ms (" + lastError + ")" + suffix,
                                kWritebackTimeoutStage, completedAt, true, false);
        receipt.outcomeUnknown = true;
    }
    normalizeWritebackOutcome(receipt);
    return receipt;
}

void CanDriverService::completeNotSent(StagedWrite& staged, const std::string& message, const std::string& stage) {
    completeWritebackResult(staged.result, false, message, stage, nowMs());
    if (!staged.claimed) {
        return;
    }
    // The command was reserved in the control ledger and is known not to have been sent: record that
    // definite result so a replay of the cmdId reports it instead of an open reservation.
    const auto& device = runtimeDevices_[runtimePoints_[staged.pointIndex].deviceIndex].config;
    try {
        ControlDedupStore(config_.memoryStore.controlDedupPath).finish(device.machineCode, device.meterCode, staged.result);
    } catch (const std::exception& ex) {
        auto receipt = beginWritebackResult(staged.command, staged.startedAt);
        completeWritebackResult(receipt, false,
                                std::string("not sent, control ledger result not recorded (") + ex.what() + "): " + message,
                                kWritebackUnconfirmedStage, nowMs());
        receipt.outcomeUnknown = true;
        staged.result = receipt;
    }
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
                // Queued by the kernel only; a startup write never waits for device feedback.
                publishStatusEvent(
                    "startup-write-submitted",
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
    const auto pointIndex = static_cast<std::size_t>(&runtimePoint - runtimePoints_.data());
    std::string note;
    std::int64_t sentAtWallMs = 0;
    const auto error =
        transmitJournaled(runtimePoint.writeKey, {std::make_pair(pointIndex, value)}, note, sentAtWallMs);
    if (!error.empty()) {
        throw std::runtime_error(error);
    }
}

void CanDriverService::requireInterfaceUsable() const {
    if (interfaceFault_.load()) {
        // Queued frames would go out whenever the bus recovers, possibly long after the command.
        throw std::runtime_error("CAN interface offline (bus-off or link down): frame not sent");
    }
}

std::string CanDriverService::transmitJournaled(std::uint64_t key,
                                                const std::vector<std::pair<std::size_t, double>>& writes,
                                                std::string& note, std::int64_t& sentAtWallMs) {
    note.clear();
    sentAtWallMs = 0;
    std::vector<std::uint8_t> payload;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        const auto& frame = txFrames_.at(key);
        name = frameName(frame.frameId);
        if (!frame.trusted) {
            std::string missing;
            for (const auto pointIndex : frame.signalPoints) {
                const bool written = std::find_if(writes.begin(), writes.end(),
                    [pointIndex](const std::pair<std::size_t, double>& write) { return write.first == pointIndex; }) !=
                    writes.end();
                if (!written) {
                    missing += (missing.empty() ? "" : ", ") + runtimePoints_[pointIndex].point.pointCode;
                }
            }
            if (!missing.empty()) {
                // Writing one signal would send the template (or stale) values of the others. The receipt
                // message holds 127 bytes: the missing signals come before the reason.
                const auto refusal = "CAN frame " + name + " not sent: no trusted state, write all signals together"
                    " (missing " + missing + "); " + frame.untrustedReason;
                std::cerr << refusal << " (or declare write.can.templateSafe if its payloadTemplate is a safe state)"
                          << std::endl;
                return refusal;
            }
        }
        payload = frame.payload;
        try {
            for (const auto& write : writes) {
                const auto& point = runtimePoints_[write.first].point;
                CanSignalCodec::encodeInto(payload, write.second, point.write, point.read.signedFlag);
            }
            requireInterfaceUsable();
        } catch (const std::exception& ex) {
            return ex.what();
        }
    }
    // 1. Write-ahead: the journal marks the frame pending before anything goes out. A crash or power
    //    loss from here until step 3 leaves it pending, i.e. untrusted after the restart.
    try {
        persistFrameStates(key);
    } catch (const std::exception& ex) {
        return "CAN frame " + name + " not sent: requested-state journal write failed (" + std::string(ex.what()) +
            ")";
    }
    // 2. Send.
    std::string sendError;
    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        auto& frame = txFrames_.at(key);
        try {
            requireInterfaceUsable();
            openSocketLocked();
            sentAtWallMs = nowMs();  // feedback must be newer than this
            transmitLocked(frame, payload);
            storeSentPayloadLocked(frame, payload);
            frame.trusted = true;
            frame.untrustedReason.clear();
        } catch (const std::exception& ex) {
            sendError = ex.what();
        }
    }
    if (!sendError.empty()) {
        try {
            persistFrameStates(0);  // not sent: the previous requested state stands
        } catch (const std::exception& ex) {
            std::cerr << "CAN requested-state journal still marks " << name << " pending (" << ex.what()
                      << "); it is untrusted after a restart" << std::endl;
        }
        return sendError;
    }
    // 3. Commit the new requested state.
    try {
        persistFrameStates(0);
    } catch (const std::exception& ex) {
        {
            std::lock_guard<std::mutex> lock(socketMutex_);
            auto& frame = txFrames_.at(key);
            frame.trusted = false;
            frame.untrustedReason = std::string("state not recorded after a send: ") + ex.what();
        }
        note = "requested state not recorded, CAN frame " + name + " untrusted until written whole (" + ex.what() +
            ")";
        std::cerr << note << std::endl;
        publishStatusEvent("frame-state-commit-failed", nowMs(),
                           std::string(R"("frameId":")") + name + R"(","message":")" + escapeJson(ex.what()) + R"(")");
    }
    return std::string();
}

void CanDriverService::storeSentPayloadLocked(TxFrame& frame, const std::vector<std::uint8_t>& payload) {
    auto mirror = payload;
    for (const auto pointIndex : frame.pulsePoints) {
        const auto& point = runtimePoints_[pointIndex].point;
        CanSignalCodec::encodeInto(mirror, point.write.can.pulseIdleValue, point.write, point.read.signedFlag);
    }
    frame.payload = mirror;
}

void CanDriverService::loadFrameStates() {
    const auto& interfaceName = config_.protocol.can.interfaceName;
    std::string fileProblem;
    std::vector<can_frame_state::Entry> entries;
    std::string text;
    try {
        if (!can_frame_state::readFile(frameStatePath_, text)) {
            fileProblem = "no journal";
        } else {
            entries = can_frame_state::parse(text, interfaceName);
        }
    } catch (const std::exception& ex) {
        fileProblem = std::string("journal rejected: ") + ex.what();
        entries.clear();
    }
    if (!fileProblem.empty() && !txFrames_.empty()) {
        std::cerr << "CAN requested-state journal " << frameStatePath_ << ": " << fileProblem << std::endl;
    }
    for (auto& item : txFrames_) {
        auto& frame = item.second;
        frame.trusted = false;
        frame.untrustedReason = fileProblem.empty() ? "no journal entry" : fileProblem;
    }
    for (const auto& entry : entries) {
        const auto found = txFrames_.find(CanSignalCodec::frameKey(entry.frameId, entry.extended));
        if (found == txFrames_.end()) {
            continue;
        }
        auto& frame = found->second;
        if (entry.fd != frame.fd || entry.remoteRequest != frame.remoteRequest ||
            entry.dlc != static_cast<int>(frame.payload.size()) || entry.layout != frame.layout) {
            frame.untrustedReason = "frame definition changed since recorded";
        } else if (entry.pending) {
            frame.untrustedReason = "send in flight at power loss/crash";
        } else if (entry.requested.empty()) {
            frame.untrustedReason = "no state recorded";
        } else {
            // Pulse signals are never restored active, whatever the journal says.
            storeSentPayloadLocked(frame, entry.requested);
            frame.trusted = true;
            frame.untrustedReason.clear();
        }
    }
    for (auto& item : txFrames_) {
        auto& frame = item.second;
        const auto name = frameName(frame.frameId);
        if (frame.trusted) {
            std::cerr << "CAN frame " << name << ": last requested state restored (gateway request, not the "
                      << "device state)" << std::endl;
        } else if (frame.templateSafe) {
            frame.trusted = true;
            std::cerr << "CAN frame " << name << ": " << frame.untrustedReason
                      << "; starts from its payloadTemplate (declared templateSafe)" << std::endl;
            frame.untrustedReason.clear();
        } else if (frame.signalPoints.size() > 1 || !frame.pulsePoints.empty()) {
            std::cerr << "CAN frame " << name << ": " << frame.untrustedReason
                      << "; writes are refused until all its signals are written together" << std::endl;
        }
    }
}

void CanDriverService::persistFrameStates(std::uint64_t pendingKey) {
    std::vector<std::pair<std::uint64_t, can_frame_state::Entry>> snapshot;
    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        for (const auto& item : txFrames_) {
            can_frame_state::Entry entry;
            entry.frameId = item.second.frameId;
            entry.extended = item.second.extended;
            entry.fd = item.second.fd;
            entry.remoteRequest = item.second.remoteRequest;
            entry.dlc = static_cast<int>(item.second.payload.size());
            entry.layout = item.second.layout;
            entry.pending = item.first == pendingKey;
            if (item.second.trusted) {
                entry.requested = item.second.payload;  // pulse signals already idle
            }
            snapshot.emplace_back(item.first, entry);
        }
    }
    std::sort(snapshot.begin(), snapshot.end(),
              [](const std::pair<std::uint64_t, can_frame_state::Entry>& left,
                 const std::pair<std::uint64_t, can_frame_state::Entry>& right) { return left.first < right.first; });
    std::vector<can_frame_state::Entry> entries;
    for (const auto& item : snapshot) {
        entries.push_back(item.second);
    }
    std::lock_guard<std::mutex> lock(frameStateMutex_);
    can_frame_state::writeAtomically(frameStatePath_,
                                     can_frame_state::serialize(config_.protocol.can.interfaceName, entries));
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
