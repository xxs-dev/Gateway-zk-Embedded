#include <cstdint>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "edge_gateway/gateway_daemon.hpp"
#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"
#include "edge_gateway/point_store_router.hpp"
#include "edge_gateway/power_control_ownership.hpp"

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
        if (writeDelayMs_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(writeDelayMs_));
    }
    void writeSingleRegister(int slave, int address, std::uint16_t value,
                             const edge_gateway::BeforePhysicalWrite& beforeWrite) override {
        if (preSendDelayMs_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(preSendDelayMs_));
        if (beforeWrite) ++guardCalls_;
        if (beforeWrite) beforeWrite();
        writeSingleRegister(slave, address, value);
    }

    void setWriteDelayMs(int value) { writeDelayMs_ = value; }
    void setPreSendDelayMs(int value) { preSendDelayMs_ = value; }
    int guardCalls() const { return guardCalls_; }

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
    int writeDelayMs_ = 0;
    int preSendDelayMs_ = 0;
    int guardCalls_ = 0;
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

std::string uniqueStoreName(const char* prefix) {
    static std::uint64_t sequence = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string(prefix) + "_" + std::to_string(stamp) + "_" + std::to_string(++sequence);
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

void verifyCollectLoopUsesActualPointIntervals() {
    auto item = config("gateway_daemon_interval_test");
    item.collect.defaultIntervalMs = 2;
    require(
        edge_gateway::resolveCollectLoopIntervalMs(item) == 100,
        "explicit point intervals should prevent an unused device default from causing a busy loop"
    );

    item.meters[1].points[0].read.intervalMs = 5;
    require(
        edge_gateway::resolveCollectLoopIntervalMs(item) == 5,
        "collect loop should use the shortest enabled readable point interval"
    );

    for (auto& meter : item.meters) {
        meter.enabled = false;
    }
    require(
        edge_gateway::resolveCollectLoopIntervalMs(item) == 2,
        "device default should remain the fallback when no readable point is enabled"
    );
}

void verifyDeferredCollectionResumesAndHonorsControlLease() {
    class DeferredCollector final : public edge_gateway::ICollector {
    public:
        DeferredCollector(std::atomic<int>& calls, edge_gateway::PriorityControlLease* lease)
            : calls_(calls), lease_(lease) {}
        edge_gateway::CollectCycleResult collectOnce(std::int64_t now, bool) override {
            const auto count = ++calls_;
            if (lease_) lease_->acquire("test-deferred", "METER_1", 1001, now, 30000);
            edge_gateway::CollectCycleResult result;
            result.workDeferred = count < 3;
            return result;
        }
        void publishDeviceOnlineStatus(bool, std::int64_t) const override {}
    private:
        std::atomic<int>& calls_;
        edge_gateway::PriorityControlLease* lease_;
    };
    for (bool block : {false, true}) {
        const auto name = uniqueStoreName("gateway_deferred_collection");
        const auto leasePath = "/tmp/" + name + ".json";
        {
            auto cfg = config(name);
            cfg.meters.resize(1);
            cfg.meters[0].points[0].read.intervalMs = 5000;
            cfg.mqttDriver.priorityControlLeaseFile = leasePath;
            edge_gateway::MemoryPointStore store(cfg.memoryStore);
            edge_gateway::PriorityControlLease lease(leasePath, "test-owner");
            std::atomic<int> calls{0};
            edge_gateway::GatewayDaemon daemon(cfg, store,
                [&](const edge_gateway::DeviceConfig&, edge_gateway::MemoryPointStore&) {
                    return std::unique_ptr<edge_gateway::ICollector>(new DeferredCollector(calls, block ? &lease : nullptr));
                },
                [](const edge_gateway::DeviceConfig& device, edge_gateway::MemoryPointStore&) {
                    return std::unique_ptr<edge_gateway::ICommandExecutor>(new edge_gateway::UnsupportedCommandExecutor(device));
                }, {});
            daemon.start();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
            while (calls.load() < (block ? 1 : 3) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            daemon.stop();
            require(calls.load() == (block ? 1 : 3),
                    "deferred work slept a full interval, spun after completion, or bypassed a control lease");
        }
        std::remove(leasePath.c_str());
        std::remove((leasePath + ".lock").c_str());
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(name);
    }
}

void verifyCompletionDeadlineWakesBetweenFixedTicks() {
    class DeadlineCollector final : public edge_gateway::ICollector {
    public:
        explicit DeadlineCollector(std::atomic<int>& calls) : calls_(calls) {}
        edge_gateway::CollectCycleResult collectOnce(std::int64_t, bool) override {
            const auto count = ++calls_;
            edge_gateway::CollectCycleResult result;
            result.nextDelayMs = count < 3 ? 20 : 0;
            return result;
        }
        void publishDeviceOnlineStatus(bool, std::int64_t) const override {}
    private:
        std::atomic<int>& calls_;
    };
    const auto name = uniqueStoreName("gateway_collection_deadline");
    {
        auto cfg = config(name);
        cfg.meters.resize(1);
        cfg.meters[0].points[0].read.intervalMs = 500;
        cfg.mqttDriver.priorityControlLeaseFile = "/tmp/" + name + ".json";
        edge_gateway::MemoryPointStore store(cfg.memoryStore);
        std::atomic<int> calls{0};
        edge_gateway::GatewayDaemon daemon(cfg, store,
            [&](const edge_gateway::DeviceConfig&, edge_gateway::MemoryPointStore&) {
                return std::unique_ptr<edge_gateway::ICollector>(new DeadlineCollector(calls));
            },
            [](const edge_gateway::DeviceConfig& device, edge_gateway::MemoryPointStore&) {
                return std::unique_ptr<edge_gateway::ICommandExecutor>(new edge_gateway::UnsupportedCommandExecutor(device));
            }, {});
        daemon.start();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
        while (calls.load() < 4 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        daemon.stop();
        require(calls.load() == 4, "early deadlines waited for fixed ticks or delayed the later normal cadence");
    }
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(name);
}

void verifyAlarmTransitionFlushesWithoutWaitingForPeriodicLoop() {
    const std::string storeName = uniqueStoreName("gateway_daemon_alarm_flush_test");
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);

    {
        auto deviceConfig = config(storeName);
        auto& alarm = deviceConfig.meters.front().points.front();
        alarm.category = "alarm";
        alarm.isStore = true;
        alarm.persistOnChange = true;
        alarm.persistIntervalSec = 60;

        edge_gateway::MemoryPointStore store(deviceConfig.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        edge_gateway::GatewayDaemon daemon(deviceConfig, store, client);

        daemon.collectOnce(1000);

        require(client->readCount(1) == 1, "alarm point should be collected");
        require(
            store.getStats().persistentCount == 0,
            "alarm transition must flush without waiting for the periodic persistence thread"
        );
    }

    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
}

void verifyStaleControlGenerationIsRejectedBeforeDeviceWrite() {
    const std::string storeName = uniqueStoreName("gateway_daemon_control_generation_test");
    const std::string ownershipFile = "/tmp/gateway-daemon-power-control-owner.json";
    const std::string priorityLeaseFile = "/tmp/gateway-daemon-control-generation-priority.json";
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
    std::remove(ownershipFile.c_str());
    std::remove((ownershipFile + ".lock").c_str());
    std::remove(priorityLeaseFile.c_str());

    try {
        auto deviceConfig = config(storeName);
        deviceConfig.memoryStore.pendingWriteMaxAgeMs = 200;
        deviceConfig.mqttDriver.powerControlOwnershipFile = ownershipFile;
        deviceConfig.mqttDriver.priorityControlLeaseFile = priorityLeaseFile;
        {
            edge_gateway::MemoryPointStore store(deviceConfig.memoryStore);
            edge_gateway::PointStoreRouter router;
            router.setPowerControlOwnershipFile(ownershipFile, "mqtt-forwarder");
            router.addStore(storeName, store);
            router.addRoutesFromDeviceConfigs({deviceConfig}, storeName);

            edge_gateway::PowerControlOwnership external(ownershipFile, "mqtt-forwarder");
            const auto takeover = external.acquireOrRenew(
                "pcs-power", "third-party-session", {1001}, "remote-command-1", 3000, 15000
            );
            require(takeover.accepted, "test takeover should be accepted");

            auto client = std::make_shared<FakeModbusClient>();
            edge_gateway::GatewayDaemon daemon(
                deviceConfig, store, client, nullptr, nullptr, nullptr, std::string()
            );

            edge_gateway::PendingWriteCommand directLocal;
            directLocal.cmdId = "CMD_DIRECT_LOCAL_DURING_TAKEOVER";
            directLocal.index = 1001;
            directLocal.value = 44;
            directLocal.source = "compute-engine";
            directLocal.ts = 3000;
            directLocal.acceptedAt = 3000;
            store.submitWriteCommand(directLocal);
            daemon.processWritebackOnce(3000);
            const auto directLocalResult = store.getWritebackResult(directLocal.cmdId);
            require(
                directLocalResult && !directLocalResult->success &&
                    directLocalResult->stage == "control-rejected",
                "a generation-zero write that bypasses the router must still respect active ownership"
            );
            require(client->writeCount(1) == 0, "direct local write must not bypass active takeover");

            edge_gateway::PendingWriteCommand command;
            command.cmdId = "CMD_STALE_GENERATION";
            command.index = 1001;
            command.value = 55;
            command.source = "mqtt-forwarder";
            command.ts = 3000;
            command.acceptedAt = 3000;
            command.controlGeneration = takeover.generation;
            const auto submitted = router.submitWriteCommand(command);
            require(submitted.accepted, "takeover owner command should enter the write queue");
            const auto queued = store.peekPendingWriteCommands();
            require(
                queued.size() == 1 && queued.front().controlGeneration == takeover.generation,
                "router should stamp the active control generation"
            );

            const auto releasedGeneration = external.releaseAndAdvance("third-party-session");
            require(
                releasedGeneration != 0 && releasedGeneration != takeover.generation,
                "test release should invalidate the queued generation"
            );

            daemon.processWritebackOnce(3001);

            const auto result = store.getWritebackResult(command.cmdId);
            require(result && !result->success, "stale control command should fail writeback");
            require(result->stage == "control-rejected", "stale command should be rejected by ownership");
            require(client->writeCount(1) == 0, "rejected command must not reach the field device");

            edge_gateway::PriorityControlLease safetyLease(priorityLeaseFile, "mqtt-driver");
            safetyLease.acquire("CMD_SAFETY_OVERRIDE", "METER_1", 1001, 3002, 30000);
            edge_gateway::PendingWriteCommand safety;
            safety.cmdId = "CMD_SAFETY_OVERRIDE";
            safety.index = 1001;
            safety.value = 0;
            safety.source = "safety";
            safety.ts = 3002;
            safety.acceptedAt = 3002;
            safety.highPriority = true;
            const auto safetySubmitted = router.submitWriteCommand(safety);
            require(safetySubmitted.accepted, "safety override should bypass ownership at enqueue");
            daemon.processWritebackOnce(3002);
            const auto safetyResult = store.getWritebackResult(safety.cmdId);
            require(
                safetyResult && safetyResult->success,
                "safety override should bypass stale ownership at device write"
            );
            require(client->writeCount(1) == 1, "safety override must reach the field device exactly once");

            edge_gateway::PendingWriteCommand stale;
            stale.cmdId = "CMD_EXPIRED_IN_QUEUE";
            stale.index = 1001;
            stale.value = 9;
            stale.source = "compute-engine";
            stale.ts = 3003;
            stale.acceptedAt = 3003;
            store.submitWriteCommand(stale);
            daemon.processWritebackOnce(3003 + deviceConfig.memoryStore.pendingWriteMaxAgeMs + 1);
            const auto staleResult = store.getWritebackResult(stale.cmdId);
            require(staleResult && !staleResult->success && staleResult->stage == "expired",
                "a queued write older than pendingWriteMaxAgeMs must fail with stage expired");
            require(client->writeCount(1) == 1, "an expired write must never reach the field device");

            client->setWriteDelayMs(400);
            for (int i = 0; i < 2; ++i) {
                edge_gateway::PendingWriteCommand queued;
                queued.cmdId = "CMD_BATCH_" + std::to_string(i);
                queued.index = 1001;
                queued.value = 10 + i;
                queued.source = "compute-engine";
                queued.ts = 4000;
                queued.acceptedAt = 4000;
                store.submitWriteCommand(queued);
            }
            daemon.processWritebackOnce(4000);
            const auto batchFirst = store.getWritebackResult("CMD_BATCH_0");
            const auto batchSecond = store.getWritebackResult("CMD_BATCH_1");
            require(batchFirst && batchFirst->success, "first batch write should execute");
            require(batchSecond && !batchSecond->success && batchSecond->stage == "expired",
                "second batch write must expire while the first write occupies the executor");
            require(client->writeCount(1) == 2, "only first batch write should reach device");

            client->setWriteDelayMs(0);
            client->setPreSendDelayMs(400);
            edge_gateway::PendingWriteCommand beforeSend;
            beforeSend.cmdId = "CMD_EXPIRES_BEFORE_SEND";
            beforeSend.index = 1001;
            beforeSend.value = 12;
            beforeSend.source = "compute-engine";
            beforeSend.ts = 5000;
            beforeSend.acceptedAt = 5000;
            store.submitWriteCommand(beforeSend);
            daemon.processWritebackOnce(5000);
            const auto beforeSendResult = store.getWritebackResult(beforeSend.cmdId);
            require(beforeSendResult && !beforeSendResult->success && beforeSendResult->stage == "expired",
                "write must expire after transport wait and before physical send");
            require(client->writeCount(1) == 2, "expired before-send write reached device");

            client->setPreSendDelayMs(0);
            deviceConfig.memoryStore.pendingWriteMaxAgeMs = 0;
            edge_gateway::GatewayDaemon noExpiry(deviceConfig, store, client);
            edge_gateway::PendingWriteCommand noExpiryCommand = beforeSend;
            noExpiryCommand.cmdId = "CMD_ZERO_MAX_AGE";
            noExpiryCommand.ts = 1;
            noExpiryCommand.acceptedAt = 1;
            store.submitWriteCommand(noExpiryCommand);
            const auto guardsBefore = client->guardCalls();
            noExpiry.processWritebackOnce(100000);
            const auto noExpiryResult = store.getWritebackResult(noExpiryCommand.cmdId);
            require(noExpiryResult && noExpiryResult->success, "maxAge=0 must preserve ordinary write");
            require(client->guardCalls() == guardsBefore, "maxAge=0 must not add a guarded write callback");
            require(client->writeCount(1) == 3, "maxAge=0 ordinary write did not reach device");
        }
    } catch (...) {
        std::remove(ownershipFile.c_str());
        std::remove((ownershipFile + ".lock").c_str());
        std::remove(priorityLeaseFile.c_str());
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
        throw;
    }

    std::remove(ownershipFile.c_str());
    std::remove((ownershipFile + ".lock").c_str());
    std::remove(priorityLeaseFile.c_str());
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
}

void verifyExpiredControlLeaseIsRejectedBeforeDeviceWrite() {
    const std::string storeName = uniqueStoreName("gateway_daemon_expired_control_test");
    const std::string ownershipFile = "/tmp/gateway-daemon-expired-control-owner.json";
    const std::string priorityLeaseFile = "/tmp/gateway-daemon-expired-control-priority.json";
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
    std::remove(ownershipFile.c_str());
    std::remove((ownershipFile + ".lock").c_str());
    std::remove(priorityLeaseFile.c_str());

    try {
        auto deviceConfig = config(storeName);
        deviceConfig.mqttDriver.powerControlOwnershipFile = ownershipFile;
        deviceConfig.mqttDriver.priorityControlLeaseFile = priorityLeaseFile;
        {
            edge_gateway::MemoryPointStore store(deviceConfig.memoryStore);
            edge_gateway::PointStoreRouter router;
            router.setPowerControlOwnershipFile(ownershipFile, "mqtt-forwarder");
            router.addStore(storeName, store);
            router.addRoutesFromDeviceConfigs({deviceConfig}, storeName);

            edge_gateway::PowerControlOwnership external(ownershipFile, "mqtt-forwarder");
            const auto takeover = external.acquireOrRenew(
                "pcs-power", "third-party-session", {1001}, "remote-expiring", 5000, 1000
            );
            require(takeover.accepted, "expiring takeover should be accepted");
            require(
                external.recordReceipt(
                    "third-party-session", "remote-expiring", takeover.generation, true
                ),
                "expiring command receipt should be committed"
            );

            edge_gateway::PendingWriteCommand command;
            command.cmdId = "CMD_EXPIRED_LEASE";
            command.index = 1001;
            command.value = 66;
            command.source = "mqtt-forwarder";
            command.ts = 5000;
            command.acceptedAt = 5000;
            command.controlGeneration = takeover.generation;
            const auto submitted = router.submitWriteCommand(command);
            require(submitted.accepted, "expiring owner command should enter the write queue");

            auto client = std::make_shared<FakeModbusClient>();
            edge_gateway::GatewayDaemon daemon(
                deviceConfig, store, client, nullptr, nullptr, nullptr, std::string()
            );
            daemon.processWritebackOnce(6001);

            const auto result = store.getWritebackResult(command.cmdId);
            require(result && !result->success, "expired lease command should fail writeback");
            require(
                result->stage == "control-rejected",
                "expired lease command should be rejected by ownership"
            );
            require(client->writeCount(1) == 0, "expired command must not reach the field device");

            edge_gateway::PendingWriteCommand zeroGenerationRemote;
            zeroGenerationRemote.cmdId = "CMD_EXPIRED_ZERO_GENERATION";
            zeroGenerationRemote.index = 1001;
            zeroGenerationRemote.value = 67;
            zeroGenerationRemote.source = "mqtt-forwarder";
            zeroGenerationRemote.ts = 6002;
            zeroGenerationRemote.acceptedAt = 6002;
            store.submitWriteCommand(zeroGenerationRemote);
            daemon.processWritebackOnce(6002);
            const auto zeroGenerationResult = store.getWritebackResult(zeroGenerationRemote.cmdId);
            require(
                zeroGenerationResult && !zeroGenerationResult->success &&
                    zeroGenerationResult->stage == "control-rejected",
                "expired zero-generation third-party command should fail ownership validation"
            );
            require(client->writeCount(1) == 0, "zero-generation remote command must not reach the field device");
        }
    } catch (...) {
        std::remove(ownershipFile.c_str());
        std::remove((ownershipFile + ".lock").c_str());
        std::remove(priorityLeaseFile.c_str());
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
        throw;
    }

    std::remove(ownershipFile.c_str());
    std::remove((ownershipFile + ".lock").c_str());
    std::remove(priorityLeaseFile.c_str());
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
}

}  // namespace

int main() {
    const std::string storeName = uniqueStoreName("gateway_daemon_realtime_priority_test");
    const std::string leaseFile = "/tmp/gateway-daemon-realtime-priority.json";
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
    std::remove(leaseFile.c_str());

    try {
        verifyCollectLoopUsesActualPointIntervals();
        verifyDeferredCollectionResumesAndHonorsControlLease();
        verifyCompletionDeadlineWakesBetweenFixedTicks();
        verifyAlarmTransitionFlushesWithoutWaitingForPeriodicLoop();
        verifyStaleControlGenerationIsRejectedBeforeDeviceWrite();
        verifyExpiredControlLeaseIsRejectedBeforeDeviceWrite();
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

            require(client->readCount(1) == 1, "realtime collection should retain one background meter slot");
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
        priorityCommand.controlGeneration = 0xA11C0001U;
        writeStore.submitWriteCommand(priorityCommand);

        const auto pendingBeforeConsume = writeStore.peekPendingWriteCommands();
        require(
            pendingBeforeConsume.size() == 2 &&
            pendingBeforeConsume.back().cmdId == "CMD_WRITE_1" &&
            pendingBeforeConsume.back().highPriority &&
            pendingBeforeConsume.back().controlGeneration == 0xA11C0001U,
            "priority pending write should retain control generation before daemon consumption"
        );

        writeDaemon.processWritebackOnce(2001);

        const auto priorityResult = writeStore.getWritebackResult("CMD_WRITE_1");
        const auto normalResult = writeStore.getWritebackResult("CMD_NORMAL");
        const auto pending = writeStore.peekPendingWriteCommands();
        require(priorityResult && priorityResult->success, "priority control command should process while lease is active");
        require(priorityResult->highPriority, "writeback result should preserve the high-priority flag across shared memory");
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
