#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "edge_gateway/event_engine_service.hpp"
#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace {

using namespace edge_gateway;

std::string gSqliteLibraryPath;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class CapturingPublisher : public IMqttDriverPublisher {
public:
    void publishFullSnapshot(const std::string&, const std::vector<StoredPointValue>&, const std::string&) override {}
    void publishAlarm(const std::string&, std::uint32_t, const StoredPointValue&, const std::string&, bool) override {}
    void publishOnDemand(const std::string&, const std::vector<StoredPointValue>&, const std::string&) override {}
    void publishChangeEvent(const std::string&, const StoredPointValue&) override {}
    void publishCommandReply(const std::string&, const MqttCommandReply&) override {}
    void publishOtaReply(const std::string&, const OtaReply&) override {}
    void publishOtaStatus(const std::string&, const OtaStatus&) override {}
    void publishJsonMessage(const std::string& topic, const std::string& payload) override {
        topics.push_back(topic);
        payloads.push_back(payload);
    }
    std::vector<MqttIncomingMessage> pollIncoming(int) override { return {}; }

    bool hasActiveAlarm(std::uint32_t index) const {
        const auto indexText = std::string("\"index\":") + std::to_string(index);
        for (const auto& payload : payloads) {
            if (payload.find("\"type\":\"alarm\"") != std::string::npos &&
                payload.find(indexText) != std::string::npos &&
                payload.find("\"active\":true") != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    bool hasChange(std::uint32_t index) const {
        const auto indexText = std::string("\"index\":") + std::to_string(index);
        for (const auto& payload : payloads) {
            if (payload.find("\"type\":\"change\"") != std::string::npos &&
                payload.find(indexText) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    std::size_t countEvents(
        const std::string& type,
        std::uint32_t index,
        const std::string& detail = std::string()
    ) const {
        const auto typeText = std::string("\"type\":\"") + type + "\"";
        const auto indexText = std::string("\"index\":") + std::to_string(index);
        std::size_t count = 0;
        for (const auto& payload : payloads) {
            if (payload.find(typeText) != std::string::npos &&
                payload.find(indexText) != std::string::npos &&
                (detail.empty() || payload.find(detail) != std::string::npos)) {
                ++count;
            }
        }
        return count;
    }

    std::string joinedPayloads() const {
        std::string result;
        for (const auto& payload : payloads) {
            if (!result.empty()) result += " | ";
            result += payload;
        }
        return result;
    }

    std::vector<std::string> topics;
    std::vector<std::string> payloads;
};

PointDefinition makePoint(std::uint32_t index, const std::string& code, bool alarm) {
    PointDefinition point;
    point.index = index;
    point.pointCode = code;
    point.enabled = true;
    point.read.enable = true;
    point.reportOnChange = true;
    point.read.cachePolicy.ttlMs = 60000;
    if (alarm) {
        AlarmRuleConfig rule;
        rule.type = "high";
        rule.threshold = 50.0;
        rule.reportRecovery = true;
        point.alarms.push_back(rule);
    }
    return point;
}

PointValue makeValue(std::uint32_t index, double value, std::int64_t ts) {
    PointValue point;
    point.index = index;
    point.value = value;
    point.quality = 1;
    point.ts = ts;
    point.expireAt = ts + 60000;
    return point;
}

struct Fixture {
    std::string storeName;
    MemoryStoreConfig storeConfig;
    std::unique_ptr<MemoryPointStore> store;
    PointStoreRouter router;
    DeviceConfig device;
    std::shared_ptr<CapturingPublisher> publisher;
    std::unique_ptr<EventEngineService> service;
};

std::unique_ptr<MqttEventOutbox> makeEventOutbox(
    const std::string& path,
    std::size_t maxDiskBytes = 8 * 1024 * 1024
) {
    return std::unique_ptr<MqttEventOutbox>(new MqttEventOutbox(
        path,
        gSqliteLibraryPath,
        12,
        24,
        100,
        maxDiskBytes
    ));
}

std::string makeDatabasePath(const std::string& suffix) {
    const auto nonce = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return "event_engine_service_test_" + suffix + "_" + std::to_string(nonce) + ".db";
}

void removeDatabase(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-shm").c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-journal").c_str());
}

Fixture makeFixture(
    const std::string& suffix,
    const std::string& deliveryMode = "hybrid",
    const std::string& outboxPath = std::string(),
    const MqttForwardConfig& forwardConfig = MqttForwardConfig(),
    std::size_t updateDrainBatchSize = 64,
    std::size_t outboxMaxDiskBytes = 8 * 1024 * 1024
) {
    Fixture fixture;
    fixture.storeName = "event_engine_service_test_" + suffix;
    MemoryPointStore::cleanupOrphanedSegment(fixture.storeName);
    fixture.storeConfig.sharedMemoryName = fixture.storeName;
    fixture.storeConfig.maxLatestPoints = 16;
    fixture.store.reset(new MemoryPointStore(fixture.storeConfig));

    fixture.device.machineCode = "GW_EVENT_TEST";
    fixture.device.meterCode = "METER_EVENT_TEST";
    fixture.device.memoryStore.sharedMemoryName = fixture.storeName;
    fixture.device.points = {
        makePoint(610001, "alarm_point", true),
        makePoint(610002, "noise_point", false),
        makePoint(610003, "filtered_point", false)
    };
    fixture.store->registerPoints(
        fixture.device.machineCode,
        fixture.device.meterCode,
        fixture.device.points
    );
    fixture.router.addStore(fixture.storeName, *fixture.store);
    fixture.router.addRoutesFromDeviceConfigs({fixture.device}, fixture.storeName);

    EventEngineConfig eventConfig;
    eventConfig.enabled = true;
    eventConfig.deliveryMode = deliveryMode;
    eventConfig.scanFallbackIntervalMs = 5000;
    eventConfig.updateDrainBatchSize = updateDrainBatchSize;
    MqttConfig mqttConfig;
    mqttConfig.alarmTopic = "edge/alarm";
    mqttConfig.changeEventTopic = "edge/change";
    mqttConfig.statusTopic = "edge/status";
    fixture.publisher.reset(new CapturingPublisher());
    std::unique_ptr<MqttEventOutbox> eventOutbox;
    if (!outboxPath.empty()) {
        eventConfig.publishMode = "mqtt_driver_outbox";
        eventOutbox = makeEventOutbox(outboxPath, outboxMaxDiskBytes);
    }
    fixture.service.reset(new EventEngineService(
        eventConfig,
        mqttConfig,
        {fixture.device},
        fixture.router,
        {fixture.store.get()},
        fixture.publisher,
        std::move(eventOutbox),
        std::unique_ptr<SqliteAlarmWriter>(),
        forwardConfig
    ));
    return fixture;
}

void restartWithOutbox(
    Fixture& fixture,
    const std::string& outboxPath,
    const MqttForwardConfig& forwardConfig = MqttForwardConfig()
) {
    fixture.service.reset();
    EventEngineConfig eventConfig;
    eventConfig.enabled = true;
    eventConfig.deliveryMode = "hybrid";
    eventConfig.scanFallbackIntervalMs = 5000;
    eventConfig.updateDrainBatchSize = 64;
    eventConfig.publishMode = "mqtt_driver_outbox";
    MqttConfig mqttConfig;
    mqttConfig.alarmTopic = "edge/alarm";
    mqttConfig.changeEventTopic = "edge/change";
    mqttConfig.statusTopic = "edge/status";
    fixture.service.reset(new EventEngineService(
        eventConfig,
        mqttConfig,
        {fixture.device},
        fixture.router,
        {fixture.store.get()},
        fixture.publisher,
        makeEventOutbox(outboxPath),
        std::unique_ptr<SqliteAlarmWriter>(),
        forwardConfig
    ));
}

void cleanupFixture(Fixture& fixture) {
    fixture.service.reset();
    fixture.store.reset();
    MemoryPointStore::cleanupOrphanedSegment(fixture.storeName);
}

void testFallbackScanRunsWhileUpdatesKeepArriving() {
    auto fixture = makeFixture("fallback_under_load");
    fixture.store->putLatest(makeValue(610001, 100.0, 1000));
    (void)fixture.store->drainPointUpdates();
    fixture.store->putLatest(makeValue(610002, 1.0, 1001));

    fixture.service->runOnce(1100);

    require(
        fixture.publisher->hasActiveAlarm(610001),
        "due fallback scan must reconcile alarms even when another point keeps updating; payloads=" +
            fixture.publisher->joinedPayloads()
    );
    cleanupFixture(fixture);
}

void testSequenceGapTriggersImmediateReconciliation() {
    auto fixture = makeFixture("sequence_gap");
    fixture.store->putLatest(makeValue(610001, 0.0, 2000));
    fixture.store->putLatest(makeValue(610002, 0.0, 2001));
    fixture.service->runOnce(2100);
    require(!fixture.publisher->hasActiveAlarm(610001), "normal baseline must not activate the alarm");

    fixture.store->putLatest(makeValue(610001, 100.0, 2200));
    (void)fixture.store->drainPointUpdates();
    fixture.store->putLatest(makeValue(610002, 1.0, 2201));
    fixture.service->runOnce(2202);

    require(
        fixture.publisher->hasActiveAlarm(610001),
        "point-update sequence gap must trigger immediate full reconciliation; payloads=" +
            fixture.publisher->joinedPayloads()
    );
    cleanupFixture(fixture);
}

void testPeriodicDeliverySuppressesChangesButNotAlarms() {
    auto fixture = makeFixture("periodic_delivery", "periodic");
    fixture.store->putLatest(makeValue(610001, 0.0, 3000));
    fixture.service->runOnce(3001);

    fixture.store->putLatest(makeValue(610001, 100.0, 3100));
    fixture.service->runOnce(3101);

    require(!fixture.publisher->hasChange(610001), "periodic delivery must suppress report-on-change events");
    require(
        fixture.publisher->hasActiveAlarm(610001),
        "periodic delivery must not suppress alarms; payloads=" + fixture.publisher->joinedPayloads()
    );
    cleanupFixture(fixture);
}

void testRapidRaiseAndClearPreservesEveryTransition() {
    auto fixture = makeFixture("rapid_raise_clear");
    fixture.store->putLatest(makeValue(610001, 0.0, 3200));
    fixture.service->runOnce(3201);

    fixture.store->putLatest(makeValue(610001, 100.0, 3300));
    fixture.store->putLatest(makeValue(610001, 0.0, 3301));
    fixture.service->runOnce(3302);

    require(
        fixture.publisher->countEvents("change", 610001) == 2,
        "0->1->0 in one drained batch must preserve both change events; payloads=" +
            fixture.publisher->joinedPayloads()
    );
    require(
        fixture.publisher->countEvents("alarm", 610001, "\"active\":true") == 1 &&
            fixture.publisher->countEvents("alarm", 610001, "\"active\":false") == 1,
        "0->1->0 in one drained batch must preserve alarm raise and clear; payloads=" +
            fixture.publisher->joinedPayloads()
    );
    cleanupFixture(fixture);
}

void testFallbackScanDoesNotSkipQueuedTransient() {
    auto fixture = makeFixture(
        "fallback_queue_tail",
        "hybrid",
        std::string(),
        MqttForwardConfig(),
        1
    );
    fixture.store->putLatest(makeValue(610001, 0.0, 6000));
    fixture.service->runOnce(6001);

    fixture.store->putLatest(makeValue(610001, 100.0, 12000));
    fixture.store->putLatest(makeValue(610001, 0.0, 12001));
    fixture.store->putLatest(makeValue(610001, 100.0, 12002));
    fixture.service->runOnce(12003);
    fixture.service->runOnce(12004);
    fixture.service->runOnce(12005);
    fixture.service->runOnce(12006);

    require(
        fixture.publisher->countEvents("change", 610001) == 3,
        "fallback scan must not advance past a queued 0->1->0->1 transient; payloads=" +
            fixture.publisher->joinedPayloads()
    );
    require(
        fixture.publisher->countEvents("alarm", 610001, "\"active\":true") == 2 &&
            fixture.publisher->countEvents("alarm", 610001, "\"active\":false") == 1,
        "fallback scan must preserve queued alarm raise/recovery order; payloads=" +
            fixture.publisher->joinedPayloads()
    );
    cleanupFixture(fixture);
}

void testRestartRestoresNormalBaselineWithoutSwallowingFirstChange() {
    const auto outboxPath = makeDatabasePath("restart_baseline");
    removeDatabase(outboxPath);
    auto fixture = makeFixture("restart_baseline", "hybrid", outboxPath);
    fixture.store->putLatest(makeValue(610001, 0.0, 3400));
    fixture.service->runOnce(3401);

    restartWithOutbox(fixture, outboxPath);
    fixture.store->putLatest(makeValue(610001, 100.0, 3500));
    fixture.service->runOnce(3501);

    restartWithOutbox(fixture, outboxPath);
    fixture.store->putLatest(makeValue(610001, 100.0, 3600));
    fixture.service->runOnce(3601);
    fixture.store->putLatest(makeValue(610001, 0.0, 3700));
    fixture.service->runOnce(3701);
    fixture.service.reset();

    auto outbox = makeEventOutbox(outboxPath);
    const auto states = outbox->loadStates();
    require(!states.empty(), "normal change/alarm baselines must survive an EventEngine restart");
    std::vector<std::string> payloads;
    outbox->replay("main", [&](const std::string&, const std::string& payload) {
        payloads.push_back(payload);
    });
    const auto countType = [&](const std::string& type) {
        const auto marker = std::string("\"type\":\"") + type + "\"";
        std::size_t count = 0;
        for (const auto& payload : payloads) {
            if (payload.find(marker) != std::string::npos &&
                payload.find("\"index\":610001") != std::string::npos) {
                ++count;
            }
        }
        return count;
    };
    require(countType("change") == 2,
        "restart must preserve both the first change and its later recovery without duplicates");
    require(countType("alarm") == 2,
        "restored alarm lifecycle must suppress duplicate raises and preserve the recovery");
    require(outbox->pendingCount("third-party") == 0,
        "events.enabled=false must not create third-party target rows");

    outbox.reset();
    cleanupFixture(fixture);
    removeDatabase(outboxPath);
}

MqttForwardConfig makeLegacyForwardConfig(bool inheritIndexes) {
    MqttForwardConfig forward;
    forward.enabled = true;
    forward.pointIndexes = {610001, 610002};
    forward.payloadFormat = "legacy";
    forward.legacyTelemetryMappedOnly = true;
    forward.legacyTelemetryPointMappings = {
        LegacyTelemetryPointMapping{610001, "OLD_METER", "OLD_ALARM"},
        LegacyTelemetryPointMapping{610002, "OLD_METER", "OLD_NOISE"}
    };
    forward.events.enabled = true;
    forward.events.targetId = "partner-a";
    forward.events.changeTopic = "third/change";
    forward.events.alarmTopic = "third/alarm";
    forward.events.changeTopicMachineScoped = false;
    forward.events.alarmTopicMachineScoped = true;
    if (!inheritIndexes) {
        forward.events.pointIndexes = {610001, 610002};
    }
    return forward;
}

std::string joinValues(const std::vector<std::string>& values) {
    std::string result;
    for (const auto& value : values) {
        result += value + "\n";
    }
    return result;
}

void testAtomicMainAndThirdPartyFanoutFilteringAndLegacyMapping() {
    const auto outboxPath = makeDatabasePath("fanout_legacy");
    removeDatabase(outboxPath);
    const auto forward = makeLegacyForwardConfig(false);
    auto fixture = makeFixture("fanout_legacy", "hybrid", outboxPath, forward);
    fixture.store->putLatest(makeValue(610001, 0.0, 4000));
    fixture.store->putLatest(makeValue(610002, 0.0, 4001));
    fixture.store->putLatest(makeValue(610003, 0.0, 4002));
    fixture.service->runOnce(4003);

    fixture.store->putLatest(makeValue(610001, 100.0, 4100));
    fixture.store->putLatest(makeValue(610002, 1.0, 4101));
    fixture.store->putLatest(makeValue(610003, 1.0, 4102));
    fixture.service->runOnce(4103);
    fixture.service.reset();

    auto outbox = makeEventOutbox(outboxPath);
    require(outbox->pendingCount("main") == 4,
        "main target must receive three changes and one alarm");
    require(outbox->pendingCount("partner-a") == 3,
        "filtered third-party target must receive two changes and one alarm");
    const auto states = outbox->loadStates();
    require(states.size() == 4,
        "fanout transaction must persist three change baselines and one alarm lifecycle");

    std::vector<std::string> mainPayloads;
    outbox->replay("main", [&](const std::string&, const std::string& payload) {
        mainPayloads.push_back(payload);
    });
    std::vector<std::string> thirdTopics;
    std::vector<std::string> thirdPayloads;
    outbox->replay("partner-a", [&](const std::string& topic, const std::string& payload) {
        thirdTopics.push_back(topic);
        thirdPayloads.push_back(payload);
    });
    const auto mainJoined = joinValues(mainPayloads);
    const auto thirdJoined = joinValues(thirdPayloads);
    require(mainJoined.find("METER_EVENT_TEST") != std::string::npos &&
            mainJoined.find("alarm_point") != std::string::npos,
        "main payload must retain native meterCode/pointCode");
    require(thirdJoined.find("OLD_METER") != std::string::npos &&
            thirdJoined.find("OLD_ALARM") != std::string::npos &&
            thirdJoined.find("OLD_NOISE") != std::string::npos,
        "legacy third-party payload must use the same mappings as full forwarding");
    require(mainJoined.find("\"schemaVersion\":\"2.0\"") != std::string::npos &&
            mainJoined.find("\"eventId\":\"change:v1:GW_EVENT_TEST:610001:1\"") !=
                std::string::npos &&
            thirdJoined.find("\"eventId\":\"change:v1:GW_EVENT_TEST:610001:1\"") !=
                std::string::npos,
        "main and third-party payloads must expose the same stable eventId for deduplication");
    require(thirdJoined.find("filtered_point") == std::string::npos &&
            thirdJoined.find("\"index\":610003") == std::string::npos,
        "events.pointIndexes must filter third-party changes without filtering main");
    require(std::find(thirdTopics.begin(), thirdTopics.end(), "third/change") != thirdTopics.end(),
        "unscoped third-party change topic changed unexpectedly");
    require(std::find(
            thirdTopics.begin(), thirdTopics.end(), "third/alarm/GW_EVENT_TEST"
        ) != thirdTopics.end(),
        "machine-scoped third-party alarm topic is incorrect");

    outbox.reset();
    cleanupFixture(fixture);
    removeDatabase(outboxPath);
}

void testEmptyEventIndexesInheritForwardIndexes() {
    const auto outboxPath = makeDatabasePath("inherit_filter");
    removeDatabase(outboxPath);
    auto forward = makeLegacyForwardConfig(true);
    forward.events.alarmTopic.clear();
    auto fixture = makeFixture("inherit_filter", "hybrid", outboxPath, forward);
    fixture.store->putLatest(makeValue(610001, 0.0, 5000));
    fixture.store->putLatest(makeValue(610003, 0.0, 5001));
    fixture.service->runOnce(5002);
    fixture.store->putLatest(makeValue(610001, 100.0, 5100));
    fixture.store->putLatest(makeValue(610003, 1.0, 5101));
    fixture.service->runOnce(5102);
    fixture.service.reset();

    auto outbox = makeEventOutbox(outboxPath);
    require(outbox->pendingCount("partner-a") == 1,
        "empty alarmTopic must suppress only third-party alarms while inherited indexes keep changes");
    std::vector<std::string> payloads;
    outbox->replay("partner-a", [&](const std::string&, const std::string& payload) {
        payloads.push_back(payload);
    });
    require(payloads.size() == 1 &&
            payloads.front().find("\"index\":610001") != std::string::npos,
        "inherited third-party filter forwarded an index outside mqttForward.pointIndexes");

    outbox.reset();
    cleanupFixture(fixture);
    removeDatabase(outboxPath);
}

void testEmptyChangeTopicStillForwardsAlarms() {
    const auto outboxPath = makeDatabasePath("alarm_only_forward");
    removeDatabase(outboxPath);
    auto forward = makeLegacyForwardConfig(false);
    forward.events.changeTopic.clear();
    auto fixture = makeFixture("alarm_only_forward", "hybrid", outboxPath, forward);
    fixture.store->putLatest(makeValue(610001, 0.0, 5200));
    fixture.service->runOnce(5201);
    fixture.store->putLatest(makeValue(610001, 100.0, 5300));
    fixture.service->runOnce(5301);
    fixture.service.reset();

    auto outbox = makeEventOutbox(outboxPath);
    require(outbox->pendingCount("partner-a") == 1,
        "empty changeTopic must suppress only third-party changes and keep alarms");
    std::vector<std::string> topics;
    outbox->replay("partner-a", [&](const std::string& topic, const std::string&) {
        topics.push_back(topic);
    });
    require(topics == std::vector<std::string>{"third/alarm/GW_EVENT_TEST"},
        "alarm-only forwarding used the wrong topic");

    outbox.reset();
    cleanupFixture(fixture);
    removeDatabase(outboxPath);
}

void testFullThirdPartyQueueDoesNotRollBackPrimaryAlarm() {
    const auto outboxPath = makeDatabasePath("primary_capacity_isolation");
    removeDatabase(outboxPath);
    constexpr std::size_t maxPendingBytesPerTarget = 600;
    {
        auto seed = makeEventOutbox(outboxPath, maxPendingBytesPerTarget);
        MqttEventOutbox::EventMessage blocked;
        blocked.eventType = "alarm";
        blocked.topic = "third/alarm/GW_EVENT_TEST";
        blocked.payload = std::string(520, 'x');
        blocked.eventTs = 1000;
        blocked.eventId = "third-backlog";
        blocked.targetId = "partner-a";
        require(seed->enqueueBatch({blocked}).size() == 1,
            "failed to seed the third-party alarm backlog");
    }

    auto forward = makeLegacyForwardConfig(false);
    auto fixture = makeFixture(
        "primary_capacity_isolation",
        "hybrid",
        outboxPath,
        forward,
        64,
        maxPendingBytesPerTarget
    );
    fixture.store->putLatest(makeValue(610001, 100.0, 2000));
    fixture.service->runOnce(2001);

    auto observer = makeEventOutbox(outboxPath, maxPendingBytesPerTarget);
    require(observer->pendingCount("main") == 1,
        "a full third-party queue rolled back the primary alarm");
    require(observer->pendingCount("partner-a") == 1,
        "third-party capacity failure changed its existing protected alarm backlog");
    const auto states = observer->loadStates();
    require(std::any_of(states.begin(), states.end(), [](const MqttEventOutbox::EventState& state) {
        return state.eventType == "alarm" && state.index == 610001 && state.active;
    }), "a full third-party queue rolled back the primary alarm lifecycle state");

    observer.reset();
    cleanupFixture(fixture);
    removeDatabase(outboxPath);
}

#ifndef _WIN32
void testBrokenStoreDoesNotBlockHealthyStores() {
    const std::string badName = "event_engine_broken_store_test";
    const std::string goodName = "event_engine_healthy_store_test";
    MemoryPointStore::cleanupOrphanedSegment(badName);
    MemoryPointStore::cleanupOrphanedSegment(goodName);

    MemoryStoreConfig badConfig;
    badConfig.sharedMemoryName = badName;
    badConfig.maxLatestPoints = 16;
    MemoryStoreConfig goodConfig = badConfig;
    goodConfig.sharedMemoryName = goodName;
    auto badStore = std::unique_ptr<MemoryPointStore>(new MemoryPointStore(badConfig));
    auto goodStore = std::unique_ptr<MemoryPointStore>(new MemoryPointStore(goodConfig));

    DeviceConfig goodDevice;
    goodDevice.machineCode = "GW_HEALTHY";
    goodDevice.meterCode = "METER_HEALTHY";
    goodDevice.memoryStore.sharedMemoryName = goodName;
    goodDevice.points = {makePoint(620001, "healthy_alarm", true)};
    goodStore->registerPoints(goodDevice.machineCode, goodDevice.meterCode, goodDevice.points);

    PointStoreRouter router;
    router.addStore(badName, *badStore);
    router.addStore(goodName, *goodStore);
    router.addRoutesFromDeviceConfigs({goodDevice}, goodName);
    EventEngineConfig eventConfig;
    eventConfig.enabled = true;
    eventConfig.scanFallbackIntervalMs = 5000;
    MqttConfig mqttConfig;
    mqttConfig.alarmTopic = "edge/alarm";
    mqttConfig.statusTopic = "edge/status";
    auto publisher = std::make_shared<CapturingPublisher>();
    EventEngineService service(
        eventConfig,
        mqttConfig,
        {goodDevice},
        router,
        {badStore.get(), goodStore.get()},
        publisher
    );

    goodStore->putLatest(makeValue(620001, 100.0, 4000));
    require(MemoryPointStore::cleanupOrphanedSegment(badName), "failed to unlink bad store segment");
    const auto badPosixName = "/" + badName;
    const int badFd = shm_open(badPosixName.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    require(badFd >= 0, "failed to create malformed replacement segment");
    require(ftruncate(badFd, 64) == 0, "failed to size malformed replacement segment");
    close(badFd);

    service.runOnce(4001);
    require(
        publisher->hasActiveAlarm(620001),
        "one malformed shared segment must not block healthy device alarms"
    );

    shm_unlink(badPosixName.c_str());
    goodStore.reset();
    badStore.reset();
    MemoryPointStore::cleanupOrphanedSegment(goodName);
}
#endif

}  // namespace

int main(int argc, char** argv) {
    const char* environmentLibrary = std::getenv("SQLITE3_LIBRARY_PATH");
    gSqliteLibraryPath = argc > 1
        ? argv[1]
        : (environmentLibrary == nullptr ? std::string() : std::string(environmentLibrary));
    try {
        testFallbackScanRunsWhileUpdatesKeepArriving();
        testSequenceGapTriggersImmediateReconciliation();
        testPeriodicDeliverySuppressesChangesButNotAlarms();
        testRapidRaiseAndClearPreservesEveryTransition();
        testFallbackScanDoesNotSkipQueuedTransient();
        testRestartRestoresNormalBaselineWithoutSwallowingFirstChange();
        testAtomicMainAndThirdPartyFanoutFilteringAndLegacyMapping();
        testEmptyEventIndexesInheritForwardIndexes();
        testEmptyChangeTopicStillForwardsAlarms();
        testFullThirdPartyQueueDoesNotRollBackPrimaryAlarm();
#ifndef _WIN32
        testBrokenStoreDoesNotBlockHealthyStores();
#endif
        std::cout << "event_engine_service_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "event_engine_service_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
