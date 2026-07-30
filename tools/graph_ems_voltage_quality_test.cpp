#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "edge_gateway/graph_ems_engine.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireNear(double actual, double expected, double tolerance, const std::string& message) {
    if (std::fabs(actual - expected) > tolerance) {
        throw std::runtime_error(message + " actual=" + std::to_string(actual) +
            " expected=" + std::to_string(expected));
    }
}

std::string uniqueSuffix() {
    return std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
}

std::string graphPath() {
    const char* candidates[] = {
        "config/examples/voltage-quality-daily-graph.json",
        "../config/examples/voltage-quality-daily-graph.json",
        "../../config/examples/voltage-quality-daily-graph.json"
    };
    for (const auto* candidate : candidates) {
        std::ifstream input(candidate, std::ios::in | std::ios::binary);
        if (input.is_open()) {
            return candidate;
        }
    }
    throw std::runtime_error("cannot locate voltage-quality-daily-graph.json");
}

std::int64_t localEpochMs(int year, int month, int day, int hour, int minute) {
    std::tm localTime{};
    localTime.tm_year = year - 1900;
    localTime.tm_mon = month - 1;
    localTime.tm_mday = day;
    localTime.tm_hour = hour;
    localTime.tm_min = minute;
    localTime.tm_isdst = -1;
    const auto seconds = std::mktime(&localTime);
    require(seconds >= 0, "failed to build local test timestamp");
    return static_cast<std::int64_t>(seconds) * 1000LL;
}

void addRoute(
    edge_gateway::PointStoreRouter& router,
    std::uint32_t index,
    const std::string& sharedMemoryName
) {
    edge_gateway::PointStoreRoute route;
    route.index = index;
    route.machineCode = "VOLTAGE_QUALITY_TEST";
    route.meterCode = "EMS_CORE";
    route.pointCode = "VQ_" + std::to_string(index);
    route.interfaceCode = "compute";
    route.interfaceType = "computed";
    route.sharedMemoryName = sharedMemoryName;
    route.isStore = true;
    route.persistIntervalSec = 60;
    router.addRoute(route);
}

void put(
    edge_gateway::PointStoreRouter& router,
    std::uint32_t index,
    double value,
    int quality,
    std::int64_t nowMs
) {
    edge_gateway::PointValue point;
    point.index = index;
    point.value = value;
    point.quality = quality;
    point.ts = nowMs;
    point.expireAt = nowMs + 600000;
    const auto routed = router.putLatestByIndex(point);
    require(routed.accepted, "failed to seed point " + std::to_string(index) + ": " + routed.message);
}

void putSiteVoltages(
    edge_gateway::PointStoreRouter& router,
    double phaseVoltage,
    double lineVoltage,
    int quality,
    std::int64_t nowMs
) {
    const std::uint32_t phaseIndexes[] = {1030, 1031, 1032};
    const std::uint32_t lineIndexes[] = {1053, 1054, 1055};
    for (const auto index : phaseIndexes) {
        put(router, index, phaseVoltage, quality, nowMs);
    }
    for (const auto index : lineIndexes) {
        put(router, index, lineVoltage, quality, nowMs);
    }
}

double latest(edge_gateway::PointStoreRouter& router, std::uint32_t index, std::int64_t nowMs) {
    const auto value = router.getLatestByIndex(index, nowMs);
    require(static_cast<bool>(value), "missing output index " + std::to_string(index));
    require(value->quality == 1 && !value->stale, "invalid output index " + std::to_string(index));
    return value->value;
}

void requireNoErrors(const edge_gateway::GraphEmsRunResult& result, const std::string& context) {
    if (result.errors.empty()) {
        return;
    }
    std::string message = context;
    for (const auto& error : result.errors) {
        message += "; " + error;
    }
    throw std::runtime_error(message);
}

}  // namespace

int main() {
    const auto suffix = uniqueSuffix();
    const auto stateFile = "/tmp/graph_ems_voltage_quality_state_test_" + suffix + ".json";
    const auto sharedMemoryName = "gateway_voltage_quality_test_" + suffix;
    try {
        edge_gateway::MemoryStoreConfig memoryConfig;
        memoryConfig.sharedMemoryName = sharedMemoryName;
        memoryConfig.maxLatestPoints = 128;
        memoryConfig.maxPendingWrites = 16;
        memoryConfig.maxPersistentSamples = 128;
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
        {
            edge_gateway::MemoryPointStore store(memoryConfig);
            edge_gateway::PointStoreRouter router;
            router.addStore(sharedMemoryName, store);
            const std::vector<std::uint32_t> inputIndexes = {
                1030, 1031, 1032, 1053, 1054, 1055,
                1130, 1131, 1132, 1153, 1154, 1155
            };
            for (const auto index : inputIndexes) {
                addRoute(router, index, sharedMemoryName);
            }
            for (std::uint32_t index = 710000; index <= 710021; ++index) {
                addRoute(router, index, sharedMemoryName);
            }
            for (std::uint32_t index = 710100; index <= 710121; ++index) {
                addRoute(router, index, sharedMemoryName);
            }

            const auto base = 1721001600000LL;
            {
                edge_gateway::GraphEmsEngine engine(
                    edge_gateway::GraphEmsConfig::loadFromFile(graphPath()),
                    router,
                    600000,
                    stateFile,
                    {{"stateSaveIntervalMs", "0"}}
                );
                for (int sample = 0; sample < 150; ++sample) {
                    const auto now = base + sample * 200LL;
                    putSiteVoltages(router, 220.0, 380.0, 1, now);
                    engine.runOnce(now);
                }
            }
            {
                edge_gateway::GraphEmsEngine engine(
                    edge_gateway::GraphEmsConfig::loadFromFile(graphPath()),
                    router,
                    600000,
                    stateFile,
                    {{"stateSaveIntervalMs", "0"}}
                );
                for (int sample = 150; sample < 300; ++sample) {
                    const auto now = base + sample * 200LL;
                    putSiteVoltages(router, 220.0, 380.0, 1, now);
                    engine.runOnce(now);
                }

                putSiteVoltages(router, 250.0, 380.0, 1, base + 60000);
                const auto firstMinuteResult = engine.runOnce(base + 60000);
                requireNoErrors(firstMinuteResult, "first minute run failed");
                requireNear(latest(router, 710006, base + 60000), 1.0, 0.0001, "qualified minute status mismatch");
                requireNear(latest(router, 710000, base + 60000), 220.0, 0.0001, "220V minute average mismatch");
                requireNear(latest(router, 710003, base + 60000), 380.0, 0.0001, "380V minute average mismatch");
                requireNear(latest(router, 710008, base + 60000), 20240715.0, 0.0001, "day key mismatch");
                requireNear(latest(router, 710007, base + 60000), 100.0, 0.0001, "minute coverage mismatch");
                requireNear(latest(router, 710009, base + 60000), 1.0, 0.0001, "day monitored minutes mismatch");
                requireNear(latest(router, 710012, base + 60000), 100.0, 0.0001, "day qualified rate mismatch");
                requireNear(latest(router, 710014, base + 60000), 1.0, 0.0001, "95 percent pass status mismatch");

                for (int sample = 1; sample < 300; ++sample) {
                    const auto now = base + 60000 + sample * 200LL;
                    putSiteVoltages(router, 250.0, 380.0, 1, now);
                    engine.runOnce(now);
                }
                putSiteVoltages(router, 0.0, 0.0, 0, base + 120000);
                engine.runOnce(base + 120000);
                requireNear(latest(router, 710006, base + 120000), 2.0, 0.0001, "overlimit minute status mismatch");
                requireNear(latest(router, 710009, base + 120000), 2.0, 0.0001, "day monitored minutes after overlimit mismatch");
                requireNear(latest(router, 710010, base + 120000), 1.0, 0.0001, "day overlimit minutes mismatch");
                requireNear(latest(router, 710012, base + 120000), 50.0, 0.0001, "day qualified rate after overlimit mismatch");
                requireNear(latest(router, 710014, base + 120000), 0.0, 0.0001, "day pass status should fail below 95 percent");

                for (int sample = 1; sample < 300; ++sample) {
                    const auto now = base + 120000 + sample * 200LL;
                    putSiteVoltages(router, 0.0, 0.0, 0, now);
                    engine.runOnce(now);
                }
                engine.runOnce(base + 180000);
                requireNear(latest(router, 710006, base + 180000), 3.0, 0.0001, "invalid minute status mismatch");
                requireNear(latest(router, 710011, base + 180000), 1.0, 0.0001, "day invalid minutes mismatch");
                requireNear(latest(router, 710012, base + 180000), 50.0, 0.0001, "invalid minute must not dilute qualified rate");
                requireNear(latest(router, 710013, base + 180000), 200.0 / 3.0, 0.0001, "day availability mismatch");

                for (int sample = 1; sample < 300; ++sample) {
                    const auto now = base + 180000 + sample * 200LL;
                    if (sample % 5 == 0) {
                        putSiteVoltages(router, 220.0, 380.0, 1, now);
                    }
                    engine.runOnce(now);
                }
                engine.runOnce(base + 240000);
                requireNear(latest(router, 710006, base + 240000), 3.0, 0.0001, "reused source values must not count as 200ms samples");
                requireNear(latest(router, 710007, base + 240000), 59.0 / 3.0, 0.0001, "fresh source sample coverage mismatch");
                requireNear(latest(router, 710011, base + 240000), 2.0, 0.0001, "low source update rate must add an invalid minute");
            }

            std::remove(stateFile.c_str());
            const auto dayEnd = localEpochMs(2024, 7, 16, 23, 59);
            edge_gateway::GraphEmsEngine rolloverEngine(
                edge_gateway::GraphEmsConfig::loadFromFile(graphPath()),
                router,
                600000,
                stateFile,
                {{"stateSaveIntervalMs", "0"}}
            );
            for (int sample = 0; sample < 300; ++sample) {
                const auto now = dayEnd + sample * 200LL;
                putSiteVoltages(router, 220.0, 380.0, 1, now);
                rolloverEngine.runOnce(now);
            }
            putSiteVoltages(router, 220.0, 380.0, 1, dayEnd + 60000);
            const auto rolloverResult = rolloverEngine.runOnce(dayEnd + 60000);
            requireNoErrors(rolloverResult, "day rollover run failed");
            requireNear(latest(router, 710008, dayEnd + 60000), 20240717.0, 0.0001, "current day key did not roll over");
            requireNear(latest(router, 710009, dayEnd + 60000), 0.0, 0.0001, "new day must start with zero monitored minutes");
            requireNear(latest(router, 710015, dayEnd + 60000), 20240716.0, 0.0001, "completed day key mismatch");
            requireNear(latest(router, 710016, dayEnd + 60000), 1.0, 0.0001, "completed day monitored minutes mismatch");
            requireNear(latest(router, 710019, dayEnd + 60000), 100.0, 0.0001, "completed day qualified rate mismatch");
            requireNear(latest(router, 710021, dayEnd + 60000), 1.0, 0.0001, "completed day pass status mismatch");
        }
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
        std::remove(stateFile.c_str());
        std::remove((stateFile + ".tmp").c_str());
        std::cout << "graph_ems_voltage_quality_test passed\n";
        return 0;
    } catch (const std::exception& ex) {
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
        std::remove(stateFile.c_str());
        std::remove((stateFile + ".tmp").c_str());
        std::cerr << "graph_ems_voltage_quality_test failed: " << ex.what() << "\n";
        return 1;
    }
}
