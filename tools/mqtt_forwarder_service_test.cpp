#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "edge_gateway/builtin_mqtt_driver_publisher.hpp"
#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/json_value.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/mqtt_driver_service.hpp"
#include "edge_gateway/mqtt_control_result_store.hpp"
#include "edge_gateway/mqtt_event_outbox.hpp"
#include "edge_gateway/mqtt_event_stats.hpp"
#include "edge_gateway/mqtt_forwarder_service.hpp"
#include "edge_gateway/point_store_router.hpp"
#include "edge_gateway/power_control_ownership.hpp"
#include "edge_gateway/process_file_lock.hpp"

#ifndef _WIN32
#include <atomic>
#include <cstring>
#include <mutex>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
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

class FlakyEventMqttDriverPublisher : public CapturingMqttDriverPublisher {
public:
    void publishJsonMessage(
        const std::string& topic,
        const std::string& payload
    ) override {
        ++eventAttempts;
        if (eventFailuresRemaining > 0) {
            --eventFailuresRemaining;
            throw std::runtime_error("simulated event publish failure");
        }
        CapturingMqttDriverPublisher::publishJsonMessage(topic, payload);
    }

    int eventAttempts = 0;
    int eventFailuresRemaining = 0;
};

class FlakyReliableMqttDriverPublisher : public CapturingMqttDriverPublisher {
public:
    void publishReliableJsonMessage(
        const std::string& topic,
        const std::string& payload
    ) override {
        reliableTopics.push_back(topic);
        reliablePayloads.push_back(payload);
        if (reliableFailuresRemaining > 0) {
            --reliableFailuresRemaining;
            throw std::runtime_error("simulated reliable publish failure");
        }
        CapturingMqttDriverPublisher::publishJsonMessage(topic, payload);
    }

    std::vector<std::string> reliableTopics;
    std::vector<std::string> reliablePayloads;
    int reliableFailuresRemaining = 0;
};

class ControlInjectingEventPublisher : public CapturingMqttDriverPublisher {
public:
    void publishReliableJsonMessage(
        const std::string& topic,
        const std::string& payload
    ) override {
        reliableTopics.push_back(topic);
        reliablePayloads.push_back(payload);
        CapturingMqttDriverPublisher::publishJsonMessage(topic, payload);
        if (!injected) {
            MqttIncomingMessage command;
            command.type = MqttIncomingType::CommandRequest;
            command.topic = "third/cmd";
            command.payload = "{\"type\":1,\"target\":1,\"id\":\"EVENT_ORDER\"}";
            incoming.push_back(std::move(command));
            injected = true;
        }
    }

    std::vector<std::string> reliableTopics;
    std::vector<std::string> reliablePayloads;
    bool injected = false;
};

class OrderedEventMqttDriverPublisher : public CapturingMqttDriverPublisher {
public:
    void publishFullSnapshot(
        const std::string& topic,
        const std::vector<StoredPointValue>& values,
        const std::string& jsonFormat
    ) override {
        operations.push_back("full");
        CapturingMqttDriverPublisher::publishFullSnapshot(topic, values, jsonFormat);
    }

    void publishJsonMessage(
        const std::string& topic,
        const std::string& payload
    ) override {
        operations.push_back("event");
        CapturingMqttDriverPublisher::publishJsonMessage(topic, payload);
    }

    std::vector<std::string> operations;
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

class FakeEventStatsSource : public IEventStatsSource {
public:
    ~FakeEventStatsSource() override {
        if (destroyed) *destroyed = true;
    }

    EventStatsCacheEntry snapshot(const std::string& key) const override {
        requestedKeys.push_back(key);
        if (throwOnSnapshot) throw std::runtime_error("simulated statistics snapshot failure");
        for (const auto& entry : entries) {
            if (entry.query.key == key) return entry;
        }
        throw std::runtime_error("unexpected statistics key: " + key);
    }

    std::vector<EventStatsCacheEntry> entries;
    mutable std::vector<std::string> requestedKeys;
    bool throwOnSnapshot = false;
    std::shared_ptr<bool> destroyed;
};

EventStatsCacheEntry freshEventStats(const EventStatsQuery& query, std::int64_t count) {
    EventStatsCacheEntry entry;
    entry.query = query;
    entry.status = EventStatsStatus::Fresh;
    entry.hasValue = true;
    entry.valid = true;
    entry.value.pendingCount = count;
    entry.sampledAtUnixMs = 1771000000000LL;
    entry.ageMs = 25;
    return entry;
}

std::string joinValues(const std::vector<std::string>& values) {
    std::string joined;
    for (const auto& value : values) {
        joined += value;
        joined.push_back('\n');
    }
    return joined;
}

std::string readTextFile(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    );
}

class FullClaimHealthCapturingPublisher : public CapturingMqttDriverPublisher {
public:
    void publishFullSnapshot(
        const std::string& topic,
        const std::vector<StoredPointValue>& values,
        const std::string& jsonFormat
    ) override {
        healthDuringFullPublish = readTextFile(healthFile);
        CapturingMqttDriverPublisher::publishFullSnapshot(topic, values, jsonFormat);
    }

    std::string healthFile;
    std::string healthDuringFullPublish;
};

class BlockingFullHealthCapturingPublisher : public CapturingMqttDriverPublisher {
public:
    void publishFullSnapshot(
        const std::string& topic,
        const std::vector<StoredPointValue>& values,
        const std::string& jsonFormat
    ) override {
        healthBeforeBlock = readTextFile(healthFile);
        std::this_thread::sleep_for(std::chrono::milliseconds(blockMs));
        healthAfterBlock = readTextFile(healthFile);
        CapturingMqttDriverPublisher::publishFullSnapshot(topic, values, jsonFormat);
    }

    std::string healthFile;
    std::string healthBeforeBlock;
    std::string healthAfterBlock;
    int blockMs = 250;
};

PointDefinition makeWritablePoint(std::uint32_t index, const std::string& pointCode) {
    auto point = makePoint(index, pointCode);
    point.write.enable = true;
    return point;
}

std::string sqliteLibraryPath() {
    const auto* value = std::getenv("SQLITE3_LIBRARY_PATH");
    return value == nullptr ? std::string() : std::string(value);
}

std::uint64_t uniqueTestSuffix() {
    static std::uint64_t sequence = 0;
    const auto timestamp = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()
    );
    return timestamp + (++sequence);
}

std::string temporaryTestPath(const std::string& name, const std::string& extension) {
#ifdef _WIN32
    const auto* base = std::tmpnam(nullptr);
    require(base != nullptr, "failed to allocate a temporary path");
    return std::string(base) + "_" + name + extension;
#else
    return "/tmp/gateway_" + name + "_" + std::to_string(getpid()) + "_" +
        std::to_string(uniqueTestSuffix()) + extension;
#endif
}

void removeSqliteFiles(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-shm").c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-journal").c_str());
}

void testControlResultStoreKeepsProcessSqliteApiStable() {
    const auto firstPath = temporaryTestPath("control_result_api_first", ".db");
    const auto secondPath = temporaryTestPath("control_result_api_second", ".db");
    const auto rejectedPath = temporaryTestPath("control_result_api_rejected", ".db");
    removeSqliteFiles(firstPath);
    removeSqliteFiles(secondPath);
    removeSqliteFiles(rejectedPath);

    {
        MqttControlResultStore first(firstPath);
        {
            MqttControlResultStore second(secondPath);
            require(!second.find("missing"), "second control result store should be usable");
        }
        require(
            !first.find("missing"),
            "destroying one control result store must not invalidate another store's sqlite API"
        );

        bool rejectedLibrarySwitch = false;
        try {
            MqttControlResultStore rejected(
                rejectedPath,
                temporaryTestPath("different_sqlite_runtime", ".so")
            );
        } catch (const std::invalid_argument&) {
            rejectedLibrarySwitch = true;
        }
        require(
            rejectedLibrarySwitch,
            "control result stores must reject a process-local sqlite library switch"
        );
    }

    removeSqliteFiles(firstPath);
    removeSqliteFiles(secondPath);
    removeSqliteFiles(rejectedPath);
}

std::int64_t monotonicTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

std::string escapeJsonString(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const auto ch : value) {
        if (ch == '\\' || ch == '"') {
            result.push_back('\\');
        }
        result.push_back(ch);
    }
    return result;
}

std::int64_t jsonInteger(const std::string& payload, const std::string& name) {
    const auto marker = "\"" + name + "\":";
    const auto position = payload.find(marker);
    require(position != std::string::npos, "missing JSON integer field " + name);
    return std::stoll(payload.substr(position + marker.size()));
}

const json::JsonValue& healthField(const json::JsonValue& health, const std::string& name) {
    const auto* field = health.find(name);
    require(field != nullptr, "missing health field " + name);
    return *field;
}

void requireStatsScope(
    const json::JsonValue& health,
    bool waiting,
    const std::string& target,
    const std::vector<std::string>& include
) {
    require(healthField(health, "eventStatsKey").asString() ==
        (waiting ? "forwarder.waiting" : "forwarder.enabled"), "wrong Forwarder statistics key");
    require(healthField(health, "eventStatsContext").asString() ==
        (waiting ? "awaiting-business-delegation" : "configured-business-topics"),
        "wrong Forwarder statistics context");
    require(healthField(health, "eventStatsTargetId").asString() == target,
        "wrong Forwarder statistics target");
    require(healthField(health, "eventStatsSelection").asString() == "only",
        "Forwarder statistics must select business event types explicitly");
    std::vector<std::string> actual;
    for (const auto& value : healthField(health, "eventStatsInclude").asArray().values) {
        actual.push_back(value->asString());
    }
    require(actual == include, "wrong Forwarder statistics included types");
    require(healthField(health, "eventStatsExclude").asArray().values.empty(),
        "Forwarder statistics must not add excluded types");
}

MqttEventOutbox::EventMessage makeOutboxEvent(
    const std::string& eventId,
    const std::string& targetId,
    const std::string& eventType,
    const std::string& topic,
    const std::string& payload,
    std::int64_t eventTs
) {
    MqttEventOutbox::EventMessage event;
    event.eventId = eventId;
    event.targetId = targetId;
    event.eventType = eventType;
    event.topic = topic;
    event.payload = payload;
    event.eventTs = eventTs;
    return event;
}

struct EventForwarderTestEnvironment {
    explicit EventForwarderTestEnvironment(
        const std::string& testName,
        const std::string& targetId,
        bool primaryFullUpload,
        std::shared_ptr<IMqttDriverPublisher> mqttPublisher,
        std::size_t replayBatchSize = 100
    )
        : sharedMemoryName("mqtt_forwarder_events_" + std::to_string(uniqueTestSuffix())),
          databasePath(temporaryTestPath(testName, ".db")),
          healthFile(temporaryTestPath(testName + "_health", ".json")),
          replayLockFile(temporaryTestPath(testName + "_replay", ".lock")),
          readyFile(temporaryTestPath(testName + "_ready", ".json")),
          publishLockFile(temporaryTestPath(testName + "_full", ".lock")),
          publisher(std::move(mqttPublisher)) {
        MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
        removeSqliteFiles(databasePath);
        std::remove(healthFile.c_str());
        std::remove(replayLockFile.c_str());
        std::remove(readyFile.c_str());
        std::remove((readyFile + ".lock").c_str());
        std::remove(publishLockFile.c_str());

        MemoryStoreConfig storeConfig;
        storeConfig.sharedMemoryName = sharedMemoryName;
        store.reset(new MemoryPointStore(storeConfig));
        router.addStore(sharedMemoryName, *store);

        DeviceConfig deviceConfig;
        deviceConfig.machineCode = machineCode;
        deviceConfig.memoryStore.sharedMemoryName = sharedMemoryName;
        LogicalDeviceConfig meter;
        meter.meterCode = "EVENT_METER";
        meter.points.push_back(makePoint(pointIndex, "EVENT_POINT"));
        deviceConfig.meters.push_back(meter);
        router.addRoutesFromDeviceConfigs({deviceConfig}, sharedMemoryName);

        PointValue value;
        value.index = pointIndex;
        value.value = 12.5;
        value.ts = 1771000000000LL;
        value.expireAt = 4102444800000LL;
        require(router.putLatestByIndex(value).accepted, "failed to seed event forwarder point");

        forward.enabled = true;
        forward.fullTelemetryTopic = "edge/telemetry/full";
        forward.pointIndexes = {pointIndex};
        forward.intervalMs = 1000;
        forward.publishOnStart = false;
        forward.healthHeartbeatMs = 1000;
        forward.healthLeaseTtlMs = 3000;
        forward.primaryFullUpload = primaryFullUpload;
        forward.primaryMachineCode = machineCode;
        forward.primaryClientId = machineCode + "-full";
        forward.publishLockFile = publishLockFile;
        forward.events.enabled = true;
        forward.events.targetId = targetId;
        forward.events.changeTopic = changeTopic;
        forward.events.alarmTopic = alarmTopic;
        forward.events.changeTopicMachineScoped = true;
        forward.events.alarmTopicMachineScoped = true;
        forward.events.replayIntervalMs = 10;
        forward.events.replayMaxBytes = 256 * 1024;

        ownedOutbox.reset(new MqttEventOutbox(
            databasePath,
            sqliteLibraryPath(),
            12,
            24,
            replayBatchSize
        ));
        outbox = ownedOutbox.get();
    }

    ~EventForwarderTestEnvironment() {
        service.reset();
        ownedOutbox.reset();
        store.reset();
        MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
        removeSqliteFiles(databasePath);
        std::remove(healthFile.c_str());
        std::remove((healthFile + ".tmp").c_str());
        std::remove(replayLockFile.c_str());
        std::remove(readyFile.c_str());
        std::remove((readyFile + ".lock").c_str());
        std::remove(publishLockFile.c_str());
        std::remove((databasePath + ".priority.json").c_str());
        std::remove((databasePath + ".priority.json.lock").c_str());
        std::remove((databasePath + ".ownership.json").c_str());
        std::remove((databasePath + ".ownership.json.lock").c_str());
    }

    void createService(std::unique_ptr<IEventStatsSource> eventStats = nullptr) {
        require(!service && ownedOutbox, "event forwarder service was already created");
        service.reset(new MqttForwarderService(
            forward,
            router,
            publisher,
            healthFile,
            std::move(ownedOutbox),
            databasePath,
            replayLockFile,
            readyFile,
            std::move(eventStats)
        ));
    }

    std::unique_ptr<MqttDriverService> createDriver(
        const std::shared_ptr<CapturingMqttDriverPublisher>& driverPublisher) {
        MqttConfig mqtt;
        mqtt.enabled = true;
        mqtt.topicMachineCode = machineCode;
        mqtt.clientId = machineCode;
        mqtt.fullTelemetryTopic = forward.fullTelemetryTopic;
        mqtt.eventOutboxSqlitePath = databasePath;
        mqtt.changeEventTopic = forward.events.changeTopic;
        mqtt.alarmTopic = forward.events.alarmTopic;
        mqtt.changeEventTopicMachineScoped = forward.events.changeTopicMachineScoped;
        mqtt.alarmTopicMachineScoped = forward.events.alarmTopicMachineScoped;
        MqttDriverConfig config;
        config.enabled = true;
        config.scanIntervalMs = 10;
        config.priorityControlLeaseFile = databasePath + ".priority.json";
        config.powerControlOwnershipFile = databasePath + ".ownership.json";
        config.fullUploadWorker.mode = "isolated";
        config.fullUploadWorker.clientIdSuffix = "-full";
        config.fullUploadWorker.eventForwardingEnabled = true;
        config.fullUploadWorker.healthFile = healthFile;
        config.fullUploadWorker.eventReplayLockFile = replayLockFile;
        config.fullUploadWorker.eventDelegationReadyFile = readyFile;
        config.fullUploadWorker.failoverTimeoutMs = forward.healthLeaseTtlMs;
        auto writer = std::unique_ptr<MqttEventOutbox>(new MqttEventOutbox(
            databasePath, sqliteLibraryPath(), 12, 24, 100));
        return std::unique_ptr<MqttDriverService>(new MqttDriverService(
            mqtt, config, {}, router, driverPublisher, std::move(writer)));
    }

    void writeValidDelegationReady() const {
        writeDelegationReadyAt(monotonicTimeMs());
    }

    void writeDelegationReadyAt(std::int64_t heartbeat) const {
        std::ofstream output(readyFile.c_str(), std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "failed to create Driver delegation ready file");
        output
            << "{\"dataPlaneVersion\":2"
            << ",\"externalOutbox\":true"
            << ",\"eventFallbackCapable\":true"
            << ",\"machineCode\":\"" << escapeJsonString(machineCode) << "\""
            << ",\"outboxPath\":\"" << escapeJsonString(databasePath) << "\""
            << ",\"changeTopic\":\"" << escapeJsonString(forward.events.changeTopic) << "\""
            << ",\"alarmTopic\":\"" << escapeJsonString(forward.events.alarmTopic) << "\""
            << ",\"changeTopicMachineScoped\":" << (forward.events.changeTopicMachineScoped ? "true" : "false")
            << ",\"alarmTopicMachineScoped\":" << (forward.events.alarmTopicMachineScoped ? "true" : "false")
            << ",\"eventReplayLockFile\":\"" << escapeJsonString(replayLockFile) << "\""
            << ",\"heartbeatMonotonicMs\":" << heartbeat
            << ",\"leaseUntilMonotonicMs\":" << (heartbeat + 30000)
            << "}";
        output.close();
        require(static_cast<bool>(output), "failed to persist Driver delegation ready file");
    }

    static constexpr std::uint32_t pointIndex = 3601;
    const std::string machineCode = "GW_TEST";
    const std::string changeTopic = "edge/event/change/GW_TEST";
    const std::string alarmTopic = "edge/alarm/GW_TEST";
    std::string sharedMemoryName;
    std::string databasePath;
    std::string healthFile;
    std::string replayLockFile;
    std::string readyFile;
    std::string publishLockFile;
    std::unique_ptr<MemoryPointStore> store;
    PointStoreRouter router;
    std::shared_ptr<IMqttDriverPublisher> publisher;
    MqttForwardConfig forward;
    std::unique_ptr<MqttEventOutbox> ownedOutbox;
    MqttEventOutbox* outbox = nullptr;
    std::unique_ptr<MqttForwarderService> service;
};

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
        const auto resultStorePath = MqttControlResultStore::defaultPathForOwnershipFile(ownershipFile);
        std::remove(resultStorePath.c_str());
        std::remove((resultStorePath + "-shm").c_str());
        std::remove((resultStorePath + "-wal").c_str());

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
        const auto resultStorePath = MqttControlResultStore::defaultPathForOwnershipFile(ownershipFile);
        std::remove(resultStorePath.c_str());
        std::remove((resultStorePath + "-shm").c_str());
        std::remove((resultStorePath + "-wal").c_str());
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

    environment.runCommand("{\"type\":0,\"id\":\"A\"}", startedAt + 150);
    environment.requireNoWrites("same id with a different command type must not enqueue");
    environment.requireRejectedReply("same id with a different command type must be rejected");
    require(
        static_cast<bool>(observer.active(startedAt + 151)),
        "type-mismatched duplicate must not release remote ownership"
    );

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
    environment.requireNoWrites("same id with a different target must not enqueue");
    environment.requireRejectedReply("same id with a different target must be rejected");
    require(
        environment.publisher->jsonPayloads.back().find("command id payload mismatch") !=
            std::string::npos,
        "same id with a different target must explain the payload mismatch"
    );
    const auto repeatedLease = observer.active(startedAt + 301);
    require(repeatedLease && repeatedLease->expireAtMs == renewedLease->expireAtMs,
        "payload-mismatched duplicate must not renew the lease");

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

void testConcurrentConstructionPreservesActiveLease() {
    ControlTestEnvironment environment(true);
    PowerControlOwnership observer(environment.ownershipFile, "observer");
    const auto active = observer.active(4102444800001LL);
    require(
        active && active->sessionId == environment.forward.control.sessionId,
        "constructing another forwarder must not clear a possibly live remote lease"
    );
    require(
        !environment.submitLocal(1.0, 4102444800001LL),
        "local EMS must remain blocked until the preserved lease is released or expires"
    );
}

void testChangedSessionConstructionPreservesForeignLease() {
    ControlTestEnvironment environment(true, 4095, "previous-config-session");
    PowerControlOwnership observer(environment.ownershipFile, "observer");
    const auto active = observer.active(4102444800001LL);
    require(
        active && active->sessionId == "previous-config-session",
        "a newly configured session must not clear the previous possibly live session"
    );
    require(
        !environment.submitLocal(1.0, 4102444800001LL),
        "local EMS must remain blocked while the previous session lease is still active"
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

    environment.runCommand("{\"type\":1,\"target\":1,\"id\":\"OLD_A\"}", startedAt + 30);
    environment.requireNoWrites("accepted duplicate after restart must not enqueue");
    require(
        environment.publisher->jsonPayloads.back().find("\"accepted\":true") != std::string::npos &&
        environment.publisher->jsonPayloads.back().find("\"duplicate\":true") != std::string::npos,
        "accepted duplicate after restart must return its persisted result"
    );
    require(!observer.active(startedAt + 31), "accepted duplicate after restart must not reacquire control");
}

void testAcceptedReceiptRejectsChangedTargetMapping() {
    ControlTestEnvironment environment;
    const std::int64_t startedAt = 1770001950000LL;
    environment.runCommand("{\"type\":1,\"target\":1,\"id\":\"MAP_A\"}", startedAt);
    requireMappedWrites(environment, 11.0, 1.5, "first mapped command should be accepted");

    environment.service.reset();
    environment.forward.control.targets[0].scale = 20.0;
    environment.service.reset(
        new MqttForwarderService(environment.forward, environment.router, environment.publisher)
    );
    environment.runCommand("{\"type\":1,\"target\":1,\"id\":\"MAP_A\"}", startedAt + 10);
    environment.requireNoWrites("same id under a changed mapping must not enqueue");
    environment.requireRejectedReply("same id under a changed mapping must be rejected");
    require(
        environment.publisher->jsonPayloads.back().find("command id payload mismatch") !=
            std::string::npos,
        "changed mapping must be reported as a command-id payload mismatch"
    );
}

void testControlPublishesFinalDeviceResults() {
    ControlTestEnvironment environment;
    const std::int64_t startedAt = 1770001960000LL;
    environment.runCommand("{\"type\":1,\"target\":2,\"id\":\"RESULT_A\"}", startedAt);
    requireMappedWrites(environment, 21.0, 3.5, "result test command should be queued");

    WritebackResultRecord first;
    first.cmdId = "RESULT_A";
    first.index = 4001;
    first.value = 21.0;
    first.success = true;
    first.message = "ok";
    first.stage = "writeback";
    first.requestedAt = startedAt;
    first.acceptedAt = startedAt;
    first.startedAt = startedAt + 10;
    first.completedAt = startedAt + 30;
    first.queueDelayMs = 10;
    first.deviceWriteMs = 20;
    first.edgeElapsedMs = 30;
    first.totalElapsedMs = 30;
    WritebackResultRecord second = first;
    second.index = 4002;
    second.value = 3.5;
    second.completedAt = startedAt + 40;
    second.deviceWriteMs = 30;
    second.edgeElapsedMs = 40;
    second.totalElapsedMs = 40;
    environment.store->recordWritebackResult(first);
    environment.store->recordWritebackResult(second);

    environment.service->runOnce(startedAt + 50);
    const auto joined = joinValues(environment.publisher->jsonPayloads);
    require(joined.find("\"id\":\"RESULT_A\"") != std::string::npos,
        "final control result must retain the command id");
    require(joined.find("\"stage\":\"device-result\"") != std::string::npos &&
            joined.find("\"success\":true") != std::string::npos,
        "final control result must report successful device completion");
    require(joined.find("\"index\":4001") != std::string::npos &&
            joined.find("\"index\":4002") != std::string::npos,
        "final control result must include every mapped target");
    require(joined.find("\"totalElapsedMs\":40") != std::string::npos,
        "final control result must expose the aggregate end-to-end duration");
}

void testControlPublishesFinalResultTimeout() {
    ControlTestEnvironment environment;
    const std::int64_t startedAt = 1770001965000LL;
    environment.runCommand("{\"type\":1,\"target\":2,\"id\":\"RESULT_TIMEOUT\"}", startedAt);
    requireMappedWrites(environment, 21.0, 3.5, "timeout test command should be queued");

    environment.service->runOnce(startedAt + environment.forward.control.leaseTtlMs + 1);
    const auto joined = joinValues(environment.publisher->jsonPayloads);
    require(joined.find("\"id\":\"RESULT_TIMEOUT\"") != std::string::npos &&
            joined.find("\"stage\":\"writeback-timeout\"") != std::string::npos &&
            joined.find("\"success\":false") != std::string::npos,
        "missing device results must produce a correlated final timeout reply");
}

void testControlRecoversPendingResultAfterRestart() {
    ControlTestEnvironment environment;
    const std::int64_t startedAt = 1770001968000LL;
    environment.runCommand("{\"type\":1,\"target\":2,\"id\":\"RESULT_RESTART\"}", startedAt);
    requireMappedWrites(environment, 21.0, 3.5, "restart result command should be queued");

    WritebackResultRecord first;
    first.cmdId = "RESULT_RESTART";
    first.index = 4001;
    first.value = 21.0;
    first.success = true;
    first.message = "ok";
    first.stage = "writeback";
    first.requestedAt = startedAt;
    first.acceptedAt = startedAt;
    first.startedAt = startedAt + 10;
    first.completedAt = startedAt + 30;
    first.queueDelayMs = 10;
    first.deviceWriteMs = 20;
    first.edgeElapsedMs = 30;
    first.totalElapsedMs = 30;
    WritebackResultRecord second = first;
    second.index = 4002;
    second.value = 3.5;
    second.completedAt = startedAt + 40;
    second.deviceWriteMs = 30;
    second.edgeElapsedMs = 40;
    second.totalElapsedMs = 40;
    environment.store->recordWritebackResult(first);
    environment.store->recordWritebackResult(second);

    environment.service.reset();
    environment.publisher->jsonPayloads.clear();
    environment.publisher->jsonTopics.clear();
    environment.service.reset(
        new MqttForwarderService(environment.forward, environment.router, environment.publisher)
    );
    environment.service->runOnce(startedAt + 50);

    const auto joined = joinValues(environment.publisher->jsonPayloads);
    require(joined.find("\"id\":\"RESULT_RESTART\"") != std::string::npos &&
            joined.find("\"stage\":\"device-result\"") != std::string::npos &&
            joined.find("\"success\":true") != std::string::npos,
        "restart must recover a pending command and publish its device result");
}

void testReservedControlIsNotReportedAsSubmittedAfterRestart() {
    ControlTestEnvironment environment;
    const std::int64_t startedAt = 1770001965000LL;
    environment.runCommand("{\"type\":1,\"target\":2,\"id\":\"PHASE_SOURCE\"}", startedAt);
    requireMappedWrites(environment, 21.0, 3.5, "phase source command should be queued");

    const auto resultStorePath = MqttControlResultStore::defaultPathForOwnershipFile(
        environment.ownershipFile
    );
    MqttControlResultRecord reserved;
    {
        MqttControlResultStore durable(resultStorePath);
        const auto source = durable.find("PHASE_SOURCE");
        require(source && source->submitted,
            "accepted control command did not persist its submitted phase");
        require(durable.storeFinalPayload(source->id, source->fingerprint, "{\"source\":true}"),
            "failed to finalize the phase source record");
        require(durable.markDelivered(source->id, source->fingerprint, startedAt + 1),
            "failed to mark the phase source record delivered");
        reserved = *source;
        reserved.id = "PHASE_RESERVED";
        reserved.acceptedAtMs = startedAt + 10;
        reserved.deadlineMs = startedAt + 1010;
        reserved.finalPayload.clear();
        reserved.delivered = false;
        reserved.submitted = false;
        require(durable.reservePending(reserved) == MqttControlReserveStatus::Inserted,
            "failed to seed a reserved-only control record");
    }

    environment.service.reset();
    environment.publisher = std::make_shared<CapturingMqttDriverPublisher>();
    environment.service.reset(new MqttForwarderService(
        environment.forward,
        environment.router,
        environment.publisher
    ));
    environment.runCommand(
        "{\"type\":1,\"target\":2,\"id\":\"PHASE_RESERVED\"}",
        startedAt + 20
    );
    environment.requireNoWrites("reserved-only duplicate must not enqueue a device write");
    environment.requireRejectedReply("reserved-only duplicate must not claim device submission");
    require(environment.publisher->jsonPayloads.back().find("submission was not confirmed") !=
            std::string::npos,
        "reserved-only duplicate did not explain the uncertain submission state");

    environment.service->runOnce(startedAt + 1020);
    const auto joined = joinValues(environment.publisher->jsonPayloads);
    require(joined.find("\"stage\":\"submission-uncertain\"") != std::string::npos &&
            joined.find("\"accepted\":false") != std::string::npos,
        "reserved-only timeout did not publish an explicit uncertain submission result");
}

void testControlResultRetentionKeepsPendingAndUndeliveredRecords() {
#ifdef _WIN32
    const std::string path = std::tmpnam(nullptr);
#else
    const std::string path = "/tmp/mqtt_control_result_retention_test.db";
#endif
    std::remove(path.c_str());
    std::remove((path + "-shm").c_str());
    std::remove((path + "-wal").c_str());
    constexpr std::int64_t nowMs = 2000000000LL;
    constexpr std::int64_t dayMs = 24LL * 60LL * 60LL * 1000LL;
    {
        MqttControlResultStore store(path, {}, 1, 2);
        const auto reserve = [&](const std::string& id, std::int64_t acceptedAt) {
            MqttControlResultRecord record;
            record.id = id;
            record.fingerprint = "fingerprint-" + id;
            record.generation = 1;
            record.acceptedAtMs = acceptedAt;
            record.deadlineMs = acceptedAt + 1000;
            PointStoreRoute route;
            route.index = 4001;
            route.sharedMemoryName = "retention-test";
            route.writable = true;
            record.routes.push_back(route);
            require(store.reservePending(record) == MqttControlReserveStatus::Inserted,
                "failed to reserve control retention fixture");
            require(store.markSubmitted(id, record.fingerprint),
                "failed to mark control retention fixture submitted");
            return record;
        };
        const auto deliver = [&](const MqttControlResultRecord& record, std::int64_t deliveredAt) {
            require(store.storeFinalPayload(record.id, record.fingerprint, "{\"final\":true}"),
                "failed to finalize control retention fixture");
            require(store.markDelivered(record.id, record.fingerprint, deliveredAt),
                "failed to deliver control retention fixture");
        };

        const auto expired = reserve("RETENTION_EXPIRED", nowMs - 3 * dayMs);
        deliver(expired, nowMs - 2 * dayMs);
        const auto cappedOldest = reserve("RETENTION_CAP_OLD", nowMs - 300);
        deliver(cappedOldest, nowMs - 300);
        const auto cappedMiddle = reserve("RETENTION_CAP_MIDDLE", nowMs - 200);
        deliver(cappedMiddle, nowMs - 200);
        const auto cappedNewest = reserve("RETENTION_CAP_NEW", nowMs - 100);
        deliver(cappedNewest, nowMs - 100);
        (void)reserve("RETENTION_PENDING", nowMs - 50);
        const auto undelivered = reserve("RETENTION_UNDELIVERED", nowMs - 40);
        require(store.storeFinalPayload(
                undelivered.id,
                undelivered.fingerprint,
                "{\"undelivered\":true}"),
            "failed to create undelivered control result fixture");

        store.cleanupIfDue(nowMs);
        require(!store.find("RETENTION_EXPIRED"),
            "expired delivered control result was not cleaned");
        require(!store.find("RETENTION_CAP_OLD"),
            "delivered control result count limit did not remove the oldest row");
        require(store.find("RETENTION_CAP_MIDDLE") && store.find("RETENTION_CAP_NEW"),
            "control result count cleanup removed recent delivered rows");
        require(static_cast<bool>(store.find("RETENTION_PENDING")),
            "control result cleanup removed a pending command");
        require(static_cast<bool>(store.find("RETENTION_UNDELIVERED")),
            "control result cleanup removed an undelivered final result");
    }
    std::remove(path.c_str());
    std::remove((path + "-shm").c_str());
    std::remove((path + "-wal").c_str());
}

void testFinalControlResultReplaysPersistedPayloadVerbatim() {
    ControlTestEnvironment environment;
    environment.service.reset();
    auto publisher = std::make_shared<FlakyReliableMqttDriverPublisher>();
    publisher->reliableFailuresRemaining = 1;
    environment.publisher = publisher;
    environment.forward.publishOnStart = false;
    environment.service.reset(
        new MqttForwarderService(environment.forward, environment.router, environment.publisher)
    );

    const std::int64_t startedAt = 1770001969000LL;
    environment.runCommand("{\"type\":1,\"target\":2,\"id\":\"RESULT_REPLAY\"}", startedAt);
    requireMappedWrites(environment, 21.0, 3.5, "replay test command should be queued");

    WritebackResultRecord first;
    first.cmdId = "RESULT_REPLAY";
    first.index = 4001;
    first.value = 21.0;
    first.success = true;
    first.message = "ok";
    first.stage = "writeback";
    first.requestedAt = startedAt;
    first.acceptedAt = startedAt;
    first.startedAt = startedAt + 10;
    first.completedAt = startedAt + 30;
    first.totalElapsedMs = 30;
    WritebackResultRecord second = first;
    second.index = 4002;
    second.value = 3.5;
    second.completedAt = startedAt + 40;
    second.totalElapsedMs = 40;
    environment.store->recordWritebackResult(first);
    environment.store->recordWritebackResult(second);

    environment.service->runOnce(startedAt + 50);
    require(publisher->reliablePayloads.size() == 1,
        "failed final publication must make exactly one reliable attempt in the run");
    const auto firstAttempt = publisher->reliablePayloads.front();
    {
        MqttControlResultStore durable(
            MqttControlResultStore::defaultPathForOwnershipFile(environment.ownershipFile)
        );
        const auto record = durable.find("RESULT_REPLAY");
        require(record && record->finalPayload == firstAttempt && !record->delivered,
            "final payload must be durable and undelivered before a failed publish returns");
    }

    environment.runCommand(
        "{\"type\":1,\"target\":2,\"id\":\"RESULT_REPLAY\"}",
        startedAt + 60
    );
    environment.requireNoWrites("final-result retry must not enqueue another device write");
    require(publisher->reliablePayloads.size() == 2,
        "same-id retry must publish the stored final result");
    require(publisher->reliablePayloads[1] == firstAttempt,
        "same-id retry must replay the byte-identical final payload");
    {
        MqttControlResultStore durable(
            MqttControlResultStore::defaultPathForOwnershipFile(environment.ownershipFile)
        );
        const auto record = durable.find("RESULT_REPLAY");
        require(record && record->delivered,
            "successful reliable replay must mark the final result delivered");
    }
}

void testDurableIdempotencySurvivesOwnershipReceiptEviction() {
    ControlTestEnvironment environment;
    const std::int64_t startedAt = 1770001970000LL;
    environment.runCommand("{\"type\":1,\"target\":1,\"id\":\"DURABLE_OLD\"}", startedAt);
    requireMappedWrites(environment, 11.0, 1.5, "durable idempotency command should be queued");

    for (int index = 0; index < 70; ++index) {
        environment.runCommand(
            "{\"type\":0,\"id\":\"RECEIPT_" + std::to_string(index) + "\"}",
            startedAt + index + 1
        );
        environment.requireNoWrites("local-mode receipt must not enqueue a device write");
    }

    PowerControlOwnership observer(environment.ownershipFile, "observer");
    require(!observer.lookupReceipt("DURABLE_OLD").found,
        "test precondition failed: ownership receipt was not evicted after 64 entries");

    environment.runCommand(
        "{\"type\":1,\"target\":1,\"id\":\"DURABLE_OLD\"}",
        startedAt + 100
    );
    environment.requireNoWrites(
        "durable command id must prevent a device rewrite after ownership receipt eviction"
    );
    require(
        environment.publisher->jsonPayloads.back().find("\"duplicate\":true") !=
            std::string::npos,
        "evicted ownership receipt must still resolve through the durable result store"
    );
}

void testLocalModeReceiptPreventsIdReuseForRemoteMode() {
    ControlTestEnvironment environment;
    const std::int64_t startedAt = 1770001970000LL;
    environment.runCommand("{\"type\":0,\"id\":\"MODE_A\"}", startedAt);
    environment.requireNoWrites("local-mode command must not enqueue a device write");
    require(
        environment.submitLocal(1.0, startedAt + 1),
        "a durable local-mode receipt must not block local EMS writes"
    );
    environment.store->drainPendingWriteCommands();

    environment.runCommand("{\"type\":1,\"target\":1,\"id\":\"MODE_A\"}", startedAt + 10);
    environment.requireNoWrites("a local-mode id must not be reusable for remote mode");
    environment.requireRejectedReply("reusing a local-mode id for remote mode must be rejected");
    require(
        environment.publisher->jsonPayloads.back().find("command id payload mismatch") !=
            std::string::npos,
        "type-changing id reuse must report a payload mismatch"
    );
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

void testEventForwardingRejectsQosZero() {
    MqttForwardConfig forward;
    forward.enabled = true;
    forward.qos = 0;
    forward.fullTelemetryTopic = "third/full";
    forward.pointIndexes = {2301};
    forward.events.enabled = true;
    forward.events.targetId = "third-party";
    forward.events.changeTopic = "third/change";

    PointStoreRouter router;
    auto publisher = std::make_shared<CapturingMqttDriverPublisher>();
    try {
        MqttForwarderService service(forward, router, publisher);
        throw std::runtime_error("qos 0 event forwarding was accepted");
    } catch (const std::invalid_argument& ex) {
        require(
            std::string(ex.what()).find("qos 1 or 2") != std::string::npos,
            "event forwarding must require broker acknowledgement"
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
    driverConfig.priorityControlLeaseFile = temporaryTestPath("realtime_driver_priority", ".json");
    driverConfig.powerControlOwnershipFile = temporaryTestPath("realtime_driver_ownership", ".json");
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

void testPrimaryStatsFollowDelegationAndConfiguredTopics() {
    for (const std::string enabledType : {"alarm", "change"}) {
        auto publisher = std::make_shared<CapturingMqttDriverPublisher>();
        EventForwarderTestEnvironment environment("primary_stats_scope", "main", true, publisher);
        if (enabledType == "alarm") environment.forward.events.changeTopic.clear();
        else environment.forward.events.alarmTopic.clear();
        std::unique_ptr<FakeEventStatsSource> eventStats(new FakeEventStatsSource);
        auto* stats = eventStats.get();
        stats->entries = {
            freshEventStats(mqttForwarderStatsQuery(environment.forward.events, true), 19),
            freshEventStats(mqttForwarderStatsQuery(environment.forward.events, false), 7)
        };
        const auto startedAt = 1771000600000LL;
        require(environment.outbox->enqueueBatch({
            makeOutboxEvent("stats-alarm", "main", "alarm", environment.alarmTopic, "{}", startedAt),
            makeOutboxEvent("stats-change", "main", "change", environment.changeTopic, "{}", startedAt)
        }).size() == 2, "failed to seed statistics scope events");
        environment.createService(std::move(eventStats));

        environment.service->runOnce(startedAt);
        auto health = json::JsonParser(readTextFile(environment.healthFile)).parse();
        requireStatsScope(health, true, "main", {"alarm", "change"});
        require(healthField(health, "eventPendingCount").asNumber() == 19,
            "waiting health must read the injected business count, not the Outbox count");
        require(!healthField(health, "eventForwarding").asBool() && publisher->jsonTopics.empty(),
            "fresh statistics must not authorize undelegated event publication");
        require(healthField(health, "eventLastAckAtMs").asNumber() == 0,
            "statistics must not create an MQTT acknowledgement");

        environment.writeValidDelegationReady();
        ProcessFileLock liveLock(environment.readyFile + ".lock");
        require(liveLock.tryAcquire(), "failed to hold statistics test Driver live lock");
        {
            ProcessFileLock replayLock(environment.replayLockFile);
            require(replayLock.tryAcquire(), "failed to hold statistics test replay lock");
            environment.service->runOnce(startedAt + 2000);
            health = json::JsonParser(readTextFile(environment.healthFile)).parse();
            requireStatsScope(health, true, "main", {"alarm", "change"});
            require(healthField(health, "eventLastError").asString() == "primary event replay ownership is busy",
                "statistics must preserve the lock contention diagnostic");
            require(publisher->jsonTopics.empty(), "statistics bypassed the replay ownership lock");
        }

        environment.service->runOnce(startedAt + 4000);
        health = json::JsonParser(readTextFile(environment.healthFile)).parse();
        requireStatsScope(health, false, "main", {enabledType});
        require(healthField(health, "eventPendingCount").asNumber() == 7,
            "delegated health must use the configured-topic statistics snapshot");
        require(publisher->jsonTopics == std::vector<std::string>{
            enabledType == "alarm" ? environment.alarmTopic : environment.changeTopic},
            "delegated replay did not preserve configured topic filtering");
        require(environment.outbox->pendingCount("main") == 1,
            "delegated replay consumed the disabled event type");
        require(healthField(health, "eventForwarding").asBool() &&
            healthField(health, "eventOutboxHealthy").asBool(), "delegated event replay must remain healthy");
        const auto lastAck = healthField(health, "eventLastAckAtMs").asNumber();
        require(lastAck > 0, "delegated publication must record an actual acknowledgement");

        ProcessFileLock replayLock(environment.replayLockFile);
        require(replayLock.tryAcquire(), "failed to reacquire statistics test replay lock");
        environment.service->runOnce(startedAt + 6000);
        health = json::JsonParser(readTextFile(environment.healthFile)).parse();
        requireStatsScope(health, true, "main", {"alarm", "change"});
        require(healthField(health, "eventPendingCount").asNumber() == 19 &&
            !healthField(health, "eventForwarding").asBool(),
            "each replay attempt must reset the main statistics scope to waiting");
        require(healthField(health, "eventLastAckAtMs").asNumber() == lastAck,
            "waiting statistics must preserve the last real acknowledgement");
        require(stats->requestedKeys.front() == "forwarder.waiting" &&
            stats->requestedKeys.back() == "forwarder.waiting" &&
            std::find(stats->requestedKeys.begin(), stats->requestedKeys.end(), "forwarder.enabled") !=
                stats->requestedKeys.end(), "health did not request both delegation statistics keys");
    }
}

void testInvalidStatsDoNotAffectDelegatedEventDelivery() {
    struct Scenario {
        const char* name;
        EventStatsStatus status;
        bool hasValue;
        bool throwOnSnapshot;
        bool wrongScope;
        bool ipc;
        const char* healthStatus;
    };
    const std::vector<Scenario> scenarios = {
        {"error", EventStatsStatus::Error, true, false, false, false, "error"},
        {"stale", EventStatsStatus::Stale, true, false, false, false, "stale"},
        {"stopped", EventStatsStatus::Stopped, true, false, false, false, "stopped"},
        {"never", EventStatsStatus::NeverSampled, false, false, false, false, "never-sampled"},
        {"exception", EventStatsStatus::Fresh, true, true, false, false, "error"},
        {"scope", EventStatsStatus::Fresh, true, false, true, false, "error"},
        {"ipc", EventStatsStatus::Fresh, true, false, false, true, "error"},
        {"fresh-error", EventStatsStatus::Fresh, true, false, false, false, "error"}
    };
    for (const auto& scenario : scenarios) {
        auto publisher = std::make_shared<CapturingMqttDriverPublisher>();
        EventForwarderTestEnvironment environment("invalid_stats", "main", true, publisher);
        std::unique_ptr<FakeEventStatsSource> eventStats(new FakeEventStatsSource);
        auto entry = freshEventStats(mqttForwarderStatsQuery(environment.forward.events, false), 31);
        entry.status = scenario.status;
        entry.hasValue = scenario.hasValue;
        entry.valid = scenario.status == EventStatsStatus::Fresh;
        entry.ageMs = 6001;
        if (scenario.status == EventStatsStatus::Error || std::string(scenario.name) == "fresh-error") {
            entry.error = "simulated statistics error";
        }
        if (scenario.wrongScope) entry.query.scope.targetId = "other-target";
        if (scenario.ipc) entry.backend = EventStatsBackend::Ipc;
        eventStats->entries = {entry};
        eventStats->throwOnSnapshot = scenario.throwOnSnapshot;
        const auto startedAt = 1771000700000LL;
        require(environment.outbox->enqueueBatch({
            makeOutboxEvent("invalid-stats-alarm", "main", "alarm", environment.alarmTopic, "{}", startedAt)
        }).size() == 1, "failed to seed invalid-statistics delivery event");
        environment.writeValidDelegationReady();
        ProcessFileLock liveLock(environment.readyFile + ".lock");
        require(liveLock.tryAcquire(), "failed to hold invalid-statistics Driver live lock");
        environment.createService(std::move(eventStats));

        environment.service->runOnce(startedAt);
        const auto health = json::JsonParser(readTextFile(environment.healthFile)).parse();
        requireStatsScope(health, false, "main", {"alarm", "change"});
        require(healthField(health, "eventStatsStatus").asString() == scenario.healthStatus,
            std::string("wrong invalid-statistics status for ") + scenario.name);
        require(!healthField(health, "eventStatsValid").asBool() &&
            healthField(health, "eventPendingCount").isNull(),
            "unavailable statistics must not report a valid current count");
        const bool keepHistory = scenario.hasValue && !scenario.throwOnSnapshot &&
            !scenario.wrongScope && !scenario.ipc;
        require(healthField(health, "eventStatsHasValue").asBool() == keepHistory,
            "invalid-statistics history availability is incorrect");
        if (keepHistory) {
            require(healthField(health, "eventPendingLastKnownCount").asNumber() == 31 &&
                healthField(health, "eventStatsAgeMs").asNumber() == 6001,
                "failed or expired statistics must expose last-known count and age separately");
        } else {
            require(healthField(health, "eventPendingLastKnownCount").isNull() &&
                healthField(health, "eventStatsAgeMs").isNull(),
                "unavailable or rejected snapshots must not invent statistics history");
        }
        require(publisher->jsonTopics == std::vector<std::string>{environment.alarmTopic} &&
            environment.outbox->pendingCount("main") == 0,
            "invalid statistics prevented event delivery or acknowledgement");
        require(healthField(health, "eventForwarding").asBool() &&
            healthField(health, "eventOutboxHealthy").asBool() &&
            healthField(health, "eventLastError").asString().empty(),
            "statistics failure changed event delegation or delivery health");
        const auto lastAck = healthField(health, "eventLastAckAtMs").asNumber();
        require(lastAck > 0, "statistics failure suppressed the successful event acknowledgement time");
        environment.service->runOnce(startedAt + 2000);
        const auto nextHealth = json::JsonParser(readTextFile(environment.healthFile)).parse();
        require(healthField(nextHealth, "eventLastAckAtMs").asNumber() == lastAck,
            "statistics must not advance acknowledgement time without a new delivery");
    }
}

void testThirdPartyStatsKeepTargetAndDeliveryErrorsIndependent() {
    auto publisher = std::make_shared<FlakyEventMqttDriverPublisher>();
    publisher->eventFailuresRemaining = 1;
    EventForwarderTestEnvironment environment("third_stats", "partner-west", false, publisher);
    std::unique_ptr<FakeEventStatsSource> eventStats(new FakeEventStatsSource);
    auto* stats = eventStats.get();
    auto entry = freshEventStats(mqttForwarderStatsQuery(environment.forward.events, false), 43);
    entry.status = EventStatsStatus::Error;
    entry.valid = false;
    entry.error = "statistics unavailable";
    stats->entries = {entry};
    const auto startedAt = 1771000800000LL;
    require(environment.outbox->enqueueBatch({
        makeOutboxEvent("third-stats", "partner-west", "change", environment.changeTopic, "{}", startedAt),
        makeOutboxEvent("main-stats", "main", "change", environment.changeTopic, "{}", startedAt)
    }).size() == 2, "failed to seed third-party statistics target events");
    environment.createService(std::move(eventStats));
    environment.service->runOnce(startedAt);
    auto health = json::JsonParser(readTextFile(environment.healthFile)).parse();
    requireStatsScope(health, false, "partner-west", {"alarm", "change"});
    require(!healthField(health, "eventOutboxHealthy").asBool() &&
        !healthField(health, "eventForwarding").asBool(), "failed delivery must remain unhealthy");
    require(healthField(health, "eventLastError").asString() == "simulated event publish failure" &&
        healthField(health, "eventStatsError").asString() == "statistics unavailable",
        "delivery and statistics errors must remain independent");
    require(healthField(health, "eventLastAckAtMs").asNumber() == 0,
        "last-known statistics must not acknowledge failed delivery");

    stats->entries.front().status = EventStatsStatus::Stale;
    stats->entries.front().error.clear();
    stats->entries.front().ageMs = 6001;
    environment.service->runOnce(startedAt + 2000);
    health = json::JsonParser(readTextFile(environment.healthFile)).parse();
    requireStatsScope(health, false, "partner-west", {"alarm", "change"});
    require(healthField(health, "eventPendingCount").isNull() &&
        healthField(health, "eventPendingLastKnownCount").asNumber() == 43,
        "third-party expired statistics must separate current and last-known counts");
    require(healthField(health, "eventOutboxHealthy").asBool() &&
        healthField(health, "eventForwarding").asBool() &&
        healthField(health, "eventLastError").asString().empty(),
        "expired statistics must not prevent third-party delivery recovery");
    require(publisher->eventAttempts == 2 && publisher->jsonTopics.size() == 1 &&
        environment.outbox->pendingCount("partner-west") == 0 &&
        environment.outbox->pendingCount("main") == 1,
        "third-party statistics target must not alter independent delivery targets");
    require(std::all_of(stats->requestedKeys.begin(), stats->requestedKeys.end(),
        [](const std::string& key) { return key == "forwarder.enabled"; }),
        "third-party statistics must never await main delegation, even after delivery failure");
}

void testHealthReadsCurrentStatsWithoutReplayAndOwnsSourceUntilDestruction() {
    auto publisher = std::make_shared<CapturingMqttDriverPublisher>();
    EventForwarderTestEnvironment environment("stats_lifetime", "third-party", false, publisher);
    environment.forward.events.replayIntervalMs = 100000;
    std::unique_ptr<FakeEventStatsSource> eventStats(new FakeEventStatsSource);
    auto* stats = eventStats.get();
    const auto destroyed = std::make_shared<bool>(false);
    stats->destroyed = destroyed;
    stats->entries = {freshEventStats(mqttForwarderStatsQuery(environment.forward.events, false), 53)};
    environment.createService(std::move(eventStats));
    const auto startedAt = 1771000900000LL;
    environment.service->runOnce(startedAt);
    auto health = json::JsonParser(readTextFile(environment.healthFile)).parse();
    require(healthField(health, "eventPendingCount").asNumber() == 53 &&
        healthField(health, "eventStatsValid").asBool(), "health did not read the fresh fake count");
    const auto snapshots = stats->requestedKeys.size();

    stats->entries.front().status = EventStatsStatus::Stale;
    stats->entries.front().valid = false;
    stats->entries.front().ageMs = 6001;
    environment.service->stop();
    require(!*destroyed, "service.stop must not destroy its statistics source");
    environment.service->runOnce(startedAt + 2000);
    health = json::JsonParser(readTextFile(environment.healthFile)).parse();
    require(stats->requestedKeys.size() > snapshots &&
        healthField(health, "eventStatsStatus").asString() == "stale" &&
        healthField(health, "eventPendingCount").isNull() &&
        healthField(health, "eventPendingLastKnownCount").asNumber() == 53,
        "health must re-read cache validity even when no event replay is due");
    require(healthField(health, "eventForwarding").asBool() &&
        healthField(health, "eventOutboxHealthy").asBool() &&
        healthField(health, "eventLastAckAtMs").asNumber() == 0,
        "changing diagnostic freshness must not change delivery state or invent an acknowledgement");
    environment.service.reset();
    require(*destroyed, "service destruction must release its owned statistics source");
}

void testMissingStatsRemainNullWithoutBlockingDelivery() {
    for (const bool primary : {false, true}) {
        auto publisher = std::make_shared<CapturingMqttDriverPublisher>();
        const std::string target = primary ? "main" : "third-party";
        EventForwarderTestEnvironment environment("missing_stats", target, primary, publisher);
        environment.forward.events.alarmTopic.clear();
        const auto startedAt = 1771001000000LL;
        require(environment.outbox->enqueueBatch({
            makeOutboxEvent("missing-stats", target, "change", environment.changeTopic, "{}", startedAt)
        }).size() == 1, "failed to seed missing-statistics event");
        environment.createService();
        environment.service->runOnce(startedAt);
        auto health = json::JsonParser(readTextFile(environment.healthFile)).parse();
        requireStatsScope(health, primary, target,
            primary ? std::vector<std::string>{"alarm", "change"} : std::vector<std::string>{"change"});
        require(healthField(health, "eventStatsStatus").asString() == "never-sampled" &&
            !healthField(health, "eventStatsValid").asBool() &&
            !healthField(health, "eventStatsHasValue").asBool() &&
            healthField(health, "eventPendingCount").isNull() &&
            healthField(health, "eventPendingLastKnownCount").isNull() &&
            healthField(health, "eventStatsAgeMs").isNull() &&
            !healthField(health, "eventStatsError").asString().empty(),
            "missing statistics must expose unavailability, never a fabricated zero");
        require(healthField(health, "eventOutboxHealthy").asBool(),
            "missing statistics must not mark the Outbox unhealthy");
        if (primary) {
            require(publisher->jsonTopics.empty() &&
                healthField(health, "eventLastError").asString() == "mqtt driver event delegation is not ready",
                "missing statistics must preserve the waiting delegation diagnostic");
            environment.writeValidDelegationReady();
            ProcessFileLock liveLock(environment.readyFile + ".lock");
            require(liveLock.tryAcquire(), "failed to hold missing-statistics Driver live lock");
            environment.service->runOnce(startedAt + 2000);
            health = json::JsonParser(readTextFile(environment.healthFile)).parse();
        }
        requireStatsScope(health, false, target, {"change"});
        require(publisher->jsonTopics.size() == 1 && environment.outbox->pendingCount(target) == 0 &&
            healthField(health, "eventForwarding").asBool() &&
            healthField(health, "eventLastError").asString().empty() &&
            healthField(health, "eventPendingCount").isNull(),
            "missing statistics must not block authorized publication or cause a synchronous count fallback");
    }
}

void testThirdPartyEventTargetIsIsolatedAndFailureDoesNotAcknowledge() {
    auto publisher = std::make_shared<FlakyEventMqttDriverPublisher>();
    publisher->eventFailuresRemaining = 1;
    EventForwarderTestEnvironment environment(
        "third_target",
        "third-party",
        false,
        publisher
    );
    const auto eventTs = 1771000100000LL;
    const auto inserted = environment.outbox->enqueueBatch({
        makeOutboxEvent(
            "shared-change-1",
            "main",
            "change",
            "edge/event/change/GW_TEST",
            "{\"target\":\"main\"}",
            eventTs
        ),
        makeOutboxEvent(
            "shared-change-1",
            "third-party",
            "change",
            "third/event/change/GW_TEST",
            "{\"target\":\"third-party\"}",
            eventTs
        )
    });
    require(inserted.size() == 2, "test fan-out rows were not persisted independently");
    environment.createService();

    environment.service->runOnce(eventTs);
    require(publisher->eventAttempts == 1, "third-party event replay did not attempt publication");
    require(environment.outbox->pendingCount("third-party") == 1,
        "failed third-party publication was incorrectly marked sent");
    require(environment.outbox->pendingCount("main") == 1,
        "third-party failure changed the independent main target row");
    require(publisher->jsonTopics.empty(), "failed event publication was recorded as successful");

    environment.service->runOnce(eventTs + environment.forward.events.replayIntervalMs);
    require(publisher->eventAttempts == 2, "third-party event was not retried");
    require(publisher->jsonTopics == std::vector<std::string>{"third/event/change/GW_TEST"},
        "third-party replay selected a row owned by another target");
    require(environment.outbox->pendingCount("third-party") == 0,
        "successful third-party publication was not acknowledged");
    require(environment.outbox->pendingCount("main") == 1,
        "third-party acknowledgement consumed the main target row");
}

void testPrimaryEventReplayRequiresReadyFileAndLiveDriverLock() {
    auto publisher = std::make_shared<CapturingMqttDriverPublisher>();
    EventForwarderTestEnvironment environment(
        "primary_delegation_gate",
        "main",
        true,
        publisher
    );
    const auto eventTs = 1771000200000LL;
    require(environment.outbox->enqueueBatch({
        makeOutboxEvent(
            "main-alarm-gated",
            "main",
            "alarm",
            environment.alarmTopic,
            "{\"active\":true}",
            eventTs
        )
    }).size() == 1, "gated main alarm was not persisted");
    environment.createService();

    environment.service->runOnce(eventTs);
    require(environment.outbox->pendingCount("main") == 1,
        "primary Forwarder consumed main events without a delegation ready file");
    require(publisher->jsonTopics.empty(),
        "primary Forwarder published an event without a delegation ready file");

    environment.writeValidDelegationReady();
    environment.service->runOnce(eventTs + environment.forward.events.replayIntervalMs);
    require(environment.outbox->pendingCount("main") == 1,
        "stale ready file without a live Driver lock transferred event ownership");
    require(publisher->jsonTopics.empty(),
        "stale ready file without a live Driver lock allowed event publication");
}

void testPrimaryEventReplayConsumesAlarmAndChangeWithLiveDelegation() {
    auto publisher = std::make_shared<CapturingMqttDriverPublisher>();
    EventForwarderTestEnvironment environment(
        "primary_delegation_live",
        "main",
        true,
        publisher
    );
    const auto eventTs = 1771000300000LL;
    require(environment.outbox->enqueueBatch({
        makeOutboxEvent(
            "main-change-live",
            "main",
            "change",
            environment.changeTopic,
            "{\"value\":12.5}",
            eventTs
        ),
        makeOutboxEvent(
            "main-alarm-live",
            "main",
            "alarm",
            environment.alarmTopic,
            "{\"active\":true}",
            eventTs + 1
        ),
        makeOutboxEvent(
            "third-change-live",
            "third-party",
            "change",
            "third/event/change/GW_TEST",
            "{\"value\":99}",
            eventTs + 2
        )
    }).size() == 3, "live delegation events were not persisted");
    environment.writeValidDelegationReady();
    ProcessFileLock driverLiveLock(environment.readyFile + ".lock");
    require(driverLiveLock.tryAcquire(), "failed to hold the Driver delegation live lock");
    std::unique_ptr<FakeEventStatsSource> eventStats(new FakeEventStatsSource);
    eventStats->entries = {freshEventStats(mqttForwarderStatsQuery(environment.forward.events, false), 0)};
    environment.createService(std::move(eventStats));

    environment.service->runOnce(eventTs);
    require(
        publisher->jsonTopics ==
            std::vector<std::string>{environment.alarmTopic, environment.changeTopic},
        "live main event replay did not publish alarm first and then change"
    );
    require(environment.outbox->pendingCount("main") == 0,
        "live main alarm/change rows were not acknowledged");
    require(environment.outbox->pendingCount("third-party") == 1,
        "main Forwarder consumed a third-party target row");

    const auto health = readTextFile(environment.healthFile);
    require(health.find("\"dataPlaneVersion\":2") != std::string::npos,
        "Forwarder health is missing the data plane version");
    require(jsonInteger(health, "eventPendingCount") == 0,
        "Forwarder health did not report the drained main event backlog");
    require(jsonInteger(health, "eventLastAckAtMs") > 0,
        "Forwarder health did not report the latest MQTT event acknowledgement");
}

void testPrimaryEventDelegationRemainsHealthyDuringFullPublish() {
    auto publisher = std::make_shared<FullClaimHealthCapturingPublisher>();
    EventForwarderTestEnvironment environment(
        "primary_delegation_full_claim",
        "main",
        true,
        publisher
    );
    environment.forward.publishOnStart = true;
    publisher->healthFile = environment.healthFile;
    environment.writeValidDelegationReady();
    ProcessFileLock driverLiveLock(environment.readyFile + ".lock");
    require(driverLiveLock.tryAcquire(), "failed to hold the Driver delegation live lock");
    environment.createService();

    environment.service->runOnce(1771000350000LL);

    require(
        publisher->healthDuringFullPublish.find("\"state\":\"claiming\"") !=
            std::string::npos,
        "full publish did not expose its in-progress health claim"
    );
    require(
        publisher->healthDuringFullPublish.find("\"eventForwarding\":true") !=
            std::string::npos,
        "full publish claim must not revoke the independent event delegation"
    );
}

void testBlockedFullPublishDoesNotRenewLeaseFromAnotherThread() {
    auto publisher = std::make_shared<BlockingFullHealthCapturingPublisher>();
    EventForwarderTestEnvironment environment(
        "primary_blocked_full_lease",
        "main",
        true,
        publisher
    );
    environment.forward.publishOnStart = true;
    environment.forward.healthHeartbeatMs = 20;
    environment.forward.healthLeaseTtlMs = 100;
    publisher->healthFile = environment.healthFile;
    publisher->blockMs = 180;
    environment.writeValidDelegationReady();
    ProcessFileLock driverLiveLock(environment.readyFile + ".lock");
    require(driverLiveLock.tryAcquire(), "failed to hold the Driver delegation live lock");
    environment.createService();

    environment.service->runOnce(1771000360000LL);

    require(!publisher->healthBeforeBlock.empty(),
        "blocked Full test did not capture the initial ownership claim");
    require(publisher->healthBeforeBlock == publisher->healthAfterBlock,
        "Full publishing must not renew its lease from a hidden background heartbeat");
    require(
        publisher->healthBeforeBlock.find("\"eventForwarding\":true") != std::string::npos,
        "blocked Full claim must preserve independent event delegation until lease expiry"
    );
    const auto expired = json::JsonParser(readTextFile(environment.healthFile)).parse();
    require(healthField(expired, "healthy").asBool() &&
        !healthField(expired, "eventForwarding").asBool(),
        "Full completion must not revive the event lease that expired during its blocked publish");
}

void testFullPublishFailurePreservesHealthyEventDelegation() {
    auto publisher = std::make_shared<FlakyMqttDriverPublisher>();
    publisher->fullFailuresRemaining = 1;
    EventForwarderTestEnvironment environment(
        "primary_full_failure_event_delegation",
        "main",
        true,
        publisher
    );
    environment.forward.publishOnStart = true;
    environment.writeValidDelegationReady();
    ProcessFileLock driverLiveLock(environment.readyFile + ".lock");
    require(driverLiveLock.tryAcquire(), "failed to hold the Driver delegation live lock");
    environment.createService();

    environment.service->runOnce(1771000370000LL);

    const auto health = readTextFile(environment.healthFile);
    require(health.find("\"healthy\":false") != std::string::npos,
        "failed Full publication must mark only the Full channel unhealthy");
    require(health.find("\"eventForwarding\":true") != std::string::npos,
        "failed Full publication must not revoke healthy event delegation");
    require(health.find("\"eventOutboxHealthy\":true") != std::string::npos,
        "failed Full publication must not mark the event outbox unhealthy");

    environment.outbox->enqueueBatch({
        makeOutboxEvent("after-full-alarm", "main", "alarm", "test/alarm", "{}", 1771000370001LL),
        makeOutboxEvent("after-full-change", "main", "change", "test/change", "{}", 1771000370001LL),
        makeOutboxEvent("after-full-management", "main", "ota_status", "test/management", "{}", 1771000370001LL)
    });
    auto driverPublisher = std::make_shared<CapturingMqttDriverPublisher>();
    auto driver = environment.createDriver(driverPublisher);
    driver->runEventReplayOnce(1771000370001LL);
    require(std::count(driverPublisher->jsonTopics.begin(), driverPublisher->jsonTopics.end(),
            "test/alarm") == 0 &&
        std::count(driverPublisher->jsonTopics.begin(), driverPublisher->jsonTopics.end(),
            "test/change") == 0,
        "actual Forwarder Full failure health must keep business ownership out of Driver");
    require(std::count(driverPublisher->jsonTopics.begin(), driverPublisher->jsonTopics.end(),
            "test/management") == 1,
        "independent event delegation must keep Driver management replay available");
}

void testEventLeaseOnlyRenewsOnReplay() {
    auto publisher = std::make_shared<CapturingMqttDriverPublisher>();
    EventForwarderTestEnvironment environment("event_lease_progress", "main", true, publisher);
    environment.forward.healthLeaseTtlMs = 100;
    environment.forward.healthHeartbeatMs = 1;
    environment.forward.events.replayIntervalMs = 100000;
    environment.writeValidDelegationReady();
    ProcessFileLock driverLiveLock(environment.readyFile + ".lock");
    require(driverLiveLock.tryAcquire(), "failed to hold Driver live lock");
    environment.createService();
    const auto wall = 1771000500000LL;
    environment.service->runOnce(wall);
    const auto first = json::JsonParser(readTextFile(environment.healthFile)).parse();
    const auto heartbeat = healthField(first, "eventHeartbeatMonotonicMs").asNumber();
    const auto deadline = healthField(first, "eventLeaseUntilMonotonicMs").asNumber();
    require(deadline - heartbeat == 100, "event lease must use the configured TTL");
    require(healthField(first, "eventLastAckAtMs").asNumber() == 0,
        "successful empty replay must not fabricate an ACK");

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    environment.service->runOnce(wall + 1000);
    const auto full = json::JsonParser(readTextFile(environment.healthFile)).parse();
    require(healthField(full, "eventHeartbeatMonotonicMs").asNumber() == heartbeat &&
        healthField(full, "eventLeaseUntilMonotonicMs").asNumber() == deadline,
        "Full success or health writes must not extend an idle event lease");

    std::this_thread::sleep_for(std::chrono::milliseconds(140));
    environment.service->runOnce(wall + 2000);
    const auto expired = json::JsonParser(readTextFile(environment.healthFile)).parse();
    require(healthField(expired, "healthy").asBool() &&
        !healthField(expired, "eventForwarding").asBool(),
        "Full health must not hide event lease expiry");
    auto driverPublisher = std::make_shared<CapturingMqttDriverPublisher>();
    auto driver = environment.createDriver(driverPublisher);
    environment.outbox->enqueueBatch({makeOutboxEvent(
        "lease-expired-alarm", "main", "alarm", "test/expired", "{}", wall + 2001)});
    driver->runEventReplayOnce(wall + 2001);
    require(std::count(driverPublisher->jsonTopics.begin(), driverPublisher->jsonTopics.end(),
        "test/expired") == 1, "Driver must take over an expired event lease while Full is healthy");
}

void testEventReplayRecoversAfterClockRollbackAndStop() {
    auto publisher = std::make_shared<CapturingMqttDriverPublisher>();
    EventForwarderTestEnvironment environment("event_replay_clock_stop", "main", true, publisher);
    environment.forward.healthHeartbeatMs = 1;
    environment.forward.events.replayIntervalMs = 100000;
    environment.createService();
    environment.writeValidDelegationReady();
    ProcessFileLock driverLiveLock(environment.readyFile + ".lock");
    require(driverLiveLock.tryAcquire(), "failed to hold clock/stop Driver live lock");
    const auto wall = 1771000520000LL;
    environment.service->runOnce(wall);
    const auto first = json::JsonParser(readTextFile(environment.healthFile)).parse();
    const auto heartbeat = healthField(first, "eventHeartbeatMonotonicMs").asNumber();
    require(healthField(first, "eventForwarding").asBool(), "clock/stop test needs an initial lease");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    environment.outbox->enqueueBatch({makeOutboxEvent(
        "clock-rollback-alarm", "main", "alarm", "test/rollback", "{}", wall - 100)});
    environment.writeValidDelegationReady();
    environment.service->runOnce(wall - 100);
    require(std::count(publisher->jsonTopics.begin(), publisher->jsonTopics.end(), "test/rollback") == 1,
        "wall clock rollback must not stall event replay until the old clock catches up");
    // Force a health write after the rollback without another event replay.
    environment.service->runOnce(wall + 3000);
    const auto recovered = json::JsonParser(readTextFile(environment.healthFile)).parse();
    require(healthField(recovered, "eventHeartbeatMonotonicMs").asNumber() > heartbeat &&
        healthField(recovered, "eventForwarding").asBool(), "successful replay must renew its own lease");

    environment.service->stop();
    environment.outbox->enqueueBatch({makeOutboxEvent(
        "restart-alarm", "main", "alarm", "test/restart", "{}", wall + 3001)});
    environment.writeDelegationReadyAt(monotonicTimeMs() - environment.forward.healthLeaseTtlMs - 1);
    environment.service->runOnce(wall + 4000);
    const auto staleReady = json::JsonParser(readTextFile(environment.healthFile)).parse();
    require(std::count(publisher->jsonTopics.begin(), publisher->jsonTopics.end(), "test/restart") == 0 &&
        healthField(staleReady, "eventLastError").asString() == "mqtt driver event delegation is not ready" &&
        !healthField(staleReady, "eventForwarding").asBool(),
        "restart must reject an expired Driver ready heartbeat even after resetting replay cadence");
    environment.service->stop();
    environment.writeValidDelegationReady();
    environment.service->runOnce(wall + 4001);
    require(std::count(publisher->jsonTopics.begin(), publisher->jsonTopics.end(), "test/restart") == 1,
        "restart with fresh Driver ready must replay immediately: " + readTextFile(environment.healthFile));
}

void testEventLeaseRevokedAfterSuccessfulReplayLosesOwnership() {
    for (const std::string reason : {"publish-failure", "missing-ready", "replay-busy"}) {
        auto publisher = std::make_shared<FlakyEventMqttDriverPublisher>();
        EventForwarderTestEnvironment environment("lease_revoke_" + reason, "main", true, publisher);
        environment.forward.healthHeartbeatMs = 1;
        environment.writeValidDelegationReady();
        ProcessFileLock liveLock(environment.readyFile + ".lock");
        require(liveLock.tryAcquire(), "failed to hold revocation Driver lock");
        environment.createService();
        const auto wall = 1771000550000LL;
        environment.service->runOnce(wall);
        auto health = json::JsonParser(readTextFile(environment.healthFile)).parse();
        require(healthField(health, "eventForwarding").asBool(),
            "revocation requires a previously successful event lease");
        environment.outbox->enqueueBatch({makeOutboxEvent(
            "revoke-alarm", "main", "alarm", "test/revoke", "{}", wall + 1)});
        std::unique_ptr<ProcessFileLock> busyLock;
        if (reason == "publish-failure") publisher->eventFailuresRemaining = 1;
        else if (reason == "missing-ready") std::remove(environment.readyFile.c_str());
        else {
            busyLock.reset(new ProcessFileLock(environment.replayLockFile));
            require(busyLock->tryAcquire(), "failed to hold revocation replay lock");
        }
        environment.service->runOnce(wall + 2000);
        health = json::JsonParser(readTextFile(environment.healthFile)).parse();
        require(healthField(health, "healthy").asBool() &&
            !healthField(health, "eventForwarding").asBool() &&
            healthField(health, "eventHeartbeatMonotonicMs").asNumber() == 0 &&
            healthField(health, "eventLeaseUntilMonotonicMs").asNumber() == 0,
            "Full success must not preserve a revoked event lease: " + reason);
        require(healthField(health, "eventOutboxHealthy").asBool() == (reason != "publish-failure"),
            "ownership loss must be distinguished from event publication failure");
        require(environment.outbox->pendingCount("main") == 1,
            "revoked publication must retain its pending event");
        busyLock.reset();
        auto driverPublisher = std::make_shared<CapturingMqttDriverPublisher>();
        auto driver = environment.createDriver(driverPublisher);
        driver->runEventReplayOnce(wall + 2001);
        require(std::count(driverPublisher->jsonTopics.begin(), driverPublisher->jsonTopics.end(),
            "test/revoke") == 1, "Driver must recover events after event lease revocation: " + reason);
    }
}

void testStoppedEventLeaseFileExpiresWithoutFullRenewal() {
    auto publisher = std::make_shared<CapturingMqttDriverPublisher>();
    EventForwarderTestEnvironment environment("lease_stop_file", "main", true, publisher);
    environment.forward.healthLeaseTtlMs = 300;
    environment.writeValidDelegationReady();
    ProcessFileLock liveLock(environment.readyFile + ".lock");
    require(liveLock.tryAcquire(), "failed to hold stopped lease Driver lock");
    environment.createService();
    const auto wall = 1771000580000LL;
    environment.service->runOnce(wall);
    const auto beforeStop = readTextFile(environment.healthFile);
    require(healthField(json::JsonParser(beforeStop).parse(), "eventForwarding").asBool(),
        "stopped-file test requires an active lease");
    environment.service->stop();
    require(readTextFile(environment.healthFile) == beforeStop,
        "stop must not renew Full or event timestamps by rewriting health");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    environment.outbox->enqueueBatch({makeOutboxEvent(
        "stopped-alarm", "main", "alarm", "test/stopped", "{}", wall + 1)});
    auto driverPublisher = std::make_shared<CapturingMqttDriverPublisher>();
    auto driver = environment.createDriver(driverPublisher);
    driver->runEventReplayOnce(wall + 1);
    require(readTextFile(environment.healthFile) == beforeStop &&
        std::count(driverPublisher->jsonTopics.begin(), driverPublisher->jsonTopics.end(),
            "test/stopped") == 1, "Driver must expire an unchanged stopped Forwarder health file");
}

void testDueFullStillPublishesWhileEventBacklogRemains() {
    auto publisher = std::make_shared<OrderedEventMqttDriverPublisher>();
    EventForwarderTestEnvironment environment(
        "event_full_fairness",
        "third-party",
        false,
        publisher,
        1
    );
    environment.forward.publishOnStart = true;
    const auto startedAt = 1771000400000LL;
    std::vector<MqttEventOutbox::EventMessage> events;
    for (int index = 0; index < 4; ++index) {
        events.push_back(makeOutboxEvent(
            "fairness-alarm-" + std::to_string(index),
            "third-party",
            "alarm",
            environment.alarmTopic,
            "{\"sequence\":" + std::to_string(index) + "}",
            startedAt + index
        ));
    }
    require(environment.outbox->enqueueBatch(events).size() == events.size(),
        "fairness event backlog was not persisted");
    environment.createService();

    environment.service->runOnce(startedAt);
    require(environment.outbox->pendingCount("third-party") == 3,
        "first bounded replay did not leave a sustained event backlog");
    require(
        publisher->operations == std::vector<std::string>{"event", "full"},
        "initial runOnce did not give both event replay and Full a send opportunity"
    );

    environment.service->runOnce(startedAt + environment.forward.intervalMs + 1000);
    require(environment.outbox->pendingCount("third-party") == 2,
        "second bounded replay did not preserve the sustained backlog precondition");
    require(
        publisher->operations ==
            std::vector<std::string>{"event", "full", "event", "full"},
        "an event backlog starved the due Full publication in the same runOnce"
    );
    require(publisher->fullSnapshotCounts.size() == 2,
        "due Full was not published on every eligible runOnce");
}

void testControlModeReplaysBoundedBatchBeforeNextPoll() {
    ControlTestEnvironment environment;
    environment.service.reset();
    auto publisher = std::make_shared<ControlInjectingEventPublisher>();
    environment.publisher = publisher;
    environment.forward.publishOnStart = false;
    environment.forward.events.enabled = true;
    environment.forward.events.targetId = "third-party";
    environment.forward.events.changeTopic = "third/change";
    environment.forward.events.alarmTopic = "third/alarm";
    environment.forward.events.replayIntervalMs = 10;
    environment.forward.events.replayMaxBytes = 256 * 1024;

    const auto databasePath = temporaryTestPath("control_event_order", ".db");
    removeSqliteFiles(databasePath);
    std::unique_ptr<MqttEventOutbox> outbox(new MqttEventOutbox(
        databasePath,
        sqliteLibraryPath(),
        12,
        24,
        100
    ));
    auto* outboxObserver = outbox.get();
    const std::int64_t startedAt = 1771000500000LL;
    std::vector<MqttEventOutbox::EventMessage> events;
    for (int index = 0; index < 100; ++index) {
        events.push_back(makeOutboxEvent(
            "control-order-" + std::to_string(index),
            "third-party",
            "change",
            "third/change",
            "{\"sequence\":" + std::to_string(index) + "}",
            startedAt + index
        ));
    }
    require(outbox->enqueueBatch(events).size() == events.size(),
        "control-order event backlog was not persisted");
    environment.service.reset(new MqttForwarderService(
        environment.forward,
        environment.router,
        environment.publisher,
        {},
        std::move(outbox),
        databasePath
    ));

    environment.service->runOnce(startedAt);
    require(publisher->reliablePayloads.size() == 8,
        "control mode must replay one bounded eight-event batch in a runOnce");
    require(outboxObserver->pendingCount("third-party") == 92,
        "first control-mode run must leave 92 of 100 events pending");
    environment.requireNoWrites(
        "command injected during event publication must wait for the next control poll"
    );

    environment.service->runOnce(startedAt + environment.forward.events.replayIntervalMs);
    requireMappedWrites(environment, 11.0, 1.5,
        "next runOnce must poll the injected control command before replaying another event");
    require(publisher->reliablePayloads.size() == 16,
        "second control-mode run must replay only one additional bounded batch");
    require(outboxObserver->pendingCount("third-party") == 84,
        "second control-mode run must leave the remaining event backlog intact");

    environment.service.reset();
    removeSqliteFiles(databasePath);
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

std::size_t publishPayloadOffset(const std::vector<std::uint8_t>& packet) {
    std::size_t cursor = 1;
    while (cursor < packet.size() && (packet[cursor++] & 0x80) != 0) {
    }
    if (cursor + 2 > packet.size()) {
        return packet.size();
    }
    const auto topicLength =
        (static_cast<std::size_t>(packet[cursor]) << 8) | packet[cursor + 1];
    cursor += 2 + topicLength;
    const auto qos = (packet[0] >> 1) & 0x03;
    if (qos > 0) {
        cursor += 2;
    }
    return std::min(cursor, packet.size());
}

std::uint16_t publishPacketIdentifier(const std::vector<std::uint8_t>& packet) {
    const auto payloadOffset = publishPayloadOffset(packet);
    const auto qos = packet.empty() ? 0 : (packet[0] >> 1) & 0x03;
    if (qos == 0 || payloadOffset < 2 || payloadOffset > packet.size()) {
        return 0;
    }
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(packet[payloadOffset - 2]) << 8) |
        packet[payloadOffset - 1]
    );
}

std::string publishPacketPayload(const std::vector<std::uint8_t>& packet) {
    const auto offset = publishPayloadOffset(packet);
    return std::string(
        reinterpret_cast<const char*>(packet.data() + offset),
        reinterpret_cast<const char*>(packet.data() + packet.size())
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

class ReliableControlResultBroker {
public:
    explicit ReliableControlResultBroker(bool acknowledge) : acknowledge_(acknowledge) {
        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        require(listenFd_ >= 0, "control-result broker socket failed");
        int opt = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        require(bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0,
            "control-result broker bind failed");
        socklen_t len = sizeof(addr);
        require(getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0,
            "control-result broker getsockname failed");
        port_ = ntohs(addr.sin_port);
        require(listen(listenFd_, 1) == 0, "control-result broker listen failed");
        thread_ = std::thread([this]() { run(); });
    }

    ~ReliableControlResultBroker() {
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

    std::string payload() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return payload_;
    }

private:
    void run() {
        const int fd = accept(listenFd_, nullptr, nullptr);
        if (fd < 0) {
            return;
        }
        try {
            const auto connect = readMqttPacket(fd);
            require(!connect.empty() && (connect[0] & 0xF0) == 0x10,
                "control-result broker expected CONNECT");
            const std::uint8_t connAck[] = {0x20, 0x02, 0x00, 0x00};
            send(fd, connAck, sizeof(connAck), 0);

            const auto publish = readMqttPacket(fd);
            require(!publish.empty() && (publish[0] & 0xF0) == 0x30,
                "control-result broker expected PUBLISH");
            {
                std::lock_guard<std::mutex> lock(mutex_);
                payload_ = publishPacketPayload(publish);
            }
            auto packetId = publishPacketIdentifier(publish);
            if (!acknowledge_) {
                packetId = static_cast<std::uint16_t>(packetId == 65535 ? 1 : packetId + 1);
            }
            const std::uint8_t pubAck[] = {
                0x40,
                0x02,
                static_cast<std::uint8_t>((packetId >> 8) & 0xFF),
                static_cast<std::uint8_t>(packetId & 0xFF)
            };
            send(fd, pubAck, sizeof(pubAck), 0);
        } catch (...) {
        }
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }

    bool acknowledge_ = false;
    int listenFd_ = -1;
    int port_ = 0;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::string payload_;
};

void testBuiltinPublisherFailureKeepsFinalUndeliveredAndRestartRetries() {
    ControlTestEnvironment environment;
    environment.service.reset();
    environment.forward.publishOnStart = false;
    const auto databasePath = MqttControlResultStore::defaultPathForOwnershipFile(
        environment.ownershipFile
    );
    const std::string finalPayload =
        "{\"id\":\"BUILTIN_FINAL\",\"stage\":\"device-result\",\"success\":true}";
    {
        MqttControlResultStore durable(databasePath);
        MqttControlResultRecord record;
        record.id = "BUILTIN_FINAL";
        record.fingerprint = "fixed-fingerprint";
        record.type = 1;
        record.targetKw = 1.0;
        record.generation = 1;
        record.acceptedAtMs = 1771000600000LL;
        record.deadlineMs = 1771000601000LL;
        record.routes.push_back(*environment.router.routeByIndex(4001));
        require(durable.reservePending(record) == MqttControlReserveStatus::Inserted,
            "failed to seed a pending production-publisher result");
        require(durable.storeFinalPayload(record.id, record.fingerprint, finalPayload),
            "failed to seed the final production-publisher payload");
    }

    {
        ReliableControlResultBroker broker(false);
        auto mqtt = MqttForwarderService::makeTxOnlyMqttConfig(environment.forward, "GW_TEST");
        mqtt.broker = "tcp://127.0.0.1:" + std::to_string(broker.port());
        mqtt.commandReplyTopic = environment.forward.control.replyTopic;
        mqtt.commandReplyTopicMachineScoped = false;
        mqtt.qos = 1;
        mqtt.controlQos = 1;
        auto publisher = std::make_shared<BuiltinMqttDriverPublisher>(
            mqtt,
            MqttPublisherMode::TxOnly
        );
        environment.service.reset(
            new MqttForwarderService(environment.forward, environment.router, publisher)
        );
        environment.service->runOnce(1771000600100LL);
        environment.service.reset();
        publisher.reset();
        require(broker.payload() == finalPayload,
            "production publisher failure test did not send the persisted final payload");
    }
    {
        MqttControlResultStore durable(databasePath);
        const auto record = durable.find("BUILTIN_FINAL");
        require(record && !record->delivered,
            "rejected production PUBACK must not mark a final result delivered");
    }

    {
        ReliableControlResultBroker broker(true);
        auto mqtt = MqttForwarderService::makeTxOnlyMqttConfig(environment.forward, "GW_TEST");
        mqtt.broker = "tcp://127.0.0.1:" + std::to_string(broker.port());
        mqtt.commandReplyTopic = environment.forward.control.replyTopic;
        mqtt.commandReplyTopicMachineScoped = false;
        mqtt.qos = 1;
        mqtt.controlQos = 1;
        auto publisher = std::make_shared<BuiltinMqttDriverPublisher>(
            mqtt,
            MqttPublisherMode::TxOnly
        );
        environment.service.reset(
            new MqttForwarderService(environment.forward, environment.router, publisher)
        );
        environment.service->runOnce(1771000600200LL);
        environment.service.reset();
        publisher.reset();
        require(broker.payload() == finalPayload,
            "restart retry must resend the byte-identical production final payload");
    }
    {
        MqttControlResultStore durable(databasePath);
        const auto record = durable.find("BUILTIN_FINAL");
        require(record && record->delivered,
            "acknowledged production PUBACK must mark the final result delivered");
    }
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

struct ForwardReplayScript {
    int created = 0, destroyed = 0, calls = 0;
    bool wrongThread = false;
    std::function<MqttEventReplayProgress(const MqttEventReplayRequest&, bool)> step;
};

class ForwardScriptedReplay : public IMqttEventReplay {
public:
    ForwardScriptedReplay(ForwardReplayScript& script, MqttEventReplayRequest request)
        : script_(script), request_(std::move(request)), owner_(std::this_thread::get_id()) { ++script_.created; }
    ~ForwardScriptedReplay() override {
        ++script_.destroyed;
        script_.wrongThread = script_.wrongThread || owner_ != std::this_thread::get_id();
    }
    MqttEventReplayProgress runOnce(bool drain) override {
        ++script_.calls;
        script_.wrongThread = script_.wrongThread || owner_ != std::this_thread::get_id();
        return script_.step(request_, drain);
    }
private:
    ForwardReplayScript& script_;
    MqttEventReplayRequest request_;
    std::thread::id owner_;
};

MqttEventReplayFactory forwardReplayFactory(ForwardReplayScript& script) {
    return [&script](const MqttEventReplayRequest& request) {
        require(request.lane == MqttEventReplayLane::ForwardEvents && request.authorized(), "unauthorized forward factory");
        return std::unique_ptr<IMqttEventReplay>(new ForwardScriptedReplay(script, request));
    };
}

void writeIpcReady(EventForwarderTestEnvironment& env, const std::string& generation = "gen") {
    env.writeValidDelegationReady();
    auto text = readTextFile(env.readyFile);
    text.insert(1, "\"eventReplayBackend\":\"ipc-lab\",\"eventStoreId\":\"ipc-store\",\"eventStoreConfigGeneration\":\"" + generation + "\",");
    std::ofstream output(env.readyFile);
    output << text;
}

void testIpcForwarderDelegationPendingAndHealth() {
    auto publisher = std::make_shared<CapturingMqttDriverPublisher>();
    EventForwarderTestEnvironment env("ipc_delegation", "main", true, publisher);
    env.ownedOutbox.reset();
    removeSqliteFiles(env.databasePath);
    // Primary shares the fixed alarm/change sender even when one topic is disabled.
    env.forward.events.changeTopic.clear();
    env.forward.events.replayIntervalMs = 1000;
    env.forward.healthHeartbeatMs = 1;
    ForwardReplayScript script;
    script.step = [&](const auto& request, bool drain) {
        require(request.includeTypes == std::vector<std::string>{"alarm", "change"} &&
            request.targetId == "main" && request.maxMessages == 16 && request.maxBytes == 32768,
            "primary IPC scope or limit drifted");
        MqttEventReplayProgress result;
        if (script.calls == 1) {
            require(!drain && request.authorized(), "first replay denied");
            result.pending = true;
            result.attemptedBytes = 500;
        } else {
            require(drain && !request.authorized(), "lost delegation allowed pending network");
            result.pending = script.calls == 2;
            if (!result.pending) result.ackedCount = 1;
        }
        return result;
    };
    env.service.reset(new MqttForwarderService(env.forward, env.router, publisher, env.healthFile, nullptr,
        env.databasePath, env.replayLockFile, env.readyFile, nullptr, forwardReplayFactory(script), {"ipc-store", "gen"}));
    require(script.created == 0, "service constructor occupied sender");
    ProcessFileLock live(env.readyFile + ".lock");
    require(live.tryAcquire(), "test delegation live lock unavailable");
    env.writeValidDelegationReady();
    env.service->runOnce(10000);
    require(script.created == 0, "IPC accepted legacy delegation identity");
    writeIpcReady(env, "foreign-gen");
    env.service->runOnce(11000);
    require(script.created == 0, "IPC accepted foreign store generation delegation");
    writeIpcReady(env);
    {
        ProcessFileLock busy(env.replayLockFile);
        require(busy.tryAcquire(), "test replay lock unavailable");
        env.service->runOnce(12000);
        require(script.created == 0, "busy replay lock constructed sender");
    }
    env.service->runOnce(13000);
    require(script.created == 1 && script.destroyed == 0, "pending replay abandoned sender");
    const auto first = json::JsonParser(readTextFile(env.healthFile)).parse();
    require(!healthField(first, "eventForwarding").asBool() &&
        healthField(first, "eventLastAckAtMs").asNumber() == 0, "unknown ACK fabricated healthy lease");
    std::remove(env.readyFile.c_str());
    env.service->runOnce(13001);
    require(script.calls == 2 && script.destroyed == 0, "loss did not drain before next cadence");
    { ProcessFileLock busy(env.replayLockFile); require(!busy.tryAcquire(), "unknown mutation released replay ownership"); }
    env.service->runOnce(13002);
    require(script.destroyed == 1 && !script.wrongThread, "drained actor retained or destroyed off-thread");
    { ProcessFileLock busy(env.replayLockFile); require(busy.tryAcquire(), "drained actor retained replay lock"); }
    env.service->runOnce(14000);
    const auto drained = json::JsonParser(readTextFile(env.healthFile)).parse();
    require(!healthField(drained, "eventForwarding").asBool() &&
        healthField(drained, "eventLastAckAtMs").asNumber() > 0, "drain ACK lost or drain renewed health");
    require(script.created == 1 && !std::ifstream(env.databasePath).good(), "standby opened actor or legacy SQL");
}

void testIpcForwarderControlBoundedAndOrdered() {
    ControlTestEnvironment env;
    env.service.reset();
    auto publisher = std::make_shared<ControlInjectingEventPublisher>();
    env.publisher = publisher;
    env.forward.publishOnStart = false;
    env.forward.events.enabled = true;
    env.forward.events.targetId = "third-party";
    env.forward.events.changeTopic = "third/change";
    env.forward.events.alarmTopic.clear();
    env.forward.events.replayIntervalMs = 10;
    ForwardReplayScript script;
    script.step = [&](const auto& request, bool drain) {
        require(!drain && request.authorized() && request.maxMessages == 8 && request.targetId == "third-party" &&
            request.includeTypes == std::vector<std::string>{"change"}, "control IPC batch limit or target incorrect");
        for (std::size_t i = 0; i < request.maxMessages; ++i)
            publisher->publishReliableJsonMessage("third/change", "{}");
        MqttEventReplayProgress result;
        result.healthy = true;
        result.ackedCount = result.changeCount = request.maxMessages;
        return result;
    };
    env.service.reset(new MqttForwarderService(env.forward, env.router, publisher, {}, nullptr, {}, {}, {}, nullptr,
        forwardReplayFactory(script), {"ipc-store", "gen"}));
    env.service->runOnce(1771000500000LL);
    require(publisher->reliablePayloads.size() == 8 && script.destroyed == 1, "control IPC batch was not bounded");
    env.requireNoWrites("IPC replay polled commands out of order");
    env.service->runOnce(1771000500010LL);
    requireMappedWrites(env, 11.0, 1.5, "IPC replay changed control mapping/admission order");
    require(publisher->reliablePayloads.size() == 16, "second IPC control batch incorrect");
}

void testIpcForwarderLeaseOnlyRenewsAfterHealthyReplay() {
    auto publisher = std::make_shared<CapturingMqttDriverPublisher>();
    EventForwarderTestEnvironment env("ipc_lease", "main", true, publisher);
    env.ownedOutbox.reset();
    env.forward.events.replayIntervalMs = 100000;
    env.forward.healthHeartbeatMs = 1;
    ForwardReplayScript script;
    script.step = [](const auto&, bool drain) { MqttEventReplayProgress result; result.healthy = !drain; return result; };
    env.service.reset(new MqttForwarderService(env.forward, env.router, publisher, env.healthFile, nullptr,
        env.databasePath, env.replayLockFile, env.readyFile, nullptr, forwardReplayFactory(script), {"ipc-store", "gen"}));
    ProcessFileLock live(env.readyFile + ".lock");
    require(live.tryAcquire(), "lease test live lock unavailable");
    writeIpcReady(env);
    env.service->runOnce(1771000500000LL);
    auto first = json::JsonParser(readTextFile(env.healthFile)).parse();
    require(healthField(first, "eventForwarding").asBool() && script.destroyed == 1, "empty IPC replay failed health/actor release");
    const auto heartbeat = healthField(first, "eventHeartbeatMonotonicMs").asNumber();
    env.service->runOnce(1771000501000LL);
    auto full = json::JsonParser(readTextFile(env.healthFile)).parse();
    require(script.created == 1 && healthField(full, "eventHeartbeatMonotonicMs").asNumber() == heartbeat &&
        healthField(full, "eventLastAckAtMs").asNumber() == 0, "Full renewed IPC event lease or fabricated ACK");
}

}  // namespace

int main() {
    try {
        testIpcForwarderDelegationPendingAndHealth();
        testIpcForwarderControlBoundedAndOrdered();
        testIpcForwarderLeaseOnlyRenewsAfterHealthyReplay();
        std::cerr << "running control result sqlite lifetime test" << std::endl;
        testControlResultStoreKeepsProcessSqliteApiStable();
        std::cerr << "running tx-only service test" << std::endl;
        testTxOnlyConfigClearsControlTopicsAndNeverPolls();
        std::cerr << "running disabled forwarder test" << std::endl;
        testDisabledForwarderPublishesNothing();
        std::cerr << "running legacy payload test" << std::endl;
        testLegacyPayloadUsesIndependentMappings();
        std::cerr << "running unrouted point test" << std::endl;
        testUnroutedPointIndexIsRejected();
        std::cerr << "running event qos acknowledgement test" << std::endl;
        testEventForwardingRejectsQosZero();
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
        std::cerr << "running third-party event target and retry test" << std::endl;
        testThirdPartyEventTargetIsIsolatedAndFailureDoesNotAcknowledge();
        std::cerr << "running primary statistics delegation and topic scope test" << std::endl;
        testPrimaryStatsFollowDelegationAndConfiguredTopics();
        std::cerr << "running invalid statistics delegated delivery test" << std::endl;
        testInvalidStatsDoNotAffectDelegatedEventDelivery();
        std::cerr << "running third-party statistics and delivery error isolation test" << std::endl;
        testThirdPartyStatsKeepTargetAndDeliveryErrorsIndependent();
        std::cerr << "running health statistics freshness and source lifetime test" << std::endl;
        testHealthReadsCurrentStatsWithoutReplayAndOwnsSourceUntilDestruction();
        std::cerr << "running missing statistics delivery test" << std::endl;
        testMissingStatsRemainNullWithoutBlockingDelivery();
        std::cerr << "running primary event delegation gate test" << std::endl;
        testPrimaryEventReplayRequiresReadyFileAndLiveDriverLock();
        std::cerr << "running primary event live delegation test" << std::endl;
        testPrimaryEventReplayConsumesAlarmAndChangeWithLiveDelegation();
        std::cerr << "running event delegation during full publish test" << std::endl;
        testPrimaryEventDelegationRemainsHealthyDuringFullPublish();
        std::cerr << "running blocked Full lease ownership test" << std::endl;
        testBlockedFullPublishDoesNotRenewLeaseFromAnotherThread();
        std::cerr << "running Full failure event delegation test" << std::endl;
        testFullPublishFailurePreservesHealthyEventDelegation();
        std::cerr << "running event lease expiry and renewal test" << std::endl;
        testEventLeaseOnlyRenewsOnReplay();
        std::cerr << "running event clock rollback and restart readiness test" << std::endl;
        testEventReplayRecoversAfterClockRollbackAndStop();
        testEventLeaseRevokedAfterSuccessfulReplayLosesOwnership();
        testStoppedEventLeaseFileExpiresWithoutFullRenewal();
        std::cerr << "running event/full fairness test" << std::endl;
        testDueFullStillPublishesWhileEventBacklogRemains();
        std::cerr << "running control/event deterministic ordering test" << std::endl;
        testControlModeReplaysBoundedBatchBeforeNextPoll();
        std::cerr << "running third-party control mqtt config test" << std::endl;
        testControlMqttConfigUsesOnlyExactControlTopics();
        std::cerr << "running third-party takeover lifecycle test" << std::endl;
        testControlTakeoverDuplicateReleaseAndExpiry();
        std::cerr << "running third-party unsafe payload rejection test" << std::endl;
        testControlRejectsUnsafePayloads();
        std::cerr << "running concurrent construction lease preservation test" << std::endl;
        testConcurrentConstructionPreservesActiveLease();
        std::cerr << "running changed-session lease preservation test" << std::endl;
        testChangedSessionConstructionPreservesForeignLease();
        std::cerr << "running failed submission receipt test" << std::endl;
        testFailedSubmitReceiptRejectsDuplicates();
        std::cerr << "running persistent accepted receipt test" << std::endl;
        testAcceptedReceiptsSurviveRestart();
        std::cerr << "running changed target mapping receipt test" << std::endl;
        testAcceptedReceiptRejectsChangedTargetMapping();
        std::cerr << "running final device control result test" << std::endl;
        testControlPublishesFinalDeviceResults();
        std::cerr << "running final device control timeout test" << std::endl;
        testControlPublishesFinalResultTimeout();
        std::cerr << "running final device control restart recovery test" << std::endl;
        testControlRecoversPendingResultAfterRestart();
        std::cerr << "running reserved control restart phase test" << std::endl;
        testReservedControlIsNotReportedAsSubmittedAfterRestart();
        std::cerr << "running control result retention test" << std::endl;
        testControlResultRetentionKeepsPendingAndUndeliveredRecords();
        std::cerr << "running byte-identical final result replay test" << std::endl;
        testFinalControlResultReplaysPersistedPayloadVerbatim();
        std::cerr << "running durable idempotency receipt eviction test" << std::endl;
        testDurableIdempotencySurvivesOwnershipReceiptEviction();
        std::cerr << "running local-mode receipt reuse test" << std::endl;
        testLocalModeReceiptPreventsIdReuseForRemoteMode();
#ifndef _WIN32
        std::cerr << "running production publisher final result retry test" << std::endl;
        testBuiltinPublisherFailureKeepsFinalUndeliveredAndRestartRetries();
        std::cerr << "running tx-only publisher test" << std::endl;
        testTxOnlyPublisherNeverSubscribes();
#endif
        std::cout << "mqtt_forwarder_service_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "mqtt_forwarder_service_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
