#include "edge_gateway/flexible_phase_balance.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireNear(double actual, double expected, double tolerance, const char* message) {
    if (std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(
            std::string(message) + " actual=" + std::to_string(actual) +
            " expected=" + std::to_string(expected)
        );
    }
}

edge_gateway::FlexiblePhaseBalanceInput baseInput() {
    edge_gateway::FlexiblePhaseBalanceInput input;
    input.loadPowerKw = {30.0, 20.0, 10.0};
    input.pcsMaxChargeKw = {20.0, 20.0, 20.0};
    input.pcsMaxDischargeKw = {20.0, 20.0, 20.0};
    input.allowedSpreadKw = 0.0;
    input.batteryMaxChargeKw = 30.0;
    input.batteryMaxDischargeKw = 30.0;
    return input;
}

void testIdleInsideAllowedBand() {
    auto input = baseInput();
    input.loadPowerKw = {20.0, 20.3, 19.8};
    input.allowedSpreadKw = 1.0;
    const auto output = edge_gateway::solveFlexiblePhaseBalance(input);
    require(output.mode == edge_gateway::FlexiblePhaseBalanceMode::Idle, "balanced input should stay idle");
    requireNear(output.batteryNetPowerKw, 0.0, 1e-9, "idle battery power mismatch");
    requireNear(output.finalCompensationKw[0], 0.0, 1e-9, "idle command mismatch");
}

void testPcsTransferHasZeroNetEnergy() {
    const auto output = edge_gateway::solveFlexiblePhaseBalance(baseInput());
    require(
        output.mode == edge_gateway::FlexiblePhaseBalanceMode::PcsTransfer,
        "unconstrained balancing should use PCS transfer only"
    );
    requireNear(output.pcsTransferKw[0], 10.0, 1e-6, "PCS A transfer mismatch");
    requireNear(output.pcsTransferKw[1], 0.0, 1e-6, "PCS B transfer mismatch");
    requireNear(output.pcsTransferKw[2], -10.0, 1e-6, "PCS C transfer mismatch");
    requireNear(output.batteryNetPowerKw, 0.0, 1e-6, "PCS transfer must not consume net battery power");
    requireNear(output.finalSpreadKw, 0.0, 1e-6, "PCS transfer should balance phases");
}

void testBatteryAssistUsesMinimumEnergyForAllowedSpread() {
    auto input = baseInput();
    input.pcsMaxChargeKw = {20.0, 20.0, 3.0};
    input.allowedSpreadKw = 5.0;
    input.batteryMaxDischargeKw = 20.0;
    const auto output = edge_gateway::solveFlexiblePhaseBalance(input);
    if (output.mode != edge_gateway::FlexiblePhaseBalanceMode::BatteryAssist) {
        throw std::runtime_error(
            "phase limit residual should use battery assist mode=" +
            std::to_string(static_cast<int>(output.mode)) +
            " finalSpread=" + std::to_string(output.finalSpreadKw) +
            " unserved=" + std::to_string(output.unservedSpreadKw) +
            " battery=" + std::to_string(output.batteryNetPowerKw)
        );
    }
    require(output.pcsOnlySpreadKw > input.allowedSpreadKw, "PCS-only stage should retain a residual");
    requireNear(output.finalSpreadKw, 5.0, 1e-5, "battery assist should stop at the allowed spread");
    requireNear(output.batteryNetPowerKw, 11.0, 1e-5, "battery assist should use minimum required energy");
    require(output.batteryNetPowerKw < 21.0, "battery assist must not over-correct to exact balance");
}

void testBatteryLimitReportsUnservedSpread() {
    auto input = baseInput();
    input.pcsMaxChargeKw = {20.0, 20.0, 3.0};
    input.allowedSpreadKw = 5.0;
    input.batteryMaxDischargeKw = 5.0;
    const auto output = edge_gateway::solveFlexiblePhaseBalance(input);
    require(
        output.mode == edge_gateway::FlexiblePhaseBalanceMode::CapacityLimited,
        "insufficient battery capability should report capacity limit"
    );
    requireNear(output.batteryNetPowerKw, 5.0, 1e-6, "battery discharge limit must be enforced");
    require(output.unservedSpreadKw > 0.0, "capacity limit must expose unserved spread");
}

void testBatteryChargeAssistMirrorsDischargeAssist() {
    auto input = baseInput();
    input.loadPowerKw = {-30.0, -20.0, -10.0};
    input.pcsMaxChargeKw = {20.0, 20.0, 20.0};
    input.pcsMaxDischargeKw = {20.0, 20.0, 3.0};
    input.allowedSpreadKw = 5.0;
    input.batteryMaxChargeKw = 20.0;
    const auto output = edge_gateway::solveFlexiblePhaseBalance(input);
    require(
        output.mode == edge_gateway::FlexiblePhaseBalanceMode::BatteryAssist,
        "mirrored phase residual should use battery charging assist"
    );
    requireNear(output.finalSpreadKw, 5.0, 1e-5, "charging assist spread mismatch");
    requireNear(output.batteryNetPowerKw, -11.0, 1e-5, "charging assist minimum energy mismatch");
}

void testSocGateKeepsPcsTransferAvailable() {
    auto input = baseInput();
    input.pcsMaxChargeKw = {20.0, 20.0, 3.0};
    input.allowedSpreadKw = 5.0;
    input.batteryDischargeAllowed = false;
    const auto output = edge_gateway::solveFlexiblePhaseBalance(input);
    require(
        output.mode == edge_gateway::FlexiblePhaseBalanceMode::CapacityLimited,
        "SOC discharge gate should prevent battery assistance"
    );
    requireNear(output.batteryNetPowerKw, 0.0, 1e-6, "SOC gate must keep battery net power at zero");
    requireNear(output.finalCompensationKw[0], output.pcsTransferKw[0], 1e-6, "PCS stage must remain available");
}

}  // namespace

int main() {
    try {
        testIdleInsideAllowedBand();
        testPcsTransferHasZeroNetEnergy();
        testBatteryAssistUsesMinimumEnergyForAllowedSpread();
        testBatteryLimitReportsUnservedSpread();
        testBatteryChargeAssistMirrorsDischargeAssist();
        testSocGateKeepsPcsTransferAvailable();
        std::cout << "flexible_phase_balance_test passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "flexible_phase_balance_test failed: " << ex.what() << "\n";
        return 1;
    }
}
