#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "edge_gateway/graph_ems_engine.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string uniqueSuffix() {
    return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

std::string readText(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input.is_open()) {
        return std::string();
    }
    return std::string(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>()
    );
}

std::string projectFilePath(const std::string& relativePath) {
    const std::string candidates[] = {
        relativePath,
        "../" + relativePath,
        "../../" + relativePath
    };
    for (const auto& candidate : candidates) {
        std::ifstream input(candidate, std::ios::in | std::ios::binary);
        if (input.is_open()) {
            return candidate;
        }
    }
    throw std::runtime_error("cannot locate project file " + relativePath);
}

std::string fixturePath() {
    return projectFilePath("tools/testdata/graph_ems_v2_direct.logic.json");
}

void writeText(const std::string& path, const std::string& text) {
    std::ofstream output(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("cannot create test file " + path);
    }
    output << text;
}

std::string withPreserveImportedBehavior(std::string json, bool enabled) {
    const std::string marker = "$PRESERVE_IMPORTED_BEHAVIOR";
    const auto offset = json.find(marker);
    require(offset != std::string::npos, "migration compatibility fixture marker is missing");
    json.replace(offset, marker.size(), enabled ? "true" : "false");
    return json;
}

std::string duplicateTargetGraph(bool preserveImportedBehavior) {
    return withPreserveImportedBehavior(R"json({
  "schemaVersion":"2.0.0",
  "graphCode":"duplicate-target-migration",
  "compile":{
    "maxNodes":8,"maxEdges":8,"virtualIndexStart":700000,"virtualIndexEnd":700010,
    "preserveImportedBehavior":$PRESERVE_IMPORTED_BEHAVIOR
  },
  "nodes":[
    {
      "id":"write_first","type":"controlWrite","order":10,
      "parameters":{"inputIndex":100,"targetIndex":200,"minValue":-100,"maxValue":100,"submitWrites":true},
      "ports":[
        {"id":"input","direction":"input","valueType":"number","runtimePath":"/inputIndex",
         "binding":{"kind":"point","index":100}},
        {"id":"target","direction":"target","valueType":"number","runtimePath":"/targetIndex",
         "binding":{"kind":"point","index":200}}
      ]
    },
    {
      "id":"write_second","type":"controlWrite","order":20,
      "parameters":{"inputIndex":101,"targetIndex":200,"minValue":-100,"maxValue":100,"submitWrites":true},
      "ports":[
        {"id":"input","direction":"input","valueType":"number","runtimePath":"/inputIndex",
         "binding":{"kind":"point","index":101}},
        {"id":"target","direction":"target","valueType":"number","runtimePath":"/targetIndex",
         "binding":{"kind":"point","index":200}}
      ]
    }
  ],
  "links":[
    {"id":"write_order","kind":"dependency","fromNodeId":"write_first","toNodeId":"write_second"}
  ]
})json", preserveImportedBehavior);
}

std::string duplicateOutputGraph(bool preserveImportedBehavior) {
    return withPreserveImportedBehavior(R"json({
  "schemaVersion":"2.0.0",
  "graphCode":"duplicate-output-migration",
  "compile":{
    "maxNodes":8,"maxEdges":8,"virtualIndexStart":700000,"virtualIndexEnd":700010,
    "preserveImportedBehavior":$PRESERVE_IMPORTED_BEHAVIOR
  },
  "nodes":[
    {
      "id":"schedule_output","type":"timeSource","order":10,
      "parameters":{"component":"hour","outputIndex":700001},
      "ports":[{
        "id":"value","direction":"output","valueType":"number","runtimePath":"/outputIndex",
        "binding":{"kind":"automatic","index":700001,"constant":null}
      }]
    },
    {
      "id":"cycle_output","type":"timeSource","order":20,
      "parameters":{"component":"minute","outputIndex":700001},
      "ports":[{
        "id":"value","direction":"output","valueType":"number","runtimePath":"/outputIndex",
        "binding":{"kind":"automatic","index":700001}
      }]
    }
  ],
  "links":[
    {"id":"preserve_order","kind":"dependency","fromNodeId":"schedule_output","toNodeId":"cycle_output"}
  ]
})json", preserveImportedBehavior);
}

std::string weakTypeLinkGraph(bool preserveImportedBehavior) {
    return withPreserveImportedBehavior(R"json({
  "schemaVersion":"2.0.0",
  "graphCode":"weak-type-link-migration",
  "compile":{
    "maxNodes":8,"maxEdges":8,"virtualIndexStart":700000,"virtualIndexEnd":700010,
    "preserveImportedBehavior":$PRESERVE_IMPORTED_BEHAVIOR
  },
  "nodes":[
    {
      "id":"source","type":"pointInput","order":0,"parameters":{"inputIndex":100},
      "ports":[
        {"id":"value","direction":"output","valueType":"power","unit":"kW",
         "runtimePath":"/inputIndex","binding":{"kind":"point","index":100}}
      ]
    },
    {
      "id":"limit","type":"rateLimit","order":10,
      "parameters":{
        "inputIndex":100,"outputIndex":700000,"risePerSecond":10,"fallPerSecond":10,
        "minValue":-100,"maxValue":100
      },
      "ports":[
        {"id":"input","direction":"input","valueType":"state","unit":"V",
         "runtimePath":"/inputIndex","binding":{"kind":"automatic","index":100}},
        {"id":"output","direction":"output","valueType":"number","runtimePath":"/outputIndex",
         "binding":{"kind":"automatic","index":700000}}
      ]
    }
  ],
  "links":[
    {"id":"weak_data","kind":"data","fromNodeId":"source","fromPortId":"value",
     "toNodeId":"limit","toPortId":"input"}
  ]
})json", preserveImportedBehavior);
}

std::string unavailableScheduleGraph(std::int64_t nowMs) {
    const auto seconds = static_cast<std::time_t>(nowMs / 1000);
    const auto* local = std::localtime(&seconds);
    require(local != nullptr, "cannot resolve local hour for schedule test");
    std::ostringstream out;
    out << R"json({
  "schemaVersion":"2.0.0",
  "graphCode":"unavailable-indexed-schedule",
  "compile":{"maxNodes":4,"maxEdges":4,"virtualIndexStart":700000,"virtualIndexEnd":700010},
  "nodes":[{
    "id":"schedule","type":"scheduleSelect","order":0,
    "parameters":{
      "scheduleCurve":[{"hour":)json" << local->tm_hour << R"json(,"powerIndex":100,"targetSocIndex":101,"modeIndex":102}],
      "powerOutputIndex":700000,"socOutputIndex":700001,"modeOutputIndex":700002
    },
    "ports":[
      {"id":"power","direction":"input","valueType":"power","runtimePath":"/scheduleCurve/0/powerIndex","binding":{"kind":"point","index":100}},
      {"id":"soc","direction":"input","valueType":"soc","runtimePath":"/scheduleCurve/0/targetSocIndex","binding":{"kind":"point","index":101}},
      {"id":"mode","direction":"input","valueType":"enum","runtimePath":"/scheduleCurve/0/modeIndex","binding":{"kind":"point","index":102}},
      {"id":"power_output","direction":"output","valueType":"power","runtimePath":"/powerOutputIndex","binding":{"kind":"automatic","index":700000}},
      {"id":"soc_output","direction":"output","valueType":"soc","runtimePath":"/socOutputIndex","binding":{"kind":"automatic","index":700001}},
      {"id":"mode_output","direction":"output","valueType":"enum","runtimePath":"/modeOutputIndex","binding":{"kind":"automatic","index":700002}}
    ]
  }],
  "links":[]
})json";
    return out.str();
}

void expectLoadFailure(const std::string& path, const std::string& expectedMessage) {
    try {
        edge_gateway::GraphEmsConfig::loadFromFile(path);
    } catch (const std::exception& ex) {
        require(
            std::string(ex.what()).find(expectedMessage) != std::string::npos,
            "unexpected load error: " + std::string(ex.what())
        );
        return;
    }
    throw std::runtime_error("expected GraphEms V2 load failure for " + path);
}

bool hasLoadWarning(const edge_gateway::GraphEmsConfig& config, const std::string& expected) {
    for (const auto& warning : config.loadWarnings) {
        if (warning.find(expected) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void addRoute(
    edge_gateway::PointStoreRouter& router,
    std::uint32_t index,
    const std::string& sharedMemoryName,
    bool writable = false
) {
    edge_gateway::PointStoreRoute route;
    route.index = index;
    route.machineCode = "GRAPH_EMS_V2_TEST";
    route.meterCode = "EMS_CORE";
    route.pointCode = "P_" + std::to_string(index);
    route.interfaceCode = "compute";
    route.interfaceType = "computed";
    route.sharedMemoryName = sharedMemoryName;
    route.writable = writable;
    route.isStore = true;
    route.persistIntervalSec = 60;
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
    point.expireAt = nowMs + 600000;
    const auto result = router.putLatestByIndex(point);
    require(result.accepted, "failed to seed V2 input point");
}

double latest(edge_gateway::PointStoreRouter& router, std::uint32_t index, std::int64_t nowMs) {
    const auto value = router.getLatestByIndex(index, nowMs);
    require(static_cast<bool>(value), "missing V2 output point " + std::to_string(index));
    return value->value;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 3 && std::string(argv[1]) == "--check-file") {
        try {
            const auto config = edge_gateway::GraphEmsConfig::loadFromFile(argv[2]);
            std::cout << "graphCode=" << config.graphCode
                      << " nodes=" << config.nodes.size()
                      << " edges=" << config.edges.size()
                      << " warnings=" << config.loadWarnings.size() << "\n";
            for (const auto& warning : config.loadWarnings) {
                std::cout << "warning: " << warning << "\n";
            }
            return 0;
        } catch (const std::exception& ex) {
            std::cerr << "GraphEms V2 file check failed: " << ex.what() << "\n";
            return 1;
        }
    }
    if (argc != 1) {
        std::cerr << "usage: graph_ems_v2_loader_test [--check-file FILE]\n";
        return 2;
    }
    const auto suffix = uniqueSuffix();
    const auto legacyFile = "graph_ems_v2_legacy_" + suffix + ".json";
    const auto mismatchFile = "graph_ems_v2_mismatch_" + suffix + ".logic.json";
    const auto unresolvedFile = "graph_ems_v2_unresolved_" + suffix + ".logic.json";
    const auto semanticKindFile = "graph_ems_v2_semantic_kind_" + suffix + ".logic.json";
    const auto rangeFile = "graph_ems_v2_range_" + suffix + ".logic.json";
    const auto strictDuplicateOutputFile = "graph_ems_v2_duplicate_output_strict_" + suffix + ".logic.json";
    const auto preservedDuplicateOutputFile = "graph_ems_v2_duplicate_output_preserved_" + suffix + ".logic.json";
    const auto strictDuplicateTargetFile = "graph_ems_v2_duplicate_target_strict_" + suffix + ".logic.json";
    const auto preservedDuplicateTargetFile = "graph_ems_v2_duplicate_target_preserved_" + suffix + ".logic.json";
    const auto strictWeakTypeFile = "graph_ems_v2_weak_type_strict_" + suffix + ".logic.json";
    const auto preservedWeakTypeFile = "graph_ems_v2_weak_type_preserved_" + suffix + ".logic.json";
    const auto unknownValueTypeFile = "graph_ems_v2_unknown_value_type_" + suffix + ".logic.json";
    const auto unavailableScheduleFile = "graph_ems_v2_unavailable_schedule_" + suffix + ".logic.json";
    const auto sharedMemoryName = "gateway_graph_ems_v2_test_" + suffix;
    try {
        require(
            std::string(edge_gateway::GraphEmsRuntimeCapabilities::runtimeSchema()) == "2.x",
            "runtime capability must advertise V2"
        );
        require(
            std::string(edge_gateway::GraphEmsRuntimeCapabilities::editorSourceSchema()) == "2.x",
            "editor capability must advertise V2"
        );
        require(
            edge_gateway::GraphEmsRuntimeCapabilities::executesEditorSource(),
            "edge must execute the V2 source graph directly"
        );

        const char* deployableGraphs[] = {
            "config/examples/graph-ems-generic-nodes-example.json",
            "config/examples/graph-ems-sequence-example.json",
            "config/examples/shuntong_ems_graph.json",
            "config/examples/shuntong_ems_modular_graph.json",
            "config/examples/voltage-quality-daily-graph.json",
            "config/examples/comm104-voltage-quality-daily-graph.json",
            "config/factory/runtime/logic/shuntong_ems_graph.json"
        };
        for (const auto* relativePath : deployableGraphs) {
            const auto deployable = edge_gateway::GraphEmsConfig::loadFromFile(
                projectFilePath(relativePath)
            );
            require(
                deployable.schemaVersion == "2" || deployable.schemaVersion.rfind("2.", 0) == 0,
                std::string("deployable GraphEms artifact is not V2: ") + relativePath
            );
        }

        const auto fixture = fixturePath();
        const auto config = edge_gateway::GraphEmsConfig::loadFromFile(fixture);
        require(config.schemaVersion == "2.0.0", "V2 schema was not retained");
        require(config.nodes.size() == 2, "pointInput alias was not folded out of the execution plan");
        require(config.nodes[0].id == "calculate" && config.nodes[1].id == "limit", "node order is not deterministic");
        require(config.edges.size() == 1, "pointInput dependency should be folded while runtime dependency remains");
        require(config.edges[0].from == "calculate" && config.edges[0].to == "limit", "runtime edge is incorrect");
        require(config.nodes[0].params.at("inputs.0.index") == "100", "linked pointInput index was not materialized");
        require(config.nodes[0].params.at("inputs.1.value") == "2", "constant binding was not materialized");
        require(config.nodes[0].params.at("outputIndex") == "700000", "automatic output index was not materialized");

        edge_gateway::MemoryStoreConfig memoryConfig;
        memoryConfig.sharedMemoryName = sharedMemoryName;
        memoryConfig.maxLatestPoints = 32;
        memoryConfig.maxPendingWrites = 8;
        memoryConfig.maxPersistentSamples = 32;
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
        {
            edge_gateway::MemoryPointStore store(memoryConfig);
            edge_gateway::PointStoreRouter router;
            router.addStore(sharedMemoryName, store);
            addRoute(router, 100, sharedMemoryName);
            addRoute(router, 700000, sharedMemoryName);
            addRoute(router, 700001, sharedMemoryName);
            const std::int64_t nowMs = 1721001600000LL;
            put(router, 100, 5.0, nowMs);
            edge_gateway::GraphEmsEngine engine(config, router);
            auto result = engine.runOnce(nowMs);
            require(result.errors.empty(), "V2 direct execution first cycle failed");
            result = engine.runOnce(nowMs + 1000);
            require(result.errors.empty(), "V2 direct execution second cycle failed");
            require(std::fabs(latest(router, 700000, nowMs + 1000) - 7.0) < 0.000001, "formula output mismatch");
            require(std::fabs(latest(router, 700001, nowMs + 1000) - 7.0) < 0.000001, "linked rate output mismatch");
        }
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);

        const std::int64_t scheduleNowMs = 1721001600000LL;
        writeText(unavailableScheduleFile, unavailableScheduleGraph(scheduleNowMs));
        const auto unavailableSchedule = edge_gateway::GraphEmsConfig::loadFromFile(unavailableScheduleFile);
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
        {
            edge_gateway::MemoryPointStore store(memoryConfig);
            edge_gateway::PointStoreRouter router;
            router.addStore(sharedMemoryName, store);
            const std::uint32_t indexes[] = {100, 101, 102, 700000, 700001, 700002};
            for (const auto index : indexes) {
                addRoute(router, index, sharedMemoryName);
            }
            edge_gateway::GraphEmsEngine engine(unavailableSchedule, router);
            const auto result = engine.runOnce(scheduleNowMs);
            require(result.errors.empty(), "unavailable indexed schedule produced an execution error");
            require(result.latestWrites == 0, "unavailable indexed schedule must not overwrite outputs");
            require(!router.getLatestByIndex(700000, scheduleNowMs), "unavailable schedule wrote power output");
            require(!router.getLatestByIndex(700001, scheduleNowMs), "unavailable schedule wrote SOC output");
            require(!router.getLatestByIndex(700002, scheduleNowMs), "unavailable schedule wrote mode output");
        }
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);

        writeText(legacyFile, R"json({
  "schemaVersion":"1.0.0",
  "graphCode":"legacy-migration-input",
  "limits":{"maxNodes":4,"maxEdges":4},
  "nodes":[],
  "edges":[]
})json");
        expectLoadFailure(legacyFile, "requires schemaVersion major 2");
        const auto legacy = edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration(legacyFile);
        require(legacy.schemaVersion == "1.0.0", "explicit V1 migration loader failed");

        auto mismatched = readText(fixture);
        const auto outputValue = mismatched.find("\"outputIndex\": 700000");
        require(outputValue != std::string::npos, "fixture outputIndex marker is missing");
        mismatched.replace(outputValue, std::string("\"outputIndex\": 700000").size(), "\"outputIndex\": 700099");
        writeText(mismatchFile, mismatched);
        expectLoadFailure(mismatchFile, "binding.index does not match node.parameters");

        writeText(unresolvedFile, R"json({
  "schemaVersion":"2.0.0",
  "graphCode":"unresolved-binding",
  "compile":{"maxNodes":4,"maxEdges":4,"virtualIndexStart":700000,"virtualIndexEnd":700010},
  "nodes":[{
    "id":"source","type":"pointInput","order":0,"parameters":{},
    "ports":[{
      "id":"value","direction":"output","valueType":"number","runtimePath":"/inputIndex",
      "binding":{"kind":"point","pointCode":"only_code","semanticRole":"only.role"}
    }]
  }],
  "links":[]
})json");
        expectLoadFailure(unresolvedFile, "without a materialized binding.index");

        writeText(semanticKindFile, R"json({
  "schemaVersion":"2.0.0",
  "graphCode":"semantic-kind-is-not-valid",
  "compile":{"maxNodes":4,"maxEdges":4,"virtualIndexStart":700000,"virtualIndexEnd":700010},
  "nodes":[{
    "id":"source","type":"pointInput","order":0,"parameters":{"inputIndex":100},
    "ports":[{
      "id":"value","direction":"output","valueType":"number","runtimePath":"/inputIndex",
      "binding":{"kind":"semantic","index":100,"semanticRole":"fixture.source"}
    }]
  }],
  "links":[]
})json");
        expectLoadFailure(semanticKindFile, "unsupported binding kind");

        writeText(rangeFile, R"json({
  "schemaVersion":"2.0.0",
  "graphCode":"automatic-range",
  "compile":{"maxNodes":4,"maxEdges":4,"virtualIndexStart":700000,"virtualIndexEnd":700010},
  "nodes":[{
    "id":"clock","type":"timeSource","order":0,
    "parameters":{"component":"hour","outputIndex":800000},
    "ports":[{
      "id":"value","direction":"output","valueType":"number","runtimePath":"/outputIndex",
      "binding":{"kind":"automatic","index":800000}
    }]
  }],
  "links":[]
        })json");
        expectLoadFailure(rangeFile, "outside compile virtual range");

        writeText(strictDuplicateOutputFile, duplicateOutputGraph(false));
        expectLoadFailure(strictDuplicateOutputFile, "duplicate executable output indexes: 700001");

        writeText(preservedDuplicateOutputFile, duplicateOutputGraph(true));
        const auto preservedDuplicateOutput =
            edge_gateway::GraphEmsConfig::loadFromFile(preservedDuplicateOutputFile);
        require(preservedDuplicateOutput.nodes.size() == 2, "preserved duplicate-output graph lost nodes");
        require(
            preservedDuplicateOutput.nodes[0].id == "schedule_output" &&
                preservedDuplicateOutput.nodes[1].id == "cycle_output",
            "preserved duplicate-output graph changed node order"
        );
        require(
            preservedDuplicateOutput.edges.size() == 1 &&
                preservedDuplicateOutput.edges[0].from == "schedule_output" &&
                preservedDuplicateOutput.edges[0].to == "cycle_output",
            "preserved duplicate-output graph changed its dependency edge"
        );
        require(
            hasLoadWarning(preservedDuplicateOutput, "duplicate executable output index 700001"),
            "preserved duplicate output did not identify the shared index"
        );
        require(
            hasLoadWarning(preservedDuplicateOutput, "schedule_output.value (order=10)"),
            "preserved duplicate output warning omitted the first node and order"
        );
        require(
            hasLoadWarning(preservedDuplicateOutput, "cycle_output.value (order=20)"),
            "preserved duplicate output warning omitted the duplicate node and order"
        );
        require(
            hasLoadWarning(preservedDuplicateOutput, "profile misconfiguration may cause same-cycle overwrite"),
            "preserved duplicate output warning omitted the profile risk"
        );

        writeText(strictDuplicateTargetFile, duplicateTargetGraph(false));
        expectLoadFailure(strictDuplicateTargetFile, "duplicate device target indexes");

        writeText(preservedDuplicateTargetFile, duplicateTargetGraph(true));
        const auto preservedDuplicateTarget =
            edge_gateway::GraphEmsConfig::loadFromFile(preservedDuplicateTargetFile);
        require(preservedDuplicateTarget.nodes.size() == 2, "preserved duplicate-target graph lost nodes");
        require(
            preservedDuplicateTarget.nodes[0].id == "write_first" &&
                preservedDuplicateTarget.nodes[1].id == "write_second",
            "preserved duplicate-target graph changed node order"
        );
        require(
            preservedDuplicateTarget.edges.size() == 1 &&
                preservedDuplicateTarget.edges[0].from == "write_first" &&
                preservedDuplicateTarget.edges[0].to == "write_second",
            "preserved duplicate-target graph changed its dependency edge"
        );
        require(
            hasLoadWarning(preservedDuplicateTarget, "duplicate device target index 200"),
            "preserved duplicate target did not emit a migration warning"
        );

        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
        {
            edge_gateway::MemoryPointStore store(memoryConfig);
            edge_gateway::PointStoreRouter router;
            router.addStore(sharedMemoryName, store);
            addRoute(router, 100, sharedMemoryName);
            addRoute(router, 101, sharedMemoryName);
            addRoute(router, 200, sharedMemoryName, true);
            const std::int64_t commandNowMs = 1721001601234LL;
            put(router, 100, 11.0, commandNowMs);
            put(router, 101, 22.0, commandNowMs);

            edge_gateway::GraphEmsEngine limitedEngine(
                preservedDuplicateTarget,
                router,
                600000,
                std::string(),
                {},
                "budget_rule"
            );
            const auto limitedResult = limitedEngine.runOnce(commandNowMs, 1);
            require(limitedResult.deviceWrites == 1, "write budget must allow exactly one device write");
            require(limitedResult.writeLimitReached, "write budget exhaustion was not reported");
            require(limitedResult.deviceWritesSkipped == 1, "write budget must skip the second eligible node");
            auto pending = router.peekPendingWrites();
            require(pending.size() == 1, "write budget was checked after submitting the second command");
            require(pending[0].value == 11.0, "write budget did not preserve deterministic node order");
            store.drainPendingWriteCommands();

            edge_gateway::GraphEmsEngine commandIdEngine(
                preservedDuplicateTarget,
                router,
                600000,
                std::string(),
                {},
                "command_rule"
            );
            const auto commandIdResult = commandIdEngine.runOnce(commandNowMs, 2);
            require(commandIdResult.deviceWrites == 2, "two eligible control nodes were not submitted");
            pending = router.peekPendingWrites();
            require(pending.size() == 2, "same-target commands were unexpectedly collapsed");
            require(pending[0].cmdId != pending[1].cmdId, "same-millisecond same-target cmdId collision");
            require(pending[0].cmdId.find("command_rule") != std::string::npos, "cmdId omitted ruleCode");
            require(pending[0].cmdId.find("write_first") != std::string::npos, "first cmdId omitted nodeId");
            require(pending[1].cmdId.find("write_second") != std::string::npos, "second cmdId omitted nodeId");
            require(pending[0].cmdId.size() < 64 && pending[1].cmdId.size() < 64, "cmdId exceeds shared-store limit");
            const auto firstCommandId = pending[0].cmdId;
            const auto secondCommandId = pending[1].cmdId;
            require(
                firstCommandId.rfind("_1") == firstCommandId.size() - 2 &&
                    secondCommandId.rfind("_2") == secondCommandId.size() - 2,
                "cmdId did not include the initial monotonic sequence"
            );
            store.drainPendingWriteCommands();
            const auto repeatedResult = commandIdEngine.runOnce(commandNowMs, 2);
            require(repeatedResult.deviceWrites == 2, "repeated same-millisecond commands were not submitted");
            pending = router.peekPendingWrites();
            require(pending.size() == 2, "repeated command queue size mismatch");
            require(
                pending[0].cmdId.rfind("_3") == pending[0].cmdId.size() - 2 &&
                    pending[1].cmdId.rfind("_4") == pending[1].cmdId.size() - 2,
                "cmdId sequence did not advance monotonically across scans"
            );
            require(
                pending[0].cmdId != firstCommandId && pending[1].cmdId != secondCommandId,
                "same-millisecond cmdId was reused on a later scan"
            );
        }
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);

        writeText(strictWeakTypeFile, weakTypeLinkGraph(false));
        expectLoadFailure(strictWeakTypeFile, "connects incompatible port types or units");

        writeText(preservedWeakTypeFile, weakTypeLinkGraph(true));
        const auto preservedWeakType = edge_gateway::GraphEmsConfig::loadFromFile(preservedWeakTypeFile);
        require(
            preservedWeakType.nodes.size() == 1 && preservedWeakType.nodes[0].id == "limit",
            "weakly typed migration link did not fold pointInput"
        );
        require(
            preservedWeakType.nodes[0].params.at("inputIndex") == "100",
            "weakly typed migration link did not retain its source index"
        );
        require(
            hasLoadWarning(preservedWeakType, "weakly typed data link weak_data"),
            "weakly typed migration link did not emit a migration warning"
        );

        writeText(unknownValueTypeFile, R"json({
  "schemaVersion":"2.0.0",
  "graphCode":"unknown-value-type",
  "compile":{"maxNodes":4,"maxEdges":4,"virtualIndexStart":700000,"virtualIndexEnd":700010},
  "nodes":[{
    "id":"source","type":"pointInput","order":0,"parameters":{"inputIndex":100},
    "ports":[{
      "id":"value","direction":"output","valueType":"opaque","runtimePath":"/inputIndex",
      "binding":{"kind":"point","index":100}
    }]
  }],
  "links":[]
})json");
        expectLoadFailure(unknownValueTypeFile, "unsupported valueType");

        std::remove(legacyFile.c_str());
        std::remove(mismatchFile.c_str());
        std::remove(unresolvedFile.c_str());
        std::remove(semanticKindFile.c_str());
        std::remove(rangeFile.c_str());
        std::remove(strictDuplicateOutputFile.c_str());
        std::remove(preservedDuplicateOutputFile.c_str());
        std::remove(strictDuplicateTargetFile.c_str());
        std::remove(preservedDuplicateTargetFile.c_str());
        std::remove(strictWeakTypeFile.c_str());
        std::remove(preservedWeakTypeFile.c_str());
        std::remove(unknownValueTypeFile.c_str());
        std::remove(unavailableScheduleFile.c_str());
        std::cout << "graph_ems_v2_loader_test passed\n";
        return 0;
    } catch (const std::exception& ex) {
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
        std::remove(legacyFile.c_str());
        std::remove(mismatchFile.c_str());
        std::remove(unresolvedFile.c_str());
        std::remove(semanticKindFile.c_str());
        std::remove(rangeFile.c_str());
        std::remove(strictDuplicateOutputFile.c_str());
        std::remove(preservedDuplicateOutputFile.c_str());
        std::remove(strictDuplicateTargetFile.c_str());
        std::remove(preservedDuplicateTargetFile.c_str());
        std::remove(strictWeakTypeFile.c_str());
        std::remove(preservedWeakTypeFile.c_str());
        std::remove(unknownValueTypeFile.c_str());
        std::remove(unavailableScheduleFile.c_str());
        std::cerr << "graph_ems_v2_loader_test failed: " << ex.what() << "\n";
        return 1;
    }
}
