#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#else
#include <direct.h>
#endif
#include "edge_gateway/compute_engine_service.hpp"
#include "edge_gateway/compute_graph_observation.hpp"
#include "../src/ota_query_sha256.hpp"
using namespace edge_gateway;
void require(bool v, const char* message) { if (!v) throw std::runtime_error(message); }
void write(const std::string& p, const std::string& s) { std::ofstream out(p, std::ios::binary); out << s; if (!out) throw std::runtime_error("write failed"); }
std::int64_t now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
int main(int argc, char** argv) {
    try {
        require(argc == 2, "use an existing isolated temporary directory");
        const std::string dir = argv[1], graph = dir + "/graph.json", health = dir + "/health.json";
        const std::string initial = R"({"schemaVersion":"2.0.0","graphCode":"observation","compile":{"maxNodes":1,"maxEdges":1,"virtualIndexStart":700000,"virtualIndexEnd":700100},"nodes":[],"links":[]})";
        const auto updated = initial + "\n";
        write(graph, initial);
        require(GraphEmsConfig::loadFromFile(graph).sourceSha256 == ota_query_detail::sha256Hex(initial), "loaded hash differs from parsed bytes");
#ifdef _WIN32
        require(_chdir(dir.c_str()) == 0, "private test working directory");
#else
        require(chdir(dir.c_str()) == 0, "private test working directory");
#endif
        const std::string relative = "graph.json";
        const auto relativeConfig = GraphEmsConfig::loadFromFile(relative);
        require(relativeConfig.sourcePath != relative && relativeConfig.sourceSha256 == ota_query_detail::sha256Hex(initial), "relative load location not captured");
        PointStoreRouter router;
        ComputeEngineConfig config;
        config.enabled = true; config.healthFile = health; config.scanIntervalMs = 5; config.healthPublishIntervalMs = 100;
        ComputeRuleConfig rule;
        rule.ruleCode = "rule\"\\name"; rule.enabled = true; rule.script.type = "graphEms"; rule.script.graphFile = relative;
        rule.script.graphStateFile = dir + "/state.json";
        config.rules.push_back(rule);
        std::string oldObservation;
        {
            ComputeEngineService service(config, router);
            // Replacement after constructor must not change the loaded fingerprint.
            write(graph, updated);
            service.start();
            for (int i = 0; i < 50; ++i) {
                try { oldObservation = observeComputeGraphs(health, now()); break; } catch (...) {}
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            service.stop();
        }
        require(oldObservation.find(ota_query_detail::sha256Hex(initial)) != std::string::npos, "old running graph was relabelled after disk replacement");
        require(oldObservation.find(ota_query_detail::sha256Hex(updated)) == std::string::npos, "disk bytes confused with loaded bytes");
#ifdef __linux__
        require(oldObservation.find("\"processPresent\":true") != std::string::npos, "live Linux process identity not recognised");
#endif
        std::remove(health.c_str());
        std::string newObservation;
        {
            ComputeEngineService service(config, router);
            service.start();
            for (int i = 0; i < 50; ++i) {
                try { newObservation = observeComputeGraphs(health, now()); break; } catch (...) {}
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            service.stop();
        }
        require(newObservation.find(ota_query_detail::sha256Hex(updated)) != std::string::npos, "service reconstruction did not load new bytes");
        const auto stale = observeComputeGraphs(health, now() + 31000);
        require(json::JsonParser(stale).parse().find("ageMs")->asNumber() >= 31000, "stale file lost its age");
        std::ifstream savedHealth(health);
        auto bytes = std::string(std::istreambuf_iterator<char>(savedHealth), std::istreambuf_iterator<char>());
        savedHealth.close();
        const auto identity = computeProcessIdentity(
#ifdef _WIN32
            0
#else
            getpid()
#endif
        );
        if (!identity.empty()) {
            const auto pos = bytes.find(identity);
            require(pos != std::string::npos, "process marker missing");
            bytes.replace(pos, identity.size(), identity + "0");
            write(health, bytes);
            require(observeComputeGraphs(health, now()).find("\"processPresent\":false") != std::string::npos, "reused PID accepted");
        }
        write(health, "{\"schemaVersion\":\"1.1\",\"healthy\":true}");
        bool rejected = false;
        try { observeComputeGraphs(health, now()); } catch (...) { rejected = true; }
        require(rejected, "legacy health was treated as loaded graph proof");
        std::remove(graph.c_str()); std::remove(health.c_str()); std::remove(rule.script.graphStateFile.c_str());
        std::cout << "{\"passed\":true,\"replacementWhileLoaded\":true,\"reloadChangesDigest\":true,\"staleAndReusedPidChecked\":true,\"physicalWrites\":false}\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
