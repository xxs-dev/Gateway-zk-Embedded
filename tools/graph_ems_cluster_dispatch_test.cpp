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
    const std::string& sharedMemoryName,
    bool writable = false
) {
    edge_gateway::PointStoreRoute route;
    route.index = index;
    route.machineCode = "CLUSTER_GRAPH_TEST";
    route.meterCode = "EMS_CLUSTER";
    route.pointCode = "POINT_" + std::to_string(index);
    route.interfaceCode = "compute";
    route.interfaceType = "compute";
    route.sharedMemoryName = sharedMemoryName;
    route.writable = writable;
    route.write.enable = writable;
    router.addRoute(route);
}

void putPoint(
    edge_gateway::PointStoreRouter& router,
    std::uint32_t index,
    double value,
    int quality,
    std::int64_t timestamp,
    std::int64_t expireAt
) {
    edge_gateway::PointValue point;
    point.index = index;
    point.value = value;
    point.quality = quality;
    point.ts = timestamp;
    point.expireAt = expireAt;
    const auto routed = router.putLatestByIndex(point);
    require(routed.accepted, "failed to seed point " + std::to_string(index));
}

void put(
    edge_gateway::PointStoreRouter& router,
    std::uint32_t index,
    double value,
    std::int64_t nowMs
) {
    putPoint(router, index, value, 1, nowMs, nowMs + 10000);
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

void testPowerConstraintFailsClosedForInvalidStateInputs(const std::string& suffix) {
    const std::vector<std::uint32_t> activeInputs = {726000, 726001, 726002};
    const std::vector<std::uint32_t> reactiveInputs = {726003, 726004, 726005};
    const std::vector<std::uint32_t> activeOutputs = {726010, 726011, 726012};
    const std::vector<std::uint32_t> reactiveOutputs = {726013, 726014, 726015};
    const std::vector<std::uint32_t> stateInputs = {726020, 726021, 726022};
    const std::vector<double> stateValues = {50.0, 95.0, 5.0};
    const std::vector<std::string> invalidModes = {"missing", "bad-quality", "expired"};
    const std::int64_t nowMs = 10000;

    edge_gateway::GraphEmsNodeConfig constraint;
    constraint.id = "soc-safety";
    constraint.type = "powerConstraint";
    constraint.params = {
        {"stateIndex", std::to_string(stateInputs[0])},
        {"stateUpperIndex", std::to_string(stateInputs[1])},
        {"stateLowerIndex", std::to_string(stateInputs[2])}
    };
    setIndexArray(constraint, "activeInputIndexes", activeInputs);
    setIndexArray(constraint, "reactiveInputIndexes", reactiveInputs);
    setIndexArray(constraint, "activeOutputIndexes", activeOutputs);
    setIndexArray(constraint, "reactiveOutputIndexes", reactiveOutputs);

    edge_gateway::GraphEmsConfig config;
    config.graphCode = "soc-safety-test";
    config.nodes.push_back(constraint);

    for (std::size_t invalidInput = 0; invalidInput < stateInputs.size(); ++invalidInput) {
        for (const auto& invalidMode : invalidModes) {
            const auto sharedMemoryName = "graph_ems_soc_safety_" + suffix + "_" +
                std::to_string(invalidInput) + "_" + invalidMode;
            edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
            try {
                edge_gateway::MemoryStoreConfig memoryConfig;
                memoryConfig.sharedMemoryName = sharedMemoryName;
                memoryConfig.maxLatestPoints = 64;
                memoryConfig.maxPendingWrites = 8;
                memoryConfig.maxPersistentSamples = 8;
                edge_gateway::MemoryPointStore store(memoryConfig);
                edge_gateway::PointStoreRouter router;
                router.addStore(sharedMemoryName, store);
                for (const auto index : activeInputs) addRoute(router, index, sharedMemoryName);
                for (const auto index : reactiveInputs) addRoute(router, index, sharedMemoryName);
                for (const auto index : activeOutputs) addRoute(router, index, sharedMemoryName);
                for (const auto index : reactiveOutputs) addRoute(router, index, sharedMemoryName);
                for (const auto index : stateInputs) addRoute(router, index, sharedMemoryName);

                const double activeValues[] = {12.0, -8.0, 4.0};
                for (std::size_t phase = 0; phase < activeInputs.size(); ++phase) {
                    put(router, activeInputs[phase], activeValues[phase], nowMs);
                    put(router, reactiveInputs[phase], static_cast<double>(phase + 1), nowMs);
                }
                for (std::size_t stateInput = 0; stateInput < stateInputs.size(); ++stateInput) {
                    if (stateInput == invalidInput && invalidMode == "missing") continue;
                    const auto quality = stateInput == invalidInput && invalidMode == "bad-quality" ? 0 : 1;
                    const auto expireAt = stateInput == invalidInput && invalidMode == "expired"
                        ? nowMs - 1 : nowMs + 10000;
                    putPoint(
                        router,
                        stateInputs[stateInput],
                        stateValues[stateInput],
                        quality,
                        nowMs - 10,
                        expireAt
                    );
                }

                edge_gateway::GraphEmsEngine engine(config, router, 1000);
                requireNoErrors(engine.runOnce(nowMs));
                for (const auto output : activeOutputs) {
                    requireNear(
                        latest(router, output, nowMs),
                        0.0,
                        "invalid SOC constraint input must fail closed: " + invalidMode
                    );
                }
            } catch (...) {
                edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
                throw;
            }
            edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
        }
    }
}

void testRestoredControlOutputKeepsOriginalTimestamp(const std::string& suffix) {
    constexpr std::uint32_t conditionIndex = 727000;
    constexpr std::uint32_t restoredOutputIndex = 727001;
    constexpr std::uint32_t deviceTargetIndex = 727002;
    constexpr std::int64_t storedAt = 1000;
    constexpr std::int64_t nowMs = 10000;
    const auto sharedMemoryName = "graph_ems_restore_safety_" + suffix;
    const auto stateFile = "graph_ems_restore_safety_" + suffix + ".json";
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
    std::remove(stateFile.c_str());
    try {
        edge_gateway::MemoryStoreConfig memoryConfig;
        memoryConfig.sharedMemoryName = sharedMemoryName;
        memoryConfig.maxLatestPoints = 16;
        memoryConfig.maxPendingWrites = 8;
        memoryConfig.maxPersistentSamples = 8;
        edge_gateway::MemoryPointStore store(memoryConfig);
        edge_gateway::PointStoreRouter router;
        router.addStore(sharedMemoryName, store);
        addRoute(router, conditionIndex, sharedMemoryName);
        addRoute(router, restoredOutputIndex, sharedMemoryName);
        addRoute(router, deviceTargetIndex, sharedMemoryName, true);

        edge_gateway::GraphEmsNodeConfig source;
        source.id = "source";
        source.type = "switch";
        source.params = {
            {"conditionIndex", std::to_string(conditionIndex)},
            {"trueValue", "25"},
            {"falseValue", "0"},
            {"outputIndex", std::to_string(restoredOutputIndex)}
        };
        edge_gateway::GraphEmsNodeConfig writer;
        writer.id = "writer";
        writer.type = "controlWrite";
        writer.params = {
            {"submitWrites", "true"},
            {"inputIndex", std::to_string(restoredOutputIndex)},
            {"targetIndex", std::to_string(deviceTargetIndex)},
            {"minValue", "-100"},
            {"maxValue", "100"}
        };
        edge_gateway::GraphEmsConfig config;
        config.graphCode = "restore-control-safety";
        config.nodes = {source, writer};
        config.edges.push_back({"source", "writer"});

        std::ofstream state(stateFile.c_str(), std::ios::out | std::ios::trunc);
        state << "{\"graphCode\":\"restore-control-safety\",\"points\":["
              << "{\"index\":" << restoredOutputIndex
              << ",\"value\":25,\"ts\":" << storedAt << "}]}";
        state.close();

        edge_gateway::GraphEmsEngine engine(config, router, 1000, stateFile);
        requireNoErrors(engine.runOnce(nowMs));
        const auto restored = router.getLatestByIndex(restoredOutputIndex, nowMs);
        require(static_cast<bool>(restored), "restored control output is missing");
        require(restored->ts == storedAt, "restored control output timestamp must not be refreshed");
        require(restored->stale, "expired restored control output must remain stale");
        require(router.peekPendingWrites().empty(),
                "expired restored control output must not submit a device command");
    } catch (...) {
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
        std::remove(stateFile.c_str());
        throw;
    }
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
    std::remove(stateFile.c_str());
}

void testLegacyPcsWritebackPreservesFractionalPower(const std::string& suffix) {
    constexpr std::uint32_t comStatusIndex = 728000;
    const std::vector<std::uint32_t> inputs = {728001, 728002, 728003};
    const std::vector<std::uint32_t> targets = {728010, 728011, 728012};
    const double expected[] = {2.9, -2.9, -0.9};
    const auto sharedMemoryName = "graph_ems_pcs_fraction_" + suffix;
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
    try {
        edge_gateway::MemoryStoreConfig memoryConfig;
        memoryConfig.sharedMemoryName = sharedMemoryName;
        memoryConfig.maxLatestPoints = 16;
        memoryConfig.maxPendingWrites = 8;
        memoryConfig.maxPersistentSamples = 8;
        edge_gateway::MemoryPointStore store(memoryConfig);
        edge_gateway::PointStoreRouter router;
        router.addStore(sharedMemoryName, store);
        addRoute(router, comStatusIndex, sharedMemoryName);
        for (const auto input : inputs) addRoute(router, input, sharedMemoryName);
        for (const auto target : targets) addRoute(router, target, sharedMemoryName, true);

        edge_gateway::GraphEmsNodeConfig writer;
        writer.id = "legacy-pcs-writeback";
        writer.type = "pcsWriteback";
        writer.params = {
            {"submitWrites", "true"},
            {"comStatusIndex", std::to_string(comStatusIndex)},
            {"paInput", std::to_string(inputs[0])},
            {"pbInput", std::to_string(inputs[1])},
            {"pcInput", std::to_string(inputs[2])},
            {"pControlAIndex", std::to_string(targets[0])},
            {"pControlBIndex", std::to_string(targets[1])},
            {"pControlCIndex", std::to_string(targets[2])}
        };
        edge_gateway::GraphEmsConfig config;
        config.graphCode = "legacy-pcs-fraction-test";
        config.nodes.push_back(writer);

        constexpr std::int64_t nowMs = 20000;
        put(router, comStatusIndex, 1.0, nowMs);
        for (std::size_t phase = 0; phase < inputs.size(); ++phase) {
            put(router, inputs[phase], expected[phase], nowMs);
        }

        edge_gateway::GraphEmsEngine engine(config, router, 1000);
        const auto result = engine.runOnce(nowMs);
        requireNoErrors(result);
        const auto writes = router.peekPendingWrites();
        require(writes.size() == inputs.size(),
                "legacy pcsWriteback must submit every non-zero fractional phase target");
        for (std::size_t phase = 0; phase < writes.size(); ++phase) {
            require(writes[phase].index == targets[phase], "legacy pcsWriteback target index mismatch");
            requireNear(
                writes[phase].value,
                expected[phase],
                "legacy pcsWriteback must preserve fractional power"
            );
        }
    } catch (...) {
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
        throw;
    }
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
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
        testPowerConstraintFailsClosedForInvalidStateInputs(suffix);
        testRestoredControlOutputKeepsOriginalTimestamp(suffix);
        testLegacyPcsWritebackPreservesFractionalPower(suffix);
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
