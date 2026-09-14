#pragma once

#include "edge_gateway/event_store_stats.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace edge_gateway {

class EventStoreDatabase;

class MqttEventOutbox {
public:
    enum class AccessMode { ReadWrite, ReadOnly };
    // Change journal profiles only while all users of a database are stopped.
    // Every writer must use the same profile; do not mix SQLite libraries within one process.
    enum class StorageProfile { DeleteNormal, DeleteFull, WalNormal, WalFull };
    struct StorageSettings {
        std::string sqliteVersion;
        std::string sqliteLibraryPath;
        std::string journalMode;
        int synchronous = 0;
        int busyTimeoutMs = 0;
        int walAutoCheckpointPages = 0;
    };
    struct EventMessage {
        std::string eventType;
        std::string topic;
        std::string payload;
        std::int64_t eventTs = 0;
        std::string eventId;
        std::string targetId = "main";
    };

    struct EventState {
        std::string stateKey;
        std::string eventType;
        std::uint32_t index = 0;
        std::string alarmType;
        bool active = false;
        double value = 0.0;
        int quality = 0;
        std::int64_t sourceTs = 0;
        std::string lifecycle;
    };

    struct FanoutEnqueueResult {
        std::vector<std::int64_t> ids;
        std::vector<std::string> failedTargetIds;
    };

    struct EventTypeFilter {
        std::vector<std::string> include;
        std::vector<std::string> exclude;
    };

    struct ReplayStats {
        std::size_t count = 0;
        std::size_t bytes = 0;
        std::size_t alarmCount = 0;
        std::size_t changeCount = 0;
        std::size_t otherCount = 0;
    };

    struct ReplayMessage {
        std::string topic;
        std::string payload;
    };

    MqttEventOutbox(
        std::string dbPath,
        std::string libraryPath,
        int retentionMonths,
        int cleanupIntervalHours,
        std::size_t replayBatchSize,
        std::size_t maxDiskBytes = 0,
        StorageProfile storageProfile = StorageProfile::DeleteNormal,
        AccessMode accessMode = AccessMode::ReadWrite
    );
    ~MqttEventOutbox();

    MqttEventOutbox(const MqttEventOutbox&) = delete;
    MqttEventOutbox& operator=(const MqttEventOutbox&) = delete;

    std::int64_t enqueue(
        const std::string& eventType,
        const std::string& topic,
        const std::string& payload,
        std::int64_t eventTs
    );
    std::vector<std::int64_t> enqueueBatch(const std::vector<EventMessage>& events);
    std::vector<std::int64_t> enqueueBatchWithStates(
        const std::vector<EventMessage>& events,
        const std::vector<EventState>& states
    );
    FanoutEnqueueResult enqueueFanoutWithStates(
        const std::vector<EventMessage>& events,
        const std::vector<EventState>& states
    );
    std::vector<EventState> loadStates();
    void markSent(std::int64_t id, std::int64_t sentAt);
    void markSentBatch(const std::vector<std::int64_t>& ids, std::int64_t sentAt);
    std::size_t pendingCount();
    std::size_t pendingCount(const std::string& targetId);
    std::size_t pendingCount(const std::string& targetId, const EventTypeFilter& eventTypes);
    EventPendingStats readPendingStats(const EventStatsScope& scope);
    std::size_t replay(const std::function<void(const std::string&, const std::string&)>& send);
    std::size_t replay(
        const std::string& targetId,
        const std::function<void(const std::string&, const std::string&)>& send
    );
    std::size_t replay(
        const std::string& targetId,
        const EventTypeFilter& eventTypes,
        const std::function<void(const std::string&, const std::string&)>& send
    );
    ReplayStats replayWithStats(const std::function<void(const std::string&, const std::string&)>& send);
    ReplayStats replayWithStats(
        const std::string& targetId,
        const std::function<void(const std::string&, const std::string&)>& send
    );
    ReplayStats replayWithStats(
        const std::string& targetId,
        const EventTypeFilter& eventTypes,
        const std::function<void(const std::string&, const std::string&)>& send
    );
    std::size_t replay(
        std::size_t maxBytes,
        const std::function<void(const std::string&, const std::string&)>& send
    );
    std::size_t replay(
        const std::string& targetId,
        std::size_t maxBytes,
        const std::function<void(const std::string&, const std::string&)>& send
    );
    std::size_t replay(
        const std::string& targetId,
        const EventTypeFilter& eventTypes,
        std::size_t maxBytes,
        const std::function<void(const std::string&, const std::string&)>& send
    );
    ReplayStats replayWithStats(
        std::size_t maxBytes,
        const std::function<void(const std::string&, const std::string&)>& send
    );
    ReplayStats replayWithStats(
        const std::string& targetId,
        std::size_t maxBytes,
        const std::function<void(const std::string&, const std::string&)>& send
    );
    ReplayStats replayWithStats(
        const std::string& targetId,
        const EventTypeFilter& eventTypes,
        std::size_t maxBytes,
        const std::function<void(const std::string&, const std::string&)>& send
    );
    ReplayStats replayWithStats(
        const std::string& targetId,
        const EventTypeFilter& eventTypes,
        std::size_t maxBytes,
        std::size_t maxCount,
        const std::function<void(const std::string&, const std::string&)>& send
    );
    ReplayStats replayBatchWithStats(
        const std::string& targetId,
        const EventTypeFilter& eventTypes,
        std::size_t maxBytes,
        std::size_t maxCount,
        const std::function<void(const std::vector<ReplayMessage>&)>& send
    );
    void cleanupIfDue(std::int64_t nowMs);
    StorageSettings storageSettings() const;

private:
    friend class EventStoreDatabase;
    void loadLibrary();
    void openDatabase();
    void ensureSchema();
    void resumeStatsMigration();
    ReplayStats replayWithStatsInternal(
        const std::string& targetId,
        const EventTypeFilter& eventTypes,
        std::size_t maxBytes,
        std::size_t maxCount,
        const std::function<void(const std::string&, const std::string&)>& send,
        const std::function<void(const std::vector<ReplayMessage>&)>& batchSend
    );
    FanoutEnqueueResult enqueueBatchAndStates(
        const std::vector<EventMessage>& events,
        const std::vector<EventState>& states,
        bool isolateOptionalTargets,
        bool manageTransaction = true
    );
    void markClaimSent(
        std::int64_t id,
        std::int64_t sentAt,
        const std::string& targetId,
        const EventTypeFilter& eventTypes,
        const std::string& claimToken
    );
    void releaseClaim(std::int64_t id, const std::string& claimToken);
    void enforceDiskLimit(
        const std::vector<std::int64_t>& protectedIds,
        const std::string& targetId = std::string()
    );
    std::size_t pendingBytes(const std::string& targetId = std::string());
    std::size_t prunePendingRows(
        const std::string& targetId,
        std::size_t targetBytes,
        const std::vector<std::int64_t>& protectedIds
    );
    void closeDatabase();
    void* checkedDatabase() const;
    void rollbackAfterFailure() noexcept;
    void unloadLibrary();
    std::string eventMonth(std::int64_t eventTs) const;
    std::string cleanupBeforeMonth(std::int64_t nowMs) const;

    std::string dbPath_;
    std::string libraryPath_;
    int retentionMonths_;
    int cleanupIntervalHours_;
    std::size_t replayBatchSize_;
    std::size_t maxDiskBytes_;
    StorageProfile storageProfile_;
    AccessMode accessMode_;
    std::int64_t lastCleanupMs_ = 0;
    void* libraryHandle_ = nullptr;
    void* databaseHandle_ = nullptr;
    bool databasePoisoned_ = false;
};

}  // namespace edge_gateway
