#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

edge_gateway::PointDefinition makePoint(std::uint32_t index, const std::string& pointCode) {
    edge_gateway::PointDefinition point;
    point.index = index;
    point.pointCode = pointCode;
    point.enabled = true;
    point.fullUpload = true;
    point.read.enable = true;
    point.read.dataType = "uint16";
    return point;
}

edge_gateway::PointValue makeValue(
    std::uint32_t index,
    double value,
    std::int64_t ts = 1770000000000LL
) {
    edge_gateway::PointValue pointValue;
    pointValue.index = index;
    pointValue.value = value;
    pointValue.quality = 1;
    pointValue.ts = ts;
    pointValue.expireAt = ts + 600000;
    return pointValue;
}

edge_gateway::DeviceConfig makeDeviceConfig(const std::string& sharedMemoryName) {
    using namespace edge_gateway;

    DeviceConfig config;
    config.machineCode = "GW_TEST";
    config.memoryStore.sharedMemoryName = sharedMemoryName;
    config.protocol.type = "modbus_rtu";

    LogicalDeviceConfig meter;
    meter.meterCode = "PCS_1";

    auto state = makePoint(1001, "PCS_VENDOR_RUN_STATE");
    state.normalize.enabled = true;
    state.normalize.type = "enum";
    state.normalize.targetIndex = 910001;
    state.normalize.targetPointCode = "PCS_RUN_STATE_STD";
    state.normalize.targetSemanticRole = "pcs.runState";
    state.normalize.targetName = "run state";
    state.normalize.unknownValue = 255.0;
    state.normalize.mappings.push_back(ValueNormalizeMapping{"1", "stop", 0.0, "stopped"});
    state.normalize.mappings.push_back(ValueNormalizeMapping{"2", "standby", 1.0, "standby"});
    state.normalize.mappings.push_back(ValueNormalizeMapping{"3", "run", 2.0, "running"});
    state.normalize.faultRules.push_back(ValueNormalizeFaultRule{"PCS_FAULT", "1", 3.0, "fault"});

    meter.points.push_back(state);
    meter.points.push_back(makePoint(1002, "PCS_FAULT"));
    config.meters.push_back(meter);
    return config;
}

edge_gateway::PointStoreRouter makeRouter(
    const edge_gateway::DeviceConfig& config,
    edge_gateway::MemoryPointStore& store,
    const std::string& sharedMemoryName
) {
    edge_gateway::PointStoreRouter router;
    router.addStore(sharedMemoryName, store);
    router.addRoutesFromDeviceConfigs({config}, sharedMemoryName);
    return router;
}

void seed(
    edge_gateway::PointStoreRouter& router,
    double state,
    double fault,
    std::int64_t ts = 1770000000000LL
) {
    require(router.putLatestByIndex(makeValue(1001, state, ts)).accepted, "state seed failed");
    require(router.putLatestByIndex(makeValue(1002, fault, ts)).accepted, "fault seed failed");
}

void verifyDerivedRouteAndMapping() {
    const std::string sharedMemoryName = "gateway_router_normalize_test";
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);

    edge_gateway::MemoryStoreConfig storeConfig;
    storeConfig.sharedMemoryName = sharedMemoryName;
    storeConfig.maxLatestPoints = 64;
    edge_gateway::MemoryPointStore store(storeConfig);
    const auto config = makeDeviceConfig(sharedMemoryName);
    auto router = makeRouter(config, store, sharedMemoryName);

    const auto route = router.routeByIndex(910001);
    require(static_cast<bool>(route), "derived route should use configured targetIndex");
    require(route->derived, "derived route should be marked derived");
    require(route->sourceIndex == 1001, "derived source index mismatch");
    require(route->pointCode == "PCS_RUN_STATE_STD", "derived pointCode mismatch");
    require(route->fullUpload, "derived full upload should inherit source");
    require(!route->writable, "derived route should be read-only");

    const auto indexes = router.allIndexes();
    require(
        std::find(indexes.begin(), indexes.end(), 910001) != indexes.end(),
        "all indexes should include derived index"
    );

    seed(router, 2.0, 0.0);
    auto value = router.getLatestByIndex(910001, 1770000000100LL);
    require(static_cast<bool>(value), "derived value should exist");
    require(value->index == 910001, "derived value index mismatch");
    require(value->pointCode == "PCS_RUN_STATE_STD", "derived pointCode should be enriched");
    require(value->value == 1.0, "mapped standby standard value mismatch");

    seed(router, 99.0, 0.0, 1770000000200LL);
    value = router.getLatestByIndex(910001, 1770000000300LL);
    require(static_cast<bool>(value), "unknown derived value should exist");
    require(value->value == 255.0, "unknown value should use configured fallback");

    seed(router, 2.0, 1.0, 1770000000400LL);
    value = router.getLatestByIndex(910001, 1770000000500LL);
    require(static_cast<bool>(value), "fault derived value should exist");
    require(value->value == 3.0, "fault rule should take priority over raw mapping");

    const auto meterValues = router.getLatestByMeter("GW_TEST", "PCS_1", 1770000000600LL);
    const auto derivedIt = std::find_if(
        meterValues.begin(),
        meterValues.end(),
        [](const edge_gateway::StoredPointValue& item) {
            return item.index == 910001 && item.pointCode == "PCS_RUN_STATE_STD";
        }
    );
    require(derivedIt != meterValues.end(), "meter snapshot should include derived point");

    edge_gateway::PendingWriteCommand command;
    command.index = 910001;
    command.value = 2.0;
    const auto submit = router.submitWriteCommand(command);
    require(!submit.accepted, "derived point write should be rejected");

    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
}

void verifyAutoDerivedIndex() {
    const std::string sharedMemoryName = "gateway_router_normalize_auto_test";
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);

    edge_gateway::MemoryStoreConfig storeConfig;
    storeConfig.sharedMemoryName = sharedMemoryName;
    storeConfig.maxLatestPoints = 64;
    edge_gateway::MemoryPointStore store(storeConfig);
    auto config = makeDeviceConfig(sharedMemoryName);
    config.meters.front().points.front().normalize.targetIndex = 0;
    auto router = makeRouter(config, store, sharedMemoryName);

    const auto route = router.routeByIndex(900000000U);
    require(static_cast<bool>(route), "derived route should allocate high virtual index");
    require(route->derived, "auto derived route should be marked derived");

    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
}

}  // namespace

int main() {
    try {
        verifyDerivedRouteAndMapping();
        verifyAutoDerivedIndex();
        std::cout << "point_store_router_normalize_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "point_store_router_normalize_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
