#pragma once

#include <atomic>
#include <deque>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "edge_gateway/compat.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {

class MemoryPointStore {
public:
    explicit MemoryPointStore(const std::string& segmentName = "gateway_point_store");
    explicit MemoryPointStore(const MemoryStoreConfig& config);
    ~MemoryPointStore();

    static bool cleanupOrphanedSegment(const std::string& segmentName);

    MemoryPointStore(const MemoryPointStore&) = delete;
    MemoryPointStore& operator=(const MemoryPointStore&) = delete;

    void registerPoint(
        const std::string& machineCode,
        const std::string& meterCode,
        const PointDefinition& point
    );
    void registerPoints(
        const std::string& machineCode,
        const std::string& meterCode,
        const std::vector<PointDefinition>& points
    );
    void registerDevicePoints(const std::vector<DeviceConfig>& configs);

    void putLatest(const PointValue& value);

    Optional<StoredPointValue> getLatest(
        const std::string& machineCode,
        const std::string& meterCode,
        const std::string& pointCode,
        std::int64_t nowMs
    ) const;

    Optional<StoredPointValue> getLatestByIndex(
        std::uint32_t index,
        std::int64_t nowMs
    ) const;

    std::vector<StoredPointValue> getLatestByIndexes(
        const std::vector<std::uint32_t>& indexes,
        std::int64_t nowMs
    ) const;

    std::vector<StoredPointValue> getAllLatest(std::int64_t nowMs) const;
    std::vector<PointLeaseStatus> getAllLeaseStatus(std::int64_t nowMs) const;

    std::vector<StoredPointValue> getDeviceLatest(
        const std::string& machineCode,
        const std::string& meterCode,
        std::int64_t nowMs
    ) const;

    void submitWriteCommand(const PendingWriteCommand& command);
    std::vector<PendingWriteCommand> drainPendingWriteCommands(std::size_t limit = 0);
    std::vector<PendingWriteCommand> peekPendingWriteCommands(std::size_t limit = 0) const;
    MemoryStoreStats getStats() const;

    std::vector<PersistentPointSample> drainPersistentSamples();
    std::uint64_t consumePersistentDropCount();
    std::vector<PointUpdateRecord> drainPointUpdates(std::size_t limit = 0);
    void heartbeatRegisteredPoints(std::int64_t nowMs);
    void removeExpired(std::int64_t nowMs);

private:
    void ensureCurrentMapping() const;
    void refreshCurrentMappingLocked() const;
    void releaseOwnerClaims();
    Optional<PointBinding> getBindingByIndex(std::uint32_t index) const;
    static std::string buildKey(
        const std::string& machineCode,
        const std::string& meterCode,
        const std::string& pointCode
    );
    static bool isExpired(const StoredPointValue& value, std::int64_t nowMs);
    static StoredPointValue markStale(StoredPointValue value, std::int64_t nowMs);

    mutable SharedMutex mutex_;
    std::unordered_map<std::string, std::uint32_t> keyToIndex_;
    std::unordered_map<std::uint32_t, PointBinding> bindings_;
    mutable std::unordered_map<std::uint32_t, std::size_t> latestSlotByIndex_;
    std::unordered_map<std::uint32_t, std::int64_t> lastPersistentSampleTs_;
    std::atomic<std::uint64_t> persistentDropped_{0};
    std::set<std::uint32_t> registeredIndexes_;
    std::size_t maxLatestPoints_ = 100000;
    std::size_t maxPendingWrites_ = 4096;
    std::size_t maxPersistentSamples_ = 20000;
    std::string segmentName_;
    std::uint64_t ownerId_ = 0;
    std::string ownerSource_;
    mutable void* mappingHandle_ = nullptr;
    mutable void* mutexHandle_ = nullptr;
    mutable void* sharedView_ = nullptr;
};

}  // namespace edge_gateway
