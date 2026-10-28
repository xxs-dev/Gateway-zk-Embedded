// driver-review: nb-mqtt. One control result per cmdId everywhere: the first MQTT reply, the
// shared-memory receipt, the control ledger record, a same-cmdId replay, a late final reply after the
// "unknown" wait-timeout reply, and a replay from the ledger after a real process restart all report the
// same outcome, cause (stage) and executor message, for each kind of result:
//   succeeded, definite failure, feedback mismatch (outcome unknown), no conclusive reply (unknown).
// A process killed during the physical write leaves an unconfirmed reservation that is never re-executed.
#include "control_dedup_test_support.hpp"
#include "edge_gateway/control_dedup_store.hpp"
#include "edge_gateway/gateway_daemon.hpp"
#include "edge_gateway/mqtt_driver_service.hpp"
#include "edge_gateway/point_store_router.hpp"
#include "edge_gateway/writeback_outcome.hpp"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace edge_gateway;
using dedup_test::require;

namespace {

struct Kind {
    std::string name;
    bool success;
    bool outcomeUnknown;
    bool feedbackMismatch;
    std::string message;
    std::string stage;
    std::string outcome;
};

const std::vector<Kind>& kinds() {
    static const std::vector<Kind> all = {
        {"OK", true, false, false, "ok", "writeback-succeeded", "succeeded"},
        {"FAILED", false, false, false, "modbus exception code 3", "writeback-failed", "failed"},
        {"MISMATCH", false, false, true, "verify failed: wrote 7.000000, read back 5.000000", "writeback-mismatch", "unknown"},
        {"TIMEOUT", false, true, false, "write outcome unknown: modbus response timeout", "writeback-timeout", "unknown"},
    };
    return all;
}

const Kind& kindOf(const std::string& cmdId) {
    for (const auto& kind : kinds()) {
        if (cmdId.compare(0, kind.name.size() + 1, kind.name + "_") == 0) return kind;
    }
    throw std::runtime_error("no kind for " + cmdId);
}

// Executes by cmdId prefix; CRASH_* kills the process in the middle of the "physical write".
class ScriptedExecutor : public ICommandExecutor {
public:
    explicit ScriptedExecutor(int& calls) : calls_(calls) {}
    CommandResult executePending(const PendingWriteCommand& command, std::int64_t nowMs,
                                 const BeforePhysicalWrite& beforeWrite) const override {
        if (beforeWrite) beforeWrite();
        return executeByIndex(command.cmdId, command.index, command.value, nowMs);
    }
    CommandResult executeByIndex(const std::string& cmdId, std::uint32_t index, double value,
                                 std::int64_t nowMs) const override {
        ++calls_;
        if (cmdId.compare(0, 6, "CRASH_") == 0) {
            std::fflush(nullptr);
            _exit(42);
        }
        const auto& kind = kindOf(cmdId);
        CommandResult result;
        result.cmdId = cmdId;
        result.index = index;
        result.requestedValue = value;
        result.ts = nowMs;
        result.success = kind.success;
        result.outcomeUnknown = kind.outcomeUnknown;
        result.feedbackMismatch = kind.feedbackMismatch;
        result.verifyAttempted = kind.feedbackMismatch || kind.success;
        result.verifyPassed = kind.success;
        result.message = kind.message;
        return result;
    }
private:
    int& calls_;
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

// One gateway "boot": shared memory, device daemon and MQTT service over a persistent ledger directory.
struct Gateway {
    DeviceConfig config;
    std::unique_ptr<MemoryPointStore> store;
    PointStoreRouter router;
    int calls = 0;
    std::unique_ptr<GatewayDaemon> daemon;
    std::shared_ptr<Publisher> publisher = std::make_shared<Publisher>();
    std::unique_ptr<MqttDriverService> service;
    int resultWaitMs = 0;

    Gateway(const std::string& directory, const std::string& sharedMemoryName) {
        config.machineCode = "GW";
        config.meterCode = "METER";
        config.protocol.type = "modbus_rtu";
        config.protocol.slave = 1;
        config.memoryStore.sharedMemoryName = sharedMemoryName;
        config.memoryStore.maxLatestPoints = 16;
        config.memoryStore.maxPendingWrites = 64;
        config.memoryStore.maxPersistentSamples = 4;
        config.memoryStore.sqlitePath.clear();
        config.memoryStore.controlDedupPath = directory + "/ledger.db";
        config.memoryStore.emsControlModeFile = directory + "/ems-mode.json";
        config.mqttDriver.priorityControlLeaseFile = directory + "/priority.json";
        config.mqttDriver.powerControlOwnershipFile = directory + "/owner.json";
        PointDefinition point;
        point.index = 1001;
        point.pointCode = "P1001";
        point.enabled = true;
        point.write.enable = true;
        point.write.function = 6;
        point.write.length = 1;
        point.write.dataType = "uint16";
        config.points.push_back(point);
        MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
        store.reset(new MemoryPointStore(config.memoryStore));
        router.addStore(sharedMemoryName, *store);
        router.addRoutesFromDeviceConfigs({config}, sharedMemoryName);
        daemon.reset(new GatewayDaemon(config, *store,
            [](const DeviceConfig&, MemoryPointStore&) { return std::unique_ptr<ICollector>(new IdleCollector()); },
            [this](const DeviceConfig&, MemoryPointStore&) {
                return std::unique_ptr<ICommandExecutor>(new ScriptedExecutor(calls));
            },
            {}));
        MqttConfig mqtt;
        mqtt.enabled = true;
        mqtt.clientId = "GW";
        mqtt.topicMachineCode = "GW";
        mqtt.commandReplyTopic = "edge/command/reply";
        MqttDriverConfig driver;
        driver.enabled = true;
        driver.sharedMemoryName = sharedMemoryName;
        driver.emsControlModeFile = config.memoryStore.emsControlModeFile;
        driver.priorityControlLeaseFile = config.mqttDriver.priorityControlLeaseFile;
        driver.powerControlOwnershipFile = config.mqttDriver.powerControlOwnershipFile;
        driver.scanIntervalMs = 100;
        driver.fullUploadIntervalMs = 0;
        driver.publishFullOnStart = false;
        driver.commandRateMaxPerWindow = 0;
        service.reset(new MqttDriverService(mqtt, driver, {config}, router, publisher));
        resultWaitMs = effectiveControlResultWaitMs(driver, {config});
    }
    ~Gateway() {
        service.reset();
        daemon.reset();
        store.reset();
    }
    // Delivers one command and returns the replies it produced in this scan.
    std::vector<MqttCommandReply> send(const std::string& cmdId, std::int64_t now) {
        MqttIncomingMessage message;
        message.type = MqttIncomingType::CommandRequest;
        message.payload = "{\"cmdId\":\"" + cmdId + "\",\"machineCode\":\"GW\",\"index\":1001,\"value\":7}";
        publisher->incoming.push_back(message);
        return scan(now);
    }
    std::vector<MqttCommandReply> scan(std::int64_t now) {
        const auto before = publisher->replies.size();
        service->runScanOnce(now);
        return std::vector<MqttCommandReply>(publisher->replies.begin() + static_cast<std::ptrdiff_t>(before),
                                             publisher->replies.end());
    }
    Optional<WritebackResultRecord> ledger(const std::string& cmdId) const {
        return ControlDedupStore(config.memoryStore.controlDedupPath).result("GW", "METER", cmdId);
    }
};

constexpr std::int64_t kT0 = 1770000045000LL;

void requireReply(const MqttCommandReply& reply, const Kind& kind, const std::string& what) {
    require(reply.success == kind.success && reply.outcome == kind.outcome && reply.stage == kind.stage &&
            reply.message == kind.message && reply.isFinal,
            what + " " + kind.name + ": got success=" + std::to_string(reply.success) + " outcome=" + reply.outcome +
                " stage=" + reply.stage + " final=" + std::to_string(reply.isFinal) + " message=" + reply.message);
}

void requireRecord(const Optional<WritebackResultRecord>& record, const Kind& kind, const std::string& what) {
    require(record && record->success == kind.success && writebackOutcome(*record) == kind.outcome &&
            record->stage == kind.stage && record->message == kind.message &&
            record->feedbackMismatch == kind.feedbackMismatch &&
            record->outcomeUnknown == (kind.outcome == "unknown"),
            what + " " + kind.name + (record ? ": got stage=" + record->stage + " message=" + record->message : ": missing"));
}

std::vector<MqttCommandReply> single(const std::vector<MqttCommandReply>& replies, const std::string& what) {
    require(replies.size() == 1, what + ": expected one reply, got " + std::to_string(replies.size()));
    return replies;
}

void firstReplayAndLate(const std::string& directory) {
    Gateway gateway(directory, "control_outcome_consistency_" + std::to_string(getpid()));
    // Shared-memory receipts: stage and flags agree whichever one a writer set (a writer that predates
    // the flag bits sets only the stage; success byte bits 0x04 unknown, 0x08 feedback mismatch).
    {
        WritebackResultRecord stageOnly;
        stageOnly.cmdId = "SHM_STAGE_ONLY";
        stageOnly.index = 1001;
        stageOnly.stage = "writeback-timeout";
        gateway.store->recordWritebackResult(stageOnly);
        const auto timeout = gateway.store->getWritebackResult("SHM_STAGE_ONLY", 1001);
        require(timeout && timeout->outcomeUnknown && !timeout->feedbackMismatch &&
                std::string(writebackOutcome(*timeout)) == "unknown", "stage-only timeout receipt reads as unknown");
        WritebackResultRecord flagOnly;
        flagOnly.cmdId = "SHM_FLAG_ONLY";
        flagOnly.index = 1001;
        flagOnly.stage = "writeback-failed";
        flagOnly.feedbackMismatch = true;
        gateway.store->recordWritebackResult(flagOnly);
        const auto mismatch = gateway.store->getWritebackResult("SHM_FLAG_ONLY", 1001);
        require(mismatch && mismatch->outcomeUnknown && mismatch->feedbackMismatch &&
                mismatch->stage == "writeback-mismatch", "mismatch flag forces the mismatch cause");
    }
    for (const auto& kind : kinds()) {
        // First reply: the daemon executes before the result deadline.
        const auto id = kind.name + "_FIRST";
        require(gateway.send(id, kT0).empty(), "accepted command waits for its result");
        gateway.daemon->processWritebackOnce(kT0 + 10);
        const auto first = single(gateway.scan(kT0 + 20), "first " + kind.name).front();
        requireReply(first, kind, "first reply");
        requireRecord(gateway.ledger(id), kind, "ledger record");
        requireRecord(gateway.store->getWritebackResult(id, 1001), kind, "shared-memory receipt");
        // Same cmdId again: the ledger replays the same result; nothing executes.
        const auto calls = gateway.calls;
        requireReply(single(gateway.send(id, kT0 + 30), "replay " + kind.name).front(), kind, "replay");
        gateway.daemon->processWritebackOnce(kT0 + 40);
        require(gateway.calls == calls, "replay of " + kind.name + " must not execute again");

        // Late: the result arrives after the "unknown" wait-timeout reply; the final reply supersedes it.
        const auto late = kind.name + "_LATE";
        gateway.send(late, kT0);
        const auto timeout = single(gateway.scan(kT0 + gateway.resultWaitMs), "timeout " + kind.name).front();
        require(!timeout.success && timeout.outcome == "unknown" && !timeout.isFinal &&
                timeout.stage == "writeback-timeout", "wait timeout is a non-final unknown");
        gateway.daemon->processWritebackOnce(kT0 + gateway.resultWaitMs + 10);
        requireReply(single(gateway.scan(kT0 + gateway.resultWaitMs + 20), "late " + kind.name).front(), kind,
                     "late final reply");
        require(gateway.scan(kT0 + 4LL * gateway.resultWaitMs).empty(), "nothing after the final reply");
        requireReply(single(gateway.send(late, kT0 + 5LL * gateway.resultWaitMs), "late replay").front(), kind,
                     "replay after late result");
    }
}

// Process 1: execute every kind, record the final replies, then die in the middle of a physical write.
int writerProcess(const std::string& directory) {
    Gateway gateway(directory, "control_outcome_restart_a_" + std::to_string(getppid()));
    std::ofstream out((directory + "/replies.txt").c_str());
    for (const auto& kind : kinds()) {
        const auto id = kind.name + "_RESTART";
        gateway.send(id, kT0);
        gateway.daemon->processWritebackOnce(kT0 + 10);
        const auto reply = single(gateway.scan(kT0 + 20), "writer " + kind.name).front();
        requireReply(reply, kind, "writer reply");
        out << reply.cmdId << '\t' << reply.outcome << '\t' << reply.stage << '\t' << reply.message << '\n';
    }
    out.close();
    gateway.send("CRASH_RESTART", kT0);
    gateway.daemon->processWritebackOnce(kT0 + 10);  // _exit(42) inside the write
    return 1;
}

// Process 2 (after a "reboot": new shared memory): replays come from the ledger only.
int replayProcess(const std::string& directory) {
    Gateway gateway(directory, "control_outcome_restart_b_" + std::to_string(getppid()));
    std::ifstream in((directory + "/replies.txt").c_str());
    std::string line;
    std::size_t checked = 0;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string cmdId, outcome, stage, message;
        std::getline(fields, cmdId, '\t');
        std::getline(fields, outcome, '\t');
        std::getline(fields, stage, '\t');
        std::getline(fields, message);
        const auto& kind = kindOf(cmdId);
        require(outcome == kind.outcome && stage == kind.stage && message == kind.message, "writer recorded " + cmdId);
        requireReply(single(gateway.send(cmdId, kT0 + 1000), "restart replay " + cmdId).front(), kind,
                     "replay after process restart");
        requireRecord(gateway.ledger(cmdId), kind, "ledger after restart");
        ++checked;
    }
    require(checked == kinds().size(), "every kind replayed after restart");

    // The writer died during the physical write: the reservation has no recorded result.
    require(!gateway.ledger("CRASH_RESTART"), "an interrupted dispatch has no final ledger record");
    const auto unconfirmed = single(gateway.send("CRASH_RESTART", kT0 + 2000), "crash replay").front();
    require(!unconfirmed.success && unconfirmed.outcome == "unknown" && !unconfirmed.isFinal &&
            unconfirmed.stage == "writeback-unconfirmed", "interrupted dispatch replays as non-final unconfirmed");
    gateway.daemon->processWritebackOnce(kT0 + 2010);
    require(gateway.calls == 0, "nothing is re-executed after a restart");
    const auto closing = single(gateway.scan(kT0 + 2000 + gateway.resultWaitMs), "crash horizon").front();
    require(!closing.success && closing.outcome == "unknown" && closing.isFinal &&
            closing.stage == "writeback-unconfirmed", "the watch ends with a final unknown that keeps the cause");
    return 0;
}

int runChild(const std::string& self, const std::string& phase, const std::string& directory) {
    const pid_t pid = fork();
    require(pid >= 0, "fork failed");
    if (pid == 0) {
        execl(self.c_str(), self.c_str(), phase.c_str(), directory.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    require(waitpid(pid, &status, 0) == pid, "waitpid failed");
    require(WIFEXITED(status), phase + " process did not exit normally");
    return WEXITSTATUS(status);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3) {
            const std::string phase = argv[1];
            if (phase == "--writer") return writerProcess(argv[2]);
            if (phase == "--replay") return replayProcess(argv[2]);
            return 2;
        }
        {
            dedup_test::Directory dir;
            firstReplayAndLate(dir.path.string());
        }
        dedup_test::Directory dir;
        const std::string self = "/proc/self/exe";
        char path[4096] = {};
        const auto length = readlink(self.c_str(), path, sizeof(path) - 1);
        require(length > 0, "cannot resolve own executable");
        const std::string executable(path, static_cast<std::size_t>(length));
        require(runChild(executable, "--writer", dir.path.string()) == 42,
                "writer process must die inside the physical write");
        require(runChild(executable, "--replay", dir.path.string()) == 0, "replay process failed");
        MemoryPointStore::cleanupOrphanedSegment("control_outcome_restart_a_" + std::to_string(getpid()));
        MemoryPointStore::cleanupOrphanedSegment("control_outcome_restart_b_" + std::to_string(getpid()));
        MemoryPointStore::cleanupOrphanedSegment("control_outcome_consistency_" + std::to_string(getpid()));
        std::cout << "control_outcome_consistency_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "control_outcome_consistency_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
