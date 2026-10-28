// Base fixes shared by all collectors: a backwards wall-clock step no longer freezes collection,
// freshly read values expire after the stale horizon instead of the 10-minute TTL, and configs
// saved with a UTF-8 BOM load.
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/modbus_collector.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::int64_t wallNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

struct CountingClient : edge_gateway::IModbusClient {
    int reads = 0;
    std::vector<std::uint16_t> read(int count) { ++reads; return std::vector<std::uint16_t>(static_cast<std::size_t>(count), 1); }
    std::vector<std::uint16_t> readCoils(int, int, int count) override { return read(count); }
    std::vector<std::uint16_t> readDiscreteInputs(int, int, int count) override { return read(count); }
    std::vector<std::uint16_t> readHoldingRegisters(int, int, int count) override { return read(count); }
    std::vector<std::uint16_t> readInputRegisters(int, int, int count) override { return read(count); }
    void writeSingleCoil(int, int, bool) override {}
    void writeSingleRegister(int, int, std::uint16_t) override {}
    void writeMultipleRegisters(int, int, const std::vector<std::uint16_t>&) override {}
};

edge_gateway::DeviceConfig deviceConfig(const std::string& storeName) {
    edge_gateway::DeviceConfig config;
    config.machineCode = "GW_TEST";
    config.meterCode = "M1";
    config.protocol.type = "modbus_rtu";
    config.protocol.slave = 1;
    config.memoryStore.sharedMemoryName = storeName;
    config.memoryStore.sqlitePath.clear();
    config.memoryStore.maxLatestPoints = 8;
    config.memoryStore.maxPendingWrites = 8;
    config.memoryStore.maxPersistentSamples = 8;
    edge_gateway::PointDefinition point;
    point.index = 1;
    point.pointCode = "P1";
    point.enabled = true;
    point.address = 0;
    point.read.enable = true;
    point.read.function = 3;
    point.read.length = 1;
    point.read.dataType = "uint16";
    point.read.intervalMs = 500;
    point.read.cachePolicy.storeLatest = true;
    config.points = {point};
    return config;
}

void clockStepBackDoesNotFreezeCollection() {
    const auto storeName = "collector_clock_step_" + std::to_string(getpid());
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
    {
        auto config = deviceConfig(storeName);
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<CountingClient>();
        edge_gateway::Collector collector(config, store, client, nullptr);
        // Values are timestamped with the real wall clock, so schedule around it too.
        std::int64_t now = wallNowMs();
        for (int i = 0; i < 3; ++i, now += 500) collector.collectOnce(now);
        require(client->reads == 3, "baseline collection");
        now -= 3600000;  // NTP or a manual set moves the wall clock back one hour
        const auto before = client->reads;
        for (int i = 0; i < 4; ++i, now += 500) collector.collectOnce(now);
        require(client->reads - before >= 3, "collection must continue right after the wall clock steps back (at most one interval is lost)");
        const auto latest = store.getLatestByIndex(1, wallNowMs());
        require(latest && latest->quality == 1 && !latest->stale, "value read after the step is current");
    }
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
}

void readValueExpiresAtStaleHorizon() {
    const auto storeName = "collector_expiry_" + std::to_string(getpid());
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
    {
        auto config = deviceConfig(storeName);
        config.collect.staleAfterIntervals = 20;
        config.collect.staleAfterMinMs = 30000;
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<CountingClient>();
        edge_gateway::Collector collector(config, store, client, nullptr);
        const std::int64_t now = wallNowMs();
        const auto result = collector.collectOnce(now);
        require(!result.values.empty(), "a value was read");
        const auto& value = result.values.front();
        // max(20 x 500 ms, 30 s) = 30 s, well below the 600 s TTL
        require(value.expireAt - value.ts == 30000, "expiry follows the stale horizon, got " +
                std::to_string(value.expireAt - value.ts));
        const auto later = store.getLatestByIndex(1, value.ts + 31000);
        require(!later || later->stale, "a value not refreshed for the horizon reads as stale");

        config.collect.staleAfterIntervals = 0;  // opt out: TTL only
        edge_gateway::Collector ttlOnly(config, store, client, nullptr);
        const auto ttlResult = ttlOnly.collectOnce(now + 1000000);
        require(ttlResult.values.front().expireAt - ttlResult.values.front().ts == 600000, "staleAfterIntervals=0 keeps the TTL");
    }
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
}

void configWithUtf8BomLoads() {
    const std::string text = std::string("\xEF\xBB\xBF") +
        R"({"protocol":{"type":"modbus_rtu","slave":1},"meters":[{"meterCode":"M1","slave":1,"points":[{"index":1,"pointCode":"P1","name":"p","desc":"","category":"telemetry","address":0,"read":{"enable":true,"function":3,"length":1,"dataType":"uint16"}}]}]})";
    const auto config = edge_gateway::ConfigLoader::loadFromText(text);
    require(config.meters.size() == 1 && config.meters.front().points.size() == 1, "BOM-prefixed config parsed");
}

}  // namespace

int main() {
    try {
        clockStepBackDoesNotFreezeCollection();
        readValueExpiresAtStaleHorizon();
        configWithUtf8BomLoads();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "collector_clock_and_expiry_test failed: %s\n", ex.what());
        return 1;
    }
    std::printf("collector_clock_and_expiry_test passed\n");
    return 0;
}
