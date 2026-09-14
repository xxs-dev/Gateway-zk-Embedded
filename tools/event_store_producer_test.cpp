#define main event_store_client_contract_test_main
#include "event_store_client_test.cpp"
#undef main
#include "edge_gateway/event_store_producer.hpp"
#include <atomic>
#include "edge_gateway/event_store_runtime.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace edge_gateway;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class Predicate> void until(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < end, "condition timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
void rebase(AsyncEventStoreProducer& producer, std::size_t count) {
    std::vector<MqttEventOutbox::EventState> baseline;
    until([&] { return producer.takeBaseline(baseline); });
    require(baseline.size() == count, "incorrect committed baseline");
    producer.beginBaselineUpdate(0);
    require(producer.resumeAfterBaseline(), "zero-write baseline did not resume");
}
MqttEventOutbox::EventState state(int value) {
    MqttEventOutbox::EventState item;
    item.stateKey = "alarm:1:high"; item.eventType = "alarm"; item.index = 1;
    item.alarmType = "high"; item.active = value != 0; item.value = value;
    item.quality = 1;
    item.sourceTs = 1700000000000 + value; item.lifecycle = "test:" + std::to_string(value);
    return item;
}
MqttEventOutbox::EventMessage event(int sequence) {
    MqttEventOutbox::EventMessage item;
    item.eventId = "test:" + std::to_string(sequence); item.eventType = "alarm";
    item.topic = "test/alarm"; item.payload = std::to_string(sequence);
    item.eventTs = 1700000000000 + sequence;
    return item;
}
}
int producerHappyPath() {
    try {
        Directory directory;
        EventStoreRuntimeOptions runtime;
        runtime.identity = {"async-test", "1"}; runtime.producers = {"detector"};
        runtime.databasePath = directory.path + "/events.db";
        runtime.socketPath = directory.path + "/events.sock";
        EventStoreRuntime service(runtime); service.start();
        EventStoreProducerOptions options;
        options.client.identity = runtime.identity; options.client.socketPath = runtime.socketPath;
        options.client.actorId = "detector"; options.client.timeoutMs = 200;
        options.retryMs = 10; options.queueItems = 2;
        {
            AsyncEventStoreProducer producer(options);
            rebase(producer, 0);
            require(producer.trySubmit({}, {state(0)}), "baseline rejected");
            require(producer.drain(5000), "baseline did not commit");
            require(producer.trySubmit({event(1)}, {state(1)}), "raise rejected");
            require(producer.drain(5000), "raise did not commit");
            require(producer.trySubmit({event(2)}, {state(0)}), "clear rejected");
            require(producer.drain(5000), "clear did not commit");
            require(producer.status().committed == 3, "commit count mismatch");

            service.stop();
            const auto before = std::chrono::steady_clock::now();
            require(producer.trySubmit({event(3)}, {state(1)}), "disconnected enqueue failed");
            require(producer.trySubmit({event(4)}, {state(0)}), "disconnected enqueue 2 failed");
            require(!producer.trySubmit({event(5)}, {state(1)}), "full queue accepted request");
            require(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(100),
                "detection waited for IPC");
            require(producer.status().phase == EventCommitPhase::GapDrain, "missing GAP_DRAIN");
            require(!producer.drain(20), "offline work falsely committed");
            service.start();
            rebase(producer, 1);
            require(producer.status().committed == 5, "accepted transitions were lost");
            require(producer.status().gaps == 1, "gap count mismatch");
            require(producer.trySubmit({event(6)}, {state(1)}), "resume rejected");
            require(producer.drain(5000), "resume commit failed");
        }
        {
            AsyncEventStoreProducer restarted(options);
            rebase(restarted, 1);
            require(restarted.trySubmit({event(7)}, {state(0)}), "restart state rejected");
            require(restarted.drain(5000), "restart CAS did not use committed version");
        }
        service.stop();
        MqttEventOutbox read(runtime.databasePath, "", 1, 1, 16, 0,
            MqttEventOutbox::StorageProfile::DeleteFull, MqttEventOutbox::AccessMode::ReadOnly);
        require(read.pendingCount() == 6, "event count includes rejected event or lost transition");
        std::cout << "async producer baseline, ordered commits, outage, GAP_DRAIN, rebase, restart: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}

namespace {
struct CommitGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, released = false;
    void arrive() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true; cv.notify_all();
        require(cv.wait_for(lock, std::chrono::seconds(8), [&] { return released; }), "commit gate timeout");
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        require(cv.wait_for(lock, std::chrono::seconds(8), [&] { return entered; }), "commit gate not reached");
    }
    void release() { std::lock_guard<std::mutex> lock(mutex); released = true; cv.notify_all(); }
};
struct ReleaseGate {
    CommitGate& gate;
    ~ReleaseGate() { gate.release(); }
};
EventStoreProducerOptions asyncOptions(const Lab& lab) {
    EventStoreProducerOptions options;
    options.client = lab.client(); options.queueItems = 128;
    options.queueBytes = 4 * 1024 * 1024; options.retryMs = 1;
    return options;
}

void lostCommitCannotSkip() {
    Lab lab;
    CommitGate retryGate;
    AsyncEventStoreProducer producer(asyncOptions(lab));
    ReleaseGate release{retryGate};
    rebase(producer, 0);
    lab.proxy.arm(kAppend, ReplyProxy::Fault::Rewrite, [&](const std::string&) {
        lab.proxy.arm(kAppend, ReplyProxy::Fault::Rewrite, [&](const std::string& reply) {
            retryGate.arrive(); return reply;
        });
        return "{"; // Real COMMIT succeeded, but the response is unreadable.
    });
    require(producer.trySubmit({event(101)}, {state(1)}), "first append rejected");
    require(producer.trySubmit({event(102)}, {state(0)}), "second append rejected");
    retryGate.wait();
    const auto pending = producer.status();
    require(pending.unknown && pending.committed == 0 && pending.queueItems == 2,
        "unknown COMMIT skipped/dequeued accepted work");
    require(!producer.drain(0), "unknown COMMIT reported drained");
    require(pendingCount(lab.options) == 1, "later sequence overtook unresolved COMMIT");
    retryGate.release();
    require(producer.drain(5000), "reply-loss reconciliation failed");
    lab.proxy.waitForExchanges(kAppend, 3);
    const auto exchanges = lab.proxy.exchanges(kAppend);
    require(exchanges.size() == 3 && exchanges[0].request == exchanges[1].request,
        "retry changed immutable request bytes or skipped a sequence");
    require(integerField(field(parse(exchanges[0].request), "args"), "sequence") == 1 &&
        integerField(field(parse(exchanges[2].request), "args"), "sequence") == 2,
        "post-reconciliation sequence was not contiguous");
    require(producer.status().committed == 2 && pendingCount(lab.options) == 2,
        "lost reply duplicated or lost an event");
    lab.proxy.checked();
}

void concurrentSubmit() {
    Lab lab;
    CommitGate gate;
    AsyncEventStoreProducer producer(asyncOptions(lab));
    ReleaseGate release{gate};
    rebase(producer, 0);
    lab.proxy.arm(kAppend, ReplyProxy::Fault::Rewrite, [&](const std::string& reply) {
        gate.arrive(); return reply;
    });
    require(producer.trySubmit({event(200)}, {}), "barrier event rejected");
    gate.wait();
    std::atomic<int> accepted{0}, failures{0};
    std::mutex startMutex;
    std::condition_variable startCv;
    int ready = 0;
    bool start = false;
    std::vector<std::thread> threads;
    for (int worker = 0; worker < 8; ++worker) threads.emplace_back([&, worker] {
        {
            std::unique_lock<std::mutex> lock(startMutex);
            ++ready; startCv.notify_all(); startCv.wait(lock, [&] { return start; });
        }
        for (int item = 0; item < 8; ++item) {
            try { if (producer.trySubmit({event(201 + worker * 8 + item)}, {})) ++accepted; else ++failures; }
            catch (...) { ++failures; }
        }
    });
    {
        std::unique_lock<std::mutex> lock(startMutex);
        startCv.wait(lock, [&] { return ready == 8; }); start = true; startCv.notify_all();
    }
    for (auto& thread : threads) thread.join();
    require(accepted == 64 && failures == 0 && producer.status().queueItems == 65,
        "concurrent submit lost an accepted batch or enforced wrong thread owner");
    gate.release();
    require(producer.drain(8000), "concurrent queue did not drain");
    require(pendingCount(lab.options) == 65 && producer.status().committed == 65,
        "concurrent event conservation failed");
    const auto exchanges = lab.proxy.exchanges(kAppend);
    require(exchanges.size() == 65, "unexpected concurrent replay");
    std::set<std::string> ids;
    for (std::size_t i = 0; i < exchanges.size(); ++i) {
        const auto args = field(parse(exchanges[i].request), "args");
        require(integerField(args, "sequence") == static_cast<std::int64_t>(i + 1), "sequence race");
        ids.insert(stringField(*field(args, "events").asArray().values.front(), "eventId"));
    }
    require(ids.size() == 65, "concurrent submit duplicated an identity");
}

void actorConflict() {
    Lab lab;
    auto options = asyncOptions(lab);
    AsyncEventStoreProducer owner(options);
    rebase(owner, 0);
    AsyncEventStoreProducer intruder(options);
    until([&] { return intruder.status().phase == EventCommitPhase::Fenced; });
    require(!intruder.status().error.empty(), "actor conflict is not observable");
    require(intruder.status().queueItems == 0 && !intruder.drain(0),
        "empty Fenced producer falsely reported drained");
    require(!intruder.trySubmit({event(300)}, {}), "conflicting actor accepted work");
    require(owner.trySubmit({event(301)}, {}) && owner.drain(5000), "conflict fenced legitimate owner");
    require(lab.proxy.exchanges("RegisterProducer").size() == 1 && pendingCount(lab.options) == 1,
        "actor conflict registered a second owner");
}

void replacedActorDoesNotStealBack() {
    Lab lab;
    AsyncEventStoreProducer producer(asyncOptions(lab));
    rebase(producer, 0);
    const auto registration = callEventStore(lab.options.socketPath,
        wire(lab.options, "RegisterProducer", object({{"producerId", text("p0")},
            {"sessionId", text("explicit-replacement")}, {"expectedEpoch", decimal(1)}})), kTimeoutMs);
    require(field(parse(registration), "ok").asBool(), "failed to inject actor replacement");
    require(producer.trySubmit({event(350)}, {}), "replacement test request was not queued");
    until([&] { return producer.status().phase == EventCommitPhase::Fenced; });
    const auto status = producer.status();
    require(status.queueItems == 1 && status.committed == 0 && !status.error.empty() && !producer.drain(0),
        "replaced actor skipped or falsely committed pending request");
    require(lab.proxy.exchanges("RegisterProducer").size() == 1 && pendingCount(lab.options) == 0,
        "replaced producer auto-registered to steal ownership back");
}

void invalidBatchObservable() {
    Lab lab;
    AsyncEventStoreProducer producer(asyncOptions(lab));
    rebase(producer, 0);
    require(!producer.trySubmit({}, {}), "empty batch accepted");
    const auto status = producer.status();
    require(status.rejected == 1 && status.phase == EventCommitPhase::Normal && !status.error.empty(),
        "validation must be observable without fencing the worker");
    require(lab.proxy.exchanges(kAppend).empty(), "invalid batch reached runtime");
    require(producer.trySubmit({event(390)}, {}) && producer.drain(5000),
        "validation failure blocked subsequent valid work");
}

void journalConflictFences() {
    Lab lab;
    AsyncEventStoreProducer producer(asyncOptions(lab));
    rebase(producer, 0);
    EventStoreLocalEvent local;
    local.kind = "alarm"; local.configGeneration = lab.options.identity.configGeneration;
    local.event.eventId = "journal-conflict:1";
    local.event.ts = 1700000000000; local.event.index = 1;
    local.event.alarmType = "high"; local.event.active = true;
    local.event.value = 10; local.event.threshold = 5; local.event.quality = 1;
    require(producer.trySubmit({}, {}, {local}) && producer.drain(5000), "initial journal COMMIT failed");
    local.event.value = 11;
    require(producer.trySubmit({event(380)}, {}, {local}), "conflicting journal batch not queued");
    lab.proxy.waitForExchanges(kAppend, 2);
    require(lab.proxy.exchanges(kAppend).at(1).runtimeReply.find("JOURNAL_CONFLICT") != std::string::npos,
        "runtime did not return a real JOURNAL_CONFLICT");
    try { until([&] { return producer.status().phase == EventCommitPhase::Fenced; }); }
    catch (const std::exception&) {
        throw std::runtime_error("JOURNAL_CONFLICT did not fence; client error=" + producer.status().error +
            "; append exchanges=" + std::to_string(lab.proxy.exchanges(kAppend).size()));
    }
    const auto status = producer.status();
    require(status.committed == 1 && status.queueItems == 1 && !status.error.empty(),
        "journal conflict lost pending work or falsely committed");
    require(!producer.drain(100) && !producer.trySubmit({event(381)}, {}),
        "journal-conflicted producer drained or accepted later work");
    lab.proxy.waitForExchanges(kAppend, 2);
    const auto exchanges = lab.proxy.exchanges(kAppend);
    require(exchanges.size() == 2 && exchanges.back().runtimeReply.find("JOURNAL_CONFLICT") != std::string::npos,
        "real journal conflict was retried or returned another error");
    MqttEventOutbox read(lab.options.databasePath, "", 1, 1, 16, 0,
        Profile::DeleteFull, MqttEventOutbox::AccessMode::ReadOnly);
    EventStoreDatabase db(read, lab.options.identity, lab.options.producers, true);
    const auto rows = db.readJournal(0, 64);
    require(rows.size() == 1 && rows.front().local.event.value == 10 && pendingCount(lab.options) == 0,
        "conflicting transaction changed journal or partially committed outbox");
    lab.proxy.checked();
}

void queueGapRequiresEveryBaselineCommit() {
    Lab lab;
    auto options = asyncOptions(lab); options.queueItems = 1;
    CommitGate first;
    AsyncEventStoreProducer producer(options);
    ReleaseGate release{first};
    rebase(producer, 0);
    lab.proxy.arm(kAppend, ReplyProxy::Fault::Rewrite, [&](const std::string& reply) {
        first.arrive(); return reply;
    });
    require(producer.trySubmit({event(500)}, {state(1)}), "initial accepted work rejected");
    first.wait();
    require(!producer.trySubmit({event(501)}, {state(0)}), "bounded queue accepted overflow");
    require(producer.status().phase == EventCommitPhase::GapDrain, "overflow did not enter GAP_DRAIN");
    std::vector<MqttEventOutbox::EventState> restored;
    require(!producer.takeBaseline(restored), "baseline escaped before earlier COMMIT reply");
    first.release();
    until([&] { return producer.takeBaseline(restored); });
    require(restored.size() == 1 && restored.front().active && producer.status().committed == 1,
        "post-drain baseline skipped accepted transition");
    producer.beginBaselineUpdate(65);
    require(!producer.resumeAfterBaseline(), "declared but unsubmitted baseline resumed");
    for (int batch = 0; batch < 3; ++batch) {
        CommitGate gate;
        ReleaseGate releaseBatch{gate};
        lab.proxy.arm(kAppend, ReplyProxy::Fault::Rewrite, [&](const std::string& reply) {
            gate.arrive(); return reply;
        });
        std::vector<MqttEventOutbox::EventState> states;
        const int count = batch == 2 ? 1 : 32;
        for (int i = 0; i < count; ++i) {
            auto item = state(0); item.stateKey = "baseline:" + std::to_string(batch * 32 + i);
            item.quality = 1; states.push_back(item);
        }
        require(producer.trySubmitBaseline(states), "bounded baseline batch rejected");
        gate.wait();
        require(!producer.resumeAfterBaseline() && !producer.drain(0),
            "producer resumed with baseline COMMIT still unknown to client");
        require(!producer.trySubmitBaseline({state(0)}), "full baseline queue accepted another batch");
        require(pendingCount(lab.options) == 1, "baseline fabricated an outbound event");
        gate.release();
        require(producer.drain(5000), "baseline COMMIT reply not reconciled");
        require(producer.status().phase == EventCommitPhase::Rebase,
            "producer auto-resumed before caller finished all baseline batches");
        if (batch < 2) require(!producer.resumeAfterBaseline(),
            "empty queue allowed resume before all 65 declared states were submitted");
    }
    require(producer.resumeAfterBaseline(), "final committed baseline did not resume");
    require(producer.trySubmit({event(502)}, {state(0)}) && producer.drain(5000), "real post-gap event failed");
    require(pendingCount(lab.options) == 2 && producer.status().committed == 5,
        "gap recovery lost an event or counted a rejected/fabricated event");
    const auto exchanges = lab.proxy.exchanges(kAppend);
    require(exchanges.size() == 5, "unexpected baseline replay count");
    for (std::size_t i = 1; i <= 3; ++i) {
        const auto args = field(parse(exchanges[i].request), "args");
        require(field(args, "events").asArray().values.empty() && field(args, "localEvents").asArray().values.empty(),
            "state-only rebase manufactured journal/outbox events");
    }
    std::set<std::string> keys;
    const std::size_t sizes[] = {32, 32, 1};
    for (std::size_t i = 1; i <= 3; ++i) {
        const auto args = field(parse(exchanges[i].request), "args");
        const auto& states = field(args, "states").asArray().values;
        require(states.size() == sizes[i - 1], "declared baseline was truncated at the 64-state boundary");
        for (const auto& item : states) keys.insert(stringField(*item, "stateKey"));
    }
    require(keys.size() == 65, "baseline fragments repeated or omitted a state key");
    MqttEventOutbox read(lab.options.databasePath, "", 1, 1, 16, 0,
        Profile::DeleteFull, MqttEventOutbox::AccessMode::ReadOnly);
    EventStoreDatabase db(read, lab.options.identity, lab.options.producers, true);
    const auto page = db.states("p0", "", 64);
    require(page.size() == 64, "durable baseline missing first page");
    const auto tail = db.states("p0", page.back().state.stateKey, 64);
    require(tail.size() == 2, "durable state count omitted baseline beyond 64 plus original state");
}

void baselineFragmentLimitAndDeclaration() {
    Lab lab;
    AsyncEventStoreProducer producer(asyncOptions(lab));
    std::vector<MqttEventOutbox::EventState> restored;
    until([&] { return producer.takeBaseline(restored); });
    require(!producer.trySubmitBaseline({state(0)}), "nonempty baseline accepted without size declaration");
    producer.beginBaselineUpdate(65);
    std::vector<MqttEventOutbox::EventState> oversized;
    for (int i = 0; i < 65; ++i) {
        auto item = state(0); item.stateKey = "limit:" + std::to_string(i);
        oversized.push_back(item);
    }
    require(!producer.trySubmitBaseline(oversized), "65-state single frame bypassed the 64-state batch limit");
    require(!producer.resumeAfterBaseline() && producer.status().accepted == 0 &&
        producer.status().phase == EventCommitPhase::Rebase && !producer.status().error.empty(),
        "invalid oversized fragment consumed the baseline declaration or fenced recovery");
    require(lab.proxy.exchanges(kAppend).empty(), "invalid baseline reached IPC");
}

void invalidBaselineObservable() {
    Lab lab;
    AsyncEventStoreProducer producer(asyncOptions(lab));
    std::vector<MqttEventOutbox::EventState> restored;
    until([&] { return producer.takeBaseline(restored); });
    producer.beginBaselineUpdate(2);
    require(!producer.trySubmitBaseline({state(1), state(1)}), "duplicate baseline accepted");
    const auto status = producer.status();
    require(status.rejected == 1 && !status.error.empty() && status.phase == EventCommitPhase::Rebase,
        "invalid baseline must return false with observable rejection, not throw or fence");
    require(!producer.resumeAfterBaseline(), "invalid batch consumed declared state count");
    require(producer.trySubmitBaseline({state(1)}) && producer.drain(5000), "valid first fragment failed");
    require(!producer.resumeAfterBaseline(), "partial committed baseline resumed");
    require(!producer.trySubmitBaseline({state(0)}), "duplicate key across committed fragments accepted");
    auto second = state(0); second.stateKey = "baseline:second";
    auto third = state(0); third.stateKey = "baseline:third";
    require(!producer.trySubmitBaseline({second, third}), "fragment exceeded remaining declared count");
    require(producer.status().rejected == 3 && producer.status().accepted == 1,
        "duplicate/overrun rejection changed accepted baseline count");
    require(producer.trySubmitBaseline({second}) && producer.drain(5000), "valid final fragment failed");
    require(!producer.trySubmitBaseline({third}), "extra fragment accepted after all declared states committed");
    require(producer.resumeAfterBaseline() && pendingCount(lab.options) == 0,
        "valid fragments failed recovery or manufactured outbound events");
}

void statelessInvalidObservable() {
    Lab lab;
    auto options = asyncOptions(lab); options.stateless = true;
    AsyncEventStoreProducer producer(options);
    until([&] { return producer.status().phase == EventCommitPhase::Normal; });
    require(!producer.trySubmit({}, {state(1)}), "stateless producer accepted states");
    require(producer.status().rejected == 1 && !producer.status().error.empty() &&
        producer.status().phase == EventCommitPhase::Normal,
        "stateless invalid batch missing rejected/error status");
}

void validationDoesNotFenceAcceptedWork() {
    Lab lab;
    CommitGate gate;
    AsyncEventStoreProducer producer(asyncOptions(lab));
    ReleaseGate release{gate};
    rebase(producer, 0);
    lab.proxy.arm(kAppend, ReplyProxy::Fault::Rewrite, [&](const std::string& reply) {
        gate.arrive(); return reply;
    });
    require(producer.trySubmit({event(400)}, {}), "valid batch rejected");
    gate.wait();
    require(!producer.trySubmit({}, {}), "invalid batch accepted");
    const auto reason = producer.status().error;
    require(!reason.empty(), "missing validation failure");
    gate.release();
    until([&] { return producer.status().committed == 1; });
    const auto status = producer.status();
    require(status.phase == EventCommitPhase::Normal && status.rejected == 1 && producer.drain(0),
        "validation fenced accepted work or erased cumulative rejection count");
    require(producer.trySubmit({event(401)}, {}) && producer.drain(5000),
        "worker did not remain usable after rejected validation");
    require(pendingCount(lab.options) == 2 && producer.status().committed == 2,
        "validation failure skipped or duplicated accepted work");
}
}

int main() {
    int failures = 0;
    failures += !run("async-existing-outage-gap-restart", [] { require(producerHappyPath() == 0, "existing producer regression"); });
    failures += !run("async-unknown-commit-exact-retry-no-skip", lostCommitCannotSkip);
    failures += !run("async-eight-thread-concurrent-submit", concurrentSubmit);
    failures += !run("async-actor-conflict", actorConflict);
    failures += !run("async-replaced-actor-fenced-no-stealback", replacedActorDoesNotStealBack);
    failures += !run("async-invalid-batch-observable", invalidBatchObservable);
    failures += !run("async-journal-conflict-fenced", journalConflictFences);
    failures += !run("async-gap-every-baseline-commit-before-resume", queueGapRequiresEveryBaselineCommit);
    failures += !run("async-baseline-declaration-and-64-per-fragment-limit", baselineFragmentLimitAndDeclaration);
    failures += !run("async-invalid-baseline-observable", invalidBaselineObservable);
    failures += !run("async-stateless-invalid-observable", statelessInvalidObservable);
    failures += !run("async-validation-does-not-fence-accepted-work", validationDoesNotFenceAcceptedWork);
    return failures ? 1 : 0;
}
