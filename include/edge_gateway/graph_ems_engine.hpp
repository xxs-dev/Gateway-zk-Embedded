#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "edge_gateway/models.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace edge_gateway {

struct GraphEmsNodeConfig {
    std::string id;
    std::string type;
    bool enabled = true;
    std::unordered_map<std::string, std::string> params;
};

struct GraphEmsEdgeConfig {
    std::string from;
    std::string to;
};

struct GraphEmsConfig {
    std::string schemaVersion = "1.0.0";
    std::string graphCode;
    std::size_t maxNodes = 256;
    std::size_t maxEdges = 512;
    std::vector<GraphEmsNodeConfig> nodes;
    std::vector<GraphEmsEdgeConfig> edges;

    static GraphEmsConfig loadFromFile(const std::string& path);
};

struct GraphEmsRunResult {
    std::size_t latestWrites = 0;
    std::size_t deviceWrites = 0;
    std::vector<std::string> errors;
};

class GraphEmsEngine {
public:
    GraphEmsEngine(
        GraphEmsConfig config,
        PointStoreRouter& router,
        std::int64_t defaultTtlMs = 600000,
        std::string stateFile = std::string(),
        std::unordered_map<std::string, std::string> profile = {}
    );

    GraphEmsRunResult runOnce(std::int64_t nowMs);

private:
    Optional<double> latestValue(std::uint32_t index, std::int64_t nowMs) const;
    CommandSubmitResult set(std::uint32_t index, double value, std::int64_t nowMs);
    bool profileEnabled(const std::string& key, bool defaultValue) const;
    int profileInt(const std::string& key, int defaultValue) const;
    bool shouldRunNode(const GraphEmsNodeConfig& node) const;
    void restoreState(std::int64_t nowMs);
    void saveState(std::int64_t nowMs);
    std::vector<std::uint32_t> stateOutputIndexes() const;
    bool runMeterAverage(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runDerivedLoad(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runBmsDerived(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runCosCompensation(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runVoltageCompensation(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runChargeDischarge(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runSequentialChargeDischarge(
        const GraphEmsNodeConfig& node,
        std::int64_t nowMs,
        GraphEmsRunResult& result
    );
    bool runChargeDischargeCycleTest(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runTimedChargeDischarge(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runPhotovoltaicCharge(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runPhaseBalance(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runSkOverride(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runReserveCapacity(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runFormula(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runTimeSource(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runWindowAggregate(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runScheduleSelect(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runPhaseArbiter(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runPowerConstraint(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runSwitch(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runControlGate(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runFeedbackVerify(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runControlWrite(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runRateLimit(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runHysteresis(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runDebounce(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runSequence(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runPcsPowerSolve(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runPcsWriteback(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool submitPcsWritebackCommands(
        const GraphEmsNodeConfig& node,
        std::int64_t nowMs,
        GraphEmsRunResult& result,
        bool submitMissingZeroTargets
    );

    GraphEmsConfig config_;
    PointStoreRouter& router_;
    std::int64_t defaultTtlMs_ = 600000;
    std::string stateFile_;
    std::unordered_map<std::string, std::string> profile_;
    std::vector<const GraphEmsNodeConfig*> executionOrder_;
    std::vector<std::uint32_t> snapshotIndexes_;
    mutable bool snapshotActive_ = false;
    mutable std::unordered_map<std::uint32_t, double> snapshotValues_;
    mutable std::unordered_set<std::uint32_t> snapshotResolvedIndexes_;
    bool stateRestored_ = false;
    bool stateSaved_ = false;
    std::int64_t lastStateSaveAt_ = 0;
    std::unordered_map<std::uint32_t, std::vector<double>> averageWindows_;
    struct FeedbackVerifyState {
        bool initialized = false;
        double target = 0.0;
        std::int64_t targetChangedAt = 0;
    };
    std::unordered_map<std::string, FeedbackVerifyState> feedbackVerifyStates_;
    struct RateLimitState {
        bool initialized = false;
        double output = 0.0;
        std::int64_t lastRunAt = 0;
    };
    std::unordered_map<std::string, RateLimitState> rateLimitStates_;
    struct BooleanFilterState {
        bool initialized = false;
        bool output = false;
        bool candidate = false;
        std::int64_t candidateSince = 0;
    };
    std::unordered_map<std::string, BooleanFilterState> hysteresisStates_;
    std::unordered_map<std::string, BooleanFilterState> debounceStates_;
    struct SequenceState {
        bool initialized = false;
        int current = 0;
        std::int64_t enteredAt = 0;
    };
    std::unordered_map<std::string, SequenceState> sequenceStates_;
};

}  // namespace edge_gateway
