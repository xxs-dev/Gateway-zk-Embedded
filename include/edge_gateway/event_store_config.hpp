#pragma once

#include "edge_gateway/models.hpp"
#include "edge_gateway/event_store_producer.hpp"
#include "edge_gateway/event_stats_reader.hpp"
#include "edge_gateway/event_store_replay_factory.hpp"
#include "edge_gateway/builtin_mqtt_driver_publisher.hpp"

namespace edge_gateway {

inline EventStoreClientOptions eventStoreClientOptions(const EventStoreConnectionConfig& config,
    const std::string& actor, EventStoreClientRole role) {
    if (config.backend != "ipc-lab") throw std::invalid_argument("IPC backend must be explicitly selected");
    EventStoreClientOptions options;
    options.identity = {config.storeId, config.configGeneration};
    options.socketPath = config.socketPath; options.actorId = actor;
    options.role = role; options.timeoutMs = config.timeoutMs;
    options.requireLocalJournal = true;
    return options;
}

inline EventStoreProducerOptions eventStoreProducerOptions(const EventStoreConnectionConfig& config,
    const std::string& actor) {
    EventStoreProducerOptions options;
    options.client = eventStoreClientOptions(config, actor, EventStoreClientRole::Producer);
    options.queueItems = config.queueItems; options.queueBytes = config.queueBytes;
    return options;
}

inline std::unique_ptr<IEventStatsSource> makeIpcMqttEventStatsSource(
    const EventStoreConnectionConfig& config, std::vector<EventStatsQuery> queries) {
    IpcEventStatsOptions reader;
    reader.identity = {config.storeId, config.configGeneration}; reader.socketPath = config.socketPath;
    reader.timeoutMs = config.timeoutMs;
    reader.profile = config.storageProfile == "wal-full" ? MqttEventOutbox::StorageProfile::WalFull :
        MqttEventOutbox::StorageProfile::DeleteFull;
    EventStatsCacheOptions options;
    options.reader = EventStatsReaderOptions(reader); options.queries = std::move(queries);
    auto cache = std::unique_ptr<EventStatsCache>(new EventStatsCache(std::move(options)));
    cache->start();
    return std::unique_ptr<IEventStatsSource>(cache.release());
}

inline MqttEventReplayFactory configuredEventReplay(const EventStoreConnectionConfig& config,
    const std::string& actor, MqttEventReplayLane lane, const std::string& target,
    std::vector<std::string> types, std::shared_ptr<BuiltinMqttDriverPublisher> publisher) {
    if (!publisher) throw std::invalid_argument("leased MQTT transport is required");
    EventStoreReplayFactoryOptions options;
    options.client = eventStoreClientOptions(config, actor, EventStoreClientRole::Sender);
    options.lane = lane; options.targetId = target; options.eventTypes = std::move(types);
    options.sendBatch = [publisher](const auto& events, const EventStoreSenderCall& call,
        const auto& canSend, const auto& onAttempt, const auto& onConfirmed) {
        std::vector<MqttJsonMessage> messages;
        messages.reserve(events.size());
        for (const auto& event : events) messages.push_back({event.topic, event.payload});
        publisher->publishLeasedEvents(messages, call.bootId, call.deadlineMs,
            canSend, onAttempt, onConfirmed);
    };
    return makeEventStoreReplayFactory(std::move(options));
}

} // namespace edge_gateway
