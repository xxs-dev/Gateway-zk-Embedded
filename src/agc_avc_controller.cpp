#include "edge_gateway/agc_avc_controller.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace edge_gateway {

namespace {

double clampValue(double value, double low, double high) {
    if (low > high) {
        std::swap(low, high);
    }
    return std::max(low, std::min(value, high));
}

double positiveLimit(double primary, double fallback) {
    return primary > 0.0 ? primary : std::max(0.0, fallback);
}

double rateLimit(double previous, double requested, double risePerSec, double fallPerSec, double dtSec) {
    if (dtSec <= 0.0) {
        return requested;
    }
    const double up = std::max(0.0, risePerSec) * dtSec;
    const double down = std::max(0.0, fallPerSec) * dtSec;
    if (requested > previous) {
        return std::min(requested, previous + up);
    }
    return std::max(requested, previous - down);
}

double effectiveCapability(const AgcAvcCapabilityValueConfig& config, double collected, bool hasCollected) {
    const double commissioned = std::max(0.0, config.commissionedLimit);
    if (!config.hasSourcePoint || !hasCollected || !std::isfinite(collected) || collected <= 0.0) {
        return commissioned;
    }
    if (config.combinePolicy == "source") {
        return collected;
    }
    if (commissioned <= 0.0) {
        return collected;
    }
    return std::min(commissioned, collected);
}

std::vector<double> allocateByCapacity(
    double total,
    const std::vector<double>& capacities,
    const std::vector<double>& weights
) {
    std::vector<double> result(capacities.size(), 0.0);
    double remaining = std::abs(total);
    if (remaining <= 1e-9 || capacities.empty()) {
        return result;
    }

    std::vector<bool> active(capacities.size(), false);
    for (std::size_t i = 0; i < capacities.size(); ++i) {
        active[i] = capacities[i] > 1e-9 && weights[i] > 0.0;
    }

    for (std::size_t pass = 0; pass < capacities.size() + 1 && remaining > 1e-9; ++pass) {
        double weightSum = 0.0;
        for (std::size_t i = 0; i < active.size(); ++i) {
            if (active[i]) {
                weightSum += weights[i];
            }
        }
        if (weightSum <= 1e-12) {
            break;
        }

        double distributed = 0.0;
        const double roundRemaining = remaining;
        for (std::size_t i = 0; i < active.size(); ++i) {
            if (!active[i]) {
                continue;
            }
            const double room = std::max(0.0, capacities[i] - result[i]);
            const double share = roundRemaining * weights[i] / weightSum;
            const double applied = std::min(room, share);
            result[i] += applied;
            distributed += applied;
            if (room - applied <= 1e-9) {
                active[i] = false;
            }
        }
        if (distributed <= 1e-9) {
            break;
        }
        remaining -= distributed;
    }

    if (total < 0.0) {
        for (auto& value : result) {
            value = -value;
        }
    }
    return result;
}

std::vector<double> allocateWithMinimum(
    double total,
    const std::vector<double>& capacities,
    const std::vector<double>& minimums,
    const std::vector<double>& weights
) {
    auto effectiveCapacities = capacities;
    for (std::size_t pass = 0; pass <= capacities.size(); ++pass) {
        auto result = allocateByCapacity(total, effectiveCapacities, weights);
        std::size_t belowMinimum = result.size();
        double smallestAllocation = std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < result.size(); ++i) {
            const double magnitude = std::abs(result[i]);
            const double minimum = i < minimums.size() ? std::max(0.0, minimums[i]) : 0.0;
            if (magnitude > 1e-9 && magnitude + 1e-9 < minimum && magnitude < smallestAllocation) {
                belowMinimum = i;
                smallestAllocation = magnitude;
            }
        }
        if (belowMinimum == result.size()) {
            return result;
        }
        effectiveCapacities[belowMinimum] = 0.0;
    }
    return std::vector<double>(capacities.size(), 0.0);
}

double risingDerateFactor(double value, double stop, double full) {
    if (full <= stop) {
        return 1.0;
    }
    return clampValue((value - stop) / (full - stop), 0.0, 1.0);
}

double fallingDerateFactor(double value, double full, double stop) {
    if (stop <= full) {
        return 1.0;
    }
    return clampValue((stop - value) / (stop - full), 0.0, 1.0);
}

double apparentLimit(const AgcAvcStationLimitsConfig& limits) {
    double value = positiveLimit(limits.ratedApparentPowerKva, limits.ratedActivePowerKw);
    if (limits.transformerRatedKva > 0.0) {
        value = value > 0.0 ? std::min(value, limits.transformerRatedKva) : limits.transformerRatedKva;
    }
    return value;
}

void applyPqEnvelope(double& p, double& q, double s, const std::string& priority) {
    if (s <= 0.0 || p * p + q * q <= s * s + 1e-9) {
        return;
    }
    if (priority == "reactivePowerFirst") {
        q = clampValue(q, -s, s);
        p = std::copysign(std::sqrt(std::max(0.0, s * s - q * q)), p);
        return;
    }
    if (priority == "proportional") {
        const double magnitude = std::sqrt(p * p + q * q);
        if (magnitude > 1e-9) {
            const double scale = s / magnitude;
            p *= scale;
            q *= scale;
        }
        return;
    }
    p = clampValue(p, -s, s);
    q = std::copysign(std::sqrt(std::max(0.0, s * s - p * p)), q);
}

std::string avcModeFromCommand(const AgcAvcCommandInput& command, const AvcLoopConfig& config) {
    return command.avcMode.empty() ? config.defaultMode : command.avcMode;
}

}  // namespace

const char* agcAvcRuntimeStateName(AgcAvcRuntimeState state) {
    switch (state) {
        case AgcAvcRuntimeState::Disabled: return "DISABLED";
        case AgcAvcRuntimeState::Standby: return "STANDBY";
        case AgcAvcRuntimeState::Active: return "ACTIVE";
        case AgcAvcRuntimeState::Degraded: return "DEGRADED";
        case AgcAvcRuntimeState::PausedByPriority: return "PAUSED_BY_PRIORITY";
        case AgcAvcRuntimeState::RampToZero: return "RAMP_TO_ZERO";
        case AgcAvcRuntimeState::Failsafe: return "FAILSAFE";
    }
    return "UNKNOWN";
}

AgcAvcController::AgcAvcController(AgcAvcConfig config)
    : config_(std::move(config)),
      previousPcsPkw_(config_.pcs.size(), 0.0),
      previousPcsQkvar_(config_.pcs.size(), 0.0) {
}

const AgcAvcConfig& AgcAvcController::config() const {
    return config_;
}

void AgcAvcController::reset() {
    lastCycleMs_ = 0;
    lastAvcCalculationMs_ = 0;
    integralP_ = 0.0;
    integralQ_ = 0.0;
    integralV_ = 0.0;
    previousPkw_ = 0.0;
    previousQkvar_ = 0.0;
    heldRawQkvar_ = 0.0;
    lastAvcError_ = 0.0;
    previousPcsPkw_.assign(config_.pcs.size(), 0.0);
    previousPcsQkvar_.assign(config_.pcs.size(), 0.0);
}

AgcAvcCycleOutput AgcAvcController::step(const AgcAvcCycleInput& input) {
    AgcAvcCycleOutput output;
    output.commandSequence = input.command.sequence;
    output.commandAgeMs = input.command.issuedAtMs > 0
        ? static_cast<double>(input.nowMs - input.command.issuedAtMs)
        : std::numeric_limits<double>::infinity();

    const double dtSec = lastCycleMs_ > 0 && input.nowMs > lastCycleMs_
        ? static_cast<double>(input.nowMs - lastCycleMs_) / 1000.0
        : static_cast<double>(std::max(1, config_.cycleMs)) / 1000.0;
    lastCycleMs_ = input.nowMs;

    const auto setZeroAssignments = [&](bool immediate) {
        output.pcs.clear();
        output.pcs.reserve(config_.pcs.size());
        double totalP = 0.0;
        double totalQ = 0.0;
        for (std::size_t i = 0; i < config_.pcs.size(); ++i) {
            const auto& pcsConfig = config_.pcs[i];
            AgcAvcPcsAssignment assignment;
            assignment.meterCode = pcsConfig.meterCode;
            assignment.available = pcsConfig.enabled;
            assignment.targetPkw = immediate
                ? 0.0
                : rateLimit(previousPcsPkw_[i], 0.0, pcsConfig.riseKwPerSec, pcsConfig.fallKwPerSec, dtSec);
            assignment.targetQkvar = immediate
                ? 0.0
                : rateLimit(previousPcsQkvar_[i], 0.0, pcsConfig.riseKvarPerSec, pcsConfig.fallKvarPerSec, dtSec);
            previousPcsPkw_[i] = assignment.targetPkw;
            previousPcsQkvar_[i] = assignment.targetQkvar;
            totalP += assignment.targetPkw;
            totalQ += assignment.targetQkvar;
            output.pcs.push_back(std::move(assignment));
        }
        output.effectiveTargetPkw = totalP;
        output.effectiveTargetQkvar = totalQ;
        previousPkw_ = totalP;
        previousQkvar_ = totalQ;
    };

    if (!config_.enabled) {
        output.state = AgcAvcRuntimeState::Disabled;
        output.reason = "module disabled";
        reset();
        lastCycleMs_ = input.nowMs;
        return output;
    }

    if (!input.inputHealthy) {
        output.state = AgcAvcRuntimeState::Failsafe;
        output.reason = input.inputFailureReason.empty() ? "required input invalid" : input.inputFailureReason;
        integralP_ = integralQ_ = integralV_ = 0.0;
        setZeroAssignments(true);
        return output;
    }
    if (input.emergencyStop || input.fireAlarm || !input.remoteEnable) {
        output.state = AgcAvcRuntimeState::Failsafe;
        output.reason = input.emergencyStop
            ? "emergency stop active"
            : (input.fireAlarm ? "fire alarm active" : "remote control disabled");
        integralP_ = integralQ_ = integralV_ = 0.0;
        setZeroAssignments(true);
        return output;
    }
    if (input.priorityBlocked) {
        output.state = AgcAvcRuntimeState::PausedByPriority;
        output.reason = "paused by high priority control";
        integralP_ = integralQ_ = integralV_ = 0.0;
        double actualP = 0.0;
        double actualQ = 0.0;
        for (std::size_t i = 0; i < config_.pcs.size(); ++i) {
            if (i < input.pcs.size() && input.pcs[i].inputHealthy) {
                previousPcsPkw_[i] = input.pcs[i].actualPkw;
                previousPcsQkvar_[i] = input.pcs[i].actualQkvar;
                actualP += input.pcs[i].actualPkw;
                actualQ += input.pcs[i].actualQkvar;
            }
        }
        previousPkw_ = actualP;
        previousQkvar_ = actualQ;
        return output;
    }

    const bool commandFresh = input.command.sequence > 0 &&
        input.command.issuedAtMs > 0 &&
        output.commandAgeMs >= -1000.0 &&
        output.commandAgeMs <= static_cast<double>(config_.commandTimeoutMs);
    if (!commandFresh) {
        setZeroAssignments(false);
        output.state = std::abs(output.effectiveTargetPkw) > 1e-6 || std::abs(output.effectiveTargetQkvar) > 1e-6
            ? AgcAvcRuntimeState::RampToZero
            : AgcAvcRuntimeState::Standby;
        output.reason = input.command.sequence <= 0 ? "waiting for dispatch command" : "dispatch command expired";
        integralP_ = integralQ_ = integralV_ = 0.0;
        return output;
    }

    double requestedP = 0.0;
    if (config_.agc.enabled) {
        double error = input.command.targetPkw - input.pccActivePowerKw;
        if (std::abs(error) <= std::max(0.0, config_.agc.deadbandKw)) {
            error = 0.0;
        }
        integralP_ = clampValue(
            integralP_ + error * dtSec,
            config_.agc.integralMinKw,
            config_.agc.integralMaxKw
        );
        requestedP = input.command.targetPkw + config_.agc.kp * error + config_.agc.ki * integralP_;
        if (config_.agc.frequencyDroopEnabled && input.frequencyValid) {
            const double droop = clampValue(
                config_.agc.kwPerHz * (config_.agc.nominalHz - input.frequencyHz),
                -std::abs(config_.agc.droopLimitKw),
                std::abs(config_.agc.droopLimitKw)
            );
            requestedP += droop;
        }
        output.agcErrorKw = error;
    }
    output.rawTargetPkw = requestedP;

    double requestedQ = heldRawQkvar_;
    const bool avcDue = lastAvcCalculationMs_ <= 0 ||
        input.nowMs - lastAvcCalculationMs_ >= std::max(config_.cycleMs, config_.avcCycleMs);
    if (config_.avc.enabled && avcDue) {
        const double avcDtSec = lastAvcCalculationMs_ > 0 && input.nowMs > lastAvcCalculationMs_
            ? static_cast<double>(input.nowMs - lastAvcCalculationMs_) / 1000.0
            : static_cast<double>(std::max(config_.cycleMs, config_.avcCycleMs)) / 1000.0;
        const auto mode = avcModeFromCommand(input.command, config_.avc);
        if (mode == "voltage") {
            double error = input.command.targetVoltageV - input.pccVoltageV;
            if (std::abs(error) <= std::max(0.0, config_.avc.voltageDeadbandV)) {
                error = 0.0;
            }
            integralV_ = clampValue(
                integralV_ + error * avcDtSec,
                config_.avc.integralMinKvar,
                config_.avc.integralMaxKvar
            );
            requestedQ = previousQkvar_ + config_.avc.kpV * error + config_.avc.kiV * integralV_;
            output.avcError = error;
        } else if (mode == "powerFactor") {
            const double pf = clampValue(std::abs(input.command.targetPowerFactor), 0.01, 1.0);
            const double qMagnitude = std::abs(input.pccActivePowerKw) * std::tan(std::acos(pf));
            requestedQ = std::copysign(qMagnitude, input.command.targetPowerFactor);
            output.avcError = input.command.targetPowerFactor - input.pccPowerFactor;
        } else {
            double error = input.command.targetQkvar - input.pccReactivePowerKvar;
            if (std::abs(error) <= std::max(0.0, config_.avc.deadbandKvar)) {
                error = 0.0;
            }
            integralQ_ = clampValue(
                integralQ_ + error * avcDtSec,
                config_.avc.integralMinKvar,
                config_.avc.integralMaxKvar
            );
            requestedQ = input.command.targetQkvar + config_.avc.kpQ * error + config_.avc.kiQ * integralQ_;
            output.avcError = error;
        }
        heldRawQkvar_ = requestedQ;
        lastAvcError_ = output.avcError;
        lastAvcCalculationMs_ = input.nowMs;
    } else if (!config_.avc.enabled) {
        requestedQ = 0.0;
        heldRawQkvar_ = 0.0;
        lastAvcError_ = 0.0;
    } else {
        output.avcError = lastAvcError_;
    }
    output.rawTargetQkvar = requestedQ;

    const double maxExport = positiveLimit(
        config_.stationLimits.maxExportPowerKw,
        positiveLimit(config_.agc.maxKw, config_.stationLimits.ratedActivePowerKw)
    );
    const double maxImport = positiveLimit(
        config_.stationLimits.maxImportPowerKw,
        positiveLimit(-config_.agc.minKw, config_.stationLimits.ratedActivePowerKw)
    );
    requestedP = clampValue(requestedP, -maxImport, maxExport);
    requestedP = clampValue(requestedP, config_.agc.minKw, config_.agc.maxKw);
    const double stationQ = positiveLimit(
        config_.stationLimits.maxReactivePowerKvar,
        std::max(std::abs(config_.avc.minKvar), std::abs(config_.avc.maxKvar))
    );
    requestedQ = clampValue(requestedQ, -stationQ, stationQ);
    requestedQ = clampValue(requestedQ, config_.avc.minKvar, config_.avc.maxKvar);
    applyPqEnvelope(requestedP, requestedQ, apparentLimit(config_.stationLimits), config_.pqPriority);
    requestedP = rateLimit(previousPkw_, requestedP, config_.agc.riseKwPerSec, config_.agc.fallKwPerSec, dtSec);
    requestedQ = rateLimit(previousQkvar_, requestedQ, config_.avc.riseKvarPerSec, config_.avc.fallKvarPerSec, dtSec);

    std::vector<double> pCapacity;
    std::vector<double> pMinimum;
    std::vector<double> qCapacity;
    std::vector<double> weights;
    std::vector<double> ratedApparent;
    output.pcs.reserve(config_.pcs.size());
    for (std::size_t i = 0; i < config_.pcs.size(); ++i) {
        const auto& pcsConfig = config_.pcs[i];
        const AgcAvcPcsRuntimeInput* runtime = i < input.pcs.size() ? &input.pcs[i] : nullptr;
        AgcAvcPcsAssignment assignment;
        assignment.meterCode = pcsConfig.meterCode;

        const double ratedP = effectiveCapability(
            pcsConfig.ratedActivePowerKw,
            runtime ? runtime->collectedRatedActiveKw : 0.0,
            runtime && runtime->hasCollectedRatedActive
        );
        const double ratedS = effectiveCapability(
            pcsConfig.ratedApparentPowerKva,
            runtime ? runtime->collectedRatedApparentKva : 0.0,
            runtime && runtime->hasCollectedRatedApparent
        );
        const bool available = pcsConfig.enabled && runtime != nullptr && runtime->inputHealthy && runtime->online && runtime->ready;
        assignment.available = available;
        if (!available) {
            assignment.reason = !pcsConfig.enabled ? "disabled" : "offline, not ready, or invalid input";
            pCapacity.push_back(0.0);
            pMinimum.push_back(0.0);
            qCapacity.push_back(0.0);
            weights.push_back(0.0);
            ratedApparent.push_back(0.0);
            output.pcs.push_back(std::move(assignment));
            continue;
        }

        double discharge = std::min(ratedP, positiveLimit(pcsConfig.maxDischargePowerKw, ratedP));
        double charge = std::min(ratedP, positiveLimit(pcsConfig.maxChargePowerKw, ratedP));
        if (runtime->hasAvailableDischarge) {
            discharge = std::min(discharge, std::max(0.0, runtime->availableDischargeKw));
        } else if (!pcsConfig.fallbackToStaticLimit && pcsConfig.deviceType != "gridTieInverter") {
            discharge = 0.0;
        }
        if (runtime->hasAvailableCharge) {
            charge = std::min(charge, std::max(0.0, runtime->availableChargeKw));
        } else if (!pcsConfig.fallbackToStaticLimit && pcsConfig.deviceType != "gridTieInverter") {
            charge = 0.0;
        }
        if (!runtime->dischargeAllowed) {
            discharge = 0.0;
        }
        if (!runtime->chargeAllowed) {
            charge = 0.0;
        }

        if (pcsConfig.deviceType != "gridTieInverter") {
            discharge *= risingDerateFactor(
                runtime->soc,
                pcsConfig.socDischargeStopPercent,
                pcsConfig.socDischargeDeratePercent
            );
            charge *= fallingDerateFactor(
                runtime->soc,
                pcsConfig.socChargeDeratePercent,
                pcsConfig.socChargeStopPercent
            );
        }

        double environmentalFactor = 1.0;
        if (runtime->hasTemperature) {
            environmentalFactor = std::min(environmentalFactor, fallingDerateFactor(
                runtime->temperatureC,
                pcsConfig.temperatureDerateStartC,
                pcsConfig.temperatureStopC
            ));
        }
        if (runtime->hasAcCurrent && pcsConfig.ratedAcCurrentA > 0.0 &&
            pcsConfig.currentDerateStartPercent > 0.0 && pcsConfig.currentStopPercent > 0.0) {
            const double currentPercent = runtime->maxAcCurrentA * 100.0 / pcsConfig.ratedAcCurrentA;
            environmentalFactor = std::min(environmentalFactor, fallingDerateFactor(
                currentPercent,
                pcsConfig.currentDerateStartPercent,
                pcsConfig.currentStopPercent
            ));
        }
        discharge *= environmentalFactor;
        charge *= environmentalFactor;

        double reactive = positiveLimit(pcsConfig.ratedReactivePowerKvar, ratedS);
        if (runtime->hasAvailableReactive) {
            reactive = std::min(reactive, std::max(0.0, runtime->availableReactiveKvar));
        }
        reactive *= environmentalFactor;
        assignment.availableChargeKw = charge;
        assignment.availableDischargeKw = discharge;
        assignment.availableReactiveKvar = reactive;
        const double selectedP = requestedP >= 0.0 ? discharge : charge;
        pCapacity.push_back(selectedP);
        pMinimum.push_back(requestedP >= 0.0
            ? pcsConfig.minStableDischargePowerKw
            : pcsConfig.minStableChargePowerKw);
        qCapacity.push_back(std::min(reactive, ratedS));
        weights.push_back(std::max(0.0, pcsConfig.weight) * std::max(selectedP, 1.0));
        ratedApparent.push_back(ratedS);
        output.availableActivePowerKw += selectedP;
        output.availableReactivePowerKvar += std::min(reactive, ratedS);
        output.pcs.push_back(std::move(assignment));
    }

    const auto pAllocated = allocateWithMinimum(requestedP, pCapacity, pMinimum, weights);
    std::vector<double> qAfterP(qCapacity.size(), 0.0);
    for (std::size_t i = 0; i < qAfterP.size(); ++i) {
        const double ratedS = ratedApparent[i];
        qAfterP[i] = std::min(qCapacity[i], std::sqrt(std::max(0.0, ratedS * ratedS - pAllocated[i] * pAllocated[i])));
    }
    const auto qAllocated = allocateByCapacity(requestedQ, qAfterP, weights);

    double deliveredP = 0.0;
    double deliveredQ = 0.0;
    for (std::size_t i = 0; i < output.pcs.size(); ++i) {
        if (!output.pcs[i].available) {
            previousPcsPkw_[i] = 0.0;
            previousPcsQkvar_[i] = 0.0;
            continue;
        }
        const auto& pcsConfig = config_.pcs[i];
        double targetP = rateLimit(
            previousPcsPkw_[i],
            pAllocated[i],
            pcsConfig.riseKwPerSec,
            pcsConfig.fallKwPerSec,
            dtSec
        );
        double targetQ = rateLimit(
            previousPcsQkvar_[i],
            qAllocated[i],
            pcsConfig.riseKvarPerSec,
            pcsConfig.fallKvarPerSec,
            dtSec
        );
        applyPqEnvelope(targetP, targetQ, ratedApparent[i], config_.pqPriority);
        output.pcs[i].targetPkw = targetP;
        output.pcs[i].targetQkvar = targetQ;
        previousPcsPkw_[i] = targetP;
        previousPcsQkvar_[i] = targetQ;
        deliveredP += targetP;
        deliveredQ += targetQ;
    }
    output.effectiveTargetPkw = deliveredP;
    output.effectiveTargetQkvar = deliveredQ;
    output.unservedActivePowerKw = requestedP - deliveredP;
    output.unservedReactivePowerKvar = requestedQ - deliveredQ;
    const bool degraded = std::abs(output.unservedActivePowerKw) > std::max(0.1, config_.agc.deadbandKw) ||
        std::abs(output.unservedReactivePowerKvar) > std::max(0.1, config_.avc.deadbandKvar);
    output.state = degraded ? AgcAvcRuntimeState::Degraded : AgcAvcRuntimeState::Active;
    output.reason = degraded ? "available PCS capability cannot fully satisfy target" : "dispatch target active";
    previousPkw_ = deliveredP;
    previousQkvar_ = deliveredQ;
    return output;
}

}  // namespace edge_gateway
