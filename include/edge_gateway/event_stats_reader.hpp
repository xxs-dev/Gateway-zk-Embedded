#pragma once

#include "edge_gateway/event_store.hpp"
#include "edge_gateway/event_store_stats.hpp"
#include "edge_gateway/event_stats_source.hpp"

#include <memory>

namespace edge_gateway {

struct LegacyEventStatsOptions {
    std::string databasePath;
    std::string sqliteLibraryPath;
    MqttEventOutbox::StorageProfile profile = MqttEventOutbox::StorageProfile::DeleteFull;
};

struct IpcEventStatsOptions {
    EventStoreIdentity identity;
    std::string socketPath;
    MqttEventOutbox::StorageProfile profile = MqttEventOutbox::StorageProfile::DeleteFull;
    int timeoutMs = 3000;
    std::size_t maxFrameBytes = 256 * 1024;
};

// Typed constructors prevent an IPC reader from carrying a legacy fallback path.
// Avoid std::variant: the deployed Allwinner GCC 6.3 standard library lacks it.
class EventStatsReaderOptions {
public:
    EventStatsReaderOptions() = default; // Invalid until a typed configuration is assigned.
    EventStatsReaderOptions(LegacyEventStatsOptions value) : legacy_(std::move(value)) {}
    EventStatsReaderOptions(IpcEventStatsOptions value) : ipc_(std::move(value)), isIpc_(true) {}
    const LegacyEventStatsOptions* legacy() const { return isIpc_ ? nullptr : &legacy_; }
    const IpcEventStatsOptions* ipc() const { return isIpc_ ? &ipc_ : nullptr; }
private:
    LegacyEventStatsOptions legacy_;
    IpcEventStatsOptions ipc_;
    bool isIpc_ = false;
};

class EventStatsReader {
public:
    virtual ~EventStatsReader() = default;
    virtual EventPendingStats read(const EventStatsScope& scope) = 0;
};

// Construction does no I/O. The reader belongs to its creating thread and process.
std::unique_ptr<EventStatsReader> makeEventStatsReader(const EventStatsReaderOptions& options);

struct EventStatsCacheOptions {
    EventStatsReaderOptions reader;
    std::vector<EventStatsQuery> queries;
    int pollIntervalMs = 1000;
    int staleAfterMs = 5000;
};

// Fixed, bounded query list. snapshot() performs no SQL or IPC on the caller.
// One explicit start per instance; stop invalidates samples and joins its reader.
class EventStatsCache : public IEventStatsSource {
public:
    explicit EventStatsCache(EventStatsCacheOptions options);
    ~EventStatsCache() override;
    EventStatsCache(const EventStatsCache&) = delete;
    EventStatsCache& operator=(const EventStatsCache&) = delete;
    void start();
    void stop();
    EventStatsCacheEntry snapshot(const std::string& key) const override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace edge_gateway
