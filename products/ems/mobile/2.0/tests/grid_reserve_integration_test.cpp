#include "edge_gateway/graph_ems_engine.hpp"
#include <cmath>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <unistd.h>

using namespace edge_gateway;

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        auto full = GraphEmsConfig::loadFromFile(argv[1]);
        auto nodeIndex=[&](const std::string& id,const std::string& key) {
            for(const auto& n:full.nodes) if(n.id==id) return static_cast<unsigned>(std::stoul(n.params.at(key)));
            throw std::runtime_error("Missing integration node: "+id);
        };
        PointStoreRouter empty;
        GraphEmsEngine validate(full, empty);
        std::set<std::string> selected = {"power_constraints", "cycle_basic_safety_gate",
            "cycle_charge_command_allowed", "cycle_discharge_command_allowed",
            "cycle_charge_direction_allowed", "cycle_discharge_direction_allowed",
            "cycle_direction_allowed", "cycle_start_hold_sequence", "cycle_start_hold_value",
            "cycle_phase_mode_zero", "cycle_phase_control_mode", "cycle_operable_state_gate",
            "cycle_runtime_safety_gate", "cycle_zero", "cycle_stop_clear", "cycle_start",
            "pcs_energy_stop_permit", "pcs_energy_stop"};
        for (int i=0; i<3; ++i) {
            selected.insert("cycle_safe_p"+std::to_string(i));
            selected.insert("cycle_safe_q"+std::to_string(i));
        }
        for (int i=0; i<6; ++i) selected.insert("cycle_pcs_control_write_"+std::to_string(i));
        for (const auto& n : full.nodes)
            if (n.id.find("grid_reserve_")==0) selected.insert(n.id);
        auto graph=full;
        graph.nodes.clear(); graph.edges.clear(); graph.indexClaims.clear();
        for (const auto& n : full.nodes) if (selected.count(n.id)) graph.nodes.push_back(n);
        // Preserve order through omitted automatic-policy nodes, whose outputs
        // are injected below. All new nodes, constraints and actual writers run.
        for (const auto& source : selected) {
            std::set<std::string> visited;
            std::function<void(const std::string&)> walk = [&](const std::string& current) {
                if (!visited.insert(current).second) return;
                for (const auto& e : full.edges) if (e.from==current) {
                    if (selected.count(e.to)) graph.edges.push_back({source,e.to});
                    else walk(e.to);
                }
            };
            walk(source);
        }
        const auto segment="comm104_reserve_integration_"+std::to_string(getpid());
        struct Cleanup { std::string n; ~Cleanup(){MemoryPointStore::cleanupOrphanedSegment(n);} } cleanup{segment};
        MemoryPointStore store(segment);
        PointStoreRouter router;
        router.addStore(segment,store);
        std::set<unsigned> indices;
        unsigned standby=0;
        std::vector<unsigned> reserveOutputs;
        std::string reserveNode;
        for (const auto& n : graph.nodes) {
            if (n.type=="gridReserve") {
                standby=std::stoul(n.params.at("standbyOutput"));
                reserveNode=n.id;
                for (const auto* key : {"paOutput", "pbOutput", "pcOutput", "statusOutput", "standbyOutput"})
                    reserveOutputs.push_back(std::stoul(n.params.at(key)));
            }
            for (const auto& p : n.params) {
                if (p.first.find("Index")==std::string::npos && p.first.find("index")==std::string::npos &&
                    p.first.find("Output")==std::string::npos) continue;
                std::size_t end=0;
                try { auto i=std::stoul(p.second,&end); if(i && end==p.second.size()) indices.insert(i); }
                catch(...) {}
            }
        }
        for (auto i : indices) {
            PointStoreRoute r; r.index=i; r.machineCode="TEST_ONLY"; r.meterCode="TEST";
            r.pointCode=std::to_string(i); r.sharedMemoryName=segment;
            r.writable=true; r.write.enable=true; r.write.dataType="float64";
            router.addRoute(r);
        }
        std::int64_t now=1000000;
        auto put=[&](unsigned i, double v) {
            PointValue p;p.index=i;p.value=v;p.quality=1;p.ts=now;p.expireAt=now+600000;
            if(!router.putLatestByIndex(p).accepted) throw std::runtime_error("seed failed "+std::to_string(i));
        };
        for(auto i:indices) put(i,0);
        for(auto i:{1399,3999,1214,1216,1210}) put(i,1);
        for(auto i:{1556,1557,1552,1553,151}) put(i,124.8);
        put(1279,48059);put(17,2);put(535,41.6);put(504,41.6);
        put(nodeIndex("force_full_soc_upper","outputIndex"),95);put(162,20);put(740109,1);
        for(int i=0;i<3;++i) put(nodeIndex("force_full_active_"+std::to_string(i),"outputIndex"),8);
        // Historical SOH misuse must not become a fixture dependency for the
        // current graph, which already uses true SOC correctly.
        if(indices.count(1570)) put(1570,50);
        // Old reserve-capacity branch would inject discharge even into zero input.
        put(23,1);put(457,0);put(595,1);
        for(auto i:{309,310,311}) put(i,10);
        GraphEmsEngine engine(graph,router,600000,"",{{"CHARGE_DISCHARGE_TEST","1"},
            {"PCS_ENERGY_SAVING","1"}});
        auto expect=[&](unsigned i,double want,const std::string& label) {
            auto p=router.getLatestByIndex(i,now);
            if(!p || p->quality!=1 || std::abs(p->value-want)>1e-7)
                throw std::runtime_error(label+" index "+std::to_string(i)+" got "+
                    (p?std::to_string(p->value):"missing")+" expected "+std::to_string(want));
        };
        auto step=[&](const std::string& label,int mode,double soc,int bms,double power,int status,int wantStandby) {
            now+=10000;put(740100,mode);put(1569,soc);put(1279,bms);
            std::set<unsigned> expectedPowerWrites;
            for(auto i:{1318,1319,1320}) {
                const auto current=router.getLatestByIndex(i,now);
                if(!current || std::abs(current->value-power)>.05) expectedPowerWrites.insert(i);
            }
            auto result=engine.runOnce(now);
            if(!result.errors.empty()) throw std::runtime_error(label+": "+result.errors.front());
            expect(740104,status,label);expect(standby,wantStandby,label);
            for(auto i:{740106,740107,740108}) expect(i,power,label);
            bool start=false;
            std::set<unsigned> actualPowerWrites;
            for(const auto& command:store.drainPendingWriteCommands()) {
                if(command.index==1202 && command.value!=0)
                    throw std::runtime_error(label+": unexpected STOP command");
                if(command.index>=1318 && command.index<=1320 && std::abs(command.value-power)>1e-7)
                    throw std::runtime_error(label+": hardware power differs from final safe command");
                if(command.index>=1318 && command.index<=1320) actualPowerWrites.insert(command.index);
                if(command.index==1201 && command.value==1) start=true;
                // Simulated acknowledgement only; no driver is connected.
                put(command.index,command.value);
            }
            if(actualPowerWrites!=expectedPowerWrites)
                throw std::runtime_error(label+": changed phase command missing from write queue");
            std::cout<<"PASS "<<label<<'\n';
            return start;
        };
        step("enable-at-full",3,100,4369,0,2,1);
        if(!step("full-zero-standby-start",3,100,4369,0,2,1))
            throw std::runtime_error("Full standby did not use existing start writer");
        put(1211,1);
        step("reserve-above-old-95",3,98,48059,1,1,1);
        step("latched-through-99.5",3,99.5,48059,1,1,1);
        step("full-charge-inhibit-zero",3,100,4369,0,2,1);
        step("pause-no-stop",0,100,4369,0,0,0);
        step("cold-band-zero",3,99.5,48059,0,2,1);
        step("charge-again",3,98,48059,1,1,1);
        put(1212,1);step("fault-zero",3,98,48059,0,4,0);put(1212,0);
        put(1216,0);step("not-connected-zero-keeps-standby",3,98,48059,0,3,1);
        put(1212,1);step("not-connected-real-fault",3,98,48059,0,4,0);put(1212,0);
        put(1211,0);put(1201,0);put(1202,1);
        bool transitionStart=false;
        for(int attempt=0;attempt<8;++attempt)
            transitionStart=step("not-connected-start-permitted",3,98,48059,0,3,1)||transitionStart;
        if(!transitionStart) throw std::runtime_error("Not-connected feedback blocked safe PCS enable");
        expect(1202,0,"not-connected-clears-stop");
        put(1211,1);put(1216,1);
        step("charge-inhibited",3,98,4369,0,4,1);
        put(740109,-1);step("invalid-request-keeps-standby",3,98,48059,0,4,1);put(740109,1);
        for(auto i:{740101,740102,740103}) put(i,2);
        step("manual-charge",2,50,48059,2,0,0);
        for(auto i:{740101,740102,740103}) put(i,-2);
        step("manual-discharge",2,50,48059,-2,0,0);
        step("manual-to-pause",0,50,48059,0,0,0);
        put(1202,1);put(1211,0);
        step("paused-manual-stop",0,100,48059,0,0,0);
        expect(1202,1,"pause-must-preserve-manual-stop");
        put(1212,1);
        step("retained-stop-with-real-fault",3,100,48059,0,4,0);
        expect(1202,1,"fault-must-not-clear-stop");
        put(1212,0);
        step("reenable-with-retained-stop-coil",3,100,48059,0,2,1);
        expect(1202,0,"enabled-strategy-clears-retained-stop");
        bool restarted=false;
        for(int attempt=0;attempt<8;++attempt)
            restarted=step("wait-for-start-sequence",3,100,48059,0,2,1)||restarted;
        if(!restarted) throw std::runtime_error("Retained stop coil prevented PCS restart");
        put(1211,1);
        step("operator-reenters-reserve",3,98,48059,1,1,1);
        // Recreate only the router to inject a rejected candidate publication.
        // Keep shared memory with acknowledged nonzero PCS commands from the
        // preceding real scan, so skipped downstream writers cannot pass.
        for (const auto rejected : reserveOutputs) {
            step("nonzero-before-publication-failure",3,98,48059,1,1,1);
            for (auto i : {1318,1319,1320}) expect(i,1,"nonzero-hardware-precondition");
            const auto previous=store.getLatestByIndex(rejected,now);
            if(!previous || previous->quality!=1 || previous->value!=1)
                throw std::runtime_error("Rejected candidate must start with a valid nonzero value");
            PointStoreRouter failingRouter;
            failingRouter.addStore(segment,store);
            for (auto i : indices) {
                auto route=*router.routeByIndex(i);
                if (i==rejected) route.derived=true;
                failingRouter.addRoute(route);
            }
            auto failureGraph=graph;
            for (auto pair : {std::make_pair(740104U,890001U),std::make_pair(standby,890002U)}) {
                PointStoreRoute route;route.index=pair.second;route.machineCode="TEST_ONLY";
                route.meterCode="TEST";route.pointCode=std::to_string(pair.second);route.sharedMemoryName=segment;
                failingRouter.addRoute(route);
                GraphEmsNodeConfig observer;observer.id="failure_observer_"+std::to_string(pair.second);
                observer.type="formula";observer.params={{"operation","add"},{"inputs.count","2"},
                    {"inputs.0.index",std::to_string(pair.first)},{"inputs.1.value","0"},
                    {"outputIndex",std::to_string(pair.second)}};
                failureGraph.nodes.push_back(observer);
                failureGraph.edges.push_back({reserveNode,observer.id});
            }
            GraphEmsEngine failureEngine(failureGraph,failingRouter,600000,"",
                {{"CHARGE_DISCHARGE_TEST","1"},{"PCS_ENERGY_SAVING","1"}});
            now+=10000;
            const auto result=failureEngine.runOnce(now);
            if(result.errors.size()!=1 || result.errors.front().find("gridReserve output rejected")==std::string::npos)
                throw std::runtime_error("Expected exactly the injected publication error");
            for(auto i:{740106,740107,740108}) expect(i,0,"failure-same-scan-final-zero");
            for(auto pair:{std::make_pair(890001U,4.0),std::make_pair(890002U,0.0)}) {
                const auto observed=store.getLatestByIndex(pair.first,now);
                if(!observed || observed->quality!=1 || observed->ts!=now || observed->value!=pair.second)
                    throw std::runtime_error("Protective snapshot status/standby not consumed downstream");
            }
            std::set<unsigned> zeroCommands;
            for(const auto& command:store.drainPendingWriteCommands()) {
                if(command.index>=1318 && command.index<=1320) {
                    if(command.value!=0 || command.ts!=now)
                        throw std::runtime_error("Publication failure retained nonzero PCS command");
                    zeroCommands.insert(command.index);
                }
                if((command.index==1201 || command.index==1202) && command.value!=0)
                    throw std::runtime_error("Publication failure unexpectedly started/stopped PCS");
                put(command.index,command.value);
            }
            if(zeroCommands!=std::set<unsigned>{1318,1319,1320})
                throw std::runtime_error("Publication failure skipped downstream PCS zero writers");
            for(auto i:reserveOutputs) {
                const auto shared=store.getLatestByIndex(i,now);
                if(i==rejected) {
                    // Invalidation through a read-only route is best effort.
                    // Old shared data must not leak into this graph's scan;
                    // candidate outputs are not a cross-graph/service contract.
                    if(!shared || shared->quality!=previous->quality || shared->ts!=previous->ts ||
                       shared->value!=previous->value)
                        throw std::runtime_error("Fault injection did not retain rejected shared candidate");
                } else if(!shared || shared->quality!=0 || shared->ts!=now) {
                    throw std::runtime_error("Healthy candidate was not invalidated: "+std::to_string(i));
                }
            }
            std::cout<<"PASS rejected-output-"<<rejected
                     <<" retains old shared value; four healthy candidates invalid; three same-scan zero commands\n";
        }
        // Inheriting automatic mode must pass through the original requests,
        // including zero, rather than synthesize a new charge request.
        put(23,0);
        for(int i=0;i<3;++i) put(nodeIndex("force_full_active_"+std::to_string(i),"outputIndex"),0);
        step("inherit-auto-preserves-zero-request",1,59.7,48059,0,0,0);
        for(int i=0;i<3;++i) put(nodeIndex("force_full_active_"+std::to_string(i),"outputIndex"),8);
        step("inherit-auto-preserves-original-request",1,59.7,48059,8,0,0);
        std::cout<<"PASS actual V2 loader and GraphEmsEngine control integration (memory only)\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
