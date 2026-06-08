#include <cstdio>
#include <iostream>
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
    void publishFullSnapshot(
        const std::string&,
        const std::vector<StoredPointValue>& values,
        const std::string&
    ) override {
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
        const std::string&,
        const std::vector<StoredPointValue>& values,
        const std::string&
    ) override {
        onDemandCounts.push_back(values.size());
    }

    void publishChangeEvent(
        const std::string&,
        const StoredPointValue&
    ) override {
    }

    void publishCommandReply(
        const std::string&,
        const MqttCommandReply&
    ) override {
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
    std::vector<std::size_t> fullSnapshotCounts;
    std::vector<std::size_t> onDemandCounts;
    std::vector<std::string> statusPayloads;
    std::vector<int> pollTimeouts;
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

ServiceFixture makeFixture(const std::string& suffix, int fullUploadIntervalMs) {
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
    fixture.deviceConfig.meters.push_back(meter);

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

void cleanupFixture(ServiceFixture& fixture) {
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

}  // namespace

int main() {
    try {
        testFullUploadOnlyWithoutRealtimeSession();
        testOneShotRealtimeRequestDoesNotCreatePeriodicSession();
        testRealtimeSessionPublishesUntilTtl();
        testRealtimeSessionStopRequest();
        std::cout << "mqtt_driver_service_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "mqtt_driver_service_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
