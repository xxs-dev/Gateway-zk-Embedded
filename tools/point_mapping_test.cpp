// Point mapping (CQ15): file parsing and checks against the point tables, arithmetic, graph write targets,
// the ComputeEngine stage (inputs, outputs with deadband / refresh / readback), the graph restriction while a
// mapping is active, and ComputeEngine loading a mapping file.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <unistd.h>

#include "edge_gateway/compute_engine_service.hpp"
#include "edge_gateway/graph_ems_engine.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/point_mapping.hpp"
#include "edge_gateway/point_mapping_stage.hpp"
#include "edge_gateway/point_store_router.hpp"

using namespace edge_gateway;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::cerr << "FAIL: " << what << std::endl;
    }
}

bool throwsWith(const std::string& text, const std::string& needle) {
    try {
        parsePointMapping(text);
    } catch (const std::exception& ex) {
        if (std::string(ex.what()).find(needle) != std::string::npos) return true;
        std::cerr << "  (message was: " << ex.what() << ")" << std::endl;
        return false;
    }
    return false;
}

const char* kMapping = R"json({
  "schemaVersion": "1.0", "kind": "point-mapping", "machineCode": "GW_MAP", "generatedBy": "test",
  "entries": [
    { "virtual": "ems.in.pcs[1].p_total", "virtualIndex": 680001, "direction": "in",
      "sources": [ { "meterCode": "PCS1", "pointCode": "p_a", "index": 1201 }, { "meterCode": "PCS1", "pointCode": "p_b", "index": 1202 } ],
      "aggregate": "sum", "scale": 0.001, "offset": 0, "invert": true, "validity": { "timeoutMs": 5000 } },
    { "virtual": "ems.in.pcs[1].state", "virtualIndex": 680002, "direction": "in",
      "sources": [ { "meterCode": "PCS1", "pointCode": "run_state", "index": 1209 } ],
      "enum": { "0": 1, "1": 2, "2": 3, "3": 4 } },
    { "virtual": "ems.out.pcs[1].p_set_a", "virtualIndex": 685001, "direction": "out",
      "sources": [ { "meterCode": "PCS1", "pointCode": "p_set_a", "index": 1318 } ],
      "invert": true,
      "write": { "min": -178, "max": 178, "deadband": 1, "refreshMs": 10000,
                 "readback": { "meterCode": "PCS1", "pointCode": "p_a_meas", "index": 1240, "tolerance": 1, "timeoutMs": 3000 } } }
  ] })json";

PointMappingTargets targets() {
    PointMappingTargets t;
    auto add = [&t](std::uint32_t index, const std::string& meter, const std::string& code, const std::string& protocol, bool writable) {
        t[index] = PointMappingTarget{meter, code, protocol, writable};
    };
    add(680001, "EMS_CORE", "ems.in.pcs[1].p_total", "ems_virtual", false);
    add(680002, "EMS_CORE", "ems.in.pcs[1].state", "ems_virtual", false);
    add(685001, "EMS_CORE", "ems.out.pcs[1].p_set_a", "ems_virtual", true);
    add(1201, "PCS1", "p_a", "modbus_rtu", false);
    add(1202, "PCS1", "p_b", "modbus_rtu", false);
    add(1209, "PCS1", "run_state", "modbus_rtu", false);
    add(1318, "PCS1", "p_set_a", "modbus_rtu", true);
    add(1240, "PCS1", "p_a_meas", "modbus_rtu", false);
    return t;
}

std::string replace(std::string s, const std::string& from, const std::string& to) {
    const auto pos = s.find(from);
    if (pos != std::string::npos) s.replace(pos, from.size(), to);
    return s;
}

void testParseAndCheck() {
    const auto m = parsePointMapping(kMapping);
    check(m.entries.size() == 3 && m.machineCode == "GW_MAP", "parses three entries");
    check(m.entries[0].sources.size() == 2 && m.entries[0].aggregate == "sum" && m.entries[0].invert && m.entries[0].validityTimeoutMs == 5000, "input fields");
    check(m.entries[1].enumMap.size() == 4 && m.entries[1].enumMap.at(2) == 3, "enum map");
    check(m.entries[2].output && m.entries[2].readback.index == 1240 && m.entries[2].readbackTimeoutMs == 3000 && m.entries[2].refreshMs == 10000, "output fields");
    check(checkPointMapping(m, targets(), "GW_MAP").empty(), "consistent with the point tables");

    check(throwsWith(replace(kMapping, R"("kind": "point-mapping")", R"("kind": "x")"), "kind"), "kind is required");
    check(throwsWith(replace(kMapping, R"("aggregate": "sum", )", ""), "aggregate"), "several sources need an aggregate");
    check(throwsWith(replace(kMapping, R"("0": 1, "1": 2)", R"("0": "stopped", "1": 2)"), "CQ17"), "named enum codes are refused (CQ17)");
    check(throwsWith(replace(kMapping, R"(, "tolerance": 1, "timeoutMs": 3000 } } })", " } } }"), "pointCode") ||
              throwsWith(replace(kMapping, R"(,
                 "readback": { "meterCode": "PCS1", "pointCode": "p_a_meas", "index": 1240, "tolerance": 1, "timeoutMs": 3000 })", ""), "readback"),
          "an output needs a readback");
    check(throwsWith(replace(kMapping, R"("scale": 0.001)", R"("scale": 0)"), "scale"), "scale 0 refused");
    check(throwsWith(replace(kMapping, R"("min": -178, "max": 178)", R"("min": 5, "max": 1)"), "write.min"), "min <= max");

    auto t = targets();
    t[1318].writable = false;
    auto problems = checkPointMapping(m, t, "GW_MAP");
    check(problems.size() == 1 && problems[0].rule == "mapping.write", "a non-writable output target is refused");
    t = targets();
    t[680001].pointCode = "something.else";
    t[1202].meterCode = "PCS2";
    t.erase(1240);
    problems = checkPointMapping(m, t, "GW_OTHER");
    std::string all;
    for (const auto& p : problems) all += p.rule + ":" + p.message + "\n";
    check(all.find("mapping.format:machineCode") != std::string::npos, "machine code checked: " + all);
    check(all.find("pointCode something.else") != std::string::npos, "virtual pointCode must be the virtual name");
    check(all.find("PCS2/p_b, not PCS1/p_b") != std::string::npos, "source meterCode checked");
    check(all.find("readback index 1240 is not in the device point tables") != std::string::npos, "readback point must exist");
    const auto outside = parsePointMapping(replace(kMapping, "680002", "700002"));
    t = targets();
    t[700002] = t[680002];
    check(checkPointMapping(outside, t, "GW_MAP").size() == 1, "virtual index outside 680000-689999 is refused");
    const auto twice = parsePointMapping(replace(replace(kMapping, R"("index": 1209)", R"("index": 1201)"), R"("pointCode": "run_state")", R"("pointCode": "p_a")"));
    check(checkPointMapping(twice, targets(), "GW_MAP").empty(), "two inputs may read the same device point");
}

void testArithmetic() {
    const auto m = parsePointMapping(kMapping);
    double v = 0;
    check(pointMappingToEms(m.entries[0], pointMappingAggregate("sum", {1000, 2000}), &v) && std::fabs(v + 3.0) < 1e-9, "sum, scale, invert");
    check(pointMappingToEms(m.entries[1], 2, &v) && v == 3, "enum device -> EMS");
    check(!pointMappingToEms(m.entries[1], 9, &v), "enum without an entry is invalid");
    check(pointMappingToDevice(m.entries[2], 50, &v) && v == -50, "output inverse (invert)");
    check(pointMappingToDevice(m.entries[2], -500, &v) && v == 178, "output clamped to write.max");
    check(pointMappingAggregate("min", {3, 1, 2}) == 1 && pointMappingAggregate("max", {3, 1, 2}) == 3 && pointMappingAggregate("avg", {1, 3}) == 2, "aggregates");
}

void testGraphTargets() {
    const std::string graph = R"json({"nodes":[
      {"id":"a","type":"controlWrite","parameters":{"submitWrites":true,"targetIndex":1318}},
      {"id":"b","type":"controlWrite","parameters":{"submitWrites":false,"targetIndex":1319}},
      {"id":"c","type":"controlWrite","enabled":false,"parameters":{"submitWrites":true,"targetIndex":1320}},
      {"id":"d","type":"pcsWriteback","parameters":{"pControlAIndex":2001}},
      {"id":"e","type":"formula","parameters":{"outputIndex":700001}}]})json";
    const auto w = graphWriteTargets(graph);
    check(w.size() == 7 && w[0].nodeId == "a" && w[0].index == 1318, "controlWrite with submitWrites is a device write");
    check(w[1].nodeId == "d" && w[1].index == 2001 && w[2].index == 1319, "pcsWriteback writes its six control points (defaults where not set)");
}

struct Fixture {
    std::string shm = "point_mapping_test_" + std::to_string(::getpid());
    MemoryStoreConfig cfg;
    std::unique_ptr<MemoryPointStore> store;
    PointStoreRouter router;
    Fixture() {
        MemoryPointStore::cleanupOrphanedSegment(shm);
        cfg.sharedMemoryName = shm;
        cfg.maxLatestPoints = 64;
        cfg.maxPendingWrites = 32;
        cfg.maxPersistentSamples = 32;
        store.reset(new MemoryPointStore(cfg));
        router.addStore(shm, *store);
        for (const auto& t : targets()) {
            PointStoreRoute r;
            r.index = t.first;
            r.sourceIndex = t.first;
            r.machineCode = "GW_MAP";
            r.meterCode = t.second.meterCode;
            r.pointCode = t.second.pointCode;
            r.protocolType = t.second.protocol;
            r.writable = t.second.writable;
            r.write.enable = t.second.writable;
            r.sharedMemoryName = shm;
            router.addRoute(r);
        }
        PointStoreRoute input;  // graph input for the restriction test
        input.index = 100;
        input.sourceIndex = 100;
        input.meterCode = "EMS_CORE";
        input.pointCode = "graph_input";
        input.protocolType = "ems_virtual";
        input.sharedMemoryName = shm;
        router.addRoute(input);
    }
    ~Fixture() {
        store.reset();
        MemoryPointStore::cleanupOrphanedSegment(shm);
    }
    void put(std::uint32_t index, double value, std::int64_t now, std::int64_t ttl = 600000) {
        PointValue p;
        p.index = index;
        p.value = value;
        p.quality = 1;
        p.ts = now;
        p.expireAt = now + ttl;
        router.putLatestByIndex(p);
    }
    Optional<StoredPointValue> get(std::uint32_t index, std::int64_t now) { return router.getLatestByIndex(index, now); }
    std::size_t pendingFor(std::uint32_t index, double* value) {
        std::size_t n = 0;
        for (const auto& p : router.peekPendingWrites()) {
            if (p.index == index) {
                ++n;
                *value = p.value;
            }
        }
        return n;
    }
};

void testStage() {
    Fixture f;
    const std::int64_t t0 = 1792000000000LL;
    PointMappingStage stage(parsePointMapping(kMapping), f.router, 5000, 1000);
    // inputs
    f.put(1201, 1000, t0);
    f.put(1202, 2000, t0);
    f.put(1209, 2, t0);
    auto r = stage.runInputs(t0);
    auto v = f.get(680001, t0);
    check(r.errors.empty() && r.latestWrites == 2, "inputs published");
    check(v && v->quality == 1 && std::fabs(v->value + 3.0) < 1e-9, "p_total = -(1000 + 2000) * 0.001");
    v = f.get(680002, t0);
    check(v && v->quality == 1 && v->value == 3, "state enum");
    f.put(1202, 2000, t0 - 10000, 1);  // expired source
    stage.runInputs(t0 + 100);
    v = f.get(680001, t0 + 100);
    check(v && v->quality == 0, "an invalid source makes the virtual point invalid");
    f.put(1209, 9, t0 + 200);
    stage.runInputs(t0 + 200);
    v = f.get(680002, t0 + 200);
    check(v && v->quality == 0, "a device value without an enum entry is invalid");
    check(stage.statusJson(t0).find("\"inputsInvalid\":2") != std::string::npos, "status counts invalid inputs");

    // outputs
    double sent = 0;
    r = stage.runOutputs(t0, 10);
    check(r.deviceWrites == 0 && f.pendingFor(1318, &sent) == 0, "no EMS command: nothing written");
    f.put(685001, 50, t0);
    r = stage.runOutputs(t0, 0);
    check(r.deviceWrites == 0 && r.deviceWritesSkipped == 1, "write budget respected");
    r = stage.runOutputs(t0, 10);
    check(r.deviceWrites == 1 && f.pendingFor(1318, &sent) == 1 && sent == -50, "EMS 50 -> device -50 (invert)");
    check(stage.statusJson(t0).find("\"state\":\"verifying\"") != std::string::npos, "verifying after the write");
    f.store->drainPendingWriteCommands();
    f.put(1240, -49.5, t0 + 500);
    stage.runOutputs(t0 + 500, 10);
    check(stage.statusJson(t0 + 500).find("\"outputsOk\":1") != std::string::npos, "readback within tolerance");
    f.put(685001, 50.5, t0 + 600);
    r = stage.runOutputs(t0 + 600, 10);
    check(r.deviceWrites == 0, "change within the deadband is not sent");
    f.put(685001, 60, t0 + 700);
    r = stage.runOutputs(t0 + 700, 10);
    check(r.deviceWrites == 1 && f.pendingFor(1318, &sent) == 1 && sent == -60, "change beyond the deadband is sent");
    f.store->drainPendingWriteCommands();
    stage.runOutputs(t0 + 700 + 3001, 10);
    check(stage.statusJson(t0 + 3800).find("readback-timeout") != std::string::npos, "readback still -49.5: timeout");
    f.put(685001, 60, t0 + 11000);
    r = stage.runOutputs(t0 + 11000, 10);
    check(r.deviceWrites == 1, "refreshMs resends the same value");
    f.store->drainPendingWriteCommands();
    // the driver reports a failed write
    const auto pendingId = [&]() {
        f.put(685001, 100, t0 + 12000);
        stage.runOutputs(t0 + 12000, 10);
        const auto pending = f.router.peekPendingWrites();
        return pending.empty() ? std::string() : pending.back().cmdId;
    }();
    WritebackResultRecord wb;
    wb.cmdId = pendingId;
    wb.index = 1318;
    wb.success = false;
    wb.message = "modbus timeout";
    f.store->recordWritebackResult(wb);
    stage.runOutputs(t0 + 12100, 10);
    check(!pendingId.empty() && stage.statusJson(t0).find("writeback-failed") != std::string::npos, "driver write failure is reported");
}

const char* kGraph = R"json({
  "schemaVersion":"2.0.0","graphCode":"mapping-restriction",
  "compile":{"maxNodes":8,"maxEdges":8,"virtualIndexStart":700000,"virtualIndexEnd":700010},
  "nodes":[
    {"id":"write_device","type":"controlWrite","order":10,
     "parameters":{"inputIndex":100,"targetIndex":1318,"minValue":-1000,"maxValue":1000,"submitWrites":true},
     "ports":[
       {"id":"input","direction":"input","valueType":"number","runtimePath":"/inputIndex","binding":{"kind":"point","index":100}},
       {"id":"target","direction":"target","valueType":"number","runtimePath":"/targetIndex","binding":{"kind":"point","index":1318}}]}
  ],
  "links":[]
})json";

void testGraphRestriction() {
    const std::string file = "/tmp/point-mapping-test-graph-" + std::to_string(::getpid()) + ".json";
    {
        std::ofstream out(file.c_str());
        out << kGraph;
    }
    const auto config = GraphEmsConfig::loadFromFile(file);
    std::remove(file.c_str());
    for (const bool restricted : {false, true}) {
        Fixture f;
        const std::int64_t now = 1792000000000LL;
        f.put(100, 42, now);
        GraphEmsEngine engine(config, f.router);
        GraphEmsEngineOptions options;
        options.emsPointWritesOnly = restricted;
        engine.setOptions(options);
        const auto result = engine.runOnce(now);
        double sent = 0;
        if (!restricted) {
            check(result.errors.empty() && f.pendingFor(1318, &sent) == 1 && sent == 42, "without a mapping the graph writes the device point");
        } else {
            check(!result.errors.empty() && result.errors[0].find("point mapping is active") != std::string::npos && f.pendingFor(1318, &sent) == 0,
                  "with a mapping the graph may not write the device point");
        }
    }
}

void testComputeEngineLoads() {
    const std::string file = "/tmp/point-mapping-test-" + std::to_string(::getpid()) + ".json";
    const std::string status = file + ".status";
    {
        std::ofstream out(file.c_str());
        out << kMapping;
    }
    Fixture f;
    ComputeEngineConfig config;
    config.enabled = true;
    config.pointMappingFile = file;
    config.pointMappingStatusFile = status;
    {
        ComputeEngineService service(config, f.router);
        check(service.pointMappingEntries() == 3, "ComputeEngine loads the mapping");
        const std::int64_t now = 1792000000000LL;
        f.put(1201, 1000, now);
        f.put(1202, 1000, now);
        f.put(685001, 10, now);
        service.runOnce(now);
        const auto v = f.get(680001, now);
        double sent = 0;
        check(v && v->quality == 1 && std::fabs(v->value + 2.0) < 1e-9 && f.pendingFor(1318, &sent) == 1 && sent == -10, "one scan runs inputs and outputs");
        std::ifstream in(status.c_str());
        std::stringstream s;
        s << in.rdbuf();
        check(s.str().find("\"entries\":3") != std::string::npos, "status file written");
    }
    {
        std::ofstream out(file.c_str());
        out << replace(kMapping, R"("index": 1240)", R"("index": 4242)");
    }
    bool refused = false;
    try {
        ComputeEngineService service(config, f.router);
    } catch (const std::exception& ex) {
        refused = std::string(ex.what()).find("does not match the point tables") != std::string::npos;
    }
    check(refused, "a mapping that does not match the point tables stops ComputeEngine from starting");
    std::remove(file.c_str());
    std::remove(status.c_str());
    config.pointMappingFile = file;  // missing file: no mapping stage
    ComputeEngineService none(config, f.router);
    check(none.pointMappingEntries() == 0, "no mapping file: no mapping stage");
}

}  // namespace

int main() {
    try {
        testParseAndCheck();
        testArithmetic();
        testGraphTargets();
        testStage();
        testGraphRestriction();
        testComputeEngineLoads();
    } catch (const std::exception& ex) {
        std::cerr << "exception: " << ex.what() << std::endl;
        return 1;
    }
    std::cout << "point_mapping_test: " << g_checks << " checks, " << g_failures << " failures" << std::endl;
    return g_failures == 0 ? 0 : 1;
}
