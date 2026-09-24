#include "edge_gateway/config_loader.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace edge_gateway;
void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
int main() {
    const auto path = "cluster_config_" + std::to_string(getpid()) + ".json";
    struct Cleanup { std::string path; ~Cleanup() { std::remove(path.c_str()); } } cleanup{path};
    const auto load = [&](const std::string& fields) {
        { std::ofstream file(path); file << "{\"emsCluster\":{" << fields << "}}"; }
        return ConfigLoader::loadAppConfigFromFile(path).emsCluster;
    };
    try {
        auto config = load("\"controlTargetIndexes\":[1,4294967295]");
        require(config.controlTargetIndexes == std::vector<std::uint32_t>({1, 4294967295U}),
                "control scope must parse exact uint32 indexes");
        for (const auto& invalid : {"[0]", "[-1]", "[1,1]", "[1.5]", "[4294967296]", "[\"1\"]", "{}"}) {
            bool rejected = false;
            try { load(std::string("\"controlTargetIndexes\":") + invalid); } catch (const std::exception&) { rejected = true; }
            require(rejected, "malformed scope must be rejected");
        }
        require(load("\"enabled\":true,\"controlTargetIndexes\":[]").controlTargetIndexes.empty(),
                "empty configured scope remains explicit all-stop");
        for (const auto& name : {"../escape", "/name", "name/child", "", "name with space"}) {
            bool rejected = false;
            try { load(std::string("\"enabled\":true,\"virtualSharedMemoryName\":\"") + name + "\""); }
            catch (const std::exception&) { rejected = true; }
            require(rejected, "enabled authority store must have a bounded simple name");
        }
        bool rejected = false;
        try { load("\"enabled\":true,\"virtualSharedMemoryName\":\"" + std::string(64, 'a') + "\""); }
        catch (const std::exception&) { rejected = true; }
        require(rejected, "authority store name must fit fixed 64-byte record");
        std::string tooMany = "\"controlTargetIndexes\":[";
        for (int i = 1; i <= 257; ++i) tooMany += (i > 1 ? "," : "") + std::to_string(i);
        rejected = false;
        try { load(tooMany + "]"); } catch (const std::exception&) { rejected = true; }
        require(rejected, "scope must fit fixed 256-entry record");
        std::cout << "cluster_config_authority_test passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
