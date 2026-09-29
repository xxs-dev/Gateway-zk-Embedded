#include <iostream>
#include <stdexcept>
#include <set>
#include <functional>
#include <cmath>
#include <unistd.h>
#include "edge_gateway/graph_ems_engine.hpp"
#include "edge_gateway/scada_project_loader.hpp"

void simulate(const edge_gateway::GraphEmsConfig& full) {
    using namespace edge_gateway;
    const std::set<std::string> selected = {
        "local_control_zero", "local_control_manual", "local_control_direction",
        "local_control_nonzero", "local_control_permit", "local_control_phase",
        "cycle_basic_safety_gate", "cycle_charge_command_allowed", "cycle_discharge_command_allowed",
        "cycle_charge_direction_allowed", "cycle_discharge_direction_allowed", "cycle_direction_allowed",
        "cycle_operable_state_gate", "cycle_runtime_safety_gate", "cycle_safe_p0", "cycle_safe_p1",
        "cycle_safe_p2", "cycle_safe_q0", "cycle_safe_q1", "cycle_safe_q2"
    };
    auto graph = full;
    graph.nodes.clear(); graph.edges.clear();
    for (const auto& node : full.nodes) if (selected.count(node.id)) graph.nodes.push_back(node);
    for (const auto& source : selected) {
        std::set<std::string> visited;
        std::function<void(const std::string&)> visit = [&](const std::string& current) {
            if (!visited.insert(current).second) return;
            for (const auto& edge : full.edges) if (edge.from==current) {
                if (selected.count(edge.to)) graph.edges.push_back({source,edge.to});
                else visit(edge.to);
            }
        };
        visit(source);
    }
    if (graph.nodes.size() != selected.size()) throw std::runtime_error("Missing safety node");
    const auto name = "comm103_control_regression_" + std::to_string(getpid());
    struct Cleanup { std::string name; ~Cleanup() { MemoryPointStore::cleanupOrphanedSegment(name); } } cleanup{name};
    MemoryPointStore store(name);
    PointStoreRouter router; router.addStore(name, store);
    auto route = [&](std::uint32_t index) {
        if (router.routeByIndex(index)) return;
        PointStoreRoute r; r.index=index; r.machineCode="TEST_ONLY"; r.meterCode="TEST";
        r.pointCode=std::to_string(index); r.sharedMemoryName=name; router.addRoute(r);
    };
    for (const auto& node : graph.nodes) for (const auto& p : node.params) {
        if (p.first.find("Index") == std::string::npos && p.first.find("index") == std::string::npos) continue;
        std::size_t consumed=0;
        try { auto i=std::stoul(p.second,&consumed); if (consumed==p.second.size() && i) route(i); } catch (...) {}
    }
    std::int64_t now=1000000;
    auto put = [&](std::uint32_t index, double value) {
        route(index); PointValue p; p.index=index; p.value=value; p.quality=1; p.ts=now; p.expireAt=now+600000;
        if (!router.putLatestByIndex(p).accepted) throw std::runtime_error("Seed rejected");
    };
    for (auto index : {1399,3999,1214,1556,1557,1210,1211}) put(index,1);
    for (auto index : {1212,1215,1462,8199,1209}) put(index,0);
    put(20128,4); put(400017,1); put(1569,50);
    for (auto index : {400627,400628,400629,400630,400631,400632}) put(index,10);
    GraphEmsEngine engine(graph,router,600000,"",{{"CHARGE_DISCHARGE_TEST","1"}});
    struct Case { const char* name; int mode; double power,soc,allowCharge,allowDischarge,expected; };
    const Case cases[] = {
        {"automatic",1,0,50,1,1,10}, {"pause",0,30,50,1,1,0},
        {"manual-charge",2,30,50,1,1,10}, {"manual-discharge",2,-30,50,1,1,10},
        {"manual-zero",2,0,50,1,1,0}, {"charge-inhibited",2,30,50,0,1,0},
        {"discharge-inhibited",2,-30,50,1,0,0}, {"upper-soc",2,30,95,1,1,0},
        {"lower-soc",2,-30,20,1,1,0}, {"invalid-mode",9,30,50,1,1,0},
        {"resume-auto",1,30,50,1,1,10}
    };
    for (const auto& c : cases) {
        now+=10000; put(730100,c.mode); put(730101,c.power); put(1569,c.soc);
        put(1556,c.allowCharge); put(1557,c.allowDischarge);
        const auto result=engine.runOnce(now,0);
        if (!result.errors.empty()) throw std::runtime_error(result.errors.front());
        if (result.deviceWrites) throw std::runtime_error("Test must never submit a device command");
        for (std::uint32_t index=700345;index<=700350;++index) {
            const auto value=router.getLatestByIndex(index,now);
            if (!value || value->quality!=1 || std::fabs(value->value-c.expected)>0.001)
                throw std::runtime_error(std::string(c.name)+" incorrect safe output "+std::to_string(index)+
                    " value="+(value?std::to_string(value->value):"missing"));
        }
        std::cout << "PASS: " << c.name << '\n';
    }
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        const auto graph = edge_gateway::GraphEmsConfig::loadFromFile(argv[1]);
        edge_gateway::PointStoreRouter router;
        edge_gateway::GraphEmsEngine engine(graph, router);
        const auto project = edge_gateway::ScadaProjectLoader::loadFromDirectory(argv[2]);
        edge_gateway::ScadaProjectLoader::validate(project);
        std::cout << "PASS: production graph loader, dependency ordering, SCADA validation\n";
        simulate(graph);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
