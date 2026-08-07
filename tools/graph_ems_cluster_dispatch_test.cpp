#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "edge_gateway/ems_cluster.hpp"
#include "edge_gateway/graph_ems_engine.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace {

constexpr std::uint32_t kEnable = 724000;
constexpr std::uint32_t kRole = 724001;
constexpr std::uint32_t kQuorum = 724005;
constexpr std::uint32_t kDispatchValid = 724050;
constexpr std::uint32_t kDispatchReason = 724051;
constexpr std::uint32_t kStationStrategy = 724052;
const std::vector<std::uint32_t> kDispatchActive = {724030, 724031, 724032};
const std::vector<std::uint32_t> kDispatchReactive = {724033, 724034, 724035};
const std::vector<std::uint32_t> kActiveOutputs = {725000, 725001, 725002};
const std::vector<std::uint32_t> kReactiveOutputs = {725003, 725004, 725005};
constexpr std::uint32_t kValidOutput = 725006;
constexpr std::uint32_t kLeaderOutput = 725007;
constexpr std::uint32_t kReasonOutput = 725008;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void requireNear(double actual, double expected, const std::string& message) {
    if (std::fabs(actual - expected) > 1e-6) {
        throw std::runtime_error(
            message + " actual=" + std::to_string(actual) + " expected=" + std::to_string(expected)
        );
    }
}

std::string uniqueSuffix() {
    return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

void addRoute(
    edge_gateway::PointStoreRouter& router,
    std::uint32_t index,
    const std::string& sharedMemoryName
) {
    edge_gateway::PointStoreRoute route;
    route.index = index;
    route.machineCode = "CLUSTER_GRAPH_TEST";
    route.meterCode = "EMS_CLUSTER";
    route.pointCode = "POINT_" + std::to_string(index);
    route.interfaceCode = "compute";
    route.interfaceType = "compute";
    route.sharedMemoryName = sharedMemoryName;
    router.addRoute(route);
}

void put(
    edge_gateway::PointStoreRouter& router,
    std::uint32_t index,
    double value,
    std::int64_t nowMs
) {
    edge_gateway::PointValue point;
    point.index = index;
    point.value = value;
    point.quality = 1;
    point.ts = nowMs;
    point.expireAt = nowMs + 10000;
    const auto routed = router.putLatestByIndex(point);
    require(routed.accepted, "failed to seed point " + std::to_string(index));
}

double latest(
    edge_gateway::PointStoreRouter& router,
    std::uint32_t index,
    std::int64_t nowMs
) {
    const auto point = router.getLatestByIndex(index, nowMs);
    require(point && point->quality == 1 && !point->stale, "missing output " + std::to_string(index));
    return point->value;
}

void requireNoErrors(const edge_gateway::GraphEmsRunResult& result) {
    if (result.errors.empty()) return;
    std::string message = "graph EMS run failed";
    for (const auto& error : result.errors) message += "; " + error;
    throw std::runtime_error(message);
}

void setIndexArray(
    edge_gateway::GraphEmsNodeConfig& node,
    const std::string& name,
    const std::vector<std::uint32_t>& values
) {
    node.params[name + ".count"] = std::to_string(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        node.params[name + "." + std::to_string(i)] = std::to_string(values[i]);
    }
}

edge_gateway::GraphEmsConfig graphConfig(bool zeroOnInvalid) {
    edge_gateway::GraphEmsNodeConfig node;
    node.id = "cluster-dispatch";
    node.type = "clusterDispatch";
    node.params = {
        {"enableIndex", std::to_string(kEnable)},
        {"roleIndex", std::to_string(kRole)},
        {"quorumIndex", std::to_string(kQuorum)},
        {"dispatchValidIndex", std::to_string(kDispatchValid)},
        {"dispatchReasonIndex", std::to_string(kDispatchReason)},
        {"stationStrategyActiveIndex", std::to_string(kStationStrategy)},
        {"validOutputIndex", std::to_string(kValidOutput)},
        {"stationLeaderOutputIndex", std::to_string(kLeaderOutput)},
        {"reasonOutputIndex", std::to_string(kReasonOutput)},
        {"maxTargetAgeMs", "1000"},
        {"zeroOnInvalid", zeroOnInvalid ? "true" : "false"}
    };
    setIndexArray(node, "dispatchActiveIndexes", kDispatchActive);
    setIndexArray(node, "dispatchReactiveIndexes", kDispatchReactive);
    setIndexArray(node, "activeOutputIndexes", kActiveOutputs);
    setIndexArray(node, "reactiveOutputIndexes", kReactiveOutputs);

    edge_gateway::GraphEmsConfig config;
    config.graphCode = "cluster-dispatch-test";
    config.nodes.push_back(std::move(node));
    return config;
}

void seedDispatch(
    edge_gateway::PointStoreRouter& router,
    std::int64_t nowMs,
    int role,
    bool valid,
    edge_gateway::EmsClusterDispatchCode reason,
    bool stationStrategy
) {
    put(router, kEnable, 1.0, nowMs);
    put(router, kRole, static_cast<double>(role), nowMs);
    put(router, kQuorum, 1.0, nowMs);
    put(router, kDispatchValid, valid ? 1.0 : 0.0, nowMs);
    put(router, kDispatchReason, static_cast<double>(reason), nowMs);
    put(router, kStationStrategy, stationStrategy ? 1.0 : 0.0, nowMs);
    const double active[] = {12.5, -6.0, 3.25};
    const double reactive[] = {1.5, 2.5, -1.0};
    for (std::size_t i = 0; i < 3; ++i) {
        put(router, kDispatchActive[i], active[i], nowMs);
        put(router, kDispatchReactive[i], reactive[i], nowMs);
    }
}

void testValidationRejectsWrongPhaseCount(const std::string& path) {
    std::ofstream output(path.c_str(), std::ios::out | std::ios::trunc);
    output << R"({
  "schemaVersion": "1.0.0",
  "graphCode": "invalid-cluster-dispatch",
  "limits": {"maxNodes": 8, "maxEdges": 8},
  "nodes": [{
    "id": "cluster",
    "type": "clusterDispatch",
    "params": {
      "enableIndex": 1, "roleIndex": 2, "quorumIndex": 3,
      "dispatchValidIndex": 4, "dispatchReasonIndex": 5,
      "stationStrategyActiveIndex": 6,
      "dispatchActiveIndexes": [7, 8],
      "dispatchReactiveIndexes": [9, 10, 11],
      "activeOutputIndexes": [12, 13, 14],
      "reactiveOutputIndexes": [15, 16, 17],
      "validOutputIndex": 18, "stationLeaderOutputIndex": 19,
      "reasonOutputIndex": 20
    }
  }],
  "edges": []
})";
    output.close();
    bool rejected = false;
    try {
        edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration(path);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "clusterDispatch must reject a non-three-phase dispatch array");
}

}  // namespace

int main() {
    const auto suffix = uniqueSuffix();
    const auto sharedMemoryName = "graph_ems_cluster_dispatch_test_" + suffix;
    const auto invalidGraphPath = "/tmp/graph_ems_cluster_dispatch_invalid_" + suffix + ".json";
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
    try {
        edge_gateway::MemoryStoreConfig memoryConfig;
        memoryConfig.sharedMemoryName = sharedMemoryName;
        memoryConfig.maxLatestPoints = 128;
        memoryConfig.maxPendingWrites = 8;
        memoryConfig.maxPersistentSamples = 8;
        {
            edge_gateway::MemoryPointStore store(memoryConfig);
            edge_gateway::PointStoreRouter router;
            router.addStore(sharedMemoryName, store);
            std::vector<std::uint32_t> indexes = {
                kEnable, kRole, kQuorum, kDispatchValid, kDispatchReason, kStationStrategy,
                kValidOutput, kLeaderOutput, kReasonOutput
            };
            indexes.insert(indexes.end(), kDispatchActive.begin(), kDispatchActive.end());
            indexes.insert(indexes.end(), kDispatchReactive.begin(), kDispatchReactive.end());
            indexes.insert(indexes.end(), kActiveOutputs.begin(), kActiveOutputs.end());
            indexes.insert(indexes.end(), kReactiveOutputs.begin(), kReactiveOutputs.end());
            for (const auto index : indexes) addRoute(router, index, sharedMemoryName);

            edge_gateway::GraphEmsEngine engine(graphConfig(true), router, 10000);
            seedDispatch(
                router,
                1000,
                static_cast<int>(edge_gateway::EmsClusterRole::Follower),
                true,
                edge_gateway::EmsClusterDispatchCode::Clamped,
                false
            );
            requireNoErrors(engine.runOnce(1100));
            requireNear(latest(router, kActiveOutputs[0], 1100), 12.5, "active phase A mismatch");
            requireNear(latest(router, kActiveOutputs[1], 1100), -6.0, "active phase B mismatch");
            requireNear(latest(router, kReactiveOutputs[2], 1100), -1.0, "reactive phase C mismatch");
            requireNear(latest(router, kValidOutput, 1100), 1.0, "valid output mismatch");
            requireNear(latest(router, kLeaderOutput, 1100), 0.0, "follower leader gate mismatch");
            requireNear(
                latest(router, kReasonOutput, 1100),
                static_cast<double>(edge_gateway::EmsClusterDispatchCode::Clamped),
                "clamped result must be preserved"
            );
            require(router.peekPendingWrites().empty(), "clusterDispatch must not submit device writes");

            seedDispatch(
                router,
                1200,
                static_cast<int>(edge_gateway::EmsClusterRole::Follower),
                false,
                edge_gateway::EmsClusterDispatchCode::Interlocked,
                false
            );
            requireNoErrors(engine.runOnce(1250));
            requireNear(latest(router, kActiveOutputs[0], 1250), 0.0, "invalid dispatch must clear active target");
            requireNear(latest(router, kReactiveOutputs[1], 1250), 0.0, "invalid dispatch must clear reactive target");
            requireNear(latest(router, kValidOutput, 1250), 0.0, "invalid dispatch valid flag mismatch");
            requireNear(
                latest(router, kReasonOutput, 1250),
                static_cast<double>(edge_gateway::EmsClusterDispatchCode::Interlocked),
                "dispatch rejection reason mismatch"
            );

            seedDispatch(
                router,
                2000,
                static_cast<int>(edge_gateway::EmsClusterRole::Leader),
                true,
                edge_gateway::EmsClusterDispatchCode::Accepted,
                true
            );
            requireNoErrors(engine.runOnce(2100));
            requireNear(latest(router, kLeaderOutput, 2100), 1.0, "leader strategy gate mismatch");
            requireNoErrors(engine.runOnce(3101));
            requireNear(latest(router, kValidOutput, 3101), 0.0, "stale dispatch must become invalid");
            requireNear(latest(router, kActiveOutputs[0], 3101), 0.0, "stale dispatch must clear target");
            requireNear(
                latest(router, kReasonOutput, 3101),
                static_cast<double>(edge_gateway::EmsClusterDispatchCode::Expired),
                "stale dispatch reason mismatch"
            );

            seedDispatch(
                router,
                4000,
                static_cast<int>(edge_gateway::EmsClusterRole::Follower),
                true,
                edge_gateway::EmsClusterDispatchCode::Accepted,
                false
            );
            edge_gateway::GraphEmsEngine holdEngine(graphConfig(false), router, 10000);
            requireNoErrors(holdEngine.runOnce(4050));
            put(router, kDispatchValid, 0.0, 4100);
            put(
                router,
                kDispatchReason,
                static_cast<double>(edge_gateway::EmsClusterDispatchCode::NoQuorum),
                4100
            );
            requireNoErrors(holdEngine.runOnce(4150));
            requireNear(latest(router, kActiveOutputs[0], 4150), 12.5, "zeroOnInvalid=false must hold target");
            requireNear(latest(router, kValidOutput, 4150), 0.0, "held target must still be marked invalid");
        }
        testValidationRejectsWrongPhaseCount(invalidGraphPath);
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
        std::remove(invalidGraphPath.c_str());
        std::cout << "graph_ems_cluster_dispatch_test passed\n";
        return 0;
    } catch (const std::exception& ex) {
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
        std::remove(invalidGraphPath.c_str());
        std::cerr << "graph_ems_cluster_dispatch_test failed: " << ex.what() << '\n';
        return 1;
    }
}
