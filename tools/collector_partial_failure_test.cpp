#include <algorithm>
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
#include "edge_gateway/command_executor.hpp"
#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/modbus_codec.hpp"
#include "edge_gateway/models.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class FakeModbusClient : public edge_gateway::IModbusClient {
public:
    std::int64_t clockMs = 0;
    int readDelayMs = 0;
    int minimumRequestGapMs = 0;
    // Reported (not enforced) per-slave frame interval, like ModbusRtuClient::requestReadyInMs.
    int frameIntervalMs = 0;
    std::map<int, std::int64_t> lastStartBySlave;
    int requestReadyInMs(int slave) const override {
        const auto last = lastStartBySlave.find(slave);
        if (frameIntervalMs <= 0 || last == lastStartBySlave.end()) return 0;
        return static_cast<int>(std::max<std::int64_t>(0, last->second + frameIntervalMs - clockMs));
    }
    std::int64_t lastRequestStartMs = -1;
    std::vector<std::pair<int, std::int64_t>> completedReads;
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
        require(slave >= 1 && slave <= 2, "unexpected slave");
        ++readCounts_[start];
        if (lastRequestStartMs >= 0) {
            clockMs = std::max(clockMs, lastRequestStartMs + minimumRequestGapMs);
        }
        lastRequestStartMs = clockMs;
        lastStartBySlave[slave] = clockMs;
        clockMs += readDelayMs;
        completedReads.emplace_back(start, clockMs);
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

class TestCollector : public edge_gateway::Collector {
public:
    TestCollector(edge_gateway::DeviceConfig config, edge_gateway::MemoryPointStore& store,
                  const std::shared_ptr<FakeModbusClient>& client,
                  std::shared_ptr<edge_gateway::IMqttPublisher> publisher = nullptr,
                  std::shared_ptr<edge_gateway::ModbusLineSchedule> line = nullptr)
        : Collector(std::move(config), store, client, std::move(publisher),
                    [client] { return client->clockMs; }, std::move(line)) {}
};

class ThrowingMqttPublisher : public edge_gateway::IMqttPublisher {
public:
    void publishTelemetry(
        const std::string&,
        const std::vector<edge_gateway::PointValue>&
    ) override {
        throw std::runtime_error("simulated telemetry publish failure");
    }

    void publishCommandResult(const edge_gateway::CommandResult&) override {
        throw std::runtime_error("simulated command result publish failure");
    }

    void publishStatusMessage(const std::string&, const std::string&) override {
        throw std::runtime_error("simulated status publish failure");
    }
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

void verifyRegisterBoolAliasDecodesConfiguredBit() {
    auto point = registerPoint(500003, "register_bool", 56);
    point.read.function = 4;
    point.read.dataType = "bool";
    point.read.bit = 9;

    const auto setBit = edge_gateway::ModbusCodec::decodeReadValue({0x0A00}, point);
    require(setBit.value == 1.0, "register bool should decode a configured set bit");

    point.read.bit = 10;
    const auto clearBit = edge_gateway::ModbusCodec::decodeReadValue({0x0A00}, point);
    require(clearBit.value == 0.0, "register bool should decode a configured clear bit");
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
    config.collect.cycleBackoffEnabled = false;
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

        TestCollector collector(config, store, client);
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

        TestCollector collector(config, store, client);
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

        TestCollector collector(config, store, client);
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

        TestCollector collector(config, store, client);
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

        TestCollector collector(config, store, client);
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

        TestCollector collector(config, store, client);
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

        TestCollector collector(config, store, client);
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

        TestCollector collector(config, store, client);
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

        TestCollector collector(config, store, client);
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

        TestCollector collector(config, store, client);
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

        TestCollector collector(config, store, client);
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

        TestCollector collector(config, store, client);
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

        TestCollector collector(config, store, client);
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

        TestCollector collector(config, store, client);
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

        TestCollector collector(config, store, client);
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

        TestCollector collector(config, store, client);
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

void verifyBackgroundJoinsRealtimeReadOnlyWhenBothRun() {
    const std::string storeName = "gateway_collector_base_priority_budget_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.maxBatchRegisters = 8;
        config.collect.maxTasksPerMeterPerCycle = 2;
        auto slow3 = lowPriorityPoint(500004, "low_priority_register_3", 3);
        auto slow4 = lowPriorityPoint(500005, "low_priority_register_4", 4);
        slow3.read.intervalMs = slow4.read.intervalMs = 1000;
        config.points = {
            onlinePoint(),
            registerPoint(500001, "base_register_0", 0),
            registerPoint(500002, "base_register_1", 1),
            registerPoint(500003, "base_register_2", 2),
            slow3,
            slow4
        };

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        for (int i = 0; i < 5; ++i) {
            client->setRegister(i, static_cast<std::uint16_t>(i + 10));
        }

        TestCollector collector(config, store, client);
        auto collected = collector.collectOnce(6400);
        require(collected.executedTasks.size() == 1 && collected.executedTasks[0].start == 0 &&
                collected.executedTasks[0].count == 5,
            "background points that run in the same turn share the realtime read");
        require(collected.values.size() == 5 && collected.values.back().value == 14.0, "shared read decoded wrong offsets");

        collected = collector.collectOnce(6500);
        require(collected.executedTasks.size() == 1 && collected.executedTasks[0].count == 3,
            "a background point that is not due must not widen the realtime read");
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

        TestCollector collector(config, store, client);
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

        TestCollector collector(config, store, client);
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

        TestCollector collector(config, store, client);
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

        TestCollector collector(config, store, client);
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

        TestCollector collector(config, store, client);
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

void verifyMqttFailureDoesNotChangeFieldOperationResult() {
    const std::string storeName = "gateway_collector_mqtt_failure_isolation_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.points = {onlinePoint(), registerPoint(500001, "writable_register", 0)};
        auto& writable = config.points.back();
        writable.write.enable = true;
        writable.write.function = 6;
        writable.write.address = 0;
        writable.write.length = 1;
        writable.write.dataType = "uint16";
        writable.write.byteOrder = "AB";

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->setRegister(0, 123);
        auto publisher = std::make_shared<ThrowingMqttPublisher>();

        TestCollector collector(config, store, client, publisher);
        const auto collected = collector.collectOnce(1000);
        require(collected.values.size() == 1, "MQTT failure must not discard collected field data");
        const auto online = store.getLatestByIndex(500000, 1000);
        require(online && online->value == 1.0, "MQTT failure must not mark the field device offline");

        edge_gateway::CommandExecutor executor(config, store, client, publisher);
        const auto result = executor.executeByIndex("CMD_MQTT_FAILURE", 500001, 456.0, 1100);
        require(result.success, "MQTT failure must not change a successful field write result");
    }
    cleanupStore(storeName);
}

void verifyCycleBackoffSkipsConfiguredTaskCycles() {
    const std::string storeName = "gateway_collector_task_cycle_backoff_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.cycleBackoffEnabled = true;
        config.collect.taskFailureBackoffThreshold = 1;
        config.collect.taskFailureBackoffCycles = 1;
        config.collect.taskFailureBackoffMaxCycles = 1;
        config.collect.slaveFailureBackoffThreshold = 100;

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->setRegister(0, 123);
        client->failStart(10);

        TestCollector collector(config, store, client);
        auto collected = collector.collectOnce(2500);
        require(collected.executedTasks.size() == 2, "first cycle should execute both tasks");
        require(client->readCount(10) == 1, "failed task should be attempted on the first cycle");

        collected = collector.collectOnce(2600);
        require(collected.executedTasks.size() == 1, "configured cycle backoff should skip one cycle");
        require(collected.executedTasks.front().start == 0, "healthy task should continue during cycle backoff");
        require(client->readCount(10) == 1, "failed task should not be read during the skipped cycle");

        collected = collector.collectOnce(2700);
        require(collected.executedTasks.size() == 2, "failed task should retry after the skipped cycle");
        require(client->readCount(10) == 2, "failed task should be read again after cycle backoff");
    }
    cleanupStore(storeName);
}

void verifyCycleBackoffKeepsHealthyAdaptiveSplitLeavesRunning() {
    const std::string storeName = "gateway_collector_cycle_adaptive_split_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.maxBatchRegisters = 16;
        config.collect.maxTasksPerMeterPerCycle = 4;
        config.collect.cycleBackoffEnabled = true;
        config.collect.taskFailureBackoffThreshold = 1;
        config.collect.taskFailureBackoffCycles = 1;
        config.collect.taskFailureBackoffMaxCycles = 4;
        config.collect.slaveFailureBackoffThreshold = 100;
        config.collect.adaptiveSplitOnFailure = true;
        config.collect.adaptiveSplitMaxRegisters = 1;
        config.collect.adaptiveSplitLeafProbeBudget = 3;
        config.points = {
            onlinePoint(),
            registerPoint(500001, "healthy_0", 0),
            registerPoint(500002, "invalid_1", 1),
            registerPoint(500003, "healthy_2", 2)
        };

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->setRegister(0, 100);
        client->setRegister(1, 200);
        client->setRegister(2, 300);
        client->failRange(0, 3);
        client->failStart(1);

        TestCollector collector(config, store, client);
        auto collected = collector.collectOnce(3000);
        require(collected.executedTasks.size() == 4, "first cycle should run parent and all split leaves");
        require(client->readCount(0) == 2, "healthy start 0 should run as parent and leaf");
        require(client->readCount(1) == 1, "invalid leaf should fail once");
        require(client->readCount(2) == 1, "healthy start 2 should run as leaf");

        collected = collector.collectOnce(3100);
        require(collected.executedTasks.size() == 2, "parent backoff should still execute both healthy leaves");
        require(client->readCount(0) == 3, "healthy start 0 should continue during parent cycle backoff");
        require(client->readCount(1) == 1, "invalid leaf should honor its own cycle backoff");
        require(client->readCount(2) == 2, "healthy start 2 should continue during parent cycle backoff");
    }
    cleanupStore(storeName);
}

void verifyTransportFailureDoesNotTriggerAdaptiveSplit() {
    const std::string storeName = "gateway_collector_transport_no_split_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.maxBatchRegisters = 16;
        config.collect.adaptiveSplitOnFailure = true;
        config.collect.adaptiveSplitMaxRegisters = 1;
        config.points = {
            onlinePoint(),
            registerPoint(500001, "register_0", 0),
            registerPoint(500002, "register_1", 1),
            registerPoint(500003, "register_2", 2)
        };

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->failRangeWithMessage(0, 3, "modbus tcp recv failed");
        TestCollector collector(config, store, client);

        bool threw = false;
        try {
            (void)collector.collectOnce(3200);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        require(threw, "transport failure should be reported to the daemon");
        require(client->readCount(0) == 1, "transport failure should execute only the parent request");
        require(client->readCount(1) == 0, "transport failure must not probe leaf start 1");
        require(client->readCount(2) == 0, "transport failure must not probe leaf start 2");
    }
    cleanupStore(storeName);
}

void verifyRealtimeFocusedOverridesPreviousLongTaskBackoff() {
    const std::string storeName = "gateway_collector_realtime_override_backoff_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.taskFailureBackoffThreshold = 1;
        config.collect.taskFailureBackoffMs = 10000;
        config.collect.taskFailureBackoffMaxMs = 10000;
        config.collect.realtimeTaskFailureBackoffMs = 500;
        config.collect.realtimeTaskFailureBackoffMaxMs = 500;

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->setRegister(0, 123);
        client->failStart(10);

        TestCollector collector(config, store, client);
        auto collected = collector.collectOnce(12000, false);
        require(collected.executedTasks.size() == 2, "normal cycle should try both tasks");
        require(client->readCount(10) == 1, "normal cycle should record first failed read");

        collected = collector.collectOnce(12600, true);
        require(collected.executedTasks.size() == 2, "focused cycle should ignore stale normal backoff after realtime interval");
        require(client->readCount(10) == 2, "focused cycle should retry failed task without waiting normal backoff");

        collected = collector.collectOnce(12700, false);
        require(collected.executedTasks.size() == 1, "normal cycle should still honor long backoff");
        require(client->readCount(10) == 2, "normal cycle should not retry during long backoff");
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

        TestCollector collector(config, store, client);
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

void verifySlowRequestsYieldAndRotateWithoutStarvation(bool focused) {
    const std::string storeName = "gateway_collector_sliced_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.points = {onlinePoint()};
        config.collect.maxTasksPerMeterPerCycle = 32;
        config.collect.realtimeMaxTasksPerMeterPerCycle = 32;
        config.collect.backgroundTaskIntervalMs = 0;
        config.memoryStore.maxLatestPoints = 512;
        for (int i = 0; i < 253; ++i) {
            auto point = registerPoint(500001 + i, "slow_" + std::to_string(i), i);
            point.read.intervalMs = 1000;
            point.collectPriority = i < 20 ? 0 : 1;
            config.points.push_back(point);
        }
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->readDelayMs = 100;
        TestCollector collector(config, store, client);
        for (int cycle = 0; cycle < 500; ++cycle) {
            const auto started = client->clockMs;
            const auto result = collector.collectOnce(1000 + started, focused);
            require(client->clockMs - started <= 1000, "slow batch monopolized the collector beyond one slice");
            require(!result.executedTasks.empty(), "overloaded collector made no progress");
            require(result.workDeferred, "unfinished slow batch must request prompt continuation");
            for (const auto& value : result.values) require(value.quality == 1, "deferred read was marked failed");
            ++client->clockMs;
        }
        std::map<int, std::int64_t> previous;
        std::int64_t longestGap = 0;
        for (const auto& read : client->completedReads) {
            if (read.first < 20 && previous.count(read.first)) {
                longestGap = std::max(longestGap, read.second - previous.at(read.first));
            }
            previous[read.first] = read.second;
        }
        require(longestGap < 3000, "32 background tasks delayed critical feedback past three seconds");
        for (int i = 0; i < 253; ++i) require(client->readCount(i) > 0, "time slicing starved a point");
        const auto online = store.getLatestByIndex(500000, 1000 + client->clockMs);
        require(online && online->value == 1, "yield was treated as an offline failure");
        std::cout << "slow-request regression: focused=" << focused << "; max critical gap=" << longestGap
                  << "ms; all 20 critical / 233 background tasks refreshed\n";
    }
    cleanupStore(storeName);
}

void verifyBackgroundYieldsWhenRealtimeBecomesDue() {
    const std::string storeName = "gateway_collector_mid_batch_due_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.points.clear();
        config.collect.maxTasksPerMeterPerCycle = 32;
        // Background runs here only because realtime is idle; keep the starvation guard out of the way.
        config.collect.backgroundTaskIntervalMs = 60000;
        for (int i = 0; i < 33; ++i) {
            auto point = registerPoint(500001 + i, "mid_batch_" + std::to_string(i), i);
            // The realtime point's next slot (1000 + 1050) falls inside the second background turn.
            point.read.intervalMs = i == 0 ? 1050 : 1000;
            point.collectPriority = i == 0 ? 0 : 1;
            config.points.push_back(point);
        }
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->readDelayMs = 100;
        TestCollector collector(config, store, client);
        require(collector.collectOnce(1000).workDeferred, "first background slice did not yield");
        ++client->clockMs;
        const auto next = collector.collectOnce(1000 + client->clockMs);
        require(next.workDeferred && next.executedTasks.size() == 1,
                "background batch ignored a realtime point becoming due between requests");
        ++client->clockMs;
        const auto resumed = collector.collectOnce(1000 + client->clockMs);
        require(resumed.executedTasks.front().start == 0, "newly due realtime point did not preempt background");
        require(client->completedReads[11].first == 0 && client->completedReads[11].second <= 1300,
                "realtime deadline waited an extra slice");
    }
    cleanupStore(storeName);
}

// Round 2: reads keep fixed slots (due = previous slot + interval), so a 500 ms point is read every
// 500 ms instead of every 500 ms + read time; a read more than one interval late skips the missed slots.
void verifyReadsKeepTheirSlots() {
    const std::string storeName = "gateway_collector_completion_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.points = {registerPoint(500001, "slot", 0)};
        config.points[0].read.intervalMs = 1000;
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->readDelayMs = 100;
        TestCollector collector(config, store, client);
        collector.collectOnce(1000);  // completes at 1100
        require(collector.collectOnce(2000).executedTasks.size() == 1,
                "the next read is due one interval after the previous slot, not after its completion");
        require(collector.collectOnce(2500).executedTasks.empty(), "a point is read once per slot");
        // Late by 2.5 intervals (slots 3000, 4000 and 5000 missed): one read, then the next future slot.
        require(collector.collectOnce(5500).executedTasks.size() == 1, "an overdue point is read");
        const auto waiting = collector.collectOnce(5900);
        require(waiting.executedTasks.empty(), "missed slots must not be read back in a burst");
        require(waiting.nextDelayMs == 100, "the next read aligns to the next future slot (6000)");
        require(collector.collectOnce(6000).executedTasks.size() == 1, "the aligned slot is read on time");
        require(client->readCount(0) == 4, "exactly one read per served slot");
        // Clock stepped back by 2 s: the slot moves with it (6000 -> 4000), the next read stays one
        // interval later instead of waiting out the step or firing at once.
        const auto stepped = collector.collectOnce(4000);
        require(stepped.executedTasks.empty() && stepped.nextDelayMs == 1000,
                "after a backward clock step the next slot keeps its distance");
        require(collector.collectOnce(5000).executedTasks.size() == 1, "the shifted slot is read on time");
        // Clock jumped forward by 15 s: one read, then the next future slot, no backlog.
        require(collector.collectOnce(20000).executedTasks.size() == 1, "after a forward jump the point is read once");
        const auto afterJump = collector.collectOnce(20500);
        require(afterJump.executedTasks.empty() && afterJump.nextDelayMs == 500,
                "after a forward jump the skipped slots are not read back");
        require(collector.collectOnce(21000).executedTasks.size() == 1, "the next future slot is read on time");
        require(client->readCount(0) == 7, "one read per served slot across clock steps");
        // Late by less than one interval (slot 22000 read at 22950, done 23050): the 23000 slot is
        // still served right away, and the schedule stays on its slots.
        require(collector.collectOnce(22950).executedTasks.size() == 1, "a late point is read");
        require(collector.collectOnce(23050).executedTasks.size() == 1, "the slot passed meanwhile is served at once");
        const auto backOnSlots = collector.collectOnce(23500);
        require(backOnSlots.executedTasks.empty() && backOnSlots.nextDelayMs == 500, "the next read is back on slot 24000");
        require(client->readCount(0) == 9, "a slot is never served twice");
    }
    cleanupStore(storeName);
}

void verifyDeferredFailureDoesNotDeclareWholeMeterFailed() {
    const std::string storeName = "gateway_collector_deferred_failure_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.offlineProbeOnly = false;
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        TestCollector collector(config, store, client);
        collector.collectOnce(1000);
        client->readDelayMs = 100;
        client->failStart(0);
        const auto deferred = collector.collectOnce(1100);
        require(deferred.workDeferred && deferred.executedTasks.size() == 1, "failed slow request did not yield");
        const auto online = store.getLatestByIndex(500000, 1200);
        require(online && online->value == 1, "one failed slice bypassed the offline failure threshold");
        const auto resumed = collector.collectOnce(1201);
        require(!resumed.values.empty() && resumed.values.front().quality == 1, "healthy deferred read did not resume");
    }
    cleanupStore(storeName);
}

void verifySlowAdaptiveSplitYieldsWithoutInventingFailures() {
    const std::string storeName = "gateway_collector_split_slice_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.maxBatchRegisters = 4;
        config.collect.adaptiveSplitMaxRegisters = 1;
        config.collect.adaptiveSplitLeafProbeBudget = 4;
        config.collect.offlineProbeOnly = false;
        config.points.clear();
        for (int i = 0; i < 4; ++i) config.points.push_back(registerPoint(500001 + i, "split_" + std::to_string(i), i));
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->readDelayMs = 100;
        client->failRangeWithMessage(0, 4, "modbus exception code=2 illegal data address");
        TestCollector collector(config, store, client);
        const auto result = collector.collectOnce(1000);
        require(result.workDeferred && result.executedTasks.size() == 1, "adaptive probes bypassed slice boundary");
        require(result.values.empty(), "unattempted adaptive leaves were marked failed");
        for (int i = 0; i < 12; ++i) {
            collector.collectOnce(1000 + client->clockMs);
            ++client->clockMs;
        }
        for (int i = 0; i < 4; ++i) {
            const auto value = store.getLatestByIndex(500001 + i, 1000 + client->clockMs);
            require(value && value->quality == 1, "deferred adaptive leaf never recovered");
        }
    }
    cleanupStore(storeName);
}

void verifyDuePointsShareOneRead(bool focused) {
    const std::string name = "gateway_selected_adjacent_test";
    cleanupStore(name);
    {
        auto config = buildConfig(name);
        config.collect.maxBatchRegisters = 8;
        config.collect.maxTasksPerMeterPerCycle = 4;
        config.collect.realtimeMaxTasksPerMeterPerCycle = 4;
        config.collect.backgroundTaskIntervalMs = 200;
        auto slow = registerPoint(500002, "middle", 11);
        slow.read.intervalMs = 500;
        config.points = {onlinePoint(), registerPoint(500001, "left", 10), slow,
                         registerPoint(500003, "right", 12), registerPoint(500004, "alias", 12)};
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->setRegister(10, 10); client->setRegister(11, 11); client->setRegister(12, 12);
        TestCollector collector(config, store, client);
        const auto merged = collector.collectOnce(1000, focused);
        require(merged.executedTasks.size() == 1 && merged.executedTasks[0].start == 10 &&
                merged.executedTasks[0].count == 3, "due adjacent points were not read together");
        require(merged.values.size() == 4, "merged read lost shared-register fields");
        for (const auto& value : merged.values) {
            const auto expected = value.index == 500001 ? 10 : value.index == 500002 ? 11 : 12;
            require(value.value == expected, "merged read offset decoded wrong register");
        }
        const auto notDue = collector.collectOnce(1200, focused);
        require(notDue.executedTasks.size() == 2 && client->readCount(11) == 0,
                "a point that is not due was used to bridge the gap");
        const auto allDue = collector.collectOnce(1500, focused);
        require(allDue.executedTasks.size() == 1 && allDue.executedTasks[0].count == 3,
                "the slower point did not rejoin the block when due again");
    }
    cleanupStore(name);
}

void verifyMergeBoundariesAndFailureIsolation() {
    for (int mode = 0; mode < 6; ++mode) {
        const std::string name = "gateway_merge_boundary_test";
        cleanupStore(name);
        {
            auto config = buildConfig(name);
            config.collect.maxBatchRegisters = mode == 1 ? 1 : 8;
            config.collect.batchOptimize = mode != 2;
            config.collect.backgroundTaskIntervalMs = 0;
            if (mode == 3) config.collect.maxGapRegisters = 1;
            auto background = registerPoint(500002, "neighbour", (mode == 0 || mode == 3) ? 12 : 11);
            if (mode == 4) background.read.function = 4;
            if (mode == 5) background.enabled = false;
            config.points = {onlinePoint(), registerPoint(500001, "critical", 10), background};
            edge_gateway::MemoryPointStore store(config.memoryStore);
            auto client = std::make_shared<FakeModbusClient>();
            client->setRegister(10, 5); client->setRegister(11, 6); client->setRegister(12, 9);
            TestCollector collector(config, store, client);
            const auto result = collector.collectOnce(1000);
            if (mode == 3) {
                require(result.executedTasks.size() == 1 && result.executedTasks[0].count == 3,
                        "maxGapRegisters=1 did not bridge a one-register gap");
                require(result.values.size() == 2 && result.values[0].value == 5.0 && result.values[1].value == 9.0,
                        "gap read decoded wrong offset");
                cleanupStore(name);
                continue;
            }
            require(result.executedTasks.size() == (mode == 5 ? 1u : 2u),
                    "merge crossed gap/function/cap/enable boundary");
            for (const auto& task : result.executedTasks) require(task.count == 1, "unexpected merged range");
        }
        cleanupStore(name);
    }
    const std::string name = "gateway_merge_failure_test";
    cleanupStore(name);
    {
        auto config = buildConfig(name);
        config.collect.maxBatchRegisters = 8;
        config.collect.backgroundTaskIntervalMs = 0;
        config.collect.taskFailureBackoffThreshold = 1;
        config.collect.taskFailureBackoffMs = 500;
        config.collect.slaveFailureBackoffThreshold = 100;
        config.points = {onlinePoint(), registerPoint(500001, "critical", 10),
                         registerPoint(500002, "neighbour", 11)};
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->setRegister(10, 7); client->setRegister(11, 8);
        client->failRangeWithMessage(10, 2, "modbus exception code=2 illegal data address");
        TestCollector collector(config, store, client);
        const auto split = collector.collectOnce(1000);
        require(client->readCount(10) == 2 && client->readCount(11) == 1,
                "a block rejected with an exception code was not split into its points");
        require(split.values.size() == 2, "split points were not delivered in the same turn");
        const auto backedOff = collector.collectOnce(1200);
        require(backedOff.executedTasks.size() == 2 && client->readCount(10) == 3,
                "the rejected block was retried during its backoff instead of reading its points");
        collector.collectOnce(1600);
        require(client->readCount(10) == 5, "the combined block was not retried after its backoff");
    }
    cleanupStore(name);
}

void verifyEarliestDeadlineFirst() {
    const std::string name = "gateway_collector_deadline_order_test";
    cleanupStore(name);
    {
        auto config = buildConfig(name);
        config.collect.maxBatchRegisters = 1;
        config.collect.maxTasksPerMeterPerCycle = 1;
        auto slow = registerPoint(500001, "slow", 0);
        slow.read.intervalMs = 1000;
        auto fast = registerPoint(500002, "fast", 10);
        fast.read.intervalMs = 200;
        config.points = {onlinePoint(), slow, fast};
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        TestCollector collector(config, store, client);
        collector.collectOnce(1000);
        collector.collectOnce(1000);
        require(client->readCount(0) == 1 && client->readCount(10) == 1, "both points should be read once first");
        // Both overdue at 2000: the fast point's deadline (1200) is earlier than the slow one's (2000).
        const auto turn = collector.collectOnce(2000);
        require(turn.executedTasks.size() == 1 && turn.executedTasks[0].start == 10,
                "the point with the earliest deadline must be read first, not the one updated longest ago");
        require(turn.workDeferred, "due work left after the turn quantum must request an immediate next turn");
        const auto next = collector.collectOnce(2000);
        require(next.executedTasks.size() == 1 && next.executedTasks[0].start == 0,
                "left-over due work must run on the next turn");
    }
    cleanupStore(name);
}

// A slave with frameIntervalMs serves one request per turn: the background share must open the turn,
// otherwise it is never reached (found with the factory ttySP1 config: background never refreshed).
void verifyBackgroundShareWithFrameInterval() {
    const std::string name = "gateway_collector_background_share_frame_test";
    cleanupStore(name);
    {
        auto config = buildConfig(name);
        config.collect.maxBatchRegisters = 1;
        config.collect.backgroundTaskIntervalMs = 1000;
        config.points = {onlinePoint()};
        for (int i = 0; i < 4; ++i) {
            config.points.push_back(registerPoint(500001 + i, "rt_" + std::to_string(i), i * 2));
        }
        auto background = lowPriorityPoint(500100, "bg", 100);
        background.read.intervalMs = 1000;
        config.points.push_back(background);
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->readDelayMs = 10;
        client->frameIntervalMs = 60;
        TestCollector collector(config, store, client);
        while (client->clockMs < 10000) {
            const auto result = collector.collectOnce(1000 + client->clockMs);
            client->clockMs += result.nextDelayMs > 0 ? result.nextDelayMs : 1;
        }
        require(client->readCount(100) >= 8, "background starved behind a busy frame-limited slave: " +
                std::to_string(client->readCount(100)) + " reads in 10 s");
        require(client->readCount(0) >= 25, "realtime reads collapsed while serving the background share");
    }
    cleanupStore(name);
}

// Background points of one meter yield to realtime points of the other meters on the same line
// (factory ttySP1: per-meter idle time let background reads take a third of an overloaded line).
void verifyBackgroundYieldsToRealtimeOfOtherMeters() {
    const std::string name = "gateway_collector_line_schedule_test";
    cleanupStore(name);
    {
        auto realtimeConfig = buildConfig(name);
        // Three separate realtime requests per 100 ms but one request per turn: always behind.
        realtimeConfig.collect.maxTasksPerMeterPerCycle = 1;
        realtimeConfig.points = {onlinePoint(), registerPoint(500001, "rt_0", 0), registerPoint(500003, "rt_2", 2),
                                 registerPoint(500005, "rt_4", 4)};
        auto backgroundConfig = buildConfig(name);
        backgroundConfig.meterCode = "METER_2";
        backgroundConfig.collect.backgroundTaskIntervalMs = 1000;
        auto background = lowPriorityPoint(500002, "bg", 10);
        backgroundConfig.points = {background};
        backgroundConfig.memoryStore = realtimeConfig.memoryStore;
        edge_gateway::MemoryPointStore store(realtimeConfig.memoryStore);
        auto line = std::make_shared<edge_gateway::ModbusLineSchedule>();
        auto realtimeClient = std::make_shared<FakeModbusClient>();
        auto backgroundClient = std::make_shared<FakeModbusClient>();
        TestCollector realtimeMeter(realtimeConfig, store, realtimeClient, nullptr, line);
        TestCollector backgroundMeter(backgroundConfig, store, backgroundClient, nullptr, line);
        for (std::int64_t now = 1000; now < 4000; now += 100) {
            realtimeMeter.collectOnce(now);
            backgroundMeter.collectOnce(now);
        }
        const auto sharedReads = backgroundClient->readCount(10);
        require(sharedReads >= 2 && sharedReads <= 4,
                "while another meter has realtime work, background reads run only as the share: " +
                std::to_string(sharedReads));

        // The realtime meter's slave waits for its frameIntervalMs most of the time: the line is free.
        auto frameLine = std::make_shared<edge_gateway::ModbusLineSchedule>();
        auto sharedClient = std::make_shared<FakeModbusClient>();
        sharedClient->frameIntervalMs = 300;
        sharedClient->readDelayMs = 10;
        auto frameBackgroundConfig = backgroundConfig;
        frameBackgroundConfig.protocol.slave = 2;
        TestCollector frameRealtime(realtimeConfig, store, sharedClient, nullptr, frameLine);
        TestCollector frameBackground(frameBackgroundConfig, store, sharedClient, nullptr, frameLine);
        while (sharedClient->clockMs < 3000) {
            const auto a = frameRealtime.collectOnce(1000 + sharedClient->clockMs);
            const auto b = frameBackground.collectOnce(1000 + sharedClient->clockMs);
            int step = 1;
            for (const auto delay : {a.nextDelayMs, b.nextDelayMs}) {
                if (delay > 0) step = step == 1 ? delay : std::min(step, delay);
            }
            sharedClient->clockMs += step;
        }
        require(sharedClient->readCount(10) >= 7,
                "a realtime slave waiting for its frame interval must leave the line to background reads: " +
                std::to_string(sharedClient->readCount(10)));

        auto idleLine = std::make_shared<edge_gateway::ModbusLineSchedule>();
        auto idleClient = std::make_shared<FakeModbusClient>();
        TestCollector aloneMeter(backgroundConfig, store, idleClient, nullptr, idleLine);
        for (std::int64_t now = 1000; now < 4000; now += 100) {
            aloneMeter.collectOnce(now);
        }
        require(idleClient->readCount(10) >= 25, "on an idle line background points must follow their interval");
    }
    cleanupStore(name);
}

// Review finding: on a shared line the 500 ms grid meter refreshed every 3.0 s while the 1000 ms
// storage meter refreshed every 1.5 s. Meters now give way to more overdue realtime work on the line.
void verifyEarliestDeadlineFirstAcrossMeters() {
    const std::string name = "gateway_collector_line_edf_test";
    cleanupStore(name);
    {
        auto fastConfig = buildConfig(name);
        fastConfig.collect.maxTasksPerMeterPerCycle = 1;
        fastConfig.points = {onlinePoint()};
        auto slowConfig = fastConfig;
        slowConfig.meterCode = "METER_2";
        slowConfig.protocol.slave = 2;
        for (int i = 0; i < 6; ++i) {
            auto fast = registerPoint(500001 + i, "fast_" + std::to_string(i), i * 2);
            fast.read.intervalMs = 100;
            fastConfig.points.push_back(fast);
            auto slow = registerPoint(500101 + i, "slow_" + std::to_string(i), 100 + i * 2);
            slow.read.intervalMs = 300;
            slowConfig.points.push_back(slow);
        }
        slowConfig.points.front().index = 500200;
        edge_gateway::MemoryPointStore store(fastConfig.memoryStore);
        auto line = std::make_shared<edge_gateway::ModbusLineSchedule>();
        auto client = std::make_shared<FakeModbusClient>();
        client->readDelayMs = 40;  // 25 requests/s for 60/s + 20/s of demand
        TestCollector fastMeter(fastConfig, store, client, nullptr, line);
        TestCollector slowMeter(slowConfig, store, client, nullptr, line);
        while (client->clockMs < 20000) {
            const auto slow = slowMeter.collectOnce(1000 + client->clockMs);
            const auto fast = fastMeter.collectOnce(1000 + client->clockMs);
            client->clockMs += (slow.workDeferred || fast.workDeferred) ? 1
                : std::max(1, std::min(slow.nextDelayMs > 0 ? slow.nextDelayMs : 20, fast.nextDelayMs > 0 ? fast.nextDelayMs : 20));
        }
        int fastReads = 0;
        int slowReads = 0;
        for (int i = 0; i < 6; ++i) {
            fastReads += client->readCount(i * 2);
            slowReads += client->readCount(100 + i * 2);
        }
        // Equal turns gave both meters the same reads; deadlines give the 100 ms meter clearly more.
        require(fastReads > slowReads * 13 / 10, "earliest deadline first across meters: fast=" +
                std::to_string(fastReads) + " slow=" + std::to_string(slowReads));
    }
    cleanupStore(name);
}

void verifyBackgroundShareWhileRealtimeOverloaded() {
    const std::string name = "gateway_collector_background_share_test";
    cleanupStore(name);
    {
        auto config = buildConfig(name);
        config.collect.maxBatchRegisters = 1;
        config.collect.maxTasksPerMeterPerCycle = 4;
        config.collect.backgroundTaskIntervalMs = 1000;
        config.points = {onlinePoint()};
        for (int i = 0; i < 8; ++i) {
            config.points.push_back(registerPoint(500001 + i, "rt_" + std::to_string(i), i));
        }
        config.points.push_back(lowPriorityPoint(500100, "bg", 100));
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->readDelayMs = 50;
        TestCollector collector(config, store, client);
        // 8 realtime reads of 50 ms every 100 ms: the meter is permanently busy with realtime work.
        for (int turn = 0; turn < 100; ++turn) {
            collector.collectOnce(1000 + client->clockMs);
            client->clockMs += 1;
        }
        const auto elapsedMs = client->clockMs;
        const auto backgroundReads = client->readCount(100);
        require(backgroundReads >= elapsedMs / 1000 - 1 && backgroundReads <= elapsedMs / 1000 + 2,
                "an overloaded meter must still give the background point about one read per backgroundTaskIntervalMs");
    }
    cleanupStore(name);
    cleanupStore(name);
    {
        // Idle capacity: background runs as soon as it is due, not once per backgroundTaskIntervalMs.
        auto config = buildConfig(name);
        config.collect.maxBatchRegisters = 1;
        config.collect.backgroundTaskIntervalMs = 5000;
        auto background = lowPriorityPoint(500100, "bg", 100);
        background.read.intervalMs = 200;
        config.points = {onlinePoint(), registerPoint(500001, "rt", 0), background};
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        TestCollector collector(config, store, client);
        for (std::int64_t now = 1000; now < 3000; now += 100) {
            collector.collectOnce(now);
        }
        require(client->readCount(100) >= 9, "background points were throttled although the bus was idle");
    }
    cleanupStore(name);
}

void verifyFastBlockWithSafetyAndSlowCoverage(bool focused) {
    const std::string name = "gateway_fast_block_profile_test";
    cleanupStore(name);
    {
        auto config = buildConfig(name);
        config.points = {onlinePoint()};
        config.collect.maxBatchRegisters = 32;
        config.collect.maxTasksPerMeterPerCycle = 1;
        config.collect.realtimeMaxTasksPerMeterPerCycle = 1;
        config.collect.backgroundTaskIntervalMs = 0;
        for (int i = 0; i < 26; ++i) {
            auto point = registerPoint(500001 + i, "fast_" + std::to_string(i), 201 + i);
            point.read.function = 4;
            point.read.intervalMs = 20;
            config.points.push_back(point);
        }
        for (int i = 0; i < 68; ++i) {
            auto point = registerPoint(500100 + i, "other_" + std::to_string(i), 1000 + i * 2);
            point.collectPriority = i < 2 ? 1 : i < 19 ? 2 : 3;
            point.read.intervalMs = i < 2 ? 1000 : i < 19 ? 5000 : 60000;
            config.points.push_back(point);
        }
        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->readDelayMs = 1;
        client->minimumRequestGapMs = 20;
        TestCollector collector(config, store, client);
        while (client->clockMs < 70000) {
            const auto result = collector.collectOnce(1000 + client->clockMs, focused);
            for (const auto& task : result.executedTasks) {
                if (task.function == 4) require(task.start == 201 && task.count == 26,
                    "fast electrical group was split across requests");
            }
            client->clockMs += result.workDeferred ? 1 : result.nextDelayMs > 0
                ? std::min(20, result.nextDelayMs) : 20;
        }
        std::map<int, std::int64_t> last;
        std::vector<std::int64_t> fastGaps;
        std::int64_t safetyGap = 0;
        for (const auto& read : client->completedReads) {
            if (last.count(read.first)) {
                const auto gap = read.second - last.at(read.first);
                if (read.first == 201) fastGaps.push_back(gap);
                else if (read.first < 1004) safetyGap = std::max(safetyGap, gap);
            }
            last[read.first] = read.second;
        }
        std::sort(fastGaps.begin(), fastGaps.end());
        require(fastGaps.size() > 1500 && fastGaps.back() <= 65, "fast block delayed behind slower reads");
        require(fastGaps[fastGaps.size()/2] <= 25, "fast block median not near requested interval");
        require(safetyGap > 0 && safetyGap < 2000, "safety group freshness lost");
        for (int i = 0; i < 68; ++i) require(client->readCount(1000 + i * 2) >= 2, "slower group starved");
        std::cout << "fast-group profile: focused=" << focused << " median=" << fastGaps[fastGaps.size()/2]
                  << "ms max=" << fastGaps.back() << "ms safetyMax=" << safetyGap << "ms\n";
    }
    cleanupStore(name);
}

void verifyNonFiniteFloatDecodeRejected() {
    auto point = registerPoint(500100, "non_finite_float", 0);
    point.read.dataType = "float32";
    point.read.length = 2;
    point.read.byteOrder = "ABCD";

    bool rejected = false;
    try {
        (void)edge_gateway::ModbusCodec::decodeReadValue({0x7FC0, 0x0000}, point);
    } catch (const std::invalid_argument& ex) {
        rejected = std::string(ex.what()).find("not finite") != std::string::npos;
    }
    require(rejected, "non-finite float register values must be rejected");
}

void verifyOfflineProbeIntervalThrottlesConfirmedOfflineMeter() {
    const std::string storeName = "gateway_collector_offline_probe_interval_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.cycleBackoffEnabled = false;
        config.collect.offlineFailureThreshold = 3;
        config.collect.slaveFailureBackoffThreshold = 100;
        config.collect.taskFailureBackoffMs = 0;
        config.collect.taskFailureBackoffMaxMs = 0;
        config.collect.offlineProbeOnly = true;
        config.collect.offlineProbeTaskCount = 1;
        config.collect.offlineProbeIntervalMs = 1000;

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->failStart(0);
        client->failStart(10);

        TestCollector collector(config, store, client);
        try {
            (void)collector.collectOnce(4000);
        } catch (const std::exception&) {
        }
        const auto readsAfterFailure = client->readCount(0) + client->readCount(10);
        require(readsAfterFailure > 0, "meter should be probed initially");

        try {
            (void)collector.collectOnce(4500);
        } catch (const std::exception&) {
        }
        const auto readsBeforeConfirmedOffline = client->readCount(0) + client->readCount(10);
        require(
            readsBeforeConfirmedOffline > readsAfterFailure,
            "transient failures below the offline threshold must not be throttled"
        );

        try {
            (void)collector.collectOnce(5000);
        } catch (const std::exception&) {
        }
        const auto readsAfterConfirmedOffline = client->readCount(0) + client->readCount(10);

        try {
            (void)collector.collectOnce(5500);
        } catch (const std::exception&) {
        }
        require(
            client->readCount(0) + client->readCount(10) == readsAfterConfirmedOffline,
            "offline probe interval should suppress retries only after confirmed offline"
        );

        try {
            (void)collector.collectOnce(6000);
        } catch (const std::exception&) {
        }
        require(
            client->readCount(0) + client->readCount(10) > readsAfterConfirmedOffline,
            "offline probe should resume after configured interval"
        );
    }
    cleanupStore(storeName);
}

void verifyOfflineProbeIntervalThrottlesFailedTaskOnly() {
    const std::string storeName = "gateway_collector_offline_probe_task_interval_test";
    cleanupStore(storeName);
    {
        auto config = buildConfig(storeName);
        config.collect.offlineFailureThreshold = 3;
        config.collect.taskFailureBackoffThreshold = 1;
        config.collect.taskFailureBackoffMs = 0;
        config.collect.taskFailureBackoffMaxMs = 0;
        config.collect.offlineProbeOnly = true;
        config.collect.offlineProbeIntervalMs = 1000;

        edge_gateway::MemoryPointStore store(config.memoryStore);
        auto client = std::make_shared<FakeModbusClient>();
        client->setRegister(0, 123);
        client->failStart(10);

        TestCollector collector(config, store, client);
        for (const auto nowMs : {4000, 4500}) {
            try {
                (void)collector.collectOnce(nowMs);
            } catch (const std::exception&) {
            }
        }
        const auto failedTaskReadsBeforeConfirmation = client->readCount(10);
        require(failedTaskReadsBeforeConfirmation == 2,
            "failed task should be retried before task offline threshold");

        try {
            (void)collector.collectOnce(5000);
        } catch (const std::exception&) {
        }
        const auto failedTaskReadsAfterConfirmation = client->readCount(10);
        require(failedTaskReadsAfterConfirmation == 3,
            "failed task should be probed at the confirmation boundary");

        try {
            (void)collector.collectOnce(5500);
        } catch (const std::exception&) {
        }
        require(client->readCount(10) == failedTaskReadsAfterConfirmation,
            "confirmed failed task should be wall-clock throttled");
        require(client->readCount(0) > 3,
            "healthy task must continue while another task is throttled");

        try {
            (void)collector.collectOnce(6000);
        } catch (const std::exception&) {
        }
        require(client->readCount(10) > failedTaskReadsAfterConfirmation,
            "failed task probe should resume after interval");
    }
    cleanupStore(storeName);
}

}  // namespace

int main() {
    try {
        verifyRegisterBoolAliasDecodesConfiguredBit();
        verifyOfflineProbeIntervalThrottlesConfirmedOfflineMeter();
        verifyOfflineProbeIntervalThrottlesFailedTaskOnly();
        verifyFastBlockWithSafetyAndSlowCoverage(false);
        verifyFastBlockWithSafetyAndSlowCoverage(true);
        verifyDuePointsShareOneRead(false);
        verifyDuePointsShareOneRead(true);
        verifyMergeBoundariesAndFailureIsolation();
        verifyEarliestDeadlineFirst();
        verifyBackgroundShareWhileRealtimeOverloaded();
        verifyBackgroundShareWithFrameInterval();
        verifyBackgroundYieldsToRealtimeOfOtherMeters();
        verifyEarliestDeadlineFirstAcrossMeters();
        verifySlowRequestsYieldAndRotateWithoutStarvation(false);
        verifySlowRequestsYieldAndRotateWithoutStarvation(true);
        verifyBackgroundYieldsWhenRealtimeBecomesDue();
        verifyReadsKeepTheirSlots();
        verifyDeferredFailureDoesNotDeclareWholeMeterFailed();
        verifySlowAdaptiveSplitYieldsWithoutInventingFailures();
        verifyPartialFailureStillOnline();
        verifyMqttFailureDoesNotChangeFieldOperationResult();
        verifyAllFailuresGoOfflineAndThrow();
        verifyTransientFailureKeepsLastGoodValueByDefault();
        verifyFailureCanKeepLastGoodValueWithGrace();
        verifyFailureMarksOldGoodValueBadAfterThreshold();
        verifyRepeatedFailureRefreshesBadQualityTimestamp();
        verifyBackedOffBadPointRefreshesBeforeBudget();
        verifyFailedTaskBackoffDoesNotSkipHealthyTask();
        verifyFailedTaskCanBackoffAfterOneFailure();
        verifyCycleBackoffSkipsConfiguredTaskCycles();
        verifyCycleBackoffKeepsHealthyAdaptiveSplitLeavesRunning();
        verifyTransportFailureDoesNotTriggerAdaptiveSplit();
        verifyFailedBatchCanSplitAndKeepGoodSubPoints();
        verifyFailedSplitCanFallBackToSinglePointReads();
        verifyLeafSplitProbeBudgetRotates();
        verifyMeterTaskBudgetPrioritizesOldestTasks();
        verifyMeterTaskBudgetDefersBackedOffTasks();
        verifyBackedOffParentTaskSplitsBeforeBudget();
        verifyNoResponseParentTaskDoesNotSplitBeforeBudget();
        verifyBackgroundJoinsRealtimeReadOnlyWhenBothRun();
        verifyLowPriorityTasksGetPeriodicBudgetSlot();
        verifyRealtimeFocusedRaisesTaskBudgetAndBackgroundSlot();
        verifyRealtimeFocusedRespectsSlowIntervalPoints();
        verifyFailedTaskBackoffExtendsAfterRepeatedFailures();
        verifyRealtimeFocusedUsesShortTaskBackoff();
        verifyRealtimeFocusedOverridesPreviousLongTaskBackoff();
        verifyOfflineProbeOnlyRotatesTasks();
        verifyNonFiniteFloatDecodeRejected();
        std::cout << "collector_partial_failure_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "collector_partial_failure_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
