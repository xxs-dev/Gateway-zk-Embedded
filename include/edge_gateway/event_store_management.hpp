#pragma once
#include "edge_gateway/event_store_producer.hpp"
#include <mutex>

namespace edge_gateway {
// Management callers require durable acceptance before deleting their own pending
// files. This bounded wait is never used by the event detection thread.
// Within a store, the exact (type, final scoped topic, payload bytes, original
// timestamp) is one business event, across processes/epochs/config generations.
// Distinct occurrences must differ in that tuple; JSON is not normalized.
// The caller must durably retain that immutable tuple until submit succeeds.
// Restarting with regenerated timestamps/topics or lossy pending-file payloads
// is a new event. Deduplication lasts only while EventStore retains the event ID;
// old random management:<uuid> IDs are not retroactively deduplicated.
// Requires runtime libcrypto SHA256. A duplicate ID is not itself proof of
// durable acceptance: servers rejecting duplicate IDs still cause submit to
// throw, and callers must retain pending files until that contract is upgraded.
class EventStoreManagementWriter {
public:
    explicit EventStoreManagementWriter(EventStoreProducerOptions options);
    void submit(const std::string& type, const std::string& topic,
        const std::string& payload, std::int64_t timestamp);
    bool drain(int timeoutMs);
private:
    int timeoutMs_;
    std::string storeId_;
    std::shared_ptr<AsyncEventStoreProducer> producer_;
    std::mutex mutex_;
    bool pending_ = false;
    MqttEventOutbox::EventMessage pendingEvent_;
};
} // namespace edge_gateway
