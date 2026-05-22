#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace edge_gateway {

class MqttEventOutbox {
public:
    struct EventMessage {
        std::string eventType;
        std::string topic;
        std::string payload;
        std::int64_t eventTs = 0;
    };

    struct ReplayStats {
        std::size_t count = 0;
        std::size_t bytes = 0;
        std::size_t alarmCount = 0;
        std::size_t changeCount = 0;
        std::size_t otherCount = 0;
    };

    MqttEventOutbox(
        std::string dbPath,
        std::string libraryPath,
        int retentionMonths,
        int cleanupIntervalHours,
        std::size_t replayBatchSize,
        std::size_t maxDiskBytes = 0
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
    void markSent(std::int64_t id, std::int64_t sentAt);
    void markSentBatch(const std::vector<std::int64_t>& ids, std::int64_t sentAt);
    std::size_t pendingCount();
    std::size_t replay(const std::function<void(const std::string&, const std::string&)>& send);
    ReplayStats replayWithStats(const std::function<void(const std::string&, const std::string&)>& send);
    std::size_t replay(
        std::size_t maxBytes,
        const std::function<void(const std::string&, const std::string&)>& send
    );
    ReplayStats replayWithStats(
        std::size_t maxBytes,
        const std::function<void(const std::string&, const std::string&)>& send
    );
    void cleanupIfDue(std::int64_t nowMs);

private:
    void loadLibrary();
    void openDatabase();
    void ensureSchema();
    void enforceDiskLimit();
    std::size_t pendingBytes();
    std::size_t prunePendingRows(std::size_t targetBytes);
    void closeDatabase();
    void unloadLibrary();
    std::string eventMonth(std::int64_t eventTs) const;
    std::string cleanupBeforeMonth(std::int64_t nowMs) const;

    std::string dbPath_;
    std::string libraryPath_;
    int retentionMonths_;
    int cleanupIntervalHours_;
    std::size_t replayBatchSize_;
    std::size_t maxDiskBytes_;
    std::int64_t lastCleanupMs_ = 0;
    void* libraryHandle_ = nullptr;
    void* databaseHandle_ = nullptr;
};

}  // namespace edge_gateway
