#pragma once

#include <vector>

namespace edge_gateway {

enum class GridFriendlyResourceTier {
    Primary = 0,
    Battery = 1,
};

struct TieredPowerResource {
    GridFriendlyResourceTier tier = GridFriendlyResourceTier::Battery;
    double maxChargeKw = 0.0;
    double maxDischargeKw = 0.0;
    double minimumStableChargeKw = 0.0;
    double minimumStableDischargeKw = 0.0;
    double weight = 1.0;
};

struct TieredPowerDispatchInput {
    // Positive means export/discharge; negative means import/charge.
    double requestedPowerKw = 0.0;
    std::vector<TieredPowerResource> resources;
};

struct TieredPowerDispatchOutput {
    std::vector<double> assignmentsKw;
    double primaryPowerKw = 0.0;
    double batteryPowerKw = 0.0;
    double deliveredPowerKw = 0.0;
    double unservedPowerKw = 0.0;
    bool primaryCapacityExhausted = false;
    bool batteryCapacityExhausted = false;
};

std::vector<double> allocatePowerByCapacity(
    double total,
    const std::vector<double>& capacities,
    const std::vector<double>& weights
);

std::vector<double> allocatePowerWithMinimum(
    double total,
    const std::vector<double>& capacities,
    const std::vector<double>& minimums,
    const std::vector<double>& weights
);

TieredPowerDispatchOutput dispatchTieredPower(const TieredPowerDispatchInput& input);

}  // namespace edge_gateway
