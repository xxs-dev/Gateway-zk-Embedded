#include "edge_gateway/flexible_phase_balance.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace edge_gateway {
namespace {

// One milliwatt is below the useful control resolution and avoids mode flapping
// when a bisection result lands immediately outside the configured spread.
constexpr double kEpsilon = 1e-6;

double spread(const std::array<double, 3>& values) {
    const auto limits = std::minmax_element(values.begin(), values.end());
    return *limits.second - *limits.first;
}

double sum(const std::array<double, 3>& values) {
    return std::accumulate(values.begin(), values.end(), 0.0);
}

bool finiteArray(const std::array<double, 3>& values) {
    return std::all_of(values.begin(), values.end(), [](double value) {
        return std::isfinite(value);
    });
}

struct PhaseEvaluation {
    std::array<double, 3> grid{};
    std::array<double, 3> compensation{};
    double netPower = 0.0;
    double phaseSpread = 0.0;
};

class PhaseSolver {
public:
    explicit PhaseSolver(const FlexiblePhaseBalanceInput& input) : input_(input) {
        for (std::size_t i = 0; i < 3; ++i) {
            lower_[i] = input_.loadPowerKw[i] - input_.pcsMaxDischargeKw[i];
            upper_[i] = input_.loadPowerKw[i] + input_.pcsMaxChargeKw[i];
        }
        searchLow_ = *std::min_element(lower_.begin(), lower_.end());
        searchHigh_ = *std::max_element(upper_.begin(), upper_.end());
    }

    PhaseEvaluation evaluate(double target) const {
        PhaseEvaluation value;
        for (std::size_t i = 0; i < 3; ++i) {
            value.grid[i] = std::max(lower_[i], std::min(target, upper_[i]));
            value.compensation[i] = input_.loadPowerKw[i] - value.grid[i];
        }
        value.netPower = sum(value.compensation);
        value.phaseSpread = spread(value.grid);
        return value;
    }

    double targetForNetPower(double requestedNetPower) const {
        const auto lowNet = evaluate(searchLow_).netPower;
        const auto highNet = evaluate(searchHigh_).netPower;
        const auto targetNet = std::max(highNet, std::min(requestedNetPower, lowNet));
        double low = searchLow_;
        double high = searchHigh_;
        for (int i = 0; i < 100; ++i) {
            const double middle = (low + high) / 2.0;
            if (evaluate(middle).netPower > targetNet) {
                low = middle;
            } else {
                high = middle;
            }
        }
        return (low + high) / 2.0;
    }

    std::pair<double, double> minimumSpreadTargets() const {
        const double maxLower = *std::max_element(lower_.begin(), lower_.end());
        const double minUpper = *std::min_element(upper_.begin(), upper_.end());
        return {std::min(maxLower, minUpper), std::max(maxLower, minUpper)};
    }

private:
    const FlexiblePhaseBalanceInput& input_;
    std::array<double, 3> lower_{};
    std::array<double, 3> upper_{};
    double searchLow_ = 0.0;
    double searchHigh_ = 0.0;
};

double leftAcceptableTarget(
    const PhaseSolver& solver,
    double low,
    double knownAcceptable,
    double allowedSpread
) {
    if (solver.evaluate(low).phaseSpread <= allowedSpread + kEpsilon) {
        return low;
    }
    double high = knownAcceptable;
    for (int i = 0; i < 100; ++i) {
        const double middle = (low + high) / 2.0;
        if (solver.evaluate(middle).phaseSpread <= allowedSpread + kEpsilon) {
            high = middle;
        } else {
            low = middle;
        }
    }
    return high;
}

double rightAcceptableTarget(
    const PhaseSolver& solver,
    double knownAcceptable,
    double high,
    double allowedSpread
) {
    if (solver.evaluate(high).phaseSpread <= allowedSpread + kEpsilon) {
        return high;
    }
    double low = knownAcceptable;
    for (int i = 0; i < 100; ++i) {
        const double middle = (low + high) / 2.0;
        if (solver.evaluate(middle).phaseSpread <= allowedSpread + kEpsilon) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return low;
}

}  // namespace

FlexiblePhaseBalanceOutput solveFlexiblePhaseBalance(const FlexiblePhaseBalanceInput& input) {
    if (!finiteArray(input.loadPowerKw) || !finiteArray(input.pcsMaxChargeKw) ||
        !finiteArray(input.pcsMaxDischargeKw) || !std::isfinite(input.allowedSpreadKw) ||
        !std::isfinite(input.batteryMaxChargeKw) || !std::isfinite(input.batteryMaxDischargeKw)) {
        throw std::invalid_argument("flexible phase balance input must be finite");
    }
    if (input.allowedSpreadKw < 0.0 || input.batteryMaxChargeKw < 0.0 ||
        input.batteryMaxDischargeKw < 0.0 ||
        std::any_of(input.pcsMaxChargeKw.begin(), input.pcsMaxChargeKw.end(), [](double value) {
            return value < 0.0;
        }) ||
        std::any_of(input.pcsMaxDischargeKw.begin(), input.pcsMaxDischargeKw.end(), [](double value) {
            return value < 0.0;
        })) {
        throw std::invalid_argument("flexible phase balance limits must be non-negative");
    }

    FlexiblePhaseBalanceOutput output;
    output.inputSpreadKw = spread(input.loadPowerKw);
    output.pcsOnlySpreadKw = output.inputSpreadKw;
    output.finalSpreadKw = output.inputSpreadKw;
    output.unservedSpreadKw = std::max(0.0, output.inputSpreadKw - input.allowedSpreadKw);
    output.finalGridPowerKw = input.loadPowerKw;
    output.targetGridPhasePowerKw = sum(input.loadPowerKw) / 3.0;
    if (output.inputSpreadKw <= input.allowedSpreadKw + kEpsilon) {
        return output;
    }

    const PhaseSolver solver(input);
    const double zeroEnergyTarget = solver.targetForNetPower(0.0);
    const auto pcsOnly = solver.evaluate(zeroEnergyTarget);
    output.pcsTransferKw = pcsOnly.compensation;
    output.pcsOnlySpreadKw = pcsOnly.phaseSpread;

    if (pcsOnly.phaseSpread <= input.allowedSpreadKw + kEpsilon) {
        output.mode = FlexiblePhaseBalanceMode::PcsTransfer;
        output.finalCompensationKw = pcsOnly.compensation;
        output.finalGridPowerKw = pcsOnly.grid;
        output.finalSpreadKw = pcsOnly.phaseSpread;
        output.unservedSpreadKw = 0.0;
        output.targetGridPhasePowerKw = zeroEnergyTarget;
        return output;
    }

    const double maxCharge = input.batteryChargeAllowed ? input.batteryMaxChargeKw : 0.0;
    const double maxDischarge = input.batteryDischargeAllowed ? input.batteryMaxDischargeKw : 0.0;
    const double batteryLowTarget = solver.targetForNetPower(maxDischarge);
    const double batteryHighTarget = solver.targetForNetPower(-maxCharge);
    const auto unconstrainedMinimum = solver.minimumSpreadTargets();

    double minimumLow = std::max(batteryLowTarget, unconstrainedMinimum.first);
    double minimumHigh = std::min(batteryHighTarget, unconstrainedMinimum.second);
    if (minimumLow > minimumHigh) {
        if (batteryHighTarget < unconstrainedMinimum.first) {
            minimumLow = batteryHighTarget;
            minimumHigh = batteryHighTarget;
        } else {
            minimumLow = batteryLowTarget;
            minimumHigh = batteryLowTarget;
        }
    }

    const double minimumEnergyTarget = std::max(minimumLow, std::min(zeroEnergyTarget, minimumHigh));
    const auto minimum = solver.evaluate(minimumEnergyTarget);
    double finalTarget = minimumEnergyTarget;
    if (minimum.phaseSpread <= input.allowedSpreadKw + kEpsilon) {
        const double acceptableLow = leftAcceptableTarget(
            solver,
            batteryLowTarget,
            minimumLow,
            input.allowedSpreadKw
        );
        const double acceptableHigh = rightAcceptableTarget(
            solver,
            minimumHigh,
            batteryHighTarget,
            input.allowedSpreadKw
        );
        finalTarget = std::max(acceptableLow, std::min(zeroEnergyTarget, acceptableHigh));
    }

    const auto finalValue = solver.evaluate(finalTarget);
    output.finalCompensationKw = finalValue.compensation;
    output.finalGridPowerKw = finalValue.grid;
    output.finalSpreadKw = finalValue.phaseSpread;
    output.unservedSpreadKw = std::max(0.0, finalValue.phaseSpread - input.allowedSpreadKw);
    output.batteryNetPowerKw = finalValue.netPower;
    output.targetGridPhasePowerKw = finalTarget;
    for (std::size_t i = 0; i < 3; ++i) {
        output.batteryAssistDeltaKw[i] = finalValue.compensation[i] - pcsOnly.compensation[i];
    }
    output.mode = output.unservedSpreadKw > kEpsilon
        ? FlexiblePhaseBalanceMode::CapacityLimited
        : (std::abs(output.batteryNetPowerKw) > kEpsilon
            ? FlexiblePhaseBalanceMode::BatteryAssist
            : FlexiblePhaseBalanceMode::PcsTransfer);
    return output;
}

}  // namespace edge_gateway
