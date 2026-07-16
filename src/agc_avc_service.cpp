#include "edge_gateway/agc_avc_service.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace edge_gateway {

namespace {

void appendRefIndex(std::vector<std::uint32_t>& indexes, const AgcAvcPointRefConfig& ref) {
    if (ref.index != 0) {
        indexes.push_back(ref.index);
    }
}

void appendRefIndexes(std::vector<std::uint32_t>& indexes, const std::vector<AgcAvcPointRefConfig>& refs) {
    for (const auto& ref : refs) {
        appendRefIndex(indexes, ref);
    }
}

void appendPcsInputIndexes(std::vector<std::uint32_t>& indexes, const AgcAvcPcsConfig& pcs) {
    appendRefIndex(indexes, pcs.points.online);
    appendRefIndex(indexes, pcs.points.ready);
    appendRefIndex(indexes, pcs.points.soc);
    appendRefIndex(indexes, pcs.points.actualP);
    appendRefIndex(indexes, pcs.points.actualQ);
    appendRefIndex(indexes, pcs.points.chargeAllowed);
    appendRefIndex(indexes, pcs.points.dischargeAllowed);
    appendRefIndex(indexes, pcs.points.availableChargeKw);
    appendRefIndex(indexes, pcs.points.availableDischargeKw);
    appendRefIndex(indexes, pcs.points.dynamicReactiveLimitKvar);
    appendRefIndex(indexes, pcs.points.temperature);
    appendRefIndexes(indexes, pcs.points.acVoltage);
    appendRefIndexes(indexes, pcs.points.acCurrent);
    if (pcs.ratedActivePowerKw.hasSourcePoint) {
        appendRefIndex(indexes, pcs.ratedActivePowerKw.sourcePoint);
    }
    if (pcs.ratedApparentPowerKva.hasSourcePoint) {
        appendRefIndex(indexes, pcs.ratedApparentPowerKva.sourcePoint);
    }
}

const StoredPointValue* findValue(
    const std::unordered_map<std::uint32_t, StoredPointValue>& values,
    std::uint32_t index
) {
    const auto it = values.find(index);
    return it == values.end() ? nullptr : &it->second;
}

bool pointUsable(const StoredPointValue& value, std::int64_t nowMs, int maxAgeMs) {
    if (value.quality <= 0 || value.stale || !std::isfinite(value.value) || value.ts <= 0) {
        return false;
    }
    return maxAgeMs <= 0 || nowMs - value.ts <= maxAgeMs;
}

double readValue(
    const AgcAvcPointRefConfig& ref,
    const std::unordered_map<std::uint32_t, StoredPointValue>& values,
    std::int64_t nowMs,
    int maxAgeMs,
    bool& healthy,
    std::string& failureReason,
    bool* valid = nullptr,
    double fallback = 0.0
) {
    if (valid != nullptr) {
        *valid = false;
    }
    if (ref.index == 0) {
        if (ref.required) {
            healthy = false;
            failureReason = "required point is not configured: " + ref.semanticRole;
        }
        return fallback;
    }
    const auto* value = findValue(values, ref.index);
    if (value == nullptr || !pointUsable(*value, nowMs, maxAgeMs)) {
        if (ref.required) {
            healthy = false;
            failureReason = "required point is missing, stale, or bad quality: " + ref.semanticRole;
        }
        return fallback;
    }
    if (valid != nullptr) {
        *valid = true;
    }
    return value->value;
}

double readMaxAbsValue(
    const std::vector<AgcAvcPointRefConfig>& refs,
    const std::unordered_map<std::uint32_t, StoredPointValue>& values,
    std::int64_t nowMs,
    int maxAgeMs,
    bool& healthy,
    std::string& failureReason,
    bool* valid
) {
    bool anyValid = false;
    double result = 0.0;
    for (const auto& ref : refs) {
        bool pointValid = false;
        const auto value = readValue(ref, values, nowMs, maxAgeMs, healthy, failureReason, &pointValid);
        if (pointValid) {
            anyValid = true;
            result = std::max(result, std::abs(value));
        }
    }
    if (valid != nullptr) {
        *valid = anyValid;
    }
    return result;
}

void inheritIdentity(AgcAvcPointRefConfig& ref, const AgcAvcPcsConfig& pcs) {
    if (ref.machineCode.empty()) {
        ref.machineCode = pcs.machineCode;
    }
    if (ref.meterCode.empty()) {
        ref.meterCode = pcs.meterCode;
    }
}

std::vector<AgcAvcPointRefConfig> allPcsRefs(AgcAvcPcsConfig pcs) {
    std::vector<AgcAvcPointRefConfig> refs = {
        pcs.points.online,
        pcs.points.ready,
        pcs.points.soc,
        pcs.points.actualP,
        pcs.points.actualQ,
        pcs.points.chargeAllowed,
        pcs.points.dischargeAllowed,
        pcs.points.availableChargeKw,
        pcs.points.availableDischargeKw,
        pcs.points.dynamicReactiveLimitKvar,
        pcs.points.temperature,
    };
    refs.insert(refs.end(), pcs.points.acVoltage.begin(), pcs.points.acVoltage.end());
    refs.insert(refs.end(), pcs.points.acCurrent.begin(), pcs.points.acCurrent.end());
    refs.insert(refs.end(), pcs.points.activeTargets.begin(), pcs.points.activeTargets.end());
    refs.insert(refs.end(), pcs.points.reactiveTargets.begin(), pcs.points.reactiveTargets.end());
    if (pcs.ratedActivePowerKw.hasSourcePoint) {
        refs.push_back(pcs.ratedActivePowerKw.sourcePoint);
    }
    if (pcs.ratedApparentPowerKva.hasSourcePoint) {
        refs.push_back(pcs.ratedApparentPowerKva.sourcePoint);
    }
    for (auto& ref : refs) {
        inheritIdentity(ref, pcs);
    }
    return refs;
}

void validateRef(
    const AgcAvcPointRefConfig& ref,
    const std::string& path,
    const IAgcAvcPointBus& pointBus,
    std::vector<AgcAvcValidationIssue>& issues,
    bool writeRequired = false
) {
    if (ref.index == 0) {
        if (ref.required || writeRequired) {
            issues.push_back({"error", path, "required shared-memory point is not configured"});
        }
        return;
    }
    const auto route = pointBus.routeByIndex(ref.index);
    if (!route) {
        issues.push_back({"error", path, "index is not present in the global point route: " + std::to_string(ref.index)});
        return;
    }
    if (!ref.machineCode.empty() && route->machineCode != ref.machineCode) {
        issues.push_back({"error", path, "machineCode does not match the shared-memory route"});
    }
    if (!ref.meterCode.empty() && route->meterCode != ref.meterCode) {
        issues.push_back({"error", path, "meterCode does not match the shared-memory route"});
    }
    if (!ref.pointCode.empty() && route->pointCode != ref.pointCode) {
        issues.push_back({"error", path, "pointCode does not match the shared-memory route"});
    }
    if ((writeRequired || ref.source == "sharedWriteback") && !route->writable) {
        issues.push_back({"error", path, "target point is not writable"});
    }
}

std::string makeCommandId(std::int64_t sequence, std::int64_t nowMs, std::uint32_t index) {
    return "AGCAVC_" + std::to_string(sequence) + "_" + std::to_string(nowMs) + "_" + std::to_string(index);
}

}  // namespace

PointStoreAgcAvcBus::PointStoreAgcAvcBus(PointStoreRouter& router)
    : router_(router) {
}

Optional<PointStoreRoute> PointStoreAgcAvcBus::routeByIndex(std::uint32_t index) const {
    return router_.routeByIndex(index);
}

std::vector<StoredPointValue> PointStoreAgcAvcBus::readSnapshot(
    const std::vector<std::uint32_t>& indexes,
    std::int64_t nowMs
) const {
    return router_.getLatestByIndexes(indexes, nowMs);
}

CommandSubmitResult PointStoreAgcAvcBus::publishVirtual(PointValue value) {
    return router_.putLatestByIndex(std::move(value));
}

CommandSubmitResult PointStoreAgcAvcBus::submitControl(const PendingWriteCommand& command) {
    return router_.submitWriteCommand(command);
}

Optional<WritebackResultRecord> PointStoreAgcAvcBus::readWriteResult(
    const PointStoreRoute& route,
    const std::string& cmdId
) const {
    return router_.getWritebackResult(route, cmdId);
}

AgcAvcService::AgcAvcService(AgcAvcConfig config, IAgcAvcPointBus& pointBus)
    : config_(std::move(config)),
      pointBus_(pointBus),
      controller_(config_),
      priorityLease_(config_.priorityControlLeaseFile, "agc-avc"),
      powerOwnership_(config_.ownership.leaseFile, "agc-avc"),
      ownershipSessionId_("AGCAVC_" + std::to_string(reinterpret_cast<std::uintptr_t>(this))) {
    for (const auto& pcs : config_.pcs) {
        for (const auto& ref : pcs.points.activeTargets) {
            if (ref.index != 0) {
                ownershipTargetIndexes_.push_back(ref.index);
            }
        }
        for (const auto& ref : pcs.points.reactiveTargets) {
            if (ref.index != 0) {
                ownershipTargetIndexes_.push_back(ref.index);
            }
        }
    }
    std::sort(ownershipTargetIndexes_.begin(), ownershipTargetIndexes_.end());
    ownershipTargetIndexes_.erase(
        std::unique(ownershipTargetIndexes_.begin(), ownershipTargetIndexes_.end()),
        ownershipTargetIndexes_.end()
    );
}

AgcAvcService::~AgcAvcService() {
    if (ownershipHeld_) {
        powerOwnership_.release(ownershipSessionId_);
    }
}

std::vector<AgcAvcValidationIssue> AgcAvcService::validate() const {
    std::vector<AgcAvcValidationIssue> issues;
    if (!config_.enabled) {
        return issues;
    }
    if (config_.cycleMs < 50 || config_.cycleMs > 60000) {
        issues.push_back({"error", "agcAvc.cycleMs", "cycleMs must be between 50 and 60000"});
    }
    if (config_.commandTimeoutMs < config_.cycleMs * 2) {
        issues.push_back({"error", "agcAvc.commandTimeoutMs", "command timeout must cover at least two control cycles"});
    }
    if (config_.inputMaxAgeMs <= 0 || config_.inputMaxAgeMs > config_.commandTimeoutMs) {
        issues.push_back({"error", "agcAvc.inputMaxAgeMs", "input max age must be positive and not exceed command timeout"});
    }
    if (config_.avcCycleMs < config_.cycleMs || config_.avcCycleMs > 60000) {
        issues.push_back({"error", "agcAvc.avcCycleMs", "AVC cycle must be at least the AGC cycle and not exceed 60000 ms"});
    }
    if (config_.writeResultTimeoutMs <= 0 || config_.maxConsecutiveWriteFailures <= 0) {
        issues.push_back({"error", "agcAvc.writeback", "write result timeout and maximum consecutive failures must be positive"});
    }
    if (config_.shadowMode && config_.submitWrites) {
        issues.push_back({"error", "agcAvc.submitWrites", "submitWrites cannot be enabled while shadowMode is true"});
    }
    if (config_.stationLimits.ratedActivePowerKw <= 0.0 || config_.stationLimits.ratedApparentPowerKva <= 0.0) {
        issues.push_back({"error", "agcAvc.stationLimits", "station rated active and apparent power must be positive"});
    }
    if (config_.stationLimits.ratedActivePowerKw > config_.stationLimits.ratedApparentPowerKva) {
        issues.push_back({"error", "agcAvc.stationLimits", "station rated active power cannot exceed apparent power"});
    }
    if (config_.pcs.empty()) {
        issues.push_back({"error", "agcAvc.pcs", "at least one PCS or inverter is required"});
    }

    validateRef(config_.interlocks.remoteEnable, "agcAvc.interlocks.remoteEnable", pointBus_, issues);
    validateRef(config_.interlocks.emergencyStop, "agcAvc.interlocks.emergencyStop", pointBus_, issues);
    validateRef(config_.interlocks.fireAlarm, "agcAvc.interlocks.fireAlarm", pointBus_, issues);
    validateRef(config_.agc.target, "agcAvc.agc.target", pointBus_, issues);
    validateRef(config_.agc.commandSequence, "agcAvc.agc.commandSequence", pointBus_, issues);
    validateRef(config_.agc.commandTimestamp, "agcAvc.agc.commandTimestamp", pointBus_, issues);
    validateRef(config_.agc.pccActivePower, "agcAvc.agc.pccActivePower", pointBus_, issues);
    validateRef(config_.agc.frequency, "agcAvc.agc.frequency", pointBus_, issues);
    validateRef(config_.avc.mode, "agcAvc.avc.mode", pointBus_, issues);
    validateRef(config_.avc.targetQ, "agcAvc.avc.targetQ", pointBus_, issues);
    validateRef(config_.avc.targetVoltage, "agcAvc.avc.targetVoltage", pointBus_, issues);
    validateRef(config_.avc.targetPowerFactor, "agcAvc.avc.targetPowerFactor", pointBus_, issues);
    validateRef(config_.avc.pccReactivePower, "agcAvc.avc.pccReactivePower", pointBus_, issues);
    validateRef(config_.avc.pccVoltage, "agcAvc.avc.pccVoltage", pointBus_, issues);
    validateRef(config_.avc.pccPowerFactor, "agcAvc.avc.pccPowerFactor", pointBus_, issues);

    std::unordered_set<std::uint32_t> writeIndexes;
    for (std::size_t i = 0; i < config_.pcs.size(); ++i) {
        const auto& pcs = config_.pcs[i];
        const auto prefix = "agcAvc.pcs[" + std::to_string(i) + "]";
        if (pcs.meterCode.empty()) {
            issues.push_back({"error", prefix + ".meterCode", "meterCode is required"});
        }
        const double ratedP = pcs.ratedActivePowerKw.commissionedLimit;
        const double ratedS = pcs.ratedApparentPowerKva.commissionedLimit;
        if (ratedP <= 0.0 || ratedS <= 0.0 || ratedP > ratedS) {
            issues.push_back({"error", prefix, "rated active/apparent power is invalid"});
        }
        if (pcs.minStableChargePowerKw > pcs.maxChargePowerKw ||
            pcs.minStableDischargePowerKw > pcs.maxDischargePowerKw) {
            issues.push_back({"error", prefix, "minimum stable power exceeds maximum power"});
        }
        if (pcs.socDischargeStopPercent < 0.0 ||
            pcs.socDischargeStopPercent > pcs.socDischargeDeratePercent ||
            pcs.socDischargeDeratePercent > pcs.socChargeDeratePercent ||
            pcs.socChargeDeratePercent > pcs.socChargeStopPercent ||
            pcs.socChargeStopPercent > 100.0) {
            issues.push_back({"error", prefix, "SOC stop and derating thresholds must be ordered within 0..100"});
        }
        if ((pcs.temperatureStopC > 0.0 && pcs.temperatureStopC <= pcs.temperatureDerateStartC) ||
            (pcs.currentStopPercent > 0.0 && pcs.currentStopPercent <= pcs.currentDerateStartPercent)) {
            issues.push_back({"error", prefix, "temperature/current stop threshold must exceed its derating threshold"});
        }
        if (pcs.feedbackTimeoutMs < 0 || pcs.feedbackToleranceKw < 0.0 || pcs.feedbackToleranceKvar < 0.0) {
            issues.push_back({"error", prefix, "feedback timeout and tolerances cannot be negative"});
        }
        const auto refs = allPcsRefs(pcs);
        for (std::size_t j = 0; j < refs.size(); ++j) {
            const bool write = refs[j].source == "sharedWriteback";
            validateRef(refs[j], prefix + ".points[" + std::to_string(j) + "]", pointBus_, issues, write);
            if (write && refs[j].index != 0 && !writeIndexes.insert(refs[j].index).second) {
                issues.push_back({"error", prefix, "duplicate PCS write target index: " + std::to_string(refs[j].index)});
            }
        }
        if (pcs.points.activeTargets.empty()) {
            issues.push_back({"error", prefix + ".points.activeTargets", "at least one active power target is required"});
        }
        if (config_.avc.enabled && pcs.points.reactiveTargets.empty()) {
            issues.push_back({"error", prefix + ".points.reactiveTargets", "AVC requires at least one reactive power target"});
        }
    }

    const std::vector<std::pair<std::string, AgcAvcPointRefConfig>> outputRefs = {
        {"state", config_.outputs.state},
        {"ownershipState", config_.outputs.ownershipState},
        {"agcEffectiveTarget", config_.outputs.agcEffectiveTarget},
        {"agcError", config_.outputs.agcError},
        {"avcEffectiveTarget", config_.outputs.avcEffectiveTarget},
        {"avcError", config_.outputs.avcError},
        {"availableActivePower", config_.outputs.availableActivePower},
        {"availableReactivePower", config_.outputs.availableReactivePower},
        {"unservedActivePower", config_.outputs.unservedActivePower},
        {"unservedReactivePower", config_.outputs.unservedReactivePower},
        {"lastCommandStatus", config_.outputs.lastCommandStatus},
        {"lastWriteStatus", config_.outputs.lastWriteStatus},
        {"consecutiveWriteFailures", config_.outputs.consecutiveWriteFailures},
    };
    for (const auto& entry : outputRefs) {
        validateRef(entry.second, "agcAvc.outputs." + entry.first, pointBus_, issues);
    }
    return issues;
}

std::vector<std::uint32_t> AgcAvcService::collectInputIndexes() const {
    std::vector<std::uint32_t> indexes;
    appendRefIndex(indexes, config_.interlocks.remoteEnable);
    appendRefIndex(indexes, config_.interlocks.emergencyStop);
    appendRefIndex(indexes, config_.interlocks.fireAlarm);
    appendRefIndex(indexes, config_.agc.target);
    appendRefIndex(indexes, config_.agc.commandSequence);
    appendRefIndex(indexes, config_.agc.commandTimestamp);
    appendRefIndex(indexes, config_.agc.pccActivePower);
    appendRefIndex(indexes, config_.agc.frequency);
    appendRefIndex(indexes, config_.avc.mode);
    appendRefIndex(indexes, config_.avc.targetQ);
    appendRefIndex(indexes, config_.avc.targetVoltage);
    appendRefIndex(indexes, config_.avc.targetPowerFactor);
    appendRefIndex(indexes, config_.avc.pccReactivePower);
    appendRefIndex(indexes, config_.avc.pccVoltage);
    appendRefIndex(indexes, config_.avc.pccPowerFactor);
    for (const auto& pcs : config_.pcs) {
        appendPcsInputIndexes(indexes, pcs);
    }
    std::sort(indexes.begin(), indexes.end());
    indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
    return indexes;
}

AgcAvcCycleOutput AgcAvcService::tick(std::int64_t nowMs) {
    const auto snapshot = pointBus_.readSnapshot(collectInputIndexes(), nowMs);
    std::unordered_map<std::uint32_t, StoredPointValue> values;
    values.reserve(snapshot.size());
    for (const auto& value : snapshot) {
        values[value.index] = value;
    }

    AgcAvcCycleInput input;
    input.nowMs = nowMs;
    bool healthy = true;
    std::string failureReason;
    input.remoteEnable = readValue(config_.interlocks.remoteEnable, values, nowMs, config_.inputMaxAgeMs, healthy, failureReason, nullptr, 1.0) > 0.5;
    input.emergencyStop = readValue(config_.interlocks.emergencyStop, values, nowMs, config_.inputMaxAgeMs, healthy, failureReason) > 0.5;
    input.fireAlarm = readValue(config_.interlocks.fireAlarm, values, nowMs, config_.inputMaxAgeMs, healthy, failureReason) > 0.5;
    input.command.targetPkw = readValue(config_.agc.target, values, nowMs, config_.commandTimeoutMs, healthy, failureReason);
    input.command.sequence = static_cast<std::int64_t>(std::llround(readValue(
        config_.agc.commandSequence,
        values,
        nowMs,
        config_.commandTimeoutMs,
        healthy,
        failureReason
    )));
    input.command.issuedAtMs = static_cast<std::int64_t>(std::llround(readValue(
        config_.agc.commandTimestamp,
        values,
        nowMs,
        config_.commandTimeoutMs,
        healthy,
        failureReason
    )));
    input.pccActivePowerKw = readValue(config_.agc.pccActivePower, values, nowMs, config_.inputMaxAgeMs, healthy, failureReason);
    input.frequencyHz = readValue(config_.agc.frequency, values, nowMs, config_.inputMaxAgeMs, healthy, failureReason, &input.frequencyValid, 50.0);
    bool avcModeValid = false;
    const auto avcMode = static_cast<int>(std::llround(readValue(
        config_.avc.mode,
        values,
        nowMs,
        config_.commandTimeoutMs,
        healthy,
        failureReason,
        &avcModeValid,
        0.0
    )));
    if (avcModeValid) {
        input.command.avcMode = avcMode == 1 ? "voltage" : (avcMode == 2 ? "powerFactor" : "reactivePower");
    }
    input.command.targetQkvar = readValue(config_.avc.targetQ, values, nowMs, config_.commandTimeoutMs, healthy, failureReason);
    input.command.targetVoltageV = readValue(config_.avc.targetVoltage, values, nowMs, config_.commandTimeoutMs, healthy, failureReason);
    input.command.targetPowerFactor = readValue(config_.avc.targetPowerFactor, values, nowMs, config_.commandTimeoutMs, healthy, failureReason, nullptr, 1.0);
    const auto observedCommand = input.command;
    const auto sequenceSnapshot = pointBus_.readSnapshot({config_.agc.commandSequence.index}, nowMs);
    bool sequenceStable = false;
    if (!sequenceSnapshot.empty() && pointUsable(sequenceSnapshot.front(), nowMs, config_.commandTimeoutMs)) {
        const auto confirmedSequence = static_cast<std::int64_t>(std::llround(sequenceSnapshot.front().value));
        sequenceStable = confirmedSequence == observedCommand.sequence;
    }
    if (!sequenceStable) {
        healthy = false;
        failureReason = "dispatch command changed while the shared-memory snapshot was being read";
    } else if (observedCommand.sequence > 0 &&
               (!commandLatched_ || observedCommand.sequence > latchedCommand_.sequence)) {
        latchedCommand_ = observedCommand;
        commandLatched_ = true;
    }
    if (commandLatched_) {
        input.command = latchedCommand_;
    }
    input.pccReactivePowerKvar = readValue(config_.avc.pccReactivePower, values, nowMs, config_.inputMaxAgeMs, healthy, failureReason);
    input.pccVoltageV = readValue(config_.avc.pccVoltage, values, nowMs, config_.inputMaxAgeMs, healthy, failureReason);
    input.pccPowerFactor = readValue(config_.avc.pccPowerFactor, values, nowMs, config_.inputMaxAgeMs, healthy, failureReason, nullptr, 1.0);
    input.priorityBlocked = priorityLease_.isBlocked(nowMs, "agc-avc");

    input.pcs.reserve(config_.pcs.size());
    for (const auto& pcsConfig : config_.pcs) {
        AgcAvcPcsRuntimeInput pcs;
        pcs.meterCode = pcsConfig.meterCode;
        bool pcsHealthy = true;
        std::string pcsFailure;
        pcs.online = readValue(pcsConfig.points.online, values, nowMs, config_.inputMaxAgeMs, pcsHealthy, pcsFailure) > 0.5;
        pcs.ready = readValue(pcsConfig.points.ready, values, nowMs, config_.inputMaxAgeMs, pcsHealthy, pcsFailure) > 0.5;
        pcs.soc = readValue(pcsConfig.points.soc, values, nowMs, config_.inputMaxAgeMs, pcsHealthy, pcsFailure);
        pcs.actualPkw = readValue(pcsConfig.points.actualP, values, nowMs, config_.inputMaxAgeMs, pcsHealthy, pcsFailure);
        pcs.actualQkvar = readValue(pcsConfig.points.actualQ, values, nowMs, config_.inputMaxAgeMs, pcsHealthy, pcsFailure);
        pcs.chargeAllowed = readValue(pcsConfig.points.chargeAllowed, values, nowMs, config_.inputMaxAgeMs, pcsHealthy, pcsFailure, nullptr, 1.0) > 0.5;
        pcs.dischargeAllowed = readValue(pcsConfig.points.dischargeAllowed, values, nowMs, config_.inputMaxAgeMs, pcsHealthy, pcsFailure, nullptr, 1.0) > 0.5;
        pcs.availableChargeKw = readValue(pcsConfig.points.availableChargeKw, values, nowMs, config_.inputMaxAgeMs, pcsHealthy, pcsFailure, &pcs.hasAvailableCharge);
        pcs.availableDischargeKw = readValue(pcsConfig.points.availableDischargeKw, values, nowMs, config_.inputMaxAgeMs, pcsHealthy, pcsFailure, &pcs.hasAvailableDischarge);
        pcs.availableReactiveKvar = readValue(pcsConfig.points.dynamicReactiveLimitKvar, values, nowMs, config_.inputMaxAgeMs, pcsHealthy, pcsFailure, &pcs.hasAvailableReactive);
        pcs.temperatureC = readValue(
            pcsConfig.points.temperature,
            values,
            nowMs,
            config_.inputMaxAgeMs,
            pcsHealthy,
            pcsFailure,
            &pcs.hasTemperature
        );
        pcs.maxAcCurrentA = readMaxAbsValue(
            pcsConfig.points.acCurrent,
            values,
            nowMs,
            config_.inputMaxAgeMs,
            pcsHealthy,
            pcsFailure,
            &pcs.hasAcCurrent
        );
        if (pcsConfig.ratedActivePowerKw.hasSourcePoint) {
            pcs.collectedRatedActiveKw = readValue(pcsConfig.ratedActivePowerKw.sourcePoint, values, nowMs, config_.inputMaxAgeMs, pcsHealthy, pcsFailure, &pcs.hasCollectedRatedActive);
        }
        if (pcsConfig.ratedApparentPowerKva.hasSourcePoint) {
            pcs.collectedRatedApparentKva = readValue(pcsConfig.ratedApparentPowerKva.sourcePoint, values, nowMs, config_.inputMaxAgeMs, pcsHealthy, pcsFailure, &pcs.hasCollectedRatedApparent);
        }
        pcs.inputHealthy = pcsHealthy;
        input.pcs.push_back(std::move(pcs));
    }
    input.inputHealthy = healthy;
    input.inputFailureReason = failureReason;

    processWriteResults(nowMs, input);
    if (consecutiveWriteFailures_ >= config_.maxConsecutiveWriteFailures) {
        input.inputHealthy = false;
        input.inputFailureReason = "writeback or PCS feedback failed repeatedly";
    }

    lastOutput_ = controller_.step(input);
    lastOutput_.lastWriteStatus = lastWriteStatus_;
    lastOutput_.consecutiveWriteFailures = consecutiveWriteFailures_;
    const bool activeControl = lastOutput_.state == AgcAvcRuntimeState::Active ||
        lastOutput_.state == AgcAvcRuntimeState::Degraded;
    const bool zeroingControl = lastOutput_.state == AgcAvcRuntimeState::RampToZero ||
        lastOutput_.state == AgcAvcRuntimeState::Standby ||
        lastOutput_.state == AgcAvcRuntimeState::Failsafe;
    const bool hasNonZeroSubmitted = std::any_of(
        lastSubmittedValue_.begin(),
        lastSubmittedValue_.end(),
        [](const auto& entry) { return std::abs(entry.second) > 1e-9; }
    );
    const bool needsOwnership = config_.submitWrites && !config_.shadowMode &&
        (activeControl || (ownershipHeld_ && zeroingControl && (hasNonZeroSubmitted || !pendingWrites_.empty())));
    if (needsOwnership) {
        ownershipHeld_ = powerOwnership_.acquire(
            config_.ownership.scope,
            ownershipSessionId_,
            ownershipTargetIndexes_,
            nowMs,
            config_.ownership.ttlMs
        );
        if (!ownershipHeld_) {
            lastOutput_.state = AgcAvcRuntimeState::PausedByPriority;
            lastOutput_.reason = "PCS power targets are owned by another controller";
        }
    } else if (ownershipHeld_) {
        powerOwnership_.release(ownershipSessionId_);
        ownershipHeld_ = false;
    }
    publishOutputs(lastOutput_, nowMs);
    if (config_.submitWrites && !config_.shadowMode && ownershipHeld_ &&
        lastOutput_.state != AgcAvcRuntimeState::PausedByPriority) {
        submitAssignments(lastOutput_, nowMs);
    }
    return lastOutput_;
}

void AgcAvcService::registerWriteFailure(std::uint32_t index) {
    lastWriteStatus_ = -1;
    ++consecutiveWriteFailures_;
    if (index != 0) {
        lastSubmittedValue_.erase(index);
        lastSubmittedAtMs_.erase(index);
    }
}

void AgcAvcService::processWriteResults(std::int64_t nowMs, const AgcAvcCycleInput& input) {
    const int failuresBefore = consecutiveWriteFailures_;
    bool successObserved = false;
    for (auto it = pendingWrites_.begin(); it != pendingWrites_.end();) {
        const auto result = pointBus_.readWriteResult(it->second.route, it->second.cmdId);
        if (result) {
            if (result->success) {
                successObserved = true;
                const auto& pending = it->second;
                if (pending.pcsIndex < config_.pcs.size() &&
                    config_.pcs[pending.pcsIndex].feedbackTimeoutMs > 0) {
                    pendingFeedback_.erase(
                        std::remove_if(
                            pendingFeedback_.begin(),
                            pendingFeedback_.end(),
                            [&](const PendingFeedbackObservation& item) {
                                return item.pcsIndex == pending.pcsIndex && item.reactive == pending.reactive;
                            }
                        ),
                        pendingFeedback_.end()
                    );
                    pendingFeedback_.push_back({
                        pending.pcsIndex,
                        pending.reactive,
                        pending.target,
                        result->completedAt > 0 ? result->completedAt : nowMs
                    });
                }
            } else {
                registerWriteFailure(it->first);
            }
            it = pendingWrites_.erase(it);
            continue;
        }
        if (nowMs - it->second.submittedAtMs >= config_.writeResultTimeoutMs) {
            registerWriteFailure(it->first);
            it = pendingWrites_.erase(it);
            continue;
        }
        ++it;
    }

    for (auto it = pendingFeedback_.begin(); it != pendingFeedback_.end();) {
        if (it->pcsIndex >= input.pcs.size() || it->pcsIndex >= config_.pcs.size()) {
            it = pendingFeedback_.erase(it);
            continue;
        }
        const auto& runtime = input.pcs[it->pcsIndex];
        const auto& pcs = config_.pcs[it->pcsIndex];
        const double actual = it->reactive ? runtime.actualQkvar : runtime.actualPkw;
        const double tolerance = it->reactive ? pcs.feedbackToleranceKvar : pcs.feedbackToleranceKw;
        if (runtime.inputHealthy && std::abs(actual - it->target) <= tolerance) {
            successObserved = true;
            it = pendingFeedback_.erase(it);
            continue;
        }
        if (nowMs - it->startedAtMs >= pcs.feedbackTimeoutMs) {
            const auto& targets = it->reactive ? pcs.points.reactiveTargets : pcs.points.activeTargets;
            registerWriteFailure(targets.empty() ? 0 : targets.front().index);
            it = pendingFeedback_.erase(it);
            continue;
        }
        ++it;
    }

    if (successObserved && consecutiveWriteFailures_ == failuresBefore) {
        consecutiveWriteFailures_ = 0;
        lastWriteStatus_ = 1;
    }
}

void AgcAvcService::publishOutputs(const AgcAvcCycleOutput& output, std::int64_t nowMs) {
    const std::vector<std::pair<AgcAvcPointRefConfig, double>> values = {
        {config_.outputs.state, static_cast<double>(output.state)},
        {config_.outputs.ownershipState, ownershipHeld_ ? 1.0 : 0.0},
        {config_.outputs.agcEffectiveTarget, output.effectiveTargetPkw},
        {config_.outputs.agcError, output.agcErrorKw},
        {config_.outputs.avcEffectiveTarget, output.effectiveTargetQkvar},
        {config_.outputs.avcError, output.avcError},
        {config_.outputs.availableActivePower, output.availableActivePowerKw},
        {config_.outputs.availableReactivePower, output.availableReactivePowerKvar},
        {config_.outputs.unservedActivePower, output.unservedActivePowerKw},
        {config_.outputs.unservedReactivePower, output.unservedReactivePowerKvar},
        {config_.outputs.lastCommandStatus, output.commandSequence > 0 ? 1.0 : 0.0},
        {config_.outputs.lastWriteStatus, static_cast<double>(output.lastWriteStatus)},
        {config_.outputs.consecutiveWriteFailures, static_cast<double>(output.consecutiveWriteFailures)},
    };
    for (const auto& entry : values) {
        if (entry.first.index == 0) {
            continue;
        }
        PointValue value;
        value.index = entry.first.index;
        value.value = entry.second;
        value.quality = 1;
        value.ts = nowMs;
        value.expireAt = nowMs + std::max(config_.cycleMs * 5, 1000);
        pointBus_.publishVirtual(std::move(value));
    }
}

void AgcAvcService::submitAssignments(const AgcAvcCycleOutput& output, std::int64_t nowMs) {
    if (output.pcs.empty()) {
        return;
    }
    for (std::size_t i = 0; i < output.pcs.size() && i < config_.pcs.size(); ++i) {
        const auto& assignment = output.pcs[i];
        const auto& pcs = config_.pcs[i];
        if (!assignment.available) {
            continue;
        }
        const auto submitTargets = [&](
            const std::vector<AgcAvcPointRefConfig>& targets,
            double total,
            double deadband,
            double rated,
            bool reactive
        ) {
            if (targets.empty()) {
                return;
            }
            for (const auto& target : targets) {
                if (target.index == 0 || pendingWrites_.count(target.index) != 0) {
                    continue;
                }
                double value = total;
                if (pcs.setpointType == "ratedPercent") {
                    value = rated > 0.0 ? total * 100.0 / rated : 0.0;
                } else if (targets.size() > 1) {
                    value = total / static_cast<double>(targets.size());
                }
                const auto lastValue = lastSubmittedValue_.find(target.index);
                const auto lastAt = lastSubmittedAtMs_.find(target.index);
                if (lastValue != lastSubmittedValue_.end() && std::abs(lastValue->second - value) < deadband) {
                    continue;
                }
                if (lastAt != lastSubmittedAtMs_.end() && nowMs - lastAt->second < pcs.minWriteIntervalMs) {
                    continue;
                }
                PendingWriteCommand command;
                command.cmdId = makeCommandId(output.commandSequence, nowMs, target.index);
                command.index = target.index;
                command.value = value;
                command.source = "agc-avc";
                command.ts = nowMs;
                command.highPriority = false;
                const auto result = pointBus_.submitControl(command);
                if (result.accepted) {
                    lastSubmittedValue_[target.index] = value;
                    lastSubmittedAtMs_[target.index] = nowMs;
                    pendingWrites_[target.index] = PendingWriteObservation{
                        result.route,
                        command.cmdId,
                        i,
                        reactive,
                        total,
                        nowMs
                    };
                }
            }
        };
        submitTargets(
            pcs.points.activeTargets,
            assignment.targetPkw,
            std::max(0.0, pcs.writeDeadbandKw),
            pcs.ratedActivePowerKw.commissionedLimit,
            false
        );
        submitTargets(
            pcs.points.reactiveTargets,
            assignment.targetQkvar,
            std::max(0.0, pcs.writeDeadbandKvar),
            pcs.ratedApparentPowerKva.commissionedLimit,
            true
        );
    }
}

const AgcAvcCycleOutput& AgcAvcService::lastOutput() const {
    return lastOutput_;
}

}  // namespace edge_gateway
