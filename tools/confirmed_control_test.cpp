#include "edge_gateway/graph_ems_engine.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace edge_gateway;
namespace fs = std::filesystem;
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
    GraphEmsRunResult step(bool errorsAllowed = false, std::size_t budget = 5) {
        now += 100;
        const double values[] = {request, readback, permit, reset, stopped, actual};
        for (unsigned i = 0; i < 6; ++i) {
            PointValue p; p.index = i + 1; p.value = values[i]; p.ts = now; p.expireAt = now + 2000; p.quality = 1;
            require(router.putLatestByIndex(p).accepted, "seed input");
        }
        auto r = engine->runOnce(now, budget);
        if (!errorsAllowed && !r.errors.empty()) throw std::runtime_error(r.errors.front());
        return r;
    }
    int status() { return static_cast<int>(router.getLatestByIndex(7, now)->value); }
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
}
int main() {
    try {
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
        { Fixture f; auto c = f.send(); for (int i = 0; i < 5; ++i) f.step(); require(f.status() == -2, "missing receipt timeout");
          f.permit = 0; f.step(); f.reset = 1; f.step(); require(f.status() == -2, "unresolved pending cannot reset");
          f.store->recordWritebackResult(f.receipt(c)); f.step(); require(f.status() == -2, "late success never clears timeout");
          f.arm(); }
        { Fixture f; auto c = f.send(); f.restart(); f.step(); require(f.status() == 2, "restart retains original pending");
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
