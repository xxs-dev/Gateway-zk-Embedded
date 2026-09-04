#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "edge_gateway/builtin_mqtt_driver_publisher.hpp"
#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/mqtt_driver_service.hpp"
#include "edge_gateway/mqtt_forwarder_service.hpp"
#include "edge_gateway/point_store_router.hpp"
#include "edge_gateway/power_control_ownership.hpp"

#ifndef _WIN32
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#endif

namespace {

using namespace edge_gateway;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class CapturingMqttDriverPublisher : public IMqttDriverPublisher {
public:
    void publishFullSnapshot(
        const std::string& topic,
        const std::vector<StoredPointValue>& values,
        const std::string& jsonFormat
    ) override {
        fullSnapshotTopics.push_back(topic);
        fullSnapshotCounts.push_back(values.size());
        fullSnapshotFormats.push_back(jsonFormat);
        std::vector<std::uint32_t> indexes;
        indexes.reserve(values.size());
        for (const auto& value : values) {
            indexes.push_back(value.index);
        }
        fullSnapshotIndexes.push_back(std::move(indexes));
        std::vector<double> pointValues;
        pointValues.reserve(values.size());
        for (const auto& value : values) {
            pointValues.push_back(value.value);
        }
        fullSnapshotValues.push_back(std::move(pointValues));
    }

    void publishAlarm(
        const std::string&,
        std::uint32_t,
        const StoredPointValue&,
        const std::string&,
        bool
    ) override {
        alarmCount += 1;
    }

    void publishOnDemand(
        const std::string& topic,
        const std::vector<StoredPointValue>& values,
        const std::string&
    ) override {
        onDemandTopics.push_back(topic);
        onDemandCounts.push_back(values.size());
    }

    void publishChangeEvent(
        const std::string&,
        const StoredPointValue&
    ) override {
        changeCount += 1;
    }

    void publishCommandReply(
        const std::string&,
        const MqttCommandReply&
    ) override {
        commandReplyCount += 1;
    }

    void publishOtaReply(
        const std::string&,
        const OtaReply&
    ) override {
        otaReplyCount += 1;
    }

    void publishOtaStatus(
        const std::string&,
        const OtaStatus&
    ) override {
        otaStatusCount += 1;
    }

    void publishJsonMessage(
        const std::string& topic,
        const std::string& payload
    ) override {
        jsonTopics.push_back(topic);
        jsonPayloads.push_back(payload);
        jsonCount += 1;
    }

    std::vector<MqttIncomingMessage> pollIncoming(int timeoutMs) override {
        pollTimeouts.push_back(timeoutMs);
        auto messages = incoming;
        incoming.clear();
        return messages;
    }

    std::vector<MqttIncomingMessage> incoming;
    std::vector<std::string> fullSnapshotTopics;
    std::vector<std::size_t> fullSnapshotCounts;
    std::vector<std::string> fullSnapshotFormats;
    std::vector<std::vector<std::uint32_t>> fullSnapshotIndexes;
    std::vector<std::vector<double>> fullSnapshotValues;
    std::vector<std::string> onDemandTopics;
    std::vector<std::size_t> onDemandCounts;
    std::vector<std::string> jsonTopics;
    std::vector<std::string> jsonPayloads;
    std::vector<int> pollTimeouts;
    int alarmCount = 0;
    int changeCount = 0;
    int commandReplyCount = 0;
    int otaReplyCount = 0;
    int otaStatusCount = 0;
    int jsonCount = 0;
};

class FlakyMqttDriverPublisher : public CapturingMqttDriverPublisher {
public:
    void publishFullSnapshot(
        const std::string& topic,
        const std::vector<StoredPointValue>& values,
        const std::string& jsonFormat
    ) override {
        ++fullAttempts;
        if (fullFailuresRemaining > 0) {
            --fullFailuresRemaining;
            throw std::runtime_error("simulated full publish failure");
        }
        CapturingMqttDriverPublisher::publishFullSnapshot(topic, values, jsonFormat);
    }

    void probeConnection() override {
        ++probeAttempts;
        if (probeFailuresRemaining > 0) {
            --probeFailuresRemaining;
            throw std::runtime_error("simulated probe failure");
        }
    }

    int fullAttempts = 0;
    int fullFailuresRemaining = 0;
    int probeAttempts = 0;
    int probeFailuresRemaining = 0;
};

PointDefinition makePoint(
    std::uint32_t index,
    const std::string& pointCode,
    bool fullUpload = true
) {
    PointDefinition point;
    point.index = index;
    point.pointCode = pointCode;
    point.enabled = true;
    point.fullUpload = fullUpload;
    point.read.enable = true;
    point.read.dataType = "uint16";
    point.read.intervalMs = 500;
    return point;
}

MqttIncomingMessage realtimeRequest(const std::string& payload) {
    MqttIncomingMessage message;
    message.type = MqttIncomingType::RealtimeRequest;
    message.payload = payload;
    return message;
}

std::string readTextFile(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    );
}

PointDefinition makeWritablePoint(std::uint32_t index, const std::string& pointCode) {
    auto point = makePoint(index, pointCode);
    point.write.enable = true;
    return point;
}

MqttIncomingMessage controlCommand(const std::string& payload, bool retained = false) {
    MqttIncomingMessage message;
    message.type = MqttIncomingType::CommandRequest;
    message.topic = "third/cmd";
    message.payload = payload;
    message.retained = retained;
    return message;
}

struct ControlTestEnvironment {
    std::string sharedMemoryName = "mqtt_forwarder_control";
    std::string ownershipFile;
    std::unique_ptr<MemoryPointStore> store;
    PointStoreRouter router;
    std::shared_ptr<CapturingMqttDriverPublisher> publisher;
    MqttForwardConfig forward;
    std::unique_ptr<MqttForwarderService> service;

    explicit ControlTestEnvironment(
        bool seedStaleLease = false,
        std::size_t maxPendingWrites = 4095,
        std::string staleSessionId = std::string()
    ) {
        MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
#ifdef _WIN32
        ownershipFile = std::tmpnam(nullptr);
#else
        ownershipFile = "/tmp/mqtt_forwarder_control_owner_test.json";
#endif
        std::remove(ownershipFile.c_str());
        std::remove((ownershipFile + ".lock").c_str());

        MemoryStoreConfig storeConfig;
        storeConfig.sharedMemoryName = sharedMemoryName;
        storeConfig.maxPendingWrites = maxPendingWrites;
        store.reset(new MemoryPointStore(storeConfig));
        router.addStore(sharedMemoryName, *store);

        DeviceConfig deviceConfig;
        deviceConfig.machineCode = "GW_TEST";
        deviceConfig.memoryStore.sharedMemoryName = sharedMemoryName;
        LogicalDeviceConfig meter;
        meter.meterCode = "PCS_TEST";
        meter.points.push_back(makeWritablePoint(4001, "P_A_SET"));
        meter.points.push_back(makeWritablePoint(4002, "P_B_SET"));
        deviceConfig.meters.push_back(meter);
        router.addRoutesFromDeviceConfigs({deviceConfig}, sharedMemoryName);

        forward.enabled = true;
        forward.broker = "tcp://127.0.0.1:1883";
        forward.fullTelemetryTopic = "third/full";
        forward.pointIndexes = {4001, 4002};
        forward.intervalMs = 60000;
        forward.control.enabled = true;
        forward.control.commandTopic = "third/cmd";
        forward.control.replyTopic = "third/reply";
        forward.control.ownershipFile = ownershipFile;
        forward.control.scope = "pcs-power";
        forward.control.sessionId = "third-party";
        forward.control.leaseTtlMs = 1000;
        forward.control.pollIntervalMs = 100;
        forward.control.minTargetKw = -10.0;
        forward.control.maxTargetKw = 10.0;
        forward.control.ownershipIndexes = {4001, 4002};
        MqttForwardControlTargetConfig firstTarget;
        firstTarget.index = 4001;
        firstTarget.scale = 10.0;
        firstTarget.offset = 1.0;
        MqttForwardControlTargetConfig secondTarget;
        secondTarget.index = 4002;
        secondTarget.scale = 2.0;
        secondTarget.offset = -0.5;
        forward.control.targets = {firstTarget, secondTarget};

        publisher = std::make_shared<CapturingMqttDriverPublisher>();
        if (seedStaleLease) {
            PowerControlOwnership staleOwner(ownershipFile, "mqtt-forwarder");
            const auto seeded = staleOwner.acquireOrRenew(
                forward.control.scope,
                staleSessionId.empty() ? forward.control.sessionId : staleSessionId,
                forward.control.ownershipIndexes,
                "STALE_BEFORE_RESTART",
                4102444800000LL,
                forward.control.leaseTtlMs
            );
            require(seeded.accepted, "failed to seed stale remote lease");
        }
        service.reset(new MqttForwarderService(forward, router, publisher));
    }

    ~ControlTestEnvironment() {
        service.reset();
        store.reset();
        MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
        std::remove(ownershipFile.c_str());
        std::remove((ownershipFile + ".lock").c_str());
    }

    void runCommand(const std::string& payload, std::int64_t nowMs, bool retained = false) {
        publisher->incoming.push_back(controlCommand(payload, retained));
        service->runOnce(nowMs);
    }

    bool submitLocal(double value, std::int64_t nowMs) {
        PendingWriteCommand local;
        local.cmdId = "LOCAL_" + std::to_string(nowMs);
        local.index = 4001;
        local.value = value;
        local.source = "compute-engine";
        local.ts = nowMs;
        local.acceptedAt = nowMs;
        return router.submitWriteCommand(local).accepted;
    }

    void requireNoWrites(const std::string& message) {
        require(store->drainPendingWriteCommands().empty(), message);
    }

    void requireRejectedReply(const std::string& message) const {
        require(!publisher->jsonPayloads.empty(), message);
        require(publisher->jsonTopics.back() == "third/reply", message);
        require(
            publisher->jsonPayloads.back().find("\"accepted\":false") != std::string::npos,
            message
        );
    }
};

void requireMappedWrites(
    ControlTestEnvironment& environment,
    double firstValue,
    double secondValue,
    const std::string& message
) {
    const auto writes = environment.store->drainPendingWriteCommands();
    require(writes.size() == 2, message);
    require(writes[0].index == 4001 && writes[0].value == firstValue, message);
    require(writes[1].index == 4002 && writes[1].value == secondValue, message);
    require(writes[0].source == "mqtt-forwarder" && writes[1].source == "mqtt-forwarder", message);
    require(!writes[0].highPriority && !writes[1].highPriority, message);
    require(writes[0].controlGeneration != 0, message);
    require(writes[0].controlGeneration == writes[1].controlGeneration, message);
}

void testControlMqttConfigUsesOnlyExactControlTopics() {
    MqttForwardConfig forward;
    forward.enabled = true;
    forward.broker = "tcp://127.0.0.1:1883";
    forward.fullTelemetryTopic = "third/full";
    forward.control.enabled = true;
    forward.control.commandTopic = "third/cmd";
    forward.control.replyTopic = "third/reply";

    const auto txOnly = MqttForwarderService::makeTxOnlyMqttConfig(forward, "GW_TEST");
    require(txOnly.commandRequestTopic.empty(), "TX-only forwarder must keep command topic empty");
    require(txOnly.commandRequestTopicMachineScoped, "TX-only defaults must remain machine-scoped");
    require(txOnly.fullTelemetryTopicMachineScoped, "legacy forward full topic must stay machine-scoped");

    const auto control = MqttForwarderService::makeMqttConfig(forward, "GW_TEST");
    require(control.commandRequestTopic == "third/cmd", "control command topic should be exact");
    require(control.commandReplyTopic == "third/reply", "control reply topic should be exact");
    require(!control.commandRequestTopicMachineScoped, "control request must not append machineCode");
    require(!control.commandReplyTopicMachineScoped, "control reply must not append machineCode");
    require(control.realtimeRequestTopic.empty(), "forwarder must not subscribe to realtime requests");
    require(control.otaRequestTopic.empty(), "forwarder must not subscribe to OTA requests");
    require(control.configApplyRequestTopic.empty(), "forwarder must not subscribe to config requests");

    forward.fullTelemetryTopicMachineScoped = false;
    const auto exactFull = MqttForwarderService::makeMqttConfig(forward, "GW_TEST");
    require(!exactFull.fullTelemetryTopicMachineScoped, "forwarder should preserve an exact full topic setting");
}

void testControlTakeoverDuplicateReleaseAndExpiry() {
    ControlTestEnvironment environment;
    const std::int64_t startedAt = 1770001000000LL;
    environment.runCommand(
        "{\"type\":\"1\",\"target\":\"2.5\",\"id\":\"A\"}",
        startedAt
    );
    requireMappedWrites(environment, 26.0, 4.5, "type=1 should queue mapped writes");
    require(!environment.submitLocal(9.0, startedAt + 1), "active takeover must block local EMS");

    PowerControlOwnership observer(environment.ownershipFile, "observer");
    const auto firstLease = observer.active(startedAt + 1);
    require(static_cast<bool>(firstLease), "type=1 should acquire ownership");
    const auto firstExpiry = firstLease->expireAtMs;

    environment.runCommand(
        "{\"type\":\"1\",\"target\":\"2.5\",\"id\":\"A\"}",
        startedAt + 100
    );
    environment.requireNoWrites("immediate duplicate must not enqueue");
    const auto duplicateLease = observer.active(startedAt + 101);
    require(duplicateLease && duplicateLease->expireAtMs == firstExpiry,
        "duplicate command must not renew the lease");

    environment.runCommand(
        "{\"type\":\"1\",\"target\":-2,\"id\":\"B\"}",
        startedAt + 200
    );
    requireMappedWrites(environment, -19.0, -4.5, "unique command should renew and enqueue");
    const auto renewedLease = observer.active(startedAt + 201);
    require(renewedLease && renewedLease->expireAtMs > firstExpiry,
        "unique command should renew the lease");

    environment.runCommand(
        "{\"type\":\"1\",\"target\":1,\"id\":\"A\"}",
        startedAt + 300
    );
    environment.requireNoWrites("nonconsecutive duplicate must not enqueue");
    const auto repeatedLease = observer.active(startedAt + 301);
    require(repeatedLease && repeatedLease->expireAtMs == renewedLease->expireAtMs,
        "nonconsecutive duplicate must not renew the lease");

    environment.runCommand("{\"type\":\"0\",\"target\":999,\"id\":\"LOCAL\"}", startedAt + 400);
    environment.requireNoWrites("type=0 must ignore target and not enqueue");
    require(environment.submitLocal(1.0, startedAt + 401), "type=0 should restore local control");
    environment.store->drainPendingWriteCommands();

    PowerControlOwnership platformOwner(environment.ownershipFile, "own-platform");
    require(
        platformOwner.acquire("pcs-power", "platform-session", {4001, 4002}, startedAt + 450, 1000),
        "own platform should acquire control for conflict test"
    );
    environment.runCommand("{\"type\":0,\"id\":\"WRONG_RELEASE\"}", startedAt + 451);
    environment.requireNoWrites("third-party type=0 must not enqueue during another ownership");
    environment.requireRejectedReply("third-party type=0 must not release another controller");
    const auto platformLease = platformOwner.active(startedAt + 452);
    require(platformLease && platformLease->owner == "own-platform",
        "third-party type=0 must preserve another controller's lease");
    platformOwner.release("platform-session");

    environment.runCommand(
        "{\"type\":1,\"target\":1,\"id\":\"C\"}",
        startedAt + 500
    );
    requireMappedWrites(environment, 11.0, 1.5, "numeric type should be accepted");
    require(environment.submitLocal(1.0, startedAt + 1501), "lease expiry should restore local control");
}

void testControlRestartStartsInLocalMode() {
    ControlTestEnvironment environment(true);
    PowerControlOwnership observer(environment.ownershipFile, "observer");
    require(!observer.active(4102444800001LL), "forwarder restart must clear its stale remote lease");
    require(
        environment.submitLocal(1.0, 4102444800001LL),
        "local EMS must be writable immediately after forwarder restart"
    );
}

void testControlRestartClearsPreviousSessionLease() {
    ControlTestEnvironment environment(true, 4095, "previous-config-session");
    PowerControlOwnership observer(environment.ownershipFile, "observer");
    require(
        !observer.active(4102444800001LL),
        "forwarder restart must clear a stale lease from its previous configured session"
    );
    require(
        environment.submitLocal(1.0, 4102444800001LL),
        "local EMS must resume immediately when the configured third-party session changes"
    );
}

void testFailedSubmitReceiptRejectsDuplicates() {
    ControlTestEnvironment environment(false, 1);
    const std::int64_t startedAt = 1770001800000LL;
    environment.runCommand(
        "{\"type\":1,\"target\":2,\"id\":\"QUEUE_FULL\"}",
        startedAt
    );
    environment.requireNoWrites("an atomic two-point write must fail against a one-slot queue");
    environment.requireRejectedReply("failed queue submission must return accepted=false");

    PowerControlOwnership observer(environment.ownershipFile, "observer");
    const auto receipt = observer.lookupReceipt("QUEUE_FULL");
    require(receipt.found && !receipt.accepted, "failed submission must persist a rejected receipt");
    require(!observer.active(startedAt + 1), "failed submission must release remote control");

    environment.runCommand(
        "{\"type\":1,\"target\":2,\"id\":\"QUEUE_FULL\"}",
        startedAt + 10
    );
    environment.requireNoWrites("duplicate failed command must not retry the device write");
    environment.requireRejectedReply("duplicate failed command must remain rejected");
    require(
        environment.publisher->jsonPayloads.back().find("\"duplicate\":true") != std::string::npos,
        "duplicate failed command must be labelled duplicate"
    );
    require(!observer.active(startedAt + 11), "duplicate failed command must not reacquire control");
}

void testAcceptedReceiptsSurviveRestart() {
    ControlTestEnvironment environment;
    const std::int64_t startedAt = 1770001900000LL;
    environment.runCommand("{\"type\":1,\"target\":1,\"id\":\"OLD_A\"}", startedAt);
    requireMappedWrites(environment, 11.0, 1.5, "first command should be accepted before restart");
    environment.runCommand("{\"type\":1,\"target\":2,\"id\":\"NEW_B\"}", startedAt + 10);
    requireMappedWrites(environment, 21.0, 3.5, "second command should be accepted before restart");

    environment.service.reset();
    environment.service.reset(
        new MqttForwarderService(environment.forward, environment.router, environment.publisher)
    );
    PowerControlOwnership observer(environment.ownershipFile, "observer");
    require(!observer.active(startedAt + 20), "restart must return control to local mode");
    const auto oldReceipt = observer.lookupReceipt("OLD_A");
    const auto newReceipt = observer.lookupReceipt("NEW_B");
    require(oldReceipt.found && oldReceipt.accepted, "older accepted receipt must survive restart");
    require(newReceipt.found && newReceipt.accepted, "latest accepted receipt must survive restart");

    environment.runCommand("{\"type\":1,\"target\":3,\"id\":\"OLD_A\"}", startedAt + 30);
    environment.requireNoWrites("accepted duplicate after restart must not enqueue");
    require(
        environment.publisher->jsonPayloads.back().find("\"accepted\":true") != std::string::npos &&
        environment.publisher->jsonPayloads.back().find("\"duplicate\":true") != std::string::npos,
        "accepted duplicate after restart must return its persisted result"
    );
    require(!observer.active(startedAt + 31), "accepted duplicate after restart must not reacquire control");
}

void testControlRejectsUnsafePayloads() {
    ControlTestEnvironment environment;
    std::int64_t nowMs = 1770002000000LL;
    auto reject = [&](const std::string& payload, bool retained, const std::string& message) {
        environment.runCommand(payload, nowMs, retained);
        environment.requireNoWrites(message);
        environment.requireRejectedReply(message);
        nowMs += 10;
    };
    reject("{\"type\":\"1\",\"target\":1,\"id\":\"RET\"}", true,
        "retained command must be rejected");
    reject("{", false, "malformed JSON must be rejected");
    reject("{\"type\":1,\"target\":1,\"id\":\"DUP\",\"id\":\"DUP\"}", false,
        "duplicate JSON field must be rejected");
    reject("{\"type\":1,\"target\":1,\"id\":\"SRC\",\"source\":\"mqtt\"}", false,
        "payload source override must be rejected");
    reject("{\"type\":1,\"target\":1,\"id\":\"HP\",\"highPriority\":true}", false,
        "payload priority override must be rejected");
    reject("{\"type\":1,\"target\":11,\"id\":\"RANGE\"}", false,
        "out-of-range target must be rejected");
    reject("{\"type\":1,\"target\":\"0x10\",\"id\":\"HEX\"}", false,
        "hexadecimal target strings must be rejected");
    reject("{\"type\":1,\"target\":\"+1\",\"id\":\"PLUS\"}", false,
        "leading-plus target strings must be rejected");

    const std::string maxId(63, 'A');
    environment.runCommand(
        "{\"type\":1,\"target\":1,\"id\":\"" + maxId + "\"}",
        nowMs
    );
    const auto maxIdWrites = environment.store->drainPendingWriteCommands();
    require(maxIdWrites.size() == 2, "63-byte control id must remain accepted");
    require(
        maxIdWrites[0].cmdId == maxId && maxIdWrites[1].cmdId == maxId,
        "accepted control id must survive the shared-memory round trip without truncation"
    );
    nowMs += 10;

    const std::string oversizedId(64, 'B');
    reject(
        "{\"type\":1,\"target\":1,\"id\":\"" + oversizedId + "\"}",
        false,
        "64-byte control id must be rejected before shared-memory submission"
    );
}

void testTxOnlyConfigClearsControlTopicsAndNeverPolls() {
    MqttForwardConfig forward;
    forward.enabled = true;
    forward.broker = "tcp://127.0.0.1:1883";
    forward.fullTelemetryTopic = "third/full";
    forward.pointIndexes = {2002};
    forward.payloadFormat = "object";
    forward.qos = 1;
    forward.intervalMs = 1000;

    const auto tx = MqttForwarderService::makeTxOnlyMqttConfig(forward, "GW_TEST");
    require(tx.clientId == "GW_TEST-forward", "forwarder clientId should default to machineCode-forward");
    require(tx.fullTelemetryTopic == "third/full", "forwarder should keep the configured full topic");
    require(tx.offlineBufferEnabled == false, "forwarder must disable disk replay");
    require(tx.commandRequestTopic.empty(), "forwarder must not carry command request topics");
    require(tx.realtimeRequestTopic.empty(), "forwarder must not carry realtime request topics");
    require(tx.alarmTopic.empty(), "forwarder must not carry alarm topics");
    require(tx.changeEventTopic.empty(), "forwarder must not carry change topics");
    require(tx.statusTopic.empty(), "forwarder must not carry status topics");
    require(tx.otaRequestTopic.empty(), "forwarder must not carry ota topics");
    require(tx.telemetryTopic.empty(), "forwarder must not reuse the main telemetry topic");

    const std::string shmName = "mqtt_forwarder_tx_only";
    MemoryPointStore::cleanupOrphanedSegment(shmName);
    MemoryStoreConfig storeConfig;
    storeConfig.sharedMemoryName = shmName;
    MemoryPointStore store(storeConfig);
    PointStoreRouter router;
    router.addStore(shmName, store);

    DeviceConfig deviceConfig;
    deviceConfig.machineCode = "GW_TEST";
    deviceConfig.memoryStore.sharedMemoryName = shmName;
    LogicalDeviceConfig meter;
    meter.meterCode = "METER_1";
    meter.points.push_back(makePoint(2001, "P_FWD"));
    meter.points.push_back(makePoint(2002, "P_NOT_FULL", false));
    deviceConfig.meters.push_back(meter);
    router.addRoutesFromDeviceConfigs({deviceConfig}, shmName);

    PointValue value;
    value.index = 2001;
    value.value = 9.5;
    value.ts = 1770000100000LL;
    value.expireAt = 1770000700000LL;
    require(router.putLatestByIndex(value).accepted, "failed to seed forwarder point");
    PointValue excludedValue = value;
    excludedValue.index = 2002;
    excludedValue.value = 10.5;
    require(router.putLatestByIndex(excludedValue).accepted, "failed to seed non-full point");

    auto publisher = std::make_shared<CapturingMqttDriverPublisher>();
    MqttForwarderService service(forward, router, publisher);
    service.runOnce(1770000100000LL);
    require(publisher->pollTimeouts.empty(), "forwarder must never call pollIncoming");
    require(publisher->onDemandCounts.empty(), "forwarder must not publish realtime");
    require(publisher->alarmCount == 0, "forwarder must not publish alarms");
    require(publisher->changeCount == 0, "forwarder must not publish change events");
    require(publisher->commandReplyCount == 0, "forwarder must not publish command replies");
    require(publisher->jsonCount == 0, "forwarder must not publish status JSON");
    require(publisher->fullSnapshotTopics.size() == 1, "forwarder should publish one full snapshot");
    require(publisher->fullSnapshotTopics.front() == "third/full", "forwarder should use mqttForward.fullTelemetryTopic");
    require(publisher->fullSnapshotCounts.front() == 1, "forwarder should publish only its configured point set");
    require(
        publisher->fullSnapshotIndexes.front() == std::vector<std::uint32_t>{2002},
        "forwarder pointIndexes must be independent from point.fullUpload"
    );
    require(publisher->fullSnapshotFormats.front() == "object", "forwarder should use mqttForward.payloadFormat");

    service.runOnce(1770000100500LL);
    require(publisher->fullSnapshotTopics.size() == 1, "forwarder should wait for intervalMs");
    service.runOnce(1770000101000LL);
    require(publisher->fullSnapshotTopics.size() == 2, "forwarder should publish the next full snapshot");
    require(publisher->pollTimeouts.empty(), "later cycles must still skip pollIncoming");
    MemoryPointStore::cleanupOrphanedSegment(shmName);
}

void testLegacyPayloadUsesIndependentMappings() {
    MqttForwardConfig forward;
    forward.enabled = true;
    forward.broker = "tcp://127.0.0.1:1883";
    forward.fullTelemetryTopic = "ky/peidian";
    forward.pointIndexes = {2051, 2052};
    forward.payloadFormat = "legacy";
    forward.legacyTelemetryMappedOnly = true;
    forward.legacyTelemetryPointMappings = {
        LegacyTelemetryPointMapping{2051, "OLD_METER", "OLD_A"},
        LegacyTelemetryPointMapping{2052, "OLD_METER", "OLD_B"}
    };
    forward.intervalMs = 1000;

    const std::string shmName = "mqtt_forwarder_legacy";
    MemoryPointStore::cleanupOrphanedSegment(shmName);
    MemoryStoreConfig storeConfig;
    storeConfig.sharedMemoryName = shmName;
    MemoryPointStore store(storeConfig);
    PointStoreRouter router;
    router.addStore(shmName, store);

    DeviceConfig deviceConfig;
    deviceConfig.machineCode = "GW_TEST";
    deviceConfig.memoryStore.sharedMemoryName = shmName;
    LogicalDeviceConfig meter;
    meter.meterCode = "NEW_METER";
    meter.points.push_back(makePoint(2051, "NEW_A"));
    meter.points.push_back(makePoint(2052, "NEW_B"));
    deviceConfig.meters.push_back(meter);
    router.addRoutesFromDeviceConfigs({deviceConfig}, shmName);

    PointValue value;
    value.index = 2051;
    value.value = 12.5;
    value.ts = 1770000150000LL;
    value.expireAt = 1770000750000LL;
    require(router.putLatestByIndex(value).accepted, "failed to seed legacy forwarding point");

    auto publisher = std::make_shared<CapturingMqttDriverPublisher>();
    MqttForwarderService service(forward, router, publisher);
    service.runOnce(1770000150000LL);

    require(publisher->fullSnapshotTopics.empty(), "legacy forwarding must not use the new snapshot encoder");
    require(publisher->jsonTopics == std::vector<std::string>{"ky/peidian"},
        "legacy forwarding should use the independent base topic");
    require(publisher->jsonPayloads.size() == 1, "legacy forwarding should publish one JSON payload");
    require(
        publisher->jsonPayloads.front() ==
            R"({"data":[{"meterid":"OLD_METER","metrics":[{"OLD_A":"12.5000","OLD_B":"0.0000"}]}],"msgid":1770000150000,"split":"false","timestamp":1770000150000})",
        "legacy forwarding must preserve old meter/point mappings and missing-value behavior"
    );
    require(publisher->pollTimeouts.empty(), "legacy forwarding must remain TX-only");
    MemoryPointStore::cleanupOrphanedSegment(shmName);
}

void testUnavailablePointStoreFailsClosed() {
    MqttForwardConfig forward;
    forward.enabled = true;
    forward.broker = "tcp://127.0.0.1:1883";
    forward.fullTelemetryTopic = "third/full";
    forward.pointIndexes = {2101};
    forward.intervalMs = 1000;

    const std::string shmName = "mqtt_forwarder_missing_store";
    PointStoreRouter router;
    DeviceConfig deviceConfig;
    deviceConfig.machineCode = "GW_TEST";
    deviceConfig.memoryStore.sharedMemoryName = shmName;
    LogicalDeviceConfig meter;
    meter.meterCode = "METER_1";
    meter.points.push_back(makePoint(2101, "P_MISSING"));
    deviceConfig.meters.push_back(meter);
    router.addRoutesFromDeviceConfigs({deviceConfig}, shmName);

#ifdef _WIN32
    const std::string healthFile = std::tmpnam(nullptr);
#else
    const std::string healthFile = "/tmp/mqtt_forwarder_missing_store_test.json";
#endif
    auto publisher = std::make_shared<CapturingMqttDriverPublisher>();
    MqttForwarderService service(forward, router, publisher, healthFile);
    service.runOnce(1770000450000LL);
    require(publisher->fullSnapshotTopics.empty(), "missing PointStore must not publish a partial full snapshot");

    std::ifstream input(healthFile.c_str());
    require(static_cast<bool>(input), "missing PointStore should write an unhealthy status");
    const std::string payload((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    require(payload.find("\"healthy\":false") != std::string::npos, "missing PointStore should fail closed");
    require(payload.find("unavailable") != std::string::npos, "health status should identify the unavailable store");
    input.close();
    std::remove(healthFile.c_str());
}

void testDisabledForwarderPublishesNothing() {
    MqttForwardConfig forward;
    forward.enabled = false;
    forward.broker = "tcp://127.0.0.1:1883";
    forward.fullTelemetryTopic = "third/full";
    forward.intervalMs = 1000;

    const std::string shmName = "mqtt_forwarder_disabled";
    MemoryPointStore::cleanupOrphanedSegment(shmName);
    MemoryStoreConfig storeConfig;
    storeConfig.sharedMemoryName = shmName;
    MemoryPointStore store(storeConfig);
    PointStoreRouter router;
    router.addStore(shmName, store);

    auto publisher = std::make_shared<CapturingMqttDriverPublisher>();
    MqttForwarderService service(forward, router, publisher);
    service.start();
    require(!service.isRunning(), "disabled forwarder must not start a publish loop");
    service.runOnce(1770000400000LL);
    require(publisher->fullSnapshotTopics.empty(), "disabled forwarder must not publish");
    require(publisher->pollTimeouts.empty(), "disabled forwarder must not poll incoming");
    MemoryPointStore::cleanupOrphanedSegment(shmName);
}

void testUnroutedPointIndexIsRejected() {
    MqttForwardConfig forward;
    forward.enabled = true;
    forward.broker = "tcp://127.0.0.1:1883";
    forward.fullTelemetryTopic = "third/full";
    forward.pointIndexes = {2301};

    PointStoreRouter router;
    auto publisher = std::make_shared<CapturingMqttDriverPublisher>();
    try {
        MqttForwarderService service(forward, router, publisher);
        throw std::runtime_error("unrouted mqttForward point index was accepted");
    } catch (const std::invalid_argument& ex) {
        require(
            std::string(ex.what()).find("unrouted index 2301") != std::string::npos,
            "unrouted point rejection should identify the configured index"
        );
    }
}

class ThrowingMqttDriverPublisher : public CapturingMqttDriverPublisher {
public:
    void publishFullSnapshot(
        const std::string&,
        const std::vector<StoredPointValue>&,
        const std::string&
    ) override {
        throw std::runtime_error("third-party broker unavailable");
    }
};

void testForwarderFailureWritesHealthAndDoesNotPoll() {
    MqttForwardConfig forward;
    forward.enabled = true;
    forward.broker = "tcp://127.0.0.1:1883";
    forward.fullTelemetryTopic = "third/full";
    forward.pointIndexes = {2201};
    forward.intervalMs = 1000;

    const std::string shmName = "mqtt_forwarder_health";
    MemoryPointStore::cleanupOrphanedSegment(shmName);
    MemoryStoreConfig storeConfig;
    storeConfig.sharedMemoryName = shmName;
    MemoryPointStore store(storeConfig);
    PointStoreRouter router;
    router.addStore(shmName, store);
    DeviceConfig deviceConfig;
    deviceConfig.machineCode = "GW_TEST";
    deviceConfig.memoryStore.sharedMemoryName = shmName;
    LogicalDeviceConfig meter;
    meter.meterCode = "METER_1";
    meter.points.push_back(makePoint(2201, "P_HEALTH"));
    deviceConfig.meters.push_back(meter);
    router.addRoutesFromDeviceConfigs({deviceConfig}, shmName);

#ifdef _WIN32
    const std::string healthFile = std::tmpnam(nullptr);
#else
    const std::string healthFile = "/tmp/mqtt_forwarder_health_test.json";
#endif
    auto publisher = std::make_shared<ThrowingMqttDriverPublisher>();
    MqttForwarderService service(forward, router, publisher, healthFile);
    service.runOnce(1770000500000LL);
    require(publisher->pollTimeouts.empty(), "failed full publish must still skip pollIncoming");

    std::ifstream input(healthFile.c_str());
    require(static_cast<bool>(input), "forwarder should write a health file after failure");
    const std::string payload((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    require(payload.find("\"healthy\":false") != std::string::npos, "third-party failure should record unhealthy");
    require(payload.find("third-party broker unavailable") != std::string::npos, "health file should keep the publish error");
    input.close();
    std::remove(healthFile.c_str());
    MemoryPointStore::cleanupOrphanedSegment(shmName);
}

void testPrimaryFullConfigInheritsConnectionAndUsesIndependentClientId() {
    MqttConfig primary;
    primary.protocolVersion = "mqtt5";
    primary.broker = "mqtts://edge.example.com:8883";
    primary.clientId = "PRIMARY_CLIENT";
    primary.username = "gateway";
    primary.password = "secret";
    primary.keepAliveSec = 23;
    primary.connectTimeoutMs = 4567;
    primary.maxPayloadBytes = 234567;
    primary.fullTelemetryTopic = "edge/full";
    primary.fullTelemetryTopicMachineScoped = false;
    primary.commandRequestTopic = "edge/cmd";
    primary.realtimeRequestTopic = "edge/realtime/request";
    primary.offlineBufferEnabled = true;
    primary.tls.enabled = true;
    primary.tls.caFile = "/tmp/ca.crt";

    MqttFullUploadWorkerConfig worker;
    worker.clientIdSuffix = "-full-worker";
    const auto isolated = MqttForwarderService::makePrimaryFullMqttConfig(
        primary,
        worker,
        "GW_TEST"
    );

    require(isolated.clientId == "GW_TEST-full-worker", "primary full clientId must be independent");
    require(isolated.broker == primary.broker, "primary full worker must inherit broker");
    require(isolated.keepAliveSec == primary.keepAliveSec, "primary full worker must inherit keepAlive");
    require(isolated.connectTimeoutMs == primary.connectTimeoutMs,
        "primary full worker must inherit connect timeout");
    require(isolated.maxPayloadBytes == primary.maxPayloadBytes,
        "primary full worker must preserve chunk sizing");
    require(isolated.tls.caFile == primary.tls.caFile, "primary full worker must inherit TLS");
    require(!isolated.offlineBufferEnabled, "primary full worker must not replay historical snapshots");
    require(isolated.commandRequestTopic.empty(), "primary full worker must not subscribe to control");
    require(isolated.realtimeRequestTopic.empty(), "primary full worker must not subscribe to realtime");
}

void testPrimaryFullRetriesQuicklyAndPublishesLatestAfterRecovery() {
    const std::string shmName = "mqtt_primary_full_retry";
    MemoryPointStore::cleanupOrphanedSegment(shmName);
    MemoryStoreConfig storeConfig;
    storeConfig.sharedMemoryName = shmName;
    MemoryPointStore store(storeConfig);
    PointStoreRouter router;
    router.addStore(shmName, store);

    DeviceConfig deviceConfig;
    deviceConfig.machineCode = "GW_TEST";
    deviceConfig.memoryStore.sharedMemoryName = shmName;
    LogicalDeviceConfig meter;
    meter.meterCode = "METER_1";
    meter.points.push_back(makePoint(2401, "P_RETRY"));
    deviceConfig.meters.push_back(meter);
    router.addRoutesFromDeviceConfigs({deviceConfig}, shmName);

    PointValue value;
    value.index = 2401;
    value.value = 1.0;
    value.ts = 1770000600000LL;
    value.expireAt = 1770004200000LL;
    require(router.putLatestByIndex(value).accepted, "failed to seed primary full retry point");

    MqttForwardConfig forward;
    forward.enabled = true;
    forward.fullTelemetryTopic = "edge/full";
    forward.pointIndexes = {2401};
    forward.intervalMs = 30000;
    forward.retryMinMs = 500;
    forward.retryMaxMs = 2000;
    forward.healthHeartbeatMs = 1000;
    forward.healthLeaseTtlMs = 3000;
    forward.failOnStoreError = false;
    forward.primaryFullUpload = true;
    forward.primaryMachineCode = "GW_TEST";
    forward.primaryClientId = "GW_TEST-full";

#ifdef _WIN32
    const std::string healthFile = std::tmpnam(nullptr);
#else
    const std::string healthFile = "/tmp/mqtt_primary_full_retry_test.json";
#endif
    std::remove(healthFile.c_str());
    forward.publishLockFile = healthFile + ".lock";
    auto publisher = std::make_shared<FlakyMqttDriverPublisher>();
    publisher->fullFailuresRemaining = 2;
    MqttForwarderService service(forward, router, publisher, healthFile);
    const std::int64_t startedAt = 1770000600000LL;

    service.runOnce(startedAt);
    require(publisher->fullAttempts == 1, "primary full should attempt immediately");
    auto health = readTextFile(healthFile);
    require(health.find("\"state\":\"retrying\"") != std::string::npos,
        "failed primary full should release the lease");
    require(health.find("\"nextAttemptAtMs\":1770000600500") != std::string::npos,
        "first retry should be scheduled after 500ms");

    service.runOnce(startedAt + 499);
    require(publisher->fullAttempts == 1, "primary full must respect the short retry delay");
    service.runOnce(startedAt + 500);
    require(publisher->fullAttempts == 2, "primary full should retry after 500ms");
    service.runOnce(startedAt + 1499);
    require(publisher->fullAttempts == 2, "second retry should wait 1000ms");
    service.runOnce(startedAt + 1500);
    require(publisher->fullAttempts == 3, "primary full should recover without waiting 30 seconds");
    require(publisher->fullSnapshotValues.size() == 1,
        "only the successful latest snapshot should be retained");
    health = readTextFile(healthFile);
    require(health.find("\"state\":\"active\"") != std::string::npos,
        "successful primary full should acquire the lease");
    require(health.find("\"leaseUntilMs\":1770000604500") != std::string::npos,
        "active lease should use the configured failover timeout");

    const auto probeAttemptsBeforeHeartbeat = publisher->probeAttempts;
    publisher->probeFailuresRemaining = 1;
    service.runOnce(startedAt + 2500);
    require(publisher->probeAttempts == probeAttemptsBeforeHeartbeat + 1,
        "primary worker should probe between full uploads");
    health = readTextFile(healthFile);
    require(health.find("\"state\":\"retrying\"") != std::string::npos,
        "failed broker probe should release the lease immediately");

    value.value = 99.0;
    value.ts = startedAt + 3000;
    require(router.putLatestByIndex(value).accepted, "failed to update latest retry point");
    service.runOnce(startedAt + 2999);
    require(publisher->probeAttempts == probeAttemptsBeforeHeartbeat + 1,
        "probe retry must wait 500ms");
    service.runOnce(startedAt + 3000);
    require(publisher->probeAttempts == probeAttemptsBeforeHeartbeat + 2,
        "probe should retry after 500ms");
    require(publisher->fullAttempts == 4, "recovered probe should trigger an immediate current full");
    require(publisher->fullSnapshotValues.size() == 2,
        "recovery must add one current snapshot, not replay failed history");
    require(publisher->fullSnapshotValues.back() == std::vector<double>{99.0},
        "recovery must publish the latest PointStore value");

    std::remove(healthFile.c_str());
    std::remove(forward.publishLockFile.c_str());
    MemoryPointStore::cleanupOrphanedSegment(shmName);
}

void testPrimaryFullKeepsAvailableStoresWhenOneStoreFails() {
    const std::string availableName = "mqtt_primary_full_available";
    const std::string missingName = "mqtt_primary_full_missing";
    MemoryPointStore::cleanupOrphanedSegment(availableName);
    MemoryStoreConfig storeConfig;
    storeConfig.sharedMemoryName = availableName;
    MemoryPointStore store(storeConfig);
    PointStoreRouter router;
    router.addStore(availableName, store);

    PointStoreRoute availableRoute;
    availableRoute.index = 2501;
    availableRoute.sourceIndex = 2501;
    availableRoute.machineCode = "GW_TEST";
    availableRoute.meterCode = "METER_OK";
    availableRoute.pointCode = "P_OK";
    availableRoute.sharedMemoryName = availableName;
    router.addRoute(availableRoute);
    PointStoreRoute missingRoute = availableRoute;
    missingRoute.index = 2502;
    missingRoute.sourceIndex = 2502;
    missingRoute.meterCode = "METER_MISSING";
    missingRoute.pointCode = "P_MISSING";
    missingRoute.sharedMemoryName = missingName;
    router.addRoute(missingRoute);

    PointValue value;
    value.index = 2501;
    value.value = 12.5;
    value.ts = 1770000700000LL;
    value.expireAt = 1770004300000LL;
    require(router.putLatestByIndex(value).accepted, "failed to seed available primary point");

    MqttForwardConfig forward;
    forward.enabled = true;
    forward.fullTelemetryTopic = "edge/full";
    forward.pointIndexes = {2501, 2502};
    forward.failOnStoreError = false;
    forward.primaryFullUpload = true;
    forward.primaryMachineCode = "GW_TEST";
    forward.primaryClientId = "GW_TEST-full";
#ifdef _WIN32
    const std::string healthFile = std::tmpnam(nullptr);
#else
    const std::string healthFile = "/tmp/mqtt_primary_full_partial_test.json";
#endif
    std::remove(healthFile.c_str());
    forward.publishLockFile = healthFile + ".lock";
    auto publisher = std::make_shared<CapturingMqttDriverPublisher>();
    MqttForwarderService service(forward, router, publisher, healthFile);
    service.runOnce(1770000700000LL);

    require(publisher->fullSnapshotCounts == std::vector<std::size_t>{1},
        "primary full should preserve the main driver's best-effort store behavior");
    require(publisher->fullSnapshotIndexes.front() == std::vector<std::uint32_t>{2501},
        "primary full should publish available points when one store is unavailable");

    MqttForwardConfig blockedForward = forward;
#ifdef _WIN32
    const std::string blockedHealth = "Z:\\gateway-test-missing\\primary-full.json";
#else
    const std::string blockedHealth = "/proc/gateway-test-missing/primary-full.json";
#endif
    auto blockedPublisher = std::make_shared<CapturingMqttDriverPublisher>();
    MqttForwarderService blockedService(
        blockedForward,
        router,
        blockedPublisher,
        blockedHealth
    );
    blockedService.runOnce(1770000701000LL);
    require(blockedPublisher->fullSnapshotCounts.empty(),
        "primary full must not publish before its ownership claim is durable");

    std::remove(healthFile.c_str());
    std::remove(forward.publishLockFile.c_str());
    MemoryPointStore::cleanupOrphanedSegment(availableName);
}

void testRealtimeStopLeavesMainFullAndForwarderFullRunning() {
    const std::string shmName = "mqtt_forwarder_isolation";
    MemoryPointStore::cleanupOrphanedSegment(shmName);
    MemoryStoreConfig storeConfig;
    storeConfig.sharedMemoryName = shmName;
    MemoryPointStore store(storeConfig);
    PointStoreRouter router;
    DeviceConfig deviceConfig;
    deviceConfig.machineCode = "GW_TEST";
    deviceConfig.memoryStore.sharedMemoryName = shmName;
    LogicalDeviceConfig meter;
    meter.meterCode = "METER_1";
    meter.points.push_back(makePoint(3001, "P_1"));
    meter.points.push_back(makePoint(3002, "P_2", false));
    deviceConfig.meters.push_back(meter);
    router.addStore(shmName, store);
    router.addRoutesFromDeviceConfigs({deviceConfig}, shmName);

    PointValue value1;
    value1.index = 3001;
    value1.value = 1.0;
    value1.ts = 1770000200000LL;
    value1.expireAt = 1770000800000LL;
    require(router.putLatestByIndex(value1).accepted, "failed to seed point 3001");
    PointValue value2;
    value2.index = 3002;
    value2.value = 2.0;
    value2.ts = 1770000200000LL;
    value2.expireAt = 1770000800000LL;
    require(router.putLatestByIndex(value2).accepted, "failed to seed point 3002");

    MqttConfig mqttConfig;
    mqttConfig.enabled = true;
    mqttConfig.topicMachineCode = "GW_TEST";
    mqttConfig.telemetryTopic = "edge/telemetry";
    mqttConfig.realtimeTelemetryTopic = "edge/telemetry/realtime";
    mqttConfig.fullTelemetryTopic = "edge/telemetry/full";
    mqttConfig.realtimeRequestTopic = "edge/telemetry/realtime/request";

    MqttDriverConfig driverConfig;
    driverConfig.enabled = true;
    driverConfig.sharedMemoryName = shmName;
    driverConfig.scanIntervalMs = 100;
    driverConfig.fullUploadIntervalMs = 1000;
    driverConfig.publishFullOnStart = false;
    driverConfig.publishAllOnFull = false;
    driverConfig.fullUploadIndexes = {3001};
    driverConfig.fullUploadJsonFormat = "object";

    auto driverPublisher = std::make_shared<CapturingMqttDriverPublisher>();
    MqttDriverService driver(
        mqttConfig,
        driverConfig,
        {deviceConfig},
        router,
        driverPublisher
    );

    MqttForwardConfig forward;
    forward.enabled = true;
    forward.broker = "tcp://127.0.0.1:1884";
    forward.fullTelemetryTopic = "third/full";
    forward.pointIndexes = {3002};
    forward.payloadFormat = "compactArray";
    forward.intervalMs = 1000;
    auto forwardPublisher = std::make_shared<CapturingMqttDriverPublisher>();
    MqttForwarderService forwarder(forward, router, forwardPublisher);

    const std::int64_t startedAt = 1770000200000LL;
    driver.runScanOnce(startedAt);
    driverPublisher->incoming.push_back(realtimeRequest(
        "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"REALTIME_TARGET\",\"indexes\":[3001],\"intervalMs\":100,\"ttlSec\":30}"
    ));
    driver.runScanOnce(startedAt + 100);
    require(driverPublisher->onDemandCounts.size() == 1, "realtime session should start");
    require(driverPublisher->fullSnapshotCounts.empty(), "main full should wait for its interval");
    require(forwardPublisher->fullSnapshotCounts.empty(), "forwarder full should wait for its interval");

    driverPublisher->incoming.push_back(realtimeRequest(
        "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"REALTIME_TARGET\",\"action\":\"stop\"}"
    ));
    driver.runScanOnce(startedAt + 150);
    driver.runScanOnce(startedAt + 300);
    require(driverPublisher->onDemandCounts.size() == 1, "stopped realtime session should not publish again");

    driver.runScanOnce(startedAt + 1000);
    forwarder.runOnce(startedAt);
    require(driverPublisher->onDemandCounts.size() == 1, "main full must not create a realtime publication");
    require(driverPublisher->fullSnapshotCounts.size() == 1, "main full should continue after realtime stop");
    require(
        driverPublisher->fullSnapshotTopics.front() == mqttConfig.fullTelemetryTopic,
        "main full should stay on the owned broker topic"
    );
    require(
        driverPublisher->fullSnapshotIndexes.front() == std::vector<std::uint32_t>{3001},
        "main full should keep its own point selection"
    );
    require(driverPublisher->fullSnapshotFormats.front() == "object", "main full should keep its own payload format");
    require(forwardPublisher->pollTimeouts.empty(), "forwarder must stay TX-only after realtime stop");
    require(forwardPublisher->onDemandCounts.empty(), "forwarder must not publish realtime");
    require(forwardPublisher->fullSnapshotCounts.size() == 1, "third-party full should continue after realtime stop");
    require(forwardPublisher->fullSnapshotTopics.front() == "third/full", "third-party full should use mqttForward topic");
    require(forwardPublisher->fullSnapshotCounts.front() == 1, "third-party full should publish its configured point set");
    require(
        forwardPublisher->fullSnapshotIndexes.front() == std::vector<std::uint32_t>{3002},
        "main full point settings must not affect third-party forwarding"
    );
    require(
        forwardPublisher->fullSnapshotFormats.front() == "compactArray",
        "main full payload format must not affect third-party forwarding"
    );

    MemoryPointStore::cleanupOrphanedSegment(shmName);
}

#ifndef _WIN32
std::vector<std::uint8_t> readMqttPacket(int fd) {
    std::vector<std::uint8_t> packet;
    std::uint8_t byte = 0;
    if (recv(fd, &byte, 1, MSG_WAITALL) != 1) {
        throw std::runtime_error("tx-only broker failed to read packet type");
    }
    packet.push_back(byte);
    std::size_t multiplier = 1;
    std::size_t remaining = 0;
    do {
        if (recv(fd, &byte, 1, MSG_WAITALL) != 1) {
            throw std::runtime_error("tx-only broker failed to read remaining length");
        }
        packet.push_back(byte);
        remaining += (byte & 0x7F) * multiplier;
        multiplier *= 128;
    } while ((byte & 0x80) != 0);
    const auto headerSize = packet.size();
    packet.resize(headerSize + remaining);
    if (remaining > 0 &&
        recv(fd, packet.data() + headerSize, remaining, MSG_WAITALL) != static_cast<ssize_t>(remaining)) {
        throw std::runtime_error("tx-only broker failed to read packet body");
    }
    return packet;
}

std::string packetTopic(const std::vector<std::uint8_t>& packet) {
    std::size_t cursor = 1;
    while (cursor < packet.size() && (packet[cursor++] & 0x80) != 0) {
    }
    if (cursor + 2 > packet.size()) {
        return {};
    }
    const auto length = (static_cast<std::size_t>(packet[cursor]) << 8) | packet[cursor + 1];
    cursor += 2;
    if (cursor + length > packet.size()) {
        return {};
    }
    return std::string(
        reinterpret_cast<const char*>(packet.data() + cursor),
        reinterpret_cast<const char*>(packet.data() + cursor + length)
    );
}

std::string connectClientId(const std::vector<std::uint8_t>& packet) {
    std::size_t cursor = 1;
    while (cursor < packet.size() && (packet[cursor++] & 0x80) != 0) {
    }
    if (cursor + 2 > packet.size()) {
        return {};
    }
    const auto protocolLength =
        (static_cast<std::size_t>(packet[cursor]) << 8) | packet[cursor + 1];
    cursor += 2 + protocolLength;
    if (cursor + 4 > packet.size()) {
        return {};
    }
    const auto protocolLevel = packet[cursor++];
    cursor += 3;
    if (protocolLevel == 5) {
        std::size_t multiplier = 1;
        std::size_t propertyLength = 0;
        std::uint8_t encoded = 0;
        do {
            if (cursor >= packet.size()) {
                return {};
            }
            encoded = packet[cursor++];
            propertyLength += (encoded & 0x7F) * multiplier;
            multiplier *= 128;
        } while ((encoded & 0x80) != 0);
        cursor += propertyLength;
    }
    if (cursor + 2 > packet.size()) {
        return {};
    }
    const auto clientIdLength =
        (static_cast<std::size_t>(packet[cursor]) << 8) | packet[cursor + 1];
    cursor += 2;
    if (cursor + clientIdLength > packet.size()) {
        return {};
    }
    return std::string(
        reinterpret_cast<const char*>(packet.data() + cursor),
        reinterpret_cast<const char*>(packet.data() + cursor + clientIdLength)
    );
}

class TxOnlyMqttBroker {
public:
    TxOnlyMqttBroker() {
        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        require(listenFd_ >= 0, "tx-only broker socket failed");
        int opt = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        require(bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "tx-only broker bind failed");
        socklen_t len = sizeof(addr);
        require(getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0, "tx-only broker getsockname failed");
        port_ = ntohs(addr.sin_port);
        require(listen(listenFd_, 4) == 0, "tx-only broker listen failed");
        thread_ = std::thread([this]() { run(); });
    }

    ~TxOnlyMqttBroker() {
        stop_.store(true);
        if (listenFd_ >= 0) {
            shutdown(listenFd_, SHUT_RDWR);
            close(listenFd_);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    int port() const {
        return port_;
    }

    std::vector<std::uint8_t> packetTypes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return packetTypes_;
    }

    std::vector<std::string> publishTopics() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return publishTopics_;
    }

    bool sawSubscribe() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sawSubscribe_;
    }

    bool sawRxClientId() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sawRxClientId_;
    }

    bool sawForwardClientId() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sawForwardClientId_;
    }

private:
    void run() {
        while (!stop_.load()) {
            const int fd = accept(listenFd_, nullptr, nullptr);
            if (fd < 0) {
                continue;
            }
            try {
                handleClient(fd);
            } catch (...) {
            }
            close(fd);
        }
    }

    void handleClient(int fd) {
        while (!stop_.load()) {
            const auto packet = readMqttPacket(fd);
            if (packet.empty()) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                packetTypes_.push_back(packet[0]);
            }
            const auto type = packet[0] & 0xF0;
            if (type == 0x10) {
                const auto clientId = connectClientId(packet);
                std::lock_guard<std::mutex> lock(mutex_);
                if (clientId.size() >= 3 && clientId.compare(clientId.size() - 3, 3, "-rx") == 0) {
                    sawRxClientId_ = true;
                }
                if (clientId == "GW_TEST-forward") {
                    sawForwardClientId_ = true;
                }
                const std::uint8_t connAck[] = {0x20, 0x02, 0x00, 0x00};
                send(fd, connAck, sizeof(connAck), 0);
                continue;
            }
            if (packet[0] == 0x82) {
                std::lock_guard<std::mutex> lock(mutex_);
                sawSubscribe_ = true;
                const std::uint8_t subAck[] = {
                    0x90,
                    0x03,
                    packet.size() > 3 ? packet[2] : static_cast<std::uint8_t>(0),
                    packet.size() > 4 ? packet[3] : static_cast<std::uint8_t>(0),
                    0x00
                };
                send(fd, subAck, sizeof(subAck), 0);
                continue;
            }
            if (type == 0x30) {
                const auto topic = packetTopic(packet);
                std::lock_guard<std::mutex> lock(mutex_);
                publishTopics_.push_back(topic);
                const auto qos = (packet[0] >> 1) & 0x03;
                if (qos == 1) {
                    std::size_t cursor = 1;
                    while (cursor < packet.size() && (packet[cursor++] & 0x80) != 0) {
                    }
                    if (cursor + 2 <= packet.size()) {
                        const auto topicLength = (static_cast<std::size_t>(packet[cursor]) << 8) | packet[cursor + 1];
                        cursor += 2 + topicLength;
                        if (cursor + 2 <= packet.size()) {
                            const std::uint8_t pubAck[] = {0x40, 0x02, packet[cursor], packet[cursor + 1]};
                            send(fd, pubAck, sizeof(pubAck), 0);
                        }
                    }
                }
                continue;
            }
            if (type == 0xC0) {
                const std::uint8_t pingResp[] = {0xD0, 0x00};
                send(fd, pingResp, sizeof(pingResp), 0);
                continue;
            }
            if (type == 0xE0) {
                return;
            }
        }
    }

    int listenFd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_ {false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<std::uint8_t> packetTypes_;
    std::vector<std::string> publishTopics_;
    bool sawSubscribe_ = false;
    bool sawRxClientId_ = false;
    bool sawForwardClientId_ = false;
};

void testTxOnlyPublisherNeverSubscribes() {
    TxOnlyMqttBroker broker;
    MqttForwardConfig forward;
    forward.enabled = true;
    forward.broker = "tcp://127.0.0.1:" + std::to_string(broker.port());
    forward.fullTelemetryTopic = "third/full";
    forward.qos = 1;
    forward.intervalMs = 1000;
    auto tx = MqttForwarderService::makeTxOnlyMqttConfig(forward, "GW_TEST");
    BuiltinMqttDriverPublisher publisher(tx, MqttPublisherMode::TxOnly);

    StoredPointValue value;
    value.index = 4001;
    value.value = 3.25;
    value.ts = 1770000300000LL;
    publisher.publishFullSnapshot(forward.fullTelemetryTopic, {value}, "compactArray");
    publisher.publishJsonMessage(forward.fullTelemetryTopic, "{\"legacy\":true}");
    publisher.probeConnection();
    const auto incoming = publisher.pollIncoming(20);
    require(incoming.empty(), "TX-only pollIncoming must not create an RX subscription");
    publisher.publishAlarm("edge/alarm", 4001, value, "high", true);
    publisher.publishChangeEvent("edge/event/change", value);
    publisher.publishOnDemand("edge/telemetry/realtime", {value}, "compactArray");

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(!broker.sawSubscribe(), "TX-only publisher must never send SUBSCRIBE");
    require(!broker.sawRxClientId(), "TX-only publisher must never open an RX connection");
    require(broker.sawForwardClientId(), "TX-only publisher should use the independent forward clientId");
    require(!broker.publishTopics().empty(), "TX-only publisher should publish the full snapshot");
    const auto publishedTopics = broker.publishTopics();
    require(
        std::all_of(
            publishedTopics.begin(),
            publishedTopics.end(),
            [](const std::string& topic) { return topic.find("third/full") == 0; }
        ),
        "TX-only publisher must only publish to the configured full topic"
    );
}
#endif

}  // namespace

int main() {
    try {
        std::cerr << "running tx-only service test" << std::endl;
        testTxOnlyConfigClearsControlTopicsAndNeverPolls();
        std::cerr << "running disabled forwarder test" << std::endl;
        testDisabledForwarderPublishesNothing();
        std::cerr << "running legacy payload test" << std::endl;
        testLegacyPayloadUsesIndependentMappings();
        std::cerr << "running unrouted point test" << std::endl;
        testUnroutedPointIndexIsRejected();
        std::cerr << "running unavailable PointStore test" << std::endl;
        testUnavailablePointStoreFailsClosed();
        std::cerr << "running forwarder health test" << std::endl;
        testForwarderFailureWritesHealthAndDoesNotPoll();
        std::cerr << "running primary full mqtt config test" << std::endl;
        testPrimaryFullConfigInheritsConnectionAndUsesIndependentClientId();
        std::cerr << "running primary full retry and recovery test" << std::endl;
        testPrimaryFullRetriesQuicklyAndPublishesLatestAfterRecovery();
        std::cerr << "running primary full partial store test" << std::endl;
        testPrimaryFullKeepsAvailableStoresWhenOneStoreFails();
        std::cerr << "running realtime/full isolation test" << std::endl;
        testRealtimeStopLeavesMainFullAndForwarderFullRunning();
        std::cerr << "running third-party control mqtt config test" << std::endl;
        testControlMqttConfigUsesOnlyExactControlTopics();
        std::cerr << "running third-party takeover lifecycle test" << std::endl;
        testControlTakeoverDuplicateReleaseAndExpiry();
        std::cerr << "running third-party unsafe payload rejection test" << std::endl;
        testControlRejectsUnsafePayloads();
        std::cerr << "running third-party restart fail-safe test" << std::endl;
        testControlRestartStartsInLocalMode();
        std::cerr << "running third-party changed-session restart fail-safe test" << std::endl;
        testControlRestartClearsPreviousSessionLease();
        std::cerr << "running failed submission receipt test" << std::endl;
        testFailedSubmitReceiptRejectsDuplicates();
        std::cerr << "running persistent accepted receipt test" << std::endl;
        testAcceptedReceiptsSurviveRestart();
#ifndef _WIN32
        testTxOnlyPublisherNeverSubscribes();
#endif
        std::cout << "mqtt_forwarder_service_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "mqtt_forwarder_service_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
