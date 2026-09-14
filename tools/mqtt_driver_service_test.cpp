#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/mqtt_driver_service.hpp"
#include "edge_gateway/mqtt_event_stats.hpp"
#include "edge_gateway/point_store_router.hpp"
#include "edge_gateway/process_file_lock.hpp"

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
        const std::string& topic,
        const std::string& payload
    ) override {
        jsonTopics.push_back(topic);
        statusPayloads.push_back(payload);
    }

    std::vector<MqttIncomingMessage> pollIncoming(int timeoutMs) override {
        pollTimeouts.push_back(timeoutMs);
        auto messages = incoming;
        incoming.clear();
        return messages;
    }

    std::vector<MqttIncomingMessage> incoming;
    std::vector<std::size_t> fullSnapshotCounts;
    std::vector<std::string> fullSnapshotTopics;
    std::vector<std::string> onDemandTopics;
    std::vector<std::size_t> onDemandCounts;
    std::vector<RealtimePublication> realtimePublications;
    std::vector<std::string> statusPayloads;
    std::vector<std::string> jsonTopics;
    std::vector<int> pollTimeouts;
    int commandReplyCount = 0;
    std::vector<MqttCommandReply> commandReplies;
};

class FlakyFullMqttDriverPublisher : public CapturingMqttDriverPublisher {
public:
    void publishFullSnapshot(
        const std::string& topic,
        const std::vector<StoredPointValue>& values,
        const std::string& format
    ) override {
        ++attempts;
        if (failuresRemaining > 0) {
            --failuresRemaining;
            throw std::runtime_error("simulated fallback publish failure");
        }
        CapturingMqttDriverPublisher::publishFullSnapshot(topic, values, format);
    }

    int attempts = 0;
    int failuresRemaining = 0;
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
    const std::string& healthFile = std::string(),
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
    fixture.mqttConfig.clientId = "GW_TEST";
    fixture.mqttConfig.topicMachineCode = "GW_TEST";
    fixture.mqttConfig.telemetryTopic = "edge/telemetry";
    fixture.mqttConfig.realtimeTelemetryTopic = "edge/telemetry/realtime";
    fixture.mqttConfig.fullTelemetryTopic = "edge/telemetry/full";
    fixture.mqttConfig.realtimeRequestTopic = "edge/telemetry/realtime/request";
    fixture.mqttConfig.statusTopic = "edge/status";

    fixture.driverConfig.enabled = true;
    fixture.driverConfig.sharedMemoryName = fixture.shmName;
    fixture.driverConfig.priorityControlLeaseFile = "/tmp/" + fixture.shmName + "-priority.json";
    fixture.driverConfig.powerControlOwnershipFile = "/tmp/" + fixture.shmName + "-ownership.json";
    std::remove(fixture.driverConfig.priorityControlLeaseFile.c_str());
    std::remove(fixture.driverConfig.powerControlOwnershipFile.c_str());
    fixture.driverConfig.scanIntervalMs = 100;
    fixture.driverConfig.fullUploadIntervalMs = fullUploadIntervalMs;
    fixture.driverConfig.publishFullOnStart = false;
    fixture.driverConfig.publishAllOnFull = false;
    fixture.driverConfig.fullUploadIndexes = {1001, 1002};
    fixture.driverConfig.healthFile = healthFile;
    fixture.driverConfig.healthPublishIntervalMs = 100;
    fixture.driverConfig.healthWindowCycles = 20;
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

void writeFullWorkerHealth(
    const std::string& path,
    const std::string& state,
    bool healthy,
    std::int64_t heartbeatAtMs,
    std::int64_t leaseUntilMs,
    std::int64_t heartbeatMonotonicMs = -1,
    std::int64_t leaseUntilMonotonicMs = -1
) {
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    output << "{\"primaryFullUpload\":true,\"state\":\"" << state
           << "\",\"healthy\":" << (healthy ? "true" : "false")
           << ",\"machineCode\":\"GW_TEST\""
           << ",\"clientId\":\"GW_TEST-full\""
           << ",\"topic\":\"edge/telemetry/full\""
           << ",\"heartbeatAtMs\":" << heartbeatAtMs
           << ",\"leaseUntilMs\":" << leaseUntilMs;
    if (heartbeatMonotonicMs >= 0 && leaseUntilMonotonicMs >= 0) {
        output << ",\"heartbeatMonotonicMs\":" << heartbeatMonotonicMs
               << ",\"leaseUntilMonotonicMs\":" << leaseUntilMonotonicMs;
    }
    output << "}";
}

struct EventOwnershipPaths {
    std::string database;
    std::string workerHealth;
    std::string replayLock;
    std::string delegationReady;
};

MqttEventOutbox::EventTypeFilter businessEventFilter() {
    return {{"alarm", "change"}, {}};
}

MqttEventOutbox::EventTypeFilter managementEventFilter() {
    return {{}, {"alarm", "change"}};
}

MqttEventOutbox::EventMessage eventMessage(
    const std::string& eventId,
    const std::string& eventType,
    const std::string& topic
) {
    MqttEventOutbox::EventMessage event;
    event.eventId = eventId;
    event.targetId = "main";
    event.eventType = eventType;
    event.topic = topic;
    event.payload = std::string("{\"eventId\":\"") + eventId + "\"}";
    event.eventTs = 1770001000000LL;
    return event;
}

std::unique_ptr<MqttEventOutbox> makeEventOutbox(const std::string& path) {
    const auto* configured = std::getenv("SQLITE3_LIBRARY_PATH");
    return std::unique_ptr<MqttEventOutbox>(new MqttEventOutbox(
        path,
        configured ? configured : "",
        12,
        24,
        100,
        0
    ));
}

void removeEventOwnershipFiles(const EventOwnershipPaths& paths) {
    std::remove(paths.database.c_str());
    std::remove((paths.database + "-wal").c_str());
    std::remove((paths.database + "-shm").c_str());
    std::remove(paths.workerHealth.c_str());
    std::remove((paths.workerHealth + ".tmp").c_str());
    std::remove(paths.replayLock.c_str());
    std::remove(paths.delegationReady.c_str());
    std::remove((paths.delegationReady + ".tmp").c_str());
    std::remove((paths.delegationReady + ".lock").c_str());
}

EventOwnershipPaths configureIsolatedEventOwnership(
    ServiceFixture& fixture,
    const std::string& suffix
) {
    EventOwnershipPaths paths;
    const auto prefix = std::string("/tmp/mqtt_driver_service_") + suffix;
    paths.database = prefix + "_events.db";
    paths.workerHealth = prefix + "_forwarder_health.json";
    paths.replayLock = prefix + "_event_replay.lock";
    paths.delegationReady = prefix + "_delegation_ready.json";
    removeEventOwnershipFiles(paths);

    fixture.mqttConfig.eventOutboxSqlitePath = paths.database;
    fixture.mqttConfig.changeEventTopic = "edge/event/change";
    fixture.mqttConfig.alarmTopic = "edge/alarm";
    fixture.mqttConfig.changeEventTopicMachineScoped = true;
    fixture.mqttConfig.alarmTopicMachineScoped = true;
    fixture.driverConfig.fullUploadWorker.mode = "isolated";
    fixture.driverConfig.fullUploadWorker.eventForwardingEnabled = true;
    fixture.driverConfig.fullUploadWorker.healthFile = paths.workerHealth;
    fixture.driverConfig.fullUploadWorker.eventReplayLockFile = paths.replayLock;
    fixture.driverConfig.fullUploadWorker.eventDelegationReadyFile = paths.delegationReady;
    fixture.driverConfig.fullUploadWorker.healthHeartbeatMs = 100;
    fixture.driverConfig.fullUploadWorker.failoverTimeoutMs = 3000;
    fixture.driverConfig.eventReplayMaxBytes = 256 * 1024;
    return paths;
}

void writeEventForwarderHealth(
    const ServiceFixture& fixture,
    const EventOwnershipPaths& paths,
    std::int64_t heartbeatMonotonicMs,
    std::int64_t leaseUntilMonotonicMs,
    bool eventForwarding = true,
    bool eventOutboxHealthy = true,
    const std::string& state = "active",
    bool healthy = true,
    const std::string& extraFields = {}
) {
    std::ofstream output(paths.workerHealth.c_str(), std::ios::binary | std::ios::trunc);
    output << "{\"healthy\":" << (healthy ? "true" : "false")
           << mqttEventStatsHealthFields(readMqttEventStats(nullptr, mqttDriverBusinessStatsQuery()))
           << ",\"dataPlaneVersion\":2"
           << ",\"state\":\"" << state << "\""
           << ",\"eventForwarding\":" << (eventForwarding ? "true" : "false")
           << ",\"eventOutboxHealthy\":" << (eventOutboxHealthy ? "true" : "false")
           << ",\"eventTargetId\":\"main\""
           << ",\"machineCode\":\"GW_TEST\""
           << ",\"clientId\":\"" << fixture.mqttConfig.clientId
           << fixture.driverConfig.fullUploadWorker.clientIdSuffix << "\""
           << ",\"topic\":\"" << fixture.mqttConfig.fullTelemetryTopic << "\""
           << ",\"eventOutboxPath\":\"" << paths.database << "\""
           << ",\"eventReplayLockFile\":\"" << paths.replayLock << "\""
           << ",\"changeTopic\":\"" << fixture.mqttConfig.changeEventTopic << "\""
           << ",\"alarmTopic\":\"" << fixture.mqttConfig.alarmTopic << "\""
           << ",\"changeTopicMachineScoped\":true"
           << ",\"alarmTopicMachineScoped\":true"
           << ",\"heartbeatMonotonicMs\":" << heartbeatMonotonicMs
           << ",\"leaseUntilMonotonicMs\":" << leaseUntilMonotonicMs
           << extraFields
           << "}";
}

std::int64_t monotonicNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

std::size_t publishedTopicCount(
    const CapturingMqttDriverPublisher& publisher,
    const std::string& topic
) {
    return static_cast<std::size_t>(std::count(
        publisher.jsonTopics.begin(),
        publisher.jsonTopics.end(),
        topic
    ));
}

void cleanupFixture(ServiceFixture& fixture) {
    std::remove(fixture.driverConfig.priorityControlLeaseFile.c_str());
    std::remove(fixture.driverConfig.powerControlOwnershipFile.c_str());
    if (!fixture.driverConfig.healthFile.empty()) {
        std::remove(fixture.driverConfig.healthFile.c_str());
    }
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

void testIsolatedFullWorkerUsesBoundedStartupGrace() {
    auto fixture = makeFixture("isolated_startup", 30000);
    fixture.service.reset();
#ifdef _WIN32
    fixture.driverConfig.fullUploadWorker.healthFile = std::tmpnam(nullptr);
#else
    fixture.driverConfig.fullUploadWorker.healthFile = "/tmp/mqtt_isolated_startup_test.json";
#endif
    std::remove(fixture.driverConfig.fullUploadWorker.healthFile.c_str());
    fixture.driverConfig.fullUploadWorker.mode = "isolated";
    fixture.driverConfig.fullUploadWorker.failoverTimeoutMs = 3000;
    fixture.driverConfig.publishFullOnStart = true;
    fixture.driverConfig.fullUploadWorker.publishLockFile =
        fixture.driverConfig.fullUploadWorker.healthFile + ".lock";
    fixture.service.reset(new MqttDriverService(
        fixture.mqttConfig,
        fixture.driverConfig,
        {fixture.deviceConfig},
        fixture.router,
        fixture.publisher
    ));

    const std::int64_t startedAt = 1770000800000LL;
    fixture.service->runScanOnce(startedAt);
    fixture.service->runScanOnce(startedAt + 2999);
    require(fixture.publisher->fullSnapshotCounts.empty(),
        "inline fallback should wait one bounded worker startup window");
    fixture.service->runScanOnce(startedAt + 3000);
    require(fixture.publisher->fullSnapshotCounts.size() == 1,
        "missing isolated worker must fail open after 3 seconds");

    std::remove(fixture.driverConfig.fullUploadWorker.healthFile.c_str());
    cleanupFixture(fixture);
}

void testIsolatedFullWorkerLeaseSuppressesAndThenImmediatelyFallsBack() {
    auto fixture = makeFixture("isolated_lease", 30000);
    fixture.service.reset();
#ifdef _WIN32
    fixture.driverConfig.fullUploadWorker.healthFile = std::tmpnam(nullptr);
#else
    fixture.driverConfig.fullUploadWorker.healthFile = "/tmp/mqtt_isolated_lease_test.json";
#endif
    fixture.driverConfig.fullUploadWorker.mode = "isolated";
    fixture.driverConfig.fullUploadWorker.failoverTimeoutMs = 3000;
    fixture.driverConfig.publishFullOnStart = true;
    fixture.driverConfig.fullUploadWorker.publishLockFile =
        fixture.driverConfig.fullUploadWorker.healthFile + ".lock";
    fixture.service.reset(new MqttDriverService(
        fixture.mqttConfig,
        fixture.driverConfig,
        {fixture.deviceConfig},
        fixture.router,
        fixture.publisher
    ));

    const std::int64_t startedAt = 1770000900000LL;
    writeFullWorkerHealth(
        fixture.driverConfig.fullUploadWorker.healthFile,
        "claiming",
        false,
        startedAt,
        startedAt + 3000
    );
    fixture.service->runScanOnce(startedAt);
    require(fixture.publisher->fullSnapshotCounts.empty(),
        "fresh claiming lease must prevent a duplicate inline full");

    writeFullWorkerHealth(
        fixture.driverConfig.fullUploadWorker.healthFile,
        "active",
        true,
        startedAt + 1000,
        startedAt + 4000
    );
    fixture.service->runScanOnce(startedAt + 1000);
    fixture.service->runScanOnce(startedAt + 3999);
    require(fixture.publisher->fullSnapshotCounts.empty(),
        "fresh active lease must suppress periodic inline full");

    fixture.service->runScanOnce(startedAt + 4001);
    require(fixture.publisher->fullSnapshotCounts.size() == 1,
        "expired worker lease must trigger inline fallback on the next scan");

    writeFullWorkerHealth(
        fixture.driverConfig.fullUploadWorker.healthFile,
        "retrying",
        false,
        startedAt + 4100,
        0
    );
    fixture.service->runScanOnce(startedAt + 4100);
    require(fixture.publisher->fullSnapshotCounts.size() == 1,
        "retrying health must not cause duplicate inline snapshots");

    std::remove(fixture.driverConfig.fullUploadWorker.healthFile.c_str());
    cleanupFixture(fixture);
}

void testIsolatedFullWorkerLeaseIgnoresWallClockRollback() {
    auto fixture = makeFixture("isolated_clock_rollback", 30000);
    fixture.service.reset();
#ifdef _WIN32
    fixture.driverConfig.fullUploadWorker.healthFile = std::tmpnam(nullptr);
#else
    fixture.driverConfig.fullUploadWorker.healthFile = "/tmp/mqtt_isolated_clock_rollback_test.json";
#endif
    fixture.driverConfig.fullUploadWorker.mode = "isolated";
    fixture.driverConfig.fullUploadWorker.failoverTimeoutMs = 3000;
    fixture.driverConfig.publishFullOnStart = true;
    fixture.driverConfig.fullUploadWorker.publishLockFile =
        fixture.driverConfig.fullUploadWorker.healthFile + ".lock";
    fixture.service.reset(new MqttDriverService(
        fixture.mqttConfig,
        fixture.driverConfig,
        {fixture.deviceConfig},
        fixture.router,
        fixture.publisher
    ));

    const std::int64_t wallNow = 1000;
    const auto monotonicNow = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
    writeFullWorkerHealth(
        fixture.driverConfig.fullUploadWorker.healthFile,
        "active",
        true,
        wallNow + 3600000,
        wallNow + 3603000,
        monotonicNow,
        monotonicNow + 3000
    );
    fixture.service->runScanOnce(wallNow);
    require(
        fixture.publisher->fullSnapshotCounts.empty(),
        "a fresh monotonic lease must survive a backward wall-clock step"
    );

    writeFullWorkerHealth(
        fixture.driverConfig.fullUploadWorker.healthFile,
        "active",
        true,
        wallNow,
        wallNow + 3000,
        monotonicNow - 4000,
        monotonicNow - 1000
    );
    fixture.service->runScanOnce(wallNow + 1);
    require(
        fixture.publisher->fullSnapshotCounts.size() == 1,
        "an expired monotonic lease must not be kept alive by wall-clock timestamps"
    );

    std::remove(fixture.driverConfig.fullUploadWorker.healthFile.c_str());
    std::remove(fixture.driverConfig.fullUploadWorker.publishLockFile.c_str());
    cleanupFixture(fixture);
}

void testIsolatedFallbackRetriesWithoutAnotherStartupGrace() {
    auto fixture = makeFixture("isolated_retry", 30000);
    fixture.service.reset();
#ifdef _WIN32
    fixture.driverConfig.fullUploadWorker.healthFile = std::tmpnam(nullptr);
#else
    fixture.driverConfig.fullUploadWorker.healthFile = "/tmp/mqtt_isolated_retry_test.json";
#endif
    fixture.driverConfig.fullUploadWorker.mode = "isolated";
    fixture.driverConfig.fullUploadWorker.failoverTimeoutMs = 3000;
    fixture.driverConfig.fullUploadWorker.retryMinMs = 500;
    fixture.driverConfig.fullUploadWorker.retryMaxMs = 2000;
    fixture.driverConfig.fullUploadWorker.publishLockFile =
        fixture.driverConfig.fullUploadWorker.healthFile + ".lock";
    fixture.driverConfig.publishFullOnStart = true;
    auto publisher = std::make_shared<FlakyFullMqttDriverPublisher>();
    publisher->failuresRemaining = 1;
    fixture.publisher = publisher;
    fixture.service.reset(new MqttDriverService(
        fixture.mqttConfig,
        fixture.driverConfig,
        {fixture.deviceConfig},
        fixture.router,
        publisher
    ));

    const std::int64_t startedAt = 1770000950000LL;
    writeFullWorkerHealth(
        fixture.driverConfig.fullUploadWorker.healthFile,
        "active",
        true,
        startedAt,
        startedAt + 3000
    );
    fixture.service->runScanOnce(startedAt);
    fixture.service->runScanOnce(startedAt + 3001);
    require(publisher->attempts == 1,
        "lost worker lease should trigger the first fallback attempt");
    fixture.service->runScanOnce(startedAt + 3500);
    require(publisher->attempts == 1,
        "fallback retry must respect the configured 500ms delay");
    fixture.service->runScanOnce(startedAt + 3501);
    require(publisher->attempts == 2 && publisher->fullSnapshotCounts.size() == 1,
        "failed fallback must retry after 500ms without another startup grace");

    std::remove(fixture.driverConfig.fullUploadWorker.healthFile.c_str());
    std::remove(fixture.driverConfig.fullUploadWorker.publishLockFile.c_str());
    cleanupFixture(fixture);
}

void testFullUploadPointSelectionIsSharedWithForwarder() {
    auto fixture = makeFixture("full_selection", 1000);
    MqttDriverConfig config = fixture.driverConfig;
    config.publishAllOnFull = true;
    config.fullUploadIndexes = {1002};
    const auto selected = MqttDriverService::resolveFullUploadIndexes(
        config,
        {fixture.deviceConfig},
        fixture.router
    );
    require(selected == std::vector<std::uint32_t>({1001, 1002}),
        "shared full selection must merge explicit and point fullUpload indexes");

    auto noFlags = fixture.deviceConfig;
    for (auto& meter : noFlags.meters) {
        for (auto& point : meter.points) {
            point.fullUpload = false;
        }
    }
    PointStoreRouter allRouter;
    allRouter.addStore(fixture.shmName, *fixture.store);
    allRouter.addRoutesFromDeviceConfigs({noFlags}, fixture.shmName);
    config.fullUploadIndexes.clear();
    const auto allSelected = MqttDriverService::resolveFullUploadIndexes(
        config,
        {noFlags},
        allRouter
    );
    require(allSelected == std::vector<std::uint32_t>({1001, 1002}),
        "publishAllOnFull must resolve to all routed indexes for both publishers");

    CameraServiceConfig cameraConfig;
    cameraConfig.enabled = true;
    cameraConfig.sharedMemoryName = fixture.shmName;
    CameraConfig camera;
    camera.cameraCode = "CAMERA_1";
    camera.statusPointIndexes.online = 1901;
    camera.statusPointIndexes.fps = 1902;
    camera.statusPointIndexes.bitrateKbps = 1903;
    camera.statusPointIndexes.errorCode = 1904;
    cameraConfig.cameras.push_back(camera);
    allRouter.addRoutesFromCameraServiceConfig(cameraConfig, "GW_TEST");
    config.publishAllOnFull = false;
    const auto cameraSelected = MqttDriverService::resolveFullUploadIndexes(
        config,
        {noFlags},
        allRouter
    );
    require(cameraSelected == std::vector<std::uint32_t>({1901, 1902, 1903, 1904}),
        "camera status routes must be identical in inline and isolated full uploads");
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
        "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"S1\",\"meterCode\":\"METER_1\",\"intervalMs\":250,\"ttlSec\":5}"
    ));
    fixture.service->runScanOnce(1770000020100LL);
    require(fixture.publisher->onDemandCounts.size() == 1, "realtime session should publish immediately");

    fixture.service->runScanOnce(1770000020200LL);
    require(fixture.publisher->onDemandCounts.size() == 1, "session should respect requested interval");
    fixture.service->runScanOnce(1770000020350LL);
    require(fixture.publisher->onDemandCounts.size() == 2, "session should publish when interval is due");
    fixture.service->runScanOnce(1770000020600LL);
    require(fixture.publisher->onDemandCounts.size() == 3, "session should keep publishing while active");
    fixture.service->runScanOnce(1770000025100LL);
    require(fixture.publisher->onDemandCounts.size() == 3, "session should stop after ttl expires");
    cleanupFixture(fixture);
}

void testRealtimeSessionStopRequest() {
    auto fixture = makeFixture("stop", 10000);
    fixture.service->runScanOnce(1770000030000LL);
    fixture.publisher->incoming.push_back(realtimeRequest(
        "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"S1\",\"meterCode\":\"METER_1\",\"intervalMs\":250,\"ttlSec\":30}"
    ));
    fixture.service->runScanOnce(1770000030100LL);
    require(fixture.publisher->onDemandCounts.size() == 1, "session should publish immediately before stop");
    fixture.publisher->incoming.push_back(realtimeRequest(
        "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"S1\",\"action\":\"stop\"}"
    ));
    fixture.service->runScanOnce(1770000030150LL);
    fixture.service->runScanOnce(1770000030350LL);
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
        "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"REALTIME_TARGET\",\"indexes\":[1001],\"intervalMs\":250,\"ttlSec\":30}"
    ));
    fixture.publisher->incoming.push_back(realtimeRequest(
        "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"REALTIME_SURVIVOR\",\"indexes\":[1001,1002],\"intervalMs\":250,\"ttlSec\":30}"
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
    fixture.service->runScanOnce(startedAt + 350);
    require(fixture.publisher->onDemandCounts.size() == 4, "unknown unsubscribe should leave both sessions active");
    require(
        std::count(fixture.publisher->onDemandCounts.begin(), fixture.publisher->onDemandCounts.end(), 1) == 2 &&
            std::count(fixture.publisher->onDemandCounts.begin(), fixture.publisher->onDemandCounts.end(), 2) == 2,
        "unknown unsubscribe should be a no-op for each realtime session"
    );

    fixture.publisher->incoming.push_back(realtimeRequest(
        "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"REALTIME_TARGET\",\"action\":\"unsubscribe\"}"
    ));
    fixture.service->runScanOnce(startedAt + 400);
    fixture.service->runScanOnce(startedAt + 600);
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
    auto fixture = makeFixture("session_selector_isolation", 1000, false, std::string(), true);
    const std::int64_t startedAt = 1770000037000LL;
    fixture.service->runScanOnce(startedAt);

    fixture.publisher->incoming.push_back(realtimeRequest(
        "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"SESSION_A\",\"meterCode\":\"METER_1\",\"intervalMs\":250,\"ttlSec\":30}"
    ));
    fixture.publisher->incoming.push_back(realtimeRequest(
        "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"SESSION_B\",\"indexes\":[2001],\"intervalMs\":250,\"ttlSec\":30}"
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

    fixture.service->runScanOnce(startedAt + 350);
    require(countSession("SESSION_A") == 2, "meter-scoped session should keep its sessionId periodically");
    require(countSession("SESSION_B") == 2, "index-scoped session should keep its sessionId periodically");

    fixture.publisher->incoming.push_back(realtimeRequest(
        "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"SESSION_A\",\"action\":\"unsubscribe\"}"
    ));
    fixture.service->runScanOnce(startedAt + 400);
    fixture.service->runScanOnce(startedAt + 600);
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

void testRealtimeSessionEchoForEverySelectorAndLegacyDefault() {
    const std::vector<std::string> selectors = {
        R"(,"indexes":[1001])", R"(,"meterCode":"METER_1")", ""
    };
    const std::vector<std::vector<std::uint32_t>> expectedIndexes = {
        {1001}, {1001, 1002}, {1001, 1002, 2001}
    };
    for (std::size_t i = 0; i < selectors.size(); ++i) {
        for (const bool legacyDefault : {false, true}) {
            auto fixture = makeFixture("echo_matrix_" + std::to_string(i) +
                (legacyDefault ? "_legacy" : "_explicit"), 2000, false, std::string(), true);
            const std::string id = legacyDefault ? "" : "STUDIO_RT_owner_0123456789abcdef";
            const std::int64_t start = 1770000040000LL;
            fixture.service->runScanOnce(start);
            fixture.publisher->incoming.push_back(realtimeRequest(
                R"({"machineCode":"GW_TEST","action":"start","sessionId":")" + id +
                R"(","intervalMs":1000,"ttlSec":20)" + selectors[i] + "}"));
            fixture.service->runScanOnce(start + 100);
            require(fixture.publisher->realtimePublications.size() == 1,
                "each selector should publish immediately exactly once");
            fixture.service->runScanOnce(start + 1099);
            require(fixture.publisher->realtimePublications.size() == 1,
                "each selector should wait for its interval");
            fixture.service->runScanOnce(start + 1100);
            require(fixture.publisher->realtimePublications.size() == 2,
                "each selector should publish once when due");
            require(fixture.publisher->fullSnapshotCounts.empty(),
                "full must wait for its independently configured interval");
            for (const auto& publication : fixture.publisher->realtimePublications) {
                require(publication.sessionId == id,
                    "immediate and periodic echo must preserve explicit IDs and omit synthesized defaults");
                require(publication.topic == fixture.mqttConfig.realtimeTelemetryTopic,
                    "each selector must keep the realtime topic");
                std::vector<std::uint32_t> indexes;
                for (const auto& value : publication.values) {
                    indexes.push_back(value.index);
                }
                std::sort(indexes.begin(), indexes.end());
                require(indexes == expectedIndexes[i], "selector returned unexpected points");
            }
            const auto stopId = legacyDefault
                ? std::string("default:GW_TEST:") + (i == 1 ? "METER_1" : "*") : id;
            fixture.publisher->incoming.push_back(realtimeRequest(
                R"({"machineCode":"GW_TEST","action":"stop","sessionId":")" + stopId + "\"}"));
            fixture.service->runScanOnce(start + 1200);
            fixture.service->runScanOnce(start + 2100);
            require(fixture.publisher->realtimePublications.size() == 2,
                "explicit or synthesized default ID must stop only that session");
            require(fixture.publisher->fullSnapshotCounts == std::vector<std::size_t>({3}),
                "full cadence and selected points must survive realtime start and stop");
            cleanupFixture(fixture);
        }
    }
    std::cout << "session echo: indexes/meter/all, immediate/periodic, Studio/default, stop/full passed\n";
}

struct RealtimeFixtureCleanup {
    ServiceFixture& fixture;
    ~RealtimeFixtureCleanup() { cleanupFixture(fixture); }
};

void testRealtimeRejectsInvalidTimingBeforePublish() {
    auto fixture = makeFixture("admission_timing", 0);
    RealtimeFixtureCleanup cleanup{fixture};
    const std::int64_t now = 1770000100000LL;
    fixture.service->runScanOnce(now);
    const std::vector<std::string> fields = {
        R"("intervalMs":249)", R"("intervalMs":60001)", R"("intervalMs":0)",
        R"("intervalMs":-1)", R"("intervalMs":4294967295)", R"("intervalMs":4294967296)",
        R"("intervalMs":250.5)", R"("intervalMs":1e300)", R"("intervalMs":"1000")",
        R"("intervalMs":null)", R"("intervalMs":1000junk)",
        R"("ttlSec":4)", R"("ttlSec":301)", R"("ttlSec":0)", R"("ttlSec":-1)",
        R"("ttlSec":9223372036854775807)", R"("ttlSec":18446744073709551616)",
        R"("ttlSec":5.5)", R"("ttlSec":1e300)", R"("ttlSec":true)"
    };
    for (const auto& field : fields) {
        fixture.publisher->incoming.push_back(realtimeRequest(
            R"({"sessionId":"INVALID",)" + field + "}"));
        fixture.service->runScanOnce(now + 1);
        require(fixture.publisher->realtimePublications.empty(),
            "invalid realtime timing published before rejection: " + field);
    }
    std::cout << "realtime admission: invalid timing rejected before publish passed\n";
}

std::string realtimeIndexArray(std::size_t count) {
    std::string result = "[";
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) result += ',';
        result += "1001";
    }
    return result + ']';
}

void testRealtimeSelectorsAreBoundedWithoutAllFallback() {
    auto fixture = makeFixture("admission_selectors", 0, false, std::string(), true);
    RealtimeFixtureCleanup cleanup{fixture};
    const std::int64_t now = 1770000100000LL;
    fixture.service->runScanOnce(now);
    const std::vector<std::string> invalid = {
        R"("indexes":[0])", R"("indexes":[1001,0])", R"("indexes":[4294967296])",
        R"("indexes":[-1])", R"("indexes":[1.5])", R"("indexes":[1e3])",
        R"("indexes":["bad"])", R"("indexes":[true])", R"("indexes":[{}])",
        R"("indexes":[1001,])", R"("indexes":[1001)", R"("indexes":"1001")",
        R"("indexes":null)", R"("indices":{})", R"("index":0)", R"("index":4294967296)",
        R"("index":1001.5)", R"("index":"1001")", R"("indexes":[01])",
        R"("note":"indexes","indexes":[0])", R"("indexes":[],"indexes":[0])",
        R"("note":0 "indexes":[0])", R"("note":"x" "indexes":[0])",
        R"("note":[] "indexes":[0])", R"("note":{} "indexes":[0])",
        "\"indexes\":" + realtimeIndexArray(4097),
        "\"index\":1001,\"indexes\":" + realtimeIndexArray(4096),
        "\"indexes\":" + realtimeIndexArray(4096) + R"(,"indices":[1001])"
    };
    for (const auto& fields : invalid) {
        fixture.publisher->incoming.push_back(realtimeRequest("{" + fields + "}"));
        fixture.service->runScanOnce(now + 1);
        require(fixture.publisher->realtimePublications.empty(),
            "invalid selector published or fell back to all: " + fields.substr(0, 100));
    }
    const std::vector<std::pair<std::string, std::size_t>> valid = {
        {"", 3}, {R"("indexes":[])", 3}, {R"("indices":[])", 3},
        {R"("indexes":[],"meterCode":"METER_1")", 2},
        {R"("indexes":[1001],"meterCode":"METER_2")", 1},
        {R"("indexes":["1001",1001],"indices":["1002"])", 2},
        {R"("indexes":[4294967295])", 0},
        {"\"indexes\":" + realtimeIndexArray(4096), 1},
        {R"("metadata":{"indexes":[0]},"indexes":[1001])", 1}
    };
    for (const auto& item : valid) {
        const auto before = fixture.publisher->realtimePublications.size();
        fixture.publisher->incoming.push_back(realtimeRequest("{" + item.first + "}"));
        fixture.service->runScanOnce(now + 2);
        require(fixture.publisher->realtimePublications.size() == before + 1,
            "valid selector should produce one legacy snapshot");
        require(fixture.publisher->realtimePublications.back().values.size() == item.second,
            "empty/meter/index selector precedence changed");
    }
    std::cout << "realtime admission: selector validity/raw bounds/empty precedence passed\n";
}

void sendRealtime(ServiceFixture& fixture, const std::string& fields, std::int64_t now) {
    fixture.publisher->incoming.push_back(realtimeRequest("{" + fields + "}"));
    fixture.service->runScanOnce(now);
}

std::string sessionFields(const std::string& id, int interval = 60000, int ttl = 5) {
    return R"("sessionId":")" + id + R"(","intervalMs":)" + std::to_string(interval) +
        R"(,"ttlSec":)" + std::to_string(ttl) + R"(,"indexes":[1001])";
}

void testRealtimeCapacityRenewStopAndExpiry() {
    auto fixture = makeFixture("admission_capacity", 0);
    RealtimeFixtureCleanup cleanup{fixture};
    const std::int64_t start = 1770000100000LL;
    fixture.service->runScanOnce(start);
    for (int i = 0; i < 16; ++i) {
        fixture.publisher->incoming.push_back(realtimeRequest("{" + sessionFields("S" + std::to_string(i)) + "}"));
    }
    fixture.service->runScanOnce(start + 100);
    require(fixture.publisher->onDemandCounts.size() == 16, "16 sessions should be admitted");
    sendRealtime(fixture, sessionFields("OVER_CAPACITY"), start + 101);
    require(fixture.publisher->onDemandCounts.size() == 16, "17th session must not read/publish first");
    sendRealtime(fixture, sessionFields("S0", 60000, 30) + R"(,"action":"renew")", start + 102);
    require(fixture.publisher->onDemandCounts.size() == 16, "full-capacity renewal must not publish immediately");
    sendRealtime(fixture,
        R"("sessionId":"S1","action":"stop","indexes":"ignored","meterCode":{},"intervalMs":1,"ttlSec":"ignored")",
        start + 103);
    sendRealtime(fixture, sessionFields("REPLACEMENT"), start + 104);
    require(fixture.publisher->onDemandCounts.size() == 17, "stop at capacity must release its slot");
    fixture.service->runScanOnce(start + 4999);
    for (int i = 0; i < 14; ++i) {
        fixture.publisher->incoming.push_back(realtimeRequest("{" + sessionFields("EXPIRED_SLOT_" + std::to_string(i)) + "}"));
    }
    fixture.service->runScanOnce(start + 5100);
    require(fixture.publisher->onDemandCounts.size() == 31, "admission must reap expiry without waiting for cleanup tick");
    sendRealtime(fixture, sessionFields("STILL_FULL"), start + 5101);
    require(fixture.publisher->onDemandCounts.size() == 31, "renewed session must retain its capacity slot past original TTL");
    std::cout << "realtime admission: 16 slots, renew/stop at capacity, expiry release passed\n";
}

void testRealtimeRenewFloodPreservesPendingDue() {
    auto fixture = makeFixture("admission_renew", 0);
    RealtimeFixtureCleanup cleanup{fixture};
    const std::int64_t start = 1770000100000LL;
    fixture.service->runScanOnce(start);
    sendRealtime(fixture, sessionFields("S", 1000, 20), start + 100);
    for (const int elapsed : {200, 600, 1000, 1100}) {
        for (int i = 0; i < 50; ++i) {
            fixture.publisher->incoming.push_back(realtimeRequest("{" + sessionFields("S", 250, 300) +
                (i % 2 ? R"(,"action":"start"})" : R"(,"action":"renew"})")));
        }
        fixture.service->runScanOnce(start + elapsed);
        require(fixture.publisher->onDemandCounts.size() == (elapsed == 1100 ? 2U : 1U),
            "repeat start/renew must neither publish immediately nor postpone pending due");
    }
    sendRealtime(fixture, sessionFields("S", 60000, 300), start + 1200);
    fixture.service->runScanOnce(start + 1350);
    require(fixture.publisher->onDemandCounts.size() == 3, "larger renewed interval must not postpone pending due");
    fixture.service->runScanOnce(start + 61349);
    require(fixture.publisher->onDemandCounts.size() == 3, "subsequent due must use new interval");
    fixture.service->runScanOnce(start + 61350);
    require(fixture.publisher->onDemandCounts.size() == 4, "renewed interval should apply after pending publication");
    std::cout << "realtime admission: repeated start/renew interval and starvation guard passed\n";
}

void testRealtimeIdentityBoundsAndStudioCompatibility() {
    auto fixture = makeFixture("admission_identity", 0);
    RealtimeFixtureCleanup cleanup{fixture};
    const std::int64_t now = 1770000100000LL;
    fixture.service->runScanOnce(now);
    const std::vector<std::string> invalid = {
        R"("sessionId":")" + std::string(129, 'S') + '"',
        R"("sessionId":"has space")", R"("sessionId":"has\ncontrol")",
        std::string(R"("sessionId":")") + char(127) + '"',
        std::string(R"("sessionId":")") + "\xc3\xa9" + '"',
        R"("sessionId":null)", R"("sessionId":123)", R"("sessionId":"bad\u0041")",
        R"("meterCode":")" + std::string(129, 'M') + '"', R"("meterCode":{})",
        R"("machineCode":true)", R"("action":123)",
        R"("note":"sessionId","sessionId":false)",
        R"("sessionId":"A","sessionId":"B")"
    };
    for (const auto& fields : invalid) {
        sendRealtime(fixture, fields, now + 1);
        require(fixture.publisher->realtimePublications.empty(), "invalid identity published: " + fields);
    }
    const std::string studioId = "STUDIO_RT_" + std::string(32, 'a') + "_" + std::string(16, 'b');
    require(studioId.size() == 59, "Studio fixture must follow confirmed owner/hash sizing");
    for (const auto& id : {studioId, std::string(128, 'S'), std::string("REALTIME_A"), std::string("default:GW_TEST:*")}) {
        sendRealtime(fixture, sessionFields(id), now + 2);
        require(fixture.publisher->realtimePublications.back().sessionId == id, "valid ID must echo without prefix restriction");
    }
    require(fixture.publisher->realtimePublications.size() == 4, "valid IDs should each publish exactly once");
    std::cout << "realtime admission: ID bytes/ASCII/types/Studio59 and 128 boundary passed\n";
}

void useRealtimeIdentity(ServiceFixture& fixture, const std::string& machine, const std::string& meter) {
    fixture.service.reset();
    fixture.deviceConfig.machineCode = machine;
    fixture.deviceConfig.meters.front().meterCode = meter;
    fixture.mqttConfig.topicMachineCode = machine;
    fixture.mqttConfig.clientId = machine;
    fixture.router = PointStoreRouter();
    fixture.router.addStore(fixture.shmName, *fixture.store);
    fixture.router.addRoutesFromDeviceConfigs({fixture.deviceConfig}, fixture.shmName);
    fixture.service.reset(new MqttDriverService(fixture.mqttConfig, fixture.driverConfig,
        {fixture.deviceConfig}, fixture.router, fixture.publisher));
}

void testRealtimeLongDefaultStopExceptionIsExact() {
    auto fixture = makeFixture("admission_default_stop", 0);
    RealtimeFixtureCleanup cleanup{fixture};
    const std::string machine(128, 'G');
    const std::string meter(128, 'M');
    const std::string key = "default:" + machine + ":" + meter;
    require(key.size() == 265, "default stop fixture must hit the exact composite-key bound");
    useRealtimeIdentity(fixture, machine, meter);
    const std::int64_t now = 1770000100000LL;
    fixture.service->runScanOnce(now);
    const auto identity = R"("machineCode":")" + machine + R"(","meterCode":")" + meter + '"';
    sendRealtime(fixture, identity + R"(,"action":"start","intervalMs":1000,"ttlSec":20)", now + 100);
    require(fixture.publisher->realtimePublications.size() == 1, "long synthetic default should start without supplied ID");
    const auto stoppedCount = [&]() {
        return std::count_if(fixture.publisher->statusPayloads.begin(), fixture.publisher->statusPayloads.end(),
            [](const std::string& value) { return value.find("realtime-session-stopped") != std::string::npos; });
    };
    const std::vector<std::string> rejected = {
        identity + R"(,"action":"start","sessionId":")" + key + '"',
        identity + R"(,"action":"renew","sessionId":")" + key + '"',
        identity + R"(,"action":"stop","sessionId":")" + key + "X\"",
        identity + R"(,"action":"stop","sessionId":")" + key.substr(0, 264) + "N\"",
        R"("machineCode":")" + machine + R"(","action":"stop","sessionId":")" + key + '"',
        R"("machineCode":"GW_TEST","meterCode":")" + meter + R"(","action":"stop","sessionId":")" + key + '"'
    };
    for (const auto& fields : rejected) {
        sendRealtime(fixture, fields, now + 200);
        require(fixture.publisher->realtimePublications.size() == 1, "long supplied ID must not start or renew");
        require(stoppedCount() == 0, "only exact verified default key may use the long-ID stop exception");
    }
    fixture.service->runScanOnce(now + 1100);
    require(fixture.publisher->realtimePublications.size() == 2, "rejected stops must preserve synthetic session");
    sendRealtime(fixture, identity + R"(,"action":"stop","sessionId":")" + key +
        R"(","indexes":[0],"intervalMs":1,"ttlSec":9223372036854775807)", now + 1200);
    fixture.service->runScanOnce(now + 2100);
    require(stoppedCount() == 1 && fixture.publisher->realtimePublications.size() == 2,
        "exact 265-byte default key should stop despite irrelevant numeric/selector values");
    sendRealtime(fixture, identity + R"(,"action":"start","intervalMs":1000,"ttlSec":20)", now + 2200);
    sendRealtime(fixture, identity + R"(,"action":"stop","indexes":false)", now + 2300);
    fixture.service->runScanOnce(now + 3200);
    require(stoppedCount() == 2 && fixture.publisher->realtimePublications.size() == 3,
        "no-ID machine/meter stop must remain supported");
    std::cout << "realtime admission: exact long default stop positive/negative and no-ID stop passed\n";
}

void testRealtimeTimingBoundariesDefaultsAndDeadlineOverflow() {
    const std::int64_t now = 1770000100000LL;
    for (const int scanInterval : {1, 100000}) {
        auto fixture = makeFixture("admission_defaults_" + std::to_string(scanInterval), 0);
        RealtimeFixtureCleanup cleanup{fixture};
        fixture.driverConfig.scanIntervalMs = scanInterval;
        useRealtimeIdentity(fixture, "GW_TEST", "METER_1");
        fixture.service->runScanOnce(now);
        sendRealtime(fixture, R"("sessionId":"DEFAULTS")", now + 100);
        const int interval = scanInterval == 1 ? 250 : 60000;
        const auto started = fixture.publisher->statusPayloads.back();
        require(started.find("\"intervalMs\":" + std::to_string(interval)) != std::string::npos,
            "missing interval must clamp configured scan to approved bounds");
        require(started.find("\"expireAtMs\":" + std::to_string(now + 30100)) != std::string::npos,
            "missing TTL must remain 30 seconds");
        fixture.service->runScanOnce(now + 349);
        require(fixture.publisher->onDemandCounts.size() == 1, "default must not fire below 250ms");
        fixture.service->runScanOnce(now + 350);
        require(fixture.publisher->onDemandCounts.size() == (scanInterval == 1 ? 2U : 1U),
            "clamped minimum must publish exactly when due");
        fixture.service->runScanOnce(now + 30100);
        require(fixture.publisher->onDemandCounts.size() == (scanInterval == 1 ? 2U : 1U),
            "default TTL must expire at 30 seconds");
    }
    auto fixture = makeFixture("admission_deadline", 0);
    RealtimeFixtureCleanup cleanup{fixture};
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    fixture.service->runScanOnce(maximum - 100000);
    sendRealtime(fixture, sessionFields("TTL_OVERFLOW", 250, 300), maximum - 100000);
    sendRealtime(fixture, sessionFields("DUE_OVERFLOW", 60000, 5), maximum - 5000);
    require(fixture.publisher->onDemandCounts.empty(), "deadline overflow must reject before immediate snapshot");
    sendRealtime(fixture, sessionFields("LAST", 1000, 5), maximum - 5000);
    require(fixture.publisher->onDemandCounts.size() == 1, "exact maximum expiry should be representable");
    fixture.service->runScanOnce(maximum - 1);
    require(fixture.publisher->onDemandCounts.size() == 2, "final periodic snapshot should not overflow next due");
    fixture.service->runScanOnce(maximum);
    require(fixture.publisher->onDemandCounts.size() == 2, "session must expire at maximum deadline");
    std::cout << "realtime admission: defaults, numeric boundaries, checked deadlines passed\n";
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

void testLegacyTelemetryUsesOldTopicAndPayloadShape() {
    auto fixture = makeFixture("legacy_topic", 1000);
    fixture.mqttConfig.legacyTelemetryEnabled = true;
    fixture.mqttConfig.legacyTelemetryTopic = "ky/peidian/GW_LEGACY";
    fixture.mqttConfig.legacyTopicMachineCode = "GW_LEGACY";
    fixture.mqttConfig.legacyTelemetryIntervalMs = 1000;
    fixture.service.reset(new MqttDriverService(
        fixture.mqttConfig,
        fixture.driverConfig,
        {fixture.deviceConfig},
        fixture.router,
        fixture.publisher
    ));

    fixture.service->runScanOnce(1770000000000LL);
    fixture.service->runScanOnce(1770000001000LL);
    auto legacyIt = std::find(
        fixture.publisher->jsonTopics.begin(),
        fixture.publisher->jsonTopics.end(),
        fixture.mqttConfig.legacyTelemetryTopic
    );
    require(legacyIt != fixture.publisher->jsonTopics.end(), "legacy telemetry topic was not published");
    const auto payloadIndex = static_cast<std::size_t>(legacyIt - fixture.publisher->jsonTopics.begin());
    const auto& payload = fixture.publisher->statusPayloads.at(payloadIndex);
    require(payload.find(R"({"data":[{"meterid":"METER_1","metrics":[)") == 0,
        "legacy telemetry payload header mismatch");
    require(payload.find(R"("metrics":[{"P_1":"12.3000","P_2":"45.6000"}])") !=
            std::string::npos,
        "legacy telemetry must place all meter points in one metrics object");
    require(payload.find(R"("split":"false")") != std::string::npos,
        "legacy telemetry must include the legacy split marker");
    bool foundDuePayload = false;
    for (std::size_t index = 0; index < fixture.publisher->jsonTopics.size(); ++index) {
        if (fixture.publisher->jsonTopics[index] == fixture.mqttConfig.legacyTelemetryTopic &&
            fixture.publisher->statusPayloads[index].find(R"("msgid":1770000001000)") != std::string::npos &&
            fixture.publisher->statusPayloads[index].find(R"("timestamp":1770000001000)") != std::string::npos) {
            foundDuePayload = true;
            break;
        }
    }
    require(foundDuePayload, "legacy telemetry must include the due publish timestamp");
    cleanupFixture(fixture);
}

void testLegacyTelemetryCanPublishFasterThanFullSnapshot() {
    auto fixture = makeFixture("legacy_faster_than_full", 10000);
    fixture.mqttConfig.legacyTelemetryEnabled = true;
    fixture.mqttConfig.legacyTelemetryTopic = "ky/peidian/GW_LEGACY";
    fixture.mqttConfig.legacyTopicMachineCode = "GW_LEGACY";
    fixture.mqttConfig.legacyTelemetryIntervalMs = 1000;
    fixture.service.reset(new MqttDriverService(
        fixture.mqttConfig,
        fixture.driverConfig,
        {fixture.deviceConfig},
        fixture.router,
        fixture.publisher
    ));

    fixture.service->runScanOnce(1770000000000LL);
    fixture.service->runScanOnce(1770000000500LL);
    fixture.service->runScanOnce(1770000001000LL);

    const auto legacyCount = static_cast<std::size_t>(std::count(
        fixture.publisher->jsonTopics.begin(),
        fixture.publisher->jsonTopics.end(),
        fixture.mqttConfig.legacyTelemetryTopic
    ));
    require(fixture.publisher->fullSnapshotCounts.empty(),
        "full snapshot should still wait for its independent 10 second interval");
    require(legacyCount == 2,
        "legacy telemetry should publish independently at its 1 second interval");
    cleanupFixture(fixture);
}

void testLegacyTelemetryRestoresLogicalMeterMapping() {
    auto fixture = makeFixture("legacy_mapping", 1000);
    fixture.mqttConfig.legacyTelemetryEnabled = true;
    fixture.mqttConfig.legacyTelemetryTopic = "ky/peidian/GW_LEGACY";
    fixture.mqttConfig.legacyTopicMachineCode = "GW_LEGACY";
    fixture.mqttConfig.legacyTelemetryIntervalMs = 1000;
    LegacyTelemetryPointMapping mapping;
    mapping.index = 1002;
    mapping.meterCode = "LEGACY_METER_2";
    mapping.pointCode = "LEGACY_POINT";
    fixture.mqttConfig.legacyTelemetryPointMappings.push_back(mapping);
    fixture.service.reset(new MqttDriverService(
        fixture.mqttConfig,
        fixture.driverConfig,
        {fixture.deviceConfig},
        fixture.router,
        fixture.publisher
    ));

    fixture.service->runScanOnce(1770000000000LL);
    fixture.service->runScanOnce(1770000001000LL);
    const auto legacyIt = std::find(
        fixture.publisher->jsonTopics.begin(),
        fixture.publisher->jsonTopics.end(),
        fixture.mqttConfig.legacyTelemetryTopic
    );
    require(legacyIt != fixture.publisher->jsonTopics.end(), "mapped legacy telemetry was not published");
    const auto payloadIndex = static_cast<std::size_t>(legacyIt - fixture.publisher->jsonTopics.begin());
    const auto& payload = fixture.publisher->statusPayloads.at(payloadIndex);
    require(payload.find(R"({"meterid":"METER_1","metrics":[{"P_1":"12.3000"}]})") !=
            std::string::npos,
        "unmapped legacy point must keep its original meter and point codes");
    require(payload.find(
            R"({"meterid":"LEGACY_METER_2","metrics":[{"LEGACY_POINT":"45.6000"}]})"
        ) != std::string::npos,
        "mapped legacy point must use the configured logical meter and point codes");
    require(payload.find("P_2") == std::string::npos,
        "mapped legacy point must not remain under its collapsed point code");
    cleanupFixture(fixture);
}

void testLegacyTelemetryMappedOnlyFiltersUnmappedPoints() {
    auto fixture = makeFixture("legacy_mapped_only", 1000);
    fixture.mqttConfig.legacyTelemetryEnabled = true;
    fixture.mqttConfig.legacyTelemetryTopic = "ky/peidian/GW_LEGACY";
    fixture.mqttConfig.legacyTopicMachineCode = "GW_LEGACY";
    fixture.mqttConfig.legacyTelemetryIntervalMs = 1000;
    fixture.mqttConfig.legacyTelemetryMappedOnly = true;
    LegacyTelemetryPointMapping mapping;
    mapping.index = 1002;
    mapping.meterCode = "LEGACY_METER_2";
    mapping.pointCode = "LEGACY_POINT";
    fixture.mqttConfig.legacyTelemetryPointMappings.push_back(mapping);
    LegacyTelemetryPointMapping missingMapping;
    missingMapping.index = 1003;
    missingMapping.meterCode = "LEGACY_METER_2";
    missingMapping.pointCode = "LEGACY_OFFLINE_POINT";
    fixture.mqttConfig.legacyTelemetryPointMappings.push_back(missingMapping);
    fixture.service.reset(new MqttDriverService(
        fixture.mqttConfig,
        fixture.driverConfig,
        {fixture.deviceConfig},
        fixture.router,
        fixture.publisher
    ));

    fixture.service->runScanOnce(1770000000000LL);
    fixture.service->runScanOnce(1770000001000LL);
    const auto legacyIt = std::find(
        fixture.publisher->jsonTopics.begin(),
        fixture.publisher->jsonTopics.end(),
        fixture.mqttConfig.legacyTelemetryTopic
    );
    require(legacyIt != fixture.publisher->jsonTopics.end(), "mapped-only legacy telemetry was not published");
    const auto payloadIndex = static_cast<std::size_t>(legacyIt - fixture.publisher->jsonTopics.begin());
    const auto& payload = fixture.publisher->statusPayloads.at(payloadIndex);
    require(payload.find("METER_1") == std::string::npos,
        "mapped-only legacy telemetry must omit unmapped physical points");
    require(payload.find(
            R"({"meterid":"LEGACY_METER_2","metrics":[{"LEGACY_POINT":"45.6000","LEGACY_OFFLINE_POINT":"0.0000"}]})"
        ) != std::string::npos,
        "mapped-only legacy telemetry must keep mapped logical points and zero-fill offline mappings");
    cleanupFixture(fixture);
}

void testHealthReportContainsDetailedMetrics() {
    const std::string healthFile = "/tmp/mqtt_driver_service_health_test.json";
    std::remove(healthFile.c_str());
    auto fixture = makeFixture("health", 1000, false, healthFile);

    fixture.service->runScanOnce(1770000100000LL);
    fixture.service->runEventReplayOnce(1770000100050LL);
    fixture.service->runScanOnce(1770000101000LL);

    const auto payload = readFile(healthFile);
    require(!payload.empty(), "mqtt health file must be written");
    require(payload.find("\"scanP50Ms\"") != std::string::npos, "mqtt health missing scanP50Ms");
    require(payload.find("\"scanP99Ms\"") != std::string::npos, "mqtt health missing scanP99Ms");
    require(payload.find("\"totalScanCycles\":2") != std::string::npos,
        "mqtt health must count scan cycles");
    require(payload.find("\"totalReplayCycles\":1") != std::string::npos,
        "mqtt health must count replay cycles");
    require(payload.find("\"fullSnapshotsPublished\":1") != std::string::npos,
        "mqtt health must count full snapshots");
    cleanupFixture(fixture);
}

class DriverStatsSource : public IEventStatsSource {
public:
    DriverStatsSource() {
        total.query = mqttDriverTotalStatsQuery();
        business.query = mqttDriverBusinessStatsQuery();
        for (auto* entry : {&total, &business}) {
            entry->status = EventStatsStatus::Fresh;
            entry->valid = true;
            entry->hasValue = true;
            entry->sampledAtUnixMs = 1770000000000LL;
        }
    }
    EventStatsCacheEntry snapshot(const std::string& key) const override {
        if (throws) throw std::runtime_error("injected stats source failure");
        if (key == total.query.key) return total;
        if (key == business.query.key) return business;
        throw std::out_of_range("unexpected stats key");
    }
    bool throws = false;
    EventStatsCacheEntry total, business;
};

void testFullBacklogUsesOnlyValidTotalSnapshot() {
    for (int scenario = 0; scenario < 9; ++scenario) {
        const auto suffix = "stats_backlog_" + std::to_string(scenario);
        auto fixture = makeFixture(suffix, 1000);
        fixture.service.reset();
        const auto path = "/tmp/mqtt_driver_" + suffix + ".db";
        std::remove(path.c_str());
        auto outbox = makeEventOutbox(path);
        auto source = std::make_unique<DriverStatsSource>();
        source->total.value = {2, 20};
        // Actual SQL and business cache are both zero: consulting either would
        // fail to defer, so the valid case proves the total cache is used.
        if (scenario == 1) { source->total.status = EventStatsStatus::NeverSampled; source->total.hasValue = false; }
        if (scenario == 2) { source->total.status = EventStatsStatus::Error; source->total.error = "read failed"; }
        if (scenario == 3) source->total.status = EventStatsStatus::Stale;
        if (scenario == 4) source->total.status = EventStatsStatus::Stopped;
        if (scenario == 5) source->total.query.scope.targetId = "other";
        if (scenario == 6) source->total.backend = EventStatsBackend::Ipc;
        if (scenario == 7) source->throws = true;
        if (scenario == 8) {
            outbox->enqueueBatch({eventMessage("stats-missing-a", "alarm", "unused/a"),
                eventMessage("stats-missing-b", "alarm", "unused/b")});
            source.reset(); // A missing source must not synchronously query this backlog.
        }
        fixture.driverConfig.snapshotBacklogThreshold = 1;
        fixture.driverConfig.snapshotBackoffIntervalMs = 5000;
        fixture.service.reset(new MqttDriverService(fixture.mqttConfig, fixture.driverConfig,
            {fixture.deviceConfig}, fixture.router, fixture.publisher, std::move(outbox),
            nullptr, {}, std::move(source)));
        fixture.service->runScanOnce(1770000000000LL);
        fixture.service->runScanOnce(1770000001100LL);
        require(fixture.publisher->fullSnapshotCounts.size() == (scenario == 0 ? 0U : 1U),
            "unexpected cached backlog decision in scenario " + std::to_string(scenario));
        if (scenario == 0) {
            fixture.service->runScanOnce(1770000005100LL);
            require(fixture.publisher->fullSnapshotCounts.size() == 1,
                "fresh high backlog must still respect the maximum Full deferral");
        }
        cleanupFixture(fixture);
        std::remove(path.c_str());
    }
}

void testStatsHealthReadsCurrentBusinessValidity() {
    const std::string healthFile = "/tmp/mqtt_driver_stats_health.json";
    auto fixture = makeFixture("stats_health", 30000, false, healthFile);
    fixture.service.reset();
    auto source = std::make_unique<DriverStatsSource>();
    auto* mutableSource = source.get();
    source->total.value = {999, 999};
    source->business.value = {5, 50};
    fixture.service.reset(new MqttDriverService(fixture.mqttConfig, fixture.driverConfig,
        {fixture.deviceConfig}, fixture.router, fixture.publisher, nullptr, nullptr, {}, std::move(source)));
    fixture.service->runScanOnce(1770000000000LL);
    auto health = readFile(healthFile);
    require(health.find("\"eventPendingCount\":5,") != std::string::npos,
        "Driver health must use the business scope, not total");
    require(health.find("\"fullBacklogPendingCount\":999,") != std::string::npos,
        "Driver health must separately expose total backlog statistics");
    mutableSource->total.status = EventStatsStatus::Error;
    mutableSource->total.error = "total-query-failed";
    mutableSource->business.status = EventStatsStatus::Stale;
    mutableSource->business.ageMs = 6000;
    fixture.service->runScanOnce(1770000000200LL);
    health = readFile(healthFile);
    require(health.find("\"eventPendingCount\":null") != std::string::npos &&
        health.find("\"eventPendingLastKnownCount\":5,") != std::string::npos &&
        health.find("\"eventStatsStatus\":\"stale\"") != std::string::npos,
        "health must re-read validity and keep stale value diagnostic only");
    require(health.find("\"eventLastAckAtMs\":0") != std::string::npos &&
        health.find("\"eventLastError\":\"\"") != std::string::npos,
        "statistics must not alter ACK or replay error state");
    require(health.find("\"fullBacklogPendingCount\":null") != std::string::npos &&
        health.find("\"fullBacklogStatsError\":\"total-query-failed\"") != std::string::npos,
        "an independent total query failure must remain visible");
    mutableSource->throws = true;
    fixture.service->runScanOnce(1770000000400LL);
    health = readFile(healthFile);
    require(health.find("\"eventStatsStatus\":\"error\"") != std::string::npos &&
        health.find("\"scanFailedCycles\":0") != std::string::npos,
        "statistics source exception must not fail the scan");
    cleanupFixture(fixture);
}

void testIsolatedEventForwarderLeaseDelegatesBusinessEventsOnly() {
    auto fixture = makeFixture("event_delegate", 30000);
    fixture.service.reset();
    const auto paths = configureIsolatedEventOwnership(fixture, "event_delegate");
    auto outbox = makeEventOutbox(paths.database);
    require(outbox->enqueueBatch({
        eventMessage("delegated-alarm", "alarm", "test/event/alarm"),
        eventMessage("delegated-change", "change", "test/event/change"),
        eventMessage("driver-management", "ota_status", "test/event/management")
    }).size() == 3, "failed to seed delegated event outbox");
    fixture.service.reset(new MqttDriverService(
        fixture.mqttConfig,
        fixture.driverConfig,
        {fixture.deviceConfig},
        fixture.router,
        fixture.publisher,
        std::move(outbox)
    ));

    const auto monotonicNow = monotonicNowMs();
    writeEventForwarderHealth(
        fixture,
        paths,
        monotonicNow,
        monotonicNow + fixture.driverConfig.fullUploadWorker.failoverTimeoutMs
    );
    fixture.service->runEventReplayOnce(1770001000000LL);

    require(publishedTopicCount(*fixture.publisher, "test/event/alarm") == 0,
        "fresh forwarder lease must keep alarm ownership out of the driver");
    require(publishedTopicCount(*fixture.publisher, "test/event/change") == 0,
        "fresh forwarder lease must keep change ownership out of the driver");
    require(publishedTopicCount(*fixture.publisher, "test/event/management") == 1,
        "fresh forwarder lease must not block driver management-event replay");

    fixture.service.reset();
    auto observer = makeEventOutbox(paths.database);
    require(observer->pendingCount("main", businessEventFilter()) == 2,
        "delegated business events must remain pending for the forwarder");
    require(observer->pendingCount("main", managementEventFilter()) == 0,
        "driver management event must be acknowledged independently");
    observer.reset();
    cleanupFixture(fixture);
    removeEventOwnershipFiles(paths);
}

void testIsolatedEventForwarderClaimKeepsBusinessEventDelegation() {
    auto fixture = makeFixture("event_delegate_claiming", 30000);
    fixture.service.reset();
    const auto paths = configureIsolatedEventOwnership(fixture, "event_delegate_claiming");
    auto outbox = makeEventOutbox(paths.database);
    require(outbox->enqueueBatch({
        eventMessage("claiming-alarm", "alarm", "test/claiming/alarm"),
        eventMessage("claiming-change", "change", "test/claiming/change"),
        eventMessage("claiming-management", "ota_status", "test/claiming/management")
    }).size() == 3, "failed to seed claiming event outbox");
    fixture.service.reset(new MqttDriverService(
        fixture.mqttConfig,
        fixture.driverConfig,
        {fixture.deviceConfig},
        fixture.router,
        fixture.publisher,
        std::move(outbox)
    ));

    const auto monotonicNow = monotonicNowMs();
    writeEventForwarderHealth(
        fixture,
        paths,
        monotonicNow,
        monotonicNow + fixture.driverConfig.fullUploadWorker.failoverTimeoutMs,
        true,
        true,
        "claiming",
        false
    );
    fixture.service->runEventReplayOnce(1770001050000LL);

    require(publishedTopicCount(*fixture.publisher, "test/claiming/alarm") == 0,
        "an in-progress Full claim must not return alarm ownership to the driver");
    require(publishedTopicCount(*fixture.publisher, "test/claiming/change") == 0,
        "an in-progress Full claim must not return change ownership to the driver");
    require(publishedTopicCount(*fixture.publisher, "test/claiming/management") == 1,
        "an in-progress Full claim must not block driver management-event replay");

    fixture.service.reset();
    auto observer = makeEventOutbox(paths.database);
    require(observer->pendingCount("main", businessEventFilter()) == 2,
        "claiming Forwarder must retain ownership of business events");
    require(observer->pendingCount("main", managementEventFilter()) == 0,
        "driver must still acknowledge management events during a Full claim");
    observer.reset();
    cleanupFixture(fixture);
    removeEventOwnershipFiles(paths);
}

void testIndependentEventLeaseValidationAndLegacyCompatibility() {
    auto fixture = makeFixture("event_lease_versions", 30000);
    fixture.service.reset();
    const auto paths = configureIsolatedEventOwnership(fixture, "event_lease_versions");
    auto outbox = makeEventOutbox(paths.database);
    auto* writer = outbox.get();
    fixture.service.reset(new MqttDriverService(fixture.mqttConfig, fixture.driverConfig,
        {fixture.deviceConfig}, fixture.router, fixture.publisher, std::move(outbox)));

    const auto now = monotonicNowMs();
    const std::string heartbeat = ",\"eventHeartbeatMonotonicMs\":" + std::to_string(now);
    const std::string deadline = ",\"eventLeaseUntilMonotonicMs\":" + std::to_string(now + 3000);
    const std::string version = ",\"eventLeaseVersion\":1";
    struct Case { std::string name; std::string fields; bool active; bool fullActive; };
    const std::vector<Case> cases{
        {"legacy-live", "", true, true},
        {"legacy-expired", "", false, false},
        {"event-live-full-failed", version + heartbeat + deadline, true, false},
        {"event-expired-full-live", version + heartbeat +
            ",\"eventLeaseUntilMonotonicMs\":" + std::to_string(now - 1), false, true},
        {"missing-version", heartbeat + deadline, false, true},
        {"missing-heartbeat", version + deadline, false, true},
        {"missing-deadline", version + heartbeat, false, true},
        {"version-only", version, false, true},
        {"unknown-version", ",\"eventLeaseVersion\":2" + heartbeat + deadline, false, true},
        {"string-version", ",\"eventLeaseVersion\":\"1\"" + heartbeat + deadline, false, true},
        {"fraction-version", ",\"eventLeaseVersion\":1.5" + heartbeat + deadline, false, true},
        {"null-version", ",\"eventLeaseVersion\":null" + heartbeat + deadline, false, true},
        {"duplicate-version", version + version + heartbeat + deadline, false, true},
        {"duplicate-deadline", version + heartbeat + deadline + deadline, false, true},
        {"negative-heartbeat", version + ",\"eventHeartbeatMonotonicMs\":-1" + deadline, false, true},
        {"future-heartbeat", version + ",\"eventHeartbeatMonotonicMs\":" +
            std::to_string(now + 20000) + ",\"eventLeaseUntilMonotonicMs\":" +
            std::to_string(now + 21000), false, true},
        {"too-long-lease", version + heartbeat + ",\"eventLeaseUntilMonotonicMs\":" +
            std::to_string(now + 30000), false, true},
        {"overflow-deadline", version + heartbeat + ",\"eventLeaseUntilMonotonicMs\":1e100", false, true},
        {"unsafe-integer", version + heartbeat + ",\"eventLeaseUntilMonotonicMs\":9007199254740992", false, true},
        {"nested-fields", ",\"unrelated\":{\"eventLeaseVersion\":2}", true, true},
    };
    auto wall = 1770001500000LL;
    for (const auto& item : cases) {
        writer->enqueueBatch({eventMessage(item.name, "alarm", "test/lease-alarm")});
        writeEventForwarderHealth(fixture, paths, now, item.fullActive ? now + 3000 : 0,
            true, true, item.fullActive ? "active" : "retrying", item.fullActive, item.fields);
        const auto before = publishedTopicCount(*fixture.publisher, "test/lease-alarm");
        fixture.service->runEventReplayOnce(wall);
        require((publishedTopicCount(*fixture.publisher, "test/lease-alarm") == before) == item.active,
            "event lease validation mismatch: " + item.name);
        // Drain delegated rows separately so each following case has one candidate.
        writer->replay([](const std::string&, const std::string&) {});
        wall += 100;
    }
    cleanupFixture(fixture);
    removeEventOwnershipFiles(paths);
}

void testIsolatedEventFallbackConsumesMainWhenLeaseMissingOrExpired() {
    const auto verifyFallback = [](const std::string& suffix, bool writeExpiredLease) {
        auto fixture = makeFixture(suffix, 30000);
        fixture.service.reset();
        const auto paths = configureIsolatedEventOwnership(fixture, suffix);
        auto outbox = makeEventOutbox(paths.database);
        require(outbox->enqueueBatch({
            eventMessage(suffix + "-alarm", "alarm", "test/fallback/alarm"),
            eventMessage(suffix + "-change", "change", "test/fallback/change")
        }).size() == 2, "failed to seed fallback event outbox");
        fixture.service.reset(new MqttDriverService(
            fixture.mqttConfig,
            fixture.driverConfig,
            {fixture.deviceConfig},
            fixture.router,
            fixture.publisher,
            std::move(outbox)
        ));
        if (writeExpiredLease) {
            const auto monotonicNow = monotonicNowMs();
            writeEventForwarderHealth(
                fixture,
                paths,
                monotonicNow - fixture.driverConfig.fullUploadWorker.failoverTimeoutMs - 1,
                monotonicNow - 1
            );
        }

        fixture.service->runEventReplayOnce(1770001100000LL);
        require(publishedTopicCount(*fixture.publisher, "test/fallback/alarm") == 1,
            "missing or expired forwarder lease must fail over alarm replay to the driver");
        require(publishedTopicCount(*fixture.publisher, "test/fallback/change") == 1,
            "missing or expired forwarder lease must fail over change replay to the driver");

        fixture.service.reset();
        auto observer = makeEventOutbox(paths.database);
        require(observer->pendingCount("main", businessEventFilter()) == 0,
            "driver fallback must acknowledge main business events after publish");
        observer.reset();
        cleanupFixture(fixture);
        removeEventOwnershipFiles(paths);
    };

    verifyFallback("event_missing_lease", false);
    verifyFallback("event_expired_lease", true);
}

void testIsolatedEventFallbackConsumesMainWhenForwardingIsDisabled() {
    auto fixture = makeFixture("event_forwarding_disabled", 30000);
    fixture.service.reset();
    const auto paths = configureIsolatedEventOwnership(fixture, "event_forwarding_disabled");
    auto outbox = makeEventOutbox(paths.database);
    require(outbox->enqueueBatch({
        eventMessage("disabled-alarm", "alarm", "test/disabled/alarm"),
        eventMessage("disabled-change", "change", "test/disabled/change")
    }).size() == 2, "failed to seed disabled-forwarding event outbox");
    fixture.service.reset(new MqttDriverService(
        fixture.mqttConfig,
        fixture.driverConfig,
        {fixture.deviceConfig},
        fixture.router,
        fixture.publisher,
        std::move(outbox)
    ));

    const auto monotonicNow = monotonicNowMs();
    writeEventForwarderHealth(
        fixture,
        paths,
        monotonicNow,
        monotonicNow + fixture.driverConfig.fullUploadWorker.failoverTimeoutMs,
        false
    );
    fixture.service->runEventReplayOnce(1770001150000LL);

    require(publishedTopicCount(*fixture.publisher, "test/disabled/alarm") == 1,
        "fresh health with event forwarding disabled must return alarm ownership to the driver");
    require(publishedTopicCount(*fixture.publisher, "test/disabled/change") == 1,
        "fresh health with event forwarding disabled must return change ownership to the driver");

    fixture.service.reset();
    auto observer = makeEventOutbox(paths.database);
    require(observer->pendingCount("main", businessEventFilter()) == 0,
        "driver fallback must acknowledge business events when forwarding is disabled");
    observer.reset();
    cleanupFixture(fixture);
    removeEventOwnershipFiles(paths);
}

void testIsolatedEventFallbackDoesNotConsumeWhenReplayLockIsBusy() {
    auto fixture = makeFixture("event_lock_busy", 30000);
    fixture.service.reset();
    const auto paths = configureIsolatedEventOwnership(fixture, "event_lock_busy");
    auto outbox = makeEventOutbox(paths.database);
    require(outbox->enqueueBatch({
        eventMessage("busy-alarm", "alarm", "test/busy/alarm"),
        eventMessage("busy-change", "change", "test/busy/change"),
        eventMessage("busy-management", "ota_status", "test/busy/management")
    }).size() == 3, "failed to seed lock-busy event outbox");
    fixture.service.reset(new MqttDriverService(
        fixture.mqttConfig,
        fixture.driverConfig,
        {fixture.deviceConfig},
        fixture.router,
        fixture.publisher,
        std::move(outbox)
    ));

    ProcessFileLock competingOwner(paths.replayLock);
    require(competingOwner.tryAcquire(), "failed to acquire competing event replay lock");
    fixture.service->runEventReplayOnce(1770001200000LL);

    require(publishedTopicCount(*fixture.publisher, "test/busy/alarm") == 0,
        "driver must not replay alarms while event ownership lock is busy");
    require(publishedTopicCount(*fixture.publisher, "test/busy/change") == 0,
        "driver must not replay changes while event ownership lock is busy");
    require(publishedTopicCount(*fixture.publisher, "test/busy/management") == 1,
        "business-event ownership contention must not block driver management events");

    fixture.service.reset();
    auto observer = makeEventOutbox(paths.database);
    require(observer->pendingCount("main", businessEventFilter()) == 2 &&
            observer->pendingCount("main", managementEventFilter()) == 0,
        "lock-busy replay must preserve business rows and acknowledge management rows");
    observer.reset();
    cleanupFixture(fixture);
    removeEventOwnershipFiles(paths);
}

void testEventDelegationReadyFileFollowsServiceLifecycle() {
    auto fixture = makeFixture("event_ready_lifecycle", 30000);
    fixture.service.reset();
    const auto paths = configureIsolatedEventOwnership(fixture, "event_ready_lifecycle");
    fixture.driverConfig.scanIntervalMs = 20;
    fixture.service.reset(new MqttDriverService(
        fixture.mqttConfig,
        fixture.driverConfig,
        {fixture.deviceConfig},
        fixture.router,
        fixture.publisher,
        makeEventOutbox(paths.database)
    ));

    fixture.service->start();
    const auto ready = readFile(paths.delegationReady);
    require(!ready.empty(), "service start must publish the event delegation capability file");
    require(ready.find("\"dataPlaneVersion\":2") != std::string::npos,
        "delegation capability must identify the external outbox data plane");
    require(ready.find("\"externalOutbox\":true") != std::string::npos,
        "delegation capability must advertise external outbox ownership");
    require(ready.find(std::string("\"eventReplayLockFile\":\"") + paths.replayLock + "\"") !=
            std::string::npos,
        "delegation capability must publish the event replay lock path");

    fixture.service->stop();
    require(readFile(paths.delegationReady).empty(),
        "service stop must remove the event delegation capability file");
    cleanupFixture(fixture);
    removeEventOwnershipFiles(paths);
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

struct DriverReplayScript {
    int created = 0, destroyed = 0, calls = 0;
    bool wrongThread = false;
    std::vector<std::size_t> budgets;
    std::function<MqttEventReplayProgress(const MqttEventReplayRequest&, bool)> step;
};

class DriverScriptedReplay : public IMqttEventReplay {
public:
    DriverScriptedReplay(DriverReplayScript& script, MqttEventReplayRequest request)
        : script_(script), request_(std::move(request)), owner_(std::this_thread::get_id()) {
        ++script_.created;
        script_.budgets.push_back(request_.maxBytes);
    }
    ~DriverScriptedReplay() override {
        ++script_.destroyed;
        script_.wrongThread = script_.wrongThread || owner_ != std::this_thread::get_id();
    }
    MqttEventReplayProgress runOnce(bool drainOnly) override {
        ++script_.calls;
        script_.wrongThread = script_.wrongThread || owner_ != std::this_thread::get_id();
        return script_.step(request_, drainOnly);
    }
private:
    DriverReplayScript& script_;
    MqttEventReplayRequest request_;
    std::thread::id owner_;
};

MqttEventReplayFactory driverReplayFactory(DriverReplayScript& script, bool management) {
    return [&script, management](const MqttEventReplayRequest& request) {
        require(request.lane == (management ? MqttEventReplayLane::MainManagement : MqttEventReplayLane::MainBusiness) &&
            request.targetId == "main" && request.maxMessages == 16 && request.excludeTypes.empty(), "IPC role mismatch");
        require(request.includeTypes == (management ? std::vector<std::string>{"ota_status"} :
            std::vector<std::string>{"alarm", "change"}), "IPC event scope mismatch");
        require(request.authorized(), "factory called without authority");
        return std::unique_ptr<IMqttEventReplay>(new DriverScriptedReplay(script, request));
    };
}

void testIpcDriverBudgetsAndCadence() {
    auto fixture = makeFixture("ipc_budgets", 1000);
    fixture.service.reset();
    fixture.mqttConfig.eventOutboxSqlitePath = "/does-not-exist/no-legacy.db";
    fixture.driverConfig.eventReplayMaxBytes = 1000;
    DriverReplayScript business, management;
    business.step = [](const auto& request, bool drain) {
        require(!drain && request.authorized(), "unexpected business drain");
        MqttEventReplayProgress result;
        result.healthy = true;
        result.attemptedBytes = 600;
        result.ackedCount = result.alarmCount = 1;
        return result;
    };
    management.step = [](const auto& request, bool drain) {
        require(!drain && request.authorized(), "unexpected management drain");
        MqttEventReplayProgress result;
        result.healthy = true;
        return result;
    };
    fixture.service.reset(new MqttDriverService(fixture.mqttConfig, fixture.driverConfig,
        {fixture.deviceConfig}, fixture.router, fixture.publisher, nullptr, nullptr, {}, nullptr,
        driverReplayFactory(business, false), driverReplayFactory(management, true), {"ipc-store", "gen"}));
    require(business.created == 0 && management.created == 0, "constructor acquired IPC actor");
    fixture.service->runEventReplayOnce(1000);
    require(business.budgets == std::vector<std::size_t>{1000} && management.budgets == std::vector<std::size_t>{400},
        "management did not receive remaining byte budget");
    require(business.destroyed == 1 && management.destroyed == 1, "idle sender retained actor lock");
    fixture.service->runEventReplayOnce(1001);
    require(business.calls == 1 && management.calls == 1, "IPC changed legacy replay cadence");
    fixture.service->runEventReplayOnce(1100);
    require(business.calls == 2 && management.calls == 2, "next replay cadence missed");
    business.step = [&](const auto&, bool) {
        MqttEventReplayProgress result;
        result.healthy = true;
        result.attemptedBytes = business.calls == 3 ? 100 : 900;
        return result;
    };
    management.step = [&](const auto& request, bool drain) {
        MqttEventReplayProgress result;
        require(request.maxBytes == 900, "pending management request was replaced");
        if (management.calls == 3) {
            require(!drain, "new management Claim unnecessarily drained");
            result.pending = true;
        } else require(drain && !request.authorized(), "smaller remainder resumed an oversized pending Claim");
        return result;
    };
    fixture.service->runEventReplayOnce(1200);
    fixture.service->runEventReplayOnce(1300);
    require(management.created == 3 && management.destroyed == 3, "management pending Claim was recreated or abandoned");
    cleanupFixture(fixture);
}

void testIpcDriverDelegationDrainsBeforeUnlock() {
    auto fixture = makeFixture("ipc_drain", 30000);
    fixture.service.reset();
    const auto paths = configureIsolatedEventOwnership(fixture, "ipc_drain");
    DriverReplayScript business, management;
    const auto delegate = [&] {
        const auto now = monotonicNowMs();
        writeEventForwarderHealth(fixture, paths, now, now + 3000, true, true, "active", true,
            ",\"eventReplayBackend\":\"ipc-lab\",\"eventStoreId\":\"ipc-store\",\"eventStoreConfigGeneration\":\"gen\"");
    };
    business.step = [&](const auto& request, bool drain) {
        MqttEventReplayProgress result;
        if (business.calls == 1) {
            require(!drain && request.authorized(), "business started without authority");
            delegate();
            require(!request.authorized(), "callback did not observe delegation change");
            result.pending = true;
            result.attemptedBytes = 200;
        } else {
            require(drain && !request.authorized(), "lost authority did not latch drain-only");
            result.pending = business.calls == 2;
            if (!result.pending) result.ackedCount = 1;
        }
        return result;
    };
    management.step = [](const auto& request, bool drain) {
        MqttEventReplayProgress result;
        result.healthy = !drain && request.authorized();
        return result;
    };
    fixture.service.reset(new MqttDriverService(fixture.mqttConfig, fixture.driverConfig,
        {fixture.deviceConfig}, fixture.router, fixture.publisher, nullptr, nullptr, {}, nullptr,
        driverReplayFactory(business, false), driverReplayFactory(management, true), {"ipc-store", "gen"}));
    delegate();
    fixture.service->runEventReplayOnce(1000);
    require(business.created == 0 && management.created == 1, "standby occupied business actor or blocked OTA");
    std::remove(paths.workerHealth.c_str());
    fixture.service->runEventReplayOnce(1100);
    require(business.created == 1 && business.destroyed == 0, "unknown batch lost actor");
    {
        ProcessFileLock competitor(paths.replayLock);
        require(!competitor.tryAcquire(), "pending batch released shared replay lock");
    }
    fixture.service->runEventReplayOnce(1101);
    require(business.created == 1 && business.calls == 2 && business.destroyed == 0,
        "lost authority did not retry pending before cadence");
    fixture.service->runEventReplayOnce(1102);
    require(business.destroyed == 1 && !business.wrongThread, "drained actor not destroyed on owner thread");
    { ProcessFileLock competitor(paths.replayLock); require(competitor.tryAcquire(), "drained replay lock retained"); }
    fixture.service->runEventReplayOnce(1200);
    require(business.created == 1 && management.created == 3, "standby recreated sender or suppressed management");
    require(!std::ifstream(paths.database).good(), "IPC constructed a legacy event database");
    cleanupFixture(fixture);
    removeEventOwnershipFiles(paths);
}

void testIpcDriverWorkerCadence(int workMs, bool fail) {
    auto fixture = makeFixture("ipc_cadence_" + std::to_string(workMs) + (fail ? "_fail" : "_ok"), 30000);
    fixture.service.reset();
    fixture.driverConfig.scanIntervalMs = 200;
    fixture.driverConfig.deliveryMaxLatencyMs = 200;
    DriverReplayScript business, management;
    std::vector<std::chrono::steady_clock::time_point> starts, finishes;
    std::atomic<std::size_t> completed{0};
    business.step = [&](const auto&, bool drain) {
        MqttEventReplayProgress result;
        if (drain) return result;
        starts.push_back(std::chrono::steady_clock::now());
        std::this_thread::sleep_for(std::chrono::milliseconds(workMs));
        finishes.push_back(std::chrono::steady_clock::now());
        completed.store(finishes.size());
        if (fail) throw std::runtime_error("injected replay failure");
        result.healthy = true;
        if (workMs) result.ackedCount = result.changeCount = 16;
        return result;
    };
    management.step = [](const auto&, bool) { MqttEventReplayProgress result; result.healthy = true; return result; };
    fixture.service.reset(new MqttDriverService(fixture.mqttConfig, fixture.driverConfig,
        {fixture.deviceConfig}, fixture.router, fixture.publisher, nullptr, nullptr, {}, nullptr,
        driverReplayFactory(business, false), driverReplayFactory(management, true), {"ipc-store", "gen"}));
    fixture.service->start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (completed.load() < 3 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    fixture.service->stop();
    require(completed.load() >= 3, "replay worker did not finish cadence samples");
    for (std::size_t i = 1; i < 3; ++i) {
        const auto gap = std::chrono::duration<double, std::milli>(starts[i] - finishes[i - 1]).count();
        const auto cycle = std::chrono::duration<double, std::milli>(starts[i] - starts[i - 1]).count();
        std::cout << "ipc cadence work=" << workMs << " fail=" << fail << " gapMs=" << gap << " cycleMs=" << cycle << '\n';
        require(gap >= 8, "replay worker busy-looped without minimum yield");
        if (workMs >= 200) require(gap < 150, "slow replay added a full post-work scheduling interval");
        else require(cycle >= 180, "empty/failed replay lost its configured cadence");
    }
    require(!business.wrongThread && !management.wrongThread, "cadence moved sender ownership across threads");
    cleanupFixture(fixture);
}

void testIpcDriverShutdownDrainsOnWorker() {
    auto fixture = makeFixture("ipc_worker", 30000);
    fixture.service.reset();
    DriverReplayScript business, management;
    std::atomic<bool> attempted{false};
    business.step = [&](const auto& request, bool drain) {
        MqttEventReplayProgress result;
        result.pending = !drain;
        if (drain) require(!request.authorized(), "shutdown allowed network");
        attempted.store(true);
        return result;
    };
    management.step = [](const auto&, bool) { MqttEventReplayProgress result; result.healthy = true; return result; };
    fixture.service.reset(new MqttDriverService(fixture.mqttConfig, fixture.driverConfig,
        {fixture.deviceConfig}, fixture.router, fixture.publisher, nullptr, nullptr, {}, nullptr,
        driverReplayFactory(business, false), driverReplayFactory(management, true), {"ipc-store", "gen"}));
    fixture.service->start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!attempted.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    fixture.service->stop();
    require(attempted && business.destroyed == 1 && !business.wrongThread && !management.wrongThread,
        "shutdown dropped pending batch or destroyed sender on caller thread");
    cleanupFixture(fixture);
}

void testIpcDriverStatsIdentityAndBackoff() {
    for (bool wrongIdentity : {false, true}) {
        const auto healthFile = std::string("/tmp/ipc_driver_stats_") + (wrongIdentity ? "wrong" : "right") + ".json";
        auto fixture = makeFixture(wrongIdentity ? "ipc_wrong_stats" : "ipc_right_stats", 1000, false, healthFile);
        fixture.service.reset();
        fixture.driverConfig.snapshotBacklogThreshold = 1;
        fixture.driverConfig.snapshotBackoffIntervalMs = 5000;
        auto source = std::make_unique<DriverStatsSource>();
        for (auto* entry : {&source->total, &source->business}) {
            entry->backend = EventStatsBackend::Ipc;
            entry->identity = {"ipc-store", wrongIdentity ? "foreign-gen" : "gen"};
            entry->value = {9, 90};
        }
        DriverReplayScript business, management;
        fixture.service.reset(new MqttDriverService(fixture.mqttConfig, fixture.driverConfig,
            {fixture.deviceConfig}, fixture.router, fixture.publisher, nullptr, nullptr, {}, std::move(source),
            driverReplayFactory(business, false), driverReplayFactory(management, true), {"ipc-store", "gen"}));
        fixture.service->runScanOnce(1770000000000LL);
        fixture.service->runScanOnce(1770000001100LL);
        require(fixture.publisher->fullSnapshotCounts.size() == (wrongIdentity ? 1u : 0u), "IPC stats identity/backoff incorrect");
        const auto health = readFile(healthFile);
        require(health.find(wrongIdentity ? "\"eventStatsValid\":false" : "\"eventStatsValid\":true") != std::string::npos,
            "IPC stats identity not reflected in health");
        require(business.created == 0 && management.created == 0, "statistics constructed sender actor");
        cleanupFixture(fixture);
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--realtime-admission") {
            testRealtimeRejectsInvalidTimingBeforePublish();
            testRealtimeSelectorsAreBoundedWithoutAllFallback();
            testRealtimeCapacityRenewStopAndExpiry();
            testRealtimeRenewFloodPreservesPendingDue();
            testRealtimeIdentityBoundsAndStudioCompatibility();
            testRealtimeLongDefaultStopExceptionIsExact();
            testRealtimeTimingBoundariesDefaultsAndDeadlineOverflow();
            return 0;
        }
        testIpcDriverBudgetsAndCadence();
        testIpcDriverDelegationDrainsBeforeUnlock();
        testIpcDriverShutdownDrainsOnWorker();
        for (int workMs : {0, 250}) for (bool fail : {false, true})
            testIpcDriverWorkerCadence(workMs, fail);
        testIpcDriverStatsIdentityAndBackoff();
        testFullUploadOnlyWithoutRealtimeSession();
        testIsolatedFullWorkerUsesBoundedStartupGrace();
        testIsolatedFullWorkerLeaseSuppressesAndThenImmediatelyFallsBack();
        testIsolatedFallbackRetriesWithoutAnotherStartupGrace();
        testFullUploadPointSelectionIsSharedWithForwarder();
        testLegacyTelemetryUsesOldTopicAndPayloadShape();
        testLegacyTelemetryCanPublishFasterThanFullSnapshot();
        testLegacyTelemetryRestoresLogicalMeterMapping();
        testLegacyTelemetryMappedOnlyFiltersUnmappedPoints();
        testOneShotRealtimeRequestDoesNotCreatePeriodicSession();
        testRealtimeSessionPublishesUntilTtl();
        testRealtimeSessionStopRequest();
        testRealtimeSessionUnsubscribeIsIsolatedFromOtherSessionsAndFullUpload();
        testRealtimeSessionsKeepMeterAndIndexSelectorsIsolated();
        testRealtimeSessionEchoForEverySelectorAndLegacyDefault();
        testCommandRequestDoesNotCreatePriorityControlLeaseByDefault();
        testCommandWritebackWaitDoesNotBlockMqttScan();
        testHighPriorityCommandRequestCreatesPriorityControlLease();
        testCommandRequestRejectedDuringActivePriorityControl();
        testAgcAvcCommandMailboxCommitsLatestWithoutWriteback();
        testHealthReportContainsDetailedMetrics();
        testFullBacklogUsesOnlyValidTotalSnapshot();
        testStatsHealthReadsCurrentBusinessValidity();
        testIsolatedFullWorkerLeaseIgnoresWallClockRollback();
        testIsolatedEventForwarderLeaseDelegatesBusinessEventsOnly();
        testIsolatedEventForwarderClaimKeepsBusinessEventDelegation();
        testIndependentEventLeaseValidationAndLegacyCompatibility();
        testIsolatedEventFallbackConsumesMainWhenLeaseMissingOrExpired();
        testIsolatedEventFallbackConsumesMainWhenForwardingIsDisabled();
        testIsolatedEventFallbackDoesNotConsumeWhenReplayLockIsBusy();
        testEventDelegationReadyFileFollowsServiceLifecycle();
        testRealtimeRejectsInvalidTimingBeforePublish();
        testRealtimeSelectorsAreBoundedWithoutAllFallback();
        testRealtimeCapacityRenewStopAndExpiry();
        testRealtimeRenewFloodPreservesPendingDue();
        testRealtimeIdentityBoundsAndStudioCompatibility();
        testRealtimeLongDefaultStopExceptionIsExact();
        testRealtimeTimingBoundariesDefaultsAndDeadlineOverflow();
        std::cout << "mqtt_driver_service_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "mqtt_driver_service_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
