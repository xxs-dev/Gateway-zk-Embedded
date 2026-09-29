#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <unordered_map>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <unistd.h>
#include "edge_gateway/graph_ems_engine.hpp"

using namespace edge_gateway;

void persistenceTest() {
    char path[]="/tmp/comm103-mode-test-XXXXXX";
    if(!mkdtemp(path)) throw std::runtime_error("Cannot create test directory");
    struct CleanupDir {std::string p;~CleanupDir(){std::remove((p+"/730100.value").c_str());rmdir(p.c_str());}} dir{path};
    auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    for(int generation=0;generation<3;++generation) {
        if(generation==2){std::ofstream file(std::string(path)+"/730100.value");file<<"corrupt";}
        const auto name="comm103_mode_test_"+std::to_string(getpid())+"_"+std::to_string(generation);
        struct Cleanup {std::string n;~Cleanup(){MemoryPointStore::cleanupOrphanedSegment(n);}} cleanup{name};
        MemoryPointStore store(name);PointStoreRouter router;
        router.setEmsVirtualParameterDirectory(path);router.addStore(name,store);
        PointStoreRoute r;r.index=730100;r.machineCode="TEST";r.meterCode="EMS_CORE";r.pointCode="local_control_mode";
        r.protocolType="ems_virtual";r.sharedMemoryName=name;r.writable=true;r.initialValue=0;
        r.write.enable=true;r.write.minValue=0;r.write.maxValue=2;r.write.step=1;r.write.allowedValues={0,1,2};
        router.addRoute(r);
        auto current=router.getLatestByIndex(r.index,now);
        if(!current || current->value!=0) throw std::runtime_error("Mode missing/restart/corruption not paused");
        if(generation) continue;
        PendingWriteCommand c;c.index=r.index;c.value=2;c.cmdId="test-mode";c.ts=now;c.source="scada-local";
        if(!router.submitWriteCommand(c).accepted) throw std::runtime_error("Mode write failed");
        auto receipt=store.getWritebackResult(c.cmdId,c.index);
        if(!receipt || !receipt->success || receipt->stage!="local-parameter-committed") throw std::runtime_error("Bad receipt");
        for(double bad:{0.5,3.0,std::numeric_limits<double>::quiet_NaN()}){
            c.value=bad;if(router.submitWriteCommand(c).accepted) throw std::runtime_error("Invalid mode accepted");
        }
        c.value=0;c.cmdId="pause";if(!router.submitWriteCommand(c).accepted) throw std::runtime_error("Pause rejected");
        current=router.getLatestByIndex(r.index,now+86400000);
        if(!current || current->stale || current->value!=0) throw std::runtime_error("Pause expired");
        router.setEmsVirtualParameterDirectory("/proc/comm103-mode-test-impossible");c.value=1;
        if(router.submitWriteCommand(c).accepted || router.getLatestByIndex(r.index,now)->value!=0)
            throw std::runtime_error("Persistence failure changed mode");
    }
    std::cout<<"PASS mode persistence, receipt, restart, corruption, invalid value, disk failure\n";
}

// All routes and writes stay in a unique simulator segment, never a physical driver.
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        auto graph = GraphEmsConfig::loadFromFile(argv[1]);
        const auto name = "comm103_full_sim_" + std::to_string(getpid());
        struct Cleanup { std::string n; ~Cleanup(){MemoryPointStore::cleanupOrphanedSegment(n);} } cleanup{name};
        MemoryPointStore store(name); PointStoreRouter router; router.addStore(name,store);
        std::unordered_map<std::string,std::string> profile;
        std::map<unsigned,double> seeds;
        std::ifstream fixture(argv[2]); std::string kind;
        while(fixture >> kind) {
            if(kind=="P") {std::string k,v;fixture>>k>>v;profile[k]=v;}
            else if(kind=="I") {unsigned i;double v;fixture>>i>>v;seeds[i]=v;}
            else throw std::runtime_error("Invalid fixture");
        }
        if(seeds.empty()) throw std::runtime_error("Empty fixture");
        for(const auto& item:seeds) {
            PointStoreRoute r;r.index=item.first;r.machineCode="SIMULATED";r.meterCode="TEST";
            r.pointCode=std::to_string(item.first);r.sharedMemoryName=name;r.writable=true;router.addRoute(r);
        }
        std::int64_t now=1788800000000;
        auto put=[&](unsigned i,double v,int q=1) {
            PointValue p;p.index=i;p.value=v;p.quality=q;p.ts=now;p.expireAt=now+86400000;
            if(!router.putLatestByIndex(p).accepted) throw std::runtime_error("Unrouted seed "+std::to_string(i));
        };
        for(const auto& item:seeds) put(item.first,item.second);
        for(auto i:{1399,3999,1214,1210,1211}) put(i,1);
        for(auto i:{1212,1215,1462,8199,1209}) put(i,0);
        put(20128,4);put(1566,1000);put(1556,200);put(1557,200);put(1569,50);put(730100,1);put(730101,0);
        auto get=[&](unsigned i) {
            auto p=router.getLatestByIndex(i,now);
            if(!p || p->quality!=1 || p->stale) throw std::runtime_error("Invalid output "+std::to_string(i));
            return p->value;
        };
        GraphEmsEngine engine(graph,router,600000,"",profile);
        auto run=[&](const char* label,double expected) {
            now+=10000;
            auto result=engine.runOnce(now,100);
            if(!result.errors.empty()) {
                for(const auto& e:result.errors) std::cerr<<e<<'\n';
                throw std::runtime_error("Graph execution errors");
            }
            for(unsigned i=700345;i<=700350;++i) {
                const double target=i<700348?expected:0;
                if(std::fabs(get(i)-target)>.001) {
                    for(auto j:{400017,730102,730104,730105,700302,400627,401552,401553,700333,700338,700343})
                        std::cerr<<j<<'='<<get(j)<<' ';
                    throw std::runtime_error(std::string(label)+" wrong final output "+std::to_string(i)+"="+std::to_string(get(i)));
                }
            }
            auto commands=store.drainPendingWriteCommands();
            for(const auto& c:commands) {
                if(c.index>=1318 && c.index<=1323 && std::fabs(c.value-(c.index<1321?expected:0))>.001)
                    throw std::runtime_error("Wrong queued power");
                if(c.index==1201 && c.value!=0 && (get(730104)!=1 || get(730105)==0))
                    throw std::runtime_error("Start without mode/direction permission");
                put(c.index,c.value);
            }
            if(get(1399)==1) for(unsigned i=1318;i<=1323;++i)
                if(std::fabs(get(i)-(i<1321?expected:0))>.001) throw std::runtime_error("Simulator power not updated");
            std::cout<<"PASS "<<label<<'\n';
        };
        run("auto-discharge",-30);
        put(730100,0);run("pause",0);
        put(730101,30);put(730100,2);run("manual-charge",10);
        put(1556,15);run("bms-charge-limit",5);
        put(1556,0);run("bms-charge-inhibit",0);
        put(1556,200);put(730101,-30);run("manual-discharge",-10);
        put(1557,12);run("bms-discharge-limit",-4);
        put(1557,0);run("bms-discharge-inhibit",0);
        put(1557,200);put(730101,0);run("manual-zero",0);
        put(730101,27);put(730100,1);run("resume-auto-ignores-cached-manual",-30);
        put(1569,20);run("soc-lower-charge",30);
        put(1569,50);put(400026,1);put(400590,6);run("original-auto-override-preserved",2);
        put(400026,0);put(400590,0);run("original-auto-override-released",30);
        put(1569,95);run("soc-upper-discharge",-30);
        put(730100,2);put(730101,30);run("manual-upper-inhibit",0);
        put(730101,-30);put(1569,20);run("manual-lower-inhibit",0);
        put(1569,50);put(730101,124.8);run("existing-phase-limit",30);
        put(730100,9);run("invalid-mode",0);
        put(730100,1,0);run("bad-quality-mode",0);
        put(730100,2);put(730101,30,0);run("bad-quality-manual-power",0);
        put(730101,30);put(3999,0);run("bms-offline",0);
        put(3999,1);put(1399,0);run("pcs-offline",0);
        put(1399,1);put(1462,1);run("bms-fault",0);
        put(1462,0);put(1211,0);put(730100,2);put(730101,0);
        run("standby-manual-zero-no-start",0);run("standby-manual-zero-no-start-next-scan",0);
        put(730100,0);run("standby-paused-no-start",0);
        std::cout<<"PASS complete graph and isolated device queue; no hardware access\n";
        persistenceTest();
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
