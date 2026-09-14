// Reuse the existing real-runtime/Unix-socket fault fixture without changing it.
// Compile this translation unit only, not event_store_client_test.cpp separately.
#define main event_store_client_suite_main
#include "event_store_client_test.cpp"
#undef main
#include "edge_gateway/event_store_sender.hpp"
#include "edge_gateway/event_store_clock.hpp"
#include "edge_gateway/event_store_replay_factory.hpp"

namespace {
using Pump = edge_gateway::EventStoreSender;
using PumpOptions = edge_gateway::EventStoreSenderOptions;
using PumpResult = edge_gateway::EventStoreSenderResult;
void seed(Lab& lab) {
    EventStoreClient producer(lab.client());
    producer.start();
    producer.execute(kAppend, object({{"events", array({event("one"), event("two"), event("three")})},
        {"states", array({})}}));
}
PumpOptions pumpOptions(Lab& lab, int& calls, bool& allowed) {
    PumpOptions options;
    options.client = lab.client(Role::Sender);
    options.client.expectedSenderTargetId = "main";
    options.client.expectedSenderEventTypes = {"alarm", "change"};
    options.authorized = [&] { return allowed; };
    options.send = [&](const edge_gateway::EventStoreSenderMessage& message,
                       const edge_gateway::EventStoreSenderCall& context) {
        require(!message.eventId.empty() && !message.topic.empty() && context.epoch == 1 &&
            context.claimSequence == 1 && !context.claimToken.empty() && context.timeoutMs == 3000 &&
            context.deadlineMs < context.leaseUntilMs, "callback lacks identity/deadline");
        ++calls;
        return true;
    };
    return options;
}
std::size_t finishCount(Lab& lab, const char* op) {
    const auto exchanges = lab.proxy.exchanges(op);
    require(!exchanges.empty(), "missing finish operation");
    return field(field(parse(exchanges.back().request), "args"), "items").asArray().values.size();
}
void lostFinish(Profile profile, bool release) {
    Lab lab(profile);
    seed(lab);
    int calls = 0;
    bool allowed = true;
    auto options = pumpOptions(lab, calls, allowed);
    bool clockWorks = true;
    options.leaseClock = [&] {
        if (!clockWorks) throw std::runtime_error("clock unavailable");
        return edge_gateway::readEventStoreLeaseTime();
    };
    if (release) options.send = [&](const auto&, const auto&) { return ++calls == 1; };
    Pump pump(options);
    const auto* op = release ? "ReleaseBatch" : "AckBatch";
    lab.proxy.arm(op, ReplyProxy::Fault::Truncate);
    require(clientError([&] { pump.runOnce(); }).outcomeUnknown(), "lost finish was not unknown");
    const int sentCalls = calls;
    const auto bytes = lab.proxy.exchanges(op).at(0).request;
    allowed = false;
    clockWorks = false;
    lab.restart();
    lab.proxy.arm(op, ReplyProxy::Fault::Reject);
    require(clientError([&] { pump.runOnce(); }).outcomeUnknown(), "rejection erased uncertainty");
    require(pump.runOnce() == PumpResult::Completed && calls == sentCalls, "finish retried network");
    exactReplay(lab.proxy, op, bytes, 3);
    require(pump.runOnce() == PumpResult::Unauthorized, "unauthorized claim");
    require(lab.proxy.exchanges("ClaimBatch").size() == 1, "extra claim during drain");
    require(finishCount(lab, "AckBatch") == (release ? 1u : 3u), "ACK subset wrong");
    if (release) require(finishCount(lab, "ReleaseBatch") == 2, "release subset wrong");
    lab.proxy.checked();
}
void lostClaim(bool revoke, bool expire) {
    Lab lab;
    seed(lab);
    int calls = 0;
    bool allowed = true;
    auto options = pumpOptions(lab, calls, allowed);
    if (expire) options.leaseClock = [] {
        auto now = edge_gateway::readEventStoreLeaseTime();
        now.milliseconds += 60000;
        return now;
    };
    Pump pump(options);
    lab.proxy.arm("ClaimBatch", ReplyProxy::Fault::Truncate);
    require(clientError([&] { pump.runOnce(); }).outcomeUnknown() && calls == 0, "unknown claim sent");
    const auto bytes = lab.proxy.exchanges("ClaimBatch").at(0).request;
    allowed = !revoke;
    require(pump.runOnce() == PumpResult::Completed, "claim did not reconcile");
    exactReplay(lab.proxy, "ClaimBatch", bytes);
    require(calls == ((revoke || expire) ? 0 : 3), "recovered claim violated gate");
    if (revoke || expire) require(finishCount(lab, "ReleaseBatch") == 3, "denied claim not released");
}
void interruptedBatch(int mode) {
    Lab lab;
    seed(lab);
    int calls = 0;
    bool allowed = true;
    auto options = pumpOptions(lab, calls, allowed);
    auto now = edge_gateway::readEventStoreLeaseTime();
    options.leaseClock = [&] { return now; };
    options.send = [&](const auto&, const auto& context) {
        ++calls;
        if (mode == 0 && calls == 2) return false;
        if (mode == 1 && calls == 2) throw std::runtime_error("network failure");
        if (mode == 2) allowed = false;
        if (mode == 3) now.bootId = "different-boot";
        if (mode == 4) now.milliseconds = context.deadlineMs;
        if (mode == 5) now.milliseconds = context.leaseUntilMs - 3000;
        return true;
    };
    Pump pump(options);
    require(pump.runOnce() == PumpResult::Completed, "interrupted batch not drained");
    require(calls == (mode < 2 ? 2 : 1), "network continued after failure/gate loss");
    require(finishCount(lab, "AckBatch") == 1 && finishCount(lab, "ReleaseBatch") == 2,
        "partial ACK/release partition wrong");
    const auto ack = field(field(parse(lab.proxy.exchanges("AckBatch").at(0).request), "args"), "items");
    const auto released = field(field(parse(lab.proxy.exchanges("ReleaseBatch").at(0).request), "args"), "items");
    require(stringField(*ack.asArray().values[0], "eventId") == "one" &&
        stringField(*released.asArray().values[0], "eventId") == "two", "partition identities changed");
}
void freshDrainDoesNotRegister() {
    Lab lab;
    seed(lab);
    int calls = 0;
    bool allowed = true;
    Pump pump(pumpOptions(lab, calls, allowed));
    const auto before = lab.proxy.exchanges().size();
    const auto result = pump.runOnceDetailed(true);
    require(result.result == PumpResult::Unauthorized && !result.pending && !result.healthy &&
        result.attemptedBytes == 0 && result.ackedCount == 0 && calls == 0,
        "fresh drain with authorization started work");
    require(lab.proxy.exchanges().size() == before && lab.proxy.exchanges("RegisterSender").empty() &&
        lab.proxy.exchanges("ClaimBatch").empty(), "fresh drain performed registration/Claim/IPC");
}

void ownerAndRegistration() {
    Lab lab;
    seed(lab);
    int calls = 0;
    bool allowed = false;
    auto options = pumpOptions(lab, calls, allowed);
    Pump* current = nullptr;
    options.send = [&](const auto&, const auto&) {
        ++calls;
        rejects([&] { current->runOnce(); });
        return true;
    };
    Pump pump(options);
    current = &pump;
    require(pump.runOnce() == PumpResult::Unauthorized && lab.proxy.exchanges("RegisterSender").empty(),
        "unauthorized registration");
    allowed = true;
    lab.proxy.arm("RegisterSender", ReplyProxy::Fault::Truncate);
    require(clientError([&] { pump.runOnce(); }).outcomeUnknown(), "registration not unknown");
    allowed = false;
    require(pump.runOnce() == PumpResult::Unauthorized, "registration drain started claim");
    const auto attempts = lab.proxy.exchanges("RegisterSender");
    require(attempts.size() == 2 && attempts[0].request == attempts[1].request, "registration changed session");
    std::exception_ptr failure;
    std::thread wrong([&] { try { pump.runOnce(); } catch (...) { failure = std::current_exception(); } });
    wrong.join();
    require(static_cast<bool>(failure), "cross-thread pump accepted");
    const auto child = fork();
    require(child >= 0, "ownership test fork failed");
    if (child == 0) {
        try { pump.runOnce(); _exit(1); } catch (const std::logic_error&) { _exit(0); }
        catch (...) { _exit(2); }
    }
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "cross-fork pump accepted");
    allowed = true;
    require(pump.runOnce() == PumpResult::Completed && calls == 3, "reentrancy damaged batch");
    require(pump.runOnce() == PumpResult::Empty && calls == 3, "empty claim sent");
}
void failClosed(int mode) {
    Lab lab;
    seed(lab);
    int calls = 0;
    bool allowed = true;
    auto options = pumpOptions(lab, calls, allowed);
    if (mode == 0) options.leaseClock = []() -> edge_gateway::EventStoreLeaseTime {
        throw std::runtime_error("CLOCK_BOOTTIME unavailable");
    };
    if (mode == 1) options.authorized = []() -> bool { throw std::runtime_error("authority unavailable"); };
    if (mode == 2) {
        options.leaseClock = [] {
            auto now = edge_gateway::readEventStoreLeaseTime();
            now.bootId = "foreign-boot";
            return now;
        };
    }
    Pump pump(options);
    require(pump.runOnce() == (mode == 1 ? PumpResult::Unauthorized : PumpResult::Completed) && calls == 0,
        "clock/authority failure permitted network");
    if (mode != 1) require(finishCount(lab, "ReleaseBatch") == 3, "clock failure abandoned batch");
}

void factoryAdapter(int mode) {
    Lab lab(mode == 1 ? Profile::WalFull : Profile::DeleteFull);
    seed(lab);
    int calls = 0;
    bool allowed = true;
    EventStoreReplayFactoryOptions options;
    options.client = lab.client(Role::Sender);
    options.lane = MqttEventReplayLane::MainBusiness;
    options.targetId = "main";
    options.eventTypes = {"alarm", "change"};
    options.send = [&](const auto&, const auto&) {
        ++calls;
        if (mode == 2 || mode == 4) return calls == 1;
        if (mode == 3) std::this_thread::sleep_for(std::chrono::milliseconds(130));
        return true;
    };
    if (mode == 3) { options.leaseMs = 100; options.networkTimeoutMs = 10; options.completionReserveMs = 1; }
    auto factory = makeEventStoreReplayFactory(options);
    { EventStoreClient probe(lab.client(Role::Sender)); } // Closure construction must not take actor lock.
    MqttEventReplayRequest request;
    request.lane = options.lane;
    request.targetId = "main";
    request.includeTypes = {"change", "alarm"};
    request.maxBytes = 32768;
    request.authorized = [&] { return allowed; };
    auto wrong = request;
    wrong.includeTypes = {"change"};
    rejects([&] { factory(wrong); });
    wrong = request;
    wrong.targetId = "third-party";
    rejects([&] { factory(wrong); });
    require(lab.proxy.exchanges("RegisterSender").empty(), "invalid factory request registered sender");
    auto replay = factory(request);
    rejects([&] { EventStoreClient competing(lab.client(Role::Sender)); });
    if (mode == 0 || mode == 1) lab.proxy.arm("AckBatch", ReplyProxy::Fault::Truncate);
    if (mode == 4) lab.proxy.arm("ReleaseBatch", ReplyProxy::Fault::Truncate);
    if (mode == 5) lab.proxy.arm("ClaimBatch", ReplyProxy::Fault::Truncate);
    if (mode == 6 || mode == 7) lab.proxy.arm("AckBatch", ReplyProxy::Fault::Rewrite, [mode](const std::string& reply) {
        const auto root = parse(reply);
        auto rows = field(root, "results").asArray();
        if (mode == 6) rows.values.pop_back();
        else rows.values.push_back(rows.values.front());
        return encode(replaceField(root, "results", Json::makeArray(std::move(rows))));
    });
    const auto first = replay->runOnce(false);
    if (mode == 0 || mode == 1 || mode == 4 || mode == 5 || mode == 6 || mode == 7) {
        require(first.pending && !first.healthy, "unknown IPC mutation lost pending state");
        require(first.ackedCount == (mode == 4 ? 1u : 0u), "unknown ACK fabricated committed count");
        require((first.attemptedBytes > 0) == (mode != 5), "attempted bytes not retained after IPC failure");
        const int previousCalls = calls;
        allowed = false;
        lab.restart();
        auto done = replay->runOnce(true);
        require(!done.pending && !done.healthy && done.attemptedBytes == 0 && calls == previousCalls,
            "drain repeated network or retained settled mutation");
        require(done.ackedCount == ((mode == 0 || mode == 1 || mode == 6 || mode == 7) ? 3u : 0u), "ACK reconciliation delta incorrect");
    } else if (mode == 2) {
        require(!first.pending && !first.healthy && first.ackedCount == 1 && first.changeCount == 1 && calls == 2,
            "partial delivery statistics incorrect");
    } else {
        require(!first.pending && !first.healthy && first.ackedCount == 0 && calls == 1,
            "expired ACK counted as effective delivery");
    }
    const auto claimCount = lab.proxy.exchanges("ClaimBatch").size();
    rejects([&] { replay->runOnce(false); });
    require(lab.proxy.exchanges("ClaimBatch").size() == claimCount, "single-batch adapter started another claim");
    replay.reset();
    { EventStoreClient released(lab.client(Role::Sender)); }
    lab.proxy.checked();
}
void scopeContract(Profile profile, int mode) {
    Lab lab(profile);
    seed(lab);
    int calls = 0;
    bool allowed = true;
    EventStoreReplayFactoryOptions options;
    options.client = lab.client(Role::Sender);
    options.lane = MqttEventReplayLane::ForwardEvents;
    options.targetId = mode == 0 ? "third-party" : "main";
    options.eventTypes = mode == 1 ? std::vector<std::string>{"alarm"} :
        mode == 2 ? std::vector<std::string>{"ota_status"} : std::vector<std::string>{"alarm", "change"};
    if (mode == 2) options.lane = MqttEventReplayLane::MainManagement;
    options.send = [&](const auto&, const auto&) { ++calls; return true; };
    auto factory = makeEventStoreReplayFactory(options);
    MqttEventReplayRequest request;
    request.lane = options.lane; request.targetId = options.targetId; request.includeTypes = options.eventTypes;
    request.maxBytes = 32768; request.authorized = [&] { return allowed; };
    if (mode >= 3 && mode <= 6) lab.proxy.arm("GetSenderScope", ReplyProxy::Fault::Rewrite, [mode](const std::string& reply) {
        auto value = parse(reply);
        if (mode == 3) return encode(object({{"ok", Json::makeBool(true)}})); // Old/malformed server.
        if (mode == 4) return encode(replaceField(value, "eventTypes", array({text("alarm"), text("change"), text("alarm")})));
        if (mode == 5) return encode(replaceField(value, "storeId", text("wrong-store")));
        return encode(replaceField(value, "scopeVersion", text("2")));
    });
    if (mode == 8) lab.proxy.arm("RegisterSender", ReplyProxy::Fault::Truncate);
    if (mode == 9) lab.proxy.arm("GetSenderScope", ReplyProxy::Fault::Truncate);
    MqttEventReplaySession session;
    const auto first = session.run(factory, request, false);
    if (mode == 7) {
        require(!first.pending && first.healthy && first.ackedCount == 3 && calls == 3,
            "equal scope with reordered type set did not publish");
    } else if (mode == 8) {
        require(first.pending && session.active() && !first.healthy && calls == 0,
            "unknown registration lost ownership");
        allowed = false;
        const auto drained = session.run(factory, request, true);
        require(!drained.pending && !session.active() && calls == 0 && lab.proxy.exchanges("ClaimBatch").empty(),
            "registration drain started new Claim/network");
        require(lab.proxy.exchanges("RegisterSender").size() == 2, "registration was not exactly replayed");
        const auto registrations = lab.proxy.exchanges("RegisterSender");
        require(registrations[0].request == registrations[1].request, "registration retry bytes changed");
    } else {
        require(!first.pending && !first.healthy && !first.error.empty() && calls == 0 && !session.active(),
            "scope failure published or retained idle actor");
        require(lab.proxy.exchanges("RegisterSender").empty() && lab.proxy.exchanges("ClaimBatch").empty(),
            "scope failure mutated actor/claimed rows before validation");
        require(pendingCount(lab.options) == 3, "scope failure changed pending events");
    }
    require(lab.proxy.exchanges("GetSenderScope").size() == 1, "scope was not read exactly once before registration");
    { EventStoreClient released(lab.client(Role::Sender)); }
    lab.proxy.checked();
}
void batchIdentityAndRetry(Profile profile, int mode) {
    Lab lab(profile);
    {
        EventStoreClient producer(lab.client());
        producer.start();
        producer.execute(kAppend, object({{"events", array({replaceField(event("one"), "eventType", text("alarm")),
            replaceField(event("two"), "eventType", text("alarm")), event("three")})}, {"states", array({})}}));
    }
    int legacyCalls = 0, batchCalls = 0;
    bool allowed = true;
    auto options = pumpOptions(lab, legacyCalls, allowed);
    auto clock = edge_gateway::readEventStoreLeaseTime();
    options.leaseClock = [&] { return clock; };
    options.sendBatch = [&](const auto& messages, const auto& call, const auto& canSend,
        const auto& attempt, const auto& confirm) {
        ++batchCalls;
        require(messages.size() == 3 && call.deadlineMs == clock.milliseconds + 3000,
            "batch lacks one shared deadline");
        if (mode == 4) { confirm(1); return; } // Never attempted.
        if (mode == 5) { attempt(9); return; } // Invalid index.
        for (std::size_t i = 0; i < messages.size(); ++i) attempt(i);
        confirm(1); // Sparse first confirmation must map to alarm/two.
        if (mode == 0 || mode == 1 || mode == 2) {
            confirm(2);
            throw std::runtime_error("transport failed after non-prefix confirms");
        }
        if (mode == 3) { confirm(1); return; } // Duplicate.
        if (mode == 6) {
            allowed = false;
            require(!canSend(), "authority loss accepted");
            allowed = true;
            require(!canSend(), "authority loss not sticky");
            confirm(2); // Existing ACK may drain after denial.
        }
        if (mode == 7 || mode == 8 || mode == 9) {
            if (mode == 7) clock.milliseconds = call.deadlineMs;
            if (mode == 8) clock.milliseconds = call.leaseUntilMs;
            if (mode == 9) clock.bootId = "changed-boot";
            require(!canSend(), "expired/foreign boot permitted network");
            confirm(2); // Late ACK must not survive.
        }
    };
    Pump pump(options);
    const char* faultOp = mode == 2 ? "ReleaseBatch" : "AckBatch";
    if (mode == 1 || mode == 2) lab.proxy.arm(faultOp, ReplyProxy::Fault::Truncate);
    auto result = pump.runOnceDetailed();
    const auto firstAcked = result.ackedCount;
    if (mode == 1 || mode == 2) {
        require(result.pending && !result.healthy, "lost sparse finish not pending");
        const auto request = lab.proxy.exchanges(faultOp).at(0).request;
        allowed = false;
        lab.restart();
        result = pump.runOnceDetailed(true);
        exactReplay(lab.proxy, faultOp, request);
        require(firstAcked + result.ackedCount == 2 && !result.pending, "sparse retry stats duplicated/lost");
    }
    require(batchCalls == 1 && legacyCalls == 0 && !result.pending && !result.healthy,
        "batch fallback/retry reentered network or hid partial failure");
    const std::size_t expected = mode == 4 || mode == 5 ? 0 : mode <= 2 || mode == 6 ? 2 : 1;
    if (expected) {
        require(finishCount(lab, "AckBatch") == expected, "sparse ACK count wrong");
        const auto ack = field(field(parse(lab.proxy.exchanges("AckBatch").back().request), "args"), "items");
        require(stringField(*ack.asArray().values[0], "eventId") == "two", "ACK used prefix identity");
        if (expected == 2) require(stringField(*ack.asArray().values[1], "eventId") == "three", "ACK lost hole");
        if (mode != 1 && mode != 2)
            require(result.alarmCount == 1 && result.changeCount == expected - 1, "ACK statistics used prefix type");
    } else require(lab.proxy.exchanges("AckBatch").empty(), "invalid observer confirmed event");
    require(finishCount(lab, "ReleaseBatch") == 3 - expected, "release complement wrong");
    const auto released = field(field(parse(lab.proxy.exchanges("ReleaseBatch").back().request), "args"), "items");
    require(stringField(*released.asArray().values[0], "eventId") == "one", "release lost unconfirmed prefix");
    lab.proxy.checked();
}

void batchFactory() {
    Lab lab;
    seed(lab);
    EventStoreReplayFactoryOptions options;
    options.client = lab.client(Role::Sender);
    options.targetId = "main";
    options.eventTypes = {"change", "alarm"};
    int calls = 0;
    options.sendBatch = [&](const auto& messages, const auto&, const auto& canSend,
        const auto& attempt, const auto& confirm) {
        ++calls;
        require(canSend(), "factory denied fresh batch");
        for (std::size_t i = 0; i < messages.size(); ++i) attempt(i);
        confirm(2); confirm(0); confirm(1);
    };
    auto factory = makeEventStoreReplayFactory(options);
    MqttEventReplayRequest request;
    request.targetId = "main"; request.includeTypes = options.eventTypes;
    request.maxBytes = 32768; request.authorized = [] { return true; };
    auto replay = factory(request);
    const auto result = replay->runOnce(false);
    require(calls == 1 && result.healthy && !result.pending && result.ackedCount == 3,
        "batch-only factory failed or lost reversed confirms");
}

void batchStorageExpiry(Profile profile) {
    Lab lab(profile);
    seed(lab);
    int calls = 0;
    bool allowed = true;
    auto options = pumpOptions(lab, calls, allowed);
    options.leaseMs = 100; options.networkTimeoutMs = 10; options.completionReserveMs = 1;
    options.sendBatch = [&](const auto&, const auto&, const auto&, const auto& attempt, const auto& confirm) {
        ++calls;
        attempt(2); confirm(2);
        std::this_thread::sleep_for(std::chrono::milliseconds(130));
    };
    Pump pump(options);
    const auto result = pump.runOnceDetailed();
    require(!result.pending && !result.healthy && result.ackedCount == 0 && calls == 1,
        "expired sparse storage ACK counted as sent");
    require(finishCount(lab, "AckBatch") == 1 && finishCount(lab, "ReleaseBatch") == 2 &&
        pendingCount(lab.options) == 3, "expired ACK lost event or released failed ACK identity");
}
} // namespace

int main() {
    if (const auto* library = std::getenv("SQLITE_LIBRARY")) testLibrary = library;
    int failed = 0;
    for (int mode = 0; mode < 10; ++mode) {
        failed += !run(("sender-batch-delete-" + std::to_string(mode)).c_str(), [=] { batchIdentityAndRetry(Profile::DeleteFull, mode); });
        failed += !run(("sender-batch-wal-" + std::to_string(mode)).c_str(), [=] { batchIdentityAndRetry(Profile::WalFull, mode); });
    }
    failed += !run("sender-batch-only-factory", batchFactory);
    failed += !run("sender-batch-storage-expiry-delete", [] { batchStorageExpiry(Profile::DeleteFull); });
    failed += !run("sender-batch-storage-expiry-wal", [] { batchStorageExpiry(Profile::WalFull); });
    failed += !run("sender-ack-unknown-delete", [] { lostFinish(Profile::DeleteFull, false); });
    failed += !run("sender-ack-unknown-wal", [] { lostFinish(Profile::WalFull, false); });
    failed += !run("sender-release-unknown", [] { lostFinish(Profile::DeleteFull, true); });
    failed += !run("sender-claim-unknown", [] { lostClaim(false, false); });
    failed += !run("sender-claim-unknown-denied-drain", [] { lostClaim(true, false); });
    failed += !run("sender-claim-unknown-expired-budget", [] { lostClaim(false, true); });
    for (int mode = 0; mode < 6; ++mode)
        failed += !run(("sender-partial-gate-" + std::to_string(mode)).c_str(), [=] { interruptedBatch(mode); });
    failed += !run("sender-owner-registration-empty-reentrancy", ownerAndRegistration);
    failed += !run("sender-fresh-authorized-drain-no-registration", freshDrainDoesNotRegister);
    for (int mode = 0; mode < 3; ++mode)
        failed += !run(("sender-fail-closed-" + std::to_string(mode)).c_str(), [=] { failClosed(mode); });
    for (int mode = 0; mode < 8; ++mode)
        failed += !run(("sender-factory-real-ipc-" + std::to_string(mode)).c_str(), [=] { factoryAdapter(mode); });
    for (int mode = 0; mode < 10; ++mode) {
        failed += !run(("sender-scope-delete-" + std::to_string(mode)).c_str(), [=] { scopeContract(Profile::DeleteFull, mode); });
        failed += !run(("sender-scope-wal-" + std::to_string(mode)).c_str(), [=] { scopeContract(Profile::WalFull, mode); });
    }
    std::cout << "SUMMARY sender groups=68 failed=" << failed << std::endl;
    return failed ? 1 : 0;
}
