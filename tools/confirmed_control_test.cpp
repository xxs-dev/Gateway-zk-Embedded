#include "edge_gateway/graph_ems_engine.hpp"
#include "edge_gateway/compute_command_observation.hpp"
#include "edge_gateway/compute_engine_service.hpp"
#include "edge_gateway/compute_graph_observation.hpp"
#include <chrono>
#include "edge_gateway/filesystem_compat.hpp"
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace edge_gateway;
namespace fs = edge_gateway::filesystem;
namespace {
void require(bool ok, const std::string& m) { if (!ok) throw std::runtime_error(m); }
struct Fixture {
    std::string name = "confirmed_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::path root = fs::temp_directory_path() / name;
    std::unique_ptr<MemoryPointStore> store;
    PointStoreRouter router;
    GraphEmsConfig config;
    std::unique_ptr<GraphEmsEngine> engine;
    std::int64_t now = 1792368000000LL;
    double request = 20, readback = 0, actual = 0, permit = 0, reset = 0, stopped = 1;
    Fixture() {
        fs::create_directory(root);
        MemoryStoreConfig memory; memory.sharedMemoryName = name;
        memory.maxLatestPoints = 16; memory.maxPendingWrites = 8; memory.maxPersistentSamples = 16;
        store.reset(new MemoryPointStore(memory)); router.addStore(name, *store);
        for (unsigned index = 1; index <= 7; ++index) {
            PointStoreRoute r; r.index = index; r.sharedMemoryName = name;
            r.machineCode = "ISOLATED"; r.meterCode = "PCS"; r.pointCode = std::to_string(index);
            r.protocolType = "modbus_tcp"; r.writable = index == 2;
            r.write.enable = true; r.write.minValue = -100; r.write.maxValue = 100;
            r.write.verifyAfterWrite = true; r.write.verifyByRead = true;
            router.addRoute(r);
        }
        config.graphCode = "confirmed-test";
        GraphEmsNodeConfig n; n.id = "output"; n.type = "confirmedControlWrite";
        n.params = {{"inputIndex","1"},{"targetIndex","2"},{"permitIndex","3"},{"resetIndex","4"},
            {"stoppedIndex","5"},{"feedbackIndex","6"},{"outputIndex","7"},{"minValue","-100"},
            {"maxValue","100"},{"receiptTimeoutMs","500"},{"maxInputAgeMs","2000"},
            {"maxGapMs","1000"},{"stoppedTolerance","0.5"},{"sameDirection","true"},{"submitWrites","true"}};
        config.nodes.push_back(n); restart();
    }
    void restart() { engine.reset(); engine.reset(new GraphEmsEngine(config, router, 600, (root / "state.json").string())); }
    void seed(std::int64_t timestamp, std::int64_t ttl = 2000) {
        const double values[] = {request, readback, permit, reset, stopped, actual};
        for (unsigned i = 0; i < 6; ++i) {
            PointValue p; p.index = i + 1; p.value = values[i]; p.ts = timestamp; p.expireAt = timestamp + ttl; p.quality = 1;
            require(router.putLatestByIndex(p).accepted, "seed input");
        }
    }
    GraphEmsRunResult step(bool errorsAllowed = false, std::size_t budget = 5) {
        now += 100;
        seed(now);
        auto r = engine->runOnce(now, budget);
        if (!errorsAllowed && !r.errors.empty()) throw std::runtime_error(r.errors.front());
        return r;
    }
    GraphEmsRunResult capturedStep(std::int64_t sampleLead, std::int64_t captureDelay, std::int64_t ttl = 2000) {
        now += 100;
        seed(now + sampleLead, ttl);
        int clockCalls = 0;
        auto r = engine->runOnce(now, 5, [&] { ++clockCalls; return now + captureDelay; });
        now += captureDelay;
        require(clockCalls == 1, "capture completion clock sampled once");
        require(r.errors.empty(), "captured scan errors");
        return r;
    }
    int status() { return static_cast<int>(router.getLatestByIndex(7, now)->value); }
    void checkObservation(const PendingWriteCommand& command, int expected, bool pending, bool confirmed) {
        const auto before = store->peekPendingWriteCommands().size();
        const auto parsed = json::JsonParser(engine->commandObservation()).parse();
        std::size_t count = 0;
        const auto validated = json::JsonParser(compute_observation_detail::commands(parsed, now, count)).parse();
        require(count == 1, "exactly one observed command owner");
        const auto& n = *validated.find("nodes")->asArray().values[0];
        require(n.find("nodeId")->asString() == "output" && n.find("targetIndex")->asNumber() == 2 &&
            n.find("outputIndex")->asNumber() == 7, "observed node mapping");
        require(n.find("cmdId")->asString() == command.cmdId && n.find("index")->asNumber() == command.index &&
            n.find("value")->asNumber() == command.value && n.find("requestedAt")->asNumber() == command.ts, "exact owner transaction identity");
        require(n.find("status")->asNumber() == expected && n.find("pending")->asBool() == pending &&
            n.find("confirmed")->asBool() == confirmed, "owner phase distinct from receipt outcome");
        require(store->peekPendingWriteCommands().size() == before, "observation must not consume or submit commands");
    }
    void arm() { permit = reset = 0; readback = actual = 0; stopped = 1; step(); reset = 1; step(); require(status() == 1, "reset arms only"); reset = 0; step(); }
    PendingWriteCommand send() {
        arm(); permit = 1; require(step().deviceWrites == 1 && status() == 2, "one original transaction");
        auto q = store->drainPendingWriteCommands(); require(q.size() == 1, "one queue entry"); return q.front();
    }
    WritebackResultRecord receipt(const PendingWriteCommand& c, bool success = true) {
        WritebackResultRecord r; r.cmdId = c.cmdId; r.index = c.index; r.value = c.value;
        r.requestedAt = c.ts; r.completedAt = now; r.success = success;
        r.verifyAttempted = r.verifyPassed = success; return r;
    }
    ~Fixture() { engine.reset(); store.reset(); MemoryPointStore::cleanupOrphanedSegment(name); fs::remove_all(root); }
};
void testRecoverySnapshotClock() {
    for (int kind = 0; kind < 5; ++kind) {
        Fixture f;
        for (unsigned index = 8; index <= 12; ++index) {
            PointStoreRoute route; route.index = index; route.sharedMemoryName = f.name;
            f.router.addRoute(route);
        }
        GraphEmsNodeConfig node; node.id = "recovery"; node.type = "feedbackRecovery";
        node.params = {{"healthyIndex","1"},{"faultIndex","2"},{"permitIndex","3"},
            {"resetIndex","4"},{"stoppedIndex","5"},{"startIndex","6"},{"outputIndex","7"},
            {"stateOutputIndex","8"},{"reasonOutputIndex","9"},{"requestIndex","10"},
            {"readbackIndex","11"},{"feedbackIndex","12"},{"sameDirection","true"},
            {"requestTolerance","0.5"},{"feedbackTolerance","1"},{"stoppedTolerance","0.5"},
            {"requestTimeoutMs","500"},{"feedbackTimeoutMs","600"},{"stableMs","100"},
            {"maxInputAgeMs","2000"},{"maxGapMs","1000"}};
        f.config.nodes = {node}; f.restart();
        const auto step = [&](double reset, double start, std::int64_t lead = 0,
                              std::int64_t delay = 0, std::int64_t ttl = 2000) {
            f.now += 100;
            const double inputs[] = {1, 0, start, reset, 1, start, 0, 0, 0, 0, 0, 0};
            for (unsigned index : {1, 2, 3, 4, 5, 6, 10, 11, 12}) {
                PointValue p; p.index = index; p.value = inputs[index - 1]; p.quality = 1;
                p.ts = f.now + lead; p.expireAt = p.ts + ttl;
                require(f.router.putLatestByIndex(p).accepted, "seed recovery input");
            }
            const auto result = f.engine->runOnce(f.now, 5, [&] { return f.now + delay; });
            f.now += delay;
            require(result.errors.empty() && result.deviceWrites == 0, "recovery scan without device writes");
        };
        step(0, 0); step(1, 0); step(0, 0); step(0, 1);
        require(f.status() == 1, "separate start grants recovery permission");
        if (kind == 0) step(0, 1, 50, 100);
        if (kind == 1) step(0, 1, 101, 100);
        if (kind == 2) step(0, 1, 0, 100, 50);
        if (kind == 3) step(0, 1, 50, 1100);
        if (kind == 4) step(0, 1, 0, -200);
        require(f.status() == (kind == 0 ? 1 : 0), "recovery uses capture completion without relaxing protection");
        require(f.router.getLatestByIndex(7, f.now)->ts == f.now, "recovery output uses capture completion");
        if (kind != 0) {
            step(0, 1);
            require(f.status() == 0, "valid recovery data alone cannot restart after trip");
        }
    }
}
void testLiveOwnerObservationClock() {
    Fixture f;
    const auto graph = (f.root / "graph.json").string(), health = (f.root / "health.json").string();
    {
        std::ofstream out(graph);
        out << R"({"schemaVersion":"2.0.0","graphCode":"clock","compile":{"maxNodes":1,"maxEdges":1,"virtualIndexStart":7,"virtualIndexEnd":16},"nodes":[{"id":"output","type":"confirmedControlWrite","parameters":{)";
        bool comma = false;
        for (const auto& p : f.config.nodes.front().params) {
            if (comma) out << ','; comma = true;
            out << '"' << p.first << "\":" << p.second;
        }
        out << R"(},"ports":[]}],"links":[]})";
    }
    f.engine.reset();
    ComputeEngineConfig config; config.enabled = true; config.scanIntervalMs = 5;
    config.healthFile = health; config.healthPublishIntervalMs = 100;
    ComputeRuleConfig rule; rule.ruleCode = "clock"; rule.enabled = true;
    rule.trigger.intervalMs = 5; rule.script.type = "graphEms"; rule.script.graphFile = graph;
    rule.script.graphStateFile = (f.root / "live-state.json").string(); config.rules = {rule};
    ComputeEngineService service(config, f.router);
    service.start();
    std::set<std::int64_t> publications;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (publications.size() < 10 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!fs::exists(health)) continue;
        std::ifstream input(health);
        const auto observed = json::JsonParser(std::string(std::istreambuf_iterator<char>(input), {})).parse();
        const auto sampledAt = static_cast<std::int64_t>(observed.find("ts")->asNumber());
        const auto& commands = *observed.find("loadedGraphs")->asArray().values.front()->find("commands");
        require(commands.find("nodes")->asArray().values.size() == 1, "live report contains actual command owner");
        std::size_t count = 0;
        compute_observation_detail::commands(commands, sampledAt, count);
        publications.insert(sampledAt);
    }
    service.stop();
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    require(!observeComputeGraphs(health, now).empty(), "final report accepted by production query");
    require(publications.size() == 10, "ten live reports validate command times against their publication time");
    require(f.store->peekPendingWriteCommands().empty(), "live observation with no permission never writes");
}
void testFormulaExpiryDuringCapture() {
    for (const auto ttl : {50, 100}) {
        Fixture f;
        GraphEmsNodeConfig node; node.id = "formula"; node.type = "formula";
        node.params = {{"operation","add"},{"inputs.count","1"},{"inputs.0.index","1"},{"outputIndex","7"}};
        f.config.nodes = {node}; f.restart(); f.seed(f.now, ttl);
        const auto result = f.engine->runOnce(f.now, 5, [&] { return f.now + 100; });
        require(result.errors.empty(), "formula capture scan");
        const auto output = f.router.getLatestByIndex(7, f.now + 100);
        require(result.latestWrites == (ttl == 100 ? 1 : 0), "formula capture expiry boundary ttl=" + std::to_string(ttl) +
            " writes=" + std::to_string(result.latestWrites) + " inputExpiry=" +
            std::to_string(f.router.getLatestByIndex(1, f.now)->expireAt) + " now=" + std::to_string(f.now));
        if (ttl == 100) require(output && output->ts == f.now + 100 && output->value == f.request, "expiry boundary stays inclusive");
        else require(!output || output->quality != 1, "expired input cannot publish a good formula value");
    }
}
}
int main() {
    try {
        testRecoverySnapshotClock();
        testLiveOwnerObservationClock();
        testFormulaExpiryDuringCapture();
        { Fixture f; auto c = f.send(); f.store->recordWritebackResult(f.receipt(c)); f.readback = f.actual = 20;
          f.step(); require(f.status() == 3, "initial confirmed command before capture delay");
          require(f.capturedStep(50, 100).deviceWrites == 0 && f.status() == 3,
                  "new producer sample during capture is not a future input fault");
          require(f.router.getLatestByIndex(7, f.now)->ts == f.now, "output uses capture completion timestamp");
          require(f.store->peekPendingWriteCommands().empty(), "capture does not replay confirmed command"); }
        for (int kind = 0; kind < 4; ++kind) {
          Fixture f; auto c = f.send(); f.store->recordWritebackResult(f.receipt(c)); f.readback = f.actual = 20; f.step();
          const auto r = kind == 0 ? f.capturedStep(101, 100) : kind == 1 ? f.capturedStep(0, 100, 50) :
                         kind == 2 ? f.capturedStep(50, 1100) : f.capturedStep(0, -200);
          require(r.deviceWrites == 0 && f.status() == -4 && f.store->peekPendingWriteCommands().empty(),
                  "future sample, capture expiry, scan gap and clock rollback still fail closed");
        }
        { Fixture f; f.send();
          const auto original = f.engine->commandObservation();
          const auto reject = [&](const std::string& before, const std::string& after) {
              auto bytes = original; const auto pos = bytes.find(before); require(pos != std::string::npos, "malformed fixture match");
              bytes.replace(pos, before.size(), after); bool failed = false;
              try { std::size_t count = 0; compute_observation_detail::commands(json::JsonParser(bytes).parse(), f.now, count); }
              catch (...) { failed = true; }
              require(failed, "malformed command publication accepted");
          };
          reject("\"status\":2", "\"status\":3");
          reject("\"confirmed\":false", "\"confirmed\":true");
          reject("\"index\":2", "\"index\":3");
          reject("\"lastScan\":" + std::to_string(f.now), "\"lastScan\":" + std::to_string(f.now + 1));
          reject("\"fault\":0", "\"fault\":-1");
        }
        { Fixture f; f.reset = f.permit = 1; for (int i = 0; i < 4; ++i) require(f.step().deviceWrites == 0, "held startup never sends"); }
        { Fixture f; auto c = f.send(); f.request = 30; require(f.step().deviceWrites == 0, "moving target cannot bypass pending");
          auto wrong = f.receipt(c); wrong.requestedAt--; f.store->recordWritebackResult(wrong); f.step(); require(f.status() == 2, "wrong receipt identity ignored");
          f.store->recordWritebackResult(f.receipt(c)); f.readback = f.actual = 20;
          require(f.step().deviceWrites == 1, "next changed target only after original receipt"); }
        { Fixture f; auto c = f.send(); f.store->recordWritebackResult(f.receipt(c, false)); f.readback = f.actual = 20;
          f.step(); require(f.status() == -1, "matching power cannot hide failure");
          f.request = 30; f.step(); require(f.store->peekPendingWriteCommands().empty(), "failure cannot retry new ID");
          f.restart(); f.step(); require(f.status() == -1, "failure survives restart"); f.arm(); f.permit = 1;
          require(f.step().deviceWrites == 1, "stopped reset permits separate run"); }
        { Fixture f; auto c = f.send(); f.checkObservation(c, 2, true, false);
          for (int i = 0; i < 5; ++i) f.step(); require(f.status() == -2, "missing receipt timeout");
          f.checkObservation(c, -2, true, false);
          f.permit = 0; f.step(); f.reset = 1; f.step(); require(f.status() == -2, "unresolved pending cannot reset");
          f.store->recordWritebackResult(f.receipt(c)); f.step(); require(f.status() == -2, "late success never clears timeout");
          f.checkObservation(c, -2, false, true);
          f.arm(); }
        { Fixture f; auto c = f.send(); f.restart(); f.step(); require(f.status() == 2, "restart retains original pending");
          f.checkObservation(c, 2, true, false);
          f.store->recordWritebackResult(f.receipt(c)); f.step(); require(f.status() == 0, "receipt after restart cannot restore arming");
          require(f.store->peekPendingWriteCommands().empty(), "restart never replays"); }
        { Fixture f; auto c = f.send(); f.store->recordWritebackResult(f.receipt(c)); f.readback = f.actual = 20;
          f.step(); require(f.status() == 3, "original verified success"); f.step(); require(f.store->peekPendingWriteCommands().empty(), "confirmed unchanged output suppressed");
          f.permit = 0; f.step(); f.permit = 1; f.request = 30; f.step(); require(f.status() == 0, "permit recovery cannot rearm"); }
        { Fixture f; f.arm(); f.permit = 1; require(f.step(false, 0).deviceWrites == 0, "budget cannot leave phantom flight");
          require(f.step().deviceWrites == 1, "fresh request after budget available"); }
        { Fixture f; f.arm(); f.permit = 1; fs::create_directory(f.root / "state.json.tmp"); f.step(true);
          require(f.status() == -3 && f.store->peekPendingWriteCommands().empty(), "storage failure before submit must fail closed"); }
        { Fixture f; f.send(); f.engine.reset(); std::ofstream(f.root / "state.json") << "broken"; f.restart(); f.step(true);
          require(f.status() == -3, "corrupt journal locked"); std::ifstream in(f.root / "state.json"); std::string s; in >> s; require(s == "broken", "preserve corrupt original"); }
        { Fixture f; bool blocked = false;
          try { GraphEmsEngine second(f.config, f.router, 600, (f.root / "state.json").string()); }
          catch (...) { blocked = true; } require(blocked, "exclusive journal ownership"); }
        { Fixture f; f.send(); f.now += 1100; f.step(); require(f.status() < 0, "scan discontinuity blocks output"); }
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "confirmed control identity, failure, timeout and restart passed\n";
}
