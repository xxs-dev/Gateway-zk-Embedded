#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "edge_gateway/legacy_ems_engine.hpp"
#include "edge_gateway/legacy_ems_point_catalog.hpp"
#include "edge_gateway/graph_ems_engine.hpp"
#include "edge_gateway/models.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace edge_gateway {

struct ComputeInputState {
    bool present = false;
    bool valid = false;
    bool stale = false;
    double value = 0.0;
    int quality = 0;
    std::int64_t ts = 0;
};

class ComputeEngineService {
public:
    ComputeEngineService(
        ComputeEngineConfig config,
        PointStoreRouter& router
    );
    ~ComputeEngineService();

    ComputeEngineService(const ComputeEngineService&) = delete;
    ComputeEngineService& operator=(const ComputeEngineService&) = delete;

    void start();
    void stop();
    void runOnce(std::int64_t nowMs);

private:
    struct RuleState {
        std::int64_t lastEvalMs = 0;
        std::unordered_map<std::uint32_t, StoredPointValue> lastInputs;
    };

    struct OutputState {
        std::int64_t lastWriteMs = 0;
        double lastValue = 0.0;
        int lastQuality = 0;
        bool hasLastValue = false;
    };

    struct LegacyRuntimeState {
        LegacyEmsPointCatalog catalog;
        std::unique_ptr<LegacyEmsEngine> engine;
    };

    struct GraphEmsRuntimeState {
        GraphEmsConfig config;
        std::unique_ptr<GraphEmsEngine> engine;
    };

    void loop();
    bool shouldEvaluate(
        const ComputeRuleConfig& rule,
        const std::unordered_map<std::uint32_t, StoredPointValue>& currentInputs,
        std::int64_t nowMs
    );
    void evaluateRule(
        const ComputeRuleConfig& rule,
        const std::unordered_map<std::uint32_t, StoredPointValue>& currentInputs,
        std::int64_t nowMs
    );
    LegacyEmsEngine& legacyEngineFor(const ComputeRuleConfig& rule);
    GraphEmsEngine& graphEmsEngineFor(const ComputeRuleConfig& rule);
    bool shouldSubmitOutput(
        const ComputeRuleConfig& rule,
        const ComputeOutputConfig& output,
        double value,
        int quality,
        std::int64_t nowMs
    );
    int outputQuality(
        const ComputeRuleConfig& rule,
        const ComputeOutputConfig& output,
        const std::unordered_map<std::string, ComputeInputState>& inputs,
        bool expressionOk
    ) const;
    std::string outputStateKey(const ComputeRuleConfig& rule, const ComputeOutputConfig& output) const;

    ComputeEngineConfig config_;
    PointStoreRouter& router_;
    std::atomic<bool> running_;
    std::thread worker_;
    std::unordered_map<std::string, RuleState> ruleStates_;
    std::unordered_map<std::string, OutputState> outputStates_;
    std::unordered_map<std::string, std::unique_ptr<LegacyRuntimeState>> legacyStates_;
    std::unordered_map<std::string, std::unique_ptr<GraphEmsRuntimeState>> graphEmsStates_;
};

}  // namespace edge_gateway
