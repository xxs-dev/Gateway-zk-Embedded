#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string uniqueName(const std::string& prefix) {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return prefix + "_" + std::to_string(now);
}

std::string joinPath(const std::string& directory, const std::string& fileName) {
    return directory + "/" + fileName;
}

std::string temporaryDirectory(const std::string& prefix) {
    return joinPath("/tmp", uniqueName(prefix));
}

void createDirectory(const std::string& directory) {
    if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
        throw std::runtime_error(
            "failed to create test directory " + directory + ": " + std::strerror(errno)
        );
    }
}

void removeRetentionDirectory(const std::string& directory, std::uint32_t index) {
    const auto fileName = std::to_string(index) + ".value";
    std::remove(joinPath(directory, fileName).c_str());
    std::remove(joinPath(directory, fileName + ".tmp").c_str());
    ::rmdir(directory.c_str());
}

edge_gateway::PointStoreRoute makeVirtualRoute(
    const std::string& sharedMemoryName,
    std::uint32_t index,
    double initialValue,
    bool retain
) {
    edge_gateway::PointStoreRoute route;
    route.index = index;
    route.sourceIndex = index;
    route.machineCode = "GW_RETAIN_TEST";
    route.meterCode = "EMS_CORE";
    route.pointCode = "ems_retained_status";
    route.category = "status";
    route.protocolType = "ems_virtual";
    route.sharedMemoryName = sharedMemoryName;
    route.initialValue = initialValue;
    route.retain = retain;
    return route;
}

edge_gateway::MemoryStoreConfig makeStoreConfig(const std::string& sharedMemoryName) {
    edge_gateway::MemoryStoreConfig config;
    config.sharedMemoryName = sharedMemoryName;
    config.maxLatestPoints = 16;
    return config;
}

void verifyInitialValueAndRestartRestore() {
    using namespace edge_gateway;

    const auto storeName = uniqueName("gateway_router_retention_test");
    const auto directory = temporaryDirectory("gateway_ems_retention");
    const auto route = makeVirtualRoute(storeName, 880001, 0.0, true);
    MemoryPointStore::cleanupOrphanedSegment(storeName);

    {
        MemoryPointStore store(makeStoreConfig(storeName));
        PointStoreRouter router;
        router.setEmsVirtualParameterDirectory(directory);
        router.addStore(storeName, store);
        router.addRoute(route);

        const auto initial = router.getLatestByIndex(route.index, 1770000000000LL);
        require(static_cast<bool>(initial), "retained point initial value missing");
        require(initial->value == 0.0, "retained point initial value mismatch");
        require(initial->quality == 1 && !initial->stale, "retained point initial quality mismatch");
        require(
            static_cast<bool>(std::ifstream(joinPath(directory, "880001.value"))),
            "initial retained value was not persisted"
        );

        PointValue update;
        update.index = route.index;
        update.value = 1.0;
        update.quality = 1;
        update.ts = 1770000001000LL;
        update.expireAt = update.ts + 1000;
        require(router.putLatestByIndex(update).accepted, "retained point update failed");

        const auto updated = router.getLatestByIndex(route.index, 1770000010000LL);
        require(static_cast<bool>(updated), "retained point became unavailable after TTL");
        require(updated->value == 1.0 && !updated->stale, "retained point update mismatch");
    }

    MemoryPointStore::cleanupOrphanedSegment(storeName);
    {
        MemoryPointStore store(makeStoreConfig(storeName));
        PointStoreRouter router;
        router.setEmsVirtualParameterDirectory(directory);
        router.addStore(storeName, store);
        router.addRoute(route);

        const auto restored = router.getLatestByIndex(route.index, 1770000020000LL);
        require(static_cast<bool>(restored), "retained point was not restored after restart");
        require(restored->value == 1.0, "retained point restored the configured default instead of saved value");
    }

    MemoryPointStore::cleanupOrphanedSegment(storeName);
    removeRetentionDirectory(directory, route.index);
}

void verifyCorruptStateFallsBackToInitialValue() {
    using namespace edge_gateway;

    const auto storeName = uniqueName("gateway_router_corrupt_retention_test");
    const auto directory = temporaryDirectory("gateway_ems_corrupt_retention");
    createDirectory(directory);
    {
        std::ofstream output(joinPath(directory, "880002.value"), std::ios::binary | std::ios::trunc);
        output << "not-a-number\n";
    }

    const auto route = makeVirtualRoute(storeName, 880002, 0.0, true);
    MemoryPointStore::cleanupOrphanedSegment(storeName);
    {
        MemoryPointStore store(makeStoreConfig(storeName));
        PointStoreRouter router;
        router.setEmsVirtualParameterDirectory(directory);
        router.addStore(storeName, store);
        router.addRoute(route);

        const auto restored = router.getLatestByIndex(route.index, 1770000030000LL);
        require(static_cast<bool>(restored), "corrupt retained value did not fall back to initial value");
        require(restored->value == 0.0, "corrupt retained value fallback mismatch");
    }

    MemoryPointStore::cleanupOrphanedSegment(storeName);
    std::ifstream repaired(joinPath(directory, "880002.value"));
    double repairedValue = -1.0;
    repaired >> repairedValue;
    require(repairedValue == 0.0, "corrupt retained value file was not repaired atomically");
    removeRetentionDirectory(directory, route.index);
}

void verifyPhysicalTelemetryIsNotInitialized() {
    using namespace edge_gateway;

    const auto storeName = uniqueName("gateway_router_physical_initial_test");
    MemoryPointStore::cleanupOrphanedSegment(storeName);
    {
        MemoryPointStore store(makeStoreConfig(storeName));
        PointStoreRouter router;
        router.addStore(storeName, store);
        auto route = makeVirtualRoute(storeName, 880003, 7.0, true);
        route.protocolType = "modbus_rtu";
        route.category = "telemetry";
        router.addRoute(route);
        require(
            !router.getLatestByIndex(route.index, 1770000040000LL),
            "physical telemetry must not restore configured or stale values"
        );
    }
    MemoryPointStore::cleanupOrphanedSegment(storeName);
}

void verifyFactoryConfigCarriesRuntimeDefaults() {
    const auto config = edge_gateway::ConfigLoader::loadFromFile(
        "config/factory/runtime/devices/device_ems_virtual.json"
    );
    require(config.protocol.type == "ems_virtual", "factory EMS virtual protocol mismatch");
    require(!config.meters.empty(), "factory EMS virtual meter missing");

    const auto& points = config.meters.front().points;
    const auto pointByIndex = [&points](std::uint32_t index) -> const edge_gateway::PointDefinition& {
        const auto it = std::find_if(points.begin(), points.end(), [index](const auto& point) {
            return point.index == index;
        });
        if (it == points.end()) {
            throw std::runtime_error("factory EMS virtual point missing: " + std::to_string(index));
        }
        return *it;
    };

    const auto& runFeedback = pointByIndex(8);
    require(runFeedback.initialValue && *runFeedback.initialValue == 0.0, "run feedback initial value mismatch");
    require(runFeedback.retain, "run feedback must be retained");

    const auto& socUpper = pointByIndex(161);
    require(socUpper.initialValue && *socUpper.initialValue == 95.0, "SOC upper initial value mismatch");
    require(socUpper.retain, "SOC upper setting must be retained");

    const auto& apparentPowerLimit = pointByIndex(151);
    require(
        apparentPowerLimit.initialValue && *apparentPowerLimit.initialValue == 0.0,
        "PCS apparent power limit initial value mismatch"
    );
    require(apparentPowerLimit.retain, "PCS apparent power limit must be retained");

    const auto& meterAverageWindow = pointByIndex(156);
    require(
        meterAverageWindow.initialValue && *meterAverageWindow.initialValue == 10.0,
        "meter average window initial value mismatch"
    );
    require(meterAverageWindow.retain, "meter average window must be retained");

    const auto& calculatedFeedback = pointByIndex(309);
    require(
        calculatedFeedback.initialValue && *calculatedFeedback.initialValue == 0.0,
        "calculated feedback initial value mismatch"
    );
    require(!calculatedFeedback.retain, "calculated telemetry should use Graph state instead of per-point retention");

    const auto& schedulePower0 = pointByIndex(400);
    require(schedulePower0.write.enable, "EMS 1.0 schedule power must remain writable");
    require(schedulePower0.retain, "EMS 1.0 schedule power must be retained");
    require(
        schedulePower0.initialValue && *schedulePower0.initialValue == 0.0,
        "EMS 1.0 schedule power initial value mismatch"
    );

    const auto& scheduleSoc0 = pointByIndex(424);
    require(scheduleSoc0.write.enable, "EMS 1.0 schedule SOC must remain writable");
    require(scheduleSoc0.retain, "EMS 1.0 schedule SOC must be retained");
    require(scheduleSoc0.write.minValue && *scheduleSoc0.write.minValue == 0.0, "schedule SOC min mismatch");
    require(scheduleSoc0.write.maxValue && *scheduleSoc0.write.maxValue == 100.0, "schedule SOC max mismatch");
}

}  // namespace

int main() {
    try {
        verifyInitialValueAndRestartRestore();
        verifyCorruptStateFallsBackToInitialValue();
        verifyPhysicalTelemetryIsNotInitialized();
        verifyFactoryConfigCarriesRuntimeDefaults();
        std::cout << "point_store_router_retention_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "point_store_router_retention_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
