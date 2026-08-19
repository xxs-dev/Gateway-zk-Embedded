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
        const std::string&,
        const std::string&
    ) override {
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
    std::vector<std::string> onDemandTopics;
    std::vector<std::size_t> onDemandCounts;
    std::vector<int> pollTimeouts;
    int alarmCount = 0;
    int changeCount = 0;
    int commandReplyCount = 0;
    int otaReplyCount = 0;
    int otaStatusCount = 0;
    int jsonCount = 0;
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
        std::cerr << "running unrouted point test" << std::endl;
        testUnroutedPointIndexIsRejected();
        std::cerr << "running unavailable PointStore test" << std::endl;
        testUnavailablePointStoreFailsClosed();
        std::cerr << "running forwarder health test" << std::endl;
        testForwarderFailureWritesHealthAndDoesNotPoll();
        std::cerr << "running realtime/full isolation test" << std::endl;
        testRealtimeStopLeavesMainFullAndForwarderFullRunning();
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
