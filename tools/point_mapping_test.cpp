// Point mapping (CQ15): file parsing and checks against the point tables, arithmetic, graph write targets,
// the ComputeEngine stage (inputs, outputs with deadband / refresh / readback), the graph restriction while a
// mapping is active, and ComputeEngine loading a mapping file.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

#include <unistd.h>

#include "edge_gateway/compute_engine_service.hpp"
#include "edge_gateway/graph_ems_engine.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/json_value.hpp"
#include "edge_gateway/cluster_write_authorization.hpp"
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
    add(685001, "EMS_CORE", "ems.out.pcs[1].p_set_a", "ems_virtual", false);
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
    t[685001].writable = true;
    problems = checkPointMapping(m, t, "GW_MAP");
    check(problems.size() == 1 && problems[0].rule == "mapping.write" && problems[0].message.find("only the strategy graph") != std::string::npos,
          "a writable EMS output point (persisted parameter) is refused");
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
        input.index = 101;  // graph permit
        input.sourceIndex = 101;
        input.pointCode = "graph_permit";
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

    // outputs: only a command published in the current scan counts; the device point is compared like controlWrite
    double sent = 0;
    r = stage.runOutputs(t0, 10);
    check(r.deviceWrites == 0 && f.pendingFor(1318, &sent) == 0, "no EMS command: nothing written");
    check(stage.statusJson(t0).find("\"state\":\"no-command\"") != std::string::npos, "no-command before the graph commands");
    f.put(685001, 50, t0);
    r = stage.runOutputs(t0, 0);
    check(r.deviceWrites == 0 && r.deviceWritesSkipped == 1, "write budget respected");
    r = stage.runOutputs(t0, 10);
    check(r.deviceWrites == 1 && f.pendingFor(1318, &sent) == 1 && sent == -50, "EMS 50 -> device -50 (invert)");
    check(stage.statusJson(t0).find("\"state\":\"verifying\"") != std::string::npos, "verifying after the write");
    r = stage.runOutputs(t0, 10);
    check(r.deviceWrites == 0, "the same value already queued is not queued twice");
    f.store->drainPendingWriteCommands();
    f.put(1240, -49.5, t0 + 500);
    r = stage.runOutputs(t0 + 500, 10);
    check(r.deviceWrites == 0 && stage.statusJson(t0 + 500).find("\"outputsOk\":1") != std::string::npos, "readback within tolerance");
    r = stage.runOutputs(t0 + 600, 10);
    check(r.deviceWrites == 0, "a command from an earlier scan is not acted on (graph quiet: device left alone)");
    f.put(685001, 50, t0 + 700);
    f.put(1318, -50, t0 + 700);
    r = stage.runOutputs(t0 + 700, 10);
    check(r.deviceWrites == 0, "device point already at the command: nothing written");
    f.put(685001, 50, t0 + 800);
    f.put(1318, -50.5, t0 + 800);
    r = stage.runOutputs(t0 + 800, 10);
    check(r.deviceWrites == 0, "device point within the deadband: nothing written");
    f.put(685001, 50, t0 + 900);
    f.put(1318, -45, t0 + 900);
    r = stage.runOutputs(t0 + 900, 10);
    check(r.deviceWrites == 1 && f.pendingFor(1318, &sent) == 1 && sent == -50, "device point changed beyond the deadband: written again (like controlWrite)");
    f.store->drainPendingWriteCommands();
    f.put(1318, -50, t0 + 950);
    f.put(685001, 60, t0 + 1200);
    r = stage.runOutputs(t0 + 1200, 10);
    check(r.deviceWrites == 1 && f.pendingFor(1318, &sent) == 1 && sent == -60, "new command beyond the deadband is sent");
    f.store->drainPendingWriteCommands();
    stage.runOutputs(t0 + 1200 + 3001, 10);
    check(stage.statusJson(t0 + 4300).find("readback-timeout") != std::string::npos, "readback still -49.5: timeout");
    f.put(1318, -60, t0 + 20000);
    r = stage.runOutputs(t0 + 20000, 10);
    check(r.deviceWrites == 0, "refreshMs: no rewrite while the graph is quiet");
    f.put(685001, 60, t0 + 20000);
    r = stage.runOutputs(t0 + 20000, 10);
    check(r.deviceWrites == 1, "refreshMs rewrites the same value while the graph commands");
    f.store->drainPendingWriteCommands();
    // the driver reports a failed write
    const auto pendingId = [&]() {
        f.put(685001, 100, t0 + 22000);
        stage.runOutputs(t0 + 22000, 10);
        const auto pending = f.router.peekPendingWrites();
        return pending.empty() ? std::string() : pending.back().cmdId;
    }();
    WritebackResultRecord wb;
    wb.cmdId = pendingId;
    wb.index = 1318;
    wb.success = false;
    wb.message = "modbus timeout";
    f.store->recordWritebackResult(wb);
    stage.runOutputs(t0 + 22100, 10);
    check(!pendingId.empty() && stage.statusJson(t0).find("writeback-failed") != std::string::npos, "driver write failure is reported");
    // a restarted stage does not replay the last command
    f.store->drainPendingWriteCommands();
    PointMappingStage restarted(parsePointMapping(kMapping), f.router, 5000, 1000);
    r = restarted.runOutputs(t0 + 23000, 10);
    check(r.deviceWrites == 0 && f.pendingFor(1318, &sent) == 0, "restart: the last command is not replayed");

    // write-only target (no current value): rewritten on every commanded scan like controlWrite, but a confirmed
    // readback keeps the state "ok"; a repeat that is no longer confirmed times out
    Fixture w;
    PointMappingStage wo(parsePointMapping(kMapping), w.router, 5000, 1000);
    const std::int64_t t2 = t0 + 200000;
    w.put(685001, 50, t2);
    r = wo.runOutputs(t2, 10);
    w.store->drainPendingWriteCommands();
    w.put(1240, -50, t2 + 500);
    std::size_t rewrites = r.deviceWrites;
    bool stayedOk = true;
    for (int k = 1; k <= 3; ++k) {
        const auto now = t2 + 1000 * k;
        w.put(685001, 50, now);
        w.put(1240, -50, now);
        rewrites += wo.runOutputs(now, 10).deviceWrites;
        w.store->drainPendingWriteCommands();
        stayedOk = stayedOk && wo.statusJson(now).find("\"outputsOk\":1") != std::string::npos;
    }
    check(rewrites == 4 && stayedOk, "write-only target: rewritten every commanded scan, state stays ok while the readback confirms");
    w.put(685001, 50, t2 + 4000);
    wo.runOutputs(t2 + 4000, 10);
    w.store->drainPendingWriteCommands();
    w.put(1240, -10, t2 + 4100);
    wo.runOutputs(t2 + 4000 + 3001, 10);
    check(wo.statusJson(t2 + 7001).find("readback-timeout") != std::string::npos, "a repeated write that the readback no longer confirms times out");

    // after a verified write the readback keeps being watched: device offline / changed locally is not "ok"
    Fixture g;
    PointMappingStage watch(parsePointMapping(kMapping), g.router, 5000, 1000);
    const std::int64_t t1 = t0 + 100000;
    g.put(685001, 50, t1);
    watch.runOutputs(t1, 10);
    g.store->drainPendingWriteCommands();
    g.put(1240, -50, t1 + 500);
    watch.runOutputs(t1 + 500, 10);
    check(watch.statusJson(t1 + 500).find("\"outputsOk\":1") != std::string::npos, "verified");
    g.put(1240, -50, t1 - 20000, 1);  // the readback expires: device offline
    watch.runOutputs(t1 + 1000, 10);
    check(watch.statusJson(t1 + 1000).find("readback-lost") != std::string::npos && watch.statusJson(t1 + 1000).find("\"outputsOk\":0") != std::string::npos,
          "readback gone after a verified write: readback-lost, not ok");
    g.put(1240, -50, t1 + 1500);
    watch.runOutputs(t1 + 1500, 10);
    check(watch.statusJson(t1 + 1500).find("\"outputsOk\":1") != std::string::npos, "readback back at the sent value: ok again");
    g.put(1240, -80, t1 + 2000);
    watch.runOutputs(t1 + 2000, 10);
    check(watch.statusJson(t1 + 2000).find("readback-mismatch") != std::string::npos, "readback moved away from the sent value: readback-mismatch");
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

const char* kGraphMapped = R"json({
  "schemaVersion":"2.0.0","graphCode":"mapping-output",
  "compile":{"maxNodes":8,"maxEdges":8,"virtualIndexStart":700000,"virtualIndexEnd":700010},
  "nodes":[
    {"id":"write_ems","type":"controlWrite","order":10,
     "parameters":{"inputIndex":100,"targetIndex":685001,"permitIndex":101,"permitValue":1,"minValue":-1000,"maxValue":1000,"deadband":5,"submitWrites":true},
     "ports":[
       {"id":"input","direction":"input","valueType":"number","runtimePath":"/inputIndex","binding":{"kind":"point","index":100}},
       {"id":"permit","direction":"input","valueType":"number","runtimePath":"/permitIndex","binding":{"kind":"point","index":101}},
       {"id":"target","direction":"target","valueType":"number","runtimePath":"/targetIndex","binding":{"kind":"point","index":685001}}]}
  ],
  "links":[]
})json";

// controlWrite to a mapped EMS output point: published on every permitted run (not compared with the EMS
// point, not persisted), nothing while the permit is off; the stage then writes the device.
void testGraphMappedOutput() {
    const std::string file = "/tmp/point-mapping-test-graph2-" + std::to_string(::getpid()) + ".json";
    {
        std::ofstream out(file.c_str());
        out << kGraphMapped;
    }
    const auto config = GraphEmsConfig::loadFromFile(file);
    std::remove(file.c_str());
    Fixture f;
    const std::int64_t now = 1792000000000LL;
    GraphEmsEngine engine(config, f.router, 3000);
    GraphEmsEngineOptions options;
    options.emsPointWritesOnly = true;
    options.mappedOutputIndexes.insert(685001);
    engine.setOptions(options);
    PointMappingStage stage(parsePointMapping(kMapping), f.router, 5000, 1000);
    f.put(100, 42, now);
    f.put(101, 1, now);
    auto result = engine.runOnce(now);
    auto cmd = f.get(685001, now);
    double sent = 0;
    check(result.errors.empty() && result.deviceWrites == 0 && cmd && cmd->value == 42 && cmd->ts == now && cmd->expireAt == now + 3000,
          "controlWrite publishes the command on the EMS output point (graph TTL)");
    check(f.pendingFor(685001, &sent) == 0 && f.pendingFor(1318, &sent) == 0, "no write command from the graph itself");
    stage.runOutputs(now, 10);
    check(f.pendingFor(1318, &sent) == 1 && sent == -42, "the mapping writes the device in the same scan");
    f.store->drainPendingWriteCommands();
    f.put(1318, -42, now + 500);
    f.put(100, 42, now + 1000);
    f.put(101, 1, now + 1000);
    result = engine.runOnce(now + 1000);
    cmd = f.get(685001, now + 1000);
    check(result.errors.empty() && cmd && cmd->ts == now + 1000, "the same command is published again on the next run");
    stage.runOutputs(now + 1000, 10);
    check(f.pendingFor(1318, &sent) == 0, "device already at the command: not written again");
    f.put(1318, -10, now + 1500);
    f.put(101, 0, now + 2000);
    f.put(100, 42, now + 2000);
    engine.runOnce(now + 2000);
    cmd = f.get(685001, now + 2000);
    stage.runOutputs(now + 2000, 10);
    check(cmd && cmd->ts == now + 1000 && f.pendingFor(1318, &sent) == 0, "permit off: no command, device left alone although it differs");
    f.put(101, 1, now + 3000);
    f.put(100, 42, now + 3000);
    engine.runOnce(now + 3000);
    stage.runOutputs(now + 3000, 10);
    check(f.pendingFor(1318, &sent) == 1 && sent == -42, "permit back, device differs: written like a direct controlWrite");
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

// Source tree with the factory files; on a target board (tests copied to /tmp) GZ_EDGE_TEST_SOURCE_DIR points at the copied tree.
std::string sourceDir() {
    const char* root = std::getenv("GZ_EDGE_TEST_SOURCE_DIR");
    return root != nullptr && *root != '\0' ? std::string(root) : std::string(POINT_MAPPING_SOURCE_DIR);
}

std::string readText(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

// Factory files for the migrated graph (gz-ems migrate): the graph only references EMS_CORE points (original
// file + the mapping point fragment) and graph-virtual points, writes only read-only EMS output points, and loads.
void testFactoryMappedGraph() {
    const std::string dir = sourceDir() + "/config/factory/runtime/";
    const std::string migration = sourceDir() + "/config/factory/migration/";
    const std::string graphPath = migration + "shuntong_ems_graph_mapped.json";
    const auto graphText = readText(graphPath);
    const auto fragmentText = readText(migration + "shuntong_ems_graph_mapped_points.json");
    check(!graphText.empty() && !fragmentText.empty(), "factory mapped graph and point fragment exist");
    std::set<std::uint32_t> ems;
    const auto emsRoot = json::JsonParser(readText(dir + "devices/device_ems_virtual.json"), 64, 20000000).parse();
    const auto* meters = emsRoot.find("meters");
    check(meters && meters->isArray(), "EMS_CORE point table readable");
    if (!meters || !meters->isArray()) return;
    for (const auto& meter : meters->asArray().values) {
        const auto* points = meter->find("points");
        if (!points || !points->isArray()) continue;
        for (const auto& p : points->asArray().values) ems.insert(static_cast<std::uint32_t>(p->find("index")->asNumber()));
    }
    std::set<std::uint32_t> inputs, outputs;
    bool fragmentOk = true;
    const auto fragment = json::JsonParser(fragmentText, 16, 2000000).parse();
    for (const auto& p : fragment.asArray().values) {
        const auto index = static_cast<std::uint32_t>(p->find("index")->asNumber());
        const auto code = p->find("pointCode")->asString();
        const bool out = code.rfind("ems.out.", 0) == 0;
        const auto* write = p->find("write");
        const auto* enable = write && write->isObject() ? write->find("enable") : nullptr;
        fragmentOk = fragmentOk && index >= kPointMappingVirtualIndexMin && index <= kPointMappingVirtualIndexMax && ems.count(index) == 0 &&
                     enable && enable->isBool() && !enable->asBool() && p->find("initialValue") == nullptr &&
                     (out || code.rfind("ems.in.", 0) == 0);
        (out ? outputs : inputs).insert(index);
    }
    check(fragmentOk && inputs.size() == 52 && outputs.size() == 12, "fragment: 52 input + 12 read-only output EMS points in 680000-689999, no initial values");
    const auto graph = json::JsonParser(graphText, 64, 20000000).parse();
    std::size_t refs = 0;
    std::string stray;
    for (const auto& n : graph.find("nodes")->asArray().values) {
        const auto* ports = n->find("ports");
        if (!ports || !ports->isArray()) continue;
        for (const auto& port : ports->asArray().values) {
            const auto* b = port->isObject() ? port->find("binding") : nullptr;
            const auto* idx = b && b->isObject() ? b->find("index") : nullptr;
            if (!idx || !idx->isNumber()) continue;
            const auto i = static_cast<std::uint32_t>(idx->asNumber());
            ++refs;
            if (!(ems.count(i) || inputs.count(i) || outputs.count(i) || (i >= 700000 && i <= 799999))) stray += " " + std::to_string(i);
        }
    }
    check(refs > 0 && stray.empty(), "mapped graph references only EMS_CORE and graph-virtual points" + stray);
    bool targetsOk = true;
    const auto writes = graphWriteTargets(graphText);
    for (const auto& w : writes) targetsOk = targetsOk && outputs.count(w.index) == 1;
    check(writes.size() == 14 && targetsOk, "the 14 submitting controlWrites target EMS output points");
    std::size_t deviceWrites = 0;
    for (const auto& w : graphWriteTargets(readText(dir + "logic/shuntong_ems_graph.json"))) deviceWrites += w.index < kPointMappingVirtualIndexMin ? 1 : 0;
    check(deviceWrites == 14, "the original factory graph is unchanged (still writes 14 device points directly)");
    bool loads = false;
    try {
        loads = GraphEmsConfig::loadFromFile(graphPath).nodes.size() > 400;
    } catch (const std::exception& ex) {
        std::cerr << "  " << ex.what() << std::endl;
    }
    check(loads, "the mapped graph loads");
}

}  // namespace


std::string stateOf(const std::string& json) {  // state of the output entry ems.out.pcs[1].p_set_a
    const auto item = json.find("\"virtual\":\"ems.out.pcs[1].p_set_a\"");
    if (item == std::string::npos) return "?";
    const auto p = json.find("\"state\":\"", item);
    if (p == std::string::npos) return "?";
    const auto e = json.find('"', p + 9);
    return json.substr(p + 9, e - p - 9);
}

// Review 2026-10-25: the verification cycle (readback newer than the write, fixed deadline, driver result after
// a confirmed readback), cluster control targets and time limits.
void testVerificationCycle() {
    const std::int64_t t0 = 1792100000000LL;
    // refreshMs shorter than the readback timeout, graph commanding every second, readback contradicting:
    // resending the same value must not postpone the timeout forever.
    const auto refresh = replace(kMapping, "\"refreshMs\": 10000", "\"refreshMs\": 1000");
    {
        Fixture f;
        PointMappingStage stage(parsePointMapping(refresh), f.router, 5000, 1000);
        f.put(1240, -50, t0 - 100);
        for (int k = 0; k <= 10; ++k) {
            const auto now = t0 + 1000 * k;
            f.put(685001, 50, now);
            f.put(1240, -10, now);  // the device keeps reporting -10 while -50 is sent
            stage.runOutputs(now, 10);
            f.store->drainPendingWriteCommands();
        }
        check(stateOf(stage.statusJson(t0 + 10000)) == "readback-timeout", "same value resent every second: the readback timeout still fires");
    }
    // A readback sampled before the write is old news: it does not confirm, even within tolerance.
    {
        Fixture f;
        PointMappingStage stage(parsePointMapping(kMapping), f.router, 5000, 1000);
        f.put(1240, -50, t0 - 10);  // already -50 before the write, never polled again
        f.put(685001, 50, t0);
        stage.runOutputs(t0, 10);
        f.store->drainPendingWriteCommands();
        stage.runOutputs(t0 + 1000, 10);
        check(stateOf(stage.statusJson(t0 + 1000)) == "verifying", "a readback older than the write does not confirm it");
        stage.runOutputs(t0 + 3001, 10);
        check(stateOf(stage.statusJson(t0 + 3001)) == "readback-timeout", "no fresh readback within timeoutMs: readback-timeout");
        f.put(1240, -50, t0 + 3500);
        stage.runOutputs(t0 + 3500, 10);
        check(stateOf(stage.statusJson(t0 + 3500)) == "ok", "a fresh readback at the sent value clears the timeout");
    }
    // Readback confirms first, then the driver reports the write failed: the failure is not lost.
    {
        Fixture f;
        PointMappingStage stage(parsePointMapping(kMapping), f.router, 5000, 1000);
        f.put(685001, 50, t0);
        stage.runOutputs(t0, 10);
        const auto pending = f.router.peekPendingWrites();
        const auto id = pending.empty() ? std::string() : pending.back().cmdId;
        f.store->drainPendingWriteCommands();
        f.put(1240, -50, t0 + 200);
        stage.runOutputs(t0 + 200, 10);
        check(stateOf(stage.statusJson(t0 + 200)) == "ok", "fresh readback within tolerance: ok");
        WritebackResultRecord wb;
        wb.cmdId = id;
        wb.index = 1318;
        wb.success = false;
        wb.message = "modbus exception 4";
        f.store->recordWritebackResult(wb);
        stage.runOutputs(t0 + 400, 10);
        check(!id.empty() && stateOf(stage.statusJson(t0 + 400)) == "writeback-failed", "driver failure after the readback confirmed is still reported");
    }
}

void testClusterAndLimits() {
    const auto mapping = parsePointMapping(kMapping);
    EmsClusterConfig cluster;
    check(checkPointMappingCluster(mapping, cluster).empty(), "cluster off: no cluster problem");
    cluster.enabled = true;
    cluster.controlEnabled = true;
    cluster.controlTargetIndexes = {1318};
    const auto problems = checkPointMappingCluster(mapping, cluster);
    check(problems.size() == 1 && problems[0].rule == "mapping.cluster", "output to a cluster control target is refused");
    cluster.controlTargetIndexes = {9999};
    check(checkPointMappingCluster(mapping, cluster).empty(), "outputs outside the cluster control targets are fine");
    PendingWriteCommand cmd;
    cmd.index = 1318;
    cmd.value = 20;
    cmd.source = "point-mapping";
    cluster.controlTargetIndexes = {1318};
    check(clusterAuthorizationRequired(cluster, cmd), "drivers require cluster authority for point-mapping writes to control targets");
    auto rejects = [](const std::string& text) {
        try {
            parsePointMapping(text);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    check(rejects(replace(kMapping, "\"timeoutMs\": 3000", "\"timeoutMs\": 1e30")), "readback timeoutMs beyond a day is refused");
    check(rejects(replace(kMapping, "\"refreshMs\": 10000", "\"refreshMs\": 86400001")), "refreshMs beyond a day is refused");
    check(rejects(replace(kMapping, "\"timeoutMs\": 5000", "\"timeoutMs\": 1e300")), "validity timeoutMs beyond a day is refused");
}

int main() {
    try {
        testParseAndCheck();
        testArithmetic();
        testGraphTargets();
        testStage();
        testGraphRestriction();
        testGraphMappedOutput();
        testComputeEngineLoads();
        testFactoryMappedGraph();
        testVerificationCycle();
        testClusterAndLimits();
    } catch (const std::exception& ex) {
        std::cerr << "exception: " << ex.what() << std::endl;
        return 1;
    }
    std::cout << "point_mapping_test: " << g_checks << " checks, " << g_failures << " failures" << std::endl;
    return g_failures == 0 ? 0 : 1;
}
