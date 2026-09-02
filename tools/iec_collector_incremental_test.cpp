#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "edge_gateway/iec_client.hpp"
#include "edge_gateway/iec_collector.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

edge_gateway::PointDefinition point(
    std::uint32_t index,
    const std::string& code,
    int typeId,
    int informationNumber
) {
    edge_gateway::PointDefinition result;
    result.index = index;
    result.pointCode = code;
    result.name = code;
    result.category = typeId == 44 ? "signal" : "telemetry";
    result.enabled = true;
    result.read.enable = true;
    result.read.dataType = typeId == 44 ? "bool" : "float32";
    result.read.intervalMs = 100;
    result.read.cachePolicy.storeLatest = true;
    result.read.cachePolicy.ttlMs = 5000;
    result.read.iec.functionType = 1;
    result.read.iec.informationNumber = informationNumber;
    result.read.iec.typeId = typeId;
    result.read.iec.commonAddress = 1;
    return result;
}

edge_gateway::IecDataValue value(int typeId, int informationNumber, double number) {
    edge_gateway::IecDataValue result;
    result.typeId = typeId;
    result.commonAddress = 1;
    result.functionType = 1;
    result.informationNumber = informationNumber;
    result.ioa = 256 + informationNumber;
    result.value = number;
    return result;
}

class IncrementalIecClient final : public edge_gateway::IecClient {
public:
    std::vector<edge_gateway::IecDataValue> poll() override {
        ++pollCount_;
        if (pollCount_ == 1) {
            return {value(44, 100, 1.0)};
        }
        return {value(10, 140, 12.5)};
    }

private:
    int pollCount_ = 0;
};

void verifyIncrementalPollKeepsOtherClassValue() {
    const std::string storeName = "gateway_iec_collector_incremental_test";
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
    {
        edge_gateway::DeviceConfig config;
        config.machineCode = "GW_IEC_TEST";
        config.meterCode = "AM5SE_F_TEST";
        config.protocol.type = "iec103";
        config.collect.defaultIntervalMs = 100;
        config.memoryStore.sharedMemoryName = storeName;
        config.memoryStore.maxLatestPoints = 32;
        config.memoryStore.maxPendingWrites = 8;
        config.memoryStore.maxPersistentSamples = 8;
        config.memoryStore.sqlitePath = storeName + ".db";
        config.points = {
            point(433001, "signal_100", 44, 100),
            point(433002, "analog_140", 10, 140)
        };

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<IncrementalIecClient>();
        edge_gateway::IecCollector collector(config, store, client);

        const auto first = collector.collectOnce(1000);
        require(first.values.size() == 1, "first class response should publish one signal");
        const auto signalBefore = store.getLatestByIndex(433001, 1000);
        require(signalBefore && signalBefore->quality == 1 && signalBefore->value == 1.0,
            "signal should be good after general interrogation");
        require(!store.getLatestByIndex(433002, 1000),
            "unseen analog should remain empty instead of becoming bad quality");

        const auto second = collector.collectOnce(1100);
        require(second.values.size() == 1, "second class response should publish one analog");
        const auto signalAfter = store.getLatestByIndex(433001, 1100);
        const auto analog = store.getLatestByIndex(433002, 1100);
        require(signalAfter && signalAfter->quality == 1 && signalAfter->value == 1.0,
            "incremental analog response must not invalidate the previous signal");
        require(signalAfter->ts == signalBefore->ts,
            "omitted signal should retain its original timestamp");
        require(analog && analog->quality == 1 && analog->value == 12.5,
            "analog should be good after its data-class response");
    }
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
}

}  // namespace

int main() {
    try {
        verifyIncrementalPollKeepsOtherClassValue();
        std::cout << "IEC collector incremental response test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << std::endl;
        return 1;
    }
}
