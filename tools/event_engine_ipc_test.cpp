#define main legacy_event_engine_test_main
#include "event_engine_service_test.cpp"
#undef main
#include "edge_gateway/event_store_producer.hpp"
#include "edge_gateway/event_store_runtime.hpp"
#include "event_store_test_files.hpp"
#include <set>
#include <limits>
#include <thread>

int localDurabilityTest() {
    try {
        char directory[] = "/tmp/event-engine-ipc-XXXXXX";
        require(mkdtemp(directory) != nullptr, "temporary directory failed");
        EventStoreRuntimeOptions config;
        config.identity = {"engine-ipc", "1"}; config.producers = {"detector"};
        config.databasePath = std::string(directory) + "/events.db";
        config.socketPath = std::string(directory) + "/events.sock";
        EventStoreRuntime runtime(config); runtime.start();
        EventStoreProducerOptions producerConfig;
        producerConfig.client.identity = config.identity;
        producerConfig.client.socketPath = config.socketPath;
        producerConfig.client.actorId = "detector";
        auto producer = std::make_shared<AsyncEventStoreProducer>(producerConfig);
        auto fixture = makeFixture("ipc_" + std::to_string(getpid()));
        fixture.service.reset();
        EventEngineConfig engine;
        MqttConfig mqtt;
        mqtt.enabled = false;
        mqtt.alarmTopic = "disabled/nonempty/alarm";
        mqtt.changeEventTopic = "disabled/nonempty/change";
        mqtt.statusTopic = "disabled/nonempty/status";
        MqttForwardConfig third;
        third.enabled = false; third.events.enabled = true;
        third.events.alarmTopic = "disabled/third/alarm";
        third.events.changeTopic = "disabled/third/change";
        fixture.service.reset(new EventEngineService(engine, mqtt, {fixture.device}, fixture.router,
            {fixture.store.get()}, fixture.publisher, nullptr, nullptr, third, producer, "1"));
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (producer->status().phase != EventCommitPhase::Normal) {
            fixture.service->runOnce(1000);
            require(std::chrono::steady_clock::now() < end, "initial baseline timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        fixture.store->putLatest(makeValue(610001, 0, 1000));
        fixture.service->runOnce(1000);
        require(producer->drain(5000), "initial state not committed");
        fixture.store->putLatest(makeValue(610001, 100, 1100));
        fixture.store->putLatest(makeValue(610001, 0, 1200));
        fixture.service->runOnce(1200);
        require(producer->drain(5000), "local transitions not committed");
        const auto published = fixture.publisher->joinedPayloads();
        fixture.service.reset(); producer.reset(); runtime.stop();
        MqttEventOutbox read(config.databasePath, "", 12, 24, 100, 0,
            MqttEventOutbox::StorageProfile::DeleteFull, MqttEventOutbox::AccessMode::ReadOnly);
        EventStoreDatabase db(read, config.identity, config.producers, true);
        auto rows = db.readJournal(0, 64);
        require(rows.size() == 4, "two alarm transitions and two changes must be durable locally");
        require(read.pendingCount() == 0, "disabled topics unexpectedly created outbound rows");
        require(rows[1].local.kind == "alarm" && rows[1].local.event.active &&
            rows[3].local.kind == "alarm" && !rows[3].local.event.active,
            "local alarm causal ordering changed");
        fixture.store.reset(); MemoryPointStore::cleanupOrphanedSegment(fixture.storeName);
        require(published.empty(), "disabled MQTT published events: " + published);
        std::cout << "event engine IPC: MQTT disabled with nonempty topics, local alarm/change durable: PASS\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

namespace {
class RecordingProducerSink final : public IEventCommitSink {
public:
    explicit RecordingProducerSink(std::shared_ptr<AsyncEventStoreProducer> value) : producer(std::move(value)) {}
    std::shared_ptr<AsyncEventStoreProducer> producer;
    std::vector<std::vector<EventStoreLocalEvent>> accepted;
    std::size_t rejected = 0;
    bool trySubmit(const std::vector<MqttEventOutbox::EventMessage>& events,
        const std::vector<MqttEventOutbox::EventState>& states,
        const std::vector<EventStoreLocalEvent>& local) override {
        require(events.empty(), "disabled MQTT generated outbound work");
        if (!producer->trySubmit(events, states, local)) { ++rejected; return false; }
        if (!local.empty()) {
            require(local.size() == 2 && states.size() == 2 && local[0].kind == "change" && local[1].kind == "alarm" &&
                local[0].event.ts == local[1].event.ts, "one sample change/alarm was split across submits");
            accepted.push_back(local);
        }
        return true;
    }
    bool takeBaseline(std::vector<MqttEventOutbox::EventState>& states) override { return producer->takeBaseline(states); }
    void beginBaselineUpdate(std::size_t count) override { producer->beginBaselineUpdate(count); }
    bool trySubmitBaseline(const std::vector<MqttEventOutbox::EventState>& states) override {
        return producer->trySubmitBaseline(states);
    }
    bool resumeAfterBaseline() override { return producer->resumeAfterBaseline(); }
    EventCommitStatus status() const override { return producer->status(); }
};

void boundedQueueRetainsDrainedTail(std::size_t slots, bool rejectFirst) {
    char directory[] = "/tmp/engine-tail-ipc-XXXXXX";
    require(mkdtemp(directory) != nullptr, "temporary directory failed");
    struct RemoveDirectory {
        std::string path;
        ~RemoveDirectory() { removeEventStoreTestTree(path); }
    } removeDirectory{directory};
    EventStoreRuntimeOptions config;
    config.identity = {"engine-tail", "1"}; config.producers = {"detector"};
    config.databasePath = std::string(directory) + "/events.db";
    config.socketPath = std::string(directory) + "/events.sock";
    EventStoreRuntime runtime(config); runtime.start();
    EventStoreProducerOptions options;
    options.client.identity = config.identity; options.client.socketPath = config.socketPath;
    options.client.actorId = "detector"; options.client.timeoutMs = 100;
    options.queueItems = slots; options.retryMs = 1;
    auto producer = std::make_shared<AsyncEventStoreProducer>(options);
    auto sink = std::make_shared<RecordingProducerSink>(producer);
    auto fixture = makeFixture("tail_" + std::to_string(getpid()) + "_" +
        std::to_string(slots) + (rejectFirst ? "_first" : "_second"));
    fixture.service.reset();
    struct Cleanup {
        Fixture& fixture;
        ~Cleanup() { fixture.service.reset(); fixture.store.reset(); MemoryPointStore::cleanupOrphanedSegment(fixture.storeName); }
    } cleanup{fixture};
    EventEngineConfig engine; engine.updateDrainBatchSize = 128;
    MqttConfig mqtt; mqtt.enabled = false;
    mqtt.alarmTopic = "disabled/alarm"; mqtt.changeEventTopic = "disabled/change"; mqtt.statusTopic = "disabled/status";
    fixture.service.reset(new EventEngineService(engine, mqtt, {fixture.device}, fixture.router,
        {fixture.store.get()}, fixture.publisher, nullptr, nullptr, {}, sink, "1"));
    const auto pump = [&](const auto& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (!done()) {
            fixture.service->runOnce(1200);
            require(std::chrono::steady_clock::now() < deadline, "Engine retained-tail recovery timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };
    pump([&] { return producer->status().phase == EventCommitPhase::Normal; });
    fixture.store->putLatest(makeValue(610001, 0, 1000));
    fixture.service->runOnce(1000);
    require(producer->drain(5000), "initial baseline COMMIT failed");
    runtime.stop();
    // Offline runtime keeps every filler in the real bounded producer queue.
    const auto fillers = rejectFirst ? slots : slots - 1;
    for (std::size_t i = 0; i < fillers; ++i) {
        MqttEventOutbox::EventState state;
        state.stateKey = "capacity-filler:" + std::to_string(i);
        state.eventType = "change"; state.quality = 1; state.sourceTs = 1000;
        require(producer->trySubmit({}, {state}), "capacity filler rejected");
    }
    fixture.store->putLatest(makeValue(610001, 100, 1100));
    fixture.store->putLatest(makeValue(610001, 0, 1200));
    fixture.service->runOnce(1200);
    require(producer->status().phase == EventCommitPhase::GapDrain && sink->rejected == 1,
        "real bounded queue failed to reject the selected sample");
    require(sink->accepted.size() == (rejectFirst ? 0 : 1), "wrong rejection boundary");
    require(fixture.store->drainPointUpdates(128).empty(), "fixture did not drain both samples in one pass");
    runtime.start();
    pump([&] { return producer->status().phase == EventCommitPhase::Normal && sink->accepted.size() == 2 && producer->drain(0); });
    fixture.service->runOnce(1200);
    require(producer->drain(5000) && sink->accepted.size() == 2, "retained samples replayed more than once");
    MqttEventOutbox read(config.databasePath, "", 12, 24, 100, 0,
        MqttEventOutbox::StorageProfile::DeleteFull, MqttEventOutbox::AccessMode::ReadOnly);
    EventStoreDatabase db(read, config.identity, config.producers, true);
    const auto rows = db.readJournal(0, 64);
    require(rows.size() == 4 && read.pendingCount() == 0, "drained current/tail lost or fabricated journal events");
    std::set<std::string> ids;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& local = rows[i].local;
        require(local.kind == (i % 2 ? "alarm" : "change") && local.event.ts == (i < 2 ? 1100 : 1200) &&
            local.event.value == (i < 2 ? 100 : 0) && local.event.quality == 1,
            "tail recovery replaced causal samples with latest snapshot");
        ids.insert(local.event.eventId);
    }
    require(ids.size() == 4 && rows[1].local.event.active && !rows[3].local.event.active,
        "raise/clear identity or ordering lost");
    require(fixture.publisher->payloads.empty(), "disabled MQTT published during gap recovery");
}

// This sink never turns an accepted baseline into a COMMIT automatically.
// Each test step controls the durable boundary without scheduling sleeps.
class ManualCommitSink final : public IEventCommitSink {
public:
    EventCommitStatus snapshot;
    bool taken = false;
    bool declared = false;
    std::size_t expectedStates = 0, declarationCount = 0;
    std::set<std::string> submittedKeys;
    std::vector<MqttEventOutbox::EventState> pending;
    std::vector<std::size_t> batches;
    std::vector<MqttEventOutbox::EventState> durable;
    std::vector<EventStoreLocalEvent> local;
    std::size_t businessSubmits = 0;
    bool rejectNext = false, gapOnReject = false;
    std::vector<MqttEventOutbox::EventState> committedBusinessStates;
    std::function<void()> onAccepted;
    ManualCommitSink() { snapshot.phase = EventCommitPhase::GapDrain; snapshot.gaps = 1; }
    bool trySubmit(const std::vector<MqttEventOutbox::EventMessage>& events,
        const std::vector<MqttEventOutbox::EventState>& states,
        const std::vector<EventStoreLocalEvent>& journal) override {
        require(snapshot.phase == EventCommitPhase::Normal, "Engine submitted business during recovery");
        require(events.empty(), "disabled MQTT emitted an outbox event during recovery");
        if (rejectNext) {
            rejectNext = false; ++snapshot.rejected; snapshot.error = "injected batch refusal";
            if (gapOnReject) { snapshot.phase = EventCommitPhase::GapDrain; ++snapshot.gaps; }
            return false;
        }
        ++businessSubmits;
        local.insert(local.end(), journal.begin(), journal.end());
        for (const auto& state : states) {
            const auto found = std::find_if(committedBusinessStates.begin(), committedBusinessStates.end(),
                [&](const auto& prior) { return prior.stateKey == state.stateKey; });
            if (found == committedBusinessStates.end()) committedBusinessStates.push_back(state);
            else *found = state;
        }
        if (onAccepted) onAccepted();
        return true;
    }
    bool takeBaseline(std::vector<MqttEventOutbox::EventState>& states) override {
        if (snapshot.phase != EventCommitPhase::Rebase || taken) return false;
        taken = true; states = committedBusinessStates; return true;
    }
    void beginBaselineUpdate(std::size_t stateCount) override {
        require(snapshot.phase == EventCommitPhase::Rebase && taken && !declared,
            "Engine omitted baseline load or redeclared its recovery total");
        declared = true; expectedStates = stateCount; ++declarationCount;
    }
    bool trySubmitBaseline(const std::vector<MqttEventOutbox::EventState>& states) override {
        require(snapshot.phase == EventCommitPhase::Rebase && taken && declared,
            "baseline submitted before drain/declaration");
        require(!states.empty() && states.size() <= 32, "Engine baseline batch exceeds contract");
        if (!pending.empty()) return false;
        require(submittedKeys.size() + states.size() <= expectedStates, "Engine exceeded declared baseline size");
        for (const auto& state : states)
            require(submittedKeys.insert(state.stateKey).second, "Engine repeated baseline state across fragments");
        pending = states; batches.push_back(states.size()); ++snapshot.accepted;
        snapshot.queueItems = 1; return true;
    }
    bool resumeAfterBaseline() override {
        require(snapshot.phase == EventCommitPhase::Rebase && taken && declared, "invalid resume call");
        if (!pending.empty() || durable.size() != expectedStates) return false;
        snapshot.phase = EventCommitPhase::Normal; return true;
    }
    EventCommitStatus status() const override { return snapshot; }
    void finishDrain() { snapshot.phase = EventCommitPhase::Rebase; }
    void commit() {
        require(!pending.empty(), "no pending baseline to commit");
        durable.insert(durable.end(), pending.begin(), pending.end()); pending.clear();
        snapshot.queueItems = 0; ++snapshot.committed;
    }
};

void invalidInputAndRefusalIsolation(int mode) {
    auto fixture = makeFixture("input-isolation-" + std::to_string(getpid()) + "-" + std::to_string(mode));
    fixture.service.reset();
    struct Cleanup {
        Fixture& fixture;
        ~Cleanup() { fixture.service.reset(); fixture.store.reset(); MemoryPointStore::cleanupOrphanedSegment(fixture.storeName); }
    } cleanup{fixture};
    auto sink = std::make_shared<ManualCommitSink>();
    sink->snapshot.phase = EventCommitPhase::Normal; sink->snapshot.gaps = 0;
    EventEngineConfig engine; engine.updateDrainBatchSize = 128;
    MqttConfig mqtt; mqtt.enabled = false;
    mqtt.alarmTopic = "disabled/alarm"; mqtt.changeEventTopic = "disabled/change"; mqtt.statusTopic = "disabled/status";
    fixture.service.reset(new EventEngineService(engine, mqtt, {fixture.device}, fixture.router,
        {fixture.store.get()}, fixture.publisher, nullptr, nullptr, {}, sink, "1"));
    fixture.store->putLatest(makeValue(610001, 0, 1000));
    fixture.service->runOnce(1000);
    require(sink->local.empty() && sink->committedBusinessStates.size() == 2, "missing valid seed baseline");
    if (mode == 0) {
        fixture.store->putLatest(makeValue(610001, 100, 1100));
        fixture.store->putLatest(makeValue(610001, std::numeric_limits<double>::quiet_NaN(), 1110));
        fixture.store->putLatest(makeValue(610001, std::numeric_limits<double>::infinity(), 1120));
        fixture.store->putLatest(makeValue(610001, -std::numeric_limits<double>::infinity(), 1130));
        fixture.store->putLatest(makeValue(610001, 100, 0));
        fixture.store->putLatest(makeValue(610001, 0, 1200));
    } else {
        sink->rejectNext = true; sink->gapOnReject = mode == 2;
        fixture.store->putLatest(makeValue(610001, 100, 1100));
        fixture.store->putLatest(makeValue(610001, 100, 1150));
        fixture.store->putLatest(makeValue(610001, 0, 1200));
    }
    fixture.service->runOnce(1200);
    if (mode == 2) {
        require(sink->snapshot.phase == EventCommitPhase::GapDrain && sink->local.empty() &&
            fixture.service->pendingInputCount() == 3 && fixture.service->rejectedInputCount() == 0,
            "false+GapDrain consumed pending samples or counted backpressure as invalid input");
        fixture.service->runOnce(1200);
        require(fixture.service->pendingInputCount() == 3, "GapDrain lost buffered current/tail");
        sink->finishDrain();
        fixture.service->runOnce(1200);
        require(sink->expectedStates == 0, "buffered replay overwrote committed baseline with latest");
    }
    require(fixture.service->pendingInputCount() == 0 &&
        fixture.service->rejectedInputCount() == (mode == 0 ? 4u : mode == 1 ? 1u : 0u),
        "invalid-input count wrong or poisoned sample blocked the tail");
    require(sink->local.size() == 4, "valid transitions were lost after an invalid sample/refusal");
    for (std::size_t i = 0; i < sink->local.size(); ++i) {
        const auto& row = sink->local[i];
        require(row.kind == (i % 2 ? "alarm" : "change") && row.event.quality == 1 &&
            row.event.ts == (i < 2 ? (mode == 1 ? 1150 : 1100) : 1200) && row.event.value == (i < 2 ? 100 : 0),
            "rejected candidate advanced baseline or corrupted later events");
    }
    require(sink->local[1].event.active && !sink->local[3].event.active && fixture.publisher->payloads.empty(),
        "invalid sample altered physical alarm or disabled MQTT behavior");
}

void twoStoreDrainFairness() {
    const auto prefix = "engine-fairness-" + std::to_string(getpid());
    const auto hotName = prefix + "-hot", coldName = prefix + "-cold";
    struct Cleanup {
        std::string hot, cold;
        ~Cleanup() { MemoryPointStore::cleanupOrphanedSegment(hot); MemoryPointStore::cleanupOrphanedSegment(cold); }
    } cleanup{hotName, coldName};
    MemoryPointStore::cleanupOrphanedSegment(hotName);
    MemoryPointStore::cleanupOrphanedSegment(coldName);
    MemoryStoreConfig hotConfig, coldConfig;
    hotConfig.sharedMemoryName = hotName; coldConfig.sharedMemoryName = coldName;
    hotConfig.maxLatestPoints = coldConfig.maxLatestPoints = 8;
    MemoryPointStore hot(hotConfig), cold(coldConfig);
    DeviceConfig first, second;
    first.machineCode = second.machineCode = "GW_FAIR";
    first.meterCode = "hot"; second.meterCode = "cold";
    first.memoryStore.sharedMemoryName = hotName; second.memoryStore.sharedMemoryName = coldName;
    first.points = {makePoint(630001, "hot", true)};
    second.points = {makePoint(630002, "cold", true)};
    hot.registerPoints(first.machineCode, first.meterCode, first.points);
    cold.registerPoints(second.machineCode, second.meterCode, second.points);
    PointStoreRouter router;
    router.addStore(hotName, hot); router.addStore(coldName, cold);
    router.addRoutesFromDeviceConfigs({first, second}, hotName);
    auto sink = std::make_shared<ManualCommitSink>();
    sink->snapshot.phase = EventCommitPhase::Normal; sink->snapshot.gaps = 0;
    auto publisher = std::make_shared<CapturingPublisher>();
    EventEngineConfig engine; engine.updateDrainBatchSize = 1; engine.scanFallbackIntervalMs = 1000;
    MqttConfig mqtt; mqtt.enabled = false;
    mqtt.alarmTopic = "disabled/alarm"; mqtt.changeEventTopic = "disabled/change"; mqtt.statusTopic = "disabled/status";
    EventEngineService service(engine, mqtt, {first, second}, router, {&hot, &cold}, publisher,
        nullptr, nullptr, {}, sink, "1");
    hot.putLatest(makeValue(630001, 0, 1000));
    cold.putLatest(makeValue(630002, 0, 1000));
    service.runOnce(1000); service.runOnce(1000);
    require(sink->local.empty() && hot.getStats().pointUpdateCount == 0 && cold.getStats().pointUpdateCount == 0,
        "two-store initial baseline did not consume both seed samples");
    cold.putLatest(makeValue(630002, 100, 1100));
    cold.putLatest(makeValue(630002, 0, 1200));
    hot.putLatest(makeValue(630001, 100, 1300));
    std::vector<EventStoreLocalEvent> coldRows;
    int firstColdRound = 0;
    std::string trace;
    for (int round = 1; round <= 4; ++round) {
        hot.putLatest(makeValue(630001, round % 2 ? 0 : 100, 1300 + round * 100));
        const auto before = sink->local.size();
        service.runOnce(10000 + round * 1000); // Fallback is due every round, while updates remain queued.
        trace += " round=" + std::to_string(round) + " hot=" + std::to_string(hot.getStats().pointUpdateCount) +
            " cold=" + std::to_string(cold.getStats().pointUpdateCount) + " journal=" + std::to_string(sink->local.size());
        require(sink->local.size() - before <= 2, "fallback bypassed the total one-sample drain budget");
        require(hot.getStats().pointUpdateCount > 0, "hot store fixture stopped generating backlog");
        for (std::size_t i = before; i < sink->local.size(); ++i) {
            if (sink->local[i].event.index != 630002) continue;
            if (!firstColdRound) firstColdRound = round;
            coldRows.push_back(sink->local[i]);
        }
        if (round == 2) require(firstColdRound > 0 && firstColdRound <= 2,
            "hot first store starved second store beyond two rounds:" + trace);
    }
    require(coldRows.size() == 4, "cold store oldest high/low samples lost to latest fallback");
    for (std::size_t i = 0; i < coldRows.size(); ++i) {
        require(coldRows[i].kind == (i % 2 ? "alarm" : "change") &&
            coldRows[i].event.ts == (i < 2 ? 1100 : 1200) && coldRows[i].event.value == (i < 2 ? 100 : 0),
            "cold sample FIFO order was bypassed by latest snapshot");
    }
    require(coldRows[1].event.active && !coldRows[3].event.active && publisher->payloads.empty(),
        "cold raise/clear or disabled MQTT contract failed");
}

class InjectingStatusPublisher final : public CapturingPublisher {
public:
    std::function<void()> afterStatus;
    void publishJsonMessage(const std::string& topic, const std::string& payload) override {
        CapturingPublisher::publishJsonMessage(topic, payload);
        if (payload.find("\"event\":\"report-on-change\"") != std::string::npos && afterStatus) {
            auto inject = std::move(afterStatus);
            afterStatus = {};
            inject();
        }
    }
};

void fallbackRetainsConcurrentInputs(bool secondStore, bool fallbackOnly,
    MqttEventOutbox::StorageProfile profile) {
    char directory[] = "/tmp/engine-fallback-ipc-XXXXXX";
    require(mkdtemp(directory) != nullptr, "fallback temporary directory failed");
    struct RemoveDirectory {
        std::string path;
        ~RemoveDirectory() { removeEventStoreTestTree(path); }
    } removeDirectory{directory};
    EventStoreRuntimeOptions config;
    config.identity = {"engine-fallback", "1"}; config.producers = {"detector"};
    config.databasePath = std::string(directory) + "/events.db";
    config.socketPath = std::string(directory) + "/events.sock";
    config.profile = profile;
    EventStoreRuntime runtime(config); runtime.start();
    EventStoreProducerOptions options;
    options.client.identity = config.identity; options.client.socketPath = config.socketPath;
    options.client.actorId = "detector";
    auto producer = std::make_shared<AsyncEventStoreProducer>(options);
    auto sink = std::make_shared<RecordingProducerSink>(producer);
    auto fixture = makeFixture(std::string("fallback-race-") + std::to_string(getpid()) +
        (secondStore ? "-other" : "-same") + (fallbackOnly ? "-snapshot" : "-fifo"));
    fixture.service.reset();
    const auto otherName = fixture.storeName + "-other";
    struct Cleanup {
        Fixture& fixture;
        std::string other;
        ~Cleanup() {
            fixture.service.reset(); fixture.store.reset();
            MemoryPointStore::cleanupOrphanedSegment(fixture.storeName);
            MemoryPointStore::cleanupOrphanedSegment(other);
        }
    } cleanup{fixture, otherName};
    MemoryStoreConfig memory; memory.sharedMemoryName = otherName; memory.maxLatestPoints = 8;
    MemoryPointStore other(memory);
    DeviceConfig device; device.machineCode = fixture.device.machineCode; device.meterCode = "other";
    device.memoryStore.sharedMemoryName = otherName;
    device.points = {makePoint(630002, "other", true)};
    other.registerPoints(device.machineCode, device.meterCode, device.points);
    fixture.router.addStore(otherName, other);
    fixture.router.addRoutesFromDeviceConfigs({device}, otherName);
    auto publisher = std::make_shared<InjectingStatusPublisher>();
    EventEngineConfig engine; engine.scanFallbackIntervalMs = 1000; engine.updateDrainBatchSize = 128;
    MqttConfig mqtt; mqtt.enabled = true;
    mqtt.alarmTopic.clear(); mqtt.changeEventTopic.clear(); mqtt.statusTopic = "diagnostic-only";
    fixture.service.reset(new EventEngineService(engine, mqtt, {fixture.device, device}, fixture.router,
        {fixture.store.get(), &other}, publisher, nullptr, nullptr, {}, sink, "1"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (producer->status().phase != EventCommitPhase::Normal) {
        fixture.service->runOnce(1000);
        require(std::chrono::steady_clock::now() < deadline, "fallback producer not ready");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    fixture.store->putLatest(makeValue(610001, 0, 1000));
    other.putLatest(makeValue(630002, 0, 1000));
    fixture.service->runOnce(1000);
    require(producer->drain(5000), "fallback baseline did not commit");
    publisher->afterStatus = [&] {
        // Deterministically model the writer running while synchronous status
        // publication blocks the detector, after its original backlog check.
        auto& target = secondStore ? other : *fixture.store;
        const auto index = secondStore ? 630002u : 610001u;
        target.putLatest(makeValue(index, secondStore ? 100 : 0, 6100));
        target.putLatest(makeValue(index, secondStore ? 0 : 100, 6200));
    };
    fixture.store->putLatest(makeValue(610001, 100, 6000));
    if (fallbackOnly) {
        // Deliberately remove FIFO evidence to exercise latest-state fallback.
        // New writes from the callback occur after its candidate was captured.
        require(fixture.store->drainPointUpdates(128).size() == 1, "fallback-only seed not drained");
    }
    fixture.service->runOnce(6000);
    require(!publisher->afterStatus, "status injection did not run (fallback disabled?)");
    require(sink->accepted.size() == 1, "fallback consumed a newly queued transition ahead of FIFO");
    fixture.service->runOnce(6300);
    require(producer->drain(5000), "fallback transition commits failed");
    MqttEventOutbox read(config.databasePath, "", 12, 24, 100, 0, profile,
        MqttEventOutbox::AccessMode::ReadOnly);
    EventStoreDatabase db(read, config.identity, config.producers, true);
    const auto rows = db.readJournal(0, 64);
    require(rows.size() == 6, "fallback skipped queued intermediate alarm/change transitions");
    std::set<std::string> ids;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& event = rows[i].local.event;
        const auto step = i / 2;
        require(rows[i].local.kind == (i % 2 ? "alarm" : "change") &&
            event.index == (step && secondStore ? 630002u : 610001u) &&
            event.ts == 6000 + static_cast<std::int64_t>(step) * 100 &&
            event.value == (step == 0 ? 100 : step == 1 ? (secondStore ? 100 : 0) : (secondStore ? 0 : 100)),
            "fallback changed FIFO identity/order/value");
        ids.insert(event.eventId);
    }
    require(ids.size() == 6 && read.pendingCount() == 0, "fallback duplicated IDs or diagnostic-only test made outbox rows");
    // A stale, queue-less snapshot must not regress the accepted state.
    fixture.store->putLatest(makeValue(610001, 0, 900));
    (void)fixture.store->drainPointUpdates(128);
    fixture.service->runOnce(8000);
    require(producer->drain(5000) && db.readJournal(0, 64).size() == 6, "stale fallback regressed state");
}

void fallbackRejectsUnownedStores(bool nullOnly) {
    auto fixture = makeFixture("unowned-" + std::to_string(getpid()) + (nullOnly ? "-null" : "-empty"));
    fixture.service.reset();
    struct Cleanup {
        Fixture& f;
        ~Cleanup() { f.service.reset(); f.store.reset(); MemoryPointStore::cleanupOrphanedSegment(f.storeName); }
    } cleanup{fixture};
    auto sink = std::make_shared<ManualCommitSink>();
    EventEngineConfig engine; engine.scanFallbackIntervalMs = 1000;
    MqttConfig mqtt;
    std::vector<MemoryPointStore*> stores;
    if (nullOnly) stores.push_back(nullptr);
    fixture.service.reset(new EventEngineService(engine, mqtt, {fixture.device}, fixture.router,
        stores, fixture.publisher, nullptr, nullptr, {}, sink, "1"));
    fixture.store->putLatest(makeValue(610001, 0, 1000));
    fixture.service->runOnce(1000);
    fixture.store->putLatest(makeValue(610001, 100, 6000));
    fixture.service->runOnce(6000);
    require(sink->local.empty(), "fallback applied an unchecked router store");
    require(fixture.store->getStats().pointUpdateCount == 2, "unowned FIFO was consumed");
    require(!fixture.router.usesOnlyStores(stores) &&
        fixture.router.usesOnlyStores({fixture.store.get()}), "router store coverage mismatch");
}

void diagnosticSummaryRetainsPartialCount(std::size_t cut) {
    auto fixture = makeFixture("summary-" + std::to_string(getpid()) + "-" + std::to_string(cut));
    fixture.service.reset();
    struct Cleanup {
        Fixture& f;
        ~Cleanup() { f.service.reset(); f.store.reset(); MemoryPointStore::cleanupOrphanedSegment(f.storeName); }
    } cleanup{fixture};
    auto sink = std::make_shared<ManualCommitSink>();
    sink->snapshot.phase = EventCommitPhase::Normal; sink->snapshot.gaps = 0;
    EventEngineConfig engine; engine.updateDrainBatchSize = 128;
    MqttConfig mqtt; mqtt.enabled = true;
    mqtt.alarmTopic.clear(); mqtt.changeEventTopic.clear(); mqtt.statusTopic = "diagnostic-only";
    fixture.service.reset(new EventEngineService(engine, mqtt, {fixture.device}, fixture.router,
        {fixture.store.get()}, fixture.publisher, nullptr, nullptr, {}, sink, "1"));
    fixture.store->putLatest(makeValue(610001, 0, 1000));
    fixture.service->runOnce(1000);
    std::size_t accepted = 0;
    if (cut) sink->onAccepted = [&] {
        if (++accepted == cut) { sink->rejectNext = true; sink->gapOnReject = true; }
    };
    for (int i = 0; i < 4; ++i) fixture.store->putLatest(makeValue(610001, i % 2 ? 0 : 100, 1100 + i * 100));
    fixture.service->runOnce(1400);
    const auto firstCount = cut ? cut : 4;
    require(fixture.publisher->payloads.size() == 1 &&
        fixture.publisher->payloads[0].find("\"valueCount\":" + std::to_string(firstCount)) != std::string::npos,
        "IPC diagnostics published per sample or lost accepted partial count");
    require(sink->local.size() == firstCount * 2, "summary changed per-sample commits");
    if (cut) {
        require(fixture.service->pendingInputCount() == 4 - cut, "partial summary consumed rejected input");
        fixture.service->runOnce(1400);
        require(fixture.publisher->payloads.size() == 1, "blocked retry duplicated partial summary");
        sink->onAccepted = {}; sink->finishDrain();
        fixture.service->runOnce(1400);
        require(fixture.publisher->payloads.size() == 2 &&
            fixture.publisher->payloads[1].find("\"valueCount\":" + std::to_string(4 - cut)) != std::string::npos,
            "resumed summary lost/recounted accepted prefix");
    }
    require(sink->local.size() == 8 && fixture.service->pendingInputCount() == 0, "summary lost transactional events");
    fixture.service->runOnce(1400);
    require(fixture.publisher->payloads.size() == (cut ? 2u : 1u), "idle retry repeated summary");
}

void multiBatchBaselineNoFabrication(bool includeInvalid = false) {
    const auto name = "engine-baseline-gates-" + std::to_string(getpid());
    struct Cleanup {
        std::string name;
        ~Cleanup() { MemoryPointStore::cleanupOrphanedSegment(name); }
    } cleanup{name};
    MemoryPointStore::cleanupOrphanedSegment(name);
    MemoryStoreConfig memory;
    memory.sharedMemoryName = name; memory.maxLatestPoints = 128;
    MemoryPointStore store(memory);
    DeviceConfig device;
    device.machineCode = "GW_TEST"; device.meterCode = "METER_TEST";
    device.memoryStore.sharedMemoryName = name;
    const std::uint32_t pointCount = includeInvalid ? 40 : 35;
    for (std::uint32_t i = 0; i < pointCount; ++i)
        device.points.push_back(makePoint(620000 + i, "p" + std::to_string(i), true));
    store.registerPoints(device.machineCode, device.meterCode, device.points);
    PointStoreRouter router;
    router.addStore(name, store); router.addRoutesFromDeviceConfigs({device}, name);
    for (std::uint32_t i = 0; i < pointCount; ++i) {
        auto value = makeValue(620000 + i, 100, 1000);
        if (i == 35) value.quality = 0;
        if (i == 36) value.value = std::numeric_limits<double>::quiet_NaN();
        if (i == 37) value.value = std::numeric_limits<double>::infinity();
        if (i == 38) value.value = -std::numeric_limits<double>::infinity();
        if (i == 39) value.ts = 0;
        store.putLatest(value);
    }
    auto publisher = std::make_shared<CapturingPublisher>();
    auto sink = std::make_shared<ManualCommitSink>();
    EventEngineConfig engine;
    engine.updateDrainBatchSize = 128;
    MqttConfig mqtt;
    mqtt.enabled = false; mqtt.alarmTopic.clear(); mqtt.changeEventTopic.clear(); mqtt.statusTopic.clear();
    EventEngineService service(engine, mqtt, {device}, router, {&store}, publisher,
        nullptr, nullptr, {}, sink, "1");
    service.runOnce(1000);
    require(sink->batches.empty() && sink->businessSubmits == 0,
        "Engine rebased or detected before accepted queue drained");
    sink->finishDrain();
    service.runOnce(1000);
    require(sink->batches == std::vector<std::size_t>{32}, "missing first bounded baseline batch");
    require(sink->declarationCount == 1 && sink->expectedStates == 70,
        "Engine did not declare the complete >64-state baseline before fragmentation");
    for (int batch = 0; batch < 3; ++batch) {
        require(sink->status().phase == EventCommitPhase::Rebase && sink->businessSubmits == 0,
            "Engine resumed before all baseline COMMITs");
        const auto accepted = sink->snapshot.accepted;
        service.runOnce(1000);
        require(sink->snapshot.accepted == accepted && sink->businessSubmits == 0,
            "uncommitted full baseline queue advanced or fabricated business events");
        sink->commit();
        service.runOnce(1000);
    }
    require(sink->batches == std::vector<std::size_t>({32, 32, 6}),
        "quality=1 baseline should contain 35 valid points, two states each");
    require(sink->snapshot.phase == EventCommitPhase::Normal && sink->durable.size() == 70,
        "Engine failed to resume after final baseline COMMIT");
    require(sink->declarationCount == 1, "Engine reset declared total while retrying partial baseline");
    std::string unexpected;
    for (const auto& row : sink->local) unexpected += " " + row.kind + ":" +
        std::to_string(row.event.index) + ":quality=" + std::to_string(row.event.quality);
    require(sink->local.empty() && publisher->payloads.empty(),
        "gap baseline fabricated events: journal=" + std::to_string(sink->local.size()) +
        unexpected + " published=" + publisher->joinedPayloads());
    std::set<std::string> keys;
    for (const auto& state : sink->durable) {
        require(state.quality == 1 && state.index < 620035, "invalid quality/value/timestamp included in baseline");
        keys.insert(state.stateKey);
    }
    require(keys.size() == 70, "multi-batch baseline duplicated/omitted state keys");
    store.putLatest(makeValue(620000, 0, 2000));
    service.runOnce(2000);
    require(sink->local.size() == 2, "first post-rebase real transition was lost/duplicated");
    require(sink->local[0].kind == "change" && sink->local[1].kind == "alarm" &&
        !sink->local[1].event.active && sink->local[1].event.quality == 1,
        "post-rebase alarm must clear the committed active baseline");
}
}

int main() {
    int failures = 0;
    for (bool nullOnly : {false, true}) {
        try { fallbackRejectsUnownedStores(nullOnly); std::cout << "engine-fallback-unowned-" << nullOnly << ": PASS\n"; }
        catch (const std::exception& error) { ++failures; std::cerr << "engine-fallback-unowned: " << error.what() << '\n'; }
    }
    for (bool second : {false, true}) for (bool snapshot : {false, true})
        for (auto profile : {MqttEventOutbox::StorageProfile::DeleteFull, MqttEventOutbox::StorageProfile::WalFull}) {
            const auto name = std::string("engine-fallback-late-writer-") + (second ? "other" : "same") +
                (snapshot ? "-after-candidate" : "-during-drain") +
                (profile == MqttEventOutbox::StorageProfile::WalFull ? "-wal" : "-delete");
            try { fallbackRetainsConcurrentInputs(second, snapshot, profile); std::cout << name << ": PASS\n"; }
            catch (const std::exception& error) { ++failures; std::cerr << name << ": " << error.what() << '\n'; }
        }
    for (std::size_t cut : {0u, 1u, 2u}) {
        const auto name = "engine-diagnostic-summary-cut-" + std::to_string(cut);
        try { diagnosticSummaryRetainsPartialCount(cut); std::cout << name << ": PASS\n"; }
        catch (const std::exception& error) { ++failures; std::cerr << name << ": " << error.what() << '\n'; }
    }
    try { require(localDurabilityTest() == 0, "existing IPC durability test failed"); }
    catch (const std::exception& error) { ++failures; std::cerr << "local-durability: " << error.what() << '\n'; }
    try { multiBatchBaselineNoFabrication(); std::cout << "engine-gated-multibatch-quality1-baseline: PASS\n"; }
    catch (const std::exception& error) { ++failures; std::cerr << "engine-gated-multibatch-baseline: " << error.what() << '\n'; }
    try { multiBatchBaselineNoFabrication(true); std::cout << "engine-invalid-quality-after-rebase: PASS\n"; }
    catch (const std::exception& error) { ++failures; std::cerr << "engine-invalid-quality-after-rebase: " << error.what() << '\n'; }
    for (std::size_t slots : {1u, 2u}) for (bool first : {true, false}) {
        const auto name = "engine-real-queue-" + std::to_string(slots) + (first ? "-current-and-tail" : "-tail");
        try { boundedQueueRetainsDrainedTail(slots, first); std::cout << name << ": PASS\n"; }
        catch (const std::exception& error) { ++failures; std::cerr << name << ": " << error.what() << '\n'; }
    }
    try { twoStoreDrainFairness(); std::cout << "engine-two-store-budget1-fairness-fifo: PASS\n"; }
    catch (const std::exception& error) { ++failures; std::cerr << "engine-two-store-fairness: " << error.what() << '\n'; }
    for (int mode = 0; mode < 3; ++mode) {
        const std::string name = mode == 0 ? "engine-nan-inf-zero-ts" : mode == 1 ? "engine-false-normal" : "engine-false-gapdrain";
        try { invalidInputAndRefusalIsolation(mode); std::cout << name << ": PASS\n"; }
        catch (const std::exception& error) { ++failures; std::cerr << name << ": " << error.what() << '\n'; }
    }
    return failures ? 1 : 0;
}
