#include "edge_gateway/ems_cluster_points.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace edge_gateway {

namespace {

struct ClusterPointDescriptor {
    std::uint32_t offset;
    const char* code;
    bool fullUpload;
};

const ClusterPointDescriptor kClusterPoints[] = {
    {ems_cluster_point::kEnable, "EMS_CLUSTER_ENABLE", true},
    {ems_cluster_point::kRole, "EMS_CLUSTER_ROLE", true},
    {ems_cluster_point::kCabinetNo, "EMS_CLUSTER_CABINET_NO", true},
    {ems_cluster_point::kTerm, "EMS_CLUSTER_TERM", true},
    {ems_cluster_point::kOnlineMembers, "EMS_CLUSTER_ONLINE_MEMBERS", true},
    {ems_cluster_point::kQuorumValid, "EMS_CLUSTER_QUORUM_VALID", true},
    {ems_cluster_point::kLeaderCabinetNo, "EMS_CLUSTER_LEADER_CABINET_NO", true},
    {ems_cluster_point::kLoadScore, "EMS_CLUSTER_LOAD_SCORE", true},
    {ems_cluster_point::kLinkType, "EMS_CLUSTER_LINK_TYPE", true},
    {ems_cluster_point::kLeaderRttMs, "EMS_CLUSTER_LEADER_RTT_MS", true},
    {ems_cluster_point::kConfigCompatible, "EMS_CLUSTER_CONFIG_COMPATIBLE", true},
    {ems_cluster_point::kSoc, "EMS_CLUSTER_LOCAL_SOC", true},
    {ems_cluster_point::kRatedActivePower, "EMS_CLUSTER_RATED_ACTIVE_POWER_KW", true},
    {ems_cluster_point::kRatedApparentPower, "EMS_CLUSTER_RATED_APPARENT_POWER_KVA", true},
    {ems_cluster_point::kAvailableChargePower, "EMS_CLUSTER_AVAILABLE_CHARGE_POWER_KW", true},
    {ems_cluster_point::kAvailableDischargePower, "EMS_CLUSTER_AVAILABLE_DISCHARGE_POWER_KW", true},
    {ems_cluster_point::kAvailableReactivePower, "EMS_CLUSTER_AVAILABLE_REACTIVE_POWER_KVAR", true},
    {ems_cluster_point::kControlReady, "EMS_CLUSTER_CONTROL_READY", true},
    {ems_cluster_point::kInterlocked, "EMS_CLUSTER_INTERLOCKED", true},
    {ems_cluster_point::kManualOverride, "EMS_CLUSTER_MANUAL_OVERRIDE", true},
    {ems_cluster_point::kDispatchPa, "EMS_CLUSTER_DISPATCH_PA_KW", true},
    {ems_cluster_point::kDispatchPb, "EMS_CLUSTER_DISPATCH_PB_KW", true},
    {ems_cluster_point::kDispatchPc, "EMS_CLUSTER_DISPATCH_PC_KW", true},
    {ems_cluster_point::kDispatchQa, "EMS_CLUSTER_DISPATCH_QA_KVAR", true},
    {ems_cluster_point::kDispatchQb, "EMS_CLUSTER_DISPATCH_QB_KVAR", true},
    {ems_cluster_point::kDispatchQc, "EMS_CLUSTER_DISPATCH_QC_KVAR", true},
    {ems_cluster_point::kFeedbackPa, "EMS_CLUSTER_FEEDBACK_PA_KW", true},
    {ems_cluster_point::kFeedbackPb, "EMS_CLUSTER_FEEDBACK_PB_KW", true},
    {ems_cluster_point::kFeedbackPc, "EMS_CLUSTER_FEEDBACK_PC_KW", true},
    {ems_cluster_point::kFeedbackQa, "EMS_CLUSTER_FEEDBACK_QA_KVAR", true},
    {ems_cluster_point::kFeedbackQb, "EMS_CLUSTER_FEEDBACK_QB_KVAR", true},
    {ems_cluster_point::kFeedbackQc, "EMS_CLUSTER_FEEDBACK_QC_KVAR", true},
    {ems_cluster_point::kDispatchResult, "EMS_CLUSTER_DISPATCH_RESULT", true},
    {ems_cluster_point::kDispatchRejectReason, "EMS_CLUSTER_DISPATCH_REJECT_REASON", true},
    {ems_cluster_point::kStationStrategyActive, "EMS_CLUSTER_STATION_STRATEGY_ACTIVE", true},
    {ems_cluster_point::kDispatchSequence, "EMS_CLUSTER_DISPATCH_SEQUENCE", true},
    {ems_cluster_point::kStationTargetPa, "EMS_CLUSTER_STATION_TARGET_PA_KW", true},
    {ems_cluster_point::kStationTargetPb, "EMS_CLUSTER_STATION_TARGET_PB_KW", true},
    {ems_cluster_point::kStationTargetPc, "EMS_CLUSTER_STATION_TARGET_PC_KW", true},
    {ems_cluster_point::kStationTargetQa, "EMS_CLUSTER_STATION_TARGET_QA_KVAR", true},
    {ems_cluster_point::kStationTargetQb, "EMS_CLUSTER_STATION_TARGET_QB_KVAR", true},
    {ems_cluster_point::kStationTargetQc, "EMS_CLUSTER_STATION_TARGET_QC_KVAR", true}
};

int leaderCabinetNo(const EmsClusterStatus& status) {
    if (status.leaderNodeId.empty()) return 0;
    if (status.leaderNodeId == status.nodeId) return status.cabinetNo;
    const auto found = std::find_if(status.members.begin(), status.members.end(), [&](const auto& member) {
        return member.nodeId == status.leaderNodeId;
    });
    return found == status.members.end() ? 0 : found->cabinetNo;
}

}  // namespace

std::vector<PointStoreRoute> emsClusterPointRoutes(
    const EmsClusterConfig& config,
    const std::string& machineCode
) {
    std::vector<PointStoreRoute> routes;
    if (!config.enabled || config.virtualSharedMemoryName.empty() || config.virtualPointBaseIndex == 0) {
        return routes;
    }
    routes.reserve(sizeof(kClusterPoints) / sizeof(kClusterPoints[0]));
    for (const auto& descriptor : kClusterPoints) {
        PointStoreRoute route;
        route.index = config.virtualPointBaseIndex + descriptor.offset;
        route.sourceIndex = route.index;
        route.machineCode = machineCode;
        route.meterCode = "EMS_CLUSTER";
        route.pointCode = descriptor.code;
        route.interfaceCode = config.virtualSharedMemoryName;
        route.interfaceType = "compute";
        route.sharedMemoryName = config.virtualSharedMemoryName;
        route.fullUpload = descriptor.fullUpload;
        route.reportOnChange = true;
        route.isStore = false;
        route.writable = false;
        routes.push_back(std::move(route));
    }
    return routes;
}

void addEmsClusterPointRoutes(
    PointStoreRouter& router,
    const EmsClusterConfig& config,
    const std::string& machineCode
) {
    for (const auto& route : emsClusterPointRoutes(config, machineCode)) {
        router.addRoute(route);
    }
}

EmsClusterPointBridge::EmsClusterPointBridge(EmsClusterConfig config, std::string machineCode)
    : config_(std::move(config)),
      machineCode_(std::move(machineCode)),
      store_(config_.virtualSharedMemoryName) {
    if (config_.virtualSharedMemoryName.empty()) {
        throw std::invalid_argument("emsCluster.virtualSharedMemoryName is required");
    }
}

bool EmsClusterPointBridge::read(
    std::uint32_t offset,
    std::int64_t nowMs,
    double& value,
    std::int64_t maxAgeMs
) const {
    const auto point = store_.getLatestByIndex(config_.virtualPointBaseIndex + offset, nowMs);
    if (!point || point->stale || point->quality != 1 || !std::isfinite(point->value)) return false;
    if (maxAgeMs >= 0 && (point->ts <= 0 || point->ts > nowMs + 1000 || nowMs - point->ts > maxAgeMs)) {
        return false;
    }
    value = point->value;
    return true;
}

EmsClusterCapability EmsClusterPointBridge::sampleCapability(std::int64_t nowMs) const {
    EmsClusterCapability result;
    double value = 0.0;
    const bool enablePresent = read(ems_cluster_point::kEnable, nowMs, value);
    result.controlEnabled = config_.controlEnabled && enablePresent && value >= 0.5;

    const bool socPresent = read(
        ems_cluster_point::kSoc,
        nowMs,
        result.socPercent,
        config_.capabilityTtlMs
    );
    const bool ratedActivePresent = read(
        ems_cluster_point::kRatedActivePower,
        nowMs,
        result.ratedActivePowerKw
    );
    const bool ratedApparentPresent = read(
        ems_cluster_point::kRatedApparentPower,
        nowMs,
        result.ratedApparentPowerKva
    );
    const bool chargePresent = read(
        ems_cluster_point::kAvailableChargePower,
        nowMs,
        result.availableChargePowerKw,
        config_.capabilityTtlMs
    );
    const bool dischargePresent = read(
        ems_cluster_point::kAvailableDischargePower,
        nowMs,
        result.availableDischargePowerKw,
        config_.capabilityTtlMs
    );
    read(
        ems_cluster_point::kAvailableReactivePower,
        nowMs,
        result.availableReactivePowerKvar,
        config_.capabilityTtlMs
    );

    bool readyPoint = false;
    if (read(ems_cluster_point::kControlReady, nowMs, value, config_.capabilityTtlMs)) {
        readyPoint = value >= 0.5;
    }
    result.interlocked = true;
    if (read(ems_cluster_point::kInterlocked, nowMs, value, config_.capabilityTtlMs)) {
        result.interlocked = value >= 0.5;
    }
    if (read(ems_cluster_point::kManualOverride, nowMs, value, config_.capabilityTtlMs)) {
        result.manualOverride = value >= 0.5;
    }

    result.socPercent = std::max(0.0, std::min(100.0, result.socPercent));
    result.ratedActivePowerKw = std::max(0.0, result.ratedActivePowerKw);
    result.ratedApparentPowerKva = std::max(0.0, result.ratedApparentPowerKva);
    result.availableChargePowerKw = std::max(0.0, result.availableChargePowerKw);
    result.availableDischargePowerKw = std::max(0.0, result.availableDischargePowerKw);
    result.availableReactivePowerKvar = std::max(0.0, result.availableReactivePowerKvar);
    result.ready = readyPoint && socPresent && ratedActivePresent && ratedApparentPresent &&
        chargePresent && dischargePresent;

    read(ems_cluster_point::kFeedbackPa, nowMs, result.actual.paKw, config_.capabilityTtlMs);
    read(ems_cluster_point::kFeedbackPb, nowMs, result.actual.pbKw, config_.capabilityTtlMs);
    read(ems_cluster_point::kFeedbackPc, nowMs, result.actual.pcKw, config_.capabilityTtlMs);
    read(ems_cluster_point::kFeedbackQa, nowMs, result.actual.qaKvar, config_.capabilityTtlMs);
    read(ems_cluster_point::kFeedbackQb, nowMs, result.actual.qbKvar, config_.capabilityTtlMs);
    read(ems_cluster_point::kFeedbackQc, nowMs, result.actual.qcKvar, config_.capabilityTtlMs);
    return result;
}

EmsClusterPhasePower EmsClusterPointBridge::sampleStationTarget(
    std::int64_t nowMs,
    bool& valid
) const {
    EmsClusterPhasePower result;
    valid = read(ems_cluster_point::kStationTargetPa, nowMs, result.paKw, config_.stationTargetTtlMs) &&
        read(ems_cluster_point::kStationTargetPb, nowMs, result.pbKw, config_.stationTargetTtlMs) &&
        read(ems_cluster_point::kStationTargetPc, nowMs, result.pcKw, config_.stationTargetTtlMs) &&
        read(ems_cluster_point::kStationTargetQa, nowMs, result.qaKvar, config_.stationTargetTtlMs) &&
        read(ems_cluster_point::kStationTargetQb, nowMs, result.qbKvar, config_.stationTargetTtlMs) &&
        read(ems_cluster_point::kStationTargetQc, nowMs, result.qcKvar, config_.stationTargetTtlMs);
    return result;
}

void EmsClusterPointBridge::write(
    std::uint32_t offset,
    double value,
    int quality,
    std::int64_t nowMs
) {
    PointValue point;
    point.index = config_.virtualPointBaseIndex + offset;
    point.machineCode = machineCode_;
    point.meterCode = "EMS_CLUSTER";
    point.pointCode = "EMS_CLUSTER_" + std::to_string(offset);
    point.category = "cluster";
    point.value = value;
    point.quality = quality;
    point.qualityMsg = quality == 1 ? "ok" : "cluster target unavailable";
    point.ts = nowMs;
    point.expireAt = nowMs + std::max(config_.dispatchTtlMs, config_.statusIntervalMs * 3);
    store_.putLatest(point);
}

void EmsClusterPointBridge::publish(
    const EmsClusterStatus& status,
    const EmsClusterDispatchState& dispatch,
    std::int64_t nowMs
) {
    write(ems_cluster_point::kRole, static_cast<int>(status.role), 1, nowMs);
    write(ems_cluster_point::kCabinetNo, status.cabinetNo, 1, nowMs);
    write(ems_cluster_point::kTerm, static_cast<double>(status.term), 1, nowMs);
    write(ems_cluster_point::kOnlineMembers, status.onlineMembers, 1, nowMs);
    write(ems_cluster_point::kQuorumValid, status.quorumValid ? 1.0 : 0.0, 1, nowMs);
    write(ems_cluster_point::kLeaderCabinetNo, leaderCabinetNo(status), 1, nowMs);
    write(ems_cluster_point::kLoadScore, status.loadScore, 1, nowMs);
    write(ems_cluster_point::kLinkType, 1.0, 1, nowMs);
    write(ems_cluster_point::kLeaderRttMs, 0.0, 1, nowMs);
    const bool compatible = std::all_of(status.members.begin(), status.members.end(), [](const auto& member) {
        return !member.online || member.compatible;
    });
    write(ems_cluster_point::kConfigCompatible, compatible ? 1.0 : 0.0, 1, nowMs);
    write(
        ems_cluster_point::kStationStrategyActive,
        status.role == EmsClusterRole::Leader && status.quorumValid &&
            status.controlConfigured && status.capability.controlEnabled ? 1.0 : 0.0,
        1,
        nowMs
    );
    write(ems_cluster_point::kDispatchSequence, static_cast<double>(dispatch.sequence), 1, nowMs);
    write(ems_cluster_point::kDispatchResult, dispatch.valid ? 1.0 : 0.0, 1, nowMs);
    write(
        ems_cluster_point::kDispatchRejectReason,
        static_cast<double>(dispatch.code),
        1,
        nowMs
    );

    const auto& target = dispatch.valid ? dispatch.accepted : EmsClusterPhasePower{};
    const int targetQuality = dispatch.valid || config_.zeroTargetOnLoss ? 1 : 0;
    write(ems_cluster_point::kDispatchPa, target.paKw, targetQuality, nowMs);
    write(ems_cluster_point::kDispatchPb, target.pbKw, targetQuality, nowMs);
    write(ems_cluster_point::kDispatchPc, target.pcKw, targetQuality, nowMs);
    write(ems_cluster_point::kDispatchQa, target.qaKvar, targetQuality, nowMs);
    write(ems_cluster_point::kDispatchQb, target.qbKvar, targetQuality, nowMs);
    write(ems_cluster_point::kDispatchQc, target.qcKvar, targetQuality, nowMs);
}

}  // namespace edge_gateway
