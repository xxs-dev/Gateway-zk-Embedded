#include <fstream>
#define main candidate_startup_test_main
#include "ems_cluster_strategy_startup_test.cpp"
#undef main

int main() {
    // Provision only the fixed voter files required by the integrated core.
    // The candidate's original startup test body is included unchanged.
    const auto name = "cluster_startup_" + std::to_string(getpid());
    for (int i = 0; i < 2; ++i) {
        std::ofstream membership(name + std::to_string(i) + "-members.json");
        membership << "{\"schemaVersion\":\"1.0\",\"clusterId\":\"" << name
                   << "\",\"membershipEpoch\":1,\"assignments\":["
                   << "{\"nodeId\":\"NODE_0\",\"cabinetNo\":1},"
                   << "{\"nodeId\":\"NODE_1\",\"cabinetNo\":2}]}";
        if (!membership) return 2;
    }
    try {
        startup();
        std::cout << "provisioned candidate strategy startup probe passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
