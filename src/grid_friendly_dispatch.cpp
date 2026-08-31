#include "edge_gateway/grid_friendly_dispatch.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace edge_gateway {
namespace {

constexpr double kEpsilon = 1e-9;

void validateFiniteNonNegative(const std::vector<double>& values, const char* name) {
    for (const auto value : values) {
        if (!std::isfinite(value) || value < 0.0) {
            throw std::invalid_argument(std::string(name) + " must contain finite non-negative values");
        }
    }
}

double sum(const std::vector<double>& values) {
    return std::accumulate(values.begin(), values.end(), 0.0);
}

}  // namespace

std::vector<double> allocatePowerByCapacity(
    double total,
    const std::vector<double>& capacities,
    const std::vector<double>& weights
) {
    if (!std::isfinite(total) || capacities.size() != weights.size()) {
        throw std::invalid_argument("power allocation input is invalid");
    }
    validateFiniteNonNegative(capacities, "capacities");
    validateFiniteNonNegative(weights, "weights");

    std::vector<double> result(capacities.size(), 0.0);
    double remaining = std::abs(total);
    if (remaining <= kEpsilon || capacities.empty()) {
        return result;
    }

    std::vector<bool> active(capacities.size(), false);
    for (std::size_t i = 0; i < capacities.size(); ++i) {
        active[i] = capacities[i] > kEpsilon && weights[i] > 0.0;
    }

    for (std::size_t pass = 0; pass <= capacities.size() && remaining > kEpsilon; ++pass) {
        double weightSum = 0.0;
        for (std::size_t i = 0; i < active.size(); ++i) {
            if (active[i]) {
                weightSum += weights[i];
            }
        }
        if (weightSum <= kEpsilon) {
            break;
        }

        const double roundRemaining = remaining;
        double distributed = 0.0;
        for (std::size_t i = 0; i < active.size(); ++i) {
            if (!active[i]) {
                continue;
            }
            const double room = std::max(0.0, capacities[i] - result[i]);
            const double applied = std::min(room, roundRemaining * weights[i] / weightSum);
            result[i] += applied;
            distributed += applied;
            if (room - applied <= kEpsilon) {
                active[i] = false;
            }
        }
        if (distributed <= kEpsilon) {
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

std::vector<double> allocatePowerWithMinimum(
    double total,
    const std::vector<double>& capacities,
    const std::vector<double>& minimums,
    const std::vector<double>& weights
) {
    if (capacities.size() != minimums.size()) {
        throw std::invalid_argument("minimum power count must match capacity count");
    }
    validateFiniteNonNegative(minimums, "minimums");

    auto effectiveCapacities = capacities;
    for (std::size_t pass = 0; pass <= capacities.size(); ++pass) {
        auto result = allocatePowerByCapacity(total, effectiveCapacities, weights);
        std::size_t belowMinimum = result.size();
        double smallestAllocation = std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < result.size(); ++i) {
            const double magnitude = std::abs(result[i]);
            if (magnitude > kEpsilon && magnitude + kEpsilon < minimums[i] && magnitude < smallestAllocation) {
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

TieredPowerDispatchOutput dispatchTieredPower(const TieredPowerDispatchInput& input) {
    if (!std::isfinite(input.requestedPowerKw)) {
        throw std::invalid_argument("requested power must be finite");
    }

    TieredPowerDispatchOutput output;
    output.assignmentsKw.assign(input.resources.size(), 0.0);
    double remaining = input.requestedPowerKw;
    for (const auto tier : {GridFriendlyResourceTier::Primary, GridFriendlyResourceTier::Battery}) {
        std::vector<std::size_t> indexes;
        std::vector<double> capacities;
        std::vector<double> minimums;
        std::vector<double> weights;
        for (std::size_t i = 0; i < input.resources.size(); ++i) {
            const auto& resource = input.resources[i];
            if (!std::isfinite(resource.maxChargeKw) || resource.maxChargeKw < 0.0 ||
                !std::isfinite(resource.maxDischargeKw) || resource.maxDischargeKw < 0.0 ||
                !std::isfinite(resource.minimumStableChargeKw) || resource.minimumStableChargeKw < 0.0 ||
                !std::isfinite(resource.minimumStableDischargeKw) || resource.minimumStableDischargeKw < 0.0 ||
                !std::isfinite(resource.weight) || resource.weight < 0.0) {
                throw std::invalid_argument("tiered resource limits must be finite and non-negative");
            }
            if (resource.tier != tier) {
                continue;
            }
            indexes.push_back(i);
            capacities.push_back(remaining >= 0.0 ? resource.maxDischargeKw : resource.maxChargeKw);
            minimums.push_back(remaining >= 0.0
                ? resource.minimumStableDischargeKw
                : resource.minimumStableChargeKw);
            weights.push_back(resource.weight);
        }

        const auto allocated = allocatePowerWithMinimum(remaining, capacities, minimums, weights);
        const double tierPower = sum(allocated);
        for (std::size_t i = 0; i < indexes.size(); ++i) {
            output.assignmentsKw[indexes[i]] = allocated[i];
        }
        if (tier == GridFriendlyResourceTier::Primary) {
            output.primaryPowerKw = tierPower;
            output.primaryCapacityExhausted = std::abs(remaining - tierPower) > kEpsilon;
        } else {
            output.batteryPowerKw = tierPower;
            output.batteryCapacityExhausted = std::abs(remaining - tierPower) > kEpsilon;
        }
        remaining -= tierPower;
        if (std::abs(remaining) <= kEpsilon) {
            remaining = 0.0;
            break;
        }
    }

    output.deliveredPowerKw = output.primaryPowerKw + output.batteryPowerKw;
    output.unservedPowerKw = input.requestedPowerKw - output.deliveredPowerKw;
    return output;
}

}  // namespace edge_gateway
