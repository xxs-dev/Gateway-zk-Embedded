#pragma once

#include "edge_gateway/event_commit_sink.hpp"
#include "edge_gateway/event_store_client.hpp"
#include <memory>

namespace edge_gateway {

struct EventStoreProducerOptions {
    EventStoreClientOptions client;
    std::size_t queueItems = 128;
    std::size_t queueBytes = 1024 * 1024;
    int retryMs = 100;
    // Management producers have no point-state baseline to apply.
    bool stateless = false;
};

// The actor and all IPC live on the worker. Detection never waits for SQL/IPC.
class AsyncEventStoreProducer final : public IEventCommitSink {
public:
    explicit AsyncEventStoreProducer(EventStoreProducerOptions options);
    ~AsyncEventStoreProducer() override;
    bool trySubmit(const std::vector<MqttEventOutbox::EventMessage>& events,
        const std::vector<MqttEventOutbox::EventState>& states,
        const std::vector<EventStoreLocalEvent>& localEvents = {}) override;
    bool takeBaseline(std::vector<MqttEventOutbox::EventState>& states) override;
    void beginBaselineUpdate(std::size_t stateCount) override;
    bool trySubmitBaseline(const std::vector<MqttEventOutbox::EventState>& states) override;
    bool resumeAfterBaseline() override;
    EventCommitStatus status() const override;
    // Returns false when accepted work remains; stopping never reports it committed.
    bool drain(int timeoutMs);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace edge_gateway
