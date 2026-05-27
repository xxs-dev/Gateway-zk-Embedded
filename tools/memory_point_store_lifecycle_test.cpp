#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

edge_gateway::PointDefinition buildPoint() {
    edge_gateway::PointDefinition point;
    point.index = 610001;
    point.pointCode = "KY_TEST_LATEST";
    point.enabled = true;
    point.read.cachePolicy.storeLatest = true;
    point.read.cachePolicy.ttlMs = 600000;
    return point;
}

edge_gateway::PointValue buildValue(std::int64_t ts) {
    edge_gateway::PointValue value;
    value.index = 610001;
    value.machineCode = "GW_TEST";
    value.meterCode = "METER_TEST";
    value.pointCode = "KY_TEST_LATEST";
    value.value = 42.0;
    value.quality = 1;
    value.ts = ts;
    value.expireAt = ts + 600000;
    return value;
}

void verifyReaderDoesNotUnlinkNamedSegment() {
    const std::string storeName = "gateway_memory_lifecycle_test";
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);

    std::unique_ptr<edge_gateway::MemoryPointStore> reader;
    {
        edge_gateway::MemoryStoreConfig config;
        config.sharedMemoryName = storeName;
        config.maxLatestPoints = 32;

        edge_gateway::MemoryPointStore writer(config);
        const auto point = buildPoint();
        writer.registerPoint("GW_TEST", "METER_TEST", point);
        writer.putLatest(buildValue(1000));

        reader.reset(new edge_gateway::MemoryPointStore(storeName));
        const auto readerLatest = reader->getLatestByIndex(point.index, 1000);
        require(static_cast<bool>(readerLatest), "reader should see writer latest value");
    }

    {
        edge_gateway::MemoryPointStore reopened(storeName);
        const auto latest = reopened.getLatestByIndex(610001, 1000);
        require(static_cast<bool>(latest), "reopened store should attach to the existing named segment");
        require(latest->value == 42.0, "reopened store value mismatch");
    }

    reader.reset();
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
}

}  // namespace

int main() {
    try {
        verifyReaderDoesNotUnlinkNamedSegment();
        std::cout << "memory_point_store_lifecycle_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "memory_point_store_lifecycle_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
