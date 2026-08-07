#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "edge_gateway/ems_cluster.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace edge_gateway {

namespace ems_cluster_point {
constexpr std::uint32_t kEnable = 0;
constexpr std::uint32_t kRole = 1;
constexpr std::uint32_t kCabinetNo = 2;
constexpr std::uint32_t kTerm = 3;
constexpr std::uint32_t kOnlineMembers = 4;
constexpr std::uint32_t kQuorumValid = 5;
constexpr std::uint32_t kLeaderCabinetNo = 6;
constexpr std::uint32_t kLoadScore = 7;
constexpr std::uint32_t kLinkType = 8;
constexpr std::uint32_t kLeaderRttMs = 9;
constexpr std::uint32_t kConfigCompatible = 10;

constexpr std::uint32_t kSoc = 20;
constexpr std::uint32_t kRatedActivePower = 21;
constexpr std::uint32_t kRatedApparentPower = 22;
constexpr std::uint32_t kAvailableChargePower = 23;
constexpr std::uint32_t kAvailableDischargePower = 24;
constexpr std::uint32_t kAvailableReactivePower = 25;
constexpr std::uint32_t kControlReady = 26;
constexpr std::uint32_t kInterlocked = 27;
constexpr std::uint32_t kManualOverride = 28;

constexpr std::uint32_t kDispatchPa = 30;
constexpr std::uint32_t kDispatchPb = 31;
constexpr std::uint32_t kDispatchPc = 32;
constexpr std::uint32_t kDispatchQa = 33;
constexpr std::uint32_t kDispatchQb = 34;
constexpr std::uint32_t kDispatchQc = 35;

constexpr std::uint32_t kFeedbackPa = 40;
constexpr std::uint32_t kFeedbackPb = 41;
constexpr std::uint32_t kFeedbackPc = 42;
constexpr std::uint32_t kFeedbackQa = 43;
constexpr std::uint32_t kFeedbackQb = 44;
constexpr std::uint32_t kFeedbackQc = 45;
constexpr std::uint32_t kDispatchResult = 50;
constexpr std::uint32_t kDispatchRejectReason = 51;
constexpr std::uint32_t kStationStrategyActive = 52;
constexpr std::uint32_t kDispatchSequence = 53;

constexpr std::uint32_t kStationTargetPa = 60;
constexpr std::uint32_t kStationTargetPb = 61;
constexpr std::uint32_t kStationTargetPc = 62;
constexpr std::uint32_t kStationTargetQa = 63;
constexpr std::uint32_t kStationTargetQb = 64;
constexpr std::uint32_t kStationTargetQc = 65;
}  // namespace ems_cluster_point

std::vector<PointStoreRoute> emsClusterPointRoutes(
    const EmsClusterConfig& config,
    const std::string& machineCode
);

void addEmsClusterPointRoutes(
    PointStoreRouter& router,
    const EmsClusterConfig& config,
    const std::string& machineCode
);

class EmsClusterPointBridge {
public:
    EmsClusterPointBridge(EmsClusterConfig config, std::string machineCode);

    EmsClusterCapability sampleCapability(std::int64_t nowMs) const;
    EmsClusterPhasePower sampleStationTarget(std::int64_t nowMs, bool& valid) const;
    void publish(
        const EmsClusterStatus& status,
        const EmsClusterDispatchState& dispatch,
        std::int64_t nowMs
    );

private:
    bool read(
        std::uint32_t offset,
        std::int64_t nowMs,
        double& value,
        std::int64_t maxAgeMs = -1
    ) const;
    void write(std::uint32_t offset, double value, int quality, std::int64_t nowMs);

    EmsClusterConfig config_;
    std::string machineCode_;
    MemoryPointStore store_;
};

}  // namespace edge_gateway
