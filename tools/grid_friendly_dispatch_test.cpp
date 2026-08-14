#include "edge_gateway/grid_friendly_dispatch.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void requireNear(double actual, double expected, const char* message) {
    if (std::abs(actual - expected) > 1e-6) {
        throw std::runtime_error(
            std::string(message) + " actual=" + std::to_string(actual) +
            " expected=" + std::to_string(expected)
        );
    }
}

edge_gateway::TieredPowerResource resource(
    edge_gateway::GridFriendlyResourceTier tier,
    double capacity
) {
    edge_gateway::TieredPowerResource value;
    value.tier = tier;
    value.maxChargeKw = capacity;
    value.maxDischargeKw = capacity;
    return value;
}

void testPrimaryThenBatteryResidual() {
    edge_gateway::TieredPowerDispatchInput input;
    input.requestedPowerKw = 70.0;
    input.resources = {
        resource(edge_gateway::GridFriendlyResourceTier::Primary, 30.0),
        resource(edge_gateway::GridFriendlyResourceTier::Battery, 100.0),
    };
    const auto output = edge_gateway::dispatchTieredPower(input);
    requireNear(output.primaryPowerKw, 30.0, "primary tier mismatch");
    requireNear(output.batteryPowerKw, 40.0, "battery residual mismatch");
    requireNear(output.unservedPowerKw, 0.0, "fully served request should have no residual");
}

void testChargeDirectionUsesSamePriority() {
    edge_gateway::TieredPowerDispatchInput input;
    input.requestedPowerKw = -50.0;
    input.resources = {
        resource(edge_gateway::GridFriendlyResourceTier::Primary, 15.0),
        resource(edge_gateway::GridFriendlyResourceTier::Battery, 20.0),
    };
    const auto output = edge_gateway::dispatchTieredPower(input);
    requireNear(output.primaryPowerKw, -15.0, "primary charge allocation mismatch");
    requireNear(output.batteryPowerKw, -20.0, "battery charge allocation mismatch");
    requireNear(output.unservedPowerKw, -15.0, "charge residual mismatch");
}

void testMinimumStablePowerSkipsUnsuitableResource() {
    auto small = resource(edge_gateway::GridFriendlyResourceTier::Primary, 100.0);
    small.minimumStableDischargeKw = 10.0;
    auto battery = resource(edge_gateway::GridFriendlyResourceTier::Battery, 100.0);
    edge_gateway::TieredPowerDispatchInput input;
    input.requestedPowerKw = 5.0;
    input.resources = {small, battery};
    const auto output = edge_gateway::dispatchTieredPower(input);
    requireNear(output.primaryPowerKw, 0.0, "below-minimum primary resource must stay idle");
    requireNear(output.batteryPowerKw, 5.0, "battery should serve the small residual");
}

}  // namespace

int main() {
    try {
        testPrimaryThenBatteryResidual();
        testChargeDirectionUsesSamePriority();
        testMinimumStablePowerSkipsUnsuitableResource();
        std::cout << "grid_friendly_dispatch_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "grid_friendly_dispatch_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
