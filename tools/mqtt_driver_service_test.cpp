#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/mqtt_driver_service.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace {

using namespace edge_gateway;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class CapturingMqttDriverPublisher : public IMqttDriverPublisher {
public:
    struct RealtimePublication {
        std::string topic;
        std::string sessionId;
        std::vector<StoredPointValue> values;
    };

    void publishFullSnapshot(
        const std::string& topic,
        const std::vector<StoredPointValue>& values,
        const std::string&
    ) override {
        fullSnapshotTopics.push_back(topic);
        fullSnapshotCounts.push_back(values.size());
    }

    void publishAlarm(
        const std::string&,
        std::uint32_t,
        const StoredPointValue&,
        const std::string&,
        bool
    ) override {
    }

    void publishOnDemand(
        const std::string& topic,
        const std::vector<StoredPointValue>& values,
        const std::string&
    ) override {
        onDemandTopics.push_back(topic);
        onDemandCounts.push_back(values.size());
        realtimePublications.push_back(RealtimePublication{topic, std::string(), values});
    }

    void publishRealtime(
        const std::string& topic,
        const std::vector<StoredPointValue>& values,
        const std::string&,
        const std::string& sessionId
    ) override {
        onDemandTopics.push_back(topic);
        onDemandCounts.push_back(values.size());
        realtimePublications.push_back(RealtimePublication{topic, sessionId, values});
    }

    void publishChangeEvent(
        const std::string&,
        const StoredPointValue&
    ) override {
    }

    void publishCommandReply(
        const std::string&,
        const MqttCommandReply& reply
    ) override {
        commandReplyCount += 1;
        commandReplies.push_back(reply);
    }

    void publishOtaReply(
        const std::string&,
        const OtaReply&
    ) override {
    }

    void publishOtaStatus(
        const std::string&,
        const OtaStatus&
    ) override {
    }

    void publishJsonMessage(
        const std::string&,
        const std::string& payload
    ) override {
        statusPayloads.push_back(payload);
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
    std::vector<std::string> onDemandTopics;
    std::vector<std::size_t> onDemandCounts;
    std::vector<RealtimePublication> realtimePublications;
    std::vector<std::string> statusPayloads;
    std::vector<int> pollTimeouts;
    int commandReplyCount = 0;
    std::vector<MqttCommandReply> commandReplies;
};

struct ServiceFixture {
    std::string shmName;
    MemoryStoreConfig storeConfig;
    std::unique_ptr<MemoryPointStore> store;
    PointStoreRouter router;
    DeviceConfig deviceConfig;
    MqttConfig mqttConfig;
    MqttDriverConfig driverConfig;
    std::shared_ptr<CapturingMqttDriverPublisher> publisher;
    std::unique_ptr<MqttDriverService> service;
};

PointDefinition makePoint(std::uint32_t index, const std::string& pointCode) {
    PointDefinition point;
    point.index = index;
    point.pointCode = pointCode;
    point.enabled = true;
    point.fullUpload = true;
    point.read.enable = true;
    point.read.dataType = "uint16";
    point.read.intervalMs = 500;
    return point;
}

ServiceFixture makeFixture(
    const std::string& suffix,
    int fullUploadIntervalMs,
    bool firstPointWritable = false,
    bool includeSecondMeter = false
) {
    ServiceFixture fixture;
    fixture.shmName = "mqtt_driver_service_test_" + suffix;
    MemoryPointStore::cleanupOrphanedSegment(fixture.shmName);
    fixture.storeConfig.sharedMemoryName = fixture.shmName;
    fixture.store.reset(new MemoryPointStore(fixture.storeConfig));

    fixture.deviceConfig.machineCode = "GW_TEST";
    fixture.deviceConfig.memoryStore.sharedMemoryName = fixture.shmName;
    LogicalDeviceConfig meter;
    meter.meterCode = "METER_1";
    meter.points.push_back(makePoint(1001, "P_1"));
    meter.points.push_back(makePoint(1002, "P_2"));
    meter.points[0].write.enable = firstPointWritable;
    fixture.deviceConfig.meters.push_back(meter);
    if (includeSecondMeter) {
        LogicalDeviceConfig secondMeter;
        secondMeter.meterCode = "METER_2";
        secondMeter.points.push_back(makePoint(2001, "P_3"));
        fixture.deviceConfig.meters.push_back(secondMeter);
    }

    fixture.router.addStore(fixture.shmName, *fixture.store);
    fixture.router.addRoutesFromDeviceConfigs({fixture.deviceConfig}, fixture.shmName);

    PointValue value1;
    value1.index = 1001;
    value1.value = 12.3;
    value1.ts = 1770000000000LL;
    value1.expireAt = 1770000600000LL;
    require(fixture.router.putLatestByIndex(value1).accepted, "failed to seed point 1001");

    PointValue value2;
    value2.index = 1002;
    value2.value = 45.6;
    value2.ts = 1770000000000LL;
    value2.expireAt = 1770000600000LL;
    require(fixture.router.putLatestByIndex(value2).accepted, "failed to seed point 1002");

    if (includeSecondMeter) {
        PointValue value3;
        value3.index = 2001;
        value3.value = 78.9;
        value3.ts = 1770000000000LL;
        value3.expireAt = 1770000600000LL;
        require(fixture.router.putLatestByIndex(value3).accepted, "failed to seed point 2001");
    }

    fixture.mqttConfig.enabled = true;
    fixture.mqttConfig.topicMachineCode = "GW_TEST";
    fixture.mqttConfig.telemetryTopic = "edge/telemetry";
    fixture.mqttConfig.realtimeTelemetryTopic = "edge/telemetry/realtime";
    fixture.mqttConfig.fullTelemetryTopic = "edge/telemetry/full";
    fixture.mqttConfig.realtimeRequestTopic = "edge/telemetry/realtime/request";
    fixture.mqttConfig.statusTopic = "edge/status";

    fixture.driverConfig.enabled = true;
    fixture.driverConfig.sharedMemoryName = fixture.shmName;
    fixture.driverConfig.scanIntervalMs = 100;
    fixture.driverConfig.fullUploadIntervalMs = fullUploadIntervalMs;
    fixture.driverConfig.publishFullOnStart = false;
    fixture.driverConfig.publishAllOnFull = false;
    fixture.driverConfig.fullUploadIndexes = {1001, 1002};
    if (includeSecondMeter) {
        fixture.driverConfig.fullUploadIndexes.push_back(2001);
    }

    fixture.publisher.reset(new CapturingMqttDriverPublisher());
    fixture.service.reset(new MqttDriverService(
        fixture.mqttConfig,
        fixture.driverConfig,
        {fixture.deviceConfig},
        fixture.router,
        fixture.publisher
    ));
    return fixture;
}

MqttIncomingMessage realtimeRequest(const std::string& payload) {
    MqttIncomingMessage message;
    message.type = MqttIncomingType::RealtimeRequest;
    message.payload = payload;
    return message;
}

MqttIncomingMessage commandRequest(const std::string& payload) {
    MqttIncomingMessage message;
    message.type = MqttIncomingType::CommandRequest;
    message.payload = payload;
    return message;
}

std::string readFile(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) {
        return {};
    }
    return std::string(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>()
    );
}

void cleanupFixture(ServiceFixture& fixture) {
    std::remove(fixture.driverConfig.priorityControlLeaseFile.c_str());
    fixture.service.reset();
    fixture.store.reset();
    MemoryPointStore::cleanupOrphanedSegment(fixture.shmName);
}

void testFullUploadOnlyWithoutRealtimeSession() {
    auto fixture = makeFixture("full_only", 1000);
    fixture.service->runScanOnce(1770000000000LL);
    fixture.service->runScanOnce(1770000000500LL);
    require(fixture.publisher->fullSnapshotCounts.empty(), "full snapshot should wait for full interval");
    require(fixture.publisher->onDemandCounts.empty(), "no realtime snapshot should publish without request");

    fixture.service->runScanOnce(1770000001000LL);
    require(fixture.publisher->fullSnapshotCounts.size() == 1, "full snapshot should publish when full interval is due");
    require(fixture.publisher->fullSnapshotCounts.back() == 2, "full snapshot should include configured full points");
    require(fixture.publisher->onDemandCounts.empty(), "full upload should not publish realtime demand messages");
    cleanupFixture(fixture);
}

void testOneShotRealtimeRequestDoesNotCreatePeriodicSession() {
    auto fixture = makeFixture("oneshot", 10000);
    fixture.service->runScanOnce(1770000010000LL);
    fixture.publisher->incoming.push_back(realtimeRequest("{\"machineCode\":\"GW_TEST\",\"meterCode\":\"METER_1\"}"));
    fixture.service->runScanOnce(1770000010100LL);
    require(fixture.publisher->onDemandCounts.size() == 1, "one-shot realtime request should publish immediately");
    require(fixture.publisher->onDemandCounts.back() == 2, "meter realtime request should include meter points");
    require(
        fixture.publisher->realtimePublications.back().sessionId.empty(),
        "legacy one-shot realtime response should remain unscoped"
    );

    fixture.service->runScanOnce(1770000010200LL);
    fixture.service->runScanOnce(1770000010500LL);
    require(fixture.publisher->onDemandCounts.size() == 1, "one-shot realtime request should not keep publishing");
    cleanupFixture(fixture);
}

void testRealtimeSessionPublishesUntilTtl() {
    auto fixture = makeFixture("session", 10000);
    fixture.service->runScanOnce(1770000020000LL);
    fixture.publisher->incoming.push_back(realtimeRequest(
        "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"S1\",\"meterCode\":\"METER_1\",\"intervalMs\":200,\"ttlSec\":1}"
    ));
    fixture.service->runScanOnce(1770000020100LL);
    require(fixture.publisher->onDemandCounts.size() == 1, "realtime session should publish immediately");

    fixture.service->runScanOnce(1770000020200LL);
    require(fixture.publisher->onDemandCounts.size() == 1, "session should respect requested interval");
    fixture.service->runScanOnce(1770000020300LL);
    require(fixture.publisher->onDemandCounts.size() == 2, "session should publish when interval is due");
    fixture.service->runScanOnce(1770000020500LL);
    require(fixture.publisher->onDemandCounts.size() == 3, "session should keep publishing while active");
    fixture.service->runScanOnce(1770000021200LL);
    require(fixture.publisher->onDemandCounts.size() == 3, "session should stop after ttl expires");
    cleanupFixture(fixture);
}

void testRealtimeSessionStopRequest() {
    auto fixture = makeFixture("stop", 10000);
    fixture.service->runScanOnce(1770000030000LL);
    fixture.publisher->incoming.push_back(realtimeRequest(
        "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"S1\",\"meterCode\":\"METER_1\",\"intervalMs\":100,\"ttlSec\":30}"
    ));
    fixture.service->runScanOnce(1770000030100LL);
    require(fixture.publisher->onDemandCounts.size() == 1, "session should publish immediately before stop");
    fixture.publisher->incoming.push_back(realtimeRequest(
        "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"S1\",\"action\":\"stop\"}"
    ));
    fixture.service->runScanOnce(1770000030150LL);
    fixture.service->runScanOnce(1770000030300LL);
    require(fixture.publisher->onDemandCounts.size() == 1, "stopped realtime session should not publish again");
    cleanupFixture(fixture);
}

void testRealtimeSessionUnsubscribeIsIsolatedFromOtherSessionsAndFullUpload() {
    auto fixture = makeFixture("unsubscribe_isolation", 1000);
    const std::int64_t startedAt = 1770000035000LL;
    require(
        fixture.mqttConfig.realtimeTelemetryTopic != fixture.mqttConfig.fullTelemetryTopic,
        "test fixture must use independent realtime and full topics"
    );
    fixture.service->runScanOnce(startedAt);

    fixture.publisher->incoming.push_back(realtimeRequest(
        "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"REALTIME_TARGET\",\"indexes\":[1001],\"intervalMs\":100,\"ttlSec\":30}"
    ));
    fixture.publisher->incoming.push_back(realtimeRequest(
        "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"REALTIME_SURVIVOR\",\"indexes\":[1001,1002],\"intervalMs\":100,\"ttlSec\":30}"
    ));
    fixture.service->runScanOnce(startedAt + 100);
    require(fixture.publisher->onDemandCounts.size() == 2, "both realtime sessions should publish immediately");
    require(
        std::count(fixture.publisher->onDemandCounts.begin(), fixture.publisher->onDemandCounts.end(), 1) == 1,
        "target realtime session should publish its configured point"
    );
    require(
        std::count(fixture.publisher->onDemandCounts.begin(), fixture.publisher->onDemandCounts.end(), 2) == 1,
        "surviving realtime session should publish its configured points"
    );
    require(fixture.publisher->fullSnapshotCounts.empty(), "full snapshot should not publish before interval");

    fixture.publisher->incoming.push_back(realtimeRequest(
        "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"UNKNOWN_SESSION\",\"action\":\"unsubscribe\"}"
    ));
    fixture.service->runScanOnce(startedAt + 150);
    fixture.service->runScanOnce(startedAt + 200);
    require(fixture.publisher->onDemandCounts.size() == 4, "unknown unsubscribe should leave both sessions active");
    require(
        std::count(fixture.publisher->onDemandCounts.begin(), fixture.publisher->onDemandCounts.end(), 1) == 2 &&
            std::count(fixture.publisher->onDemandCounts.begin(), fixture.publisher->onDemandCounts.end(), 2) == 2,
        "unknown unsubscribe should be a no-op for each realtime session"
    );

    fixture.publisher->incoming.push_back(realtimeRequest(
        "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"REALTIME_TARGET\",\"action\":\"unsubscribe\"}"
    ));
    fixture.service->runScanOnce(startedAt + 250);
    fixture.service->runScanOnce(startedAt + 300);
    require(fixture.publisher->onDemandCounts.size() == 5, "only the surviving session should keep publishing");
    require(
        std::count(fixture.publisher->onDemandCounts.begin(), fixture.publisher->onDemandCounts.end(), 1) == 2,
        "target realtime session should stop after unsubscribe"
    );
    require(
        std::count(fixture.publisher->onDemandCounts.begin(), fixture.publisher->onDemandCounts.end(), 2) == 3,
        "unsubscribing one session should not stop the other session"
    );

    fixture.service->runScanOnce(startedAt + 999);
    require(fixture.publisher->fullSnapshotCounts.empty(), "full snapshot should wait for fullUploadIntervalMs");
    fixture.service->runScanOnce(startedAt + 1000);
    require(fixture.publisher->onDemandCounts.size() == 6, "full upload should not create a realtime publication");
    require(fixture.publisher->fullSnapshotCounts.size() == 1, "full snapshot should continue after realtime unsubscribe");
    require(fixture.publisher->fullSnapshotCounts.back() == 2, "full snapshot after unsubscribe should include configured full points");
    require(
        fixture.publisher->fullSnapshotTopics.front() == fixture.mqttConfig.fullTelemetryTopic,
        "full snapshot should publish to fullTelemetryTopic"
    );
    require(
        fixture.publisher->onDemandTopics.size() == fixture.publisher->onDemandCounts.size(),
        "each realtime publication should capture its topic"
    );
    require(
        std::all_of(
            fixture.publisher->onDemandTopics.begin(),
            fixture.publisher->onDemandTopics.end(),
            [&](const std::string& topic) { return topic == fixture.mqttConfig.realtimeTelemetryTopic; }
        ),
        "realtime sessions should publish only to realtimeTelemetryTopic"
    );
    cleanupFixture(fixture);
}

void testRealtimeSessionsKeepMeterAndIndexSelectorsIsolated() {
    auto fixture = makeFixture("session_selector_isolation", 1000, false, true);
    const std::int64_t startedAt = 1770000037000LL;
    fixture.service->runScanOnce(startedAt);

    fixture.publisher->incoming.push_back(realtimeRequest(
        "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"SESSION_A\",\"meterCode\":\"METER_1\",\"intervalMs\":100,\"ttlSec\":30}"
    ));
    fixture.publisher->incoming.push_back(realtimeRequest(
        "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"SESSION_B\",\"indexes\":[2001],\"intervalMs\":100,\"ttlSec\":30}"
    ));
    fixture.service->runScanOnce(startedAt + 100);

    const auto countSession = [&](const std::string& sessionId) {
        return std::count_if(
            fixture.publisher->realtimePublications.begin(),
            fixture.publisher->realtimePublications.end(),
            [&](const CapturingMqttDriverPublisher::RealtimePublication& publication) {
                return publication.sessionId == sessionId;
            }
        );
    };
    const auto assertSessionValues = [&](const std::string& sessionId, const std::string& meterCode, std::uint32_t index) {
        for (const auto& publication : fixture.publisher->realtimePublications) {
            if (publication.sessionId != sessionId) {
                continue;
            }
            require(!publication.values.empty(), "session realtime publication should contain values");
            for (const auto& value : publication.values) {
                require(value.meterCode == meterCode, "session realtime publication leaked another meter");
                if (index != 0) {
                    require(value.index == index, "session realtime publication leaked another index");
                }
            }
        }
    };

    require(countSession("SESSION_A") == 1, "meter-scoped session should publish immediately with its sessionId");
    require(countSession("SESSION_B") == 1, "index-scoped session should publish immediately with its sessionId");
    assertSessionValues("SESSION_A", "METER_1", 0);
    assertSessionValues("SESSION_B", "METER_2", 2001);

    fixture.service->runScanOnce(startedAt + 200);
    require(countSession("SESSION_A") == 2, "meter-scoped session should keep its sessionId periodically");
    require(countSession("SESSION_B") == 2, "index-scoped session should keep its sessionId periodically");

    fixture.publisher->incoming.push_back(realtimeRequest(
        "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"SESSION_A\",\"action\":\"unsubscribe\"}"
    ));
    fixture.service->runScanOnce(startedAt + 250);
    fixture.service->runScanOnce(startedAt + 300);
    require(countSession("SESSION_A") == 2, "unsubscribed session A should stop publishing");
    require(countSession("SESSION_B") == 3, "session B should continue after session A unsubscribes");
    assertSessionValues("SESSION_A", "METER_1", 0);
    assertSessionValues("SESSION_B", "METER_2", 2001);

    fixture.service->runScanOnce(startedAt + 1000);
    require(countSession("SESSION_A") == 2, "full upload must not restart stopped session A");
    require(countSession("SESSION_B") == 4, "session B should continue alongside full upload");
    require(fixture.publisher->fullSnapshotCounts.size() == 1, "full upload should continue after session A stops");
    require(fixture.publisher->fullSnapshotCounts.front() == 3, "full upload configuration should remain unchanged");
    cleanupFixture(fixture);
}

void testCommandRequestDoesNotCreatePriorityControlLeaseByDefault() {
    auto fixture = makeFixture("command_no_lease", 10000, true);
    fixture.service.reset();
    fixture.driverConfig.priorityControlLeaseFile = "/tmp/mqtt_driver_service_command_no_lease.json";
    fixture.driverConfig.priorityControlLeaseTtlMs = 30000;
    fixture.driverConfig.controlResultWaitTimeoutMs = 0;
    std::remove(fixture.driverConfig.priorityControlLeaseFile.c_str());
    fixture.service.reset(new MqttDriverService(
        fixture.mqttConfig,
        fixture.driverConfig,
        {fixture.deviceConfig},
        fixture.router,
        fixture.publisher
    ));

    fixture.publisher->incoming.push_back(commandRequest(
        "{\"cmdId\":\"CMD_NORMAL\",\"machineCode\":\"GW_TEST\",\"index\":1001,\"value\":7}"
    ));
    fixture.service->runScanOnce(1770000040000LL);
    const auto lease = readFile(fixture.driverConfig.priorityControlLeaseFile);
    require(lease.empty(), "normal command should not create priority lease");

    const auto pending = fixture.store->peekPendingWriteCommands();
    require(pending.size() == 1, "normal command should enqueue pending write");
    require(pending.front().cmdId == "CMD_NORMAL", "normal pending write cmdId mismatch");
    require(!pending.front().highPriority, "normal pending write should not be high priority");
    cleanupFixture(fixture);
}

void testCommandWritebackWaitDoesNotBlockMqttScan() {
    auto fixture = makeFixture("command_async_writeback", 10000, true);
    fixture.service.reset();
    fixture.driverConfig.controlResultWaitTimeoutMs = 5000;
    fixture.service.reset(new MqttDriverService(
        fixture.mqttConfig,
        fixture.driverConfig,
        {fixture.deviceConfig},
        fixture.router,
        fixture.publisher
    ));

    fixture.publisher->incoming.push_back(commandRequest(
        "{\"cmdId\":\"CMD_ASYNC\",\"machineCode\":\"GW_TEST\",\"index\":1001,\"value\":7}"
    ));
    const auto startedAt = std::chrono::steady_clock::now();
    fixture.service->runScanOnce(1770000045000LL);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt
    ).count();

    require(elapsedMs < 1000, "pending writeback must not block the MQTT scan loop");
    require(fixture.publisher->commandReplies.empty(), "pending writeback must not publish a premature reply");

    WritebackResultRecord result;
    result.cmdId = "CMD_ASYNC";
    result.index = 1001;
    result.value = 7;
    result.success = true;
    result.message = "ok";
    result.stage = "writeback-completed";
    result.requestedAt = 1770000045000LL;
    result.acceptedAt = 1770000045000LL;
    result.startedAt = 1770000045020LL;
    result.completedAt = 1770000045040LL;
    result.queueDelayMs = 20;
    result.deviceWriteMs = 20;
    result.edgeElapsedMs = 40;
    result.totalElapsedMs = 40;
    fixture.store->recordWritebackResult(result);

    fixture.service->runScanOnce(1770000045050LL);
    require(fixture.publisher->commandReplies.size() == 1, "completed writeback should publish one final reply");
    require(fixture.publisher->commandReplies.front().success, "completed writeback reply should preserve success");
    require(
        fixture.publisher->commandReplies.front().stage == "writeback-completed",
        "completed writeback reply should preserve the driver stage"
    );
    cleanupFixture(fixture);
}

void testHighPriorityCommandRequestCreatesPriorityControlLease() {
    auto fixture = makeFixture("command_lease", 10000, true);
    fixture.service.reset();
    fixture.driverConfig.priorityControlLeaseFile = "/tmp/mqtt_driver_service_command_lease.json";
    fixture.driverConfig.priorityControlLeaseTtlMs = 30000;
    fixture.driverConfig.controlResultWaitTimeoutMs = 0;
    std::remove(fixture.driverConfig.priorityControlLeaseFile.c_str());
    fixture.service.reset(new MqttDriverService(
        fixture.mqttConfig,
        fixture.driverConfig,
        {fixture.deviceConfig},
        fixture.router,
        fixture.publisher
    ));

    fixture.publisher->incoming.push_back(commandRequest(
        "{\"cmdId\":\"CMD_LEASE\",\"machineCode\":\"GW_TEST\",\"index\":1001,\"value\":7,\"highPriority\":true}"
    ));
    fixture.service->runScanOnce(1770000040000LL);
    const auto lease = readFile(fixture.driverConfig.priorityControlLeaseFile);
    require(lease.find("\"cmdId\":\"CMD_LEASE\"") != std::string::npos, "command should create priority lease");
    require(lease.find("\"meterCode\":\"METER_1\"") != std::string::npos, "priority lease should include meter code");

    const auto pending = fixture.store->peekPendingWriteCommands();
    require(pending.size() == 1, "command should enqueue pending write");
    require(pending.front().cmdId == "CMD_LEASE", "pending write cmdId mismatch");
    require(pending.front().highPriority, "pending write should be marked high priority");
    cleanupFixture(fixture);
}

void testCommandRequestRejectedDuringActivePriorityControl() {
    auto fixture = makeFixture("command_blocked_by_priority", 10000, true);
    fixture.service.reset();
    fixture.driverConfig.priorityControlLeaseFile = "/tmp/mqtt_driver_service_command_blocked_by_priority.json";
    fixture.driverConfig.priorityControlLeaseTtlMs = 30000;
    fixture.driverConfig.controlResultWaitTimeoutMs = 0;
    std::remove(fixture.driverConfig.priorityControlLeaseFile.c_str());
    fixture.service.reset(new MqttDriverService(
        fixture.mqttConfig,
        fixture.driverConfig,
        {fixture.deviceConfig},
        fixture.router,
        fixture.publisher
    ));

    PriorityControlLease lease(fixture.driverConfig.priorityControlLeaseFile, "mqtt-driver");
    lease.acquire("CMD_PRIORITY_ACTIVE", "METER_1", 1001, 1770000050000LL, 30000);
    fixture.publisher->incoming.push_back(commandRequest(
        "{\"cmdId\":\"CMD_NORMAL_BLOCKED\",\"machineCode\":\"GW_TEST\",\"index\":1001,\"value\":8}"
    ));
    fixture.service->runScanOnce(1770000050100LL);

    const auto pending = fixture.store->peekPendingWriteCommands();
    require(pending.empty(), "normal command should not enqueue during active priority control");
    bool rejected = false;
    for (const auto& payload : fixture.publisher->statusPayloads) {
        if (payload.find("priority control in progress") != std::string::npos) {
            rejected = true;
        }
    }
    require(rejected, "normal command should be rejected while priority control is active");
    require(fixture.publisher->commandReplyCount == 1, "blocked command should still publish a reply");
    cleanupFixture(fixture);
}

void testAgcAvcCommandMailboxCommitsLatestWithoutWriteback() {
    auto fixture = makeFixture("agc_avc_mailbox", 10000);
    fixture.service.reset();
    PointStoreRoute mailboxRoute;
    mailboxRoute.index = 720010;
    mailboxRoute.machineCode = "GW_TEST";
    mailboxRoute.meterCode = "AGC_AVC_CORE";
    mailboxRoute.pointCode = "agc_dispatch_p";
    mailboxRoute.sharedMemoryName = fixture.shmName;
    mailboxRoute.commandMailbox = true;
    fixture.router.addRoute(mailboxRoute);
    fixture.service.reset(new MqttDriverService(
        fixture.mqttConfig,
        fixture.driverConfig,
        {fixture.deviceConfig},
        fixture.router,
        fixture.publisher
    ));
    AgcAvcCommandMailboxRuntime policy;
    policy.configured = true;
    policy.enabled = true;
    policy.shadowMode = true;
    policy.submitWrites = false;
    policy.sequenceIndex = 720011;
    policy.commandIndexes = {720010, 720011};
    fixture.service->setAgcAvcCommandMailboxRuntime(policy);

    fixture.publisher->incoming.push_back(commandRequest(
        "{\"cmdId\":\"AGC_SHADOW_1_720010\",\"machineCode\":\"GW_TEST\",\"meterCode\":\"AGC_AVC_CORE\",\"pointCode\":\"agc_dispatch_p\",\"index\":720010,\"value\":25,\"source\":\"gateway-desktop-agc-avc-shadow-test\"}"
    ));
    fixture.service->runScanOnce(1770000060000LL);

    require(fixture.store->peekPendingWriteCommands().empty(), "AGC/AVC mailbox must not enqueue a device writeback");
    const auto latest = fixture.router.getLatestByIndex(720010, 1770000060001LL);
    require(latest && std::abs(latest->value - 25.0) < 1e-9, "AGC/AVC mailbox should commit the latest command value");
    require(fixture.publisher->commandReplies.size() == 1, "AGC/AVC mailbox should reply immediately");
    require(fixture.publisher->commandReplies.front().success, "AGC/AVC mailbox reply should be successful");
    require(fixture.publisher->commandReplies.front().stage == "mailbox-committed", "AGC/AVC mailbox reply stage mismatch");
    cleanupFixture(fixture);
}

}  // namespace

int main() {
    try {
        testFullUploadOnlyWithoutRealtimeSession();
        testOneShotRealtimeRequestDoesNotCreatePeriodicSession();
        testRealtimeSessionPublishesUntilTtl();
        testRealtimeSessionStopRequest();
        testRealtimeSessionUnsubscribeIsIsolatedFromOtherSessionsAndFullUpload();
        testRealtimeSessionsKeepMeterAndIndexSelectorsIsolated();
        testCommandRequestDoesNotCreatePriorityControlLeaseByDefault();
        testCommandWritebackWaitDoesNotBlockMqttScan();
        testHighPriorityCommandRequestCreatesPriorityControlLease();
        testCommandRequestRejectedDuringActivePriorityControl();
        testAgcAvcCommandMailboxCommitsLatestWithoutWriteback();
        std::cout << "mqtt_driver_service_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "mqtt_driver_service_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
