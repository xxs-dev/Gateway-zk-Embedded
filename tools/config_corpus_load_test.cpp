// Every device config shipped in the repository must load through ConfigLoader (parse + load-time
// validation). Guards validators against rejecting configs that work in the field.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/filesystem_compat.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: config_corpus_load_test <repo root>\n");
        return 2;
    }
    const std::string root = argv[1];
    const std::vector<std::string> directories = {
        "config/runtime/devices",
        "config/factory/runtime/devices",
        "config/examples",
        "products/ems/stationary/1.0/projects/COMM202600103/runtime/devices",
    };
    int loaded = 0;
    int failed = 0;
    for (const auto& relative : directories) {
        const auto directory = root + "/" + relative;
        for (const auto& entry : edge_gateway::filesystem::directory_iterator(directory)) {
            const auto path = entry.path().string();
            const auto name = entry.path().filename().string();
            if (name.compare(0, 7, "device_") != 0 || name.size() < 5 || name.substr(name.size() - 5) != ".json") {
                continue;
            }
            std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
            std::stringstream buffer;
            buffer << input.rdbuf();
            try {
                (void)edge_gateway::ConfigLoader::loadFromText(buffer.str());
                ++loaded;
            } catch (const std::exception& ex) {
                ++failed;
                std::fprintf(stderr, "FAILED %s: %s\n", path.c_str(), ex.what());
            }
        }
    }
    std::printf("config_corpus_load_test: %d loaded, %d failed\n", loaded, failed);
    return failed == 0 && loaded > 0 ? 0 : 1;
}
