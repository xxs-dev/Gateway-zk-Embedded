// Test-only visibility allows observation of ACK timestamps without product edits.
#include <bits/stdc++.h>
#define private public
#define main existing_cluster_suite_main
#include "../../../tools/ems_cluster_test.cpp"
#undef main
#undef private

void observe(Simulation& sim, const std::string& scenario, std::int64_t start) {
    std::cout << "{\"scenario\":\"" << scenario << "\",\"elapsedMs\":"
              << sim.now() - start << ",\"nowMs\":" << sim.now() << ",\"nodes\":[";
    for (int i = 0; i < 3; ++i) {
        if (i) std::cout << ',';
        const auto& node = *sim.at(i).node;
        const auto status = node.status(sim.now());
        const auto dispatch = node.activeDispatch(sim.now());
        std::cout << "{\"id\":\"" << node.nodeId() << "\",\"role\":\""
                  << edge_gateway::EmsClusterNode::roleName(status.role)
                  << "\",\"term\":" << status.term
                  << ",\"quorumValid\":" << (status.quorumValid ? "true" : "false")
                  << ",\"controlActive\":" << (status.controlActive ? "true" : "false")
                  << ",\"dispatchValid\":" << (dispatch.valid ? "true" : "false")
                  << ",\"dispatchTerm\":" << dispatch.term
                  << ",\"dispatchSequence\":" << dispatch.sequence
                  << ",\"paKw\":" << dispatch.accepted.paKw
                  << ",\"lastQuorumMs\":" << node.lastQuorumMs_
                  << ",\"lastLeaderSeenMs\":" << node.lastLeaderSeenMs_
                  << ",\"lastAckMs\":{";
        bool first = true;
        for (const auto& member : node.members_) {
            if (!first) std::cout << ',';
            first = false;
            std::cout << '"' << member.first << "\":" << member.second.lastAckMs;
        }
        std::cout << "}}";
    }
    std::cout << "]}\n";
}

void runScenario(const std::string& name, bool partitioned) {
    Simulation sim(name, 3, {}, 3, true);
    for (int i = 0; i < 3; ++i) {
        sim.at(i).stationTarget = {18, 18, 18, 0, 0, 0};
        sim.at(i).stationTargetValid = true;
    }
    sim.run(5000);
    require(sim.leaderCount() == 1 && sim.leaderIndex() == 0,
            "fixed low-load initial leader is required");
    const auto start = sim.now();
    if (partitioned) sim.partition({0}, {1, 2});
    observe(sim, name, start);
    for (int tick = 0; tick < 100; ++tick) {
        sim.run(25);
        observe(sim, name, start);
    }
}

int main() {
    try {
        runScenario("partition-repeat-1", true);
        runScenario("partition-repeat-2", true);
        runScenario("connected-control", false);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
