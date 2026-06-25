#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "edge_gateway/gateway_daemon.hpp"
#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream output(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to write file: " + path);
    }
    output << content;
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

    void writeSingleCoil(int, int, bool) override {
    }

    void writeSingleRegister(int slave, int, std::uint16_t) override {
        ++writesBySlave_[slave];
    }

    void writeMultipleRegisters(int, int, const std::vector<std::uint16_t>&) override {
    }

    int readCount(int slave) const {
        const auto it = readsBySlave_.find(slave);
        return it == readsBySlave_.end() ? 0 : it->second;
    }

    int writeCount(int slave) const {
        const auto it = writesBySlave_.find(slave);
        return it == writesBySlave_.end() ? 0 : it->second;
    }

private:
    std::vector<std::uint16_t> readRange(int slave, int, int count) {
        ++readsBySlave_[slave];
        return std::vector<std::uint16_t>(static_cast<std::size_t>(count), static_cast<std::uint16_t>(slave));
    }

    std::map<int, int> readsBySlave_;
    std::map<int, int> writesBySlave_;
};

edge_gateway::PointDefinition point(std::uint32_t index, const std::string& code, int address) {
    edge_gateway::PointDefinition item;
    item.index = index;
    item.pointCode = code;
    item.name = code;
    item.category = "telemetry";
    item.address = address;
    item.enabled = true;
    item.read.enable = true;
    item.read.function = 3;
    item.read.length = 1;
    item.read.dataType = "uint16";
    item.read.byteOrder = "AB";
    item.read.intervalMs = 100;
    item.read.cachePolicy.storeLatest = true;
    item.read.cachePolicy.ttlMs = 600000;
    item.write.enable = true;
    item.write.function = 6;
    item.write.address = address;
    item.write.length = 1;
    item.write.dataType = "uint16";
    return item;
}

edge_gateway::DeviceConfig config(const std::string& sharedMemoryName) {
    edge_gateway::DeviceConfig item;
    item.machineCode = "GW_TEST";
    item.meterCode = "ROOT";
    item.deviceName = "Root";
    item.protocol.type = "modbus_rtu";
    item.protocol.slave = 1;
    item.collect.defaultIntervalMs = 100;
    item.collect.runtimeMeterBatchSize = 2;
    item.collect.maxTasksPerMeterPerCycle = 1;
    item.collect.maxBatchRegisters = 1;
    item.collect.maxRequestRegisters = 1;
    item.memoryStore.sharedMemoryName = sharedMemoryName;
    item.memoryStore.maxLatestPoints = 16;
    item.memoryStore.maxPendingWrites = 16;
    item.memoryStore.maxPersistentSamples = 16;
    item.memoryStore.sqlitePath.clear();

    edge_gateway::LogicalDeviceConfig meter1;
    meter1.meterCode = "METER_1";
    meter1.deviceName = "Meter 1";
    meter1.slave = 1;
    meter1.points = {point(1001, "P_1", 0)};

    edge_gateway::LogicalDeviceConfig meter2;
    meter2.meterCode = "METER_2";
    meter2.deviceName = "Meter 2";
    meter2.slave = 2;
    meter2.points = {point(2001, "P_2", 0)};

    item.meters = {meter1, meter2};
    return item;
}

}  // namespace

int main() {
    const std::string storeName = "gateway_daemon_realtime_priority_test";
    const std::string leaseFile = "/tmp/gateway-daemon-realtime-priority.json";
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
    std::remove(leaseFile.c_str());

    try {
        auto deviceConfig = config(storeName);
        {
            writeFile(
                leaseFile,
                "{\"machineCode\":\"GW_TEST\",\"updatedAtMs\":1000,\"expireAtMs\":60000,\"meterCodes\":[\"METER_2\"]}"
            );
            edge_gateway::MemoryPointStore store(deviceConfig.memoryStore);
            auto client = std::make_shared<FakeModbusClient>();
            edge_gateway::GatewayDaemon daemon(deviceConfig, store, client, nullptr, nullptr, nullptr, leaseFile);

            daemon.collectOnce(1000);

            require(client->readCount(1) == 0, "cursor meter should be deferred while realtime lease is active");
            require(client->readCount(2) == 1, "realtime lease meter should be collected first");
        }

        const std::string priorityLeaseFile = "/tmp/gateway-daemon-priority-control.json";
        std::remove(priorityLeaseFile.c_str());
        deviceConfig.mqttDriver.priorityControlLeaseFile = priorityLeaseFile;
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
        edge_gateway::MemoryPointStore writeStore(deviceConfig.memoryStore);
        auto writeClient = std::make_shared<FakeModbusClient>();
        edge_gateway::GatewayDaemon writeDaemon(deviceConfig, writeStore, writeClient, nullptr, nullptr, nullptr, leaseFile);
        edge_gateway::PriorityControlLease mqttLease(priorityLeaseFile, "mqtt-driver");
        mqttLease.acquire("CMD_WRITE_1", "METER_1", 1001, 2000, 30000);

        edge_gateway::PendingWriteCommand normalCommand;
        normalCommand.cmdId = "CMD_NORMAL";
        normalCommand.index = 1001;
        normalCommand.value = 41;
        normalCommand.source = "test";
        normalCommand.ts = 1990;
        normalCommand.acceptedAt = 1990;
        writeStore.submitWriteCommand(normalCommand);

        edge_gateway::PendingWriteCommand priorityCommand;
        priorityCommand.cmdId = "CMD_WRITE_1";
        priorityCommand.index = 1001;
        priorityCommand.value = 42;
        priorityCommand.source = "test";
        priorityCommand.ts = 2000;
        priorityCommand.acceptedAt = 2000;
        priorityCommand.highPriority = true;
        writeStore.submitWriteCommand(priorityCommand);

        writeDaemon.processWritebackOnce(2001);

        const auto priorityResult = writeStore.getWritebackResult("CMD_WRITE_1");
        const auto normalResult = writeStore.getWritebackResult("CMD_NORMAL");
        const auto pending = writeStore.peekPendingWriteCommands();
        require(priorityResult && priorityResult->success, "priority control command should process while lease is active");
        require(!normalResult, "normal write should not process while priority control lease is active");
        require(pending.size() == 1 && pending.front().cmdId == "CMD_NORMAL", "normal write should remain pending");
        require(writeClient->writeCount(1) == 1, "only priority writeback should execute while lease is active");

        std::remove(leaseFile.c_str());
        std::remove(priorityLeaseFile.c_str());
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
        std::cout << "gateway_daemon_realtime_priority_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::remove(leaseFile.c_str());
        std::remove("/tmp/gateway-daemon-priority-control.json");
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
        std::cerr << "gateway_daemon_realtime_priority_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
