#pragma once

#include <array>

namespace edge_gateway {

enum class FlexiblePhaseBalanceMode {
    Idle = 0,
    PcsTransfer = 1,
    BatteryAssist = 2,
    CapacityLimited = 3
};

struct FlexiblePhaseBalanceInput {
    std::array<double, 3> loadPowerKw{};
    std::array<double, 3> pcsMaxChargeKw{};
    std::array<double, 3> pcsMaxDischargeKw{};
    double allowedSpreadKw = 0.0;
    double batteryMaxChargeKw = 0.0;
    double batteryMaxDischargeKw = 0.0;
    bool batteryChargeAllowed = true;
    bool batteryDischargeAllowed = true;
};

struct FlexiblePhaseBalanceOutput {
    FlexiblePhaseBalanceMode mode = FlexiblePhaseBalanceMode::Idle;
    std::array<double, 3> pcsTransferKw{};
    std::array<double, 3> batteryAssistDeltaKw{};
    std::array<double, 3> finalCompensationKw{};
    std::array<double, 3> finalGridPowerKw{};
    double inputSpreadKw = 0.0;
    double pcsOnlySpreadKw = 0.0;
    double finalSpreadKw = 0.0;
    double unservedSpreadKw = 0.0;
    double batteryNetPowerKw = 0.0;
    double targetGridPhasePowerKw = 0.0;
};

FlexiblePhaseBalanceOutput solveFlexiblePhaseBalance(const FlexiblePhaseBalanceInput& input);

}  // namespace edge_gateway
