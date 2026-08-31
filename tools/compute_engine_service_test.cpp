#include <chrono>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "edge_gateway/compute_engine_service.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace {

using namespace edge_gateway;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ComputeRuleConfig makeConstantRule(
    const std::string& ruleCode,
    std::uint32_t outputIndex,
    double value
) {
    ComputeRuleConfig rule;
    rule.ruleCode = ruleCode;
    rule.enabled = true;
    rule.trigger.type = "interval";
    rule.trigger.intervalMs = 1;
    rule.script.type = "expression";
    rule.script.expression = std::to_string(value);

    ComputeOutputConfig output;
    output.name = ruleCode + "_output";
    output.index = outputIndex;
    output.mode = "latestOnly";
    output.qualityPolicy = "always_good";
    rule.outputs.push_back(output);
    return rule;
}

void testRuleBudgetUsesRoundRobinFairness() {
    const std::string storeName = "compute_engine_service_fairness_test";
    MemoryPointStore::cleanupOrphanedSegment(storeName);

    MemoryStoreConfig storeConfig;
    storeConfig.sharedMemoryName = storeName;
    storeConfig.maxLatestPoints = 16;
    std::unique_ptr<MemoryPointStore> store(new MemoryPointStore(storeConfig));

    PointStoreRouter router;
    router.addStore(storeName, *store);
    for (std::uint32_t index = 620001; index <= 620003; ++index) {
        PointStoreRoute route;
        route.index = index;
        route.sourceIndex = index;
        route.machineCode = "GW_COMPUTE_TEST";
        route.meterCode = "COMPUTE_TEST";
        route.pointCode = "output_" + std::to_string(index);
        route.sharedMemoryName = storeName;
        router.addRoute(route);
    }

    ComputeEngineConfig config;
    config.enabled = true;
    config.maxRuleEvalPerScan = 1;
    config.rules = {
        makeConstantRule("rule_a", 620001, 1.0),
        makeConstantRule("rule_b", 620002, 2.0),
        makeConstantRule("rule_c", 620003, 3.0)
    };

    ComputeEngineService service(config, router);
    service.runOnce(1000);
    service.runOnce(1001);
    service.runOnce(1002);

    for (std::uint32_t index = 620001; index <= 620003; ++index) {
        const auto value = router.getLatestByIndex(index, 1002);
        require(
            static_cast<bool>(value),
            "round-robin budget must eventually evaluate output index " + std::to_string(index)
        );
        require(
            value->value == static_cast<double>(index - 620000),
            "unexpected compute output value for index " + std::to_string(index)
        );
    }

    store.reset();
    MemoryPointStore::cleanupOrphanedSegment(storeName);
}

void testHealthReportContainsDetailedMetrics() {
    const std::string storeName = "compute_engine_service_health_test";
    const std::string healthFile = "/tmp/compute_engine_service_health_test.json";
    std::remove(healthFile.c_str());
    MemoryPointStore::cleanupOrphanedSegment(storeName);

    MemoryStoreConfig storeConfig;
    storeConfig.sharedMemoryName = storeName;
    storeConfig.maxLatestPoints = 8;
    std::unique_ptr<MemoryPointStore> store(new MemoryPointStore(storeConfig));

    PointStoreRouter router;
    router.addStore(storeName, *store);
    PointStoreRoute route;
    route.index = 630001;
    route.sourceIndex = route.index;
    route.machineCode = "GW_COMPUTE_HEALTH_TEST";
    route.meterCode = "COMPUTE_HEALTH_TEST";
    route.pointCode = "health_output";
    route.sharedMemoryName = storeName;
    router.addRoute(route);

    ComputeEngineConfig config;
    config.enabled = true;
    config.scanIntervalMs = 5;
    config.maxRuleEvalPerScan = 1;
    config.healthFile = healthFile;
    config.healthPublishIntervalMs = 1;
    config.healthWindowCycles = 20;
    config.rules = {makeConstantRule("health_rule", 630001, 7.0)};

    {
        ComputeEngineService service(config, router);
        service.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        service.stop();
    }

    std::ifstream input(healthFile);
    const std::string payload((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    require(!payload.empty(), "compute health file must be written");
    require(payload.find("\"scanP50Ms\"") != std::string::npos, "compute health missing scanP50Ms");
    require(payload.find("\"scanP99Ms\"") != std::string::npos, "compute health missing scanP99Ms");
    require(payload.find("\"deadlineMissCycles\"") != std::string::npos,
        "compute health missing deadlineMissCycles");
    require(payload.find("\"evaluatedRules\":1") != std::string::npos,
        "compute health must count evaluated rules");
    require(payload.find("\"outputsWritten\":1") != std::string::npos,
        "compute health must count output writes");

    std::remove(healthFile.c_str());
    store.reset();
    MemoryPointStore::cleanupOrphanedSegment(storeName);
}

}  // namespace

int main() {
    try {
        testRuleBudgetUsesRoundRobinFairness();
        testHealthReportContainsDetailedMetrics();
        std::cout << "compute_engine_service_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "compute_engine_service_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
