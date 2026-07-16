#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

enum class AgcAvcRuntimeState {
    Disabled = 0,
    Standby = 1,
    Active = 2,
    Degraded = 3,
    PausedByPriority = 4,
    RampToZero = 5,
    Failsafe = 6,
};

const char* agcAvcRuntimeStateName(AgcAvcRuntimeState state);

struct AgcAvcCommandInput {
    std::int64_t sequence = 0;
    std::int64_t issuedAtMs = 0;
    double targetPkw = 0.0;
    double targetQkvar = 0.0;
    double targetVoltageV = 0.0;
    double targetPowerFactor = 1.0;
    std::string avcMode;
};

struct AgcAvcPcsRuntimeInput {
    std::string meterCode;
    bool online = false;
    bool ready = false;
    bool chargeAllowed = false;
    bool dischargeAllowed = false;
    bool inputHealthy = true;
    double soc = 0.0;
    double actualPkw = 0.0;
    double actualQkvar = 0.0;
    double availableChargeKw = 0.0;
    double availableDischargeKw = 0.0;
    double availableReactiveKvar = 0.0;
    double collectedRatedActiveKw = 0.0;
    double collectedRatedApparentKva = 0.0;
    double temperatureC = 0.0;
    double maxAcCurrentA = 0.0;
    bool hasAvailableCharge = false;
    bool hasAvailableDischarge = false;
    bool hasAvailableReactive = false;
    bool hasCollectedRatedActive = false;
    bool hasCollectedRatedApparent = false;
    bool hasTemperature = false;
    bool hasAcCurrent = false;
};

struct AgcAvcCycleInput {
    std::int64_t nowMs = 0;
    bool inputHealthy = true;
    std::string inputFailureReason;
    bool remoteEnable = true;
    bool emergencyStop = false;
    bool fireAlarm = false;
    bool priorityBlocked = false;
    double pccActivePowerKw = 0.0;
    double pccReactivePowerKvar = 0.0;
    double pccVoltageV = 0.0;
    double pccPowerFactor = 1.0;
    double frequencyHz = 50.0;
    bool frequencyValid = false;
    AgcAvcCommandInput command;
    std::vector<AgcAvcPcsRuntimeInput> pcs;
};

struct AgcAvcPcsAssignment {
    std::string meterCode;
    bool available = false;
    double targetPkw = 0.0;
    double targetQkvar = 0.0;
    double availableChargeKw = 0.0;
    double availableDischargeKw = 0.0;
    double availableReactiveKvar = 0.0;
    std::string reason;
};

struct AgcAvcCycleOutput {
    AgcAvcRuntimeState state = AgcAvcRuntimeState::Disabled;
    std::string reason;
    std::int64_t commandSequence = 0;
    double commandAgeMs = 0.0;
    double rawTargetPkw = 0.0;
    double effectiveTargetPkw = 0.0;
    double agcErrorKw = 0.0;
    double rawTargetQkvar = 0.0;
    double effectiveTargetQkvar = 0.0;
    double avcError = 0.0;
    double availableActivePowerKw = 0.0;
    double availableReactivePowerKvar = 0.0;
    double unservedActivePowerKw = 0.0;
    double unservedReactivePowerKvar = 0.0;
    int lastWriteStatus = 0;
    int consecutiveWriteFailures = 0;
    std::vector<AgcAvcPcsAssignment> pcs;
};

class AgcAvcController {
public:
    explicit AgcAvcController(AgcAvcConfig config);

    const AgcAvcConfig& config() const;
    AgcAvcCycleOutput step(const AgcAvcCycleInput& input);
    void reset();

private:
    AgcAvcConfig config_;
    std::int64_t lastCycleMs_ = 0;
    std::int64_t lastAvcCalculationMs_ = 0;
    double integralP_ = 0.0;
    double integralQ_ = 0.0;
    double integralV_ = 0.0;
    double previousPkw_ = 0.0;
    double previousQkvar_ = 0.0;
    double heldRawQkvar_ = 0.0;
    double lastAvcError_ = 0.0;
    std::vector<double> previousPcsPkw_;
    std::vector<double> previousPcsQkvar_;
};

}  // namespace edge_gateway
