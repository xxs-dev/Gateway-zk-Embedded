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

Fixture makeFixture(const std::string& suffix, const std::string& deliveryMode = "hybrid") {
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
        makePoint(610002, "noise_point", false)
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
    eventConfig.updateDrainBatchSize = 64;
    MqttConfig mqttConfig;
    mqttConfig.alarmTopic = "edge/alarm";
    mqttConfig.statusTopic = "edge/status";
    fixture.publisher.reset(new CapturingPublisher());
    fixture.service.reset(new EventEngineService(
        eventConfig,
        mqttConfig,
        {fixture.device},
        fixture.router,
        {fixture.store.get()},
        fixture.publisher
    ));
    return fixture;
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

int main() {
    try {
        testFallbackScanRunsWhileUpdatesKeepArriving();
        testSequenceGapTriggersImmediateReconciliation();
        testPeriodicDeliverySuppressesChangesButNotAlarms();
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
