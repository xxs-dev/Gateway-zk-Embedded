// Every device config shipped in the repository must load through ConfigLoader (parse + load-time
// validation). Guards validators against rejecting configs that work in the field.
// App configs (apps/*.json and example app configs) must load as well: an unusable MQTT
// transport (plaintext, missing broker/CA) is refused at connect time, never at load time.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/filesystem_compat.hpp"
#include "edge_gateway/json_value.hpp"

namespace {

bool endsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool isAppConfig(const std::string& text) {
    try {
        const auto root = edge_gateway::json::JsonParser(text).parse();
        if (!root.isObject()) {
            return false;
        }
        for (const char* key : {"mqtt", "mqttDriver", "mqttForward", "systemMonitor", "cameraService"}) {
            if (root.find(key) != nullptr) {
                return true;
            }
        }
    } catch (...) {
    }
    return false;
}

}  // namespace

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
    int appsLoaded = 0;
    const std::vector<std::string> appDirectories = {
        "config/runtime/apps",
        "config/factory/runtime/apps",
        "config/examples",
    };
    for (const auto& relative : appDirectories) {
        const bool examples = relative == "config/examples";
        for (const auto& entry : edge_gateway::filesystem::directory_iterator(root + "/" + relative)) {
            const auto path = entry.path().string();
            if (!endsWith(path, ".json")) {
                continue;
            }
            std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
            std::stringstream buffer;
            buffer << input.rdbuf();
            if (examples && !isAppConfig(buffer.str())) {
                continue;
            }
            try {
                (void)edge_gateway::ConfigLoader::loadAppConfigFromFile(path);
                ++appsLoaded;
            } catch (const std::exception& ex) {
                ++failed;
                std::fprintf(stderr, "FAILED %s: %s\n", path.c_str(), ex.what());
            }
        }
    }
    std::printf("config_corpus_load_test: %d device configs loaded, %d app configs loaded, %d failed\n",
        loaded, appsLoaded, failed);
    return failed == 0 && loaded > 0 && appsLoaded > 0 ? 0 : 1;
}
