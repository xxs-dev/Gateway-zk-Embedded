#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "edge_gateway/modbus_collector.hpp"
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

    void failRange(int address, int count) {
        failedRanges_.insert(std::make_pair(address, count));
    }

    void failRangeWithMessage(int address, int count, const std::string& message) {
        const auto key = std::make_pair(address, count);
        failedRanges_.insert(key);
        failedRangeMessages_[key] = message;
    }

    void clearFailStart(int address) {
        failedStarts_.erase(address);
    }

    int readCount(int address) const {
        const auto it = readCounts_.find(address);
        return it == readCounts_.end() ? 0 : it->second;
    }

private:
    std::vector<std::uint16_t> readRange(int slave, int start, int count) {
        require(slave == 1, "unexpected slave");
        ++readCounts_[start];
        const auto key = std::make_pair(start, count);
        if (failedRanges_.find(key) != failedRanges_.end()) {
            const auto message = failedRangeMessages_.find(key);
            throw std::runtime_error(
                message == failedRangeMessages_.end()
                    ? "simulated range read failure"
                    : message->second
            );
        }
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
    std::unordered_map<int, int> readCounts_;
    std::unordered_set<int> failedStarts_;
    std::set<std::pair<int, int>> failedRanges_;
    std::map<std::pair<int, int>, std::string> failedRangeMessages_;
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

edge_gateway::PointDefinition lowPriorityPoint(
    std::uint32_t index,
    const std::string& pointCode,
    int address
) {
    auto point = registerPoint(index, pointCode, address);
    point.collectPriority = 1;
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
    config.collect.maxTasksPerMeterPerCycle = 2;
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
        require(collected.values.size() == 1, "transient failed point without cache should not publish q0 immediately");

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
        require(!static_cast<bool>(failed), "uncached failed point should wait for confirmed failure before q0");
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

void verifyTransientFailureKeepsLastGoodValueByDefault() {
    const std::string storeName = "gateway_collector_failure_marks_bad_test";
    cleanupStore(storeName);
    {
        const auto config = buildConfig(storeName);
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->setRegister(0, 123);
        client->setRegister(10, 456);

        edge_gateway::Collector collector(config, store, client);
        (void)collector.collectOnce(3000);

        auto failing = store.getLatestByIndex(500002, 3000);
        require(static_cast<bool>(failing), "initial successful point missing");
        require(failing->value == 456.0, "initial successful value mismatch");
        require(failing->quality == 1, "initial successful quality mismatch");
        const auto firstTs = failing->ts;

        client->failStart(10);
        (void)collector.collectOnce(3100);

        failing = store.getLatestByIndex(500002, 3100);
        require(static_cast<bool>(failing), "cached point missing after failure");
        require(failing->value == 456.0, "transient failed read should keep last good value by default");
        require(failing->quality == 1, "transient failed read should keep last good quality by default");
        require(failing->ts == firstTs, "transient failed read should not refresh the good timestamp");
    }
    cleanupStore(storeName);
}

void verifyFailureCanKeepLastGoodValueWithGrace() {
    const std::string storeName = "gateway_collector_keep_last_good_grace_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.failureGoodValueGraceMs = 1000;
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->setRegister(0, 123);
        client->setRegister(10, 456);

        edge_gateway::Collector collector(config, store, client);
        (void)collector.collectOnce(3000);

        auto failing = store.getLatestByIndex(500002, 3000);
        require(static_cast<bool>(failing), "initial successful point missing");
        require(failing->value == 456.0, "initial successful value mismatch");
        require(failing->quality == 1, "initial successful quality mismatch");

        client->failStart(10);
        (void)collector.collectOnce(3100);

        failing = store.getLatestByIndex(500002, 3100);
        require(static_cast<bool>(failing), "cached point missing after grace-protected failure");
        require(failing->value == 456.0, "configured grace should keep last good value in shared memory");
        require(failing->quality == 1, "configured grace should keep last good quality in shared memory");
    }
    cleanupStore(storeName);
}

void verifyFailureMarksOldGoodValueBadAfterThreshold() {
    const std::string storeName = "gateway_collector_good_value_grace_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.taskFailureBackoffMs = 0;
        config.collect.failureBadQualityThreshold = 2;
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->setRegister(0, 123);
        client->setRegister(10, 456);

        edge_gateway::Collector collector(config, store, client);
        (void)collector.collectOnce(3300);

        client->failStart(10);
        (void)collector.collectOnce(3400);
        auto failing = store.getLatestByIndex(500002, 3400);
        require(static_cast<bool>(failing), "cached point should remain available after first failure");
        require(failing->value == 456.0, "first failure should keep last good value");
        require(failing->quality == 1, "first failure should keep last good quality");

        (void)collector.collectOnce(3500);

        failing = store.getLatestByIndex(500002, 3500);
        require(static_cast<bool>(failing), "aged good value should still have a latest entry");
        require(failing->value == 456.0, "confirmed failure should preserve last real value");
        require(failing->quality == 0, "aged good value should turn bad after failure grace");
        require(failing->ts >= 3500, "aged good value should refresh timestamp when marked bad");
    }
    cleanupStore(storeName);
}

void verifyRepeatedFailureRefreshesBadQualityTimestamp() {
    const std::string storeName = "gateway_collector_bad_quality_refresh_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.taskFailureBackoffMs = 0;
        config.collect.failureBadQualityThreshold = 1;
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->setRegister(0, 123);
        client->failStart(10);

        edge_gateway::Collector collector(config, store, client);
        (void)collector.collectOnce(3100);
        auto failed = store.getLatestByIndex(500002, 3100);
        require(static_cast<bool>(failed), "initial failed point missing");
        require(failed->quality == 0, "initial failed point should be bad quality");
        const auto firstTs = failed->ts;

        (void)collector.collectOnce(3200);
        failed = store.getLatestByIndex(500002, 3200);
        require(static_cast<bool>(failed), "refreshed failed point missing");
        require(failed->quality == 0, "refreshed failed point should remain bad quality");
        require(failed->ts > firstTs, "bad-quality point timestamp should refresh on repeated failure");
    }
    cleanupStore(storeName);
}

void verifyBackedOffBadPointRefreshesBeforeBudget() {
    const std::string storeName = "gateway_collector_backoff_bad_refresh_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.maxBatchRegisters = 1;
        config.collect.maxTasksPerMeterPerCycle = 1;
        config.collect.taskFailureBackoffThreshold = 1;
        config.collect.taskFailureBackoffMs = 1000;
        config.collect.failureBadQualityThreshold = 1;
        config.points = {
            onlinePoint(),
            registerPoint(500001, "ok_register", 0),
            registerPoint(500002, "failing_register", 10),
            registerPoint(500003, "other_register", 20)
        };

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->setRegister(0, 123);
        client->setRegister(20, 789);
        client->failStart(10);

        edge_gateway::Collector collector(config, store, client);
        (void)collector.collectOnce(7000);
        try {
            (void)collector.collectOnce(7100);
        } catch (const std::exception&) {
        }

        auto failed = store.getLatestByIndex(500002, 7100);
        require(static_cast<bool>(failed), "bad point should exist before pre-budget refresh");
        require(failed->quality == 0, "bad point should be bad before pre-budget refresh");
        const auto firstTs = failed->ts;

        auto collected = collector.collectOnce(7200);
        failed = store.getLatestByIndex(500002, 7200);
        require(static_cast<bool>(failed), "bad point should exist after pre-budget refresh");
        require(failed->quality == 0, "bad point should stay bad after pre-budget refresh");
        require(failed->ts == firstTs, "backed-off bad point should not refresh without a serial read");
        require(collected.executedTasks.size() == 1, "budget should still execute one task");
        require(collected.executedTasks.front().start != 10,
            "backed-off failing task should not perform a serial read during refresh");
        require(client->readCount(10) == 1, "pre-budget refresh should not add serial reads");
    }
    cleanupStore(storeName);
}

void verifyFailedTaskBackoffDoesNotSkipHealthyTask() {
    const std::string storeName = "gateway_collector_task_backoff_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.taskFailureBackoffThreshold = 2;
        config.collect.taskFailureBackoffMs = 1000;
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->setRegister(0, 123);
        client->failStart(10);

        edge_gateway::Collector collector(config, store, client);
        auto collected = collector.collectOnce(1000);
        require(collected.executedTasks.size() == 2, "first cycle should execute both tasks");

        collected = collector.collectOnce(1100);
        require(collected.executedTasks.size() == 2, "second cycle should execute both tasks before backoff starts");
        require(client->readCount(10) == 2, "failed task should have two attempted reads before backoff");

        collected = collector.collectOnce(1200);
        require(collected.executedTasks.size() == 1, "backed-off failed task should be skipped");
        require(collected.executedTasks.front().start == 0, "healthy task should still execute during failed task backoff");
        require(client->readCount(0) == 3, "healthy task should keep being read");
        require(client->readCount(10) == 2, "failed task should not be retried during backoff");

        collected = collector.collectOnce(2200);
        require(collected.executedTasks.size() == 2, "failed task should retry after backoff expires");
        require(client->readCount(10) == 3, "failed task should retry after backoff");
    }
    cleanupStore(storeName);
}

void verifyFailedTaskCanBackoffAfterOneFailure() {
    const std::string storeName = "gateway_collector_task_backoff_once_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.taskFailureBackoffThreshold = 1;
        config.collect.taskFailureBackoffMs = 1000;
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->setRegister(0, 123);
        client->failStart(10);

        edge_gateway::Collector collector(config, store, client);
        auto collected = collector.collectOnce(2300);
        require(collected.executedTasks.size() == 2, "first one-failure cycle should execute both tasks");
        require(client->readCount(10) == 1, "failed task should be tried once");

        collected = collector.collectOnce(2400);
        require(collected.executedTasks.size() == 1, "failed task should be skipped after one failure");
        require(collected.executedTasks.front().start == 0, "healthy task should still execute after one-failure backoff");
        require(client->readCount(10) == 1, "failed task should not be retried during one-failure backoff");
    }
    cleanupStore(storeName);
}

void verifyFailedBatchCanSplitAndKeepGoodSubPoints() {
    const std::string storeName = "gateway_collector_adaptive_split_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.maxBatchRegisters = 16;
        config.collect.taskFailureBackoffThreshold = 1;
        config.collect.taskFailureBackoffMs = 1000;
        config.collect.adaptiveSplitOnFailure = true;
        config.collect.adaptiveSplitMaxRegisters = 1;
        config.collect.adaptiveSplitLeafProbeBudget = 3;
        config.points = {
            onlinePoint(),
            registerPoint(500001, "ok_register_0", 0),
            registerPoint(500002, "ok_register_1", 1),
            registerPoint(500003, "ok_register_2", 2)
        };

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->setRegister(0, 100);
        client->setRegister(1, 200);
        client->setRegister(2, 300);
        client->failRange(0, 3);

        edge_gateway::Collector collector(config, store, client);
        auto collected = collector.collectOnce(5000);
        require(collected.executedTasks.size() == 4, "failed batch should be followed by three split reads");
        require(client->readCount(0) == 2, "start 0 should be read once as batch and once as split");
        require(client->readCount(1) == 1, "start 1 should be read as split");
        require(client->readCount(2) == 1, "start 2 should be read as split");

        const auto v0 = store.getLatestByIndex(500001, 5000);
        const auto v1 = store.getLatestByIndex(500002, 5000);
        const auto v2 = store.getLatestByIndex(500003, 5000);
        require(static_cast<bool>(v0) && v0->value == 100.0 && v0->quality == 1, "split point 0 should be good");
        require(static_cast<bool>(v1) && v1->value == 200.0 && v1->quality == 1, "split point 1 should be good");
        require(static_cast<bool>(v2) && v2->value == 300.0 && v2->quality == 1, "split point 2 should be good");
    }
    cleanupStore(storeName);
}

void verifyFailedSplitCanFallBackToSinglePointReads() {
    const std::string storeName = "gateway_collector_recursive_split_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.maxBatchRegisters = 16;
        config.collect.taskFailureBackoffThreshold = 1;
        config.collect.taskFailureBackoffMs = 1000;
        config.collect.adaptiveSplitOnFailure = true;
        config.collect.adaptiveSplitMaxRegisters = 4;
        config.collect.adaptiveSplitMaxDepth = 2;
        config.collect.adaptiveSplitLeafProbeBudget = 4;
        config.points = {
            onlinePoint(),
            registerPoint(500001, "ok_register_0", 0),
            registerPoint(500002, "ok_register_1", 1),
            registerPoint(500003, "ok_register_2", 2),
            registerPoint(500004, "ok_register_3", 3),
            registerPoint(500005, "ok_register_4", 4),
            registerPoint(500006, "ok_register_5", 5),
            registerPoint(500007, "ok_register_6", 6),
            registerPoint(500008, "ok_register_7", 7)
        };

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        for (int i = 0; i < 8; ++i) {
            client->setRegister(i, static_cast<std::uint16_t>((i + 1) * 100));
        }
        client->failRange(0, 8);
        client->failRange(0, 4);

        edge_gateway::Collector collector(config, store, client);
        auto collected = collector.collectOnce(5100);
        require(collected.executedTasks.size() == 7, "failed split block should recurse to single-point reads");
        require(client->readCount(0) == 3, "start 0 should be tried as batch, split range, and single point");
        require(client->readCount(1) == 1, "start 1 should be read as single point");
        require(client->readCount(2) == 1, "start 2 should be read as single point");
        require(client->readCount(3) == 1, "start 3 should be read as single point");
        require(client->readCount(4) == 1, "healthy split range should be read once");

        const auto v0 = store.getLatestByIndex(500001, 5100);
        const auto v7 = store.getLatestByIndex(500008, 5100);
        require(static_cast<bool>(v0) && v0->value == 100.0 && v0->quality == 1, "recursive split point 0 should be good");
        require(static_cast<bool>(v7) && v7->value == 800.0 && v7->quality == 1, "recursive split point 7 should be good");
    }
    cleanupStore(storeName);
}

void verifyLeafSplitProbeBudgetRotates() {
    const std::string storeName = "gateway_collector_leaf_budget_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.maxBatchRegisters = 16;
        config.collect.taskFailureBackoffThreshold = 1;
        config.collect.taskFailureBackoffMs = 1000;
        config.collect.adaptiveSplitOnFailure = true;
        config.collect.adaptiveSplitMaxRegisters = 4;
        config.collect.adaptiveSplitMaxDepth = 2;
        config.collect.adaptiveSplitLeafProbeBudget = 2;
        config.points = {
            onlinePoint(),
            registerPoint(500001, "ok_register_0", 0),
            registerPoint(500002, "ok_register_1", 1),
            registerPoint(500003, "ok_register_2", 2),
            registerPoint(500004, "ok_register_3", 3)
        };

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        for (int i = 0; i < 4; ++i) {
            client->setRegister(i, static_cast<std::uint16_t>((i + 1) * 100));
        }
        client->failRange(0, 4);

        edge_gateway::Collector collector(config, store, client);
        auto collected = collector.collectOnce(5200);
        require(collected.executedTasks.size() == 3, "first leaf budget cycle should run parent and two leaves");
        require(client->readCount(0) == 2, "first leaf budget cycle should probe start 0 as leaf");
        require(client->readCount(1) == 1, "first leaf budget cycle should probe start 1 as leaf");
        require(client->readCount(2) == 0, "first leaf budget cycle should defer start 2");
        require(client->readCount(3) == 0, "first leaf budget cycle should defer start 3");

        collected = collector.collectOnce(5300);
        require(collected.executedTasks.size() == 2, "second leaf budget cycle should skip backed-off parent and probe next leaves");
        require(client->readCount(2) == 1, "second leaf budget cycle should probe start 2");
        require(client->readCount(3) == 1, "second leaf budget cycle should probe start 3");
    }
    cleanupStore(storeName);
}

void verifyMeterTaskBudgetPrioritizesOldestTasks() {
    const std::string storeName = "gateway_collector_meter_task_budget_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.maxBatchRegisters = 1;
        config.collect.maxTasksPerMeterPerCycle = 2;
        config.points = {
            onlinePoint(),
            registerPoint(500001, "register_0", 0),
            registerPoint(500002, "register_1", 1),
            registerPoint(500003, "register_2", 2),
            registerPoint(500004, "register_3", 3),
            registerPoint(500005, "register_4", 4)
        };

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        for (int i = 0; i < 5; ++i) {
            client->setRegister(i, static_cast<std::uint16_t>(i + 10));
        }

        edge_gateway::Collector collector(config, store, client);
        auto collected = collector.collectOnce(5400);
        require(collected.executedTasks.size() == 2, "first task-budget cycle should execute two tasks");
        require(collected.executedTasks[0].start == 0, "first task-budget cycle should start at task 0");
        require(collected.executedTasks[1].start == 1, "first task-budget cycle should include task 1");
        require(client->readCount(2) == 0, "first task-budget cycle should defer task 2");

        collected = collector.collectOnce(5500);
        require(collected.executedTasks.size() == 2, "second task-budget cycle should execute two tasks");
        require(collected.executedTasks[0].start == 2, "second task-budget cycle should prioritize never-updated task 2");
        require(collected.executedTasks[1].start == 3, "second task-budget cycle should prioritize never-updated task 3");
        require(client->readCount(4) == 0, "second task-budget cycle should defer task 4");

        collected = collector.collectOnce(5600);
        require(collected.executedTasks.size() == 2, "third task-budget cycle should execute two oldest tasks");
        require(collected.executedTasks[0].start == 4, "third task-budget cycle should read deferred task 4");
        require(collected.executedTasks[1].start == 0, "third task-budget cycle should return to oldest updated task 0");
    }
    cleanupStore(storeName);
}

void verifyMeterTaskBudgetDefersBackedOffTasks() {
    const std::string storeName = "gateway_collector_meter_task_backoff_budget_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.maxBatchRegisters = 1;
        config.collect.maxTasksPerMeterPerCycle = 1;
        config.collect.taskFailureBackoffThreshold = 1;
        config.collect.taskFailureBackoffMs = 1000;
        config.points = {
            onlinePoint(),
            registerPoint(500001, "register_0", 0),
            registerPoint(500002, "register_1", 1)
        };

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->setRegister(0, 10);
        client->setRegister(1, 20);

        edge_gateway::Collector collector(config, store, client);
        auto collected = collector.collectOnce(5700);
        require(collected.executedTasks.size() == 1, "first backoff-budget cycle should execute one task");
        require(collected.executedTasks[0].start == 0, "first backoff-budget cycle should start with task 0");

        client->failStart(0);
        collected = collector.collectOnce(5800);
        require(collected.executedTasks.size() == 1, "second backoff-budget cycle should execute one task");
        require(collected.executedTasks[0].start == 1, "second backoff-budget cycle should initialize task 1");

        try {
            (void)collector.collectOnce(5900);
        } catch (const std::exception&) {
        }
        require(client->readCount(0) == 2, "task 0 should fail and enter backoff");

        collected = collector.collectOnce(6000);
        require(collected.executedTasks.size() == 1, "backed-off task should not consume the next budget slot");
        require(collected.executedTasks[0].start == 1, "budget should prefer non-backed-off task 1");
    }
    cleanupStore(storeName);
}

void verifyBackedOffParentTaskSplitsBeforeBudget() {
    const std::string storeName = "gateway_collector_parent_split_budget_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.maxBatchRegisters = 4;
        config.collect.maxTasksPerMeterPerCycle = 2;
        config.collect.taskFailureBackoffThreshold = 1;
        config.collect.taskFailureBackoffMs = 1000;
        config.collect.adaptiveSplitOnFailure = true;
        config.collect.adaptiveSplitMaxRegisters = 1;
        config.collect.adaptiveSplitLeafProbeBudget = 1;
        config.points = {
            onlinePoint(),
            registerPoint(500001, "register_0", 0),
            registerPoint(500002, "register_1", 1),
            registerPoint(500003, "register_2", 2),
            registerPoint(500004, "register_3", 3)
        };

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        for (int i = 0; i < 4; ++i) {
            client->setRegister(i, static_cast<std::uint16_t>(i + 10));
        }
        client->failRange(0, 4);

        edge_gateway::Collector collector(config, store, client);
        auto collected = collector.collectOnce(6100);
        require(collected.executedTasks.size() == 2, "first parent-split cycle should run parent and one leaf");
        require(collected.executedTasks[0].start == 0 && collected.executedTasks[0].count == 4,
            "first parent-split cycle should try the parent block");

        collected = collector.collectOnce(6200);
        require(collected.executedTasks.size() == 2,
            "backed-off parent block should split before meter task budget");
        require(collected.executedTasks[0].count == 1,
            "second parent-split cycle should execute leaf tasks instead of backed-off parent");
        require(collected.executedTasks[1].count == 1,
            "second parent-split cycle should spend budget on a second leaf task");
        require(client->readCount(0) == 2,
            "leaf start 0 should not be blocked by parent task backoff");
        require(client->readCount(1) == 1,
            "leaf start 1 should run in the same budget cycle");
    }
    cleanupStore(storeName);
}

void verifyNoResponseParentTaskDoesNotSplitBeforeBudget() {
    const std::string storeName = "gateway_collector_no_response_parent_no_split_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.maxBatchRegisters = 4;
        config.collect.maxTasksPerMeterPerCycle = 2;
        config.collect.taskFailureBackoffThreshold = 1;
        config.collect.taskFailureBackoffMs = 1000;
        config.collect.adaptiveSplitOnFailure = true;
        config.collect.adaptiveSplitMaxRegisters = 1;
        config.collect.adaptiveSplitLeafProbeBudget = 4;
        config.points = {
            onlinePoint(),
            registerPoint(500001, "register_0", 0),
            registerPoint(500002, "register_1", 1),
            registerPoint(500003, "register_2", 2),
            registerPoint(500004, "register_3", 3)
        };

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        for (int i = 0; i < 4; ++i) {
            client->setRegister(i, static_cast<std::uint16_t>(i + 10));
        }
        client->failRangeWithMessage(0, 4, "modbus response timeout");

        edge_gateway::Collector collector(config, store, client);
        try {
            (void)collector.collectOnce(6300);
        } catch (const std::exception&) {
        }
        require(client->readCount(0) == 1,
            "no-response parent should be tried once without immediate split reads");
        require(client->readCount(1) == 0,
            "no-response parent should not trigger leaf read 1");

        const auto collected = collector.collectOnce(6400);
        require(collected.executedTasks.empty(),
            "backed-off no-response parent should be skipped instead of split before budget");
        require(client->readCount(0) == 1,
            "backed-off no-response parent should not perform another serial read");
        require(client->readCount(1) == 0,
            "backed-off no-response parent should still not trigger leaf read 1");
    }
    cleanupStore(storeName);
}

void verifyMeterTaskBudgetPrioritizesDefaultPriorityPointsOverLowPriority() {
    const std::string storeName = "gateway_collector_base_priority_budget_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.maxBatchRegisters = 8;
        config.collect.maxTasksPerMeterPerCycle = 2;
        config.points = {
            onlinePoint(),
            registerPoint(500001, "base_register_0", 0),
            registerPoint(500002, "base_register_1", 1),
            registerPoint(500003, "base_register_2", 2),
            lowPriorityPoint(500004, "low_priority_register_3", 3),
            lowPriorityPoint(500005, "low_priority_register_4", 4)
        };

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        for (int i = 0; i < 5; ++i) {
            client->setRegister(i, static_cast<std::uint16_t>(i + 10));
        }

        edge_gateway::Collector collector(config, store, client);
        auto collected = collector.collectOnce(6400);
        require(collected.executedTasks.size() == 1, "priority cycle should execute one realtime task");
        require(collected.executedTasks[0].start == 0,
            "default-priority points should be planned before low-priority points");
        require(collected.executedTasks[0].count == 3,
            "realtime base points should stay batched when background points are present");
        require(client->readCount(3) == 0, "low-priority point should not consume initial high priority budget");
    }
    cleanupStore(storeName);
}

void verifyLowPriorityTasksGetPeriodicBudgetSlot() {
    const std::string storeName = "gateway_collector_low_priority_slot_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.maxBatchRegisters = 1;
        config.collect.maxTasksPerMeterPerCycle = 2;
        config.collect.backgroundTaskIntervalMs = 500;
        config.collect.maxBackgroundTasksPerMeterPerCycle = 1;
        config.points = {
            onlinePoint(),
            registerPoint(500001, "base_register_0", 0),
            registerPoint(500002, "base_register_1", 1),
            registerPoint(500003, "base_register_2", 2),
            lowPriorityPoint(500004, "low_priority_register_3", 3)
        };

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        for (int i = 0; i < 4; ++i) {
            client->setRegister(i, static_cast<std::uint16_t>(i + 10));
        }

        edge_gateway::Collector collector(config, store, client);
        for (int i = 0; i < 4; ++i) {
            (void)collector.collectOnce(6500 + i * 100);
        }
        require(client->readCount(3) == 0, "low priority point should wait for periodic budget slot");

        (void)collector.collectOnce(7000);
        require(client->readCount(3) == 1, "low priority point should get a periodic budget slot");
    }
    cleanupStore(storeName);
}

void verifyRealtimeFocusedRaisesTaskBudgetAndBackgroundSlot() {
    const std::string storeName = "gateway_collector_realtime_focus_budget_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.maxBatchRegisters = 1;
        config.collect.maxTasksPerMeterPerCycle = 1;
        config.collect.realtimeMaxTasksPerMeterPerCycle = 3;
        config.collect.backgroundTaskIntervalMs = 5000;
        config.collect.maxBackgroundTasksPerMeterPerCycle = 1;
        config.collect.realtimeMaxBackgroundTasksPerMeterPerCycle = 1;
        config.points = {
            onlinePoint(),
            registerPoint(500001, "base_register_0", 0),
            registerPoint(500002, "base_register_1", 1),
            registerPoint(500003, "base_register_2", 2),
            lowPriorityPoint(500004, "low_priority_register_3", 3),
            lowPriorityPoint(500005, "low_priority_register_4", 4)
        };

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        for (int i = 0; i < 5; ++i) {
            client->setRegister(i, static_cast<std::uint16_t>(i + 10));
        }

        edge_gateway::Collector collector(config, store, client);
        auto collected = collector.collectOnce(10000, false);
        require(collected.executedTasks.size() == 1,
            "normal collection should keep the conservative per-meter task budget");
        require(client->readCount(0) == 1, "normal collection should read one high-priority task");
        require(client->readCount(1) == 0, "normal collection should defer later high-priority tasks");
        require(client->readCount(3) == 0, "normal collection should not spend the first cycle on background tasks");

        collected = collector.collectOnce(10100, true);
        require(collected.executedTasks.size() == 3,
            "realtime focused collection should raise the high-priority task budget");
        require(client->readCount(1) == 1, "focused collection should read deferred high-priority task 1");
        require(client->readCount(2) == 1, "focused collection should read deferred high-priority task 2");
        require(client->readCount(3) == 0, "focused collection should still honor background interval");
        require(client->readCount(4) == 0, "focused collection should still defer background task 4");
    }
    cleanupStore(storeName);
}

void verifyRealtimeFocusedRespectsSlowIntervalPoints() {
    const std::string storeName = "gateway_collector_realtime_force_due_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.maxBatchRegisters = 1;
        config.collect.maxTasksPerMeterPerCycle = 4;
        config.collect.realtimeMaxTasksPerMeterPerCycle = 4;

        auto slowPoint = registerPoint(500002, "slow_register", 1);
        slowPoint.read.intervalMs = 5000;
        config.points = {
            onlinePoint(),
            registerPoint(500001, "fast_register", 0),
            slowPoint
        };

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->setRegister(0, 111);
        client->setRegister(1, 222);

        edge_gateway::Collector collector(config, store, client);
        auto collected = collector.collectOnce(11000, false);
        require(collected.executedTasks.size() == 2, "initial normal cycle should read both points");
        require(client->readCount(1) == 1, "slow point should be read initially");

        client->setRegister(1, 333);
        collected = collector.collectOnce(11100, false);
        require(client->readCount(1) == 1, "normal cycle should respect slow point interval");
        auto slow = store.getLatestByIndex(500002, 11100);
        require(static_cast<bool>(slow), "slow point should remain cached");
        require(slow->value == 222.0, "normal cycle should not refresh slow point before interval");

        collected = collector.collectOnce(11200, true);
        require(client->readCount(1) == 1, "realtime focused cycle should respect slow point interval");
        slow = store.getLatestByIndex(500002, 11200);
        require(static_cast<bool>(slow), "slow point should remain cached after realtime focus");
        require(slow->value == 222.0, "realtime focused cycle should not refresh slow point before interval");

        collected = collector.collectOnce(16000, true);
        require(client->readCount(1) == 2, "slow point should refresh when its interval expires");
        slow = store.getLatestByIndex(500002, 16000);
        require(static_cast<bool>(slow), "slow point should remain cached after interval refresh");
        require(slow->value == 333.0, "slow point should refresh after interval expiry");
    }
    cleanupStore(storeName);
}

void verifyFailedTaskBackoffExtendsAfterRepeatedFailures() {
    const std::string storeName = "gateway_collector_task_backoff_extend_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.taskFailureBackoffThreshold = 1;
        config.collect.taskFailureBackoffMs = 1000;
        config.collect.taskFailureBackoffMaxMs = 4000;
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->setRegister(0, 123);
        client->failStart(10);

        edge_gateway::Collector collector(config, store, client);
        auto collected = collector.collectOnce(6000);
        require(collected.executedTasks.size() == 2, "first extend cycle should execute both tasks");
        require(client->readCount(10) == 1, "failed task should be tried once initially");

        collected = collector.collectOnce(7000);
        require(collected.executedTasks.size() == 2, "failed task should retry when first backoff expires");
        require(client->readCount(10) == 2, "failed task should have retried after first backoff");

        collected = collector.collectOnce(8000);
        require(collected.executedTasks.size() == 1, "second failure should extend backoff beyond one second");
        require(client->readCount(10) == 2, "failed task should not retry during extended backoff");

        collected = collector.collectOnce(9000);
        require(collected.executedTasks.size() == 2, "failed task should retry when extended backoff expires");
        require(client->readCount(10) == 3, "failed task should retry after extended backoff");
    }
    cleanupStore(storeName);
}

void verifyRealtimeFocusedUsesShortTaskBackoff() {
    const std::string storeName = "gateway_collector_realtime_short_backoff_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.maxBatchRegisters = 1;
        config.collect.taskFailureBackoffThreshold = 1;
        config.collect.taskFailureBackoffMs = 10000;
        config.collect.taskFailureBackoffMaxMs = 10000;
        config.collect.realtimeTaskFailureBackoffMs = 500;
        config.collect.realtimeTaskFailureBackoffMaxMs = 500;

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->setRegister(0, 123);
        client->failStart(10);

        edge_gateway::Collector collector(config, store, client);
        auto collected = collector.collectOnce(10000, true);
        require(collected.executedTasks.size() == 2, "focused first cycle should try both tasks");
        require(client->readCount(10) == 1, "focused failed task should be attempted once");

        collected = collector.collectOnce(10300, true);
        require(collected.executedTasks.size() == 1, "focused task should still honor short backoff before expiry");
        require(client->readCount(10) == 1, "focused task should not retry before short backoff expires");

        collected = collector.collectOnce(10600, true);
        require(collected.executedTasks.size() == 2, "focused task should retry after short backoff expires");
        require(client->readCount(10) == 2, "focused task should retry using realtime backoff interval");

        collected = collector.collectOnce(10900, false);
        require(collected.executedTasks.size() == 1, "normal task should honor the longer non-realtime backoff");
        require(client->readCount(10) == 2, "normal task should not retry during long backoff");
    }
    cleanupStore(storeName);
}

void verifyOfflineProbeOnlyRotatesTasks() {
    const std::string storeName = "gateway_collector_offline_probe_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.maxBatchRegisters = 1;
        config.collect.offlineFailureThreshold = 1;
        config.collect.slaveFailureBackoffThreshold = 1;
        config.collect.slaveFailureBackoffMs = 0;
        config.collect.taskFailureBackoffMs = 0;
        config.collect.offlineProbeOnly = true;
        config.collect.offlineProbeTaskCount = 1;
        config.points.push_back(registerPoint(500003, "later_ok_register", 20));

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->setRegister(20, 789);
        client->failStart(0);
        client->failStart(10);
        client->failStart(20);

        edge_gateway::Collector collector(config, store, client);
        bool threw = false;
        try {
            (void)collector.collectOnce(4000);
        } catch (const std::exception&) {
            threw = true;
        }
        require(threw, "first all-failure cycle should throw");

        client->clearFailStart(20);
        try {
            (void)collector.collectOnce(4100);
        } catch (const std::exception&) {
        }
        require(client->readCount(0) == 2, "first offline probe should retry first task");

        try {
            (void)collector.collectOnce(4200);
        } catch (const std::exception&) {
        }
        require(client->readCount(10) == 2, "second offline probe should rotate to next task");

        auto collected = collector.collectOnce(4300);
        require(collected.executedTasks.size() == 1, "offline probe should still execute one task");
        require(collected.executedTasks.front().start == 20, "third offline probe should rotate to healthy task");
        require(client->readCount(20) == 1, "third offline probe should read healthy deferred task");

        const auto online = store.getLatestByIndex(500000, 4300);
        require(static_cast<bool>(online), "online point missing after healthy probe");
        require(online->value == 1.0, "healthy offline probe should recover device online status");
    }
    cleanupStore(storeName);
}

}  // namespace

int main() {
    try {
        verifyPartialFailureStillOnline();
        verifyAllFailuresGoOfflineAndThrow();
        verifyTransientFailureKeepsLastGoodValueByDefault();
        verifyFailureCanKeepLastGoodValueWithGrace();
        verifyFailureMarksOldGoodValueBadAfterThreshold();
        verifyRepeatedFailureRefreshesBadQualityTimestamp();
        verifyBackedOffBadPointRefreshesBeforeBudget();
        verifyFailedTaskBackoffDoesNotSkipHealthyTask();
        verifyFailedTaskCanBackoffAfterOneFailure();
        verifyFailedBatchCanSplitAndKeepGoodSubPoints();
        verifyFailedSplitCanFallBackToSinglePointReads();
        verifyLeafSplitProbeBudgetRotates();
        verifyMeterTaskBudgetPrioritizesOldestTasks();
        verifyMeterTaskBudgetDefersBackedOffTasks();
        verifyBackedOffParentTaskSplitsBeforeBudget();
        verifyNoResponseParentTaskDoesNotSplitBeforeBudget();
        verifyMeterTaskBudgetPrioritizesDefaultPriorityPointsOverLowPriority();
        verifyLowPriorityTasksGetPeriodicBudgetSlot();
        verifyRealtimeFocusedRaisesTaskBudgetAndBackgroundSlot();
        verifyRealtimeFocusedRespectsSlowIntervalPoints();
        verifyFailedTaskBackoffExtendsAfterRepeatedFailures();
        verifyRealtimeFocusedUsesShortTaskBackoff();
        verifyOfflineProbeOnlyRotatesTasks();
        std::cout << "collector_partial_failure_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "collector_partial_failure_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
