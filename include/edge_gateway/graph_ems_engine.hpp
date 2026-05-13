#pragma once

#include <string>
#include <unordered_map>
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
    void saveState(std::int64_t nowMs) const;
    std::vector<std::uint32_t> stateOutputIndexes() const;
    bool runMeterAverage(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runDerivedLoad(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runBmsDerived(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runCosCompensation(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runVoltageCompensation(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runChargeDischarge(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runTimedChargeDischarge(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runPhotovoltaicCharge(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runPhaseBalance(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runSkOverride(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
    bool runReserveCapacity(const GraphEmsNodeConfig& node, std::int64_t nowMs, GraphEmsRunResult& result);
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
    bool stateRestored_ = false;
    std::unordered_map<std::uint32_t, std::vector<double>> averageWindows_;
};

}  // namespace edge_gateway
