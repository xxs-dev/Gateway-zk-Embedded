#include "edge_gateway/memory_point_store.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace edge_gateway {

namespace {

constexpr std::uint32_t kSharedStoreMagic = 0x4D505354;  // MPST
constexpr std::uint32_t kSharedStoreVersion = 6;
constexpr std::size_t kMaxLatestSlots = 100000;
constexpr std::size_t kMaxPendingWriteSlots = 4096;
constexpr std::size_t kMaxPersistentSlots = 20000;
constexpr std::size_t kMaxPointUpdateSlots = 65536;
constexpr std::size_t kMaxOwnerSlots = 64;
constexpr std::size_t kMaxClaimSlots = 100000;
constexpr std::size_t kCmdIdSize = 64;
constexpr std::size_t kSourceSize = 32;
constexpr std::int64_t kOwnerLeaseMs = 30000;

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

struct SharedPendingWriteSlot {
    std::uint64_t sequence = 0;
    std::uint32_t index = 0;
    double value = 0.0;
    std::int64_t ts = 0;
    char cmdId[kCmdIdSize] = {};
    char source[kSourceSize] = {};
    std::uint8_t occupied = 0;
    std::uint8_t reserved[7] = {};
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
    std::uint64_t persistentSequence = 0;
    std::uint64_t pointUpdateSequence = 0;
    std::uint32_t latestCount = 0;
    std::uint32_t pendingWriteHead = 0;
    std::uint32_t pendingWriteTail = 0;
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
    std::uint64_t persistentSequence = 0;
    std::uint64_t pointUpdateSequence = 0;
    std::uint32_t latestCount = 0;
    std::uint32_t pendingWriteHead = 0;
    std::uint32_t pendingWriteTail = 0;
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
    SharedPersistentSlot persistent[kMaxPersistentSlots];
    SharedPointUpdateSlot pointUpdates[kMaxPointUpdateSlots];
    SharedOwnerSlot owners[kMaxOwnerSlots];
    SharedClaimSlot claims[kMaxClaimSlots];
};

#ifdef _WIN32
class SharedLockGuard {
public:
    explicit SharedLockGuard(void* handle) : handle_(static_cast<HANDLE>(handle)) {
        if (WaitForSingleObject(handle_, INFINITE) != WAIT_OBJECT_0) {
            throw std::runtime_error("failed to lock shared memory mutex");
        }
    }

    ~SharedLockGuard() {
        ReleaseMutex(handle_);
    }

private:
    HANDLE handle_;
};
#else
class SharedLockGuard {
public:
    explicit SharedLockGuard(pthread_mutex_t* mutex) : mutex_(mutex) {
        timespec deadline{};
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 5;

        const auto rc = pthread_mutex_timedlock(mutex_, &deadline);
        if (rc == EOWNERDEAD) {
            pthread_mutex_consistent(mutex_);
            return;
        }
        if (rc == ETIMEDOUT) {
            throw std::runtime_error("shared memory mutex lock timed out");
        }
        if (rc != 0) {
            throw std::runtime_error("failed to lock shared memory mutex");
        }
    }

    ~SharedLockGuard() {
        pthread_mutex_unlock(mutex_);
    }

private:
    pthread_mutex_t* mutex_;
};
#endif

SharedStoreLayout* layoutFrom(void* sharedView) {
    return static_cast<SharedStoreLayout*>(sharedView);
}

void copyString(char* target, std::size_t size, const std::string& value) {
    const auto count = std::min(size - 1, value.size());
    std::memset(target, 0, size);
    std::memcpy(target, value.data(), count);
}

std::string readString(const char* source, std::size_t size) {
    std::size_t length = 0;
    while (length < size && source[length] != '\0') {
        ++length;
    }
    return std::string(source, source + length);
}

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::uint64_t makeOwnerId() {
    return static_cast<std::uint64_t>(currentTimeMs()) ^
           static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&makeOwnerId));
}

SharedLatestSlot* findLatestSlot(SharedStoreLayout* layout, std::uint32_t index) {
    for (auto& slot : layout->latest) {
        if (slot.occupied && slot.index == index) {
            return &slot;
        }
    }
    return nullptr;
}

const SharedLatestSlot* findLatestSlot(const SharedStoreLayout* layout, std::uint32_t index) {
    for (const auto& slot : layout->latest) {
        if (slot.occupied && slot.index == index) {
            return &slot;
        }
    }
    return nullptr;
}

SharedOwnerSlot* findOwnerSlot(SharedStoreLayout* layout, std::uint64_t ownerId) {
    for (auto& slot : layout->owners) {
        if (slot.occupied && slot.ownerId == ownerId) {
            return &slot;
        }
    }
    return nullptr;
}

SharedOwnerSlot* allocateOwnerSlot(SharedStoreLayout* layout, std::uint64_t ownerId) {
    if (auto* existing = findOwnerSlot(layout, ownerId)) {
        return existing;
    }
    for (auto& slot : layout->owners) {
        if (!slot.occupied) {
            slot = SharedOwnerSlot{};
            slot.ownerId = ownerId;
            slot.occupied = 1;
            return &slot;
        }
    }
    throw std::runtime_error("shared owner registry is full");
}

SharedClaimSlot* findClaimSlot(SharedStoreLayout* layout, std::uint32_t index, std::uint64_t ownerId) {
    for (auto& slot : layout->claims) {
        if (slot.occupied && slot.index == index && slot.ownerId == ownerId) {
            return &slot;
        }
    }
    return nullptr;
}

SharedClaimSlot* allocateClaimSlot(SharedStoreLayout* layout, std::uint32_t index, std::uint64_t ownerId) {
    if (auto* existing = findClaimSlot(layout, index, ownerId)) {
        return existing;
    }
    for (auto& slot : layout->claims) {
        if (!slot.occupied) {
            slot = SharedClaimSlot{};
            slot.index = index;
            slot.ownerId = ownerId;
            slot.occupied = 1;
            return &slot;
        }
    }
    throw std::runtime_error("shared claim registry is full");
}

bool isOwnerAlive(const SharedOwnerSlot& slot, std::int64_t nowMs) {
    return slot.occupied && slot.heartbeatMs > 0 && (nowMs - slot.heartbeatMs) <= kOwnerLeaseMs;
}

void cleanupStaleOwnersAndClaims(SharedStoreLayout* layout, std::int64_t nowMs) {
    for (auto& owner : layout->owners) {
        if (!owner.occupied || isOwnerAlive(owner, nowMs)) {
            continue;
        }
        const auto ownerId = owner.ownerId;
        owner = SharedOwnerSlot{};
        for (auto& claim : layout->claims) {
            if (claim.occupied && claim.ownerId == ownerId) {
                claim = SharedClaimSlot{};
            }
        }
    }
}

bool hasActiveClaim(const SharedStoreLayout* layout, std::uint32_t index, std::int64_t nowMs) {
    for (const auto& claim : layout->claims) {
        if (!claim.occupied || claim.index != index) {
            continue;
        }
        for (const auto& owner : layout->owners) {
            if (owner.occupied && owner.ownerId == claim.ownerId && isOwnerAlive(owner, nowMs)) {
                return true;
            }
        }
    }
    return false;
}

bool hasAnyActiveOwner(const SharedStoreLayout* layout, std::int64_t nowMs) {
    for (const auto& owner : layout->owners) {
        if (isOwnerAlive(owner, nowMs)) {
            return true;
        }
    }
    return false;
}

std::size_t ringCount(std::uint32_t head, std::uint32_t tail, std::size_t capacity) {
    return (tail + capacity - head) % capacity;
}

bool isIndexClaimedByOtherActiveOwner(
    const SharedStoreLayout* layout,
    std::uint32_t index,
    std::uint64_t ownerId,
    std::int64_t nowMs
) {
    for (const auto& claim : layout->claims) {
        if (!claim.occupied || claim.index != index || claim.ownerId == ownerId) {
            continue;
        }
        for (const auto& owner : layout->owners) {
            if (owner.occupied && owner.ownerId == claim.ownerId && isOwnerAlive(owner, nowMs)) {
                return true;
            }
        }
    }
    return false;
}

SharedLatestSlot* allocateLatestSlot(
    SharedStoreLayout* layout,
    std::uint32_t index,
    std::size_t maxLatestPoints,
    std::unordered_map<std::uint32_t, std::size_t>* latestSlotByIndex = nullptr
) {
    if (latestSlotByIndex != nullptr) {
        const auto cached = latestSlotByIndex->find(index);
        if (cached != latestSlotByIndex->end() && cached->second < kMaxLatestSlots) {
            auto* slot = &layout->latest[cached->second];
            if (slot->occupied && slot->index == index) {
                return slot;
            }
            latestSlotByIndex->erase(cached);
        }
    }
    if (latestSlotByIndex == nullptr) {
        if (auto* existing = findLatestSlot(layout, index)) {
            return existing;
        }
    }
    if (layout->header.latestCount >= maxLatestPoints) {
        throw std::runtime_error("shared latest store reached configured limit");
    }
    if (layout->header.latestCount < kMaxLatestSlots) {
        auto& slot = layout->latest[layout->header.latestCount];
        if (!slot.occupied) {
            slot = SharedLatestSlot{};
            slot.index = index;
            slot.occupied = 1;
            if (latestSlotByIndex != nullptr) {
                (*latestSlotByIndex)[index] = layout->header.latestCount;
            }
            ++layout->header.latestCount;
            return &slot;
        }
    }
    for (auto& slot : layout->latest) {
        if (!slot.occupied) {
            slot = SharedLatestSlot{};
            slot.index = index;
            slot.occupied = 1;
            ++layout->header.latestCount;
            if (latestSlotByIndex != nullptr) {
                (*latestSlotByIndex)[index] = static_cast<std::size_t>(&slot - layout->latest);
            }
            return &slot;
        }
    }
    throw std::runtime_error("shared latest store is full");
}

void pushPendingWrite(
    SharedStoreLayout* layout,
    const PendingWriteCommand& command,
    std::size_t maxPendingWrites
) {
    const auto pendingCount = ringCount(
        layout->header.pendingWriteHead,
        layout->header.pendingWriteTail,
        kMaxPendingWriteSlots
    );
    if (pendingCount >= maxPendingWrites) {
        throw std::runtime_error("shared pending write queue is full");
    }

    auto& slot = layout->pendingWrites[layout->header.pendingWriteTail];
    slot = SharedPendingWriteSlot{};
    slot.sequence = ++layout->header.writeSequence;
    slot.index = command.index;
    slot.value = command.value;
    slot.ts = command.ts;
    copyString(slot.cmdId, kCmdIdSize, command.cmdId);
    copyString(slot.source, kSourceSize, command.source);
    slot.occupied = 1;

    layout->header.pendingWriteTail =
        (layout->header.pendingWriteTail + 1) % kMaxPendingWriteSlots;
}

std::vector<PendingWriteCommand> drainPendingWrites(
    SharedStoreLayout* layout,
    std::size_t limit,
    const std::set<std::uint32_t>* allowedIndexes
) {
    std::vector<PendingWriteCommand> result;
    std::vector<SharedPendingWriteSlot> retained;
    auto head = layout->header.pendingWriteHead;
    while (head != layout->header.pendingWriteTail) {
        auto& slot = layout->pendingWrites[head];
        if (slot.occupied) {
            const bool allowed =
                allowedIndexes == nullptr ||
                allowedIndexes->empty() ||
                allowedIndexes->find(slot.index) != allowedIndexes->end();
            const bool underLimit = limit == 0 || result.size() < limit;
            if (allowed && underLimit) {
                result.push_back(PendingWriteCommand{
                    readString(slot.cmdId, kCmdIdSize),
                    slot.index,
                    slot.value,
                    readString(slot.source, kSourceSize),
                    slot.ts
                });
            } else {
                retained.push_back(slot);
            }
        }
        head = (head + 1) % kMaxPendingWriteSlots;
    }

    for (auto& slot : layout->pendingWrites) {
        slot = SharedPendingWriteSlot{};
    }
    layout->header.pendingWriteHead = 0;
    layout->header.pendingWriteTail = 0;
    for (const auto& slot : retained) {
        layout->pendingWrites[layout->header.pendingWriteTail] = slot;
        layout->header.pendingWriteTail =
            (layout->header.pendingWriteTail + 1) % kMaxPendingWriteSlots;
    }
    return result;
}

std::vector<PendingWriteCommand> peekPendingWrites(const SharedStoreLayout* layout, std::size_t limit) {
    std::vector<PendingWriteCommand> result;
    auto head = layout->header.pendingWriteHead;
    while (head != layout->header.pendingWriteTail) {
        if (limit > 0 && result.size() >= limit) {
            break;
        }

        const auto& slot = layout->pendingWrites[head];
        if (slot.occupied) {
            result.push_back(PendingWriteCommand{
                readString(slot.cmdId, kCmdIdSize),
                slot.index,
                slot.value,
                readString(slot.source, kSourceSize),
                slot.ts
            });
        }
        head = (head + 1) % kMaxPendingWriteSlots;
    }
    return result;
}

void pushPersistentSample(
    SharedStoreLayout* layout,
    const PersistentPointSample& sample,
    std::size_t maxPersistentSamples
) {
    auto& slot = layout->persistent[layout->header.persistentTail];
    slot = SharedPersistentSlot{};
    slot.sequence = ++layout->header.persistentSequence;
    slot.index = sample.index;
    slot.value = sample.value;
    slot.ts = sample.ts;
    slot.occupied = 1;

    layout->header.persistentTail =
        (layout->header.persistentTail + 1) % kMaxPersistentSlots;
    while (((layout->header.persistentTail + kMaxPersistentSlots - layout->header.persistentHead) %
            kMaxPersistentSlots) >= maxPersistentSamples) {
        layout->header.persistentHead =
            (layout->header.persistentHead + 1) % kMaxPersistentSlots;
    }
}

std::vector<PersistentPointSample> drainPersistent(SharedStoreLayout* layout) {
    std::vector<PersistentPointSample> result;
    while (layout->header.persistentHead != layout->header.persistentTail) {
        auto& slot = layout->persistent[layout->header.persistentHead];
        if (slot.occupied) {
            result.push_back(PersistentPointSample{slot.index, slot.value, slot.ts});
            slot = SharedPersistentSlot{};
        }
        layout->header.persistentHead =
            (layout->header.persistentHead + 1) % kMaxPersistentSlots;
    }
    return result;
}

void pushPointUpdate(SharedStoreLayout* layout, const PointUpdateRecord& update) {
    auto& slot = layout->pointUpdates[layout->header.pointUpdateTail];
    slot = SharedPointUpdateSlot{};
    slot.sequence = ++layout->header.pointUpdateSequence;
    slot.index = update.index;
    slot.value = update.value;
    slot.quality = update.quality;
    slot.ts = update.ts;
    slot.expireAt = update.expireAt;
    slot.occupied = 1;

    layout->header.pointUpdateTail =
        (layout->header.pointUpdateTail + 1) % kMaxPointUpdateSlots;
    if (layout->header.pointUpdateTail == layout->header.pointUpdateHead) {
        layout->header.pointUpdateHead =
            (layout->header.pointUpdateHead + 1) % kMaxPointUpdateSlots;
    }
}

std::vector<PointUpdateRecord> drainPointUpdates(SharedStoreLayout* layout, std::size_t limit) {
    std::vector<PointUpdateRecord> result;
    while (layout->header.pointUpdateHead != layout->header.pointUpdateTail) {
        if (limit > 0 && result.size() >= limit) {
            break;
        }

        auto& slot = layout->pointUpdates[layout->header.pointUpdateHead];
        if (slot.occupied) {
            PointUpdateRecord update;
            update.sequence = slot.sequence;
            update.index = slot.index;
            update.value = slot.value;
            update.quality = slot.quality;
            update.ts = slot.ts;
            update.expireAt = slot.expireAt;
            result.push_back(update);
            slot = SharedPointUpdateSlot{};
        }
        layout->header.pointUpdateHead =
            (layout->header.pointUpdateHead + 1) % kMaxPointUpdateSlots;
    }
    return result;
}

std::string normalizeSegmentName(std::string name) {
#ifndef _WIN32
    if (name.empty()) {
        name = "gateway_point_store";
    }
    if (name.front() != '/') {
        name.insert(name.begin(), '/');
    }
    return name;
#else
    return name;
#endif
}

#ifndef _WIN32
class PosixFileLockGuard {
public:
    explicit PosixFileLockGuard(int fd) : fd_(fd) {
        if (flock(fd_, LOCK_EX) != 0) {
            throw std::runtime_error("failed to lock shared memory file");
        }
    }

    ~PosixFileLockGuard() {
        flock(fd_, LOCK_UN);
    }

    PosixFileLockGuard(const PosixFileLockGuard&) = delete;
    PosixFileLockGuard& operator=(const PosixFileLockGuard&) = delete;

private:
    int fd_;
};

void initializePosixMutex(pthread_mutex_t& mutex) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
#ifdef PTHREAD_MUTEX_ROBUST
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
#endif
    pthread_mutex_init(&mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}
#endif

}  // namespace

MemoryPointStore::MemoryPointStore(const std::string& segmentName)
    : segmentName_(normalizeSegmentName(segmentName)),
      ownerId_(makeOwnerId()),
      ownerSource_("pid-" +
#ifdef _WIN32
                   std::to_string(static_cast<unsigned long long>(GetCurrentProcessId()))
#else
                   std::to_string(static_cast<unsigned long long>(getpid()))
#endif
      ) {
#ifdef _WIN32
    mappingHandle_ = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(sizeof(SharedStoreLayout)),
        segmentName_.c_str()
    );
    if (mappingHandle_ == nullptr) {
        throw std::runtime_error("CreateFileMappingA failed");
    }

    sharedView_ = MapViewOfFile(static_cast<HANDLE>(mappingHandle_), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedStoreLayout));
    if (sharedView_ == nullptr) {
        CloseHandle(static_cast<HANDLE>(mappingHandle_));
        mappingHandle_ = nullptr;
        throw std::runtime_error("MapViewOfFile failed");
    }

    const auto mutexName = segmentName_ + "_mutex";
    mutexHandle_ = CreateMutexA(nullptr, FALSE, mutexName.c_str());
    if (mutexHandle_ == nullptr) {
        UnmapViewOfFile(sharedView_);
        CloseHandle(static_cast<HANDLE>(mappingHandle_));
        sharedView_ = nullptr;
        mappingHandle_ = nullptr;
        throw std::runtime_error("CreateMutexA failed");
    }

    SharedLockGuard lock(mutexHandle_);
    auto* layout = layoutFrom(sharedView_);
    if (layout->header.magic != kSharedStoreMagic || layout->header.version != kSharedStoreVersion) {
        std::memset(layout, 0, sizeof(SharedStoreLayout));
        layout->header.magic = kSharedStoreMagic;
        layout->header.version = kSharedStoreVersion;
    }
    latestSlotByIndex_.clear();
    latestSlotByIndex_.reserve(layout->header.latestCount);
    for (std::size_t i = 0; i < kMaxLatestSlots; ++i) {
        if (layout->latest[i].occupied) {
            latestSlotByIndex_[layout->latest[i].index] = i;
        }
    }
#else
    const int fd = shm_open(segmentName_.c_str(), O_CREAT | O_RDWR, 0660);
    if (fd < 0) {
        throw std::runtime_error("shm_open failed");
    }
    mappingHandle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd) + 1);
    PosixFileLockGuard fileLock(fd);

    struct stat shmStat;
    std::memset(&shmStat, 0, sizeof(shmStat));
    if (fstat(fd, &shmStat) != 0) {
        close(fd);
        mappingHandle_ = nullptr;
        throw std::runtime_error("fstat failed");
    }

    const bool needsInitialization = shmStat.st_size == 0;
    if (needsInitialization) {
        if (ftruncate(fd, static_cast<off_t>(sizeof(SharedStoreLayout))) != 0) {
            close(fd);
            mappingHandle_ = nullptr;
            throw std::runtime_error("ftruncate failed");
        }
    } else if (shmStat.st_size != static_cast<off_t>(sizeof(SharedStoreLayout))) {
        if (ftruncate(fd, static_cast<off_t>(sizeof(SharedStoreLayout))) != 0) {
            close(fd);
            mappingHandle_ = nullptr;
            throw std::runtime_error("ftruncate resize failed");
        }
    }

    sharedView_ = mmap(nullptr, sizeof(SharedStoreLayout), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (sharedView_ == MAP_FAILED) {
        close(fd);
        sharedView_ = nullptr;
        mappingHandle_ = nullptr;
        throw std::runtime_error("mmap failed");
    }

    auto* layout = layoutFrom(sharedView_);
    if (needsInitialization ||
        shmStat.st_size != static_cast<off_t>(sizeof(SharedStoreLayout)) ||
        (layout->header.magic == 0 && layout->header.version == 0)) {
        std::memset(layout, 0, sizeof(SharedStoreLayout));
        initializePosixMutex(layout->header.mutex);
        layout->header.magic = kSharedStoreMagic;
        layout->header.version = kSharedStoreVersion;
    } else if (layout->header.magic != kSharedStoreMagic || layout->header.version != kSharedStoreVersion) {
        munmap(sharedView_, sizeof(SharedStoreLayout));
        sharedView_ = nullptr;
        close(fd);
        mappingHandle_ = nullptr;
        throw std::runtime_error("shared memory version mismatch, rebuild all processes and restart");
    }
    {
        SharedLockGuard sharedLock(&layout->header.mutex);
        latestSlotByIndex_.clear();
        latestSlotByIndex_.reserve(layout->header.latestCount);
        for (std::size_t i = 0; i < kMaxLatestSlots; ++i) {
            if (layout->latest[i].occupied) {
                latestSlotByIndex_[layout->latest[i].index] = i;
            }
        }
    }
#endif
}

bool MemoryPointStore::cleanupOrphanedSegment(const std::string& segmentName) {
#ifdef _WIN32
    (void)segmentName;
    return false;
#else
    const auto normalizedName = normalizeSegmentName(segmentName);
    const int fd = shm_open(normalizedName.c_str(), O_RDWR, 0666);
    if (fd < 0) {
        return false;
    }

    bool removed = false;
    void* view = mmap(nullptr, sizeof(SharedStoreLayout), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (view != MAP_FAILED) {
        auto* layout = layoutFrom(view);
        if (layout->header.magic == kSharedStoreMagic && layout->header.version == kSharedStoreVersion) {
            try {
                SharedLockGuard sharedLock(&layout->header.mutex);
                const auto ts = currentTimeMs();
                cleanupStaleOwnersAndClaims(layout, ts);
                if (!hasAnyActiveOwner(layout, ts)) {
                    removed = shm_unlink(normalizedName.c_str()) == 0;
                }
            } catch (...) {
            }
        }
        munmap(view, sizeof(SharedStoreLayout));
    }
    close(fd);
    return removed;
#endif
}

MemoryPointStore::MemoryPointStore(const MemoryStoreConfig& config)
    : MemoryPointStore(config.sharedMemoryName) {
    maxLatestPoints_ = config.maxLatestPoints == 0
        ? kMaxLatestSlots
        : std::max<std::size_t>(1, std::min(config.maxLatestPoints, kMaxLatestSlots));
    maxPendingWrites_ = std::max<std::size_t>(1, std::min(config.maxPendingWrites, kMaxPendingWriteSlots - 1));
    maxPersistentSamples_ = std::max<std::size_t>(1, std::min(config.maxPersistentSamples, kMaxPersistentSlots - 1));
}

MemoryPointStore::~MemoryPointStore() {
#ifdef _WIN32
    if (sharedView_ != nullptr) {
        UnmapViewOfFile(sharedView_);
        sharedView_ = nullptr;
    }
    if (mappingHandle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mappingHandle_));
        mappingHandle_ = nullptr;
    }
    if (mutexHandle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mutexHandle_));
        mutexHandle_ = nullptr;
    }
#else
    releaseOwnerClaims();
    if (sharedView_ != nullptr) {
        munmap(sharedView_, sizeof(SharedStoreLayout));
        sharedView_ = nullptr;
    }
    if (mappingHandle_ != nullptr) {
        const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(mappingHandle_) - 1);
        close(fd);
        mappingHandle_ = nullptr;
    }
#endif
}

void MemoryPointStore::releaseOwnerClaims() {
#ifdef _WIN32
    return;
#else
    if (sharedView_ == nullptr) {
        return;
    }

    auto* layout = layoutFrom(sharedView_);
    try {
        SharedLockGuard sharedLock(&layout->header.mutex);
        const auto ts = currentTimeMs();
        cleanupStaleOwnersAndClaims(layout, ts);
        for (auto& claim : layout->claims) {
            if (claim.occupied && claim.ownerId == ownerId_) {
                claim = SharedClaimSlot{};
            }
        }
        for (auto& owner : layout->owners) {
            if (owner.occupied && owner.ownerId == ownerId_) {
                owner = SharedOwnerSlot{};
            }
        }
        cleanupStaleOwnersAndClaims(layout, ts);
    } catch (...) {
        return;
    }
#endif
}

void MemoryPointStore::registerPoint(
    const std::string& machineCode,
    const std::string& meterCode,
    const PointDefinition& point
) {
    if (point.index == 0) {
        throw std::invalid_argument("point.index must be non-zero");
    }

    WriteLock lock(mutex_);
    const auto key = buildKey(machineCode, meterCode, point.pointCode);
    const auto keyIt = keyToIndex_.find(key);
    if (keyIt != keyToIndex_.end() && keyIt->second != point.index) {
        throw std::invalid_argument("point key already bound to another index");
    }

    const auto bindingIt = bindings_.find(point.index);
    if (bindingIt != bindings_.end()) {
        const auto& binding = bindingIt->second;
        if (binding.machineCode != machineCode ||
            binding.meterCode != meterCode ||
            binding.pointCode != point.pointCode) {
            throw std::invalid_argument("duplicate point.index detected");
        }
    }

    keyToIndex_[key] = point.index;
    bindings_[point.index] = PointBinding{point.index, machineCode, meterCode, point.pointCode};
    registeredIndexes_.insert(point.index);

#ifdef _WIN32
    SharedLockGuard sharedLock(mutexHandle_);
#else
    auto* layout = layoutFrom(sharedView_);
    SharedLockGuard sharedLock(&layout->header.mutex);
#endif
    auto* layout2 = layoutFrom(sharedView_);
    const auto ts = currentTimeMs();
    cleanupStaleOwnersAndClaims(layout2, ts);
    if (isIndexClaimedByOtherActiveOwner(layout2, point.index, ownerId_, ts)) {
        throw std::invalid_argument("point.index already claimed by another active process");
    }
    auto* ownerSlot = allocateOwnerSlot(layout2, ownerId_);
    ownerSlot->heartbeatMs = ts;
    copyString(ownerSlot->source, kSourceSize, ownerSource_);
    auto* claimSlot = allocateClaimSlot(layout2, point.index, ownerId_);
    claimSlot->heartbeatMs = ts;
}

void MemoryPointStore::putLatest(const PointValue& value) {
    WriteLock lock(mutex_);
    const auto key = buildKey(value.machineCode, value.meterCode, value.pointCode);
    keyToIndex_[key] = value.index;
    bindings_[value.index] = PointBinding{value.index, value.machineCode, value.meterCode, value.pointCode};

#ifdef _WIN32
    SharedLockGuard sharedLock(mutexHandle_);
#else
    auto* layout = layoutFrom(sharedView_);
    SharedLockGuard sharedLock(&layout->header.mutex);
#endif
    auto* layout2 = layoutFrom(sharedView_);
    auto* slot = allocateLatestSlot(layout2, value.index, maxLatestPoints_, &latestSlotByIndex_);
    slot->index = value.index;
    slot->value = value.value;
    slot->quality = value.quality;
    slot->ts = value.ts;
    slot->expireAt = value.expireAt;
    slot->stale = 0;
    slot->occupied = 1;
    pushPointUpdate(
        layout2,
        PointUpdateRecord{
            0,
            value.index,
            value.value,
            value.quality,
            value.ts,
            value.expireAt
        }
    );

    if (value.isStore) {
        const auto intervalMs = static_cast<std::int64_t>(std::max(1, value.persistIntervalSec)) * 1000;
        const auto lastIt = lastPersistentSampleTs_.find(value.index);
        if (lastIt == lastPersistentSampleTs_.end() || (value.ts - lastIt->second) >= intervalMs) {
            pushPersistentSample(
                layout2,
                PersistentPointSample{value.index, value.value, value.ts},
                maxPersistentSamples_
            );
            lastPersistentSampleTs_[value.index] = value.ts;
        }
    }
}

Optional<StoredPointValue> MemoryPointStore::getLatest(
    const std::string& machineCode,
    const std::string& meterCode,
    const std::string& pointCode,
    std::int64_t nowMs
) const {
    std::uint32_t index = 0;
    {
        ReadLock lock(mutex_);
        const auto keyIt = keyToIndex_.find(buildKey(machineCode, meterCode, pointCode));
        if (keyIt == keyToIndex_.end()) {
            return NullOpt;
        }
        index = keyIt->second;
    }
    return getLatestByIndex(index, nowMs);
}

Optional<StoredPointValue> MemoryPointStore::getLatestByIndex(
    std::uint32_t index,
    std::int64_t nowMs
) const {
    Optional<PointBinding> binding;
    Optional<std::size_t> cachedSlot;
    {
        ReadLock lock(mutex_);
        binding = getBindingByIndex(index);
        const auto cached = latestSlotByIndex_.find(index);
        if (cached != latestSlotByIndex_.end()) {
            cachedSlot = cached->second;
        }
    }

#ifdef _WIN32
    SharedLockGuard sharedLock(mutexHandle_);
#else
    auto* layout = layoutFrom(sharedView_);
    SharedLockGuard sharedLock(&layout->header.mutex);
#endif
    const auto* layout2 = layoutFrom(sharedView_);
    const SharedLatestSlot* slot = nullptr;
    if (cachedSlot && *cachedSlot < kMaxLatestSlots) {
        const auto* candidate = &layout2->latest[*cachedSlot];
        if (candidate->occupied && candidate->index == index) {
            slot = candidate;
        }
    }
    if (slot == nullptr) {
        slot = findLatestSlot(layout2, index);
    }
    if (slot == nullptr) {
        return NullOpt;
    }

    StoredPointValue value;
    value.index = slot->index;
    value.value = slot->value;
    value.quality = slot->quality;
    value.ts = slot->ts;
    value.expireAt = slot->expireAt;
    value.stale = slot->stale != 0;
    if (binding) {
        value.machineCode = binding->machineCode;
        value.meterCode = binding->meterCode;
        value.pointCode = binding->pointCode;
    }
    return markStale(value, nowMs);
}

std::vector<StoredPointValue> MemoryPointStore::getLatestByIndexes(
    const std::vector<std::uint32_t>& indexes,
    std::int64_t nowMs
) const {
    std::unordered_map<std::uint32_t, PointBinding> bindingsSnapshot;
    std::unordered_map<std::uint32_t, std::size_t> slotSnapshot;
    std::unordered_set<std::uint32_t> missingSlotIndexes;
    {
        ReadLock lock(mutex_);
        bindingsSnapshot.reserve(indexes.size());
        slotSnapshot.reserve(indexes.size());
        missingSlotIndexes.reserve(indexes.size());
        for (const auto index : indexes) {
            const auto binding = bindings_.find(index);
            if (binding != bindings_.end()) {
                bindingsSnapshot.emplace(index, binding->second);
            }
            const auto slot = latestSlotByIndex_.find(index);
            if (slot != latestSlotByIndex_.end()) {
                slotSnapshot.emplace(index, slot->second);
            } else {
                missingSlotIndexes.insert(index);
            }
        }
    }

#ifdef _WIN32
    SharedLockGuard sharedLock(mutexHandle_);
#else
    auto* layout = layoutFrom(sharedView_);
    SharedLockGuard sharedLock(&layout->header.mutex);
#endif
    const auto* layout2 = layoutFrom(sharedView_);
    const bool scannedMissingSlots = !missingSlotIndexes.empty();
    std::unordered_map<std::uint32_t, std::size_t> discoveredSlots;
    if (!missingSlotIndexes.empty()) {
        discoveredSlots.reserve(missingSlotIndexes.size());
        for (std::size_t slotIndex = 0;
             slotIndex < kMaxLatestSlots && discoveredSlots.size() < missingSlotIndexes.size();
             ++slotIndex) {
            const auto& candidate = layout2->latest[slotIndex];
            if (candidate.occupied && missingSlotIndexes.find(candidate.index) != missingSlotIndexes.end()) {
                discoveredSlots.emplace(candidate.index, slotIndex);
            }
        }
    }

    std::vector<StoredPointValue> result;
    result.reserve(indexes.size());
    for (const auto index : indexes) {
        const SharedLatestSlot* slot = nullptr;
        const auto cached = slotSnapshot.find(index);
        if (cached != slotSnapshot.end() && cached->second < kMaxLatestSlots) {
            const auto* candidate = &layout2->latest[cached->second];
            if (candidate->occupied && candidate->index == index) {
                slot = candidate;
            }
        }
        if (slot == nullptr) {
            const auto discovered = discoveredSlots.find(index);
            if (discovered != discoveredSlots.end() && discovered->second < kMaxLatestSlots) {
                const auto* candidate = &layout2->latest[discovered->second];
                if (candidate->occupied && candidate->index == index) {
                    slot = candidate;
                }
            }
        }
        if (slot == nullptr && scannedMissingSlots && missingSlotIndexes.find(index) != missingSlotIndexes.end()) {
            continue;
        }
        if (slot == nullptr) {
            slot = findLatestSlot(layout2, index);
        }
        if (slot == nullptr) {
            continue;
        }
        StoredPointValue value;
        value.index = slot->index;
        value.value = slot->value;
        value.quality = slot->quality;
        value.ts = slot->ts;
        value.expireAt = slot->expireAt;
        value.stale = slot->stale != 0;
        const auto binding = bindingsSnapshot.find(value.index);
        if (binding != bindingsSnapshot.end()) {
            value.machineCode = binding->second.machineCode;
            value.meterCode = binding->second.meterCode;
            value.pointCode = binding->second.pointCode;
        }
        result.push_back(markStale(value, nowMs));
    }
    return result;
}

std::vector<StoredPointValue> MemoryPointStore::getAllLatest(std::int64_t nowMs) const {
    std::unordered_map<std::uint32_t, PointBinding> bindingsSnapshot;
    {
        ReadLock lock(mutex_);
        bindingsSnapshot = bindings_;
    }

#ifdef _WIN32
    SharedLockGuard sharedLock(mutexHandle_);
#else
    auto* layout = layoutFrom(sharedView_);
    SharedLockGuard sharedLock(&layout->header.mutex);
#endif
    const auto* layout2 = layoutFrom(sharedView_);
    std::unordered_map<std::uint32_t, StoredPointValue> merged;
    merged.reserve(layout2->header.latestCount);
    for (const auto& slot : layout2->latest) {
        if (!slot.occupied) {
            continue;
        }
        StoredPointValue value;
        value.index = slot.index;
        value.value = slot.value;
        value.quality = slot.quality;
        value.ts = slot.ts;
        value.expireAt = slot.expireAt;
        value.stale = slot.stale != 0;
        const auto binding = bindingsSnapshot.find(value.index);
        if (binding != bindingsSnapshot.end()) {
            value.machineCode = binding->second.machineCode;
            value.meterCode = binding->second.meterCode;
            value.pointCode = binding->second.pointCode;
        }
        value = markStale(value, nowMs);

        const auto existing = merged.find(value.index);
        if (existing == merged.end() ||
            value.ts > existing->second.ts ||
            (value.ts == existing->second.ts && value.expireAt > existing->second.expireAt)) {
            merged[value.index] = value;
        }
    }

    std::vector<StoredPointValue> result;
    result.reserve(merged.size());
    for (const auto& entry : merged) {
        result.push_back(entry.second);
    }
    std::sort(result.begin(), result.end(), [](const StoredPointValue& lhs, const StoredPointValue& rhs) {
        return lhs.index < rhs.index;
    });
    return result;
}

std::vector<PointLeaseStatus> MemoryPointStore::getAllLeaseStatus(std::int64_t nowMs) const {
#ifdef _WIN32
    SharedLockGuard sharedLock(mutexHandle_);
#else
    auto* layout = layoutFrom(sharedView_);
    SharedLockGuard sharedLock(&layout->header.mutex);
#endif
    auto* layout2 = layoutFrom(sharedView_);
    cleanupStaleOwnersAndClaims(layout2, nowMs);

    std::unordered_map<std::uint32_t, PointLeaseStatus> merged;
    for (const auto& slot : layout2->latest) {
        if (!slot.occupied) {
            continue;
        }
        merged[slot.index].index = slot.index;
    }
    for (const auto& claim : layout2->claims) {
        if (!claim.occupied) {
            continue;
        }
        auto& item = merged[claim.index];
        item.index = claim.index;
        ++item.ownerCount;
        item.lastClaimTs = std::max(item.lastClaimTs, claim.heartbeatMs);
        for (const auto& owner : layout2->owners) {
            if (owner.occupied && owner.ownerId == claim.ownerId && isOwnerAlive(owner, nowMs)) {
                item.hasActiveOwner = true;
                break;
            }
        }
    }

    std::vector<PointLeaseStatus> result;
    result.reserve(merged.size());
    for (const auto& entry : merged) {
        result.push_back(entry.second);
    }
    std::sort(result.begin(), result.end(), [](const PointLeaseStatus& lhs, const PointLeaseStatus& rhs) {
        return lhs.index < rhs.index;
    });
    return result;
}

std::vector<StoredPointValue> MemoryPointStore::getDeviceLatest(
    const std::string& machineCode,
    const std::string& meterCode,
    std::int64_t nowMs
) const {
    std::vector<std::uint32_t> indexes;
    {
        ReadLock lock(mutex_);
        for (const auto& entry : bindings_) {
            const auto& binding = entry.second;
            if (binding.machineCode == machineCode && binding.meterCode == meterCode) {
                indexes.push_back(entry.first);
            }
        }
    }

    std::vector<StoredPointValue> result;
    result.reserve(indexes.size());
    for (const auto index : indexes) {
        const auto latest = getLatestByIndex(index, nowMs);
        if (latest) {
            result.push_back(*latest);
        }
    }
    return result;
}

void MemoryPointStore::submitWriteCommand(const PendingWriteCommand& command) {
    if (command.index == 0) {
        throw std::invalid_argument("pending write index must be non-zero");
    }

#ifdef _WIN32
    SharedLockGuard sharedLock(mutexHandle_);
#else
    auto* layout = layoutFrom(sharedView_);
    SharedLockGuard sharedLock(&layout->header.mutex);
#endif
    auto* layout2 = layoutFrom(sharedView_);
    pushPendingWrite(layout2, command, maxPendingWrites_);
}

std::vector<PendingWriteCommand> MemoryPointStore::drainPendingWriteCommands(std::size_t limit) {
    std::set<std::uint32_t> registeredIndexes;
    {
        ReadLock lock(mutex_);
        registeredIndexes = registeredIndexes_;
    }

#ifdef _WIN32
    SharedLockGuard sharedLock(mutexHandle_);
#else
    auto* layout = layoutFrom(sharedView_);
    SharedLockGuard sharedLock(&layout->header.mutex);
#endif
    auto* layout2 = layoutFrom(sharedView_);
    return drainPendingWrites(layout2, limit, registeredIndexes.empty() ? nullptr : &registeredIndexes);
}

std::vector<PendingWriteCommand> MemoryPointStore::peekPendingWriteCommands(std::size_t limit) const {
#ifdef _WIN32
    SharedLockGuard sharedLock(mutexHandle_);
#else
    auto* layout = layoutFrom(sharedView_);
    SharedLockGuard sharedLock(&layout->header.mutex);
#endif
    const auto* layout2 = layoutFrom(sharedView_);
    return peekPendingWrites(layout2, limit);
}

MemoryStoreStats MemoryPointStore::getStats() const {
#ifdef _WIN32
    SharedLockGuard sharedLock(mutexHandle_);
#else
    auto* layout = layoutFrom(sharedView_);
    SharedLockGuard sharedLock(&layout->header.mutex);
#endif
    const auto* layout2 = layoutFrom(sharedView_);
    MemoryStoreStats stats;
    stats.sharedMemoryName = segmentName_;
    stats.latestCount = layout2->header.latestCount;
    stats.latestCapacity = kMaxLatestSlots;
    stats.latestConfiguredLimit = maxLatestPoints_;
    stats.pendingWriteCount = ringCount(
        layout2->header.pendingWriteHead,
        layout2->header.pendingWriteTail,
        kMaxPendingWriteSlots
    );
    stats.pendingWriteCapacity = kMaxPendingWriteSlots - 1;
    stats.pendingWriteConfiguredLimit = maxPendingWrites_;
    stats.persistentCount = ringCount(
        layout2->header.persistentHead,
        layout2->header.persistentTail,
        kMaxPersistentSlots
    );
    stats.persistentCapacity = kMaxPersistentSlots - 1;
    stats.persistentConfiguredLimit = maxPersistentSamples_;
    stats.pointUpdateCount = ringCount(
        layout2->header.pointUpdateHead,
        layout2->header.pointUpdateTail,
        kMaxPointUpdateSlots
    );
    stats.pointUpdateCapacity = kMaxPointUpdateSlots - 1;
    stats.writeSequence = layout2->header.writeSequence;
    stats.persistentSequence = layout2->header.persistentSequence;
    stats.pointUpdateSequence = layout2->header.pointUpdateSequence;
    return stats;
}

std::vector<PersistentPointSample> MemoryPointStore::drainPersistentSamples() {
#ifdef _WIN32
    SharedLockGuard sharedLock(mutexHandle_);
#else
    auto* layout = layoutFrom(sharedView_);
    SharedLockGuard sharedLock(&layout->header.mutex);
#endif
    auto* layout2 = layoutFrom(sharedView_);
    return drainPersistent(layout2);
}

std::vector<PointUpdateRecord> MemoryPointStore::drainPointUpdates(std::size_t limit) {
#ifdef _WIN32
    SharedLockGuard sharedLock(mutexHandle_);
#else
    auto* layout = layoutFrom(sharedView_);
    SharedLockGuard sharedLock(&layout->header.mutex);
#endif
    auto* layout2 = layoutFrom(sharedView_);
    return edge_gateway::drainPointUpdates(layout2, limit);
}

void MemoryPointStore::heartbeatRegisteredPoints(std::int64_t nowMs) {
    ReadLock lock(mutex_);
#ifdef _WIN32
    SharedLockGuard sharedLock(mutexHandle_);
#else
    auto* layout = layoutFrom(sharedView_);
    SharedLockGuard sharedLock(&layout->header.mutex);
#endif
    auto* layout2 = layoutFrom(sharedView_);
    cleanupStaleOwnersAndClaims(layout2, nowMs);
    auto* ownerSlot = allocateOwnerSlot(layout2, ownerId_);
    ownerSlot->heartbeatMs = nowMs;
    copyString(ownerSlot->source, kSourceSize, ownerSource_);
    for (const auto index : registeredIndexes_) {
        auto* claimSlot = allocateClaimSlot(layout2, index, ownerId_);
        claimSlot->heartbeatMs = nowMs;
    }
}

void MemoryPointStore::removeExpired(std::int64_t nowMs) {
    WriteLock lock(mutex_);
#ifdef _WIN32
    SharedLockGuard sharedLock(mutexHandle_);
#else
    auto* layout = layoutFrom(sharedView_);
    SharedLockGuard sharedLock(&layout->header.mutex);
#endif
    auto* layout2 = layoutFrom(sharedView_);
    cleanupStaleOwnersAndClaims(layout2, nowMs);
    for (auto& slot : layout2->latest) {
        if (!slot.occupied) {
            continue;
        }
        StoredPointValue value;
        value.index = slot.index;
        value.value = slot.value;
        value.quality = slot.quality;
        value.ts = slot.ts;
        value.expireAt = slot.expireAt;
        if (isExpired(value, nowMs) || !hasActiveClaim(layout2, slot.index, nowMs)) {
            latestSlotByIndex_.erase(slot.index);
            slot = SharedLatestSlot{};
            if (layout2->header.latestCount > 0) {
                --layout2->header.latestCount;
            }
        }
    }
}

std::string MemoryPointStore::buildKey(
    const std::string& machineCode,
    const std::string& meterCode,
    const std::string& pointCode
) {
    return machineCode + ":" + meterCode + ":" + pointCode;
}

Optional<PointBinding> MemoryPointStore::getBindingByIndex(std::uint32_t index) const {
    const auto it = bindings_.find(index);
    if (it == bindings_.end()) {
        return NullOpt;
    }
    return it->second;
}

bool MemoryPointStore::isExpired(const StoredPointValue& value, std::int64_t nowMs) {
    return value.expireAt > 0 && nowMs > value.expireAt;
}

StoredPointValue MemoryPointStore::markStale(StoredPointValue value, std::int64_t nowMs) {
    value.stale = isExpired(value, nowMs);
    return value;
}

}  // namespac
