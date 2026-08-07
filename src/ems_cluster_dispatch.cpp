#include "edge_gateway/ems_cluster.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace edge_gateway {

namespace {

bool finitePhasePower(const EmsClusterPhasePower& value) {
    return std::isfinite(value.paKw) && std::isfinite(value.pbKw) &&
        std::isfinite(value.pcKw) && std::isfinite(value.qaKvar) &&
        std::isfinite(value.qbKvar) && std::isfinite(value.qcKvar);
}

double clampMagnitude(double value, double positiveLimit, double negativeLimit) {
    if (!std::isfinite(value)) return 0.0;
    return value >= 0.0
        ? std::min(value, std::max(0.0, positiveLimit))
        : std::max(value, -std::max(0.0, negativeLimit));
}

bool nearlyEqual(double lhs, double rhs) {
    return std::fabs(lhs - rhs) <= 1e-6;
}

bool samePower(const EmsClusterPhasePower& lhs, const EmsClusterPhasePower& rhs) {
    return nearlyEqual(lhs.paKw, rhs.paKw) && nearlyEqual(lhs.pbKw, rhs.pbKw) &&
        nearlyEqual(lhs.pcKw, rhs.pcKw) && nearlyEqual(lhs.qaKvar, rhs.qaKvar) &&
        nearlyEqual(lhs.qbKvar, rhs.qbKvar) && nearlyEqual(lhs.qcKvar, rhs.qcKvar);
}

bool usableCapability(const EmsClusterCapability& value) {
    return value.controlEnabled && value.ready && !value.interlocked && !value.manualOverride &&
        std::isfinite(value.socPercent) && std::isfinite(value.ratedActivePowerKw) &&
        std::isfinite(value.ratedApparentPowerKva) &&
        std::isfinite(value.availableChargePowerKw) &&
        std::isfinite(value.availableDischargePowerKw) &&
        std::isfinite(value.availableReactivePowerKvar) &&
        value.ratedActivePowerKw > 0.0 && value.ratedApparentPowerKva > 0.0;
}

struct AllocationCandidate {
    std::string nodeId;
    double capacity = 0.0;
    double weight = 1.0;
};

std::map<std::string, double> waterFill(
    double target,
    const std::vector<AllocationCandidate>& candidates
) {
    std::map<std::string, double> result;
    if (candidates.empty() || !std::isfinite(target) || nearlyEqual(target, 0.0)) return result;
    const double sign = target >= 0.0 ? 1.0 : -1.0;
    double remaining = std::fabs(target);
    std::map<std::string, double> remainingCapacity;
    for (const auto& candidate : candidates) {
        remainingCapacity[candidate.nodeId] = std::max(0.0, candidate.capacity);
        result[candidate.nodeId] = 0.0;
    }

    for (std::size_t pass = 0; pass < candidates.size() + 1 && remaining > 1e-6; ++pass) {
        double totalWeight = 0.0;
        for (const auto& candidate : candidates) {
            if (remainingCapacity[candidate.nodeId] > 1e-6) {
                totalWeight += std::max(0.01, candidate.weight);
            }
        }
        if (totalWeight <= 0.0) break;

        const double passTarget = remaining;
        double allocated = 0.0;
        for (const auto& candidate : candidates) {
            auto& capacity = remainingCapacity[candidate.nodeId];
            if (capacity <= 1e-6) continue;
            const auto share = passTarget * std::max(0.01, candidate.weight) / totalWeight;
            const auto amount = std::min(capacity, share);
            result[candidate.nodeId] += sign * amount;
            capacity -= amount;
            allocated += amount;
        }
        if (allocated <= 1e-6) break;
        remaining -= allocated;
    }
    return result;
}

double socWeight(const EmsClusterCapability& capability, bool discharge) {
    const auto soc = std::max(0.0, std::min(100.0, capability.socPercent)) / 100.0;
    return discharge ? 0.5 + soc : 1.5 - soc;
}

void allocateActivePhase(
    double target,
    double EmsClusterPhasePower::*field,
    const std::map<std::string, EmsClusterCapability>& capabilities,
    std::map<std::string, EmsClusterPhasePower>& result
) {
    std::vector<AllocationCandidate> candidates;
    const bool discharge = target >= 0.0;
    for (const auto& entry : capabilities) {
        if (!usableCapability(entry.second)) continue;
        const auto available = discharge
            ? entry.second.availableDischargePowerKw
            : entry.second.availableChargePowerKw;
        const auto capacity = std::min(entry.second.ratedActivePowerKw, std::max(0.0, available)) / 3.0;
        if (capacity > 0.0) {
            candidates.push_back({entry.first, capacity, socWeight(entry.second, discharge)});
        }
    }
    for (const auto& allocation : waterFill(target, candidates)) {
        result[allocation.first].*field = allocation.second;
    }
}

void allocateReactivePhase(
    double target,
    double EmsClusterPhasePower::*activeField,
    double EmsClusterPhasePower::*reactiveField,
    const std::map<std::string, EmsClusterCapability>& capabilities,
    std::map<std::string, EmsClusterPhasePower>& result
) {
    std::vector<AllocationCandidate> candidates;
    for (const auto& entry : capabilities) {
        if (!usableCapability(entry.second)) continue;
        const auto apparent = entry.second.ratedApparentPowerKva / 3.0;
        const auto active = result[entry.first].*activeField;
        const auto envelope = std::sqrt(std::max(0.0, apparent * apparent - active * active));
        const auto capacity = std::min(entry.second.availableReactivePowerKvar / 3.0, envelope);
        if (capacity > 0.0) candidates.push_back({entry.first, capacity, capacity});
    }
    for (const auto& allocation : waterFill(target, candidates)) {
        result[allocation.first].*reactiveField = allocation.second;
    }
}

}  // namespace

std::map<std::string, EmsClusterPhasePower> EmsClusterNode::allocateDispatch(
    const EmsClusterPhasePower& stationTarget,
    const std::map<std::string, EmsClusterCapability>& capabilities
) {
    std::map<std::string, EmsClusterPhasePower> result;
    for (const auto& entry : capabilities) result.emplace(entry.first, EmsClusterPhasePower{});
    if (!finitePhasePower(stationTarget)) return result;

    allocateActivePhase(stationTarget.paKw, &EmsClusterPhasePower::paKw, capabilities, result);
    allocateActivePhase(stationTarget.pbKw, &EmsClusterPhasePower::pbKw, capabilities, result);
    allocateActivePhase(stationTarget.pcKw, &EmsClusterPhasePower::pcKw, capabilities, result);
    allocateReactivePhase(
        stationTarget.qaKvar,
        &EmsClusterPhasePower::paKw,
        &EmsClusterPhasePower::qaKvar,
        capabilities,
        result
    );
    allocateReactivePhase(
        stationTarget.qbKvar,
        &EmsClusterPhasePower::pbKw,
        &EmsClusterPhasePower::qbKvar,
        capabilities,
        result
    );
    allocateReactivePhase(
        stationTarget.qcKvar,
        &EmsClusterPhasePower::pcKw,
        &EmsClusterPhasePower::qcKvar,
        capabilities,
        result
    );
    return result;
}

void EmsClusterNode::updateControlInputs(
    const EmsClusterCapability& capability,
    const EmsClusterPhasePower& stationTarget,
    bool stationTargetValid,
    std::int64_t nowMs
) {
    localCapability_ = capability;
    localCapabilityAtMs_ = nowMs;
    stationTarget_ = stationTarget;
    stationTargetValid_ = stationTargetValid && finitePhasePower(stationTarget);
    stationTargetAtMs_ = nowMs;
}

EmsClusterDispatchState EmsClusterNode::acceptDispatch(
    const EmsClusterPhasePower& requested,
    std::uint64_t sequence,
    int ttlMs,
    std::int64_t nowMs
) const {
    EmsClusterDispatchState result;
    result.term = currentTerm_;
    result.membershipEpoch = membershipEpoch_;
    result.sequence = sequence;
    result.receivedAtMs = nowMs;
    result.expireAtMs = nowMs + std::max(1, ttlMs);
    result.requested = requested;
    if (!config_.controlEnabled || !localCapability_.controlEnabled) {
        result.code = EmsClusterDispatchCode::ControlDisabled;
        return result;
    }
    if (!finitePhasePower(requested)) {
        result.code = EmsClusterDispatchCode::InvalidTarget;
        return result;
    }
    if (localCapabilityAtMs_ <= 0 || nowMs - localCapabilityAtMs_ > config_.capabilityTtlMs) {
        result.code = EmsClusterDispatchCode::CapabilityStale;
        return result;
    }
    if (!localCapability_.ready) {
        result.code = EmsClusterDispatchCode::NotReady;
        return result;
    }
    if (localCapability_.interlocked) {
        result.code = EmsClusterDispatchCode::Interlocked;
        return result;
    }
    if (localCapability_.manualOverride) {
        result.code = EmsClusterDispatchCode::ManualOverride;
        return result;
    }

    const auto dischargePerPhase = std::min(
        localCapability_.ratedActivePowerKw,
        localCapability_.availableDischargePowerKw
    ) / 3.0;
    const auto chargePerPhase = std::min(
        localCapability_.ratedActivePowerKw,
        localCapability_.availableChargePowerKw
    ) / 3.0;
    result.accepted.paKw = clampMagnitude(requested.paKw, dischargePerPhase, chargePerPhase);
    result.accepted.pbKw = clampMagnitude(requested.pbKw, dischargePerPhase, chargePerPhase);
    result.accepted.pcKw = clampMagnitude(requested.pcKw, dischargePerPhase, chargePerPhase);

    const auto reactiveLimit = [&](double active) {
        const auto apparent = std::max(0.0, localCapability_.ratedApparentPowerKva) / 3.0;
        const auto envelope = std::sqrt(std::max(0.0, apparent * apparent - active * active));
        return std::min(std::max(0.0, localCapability_.availableReactivePowerKvar) / 3.0, envelope);
    };
    const auto qaLimit = reactiveLimit(result.accepted.paKw);
    const auto qbLimit = reactiveLimit(result.accepted.pbKw);
    const auto qcLimit = reactiveLimit(result.accepted.pcKw);
    result.accepted.qaKvar = clampMagnitude(requested.qaKvar, qaLimit, qaLimit);
    result.accepted.qbKvar = clampMagnitude(requested.qbKvar, qbLimit, qbLimit);
    result.accepted.qcKvar = clampMagnitude(requested.qcKvar, qcLimit, qcLimit);
    result.code = samePower(result.requested, result.accepted)
        ? EmsClusterDispatchCode::Accepted
        : EmsClusterDispatchCode::Clamped;
    result.valid = true;
    return result;
}

void EmsClusterNode::invalidateDispatch(EmsClusterDispatchCode code) {
    localDispatch_.valid = false;
    localDispatch_.code = code;
    localDispatch_.accepted = {};
}

EmsClusterDispatchState EmsClusterNode::activeDispatch(std::int64_t nowMs) const {
    auto result = localDispatch_;
    if (result.valid && result.expireAtMs > 0 && nowMs >= result.expireAtMs) {
        result.valid = false;
        result.code = EmsClusterDispatchCode::Expired;
        result.accepted = {};
    }
    return result;
}

void EmsClusterNode::tickDispatch(std::int64_t nowMs) {
    if (localDispatch_.valid && localDispatch_.expireAtMs > 0 && nowMs >= localDispatch_.expireAtMs) {
        invalidateDispatch(EmsClusterDispatchCode::Expired);
    }

    if (role_ == EmsClusterRole::Follower && !leaderNodeId_.empty() && leaderLeaseValid(nowMs)) {
        if (lastCapabilityReportMs_ == 0 || nowMs - lastCapabilityReportMs_ >= config_.dispatchCycleMs) {
            auto report = baseMessage(EmsClusterMessageType::CapabilityReport);
            report.capability = localCapability_;
            outgoing_.push_back({std::move(report), leaderNodeId_, false});
            lastCapabilityReportMs_ = nowMs;
        }
        if (!config_.controlEnabled) {
            invalidateDispatch(EmsClusterDispatchCode::ControlDisabled);
            return;
        }
        if (localDispatch_.valid &&
            (lastFeedbackMs_ == 0 || nowMs - lastFeedbackMs_ >= config_.dispatchCycleMs)) {
            auto feedback = baseMessage(EmsClusterMessageType::Feedback);
            feedback.dispatchSequence = localDispatch_.sequence;
            feedback.dispatchCode = localDispatch_.code;
            feedback.acceptedPower = localDispatch_.accepted;
            feedback.capability = localCapability_;
            outgoing_.push_back({std::move(feedback), leaderNodeId_, false});
            lastFeedbackMs_ = nowMs;
        }
        return;
    }

    if (!config_.controlEnabled) {
        invalidateDispatch(EmsClusterDispatchCode::ControlDisabled);
        return;
    }

    if (role_ != EmsClusterRole::Leader) {
        invalidateDispatch(EmsClusterDispatchCode::NotLeader);
        return;
    }
    const bool quorumValid = lastQuorumMs_ > 0 && nowMs - lastQuorumMs_ <= config_.leaderLeaseMs;
    if (!quorumValid) {
        invalidateDispatch(EmsClusterDispatchCode::NoQuorum);
        return;
    }
    if (!stationTargetValid_ || stationTargetAtMs_ <= 0 ||
        nowMs - stationTargetAtMs_ > config_.stationTargetTtlMs) {
        invalidateDispatch(EmsClusterDispatchCode::Expired);
        return;
    }
    if (!localCapability_.controlEnabled) {
        invalidateDispatch(EmsClusterDispatchCode::ControlDisabled);
        return;
    }
    if (lastDispatchMs_ > 0 && nowMs - lastDispatchMs_ < config_.dispatchCycleMs) return;

    std::map<std::string, EmsClusterCapability> capabilities;
    if (localCapabilityAtMs_ > 0 && nowMs - localCapabilityAtMs_ <= config_.capabilityTtlMs &&
        isVotingMember(nodeId_)) {
        capabilities.emplace(nodeId_, localCapability_);
    }
    for (auto& entry : members_) {
        const bool fresh = entry.second.lastCapabilityMs > 0 &&
            nowMs - entry.second.lastCapabilityMs <= config_.capabilityTtlMs;
        entry.second.status.capabilityFresh = fresh;
        if (fresh && isVotingMember(entry.first)) {
            capabilities.emplace(entry.first, entry.second.status.capability);
        }
    }
    const auto allocations = allocateDispatch(stationTarget_, capabilities);
    if (allocations.empty()) {
        invalidateDispatch(EmsClusterDispatchCode::CapabilityStale);
        return;
    }

    const auto sequence = ++dispatchSequence_;
    for (const auto& allocation : allocations) {
        if (allocation.first == nodeId_) {
            localDispatch_ = acceptDispatch(
                allocation.second,
                sequence,
                config_.dispatchTtlMs,
                nowMs
            );
            continue;
        }
        auto target = baseMessage(EmsClusterMessageType::DispatchTarget);
        target.dispatchSequence = sequence;
        target.dispatchTtlMs = static_cast<std::uint32_t>(config_.dispatchTtlMs);
        target.requestedPower = allocation.second;
        outgoing_.push_back({std::move(target), allocation.first, false});
        auto found = members_.find(allocation.first);
        if (found != members_.end()) {
            auto& pending = found->second.status.dispatch;
            pending.valid = false;
            pending.term = currentTerm_;
            pending.membershipEpoch = membershipEpoch_;
            pending.sequence = sequence;
            pending.receivedAtMs = 0;
            pending.expireAtMs = nowMs + config_.dispatchTtlMs;
            pending.code = EmsClusterDispatchCode::Expired;
            pending.requested = allocation.second;
            pending.accepted = {};
        }
    }
    lastDispatchMs_ = nowMs;
}

void EmsClusterNode::handleCapabilityReport(const EmsClusterMessage& message, std::int64_t nowMs) {
    if (role_ != EmsClusterRole::Leader || message.term != currentTerm_ ||
        message.membershipEpoch != membershipEpoch_ || !isVotingMember(message.senderNodeId)) return;
    auto found = members_.find(message.senderNodeId);
    if (found == members_.end()) return;
    found->second.status.capability = message.capability;
    found->second.status.capabilityFresh = true;
    found->second.lastCapabilityMs = nowMs;
}

void EmsClusterNode::handleDispatchTarget(const EmsClusterMessage& message, std::int64_t nowMs) {
    EmsClusterDispatchState accepted;
    accepted.term = message.term;
    accepted.membershipEpoch = message.membershipEpoch;
    accepted.sequence = message.dispatchSequence;
    accepted.requested = message.requestedPower;
    accepted.receivedAtMs = nowMs;
    accepted.expireAtMs = nowMs + std::max<std::uint32_t>(1, message.dispatchTtlMs);

    if (role_ != EmsClusterRole::Follower || message.senderNodeId != leaderNodeId_) {
        accepted.code = EmsClusterDispatchCode::NotLeader;
    } else if (message.term != currentTerm_) {
        accepted.code = EmsClusterDispatchCode::TermMismatch;
    } else if (message.membershipEpoch != membershipEpoch_) {
        accepted.code = EmsClusterDispatchCode::MembershipMismatch;
    } else if (!leaderLeaseValid(nowMs)) {
        accepted.code = EmsClusterDispatchCode::NoQuorum;
    } else if (message.dispatchSequence <= localDispatch_.sequence) {
        accepted.code = EmsClusterDispatchCode::StaleSequence;
    } else if (message.dispatchTtlMs < static_cast<std::uint32_t>(config_.dispatchCycleMs) ||
        message.dispatchTtlMs > static_cast<std::uint32_t>(config_.dispatchTtlMs * 2)) {
        accepted.code = EmsClusterDispatchCode::Expired;
    } else {
        accepted = acceptDispatch(
            message.requestedPower,
            message.dispatchSequence,
            static_cast<int>(message.dispatchTtlMs),
            nowMs
        );
        localDispatch_ = accepted;
    }

    auto reply = baseMessage(EmsClusterMessageType::DispatchAck);
    reply.dispatchSequence = message.dispatchSequence;
    reply.dispatchCode = accepted.code;
    reply.requestedPower = message.requestedPower;
    reply.acceptedPower = accepted.accepted;
    outgoing_.push_back({std::move(reply), message.senderNodeId, false});
}

void EmsClusterNode::handleDispatchAck(const EmsClusterMessage& message, std::int64_t nowMs) {
    if (role_ != EmsClusterRole::Leader || message.term != currentTerm_ ||
        message.membershipEpoch != membershipEpoch_) return;
    auto found = members_.find(message.senderNodeId);
    if (found == members_.end()) return;
    auto& dispatch = found->second.status.dispatch;
    if (message.dispatchSequence < dispatch.sequence) return;
    dispatch.sequence = message.dispatchSequence;
    dispatch.term = message.term;
    dispatch.membershipEpoch = message.membershipEpoch;
    dispatch.receivedAtMs = nowMs;
    dispatch.expireAtMs = nowMs + config_.dispatchTtlMs;
    dispatch.requested = message.requestedPower;
    dispatch.accepted = message.acceptedPower;
    dispatch.code = message.dispatchCode;
    dispatch.valid = message.dispatchCode == EmsClusterDispatchCode::Accepted ||
        message.dispatchCode == EmsClusterDispatchCode::Clamped;
}

void EmsClusterNode::handleFeedback(const EmsClusterMessage& message, std::int64_t nowMs) {
    if (role_ != EmsClusterRole::Leader || message.term != currentTerm_ ||
        message.membershipEpoch != membershipEpoch_) return;
    auto found = members_.find(message.senderNodeId);
    if (found == members_.end()) return;
    found->second.status.capability = message.capability;
    found->second.lastCapabilityMs = nowMs;
    found->second.status.capabilityFresh = true;
}

}  // namespace edge_gateway
