#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "edge_gateway/legacy_ems_point_catalog.hpp"
#include "edge_gateway/models.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace edge_gateway {

struct LegacyEmsRunResult {
    std::size_t latestWrites = 0;
    std::size_t deviceWrites = 0;
};

class LegacyEmsEngine {
public:
    LegacyEmsEngine(
        const LegacyEmsPointCatalog& catalog,
        PointStoreRouter& router,
        std::int64_t defaultTtlMs = 600000,
        std::unordered_map<std::string, std::string> profile = {}
    );

    double get(std::uint32_t index, std::int64_t nowMs) const;
    CommandSubmitResult set(std::uint32_t index, double value, std::int64_t nowMs);
    CommandSubmitResult cmd(std::uint32_t index, double value, std::int64_t nowMs);
    LegacyEmsRunResult runOnce(std::int64_t nowMs);

private:
    Optional<double> latestValue(std::uint32_t index, std::int64_t nowMs) const;
    bool hasPoint(std::uint32_t index) const;
    bool hasRoute(std::uint32_t index) const;
    bool updateAverage(
        std::uint32_t inputIndex,
        std::uint32_t outputIndex,
        std::size_t windowSize,
        std::int64_t nowMs,
        LegacyEmsRunResult& result
    );
    bool updateAverageSet(
        const std::vector<std::pair<std::uint32_t, std::uint32_t>>& mappings,
        std::size_t windowSize,
        std::int64_t nowMs,
        LegacyEmsRunResult& result
    );
    Optional<double> updateAverageWindowOnly(
        std::uint32_t inputIndex,
        std::uint32_t windowKey,
        std::size_t windowSize,
        std::int64_t nowMs
    );
    bool profileEnabled(const std::string& key, bool defaultValue) const;
    int profileInt(const std::string& key, int defaultValue) const;
    bool updateTqPowerUnbalance(std::int64_t nowMs, LegacyEmsRunResult& result);
    bool updateDerivedLoadFromTqAndCn(std::int64_t nowMs, LegacyEmsRunResult& result);
    bool updateDerivedLoadFromTqAndBw(std::int64_t nowMs, LegacyEmsRunResult& result);
    bool updateBmsTodayEnergy(std::int64_t nowMs, LegacyEmsRunResult& result);
    bool updateCosTargets(std::int64_t nowMs, LegacyEmsRunResult& result);
    bool updateLvHvTargets(std::int64_t nowMs, LegacyEmsRunResult& result);
    bool updateCdFdTargets(std::int64_t nowMs, LegacyEmsRunResult& result);
    bool updateSkTargets(std::int64_t nowMs, LegacyEmsRunResult& result);
    bool updatePhTargets(std::int64_t nowMs, LegacyEmsRunResult& result);
    bool updateGfTargets(std::int64_t nowMs, LegacyEmsRunResult& result);
    bool updateDsTargets(std::int64_t nowMs, LegacyEmsRunResult& result);
    bool updatePowerSolveOutputs(std::int64_t nowMs, LegacyEmsRunResult& result);
    void submitPcsPowerCommands(
        double pcsPaOut,
        double pcsPbOut,
        double pcsPcOut,
        double pcsQaOut,
        double pcsQbOut,
        double pcsQcOut,
        std::int64_t nowMs,
        LegacyEmsRunResult& result
    );
    CommandSubmitResult putLatest(std::uint32_t index, double value, std::int64_t nowMs);
    CommandSubmitResult submitCommand(std::uint32_t index, double value, std::int64_t nowMs);

    const LegacyEmsPointCatalog& catalog_;
    PointStoreRouter& router_;
    std::int64_t defaultTtlMs_ = 600000;
    std::unordered_map<std::string, std::string> profile_;
    std::unordered_map<std::uint32_t, std::deque<double>> averageWindows_;
};

}  // namespace edge_gateway
