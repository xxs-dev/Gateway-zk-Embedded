#pragma once

#include "edge_gateway/event_store.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace edge_gateway {

enum class EventCommitPhase { Starting, Normal, GapDrain, Rebase, Fenced, Stopped };

struct EventCommitStatus {
    EventCommitPhase phase = EventCommitPhase::Starting;
    std::size_t queueItems = 0;
    std::size_t queueBytes = 0;
    std::uint64_t accepted = 0;
    std::uint64_t committed = 0;
    std::uint64_t rejected = 0;
    std::uint64_t gaps = 0;
    std::uint64_t rejectedInputSamples = 0;
    std::uint64_t pendingInputSamples = 0;
    bool unknown = false;
    std::int64_t lastCommitMonotonicMs = 0;
    std::string error;
};
std::string eventCommitStatusJson(const EventCommitStatus& status);

// Detection owns candidate lifecycle transitions; only accepted batches advance them.
// Same-state observation timestamps are a volatile stale-sample guard, not commits.
// A baseline is published only after all previously accepted work is reconciled.
class IEventCommitSink {
public:
    virtual ~IEventCommitSink() = default;
    virtual bool trySubmit(const std::vector<MqttEventOutbox::EventMessage>& events,
        const std::vector<MqttEventOutbox::EventState>& states,
        const std::vector<EventStoreLocalEvent>& localEvents = {}) = 0;
    virtual bool takeBaseline(std::vector<MqttEventOutbox::EventState>& states) = 0;
    virtual void beginBaselineUpdate(std::size_t stateCount) = 0;
    virtual bool trySubmitBaseline(const std::vector<MqttEventOutbox::EventState>& states) = 0;
    virtual bool resumeAfterBaseline() = 0;
    virtual EventCommitStatus status() const = 0;
};

} // namespace edge_gateway
