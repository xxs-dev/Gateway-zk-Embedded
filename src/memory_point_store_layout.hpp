#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#ifndef _WIN32
#include <pthread.h>
#endif

// Native ABI shared by the store, offline migration, and layout regression tests.
namespace edge_gateway {
namespace memory_layout {

constexpr std::uint32_t kSharedStoreMagic = 0x4D505354;  // MPST
constexpr std::uint32_t kSharedStoreVersion = 11;
constexpr std::uint32_t kMinimumCompatibleSharedStoreVersion = 11;
constexpr std::size_t kMaxLatestSlots = 100000;
constexpr std::size_t kMaxPendingWriteSlots = 4096;
constexpr std::size_t kMaxWritebackResultSlots = 4096;

inline bool isCompatibleSharedStoreVersion(std::uint32_t version) {
    return version >= kMinimumCompatibleSharedStoreVersion &&
        version <= kSharedStoreVersion;
}
constexpr std::size_t kMaxPersistentSlots = 20000;
constexpr std::size_t kMaxPointUpdateSlots = 65536;
constexpr std::size_t kMaxOwnerSlots = 64;
constexpr std::size_t kMaxClaimSlots = 100000;
constexpr std::size_t kCmdIdSize = 64;
constexpr std::size_t kSourceSize = 32;
constexpr std::size_t kWritebackMessageSize = 128;
constexpr std::size_t kWritebackStageSize = 32;
constexpr std::int64_t kOwnerLeaseMs = 30000;
constexpr int kSharedMutexLockTimeoutSec = 30;

inline int sharedMutexLockTimeoutSec() {
    const char* raw = std::getenv("GATEWAY_SHARED_MUTEX_LOCK_TIMEOUT_SEC");
    if (raw == nullptr || *raw == '\0') {
        return kSharedMutexLockTimeoutSec;
    }
    const auto parsed = std::atoi(raw);
    return parsed > 0 ? parsed : kSharedMutexLockTimeoutSec;
}

struct SharedLatestSlot {
    std::uint32_t index = 0;
    double value = 0.0;
    std::int32_t quality = 1;
    std::int64_t ts = 0;
    std::int64_t expireAt = 0;
    std::uint8_t stale = 0;
    std::uint8_t occupied = 0;
    std::uint8_t reserved[6] = {};
};

struct SharedClusterWriteAuthorization {
    std::uint32_t version = 0;
    std::uint32_t flags = 0;
    std::uint8_t kernelBootId[16] = {};
    std::uint8_t authorityEpoch[16] = {};
    std::int64_t notAfterMonotonicMs = 0;
    std::uint64_t dispatchSequence = 0;
    char authorityStoreName[64] = {};
};
static_assert(sizeof(SharedClusterWriteAuthorization) == 120, "cluster authorization ABI changed");

struct SharedClusterAuthoritySnapshot {
    SharedClusterWriteAuthorization authorization{};
    std::uint32_t occupied = 0;
    std::uint32_t valid = 0;
    std::int32_t role = 0;
    std::int32_t dispatchCode = 0;
    std::uint32_t stationStrategyActive = 0;
    std::uint32_t targetCount = 0;
    double targets[6] = {};
    std::uint32_t targetIndexes[256] = {};
    std::uint8_t strategyKernelBootId[16] = {};
    std::int64_t strategyNotAfterMonotonicMs = 0;
};
static_assert(sizeof(SharedClusterAuthoritySnapshot) == 1240, "cluster snapshot ABI changed");

struct SharedPendingWriteSlot {
    std::uint64_t sequence = 0;
    std::uint32_t index = 0;
    double value = 0.0;
    std::int64_t ts = 0;
    std::int64_t acceptedAt = 0;
    char cmdId[kCmdIdSize] = {};
    char source[kSourceSize] = {};
    std::uint8_t occupied = 0;
    std::uint8_t highPriority = 0;
    std::uint8_t reserved[2] = {};
    std::uint32_t controlGeneration = 0;
    SharedClusterWriteAuthorization clusterAuthorization{};
};

static_assert(sizeof(SharedPendingWriteSlot) == 264, "pending write ABI changed unexpectedly");
static_assert(
    offsetof(SharedPendingWriteSlot, controlGeneration) == 140,
    "control generation must occupy the previous reserved bytes"
);

struct SharedWritebackResultSlot {
    std::uint64_t sequence = 0;
    std::uint32_t index = 0;
    double value = 0.0;
    std::int64_t requestedAt = 0;
    std::int64_t acceptedAt = 0;
    std::int64_t startedAt = 0;
    std::int64_t completedAt = 0;
    std::int64_t queueDelayMs = 0;
    std::int64_t deviceWriteMs = 0;
    std::int64_t edgeElapsedMs = 0;
    std::int64_t totalElapsedMs = 0;
    char cmdId[kCmdIdSize] = {};
    char message[kWritebackMessageSize] = {};
    char stage[kWritebackStageSize] = {};
    std::uint8_t success = 0;
    std::uint8_t verifyAttempted = 0;
    std::uint8_t verifyPassed = 0;
    std::uint8_t occupied = 0;
};

struct SharedPersistentSlot {
    std::uint64_t sequence = 0;
    std::uint32_t index = 0;
    double value = 0.0;
    std::int64_t ts = 0;
    std::uint8_t occupied = 0;
    std::uint8_t reserved[7] = {};
};

struct SharedPointUpdateSlot {
    std::uint64_t sequence = 0;
    std::uint32_t index = 0;
    double value = 0.0;
    std::int32_t quality = 1;
    std::int64_t ts = 0;
    std::int64_t expireAt = 0;
    std::uint8_t occupied = 0;
    std::uint8_t reserved[7] = {};
};

struct SharedOwnerSlot {
    std::uint64_t ownerId = 0;
    std::int64_t heartbeatMs = 0;
    char source[kSourceSize] = {};
    std::uint8_t occupied = 0;
    std::uint8_t reserved[7] = {};
};

struct SharedClaimSlot {
    std::uint32_t index = 0;
    std::uint64_t ownerId = 0;
    std::int64_t heartbeatMs = 0;
    std::uint8_t occupied = 0;
    std::uint8_t reserved[11] = {};
};

#ifndef _WIN32
struct SharedStoreHeader {
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    pthread_mutex_t mutex{};
    std::uint64_t writeSequence = 0;
    std::uint64_t writebackResultSequence = 0;
    std::uint64_t persistentSequence = 0;
    std::uint64_t pointUpdateSequence = 0;
    std::uint32_t latestCount = 0;
    std::uint32_t pendingWriteHead = 0;
    std::uint32_t pendingWriteTail = 0;
    std::uint32_t writebackResultHead = 0;
    std::uint32_t writebackResultTail = 0;
    std::uint32_t persistentHead = 0;
    std::uint32_t persistentTail = 0;
    std::uint32_t pointUpdateHead = 0;
    std::uint32_t pointUpdateTail = 0;
};
#else
struct SharedStoreHeader {
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint64_t writeSequence = 0;
    std::uint64_t writebackResultSequence = 0;
    std::uint64_t persistentSequence = 0;
    std::uint64_t pointUpdateSequence = 0;
    std::uint32_t latestCount = 0;
    std::uint32_t pendingWriteHead = 0;
    std::uint32_t pendingWriteTail = 0;
    std::uint32_t writebackResultHead = 0;
    std::uint32_t writebackResultTail = 0;
    std::uint32_t persistentHead = 0;
    std::uint32_t persistentTail = 0;
    std::uint32_t pointUpdateHead = 0;
    std::uint32_t pointUpdateTail = 0;
};
#endif

struct SharedStoreLayout {
    SharedStoreHeader header{};
    SharedLatestSlot latest[kMaxLatestSlots];
    SharedPendingWriteSlot pendingWrites[kMaxPendingWriteSlots];
    SharedWritebackResultSlot writebackResults[kMaxWritebackResultSlots];
    SharedPersistentSlot persistent[kMaxPersistentSlots];
    SharedPointUpdateSlot pointUpdates[kMaxPointUpdateSlots];
    SharedOwnerSlot owners[kMaxOwnerSlots];
    SharedClaimSlot claims[kMaxClaimSlots];
    SharedClusterAuthoritySnapshot clusterAuthority{};
};

}  // namespace memory_layout
}  // namespace edge_gateway
