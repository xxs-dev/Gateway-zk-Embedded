#pragma once
#include <memory>

#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "edge_gateway/models.hpp"
#include "edge_gateway/manual_recovery.hpp"
#include "edge_gateway/recovery_feedback.hpp"
#include "edge_gateway/confirmed_control.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace edge_gateway {

struct GraphEmsRuntimeCapabilities {
    static constexpr const char* runtimeSchema() { return "2.x"; }
    static constexpr const char* editorSourceSchema() { return "2.x"; }
    static constexpr const char* compilerContract() { return "GraphEmsV2/direct"; }
    static constexpr bool executesEditorSource() { return true; }
};

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

struct GraphEmsIndexClaim {
    std::uint32_t index = 0;
    std::string nodeId;
    std::string portId;
    std::string kind;
    int order = 0;
};

struct GraphEmsConfig {
    // Digest of the exact bytes parsed at load time, never re-read from disk.
    std::string sourceSha256;
    std::string sourcePath;
    std::string schemaVersion = "2.0.0";
    std::string graphCode;
    std::size_t maxNodes = 256;
    std::size_t maxEdges = 512;
    bool preserveImportedBehavior = false;
    std::vector<GraphEmsNodeConfig> nodes;
    std::vector<GraphEmsEdgeConfig> edges;
    std::vector<GraphEmsIndexClaim> indexClaims;
    std::vector<std::string> loadWarnings;

    // Production loader: the referenced graphFile must contain a V2 executable graph.
    static GraphEmsConfig loadFromFile(const std::string& path);

    // Explicit migration/test input only. Production services must never call this loader.
    static GraphEmsConfig loadLegacyV1ForMigration(const std::string& path);
};

// Runtime safety options supplied by the hosting service (ComputeEngine). Defaults are the
// safe behaviour; each field is a configurable fallback for legacy sites.
struct GraphEmsEngineOptions {
    // Republish restored power-setpoint / BMS-limit outputs from the state file as good quality.
    bool restoreSetpointOutputs = false;
    // Inputs whose timestamp is further than this in the future (wall-clock step backwards) are
    // treated as missing. Points that never expire (retained parameters) are exempt. <=0 disables.
    std::int64_t maxInputFutureSkewMs = 1000;
    // Upper bound for the elapsed time a rateLimit node may convert into one output step.
    std::int64_t rateLimitMaxGapMs = 2000;
};

struct GraphEmsRunResult {
    std::size_t latestWrites = 0;
    std::size_t deviceWrites = 0;
    std::size_t deviceWritesSkipped = 0;
    bool writeLimitReached = false;
    std::vector<std::string> errors;
};

class GraphEmsEngine {
public:
    GraphEmsEngine(
        GraphEmsConfig config,
        PointStoreRouter& router,
        std::int64_t defaultTtlMs = 600000,
        std::string stateFile = std::string(),
        std::unordered_map<std::string, std::string> profile = {},
        std::string ruleCode = std::string()
    );

    GraphEmsRunResult runOnce(std::int64_t nowMs);
    GraphEmsRunResult runOnce(std::int64_t nowMs, std::size_t maxDeviceWrites);
    void setOptions(const GraphEmsEngineOptions& options) { options_ = options; }
    // Called by the owning compute thread between scans; never reads a command
    // queue or mutates/restores the journal. This is a sampled owner view.
    std::string commandObservation() const;

private:
    struct PointSnapshot {
        double value = 0.0;
        int quality = 0;
        std::int64_t ts = 0;
        bool stale = true;
        Optional<ClusterWriteAuthorization> clusterAuthorization;
        std::int64_t expireAt = 0;
    };
    void observeClusterInput(const Optional<PointSnapshot>& point) const;
    Optional<PointSnapshot> latestPoint(std::uint32_t index, std::int64_t nowMs) const;
    Optional<double> latestValue(std::uint32_t index, std::int64_t nowMs) const;
    // ttlMs > 0 overrides the (short) default output TTL, e.g. for minute-cadence statistics.
    CommandSubmitResult set(std::uint32_t index, double value, std::int64_t nowMs, std::int64_t ttlMs = 0);
    bool profileEnabled(const std::string& key, bool defaultValue) const;
    int profileInt(const std::string& key, int defaultValue) const;
    bool shouldRunNode(const GraphEmsNodeConfig& node) const;
    void restoreState(std::int64_t nowMs);
    void saveState(std::int64_t nowMs, bool force = false);
    std::vector<std::uint32_t> stateOutputIndexes() const;
    std::set<std::uint32_t> setpointOutputIndexes() const;
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
    bool runGridReserve(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runTimedChargeDischarge(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runPhotovoltaicCharge(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runPhaseBalance(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runFlexiblePhaseBalance(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runSkOverride(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runReserveCapacity(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runFormula(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runTimeSource(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runWindowAggregate(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runVoltageQualification(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runScheduleSelect(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runClusterDispatch(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runPhaseArbiter(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runPowerConstraint(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runSwitch(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runControlGate(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runFeedbackVerify(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runControlWrite(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runConfirmedControlWrite(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runRateLimit(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runHysteresis(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runDebounce(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runSequence(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runManualRecovery(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runPcsPowerSolve(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runPcsWriteback(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool submitPcsWritebackCommands(
        const GraphEmsNodeConfig& node,
        std::int64_t nowMs,
        GraphEmsRunResult& result,
        bool submitMissingZeroTargets
    );
    CommandSubmitResult submitDeviceWrite(
        const GraphEmsNodeConfig& node,
        PendingWriteCommand command,
        GraphEmsRunResult& result
    );

    GraphEmsConfig config_;
    GraphEmsEngineOptions options_;
    PointStoreRouter& router_;
    std::int64_t defaultTtlMs_ = 600000;
    std::string stateFile_;
    std::unordered_map<std::string, std::string> profile_;
    std::string ruleCode_;
    std::size_t maxDeviceWritesThisRun_ = 0;
    std::uint64_t commandSequence_ = 0;
    std::vector<const GraphEmsNodeConfig*> executionOrder_;
    std::vector<std::uint32_t> snapshotIndexes_;
    mutable bool snapshotActive_ = false;
    bool propagateClusterLineage_ = false;
    mutable bool clusterLineageInvalid_ = false;
    mutable Optional<ClusterWriteAuthorization> clusterInputAuthorization_;
    mutable std::unordered_map<std::uint32_t, PointSnapshot> snapshotPoints_;
    mutable std::unordered_map<std::uint32_t, double> snapshotValues_;
    mutable std::unordered_set<std::uint32_t> snapshotResolvedIndexes_;
    bool stateRestored_ = false;
    bool stateSaved_ = false;
    std::int64_t lastStateSaveAt_ = 0;
    std::unordered_map<std::uint32_t, std::vector<double>> averageWindows_;
    struct VoltageQualityPeriodState {
        int key = 0;
        std::uint64_t monitoredMinutes = 0;
        std::uint64_t overlimitMinutes = 0;
        std::uint64_t invalidMinutes = 0;
    };
    struct VoltageQualityState {
        std::int64_t currentMinuteKey = -1;
        std::int64_t lastSampleAt = 0;
        std::vector<double> sums;
        std::vector<std::uint32_t> counts;
        std::vector<std::int64_t> lastInputTimestamps;
        std::vector<double> lastAverages;
        int lastMinuteStatus = 0;
        double lastMinuteCoverage = 0.0;
        VoltageQualityPeriodState day;
        VoltageQualityPeriodState completedDay;
    };
    std::unordered_map<std::string, VoltageQualityState> voltageQualityStates_;
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
    std::unordered_map<std::string, ManualRecovery> manualRecoveryStates_;
    std::unordered_map<std::string, RecoveryFeedback> recoveryFeedbackStates_;
    std::unordered_map<std::string, ConfirmedControl> confirmedControls_;
    bool commandStateFault_ = false;
    std::shared_ptr<void> commandStateLock_;
    std::unordered_map<std::string, bool> gridReserveCharging_;
    // pcsPowerSolve fail-safe log throttling: node id -> (last reported fault set, last log time).
    std::unordered_map<std::string, std::pair<std::string, std::int64_t>> limitFaultLogs_;
};

}  // namespace edge_gateway
