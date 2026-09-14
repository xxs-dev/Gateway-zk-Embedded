#include "edge_gateway/event_store_producer.hpp"
#include "edge_gateway/event_store_clock.hpp"
#include <chrono>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace edge_gateway {
namespace {
using Json = json::JsonValue;
Json object(std::initializer_list<std::pair<std::string, Json>> fields) {
    Json::Object result;
    for (const auto& item : fields)
        result.values.push_back({item.first, std::make_shared<Json>(item.second)});
    return Json::makeObject(std::move(result));
}
Json text(const std::string& value) { return Json::makeString(value); }
Json number(std::int64_t value) { return text(std::to_string(value)); }

struct Batch {
    std::vector<MqttEventOutbox::EventMessage> events;
    std::vector<MqttEventOutbox::EventState> states;
    std::vector<EventStoreLocalEvent> localEvents;
    std::size_t bytes = 0;
};

std::size_t validateBatch(const Batch& batch, std::size_t frameLimit) {
    if (batch.events.size() > 64 || batch.states.size() > 64 || batch.localEvents.size() > 64 ||
        (batch.events.empty() && batch.states.empty() && batch.localEvents.empty()))
        throw std::invalid_argument("producer batch requires 1..64 events or states");
    std::size_t bytes = 4096;
    const auto add = [&](const std::string& value, std::size_t limit, bool required) {
        if ((required && value.empty()) || value.size() > limit || value.find('\0') != std::string::npos)
            throw std::invalid_argument("invalid producer text");
        bytes += 6 * value.size() + 32;
    };
    std::set<std::pair<std::string, std::string>> identities;
    for (const auto& event : batch.events) {
        add(event.eventId, 256, true); add(event.targetId, 96, true);
        add(event.eventType, 96, true); add(event.topic, 4096, true);
        add(event.payload, 256 * 1024, false);
        if (event.eventTs <= 0 || !identities.emplace(event.eventId, event.targetId).second)
            throw std::invalid_argument("invalid producer event identity/timestamp");
    }
    std::set<std::string> keys;
    for (const auto& state : batch.states) {
        add(state.stateKey, 256, true); add(state.eventType, 96, true);
        add(state.alarmType, 256, false); add(state.lifecycle, 4096, false);
        bytes += 512;
        if (!std::isfinite(state.value) || state.sourceTs < 0 || !keys.insert(state.stateKey).second)
            throw std::invalid_argument("invalid producer state");
    }
    keys.clear();
    for (const auto& local : batch.localEvents) {
        const auto& e = local.event;
        add(local.kind, 16, true); add(local.configGeneration, 96, true);
        add(e.eventId, 256, true); add(e.machineCode, 256, false);
        add(e.meterCode, 256, false); add(e.pointCode, 256, false);
        add(e.alarmType, 256, false); add(e.persistValue, 4096, false);
        bytes += 512;
        if ((local.kind != "alarm" && local.kind != "change") || local.stateVersion < 0 ||
            !keys.insert(e.eventId).second || e.ts <= 0 ||
            !std::isfinite(e.value) || !std::isfinite(e.threshold))
            throw std::invalid_argument("invalid local journal event");
    }
    if (bytes > frameLimit) throw std::length_error("producer batch exceeds bounded IPC frame");
    return bytes;
}

Json arguments(const Batch& batch, const std::map<std::string, std::int64_t>& versions) {
    Json::Array events, states, localEvents;
    for (const auto& e : batch.events) events.values.push_back(std::make_shared<Json>(object({
        {"eventId", text(e.eventId)}, {"targetId", text(e.targetId)},
        {"eventType", text(e.eventType)}, {"topic", text(e.topic)},
        {"payload", text(e.payload)}, {"eventTs", number(e.eventTs)}})));
    for (const auto& s : batch.states) {
        const auto found = versions.find(s.stateKey);
        const auto version = found == versions.end() ? 0 : found->second;
        if (version == std::numeric_limits<std::int64_t>::max())
            throw std::overflow_error("producer state version exhausted");
        states.values.push_back(std::make_shared<Json>(object({
            {"stateKey", text(s.stateKey)}, {"eventType", text(s.eventType)},
            {"index", number(s.index)}, {"alarmType", text(s.alarmType)},
            {"active", Json::makeBool(s.active)}, {"value", Json::makeNumber(s.value)},
            {"quality", number(s.quality)}, {"sourceTs", number(s.sourceTs)},
            {"lifecycle", text(s.lifecycle)}, {"expectedVersion", number(version)}})));
    }
    for (const auto& local : batch.localEvents) {
        const auto& e = local.event;
        localEvents.values.push_back(std::make_shared<Json>(object({
            {"kind", text(local.kind)}, {"configGeneration", text(local.configGeneration)},
            {"stateVersion", number(local.stateVersion)}, {"eventId", text(e.eventId)},
            {"index", number(e.index)}, {"machineCode", text(e.machineCode)},
            {"meterCode", text(e.meterCode)}, {"pointCode", text(e.pointCode)},
            {"alarmType", text(e.alarmType)}, {"active", Json::makeBool(e.active)},
            {"threshold", Json::makeNumber(e.threshold)}, {"value", Json::makeNumber(e.value)},
            {"quality", number(e.quality)}, {"ts", number(e.ts)}, {"stale", Json::makeBool(e.stale)},
            {"persistValue", text(e.persistValue)}})));
    }
    return object({{"events", Json::makeArray(std::move(events))},
        {"states", Json::makeArray(std::move(states))},
        {"localEvents", Json::makeArray(std::move(localEvents))}});
}
} // namespace

struct AsyncEventStoreProducer::Impl {
    EventStoreProducerOptions options;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<Batch> queue;
    EventCommitStatus cached;
    std::vector<MqttEventOutbox::EventState> baseline;
    bool baselineTaken = false;
    std::size_t baselineExpected = 0, baselineSubmitted = 0;
    std::set<std::string> baselineKeys;
    bool stopping = false;
    std::thread worker;

    explicit Impl(EventStoreProducerOptions value) : options(std::move(value)) {
        options.client.requireLocalJournal = true;
        if (options.client.role != EventStoreClientRole::Producer || options.queueItems == 0 ||
            options.queueItems > 4096 || options.queueBytes < options.client.maxFrameBytes ||
            options.queueBytes > 16 * 1024 * 1024 || options.retryMs < 1 || options.retryMs > 5000)
            throw std::invalid_argument("invalid async producer limits/role");
        worker = std::thread([this] { loop(); });
    }
    ~Impl() {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; wake.notify_all(); }
        worker.join();
    }
    bool delay() {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait_for(lock, std::chrono::milliseconds(options.retryMs), [&] { return stopping; });
        return stopping;
    }
    void fence(const std::string& message, bool unknown) {
        std::lock_guard<std::mutex> lock(mutex);
        cached.phase = EventCommitPhase::Fenced;
        cached.error = message;
        cached.unknown = unknown;
        wake.notify_all();
    }
    void loop() {
        try {
            EventStoreClient client(options.client);
            std::map<std::string, std::int64_t> versions;
            bool started = false;
            bool needBaseline = true;
            for (;;) {
                { std::lock_guard<std::mutex> lock(mutex); if (stopping) break; }
                try {
                    if (!started) { client.start(); started = true; }
                    if (needBaseline) {
                        const auto restored = client.loadStates();
                        std::vector<MqttEventOutbox::EventState> snapshot;
                        versions.clear();
                        for (const auto& item : restored) {
                            versions.emplace(item.state.stateKey, item.version);
                            snapshot.push_back(item.state);
                        }
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            baseline = std::move(snapshot); baselineTaken = false;
                            baselineExpected = 0; baselineSubmitted = 0; baselineKeys.clear();
                            if (options.stateless && !baseline.empty())
                                throw std::logic_error("stateless producer owns persisted point states");
                            cached.phase = options.stateless ? EventCommitPhase::Normal : EventCommitPhase::Rebase;
                            cached.error.clear(); cached.unknown = false;
                            wake.notify_all();
                        }
                        needBaseline = false;
                    }
                    Batch batch;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        wake.wait(lock, [&] { return stopping ||
                            !queue.empty() ||
                            cached.phase == EventCommitPhase::GapDrain; });
                        if (stopping) break;
                        if (queue.empty()) { needBaseline = true; continue; }
                        batch = queue.front();
                    }
                    if (client.hasPending()) client.retry();
                    else client.execute("AppendEventsAndStates", arguments(batch, versions));
                    // execute/retry verifies the committed receipt, actor, epoch and sequence.
                    for (const auto& state : batch.states) ++versions[state.stateKey];
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        cached.queueBytes -= queue.front().bytes;
                        queue.pop_front(); cached.queueItems = queue.size();
                        ++cached.committed; cached.unknown = false; cached.error.clear();
                        cached.lastCommitMonotonicMs = readEventStoreLeaseTime().milliseconds;
                        wake.notify_all();
                    }
                } catch (const EventStoreClientError& error) {
                    if (error.code() == "FENCED" || error.code() == "STALE_EPOCH" ||
                        error.code() == "CONFLICT_OR_INVALID" || error.code() == "REQUEST_CONFLICT" ||
                        error.code() == "STALE_OR_GAPPED_SEQUENCE" || error.code() == "REGISTER_CONFLICT" ||
                        error.code() == "JOURNAL_CONFLICT" || error.code() == "READ_LIMIT") {
                        fence(error.what(), error.outcomeUnknown()); return;
                    }
                    { std::lock_guard<std::mutex> lock(mutex);
                      cached.unknown = error.outcomeUnknown(); cached.error = error.what(); }
                    if (delay()) break;
                }
            }
            std::lock_guard<std::mutex> lock(mutex);
            cached.phase = EventCommitPhase::Stopped;
        } catch (const std::exception& error) {
            bool unknown;
            { std::lock_guard<std::mutex> lock(mutex); unknown = cached.unknown; }
            fence(error.what(), unknown);
        }
    }
};

AsyncEventStoreProducer::AsyncEventStoreProducer(EventStoreProducerOptions options)
    : impl_(new Impl(std::move(options))) {}
AsyncEventStoreProducer::~AsyncEventStoreProducer() = default;

bool AsyncEventStoreProducer::trySubmit(const std::vector<MqttEventOutbox::EventMessage>& events,
    const std::vector<MqttEventOutbox::EventState>& states,
    const std::vector<EventStoreLocalEvent>& localEvents) {
    Batch batch{events, states, localEvents, 0};
    try {
        if (impl_->options.stateless && (!states.empty() || !localEvents.empty()))
            throw std::invalid_argument("stateless management producer cannot submit point states/history");
        batch.bytes = validateBatch(batch, impl_->options.client.maxFrameBytes);
    }
    catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        ++impl_->cached.rejected;
        impl_->cached.error = error.what();
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto& s = impl_->cached;
    if (s.phase != EventCommitPhase::Normal || impl_->stopping) { ++s.rejected; return false; }
    if (impl_->queue.size() >= impl_->options.queueItems ||
        batch.bytes > impl_->options.queueBytes - s.queueBytes) {
        s.phase = EventCommitPhase::GapDrain; ++s.gaps; ++s.rejected;
        s.error = "bounded producer queue full; drain before baseline";
        impl_->wake.notify_all(); return false;
    }
    impl_->queue.push_back(std::move(batch));
    s.queueBytes += impl_->queue.back().bytes; s.queueItems = impl_->queue.size(); ++s.accepted;
    impl_->wake.notify_all(); return true;
}
bool AsyncEventStoreProducer::takeBaseline(std::vector<MqttEventOutbox::EventState>& states) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->cached.phase != EventCommitPhase::Rebase || impl_->baselineTaken) return false;
    states = impl_->baseline; impl_->baselineTaken = true; return true;
}
void AsyncEventStoreProducer::beginBaselineUpdate(std::size_t stateCount) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->cached.phase != EventCommitPhase::Rebase || !impl_->baselineTaken ||
        impl_->baselineSubmitted != 0 || !impl_->queue.empty() || stateCount > 65536)
        throw std::logic_error("invalid baseline update declaration");
    impl_->baselineExpected = stateCount;
}
bool AsyncEventStoreProducer::trySubmitBaseline(const std::vector<MqttEventOutbox::EventState>& states) {
    Batch batch{{}, states, {}, 0};
    try { batch.bytes = validateBatch(batch, impl_->options.client.maxFrameBytes); }
    catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        ++impl_->cached.rejected; impl_->cached.error = error.what(); return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto& s = impl_->cached;
    if (s.phase != EventCommitPhase::Rebase || !impl_->baselineTaken || impl_->stopping)
        return false;
    if (states.size() > impl_->baselineExpected - impl_->baselineSubmitted ||
        std::any_of(states.begin(), states.end(), [&](const MqttEventOutbox::EventState& state) {
            return impl_->baselineKeys.count(state.stateKey) != 0;
        })) {
        ++s.rejected; s.error = "baseline exceeds declared size or repeats a state key"; return false;
    }
    if (impl_->queue.size() >= impl_->options.queueItems ||
        batch.bytes > impl_->options.queueBytes - s.queueBytes) return false;
    impl_->queue.push_back(std::move(batch));
    impl_->baselineSubmitted += states.size();
    for (const auto& state : states) impl_->baselineKeys.insert(state.stateKey);
    s.queueBytes += impl_->queue.back().bytes; s.queueItems = impl_->queue.size(); ++s.accepted;
    impl_->wake.notify_all(); return true;
}
bool AsyncEventStoreProducer::resumeAfterBaseline() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->cached.phase != EventCommitPhase::Rebase || !impl_->baselineTaken)
        throw std::logic_error("producer baseline must be applied before resuming");
    if (!impl_->queue.empty() || impl_->cached.unknown ||
        impl_->baselineSubmitted != impl_->baselineExpected) return false;
    impl_->baseline.clear(); impl_->cached.phase = EventCommitPhase::Normal;
    impl_->wake.notify_all(); return true;
}
EventCommitStatus AsyncEventStoreProducer::status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->cached;
}
bool AsyncEventStoreProducer::drain(int timeoutMs) {
    if (timeoutMs < 0) throw std::invalid_argument("negative producer drain timeout");
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->wake.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        return impl_->queue.empty() || impl_->cached.phase == EventCommitPhase::Fenced;
    });
    return impl_->queue.empty() && !impl_->cached.unknown &&
        impl_->cached.phase != EventCommitPhase::Fenced;
}
} // namespace edge_gateway
