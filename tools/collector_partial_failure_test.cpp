#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "edge_gateway/collector.hpp"
#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class FakeModbusClient : public edge_gateway::IModbusClient {
public:
    std::vector<std::uint16_t> readCoils(int slave, int start, int count) override {
        return readRange(slave, start, count);
    }

    std::vector<std::uint16_t> readDiscreteInputs(int slave, int start, int count) override {
        return readRange(slave, start, count);
    }

    std::vector<std::uint16_t> readHoldingRegisters(int slave, int start, int count) override {
        return readRange(slave, start, count);
    }

    std::vector<std::uint16_t> readInputRegisters(int slave, int start, int count) override {
        return readRange(slave, start, count);
    }

    void writeSingleCoil(int slave, int address, bool value) override {
        (void)slave;
        (void)address;
        (void)value;
    }

    void writeSingleRegister(int slave, int address, std::uint16_t value) override {
        (void)slave;
        registers_[address] = value;
    }

    void writeMultipleRegisters(
        int slave,
        int address,
        const std::vector<std::uint16_t>& values
    ) override {
        (void)slave;
        for (std::size_t i = 0; i < values.size(); ++i) {
            registers_[address + static_cast<int>(i)] = values[i];
        }
    }

    void setRegister(int address, std::uint16_t value) {
        registers_[address] = value;
    }

    void failStart(int address) {
        failedStarts_.insert(address);
    }

private:
    std::vector<std::uint16_t> readRange(int slave, int start, int count) {
        require(slave == 1, "unexpected slave");
        if (failedStarts_.find(start) != failedStarts_.end()) {
            throw std::runtime_error("simulated read failure");
        }

        std::vector<std::uint16_t> result;
        result.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            const auto it = registers_.find(start + i);
            result.push_back(it == registers_.end() ? 0 : it->second);
        }
        return result;
    }

    std::unordered_map<int, std::uint16_t> registers_;
    std::unordered_set<int> failedStarts_;
};

edge_gateway::PointDefinition onlinePoint() {
    edge_gateway::PointDefinition point;
    point.index = 500000;
    point.pointCode = "KY02200101";
    point.name = "DEVICE_ONLINE";
    point.category = "status";
    point.enabled = true;
    point.reportOnChange = true;
    point.read.enable = true;
    point.read.function = 0;
    point.read.length = 0;
    point.read.dataType = "device_online";
    point.read.intervalMs = 100;
    point.read.cachePolicy.storeLatest = true;
    point.read.cachePolicy.ttlMs = 600000;
    return point;
}

edge_gateway::PointDefinition registerPoint(
    std::uint32_t index,
    const std::string& pointCode,
    int address
) {
    edge_gateway::PointDefinition point;
    point.index = index;
    point.pointCode = pointCode;
    point.name = pointCode;
    point.category = "telemetry";
    point.address = address;
    point.enabled = true;
    point.read.enable = true;
    point.read.function = 3;
    point.read.length = 1;
    point.read.dataType = "uint16";
    point.read.byteOrder = "AB";
    point.read.intervalMs = 100;
    point.read.cachePolicy.storeLatest = true;
    point.read.cachePolicy.ttlMs = 600000;
    return point;
}

edge_gateway::DeviceConfig buildConfig(const std::string& sharedMemoryName) {
    edge_gateway::DeviceConfig config;
    config.machineCode = "GW_TEST";
    config.meterCode = "METER_1";
    config.deviceName = "Meter 1";
    config.protocol.type = "modbus_rtu";
    config.protocol.slave = 1;
    config.collect.defaultIntervalMs = 100;
    config.collect.maxBatchRegisters = 1;
    config.memoryStore.sharedMemoryName = sharedMemoryName;
    config.memoryStore.maxLatestPoints = 128;
    config.memoryStore.maxPendingWrites = 16;
    config.memoryStore.maxPersistentSamples = 16;
    config.memoryStore.sqlitePath = sharedMemoryName + ".db";
    config.points = {
        onlinePoint(),
        registerPoint(500001, "ok_register", 0),
        registerPoint(500002, "failing_register", 10)
    };
    return config;
}

void cleanupStore(const std::string& sharedMemoryName) {
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
}

void verifyPartialFailureStillOnline() {
    const std::string storeName = "gateway_collector_partial_failure_test";
    cleanupStore(storeName);
    {
        const auto config = buildConfig(storeName);
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->setRegister(0, 123);
        client->failStart(10);

        edge_gateway::Collector collector(config, store, client);
        const auto collected = collector.collectOnce(1000);
        require(collected.executedTasks.size() == 2, "expected two planned read tasks");
        require(collected.values.size() == 2, "expected one good and one failed point value");

        const auto online = store.getLatestByIndex(500000, 1000);
        require(static_cast<bool>(online), "online point missing");
        require(online->pointCode == "KY02200101", "online point should use standard code");
        require(online->value == 1.0, "partial Modbus success should keep device online");
        require(!store.getLatest("GW_TEST", "METER_1", "device_online", 1000), "old online point code should not be registered");

        const auto ok = store.getLatestByIndex(500001, 1000);
        require(static_cast<bool>(ok), "successful point missing");
        require(ok->value == 123.0, "successful point value mismatch");
        require(ok->quality == 1, "successful point quality mismatch");

        const auto failed = store.getLatestByIndex(500002, 1000);
        require(static_cast<bool>(failed), "failed point should still publish a bad-quality value");
        require(failed->quality == 0, "failed point should be bad quality");
    }
    cleanupStore(storeName);
}

void verifyAllFailuresGoOfflineAndThrow() {
    const std::string storeName = "gateway_collector_all_failure_test";
    cleanupStore(storeName);
    {
        const auto config = buildConfig(storeName);
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->failStart(0);
        client->failStart(10);

        edge_gateway::Collector collector(config, store, client);
        bool threw = false;
        try {
            (void)collector.collectOnce(2000);
        } catch (const std::exception&) {
            threw = true;
        }
        require(threw, "all failed Modbus tasks should still raise a collection failure");

        const auto online = store.getLatestByIndex(500000, 2000);
        require(static_cast<bool>(online), "offline point missing");
        require(online->value == 0.0, "all failed Modbus tasks should mark device offline");
    }
    cleanupStore(storeName);
}

}  // namespace

int main() {
    try {
        verifyPartialFailureStillOnline();
        verifyAllFailuresGoOfflineAndThrow();
        std::cout << "collector_partial_failure_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "collector_partial_failure_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
