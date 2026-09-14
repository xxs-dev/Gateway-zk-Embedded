#include "edge_gateway/graph_ems_engine.hpp"
#include <cmath>
#include <iostream>
#include <fstream>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <unistd.h>

using namespace edge_gateway;
int main() {
    const auto name = "grid_reserve_test_" + std::to_string(getpid());
    struct Cleanup { std::string name; ~Cleanup() { MemoryPointStore::cleanupOrphanedSegment(name); } } cleanup{name};
    MemoryPointStore store(name);
    PointStoreRouter router; router.addStore(name, store);
    for (unsigned i = 1; i <= 13; ++i) {
        PointStoreRoute r; r.index=i; r.machineCode="TEST"; r.meterCode="TEST";
        r.pointCode=std::to_string(i); r.sharedMemoryName=name; router.addRoute(r);
    }
    GraphEmsConfig cfg;
    GraphEmsNodeConfig n; n.id="reserve"; n.type="gridReserve";
    n.params={{"modeIndex","1"},{"socIndex","2"},{"gridConnectedIndex","3"},
        {"safetyPermitIndex","4"},{"chargePermitIndex","5"},{"fullIndex","6"},
        {"phasePowerIndex","7"},{"maxPhaseKw","10"},{"paOutput","8"},{"pbOutput","9"},
        {"pcOutput","10"},{"statusOutput","11"},{"standbyOutput","12"}};
    cfg.nodes.push_back(n);
    const auto fixture="/tmp/"+name+".json";
    struct FileCleanup { std::string path; ~FileCleanup() { std::remove(path.c_str()); } } fileCleanup{fixture};
    auto validate=[&](const GraphEmsNodeConfig& node, bool rejected) {
        std::ofstream out(fixture);
        out << "{\"schemaVersion\":\"2.0.0\",\"graphCode\":\"test\","
               "\"compile\":{\"maxNodes\":16,\"maxEdges\":16,\"virtualIndexStart\":8,\"virtualIndexEnd\":99},"
               "\"nodes\":[{\"id\":\"reserve\",\"type\":\"gridReserve\",\"parameters\":{";
        bool first=true;
        for (const auto& p:node.params) {
            if(!first) out << ',';
            first=false;
            out << '"' << p.first << "\":" << p.second;
        }
        out << "},\"ports\":[]}],\"links\":[]}";
        out.close();
        bool threw=false;
        try { GraphEmsConfig::loadFromFile(fixture); } catch(const std::exception& e) {
            threw=true;
            if(!rejected) throw std::runtime_error(std::string("valid fixture rejected: ")+e.what());
        }
        if(threw!=rejected) throw std::runtime_error("gridReserve validation result mismatch");
    };
    validate(n,false);
    auto invalid=n;invalid.params["standbyOutput"]="8";validate(invalid,true);
    invalid=n;invalid.params["socIndex"]="8";validate(invalid,true);
    invalid=n;invalid.params["maxPhaseKw"]="0";validate(invalid,true);
    invalid=n;invalid.params["restartSoc"]="100";validate(invalid,true);
    GraphEmsEngine engine(cfg,router,1000);
    std::int64_t now=100000;
    auto put=[&](unsigned i,double value,int quality=1) {
        PointValue p;p.index=i;p.value=value;p.quality=quality;p.ts=now;p.expireAt=now+1000;
        if (!router.putLatestByIndex(p).accepted) throw std::runtime_error("seed failed");
    };
    auto step=[&](double soc,int mode,int grid,int safety,int charge,int full,double kw,double expected,int status,int standby) {
        now+=100;
        put(1,mode);put(2,soc);put(3,grid);put(4,safety);put(5,charge);put(6,full);put(7,kw);
        auto result=engine.runOnce(now);
        if (!result.errors.empty()) throw std::runtime_error(result.errors.front());
        if (result.deviceWrites!=0) throw std::runtime_error("reserve bypassed control layer");
        for (unsigned i=8;i<=12;++i) {
            auto p=router.getLatestByIndex(i,now);
            const auto want=i==11?status:(i==12?standby:expected);
            if (!p || std::abs(p->value-want)>1e-8) throw std::runtime_error("unexpected output "+std::to_string(i)+" at SOC "+std::to_string(soc));
        }
    };
    step(99.5,3,1,1,1,0,1,0,2,1); // Cold start in hysteresis band stays at zero.
    step(98.9,3,1,1,1,0,1,1,1,1);
    step(99.8,3,1,1,1,0,1,1,1,1); // Must not stop charging at 99.
    step(100,3,1,1,0,1,1,0,2,1); // Full and charge inhibited still enables standby.
    step(99,3,1,1,1,0,1,0,2,1);
    step(98.9,3,1,1,1,0,1,1,1,1);
    step(90,3,1,1,0,0,1,0,4,1);
    step(99.2,3,1,1,1,0,1,1,1,1); // Permission recovery preserves the active charge cycle.
    step(90,3,0,1,1,0,1,0,3,1);
    step(90,3,0,0,1,0,1,0,4,0); // Real safety denial wins over not-connected.
    step(90,3,2,1,1,0,1,0,4,0); // Unknown enum is not a safe transition.
    step(90,3,0,1,1,0,1,0,3,1);
    put(3,0,0);
    engine.runOnce(now);
    if (router.getLatestByIndex(8,now)->value!=0 || router.getLatestByIndex(12,now)->value!=0)
        throw std::runtime_error("invalid grid quality retained standby");
    step(99.5,3,1,1,1,0,1,0,2,1);
    step(90,3,1,1,1,0,1,1,1,1);
    step(90,3,1,0,1,0,1,0,4,0);
    step(99.5,3,1,1,1,0,1,0,2,1);
    step(90,3,1,1,1,0,1,1,1,1);
    step(90,0,1,1,1,0,1,0,0,0);
    step(99.5,3,1,1,1,0,1,0,2,1);
    step(90,2,1,1,1,0,1,0,0,0);
    step(90,3,1,1,1,0,11,0,4,1);
    step(-1,3,1,1,1,0,1,0,4,0);
    step(101,3,1,1,1,0,1,0,4,0);
    step(90,3,1,1,1,1,1,0,2,1);
    step(90,3,1,1,1,0,1,1,1,1);
    put(2,90,0);
    auto badQuality=engine.runOnce(now);
    if (!badQuality.errors.empty() || router.getLatestByIndex(8,now)->value!=0)
        throw std::runtime_error("bad quality retained charge");
    step(90,3,1,1,1,0,1,1,1,1);
    now+=2000;auto expired=engine.runOnce(now);
    if (!expired.errors.empty() || router.getLatestByIndex(8,now)->value!=0 || router.getLatestByIndex(12,now)->value!=0)
        throw std::runtime_error("expired input retained charge");
    step(90,3,1,1,1,0,1,1,1,1);
    put(5,1,0);
    engine.runOnce(now);
    if (router.getLatestByIndex(8,now)->value!=0 || router.getLatestByIndex(12,now)->value!=1)
        throw std::runtime_error("invalid charge permission lost safe standby");
    step(99.5,3,1,1,1,0,1,1,1,1);
    PointValue staleSoc; staleSoc.index=2; staleSoc.value=90; staleSoc.ts=now-2000;
    staleSoc.expireAt=now-1000; staleSoc.quality=1;
    router.putLatestByIndex(staleSoc);
    engine.runOnce(now);
    if (router.getLatestByIndex(8,now)->value!=0 || router.getLatestByIndex(12,now)->value!=0)
        throw std::runtime_error("single stale SOC retained charge");
    step(90,3,1,1,1,0,1,1,1,1);
    auto broken=cfg;
    broken.nodes[0].params["standbyOutput"]="14"; // Missing route after four successful writes.
    GraphEmsNodeConfig after; after.id="after"; after.type="switch";
    after.params={{"outputIndex","13"},{"conditionIndex","1"},{"trueIndex","8"},{"falseValue","777"}};
    broken.nodes.push_back(after);
    broken.edges.push_back({"reserve","after"});
    put(13,777);
    GraphEmsEngine brokenEngine(broken,router,1000);
    auto failure=brokenEngine.runOnce(now);
    if (failure.errors.empty() || router.getLatestByIndex(13,now)->value!=0)
        throw std::runtime_error("partial publication did not use protective zero fallback");
    for (unsigned i=8;i<=11;++i)
        if (router.getLatestByIndex(i,now)->quality==1)
            throw std::runtime_error("partial candidate remained valid");
    std::cout << "PASS grid reserve hysteresis, standby, interlocks, limits, stale data, no device writes\n";
}
