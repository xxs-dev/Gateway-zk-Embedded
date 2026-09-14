#pragma once

#include "edge_gateway/event_store.hpp"

#include <memory>
#include <stdexcept>

namespace edge_gateway {

struct EventStoreRuntimeOptions {
    EventStoreIdentity identity;
    std::vector<std::string> producers;
    std::vector<EventStoreSenderConfig> senders;
    std::string databasePath;
    std::string socketPath;
    std::string sqliteLibraryPath;
    std::string historyPath; // Empty disables the compatibility projection.
    std::size_t historyBatchSize = 32;
    int historyPollIntervalMs = 200;
    std::uint64_t minFreeBytes = 0;
    std::uint64_t maxStoreBytes = 0;
    MqttEventOutbox::StorageProfile profile = MqttEventOutbox::StorageProfile::DeleteFull;
    std::size_t maxPeers = 16;
    std::size_t maxFrameBytes = 256 * 1024;
    std::size_t memoryBudgetBytes = 16 * 1024 * 1024;
    int ioTimeoutMs = 3000;
};

struct EventStoreHistoryStatus {
    bool enabled = false;
    std::string state = "disabled";
    std::int64_t projectedThrough = 0;
    std::int64_t observedJournalThrough = 0;
    std::uint64_t failures = 0;
    std::uint64_t auditedRows = 0;
    std::string lastError;
};

class EventStoreRuntime {
public:
    explicit EventStoreRuntime(EventStoreRuntimeOptions options);
    ~EventStoreRuntime();
    EventStoreRuntime(const EventStoreRuntime&) = delete;
    EventStoreRuntime& operator=(const EventStoreRuntime&) = delete;
    void start();
    void stop();
    bool ready() const;
    EventStoreHistoryStatus historyStatus() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class EventStoreTransportError : public std::runtime_error {
public:
    EventStoreTransportError(const std::string& message, bool unknown)
        : std::runtime_error(message), unknown_(unknown) {}
    bool outcomeUnknown() const noexcept { return unknown_; }
private:
    bool unknown_;
};

// One framed request per connection. No implicit retry, local SQL, or fallback.
std::string callEventStore(const std::string& socketPath, const std::string& request,
    int timeoutMs = 3000, std::size_t maxFrameBytes = 256 * 1024);
// Pure protocol validation, shared by the runtime and laboratory client; no I/O.
void validateEventStoreRequest(const std::string& request, const EventStoreRuntimeOptions& options);
EventStoreRuntimeOptions loadEventStoreLabConfig(const std::string& path);

}  // namespace edge_gateway
