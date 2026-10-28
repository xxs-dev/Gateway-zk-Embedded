// End-to-end control result path: MQTT command -> durable ledger -> GatewayDaemon executor
// (CommandResult, incl. outcomeUnknown) -> result store -> MQTT command reply.
#include "control_dedup_test_support.hpp"
#include "edge_gateway/control_dedup_store.hpp"
#include "edge_gateway/gateway_daemon.hpp"
#include "edge_gateway/mqtt_driver_service.hpp"
#include "edge_gateway/point_store_router.hpp"

#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace edge_gateway;
using dedup_test::require;

namespace {

// The executor result the next physical write returns.
struct Script {
    bool success = true;
    bool outcomeUnknown = false;
    std::string message = "ok";
    int calls = 0;
};

class ScriptedExecutor : public ICommandExecutor {
public:
    explicit ScriptedExecutor(Script& script) : script_(script) {}
    CommandResult executePending(const PendingWriteCommand& command, std::int64_t nowMs,
                                 const BeforePhysicalWrite& beforeWrite) const override {
        if (beforeWrite) beforeWrite();
        return executeByIndex(command.cmdId, command.index, command.value, nowMs);
    }
    CommandResult executeByIndex(const std::string& cmdId, std::uint32_t index, double value,
                                 std::int64_t nowMs) const override {
        ++script_.calls;
        CommandResult result;
        result.cmdId = cmdId;
        result.index = index;
        result.requestedValue = value;
        result.ts = nowMs;
        result.success = script_.success;
        result.outcomeUnknown = script_.outcomeUnknown;
        result.message = script_.message;
        return result;
    }
private:
    Script& script_;
};

class IdleCollector : public ICollector {
public:
    CollectCycleResult collectOnce(std::int64_t, bool) override { return CollectCycleResult(); }
    void publishDeviceOnlineStatus(bool, std::int64_t) const override {}
};

class Publisher : public IMqttDriverPublisher {
public:
    std::vector<MqttIncomingMessage> incoming;
    std::vector<MqttCommandReply> replies;
    void publishFullSnapshot(const std::string&, const std::vector<StoredPointValue>&, const std::string&) override {}
    void publishAlarm(const std::string&, std::uint32_t, const StoredPointValue&, const std::string&, bool) override {}
    void publishOnDemand(const std::string&, const std::vector<StoredPointValue>&, const std::string&) override {}
    void publishChangeEvent(const std::string&, const StoredPointValue&) override {}
    void publishCommandReply(const std::string&, const MqttCommandReply& reply) override { replies.push_back(reply); }
    void publishOtaReply(const std::string&, const OtaReply&) override {}
    void publishOtaStatus(const std::string&, const OtaStatus&) override {}
    void publishJsonMessage(const std::string&, const std::string&) override {}
    std::vector<MqttIncomingMessage> pollIncoming(int) override {
        auto result = incoming;
        incoming.clear();
        return result;
    }
};

struct Fixture {
    dedup_test::Directory dir;
    DeviceConfig config;
    std::unique_ptr<MemoryPointStore> store;
    PointStoreRouter router;
    Script script;
    std::unique_ptr<GatewayDaemon> gateway;
    std::shared_ptr<Publisher> publisher = std::make_shared<Publisher>();
    std::unique_ptr<MqttDriverService> service;
    static constexpr std::int64_t t0 = 1770000045000LL;

    explicit Fixture(int controlResultWaitTimeoutMs = 5000, int commandRateMaxPerWindow = 20) {
        config.machineCode = "GW";
        config.meterCode = "METER";
        config.protocol.type = "modbus_rtu";
        config.protocol.slave = 1;
        config.memoryStore.sharedMemoryName = dir.path.filename().string();
        config.memoryStore.maxLatestPoints = 16;
        config.memoryStore.maxPendingWrites = 32;
        config.memoryStore.maxPersistentSamples = 4;
        config.memoryStore.sqlitePath.clear();
        config.memoryStore.controlDedupPath = dir.file("ledger.db");
        config.memoryStore.emsControlModeFile = dir.file("ems-mode.json");
        config.mqttDriver.priorityControlLeaseFile = dir.file("priority.json");
        config.mqttDriver.powerControlOwnershipFile = dir.file("owner.json");
        PointDefinition point;
        point.index = 1001;
        point.pointCode = "P1001";
        point.enabled = true;
        point.write.enable = true;
        point.write.function = 6;
        point.write.length = 1;
        point.write.dataType = "uint16";
        config.points.push_back(point);
        MemoryPointStore::cleanupOrphanedSegment(config.memoryStore.sharedMemoryName);
        store.reset(new MemoryPointStore(config.memoryStore));
        router.addStore(config.memoryStore.sharedMemoryName, *store);
        router.addRoutesFromDeviceConfigs({config}, config.memoryStore.sharedMemoryName);
        edge_gateway::filesystem::create_directories(dir.file("ems-virtual"));
        router.setEmsVirtualParameterDirectory(dir.file("ems-virtual"));
        PointStoreRoute virtualRoute;
        virtualRoute.index = 3001;
        virtualRoute.machineCode = "GW";
        virtualRoute.meterCode = "EMS_VIRTUAL";
        virtualRoute.pointCode = "SOC_LIMIT";
        virtualRoute.protocolType = "ems_virtual";
        virtualRoute.sharedMemoryName = config.memoryStore.sharedMemoryName;
        virtualRoute.writable = true;
        virtualRoute.write.enable = true;
        router.addRoute(virtualRoute);
        gateway.reset(new GatewayDaemon(config, *store,
            [](const DeviceConfig&, MemoryPointStore&) { return std::unique_ptr<ICollector>(new IdleCollector()); },
            [this](const DeviceConfig&, MemoryPointStore&) {
                return std::unique_ptr<ICommandExecutor>(new ScriptedExecutor(script));
            },
            {}));

        MqttConfig mqtt;
        mqtt.enabled = true;
        mqtt.clientId = "GW";
        mqtt.topicMachineCode = "GW";
        mqtt.commandReplyTopic = "edge/command/reply";
        MqttDriverConfig driver;
        driver.enabled = true;
        driver.sharedMemoryName = config.memoryStore.sharedMemoryName;
        driver.emsControlModeFile = config.memoryStore.emsControlModeFile;
        driver.priorityControlLeaseFile = config.mqttDriver.priorityControlLeaseFile;
        driver.powerControlOwnershipFile = config.mqttDriver.powerControlOwnershipFile;
        driver.scanIntervalMs = 100;
        driver.fullUploadIntervalMs = 0;
        driver.publishFullOnStart = false;
        driver.controlResultWaitTimeoutMs = controlResultWaitTimeoutMs;
        driver.commandRateMaxPerWindow = commandRateMaxPerWindow;
        service.reset(new MqttDriverService(mqtt, driver, {config}, router, publisher));
    }
    ~Fixture() {
        service.reset();
        gateway.reset();
        store.reset();
        MemoryPointStore::cleanupOrphanedSegment(config.memoryStore.sharedMemoryName);
    }
    void send(const std::string& cmdId, double value, std::int64_t now, std::uint32_t index = 1001) {
        MqttIncomingMessage message;
        message.type = MqttIncomingType::CommandRequest;
        message.payload = "{\"cmdId\":\"" + cmdId + "\",\"machineCode\":\"GW\",\"index\":" + std::to_string(index) +
            ",\"value\":" + std::to_string(value) + "}";
        publisher->incoming.push_back(message);
        service->runScanOnce(now);
    }
    const MqttCommandReply& last() const { return publisher->replies.back(); }
};

// Regression: the result-wait timeout replied "failed" after 5 s while the queued write could still
// run until pendingWriteMaxAgeMs (10 s); its late result was never reported.
void timeoutIsUnknownAndLateResultIsReported() {
    Fixture f(5000);
    f.send("CMD_LATE", 7, Fixture::t0);
    require(f.publisher->replies.empty(), "accepted command waits for its result");
    f.service->runScanOnce(Fixture::t0 + 5001);
    require(f.publisher->replies.empty(),
            "the result wait is extended past pendingWriteMaxAgeMs: no reply while the write may still start");
    f.service->runScanOnce(Fixture::t0 + 12000);
    require(f.publisher->replies.size() == 1, "deadline publishes one reply");
    require(!f.last().success && f.last().outcome == "unknown" && !f.last().isFinal &&
            f.last().stage == "writeback-timeout", "timeout reply is a non-final unknown, not a failure");
    // The daemon executes the queued write late (it was accepted in time).
    f.gateway->processWritebackOnce(Fixture::t0 + 9000);
    require(f.script.calls == 1, "queued write executed once");
    f.service->runScanOnce(Fixture::t0 + 12100);
    require(f.publisher->replies.size() == 2, "late result is published");
    require(f.last().cmdId == "CMD_LATE" && f.last().success && f.last().outcome == "succeeded" && f.last().isFinal,
            "late final result uses the same cmdId");
    f.service->runScanOnce(Fixture::t0 + 30000);
    require(f.publisher->replies.size() == 2, "nothing further after the final reply");
}

void noResultEndsWithFinalUnknown() {
    Fixture f(5000);
    f.send("CMD_LOST", 7, Fixture::t0);
    f.service->runScanOnce(Fixture::t0 + 12000);
    require(f.publisher->replies.size() == 1 && !f.last().isFinal, "first reply is non-final");
    f.service->runScanOnce(Fixture::t0 + 23999);
    require(f.publisher->replies.size() == 1, "still watching for a late result");
    f.service->runScanOnce(Fixture::t0 + 24000);
    require(f.publisher->replies.size() == 2 && f.last().isFinal && f.last().outcome == "unknown",
            "horizon closes with a final unknown reply");
    f.service->runScanOnce(Fixture::t0 + 60000);
    require(f.publisher->replies.size() == 2, "no reply after the horizon");
}

// CommandResult.outcomeUnknown must reach the reply as "unknown"; a conclusive failure as "failed".
void executorOutcomeReachesReply() {
    {
        Fixture f;
        f.script.success = false;
        f.script.outcomeUnknown = true;
        f.script.message = "no response from device";
        f.send("CMD_UNKNOWN", 7, Fixture::t0);
        f.gateway->processWritebackOnce(Fixture::t0 + 10);
        f.service->runScanOnce(Fixture::t0 + 20);
        require(f.publisher->replies.size() == 1, "unknown result published");
        require(!f.last().success && f.last().outcome == "unknown" && f.last().isFinal &&
                f.last().stage == "writeback-timeout", "executor outcomeUnknown reaches the reply");
        const auto shm = f.store->getWritebackResult("CMD_UNKNOWN", 1001);
        require(shm && shm->outcomeUnknown && !shm->success, "shared-memory receipt keeps outcomeUnknown");
        f.send("CMD_UNKNOWN", 7, Fixture::t0 + 30);
        require(f.script.calls == 1 && f.last().outcome == "unknown", "replay keeps unknown and never re-executes");
    }
    {
        Fixture f;
        f.script.success = false;
        f.script.message = "device exception 02";
        f.send("CMD_FAILED", 7, Fixture::t0);
        f.gateway->processWritebackOnce(Fixture::t0 + 10);
        f.service->runScanOnce(Fixture::t0 + 20);
        require(f.publisher->replies.size() == 1 && !f.last().success && f.last().outcome == "failed" &&
                f.last().stage == "writeback-failed", "conclusive failure reaches the reply as failed");
    }
}

// Regression: a repeated cmdId still in flight was queued again; the ledger then reported the first
// copy's claim as "unknown" while the first was still running.
void sameCmdIdIsNeverExecutedTwice() {
    Fixture f;
    f.send("CMD_DUP", 7, Fixture::t0);
    f.send("CMD_DUP", 7, Fixture::t0 + 100);
    require(f.store->peekPendingWriteCommands().size() == 1, "in-flight duplicate is not queued again");
    require(f.last().stage == "duplicate-in-flight" && f.last().outcome == "unknown" && !f.last().isFinal,
            "in-flight duplicate gets a non-final acknowledgement");
    f.send("CMD_DUP", 8, Fixture::t0 + 200);
    require(f.last().stage == "rejected" && f.store->peekPendingWriteCommands().size() == 1,
            "cmdId reuse with other content is rejected");
    f.gateway->processWritebackOnce(Fixture::t0 + 300);
    f.service->runScanOnce(Fixture::t0 + 400);
    require(f.last().success && f.last().isFinal, "final result published once");
    const auto replies = f.publisher->replies.size();
    f.send("CMD_DUP", 7, Fixture::t0 + 20000);
    require(f.publisher->replies.size() == replies + 1 && f.last().success && f.last().outcome == "succeeded",
            "a completed cmdId replays its result");
    require(f.store->peekPendingWriteCommands().empty() && f.script.calls == 1, "replay never re-executes");
}

// Regression: a duplicate cmdId and a rate-limit rejection produced the same reply.
void duplicateAndRateLimitAreDistinct() {
    Fixture f(5000, 1);
    f.send("CMD_A", 7, Fixture::t0);
    f.send("CMD_B", 7, Fixture::t0 + 10);
    require(f.last().cmdId == "CMD_B" && f.last().stage == "rate-limited" && f.last().outcome == "failed",
            "rate limit has its own stage");
    require(f.store->peekPendingWriteCommands().size() == 1, "rate-limited command is not queued");

    f.send("EV_1", 55, Fixture::t0 + 20, 3001);
    require(f.last().success && f.last().stage == "local-parameter-committed", "virtual parameter commits");
    f.send("EV_1", 55, Fixture::t0 + 30, 3001);
    require(f.last().cmdId == "EV_1" && f.last().success && f.last().stage == "local-parameter-committed",
            "duplicate within commandDedupTtlMs replays the original result");
}

}  // namespace

int main() {
    try {
        timeoutIsUnknownAndLateResultIsReported();
        noResultEndsWithFinalUnknown();
        executorOutcomeReachesReply();
        sameCmdIdIsNeverExecutedTwice();
        duplicateAndRateLimitAreDistinct();
        std::cout << "mqtt_command_outcome_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "mqtt_command_outcome_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
