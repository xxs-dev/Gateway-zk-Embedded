#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "edge_gateway/models.hpp"
#include "edge_gateway/point_store_router.hpp"
#include "edge_gateway/scada_models.hpp"

namespace edge_gateway {

struct ScadaSafetyActionResult {
    std::string actionId;
    std::string tagId;
    bool accepted = false;
    std::string message;
};

struct ScadaSafetyEvaluation {
    bool active = false;
    bool heartbeatFresh = false;
    bool triggered = false;
    bool completed = false;
    std::string message;
    std::vector<ScadaSafetyActionResult> actions;
};

class ScadaUpperComputerSafetyMonitor {
public:
    ScadaUpperComputerSafetyMonitor(
        SystemMonitorConfig::ScadaUpperComputerSafetyConfig config,
        std::string machineCode,
        PointStoreRouter& router
    );

    ScadaSafetyEvaluation runOnce(std::int64_t nowMs);

    static void writeHeartbeat(
        const std::string& leaseFile,
        const std::string& machineCode,
        std::int64_t nowMs
    );

private:
    void reloadProject(std::int64_t nowMs);
    std::int64_t readHeartbeat() const;
    std::string projectSignature(const ScadaProject& project) const;

    SystemMonitorConfig::ScadaUpperComputerSafetyConfig config_;
    std::string machineCode_;
    PointStoreRouter& router_;
    std::unique_ptr<ScadaProject> project_;
    std::int64_t lastReloadAtMs_ = 0;
    std::int64_t activatedAtMs_ = 0;
    std::string signature_;
    bool completed_ = false;
    std::unordered_set<std::string> acceptedActions_;
};

}  // namespace edge_gateway
