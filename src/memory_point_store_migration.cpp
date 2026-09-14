#include "memory_point_store_migration.hpp"
#include "memory_point_store_layout.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <map>

namespace edge_gateway {
namespace {
using namespace memory_layout;

struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) {
        if (fd < 0) throw std::runtime_error(std::string("open failed: ") + std::strerror(errno));
    }
    ~Fd() { close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void readExact(int fd, void* buffer, std::size_t size) {
    auto* bytes = static_cast<char*>(buffer);
    for (std::size_t offset = 0; offset < size;) {
        const auto count = pread(fd, bytes + offset, size - offset, offset);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "short/failed read; segment left in place");
        offset += static_cast<std::size_t>(count);
    }
}

void writeExact(int fd, const void* buffer, std::size_t size, off_t base = 0) {
    const auto* bytes = static_cast<const char*>(buffer);
    for (std::size_t offset = 0; offset < size;) {
        const auto count = pwrite(fd, bytes + offset, size - offset, base + offset);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "short/failed write; keep services stopped and inspect segment and backup");
        offset += static_cast<std::size_t>(count);
    }
}

void assertNoMappings(const struct stat& segment) {
    const auto closeDirectory = [](DIR* directory) { closedir(directory); };
    std::unique_ptr<DIR, decltype(closeDirectory)> proc(opendir("/proc"), closeDirectory);
    require(static_cast<bool>(proc), "cannot inspect /proc");
    for (;;) {
        errno = 0;
        auto* entry = readdir(proc.get());
        if (!entry) {
            require(errno == 0, "cannot enumerate /proc completely");
            break;
        }
        const std::string pid(entry->d_name);
        if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos) continue;
        const std::string path = "/proc/" + pid + "/maps";
        errno = 0;
        std::ifstream maps(path);
        if (!maps) {
            if (errno == ENOENT || errno == ESRCH) continue;
            throw std::runtime_error("cannot inspect " + path + "; offline state is unverified");
        }
        std::string line;
        while (std::getline(maps, line)) {
            std::istringstream fields(line);
            std::string range, permissions, offset, device;
            unsigned long long inode = 0;
            require(static_cast<bool>(fields >> range >> permissions >> offset >> device >> inode),
                    "invalid /proc maps entry");
            unsigned int majorNumber = 0, minorNumber = 0;
            char extra = 0;
            require(std::sscanf(device.c_str(), "%x:%x%c", &majorNumber, &minorNumber, &extra) == 2,
                    "invalid /proc maps device");
            if (inode == segment.st_ino && makedev(majorNumber, minorNumber) == segment.st_dev)
                throw std::runtime_error("segment still mapped by PID " + pid);
        }
        require(!maps.bad(), "failed reading /proc maps");
    }
}

OfflineMigrationResult validateOfflineLayout(const SharedStoreLayout& layout, bool deduplicate) {
    OfflineMigrationResult plan;
    const auto& h = layout.header;
    require(h.magic == kSharedStoreMagic, "invalid SHM magic");
    require(h.version == 8 || h.version == 9, "migration requires SHM v8 or v9");
    require(h.latestCount <= kMaxLatestSlots &&
        h.pendingWriteHead < kMaxPendingWriteSlots && h.pendingWriteTail < kMaxPendingWriteSlots &&
        h.writebackResultHead < kMaxWritebackResultSlots && h.writebackResultTail < kMaxWritebackResultSlots &&
        h.persistentHead < kMaxPersistentSlots && h.persistentTail < kMaxPersistentSlots &&
        h.pointUpdateHead < kMaxPointUpdateSlots && h.pointUpdateTail < kMaxPointUpdateSlots,
        "invalid SHM counts or ring cursors");
    require(h.pendingWriteHead == h.pendingWriteTail, "pending writes: migration refused; never replay automatically");
    for (const auto& write : layout.pendingWrites)
        require(write.occupied == 0, "occupied pending write: migration refused; never replay automatically");

    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (const auto& owner : layout.owners) {
        require(owner.occupied <= 1, "invalid owner occupancy");
        require(!owner.occupied || owner.heartbeatMs <= 0 || owner.heartbeatMs < now - kOwnerLeaseMs,
                "active or future owner lease: wait for expiry with all services stopped");
    }
    for (const auto& claim : layout.claims) {
        require(claim.occupied <= 1, "invalid claim occupancy");
        require(!claim.occupied || claim.heartbeatMs <= 0 || claim.heartbeatMs < now - kOwnerLeaseMs,
                "active or future point claim: wait for expiry with all services stopped");
    }
    std::map<std::uint32_t, std::vector<std::uint32_t>> indexes;
    for (std::uint32_t i = 0; i < kMaxLatestSlots; ++i) {
        const auto& latest = layout.latest[i];
        require(latest.occupied <= 1, "invalid latest occupancy");
        if (latest.occupied) {
            indexes[latest.index].push_back(i);
            ++plan.occupiedBefore;
        }
    }
    require(plan.occupiedBefore == h.latestCount, "latest count mismatch");
    plan.oldVersion = h.version;
    plan.occupiedAfter = static_cast<std::uint32_t>(indexes.size());
    for (const auto& entry : indexes) {
        const auto& slots = entry.second;
        if (slots.size() == 1) continue;
        require(deduplicate, "duplicate latest index; explicit --deduplicate-latest required");
        LatestDuplicateGroup group;
        group.index = entry.first;
        group.winnerSlot = slots.front();
        for (auto slot : slots)
            if (layout.latest[slot].ts > layout.latest[group.winnerSlot].ts) group.winnerSlot = slot;
        const auto& winner = layout.latest[group.winnerSlot];
        for (auto slot : slots) {
            if (slot == group.winnerSlot) continue;
            const auto& other = layout.latest[slot];
            if (other.ts == winner.ts &&
                !(std::memcmp(&other.value, &winner.value, sizeof(winner.value)) == 0 &&
                  other.quality == winner.quality && other.expireAt == winner.expireAt &&
                  other.stale == winner.stale))
                throw std::runtime_error("conflicting highest-ts latest samples for index " +
                    std::to_string(group.index) + "; migration refused");
            group.removedSlots.push_back(slot);
            ++plan.removedCount;
        }
        plan.duplicateGroups.push_back(std::move(group));
    }

    // Conservative native-ABI check: never lock/recover the original mutex, since
    // the runtime's EOWNERDEAD recovery clears payload. Unknown representations fail closed.
    pthread_mutex_t clean{};
    pthread_mutexattr_t attr{};
    require(pthread_mutexattr_init(&attr) == 0, "cannot initialize mutex attributes");
    const auto shared = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    const auto robust = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    const auto initialized = (shared == 0 && robust == 0) ? pthread_mutex_init(&clean, &attr) : -1;
    pthread_mutexattr_destroy(&attr);
    require(initialized == 0, "cannot create reference mutex");
    const bool pristine = std::memcmp(&clean, &h.mutex, sizeof(clean)) == 0;
    // libc can retain a recursion/count field after a normal lock/unlock cycle.
    const auto locked = pthread_mutex_lock(&clean);
    const auto unlocked = locked == 0 ? pthread_mutex_unlock(&clean) : -1;
    const bool usedAndUnlocked = unlocked == 0 && std::memcmp(&clean, &h.mutex, sizeof(clean)) == 0;
    pthread_mutex_destroy(&clean);
    require(pristine || usedAndUnlocked, "mutex is locked, abandoned, damaged or unsupported by this native ABI; migration refused");
    return plan;
}

}  // namespace

std::uint32_t migrateOfflinePointStore(
    const std::string& segmentName, const std::string& backupPath, bool offlineConfirmed) {
    return migrateOfflinePointStore(segmentName, backupPath, offlineConfirmed, {}).oldVersion;
}

OfflineMigrationResult migrateOfflinePointStore(
    const std::string& segmentName, const std::string& backupPath, bool offlineConfirmed,
    const OfflineMigrationOptions& options) {
    require(offlineConfirmed, "explicit --offline-confirmed required; stop all participants and disable restarts first");
    require(geteuid() == 0, "run as root in the host PID/mount namespaces to inspect all participants");
    std::string name = segmentName;
    if (!name.empty() && name.front() == '/') name.erase(0, 1);
    require(!name.empty() && name != "." && name != ".." &&
        name.find('/') == std::string::npos && name.find('\0') == std::string::npos,
        "invalid segment name");
    require(options.checkOnly || (!backupPath.empty() && backupPath.front() == '/' && backupPath.back() != '/' &&
        backupPath.find('\0') == std::string::npos), "backup requires an absolute regular file path");
    Fd source(shm_open(("/" + name).c_str(), (options.checkOnly ? O_RDONLY : O_RDWR) | O_CLOEXEC | O_NOFOLLOW, 0));
    require(flock(source.value, LOCK_EX | LOCK_NB) == 0, "segment is locked by another opener/migration");
    struct stat original{};
    require(fstat(source.value, &original) == 0 && S_ISREG(original.st_mode) &&
        original.st_size == static_cast<off_t>(sizeof(SharedStoreLayout)), "invalid SHM type/size for this native ABI");
    assertNoMappings(original);
    auto before = std::make_unique<SharedStoreLayout>();
    readExact(source.value, before.get(), sizeof(*before));
    const auto plan = validateOfflineLayout(*before, options.deduplicateLatest);
    if (options.checkOnly) return plan;

    const auto separator = backupPath.find_last_of('/');
    Fd parent(open((separator == 0 ? "/" : backupPath.substr(0, separator)).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    struct statfs filesystem{};
    require(fstatfs(parent.value, &filesystem) == 0, "cannot inspect backup filesystem");
    require(filesystem.f_type == 0xef53 || filesystem.f_type == 0x58465342 || filesystem.f_type == 0x9123683e,
            "backup requires local persistent storage (ext4/ext-family, XFS or Btrfs); other filesystems refused");
    const auto filename = backupPath.substr(separator + 1);
    Fd backup(openat(parent.value, filename.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    writeExact(backup.value, before.get(), sizeof(*before));
    require(fsync(backup.value) == 0 && fsync(parent.value) == 0, "backup durability failed; original segment unchanged");
    auto check = std::make_unique<SharedStoreLayout>();
    readExact(backup.value, check.get(), sizeof(*check));
    require(std::memcmp(before.get(), check.get(), sizeof(*before)) == 0, "backup verification failed; original segment unchanged");

    Fd named(shm_open(("/" + name).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0));
    struct stat current{};
    require(fstat(named.value, &current) == 0 && original.st_dev == current.st_dev &&
        original.st_ino == current.st_ino && original.st_size == current.st_size,
        "segment replaced/resized during backup; original left in place");
    assertNoMappings(original);
    readExact(source.value, check.get(), sizeof(*check));
    require(std::memcmp(before.get(), check.get(), sizeof(*before)) == 0,
            "segment changed during backup; migration refused");
    // Apply only the approved occupancy bytes and count, then publish v10 last.
    // The full expected image below verifies every other byte, including padding.
    for (const auto& group : plan.duplicateGroups) {
        for (auto slot : group.removedSlots) {
            before->latest[slot].occupied = 0;
            writeExact(source.value, &before->latest[slot].occupied, sizeof(before->latest[slot].occupied),
                offsetof(SharedStoreLayout, latest) + slot * sizeof(SharedLatestSlot) +
                offsetof(SharedLatestSlot, occupied));
        }
    }
    if (plan.removedCount) {
        before->header.latestCount = plan.occupiedAfter;
        writeExact(source.value, &before->header.latestCount, sizeof(before->header.latestCount),
            offsetof(SharedStoreHeader, latestCount));
    }
    const std::uint32_t version = 10;
    writeExact(source.value, &version, sizeof(version), offsetof(SharedStoreHeader, version));
    require(fsync(source.value) == 0, "SHM sync failed; keep services stopped and inspect backup/segment");
    before->header.version = version;
    readExact(source.value, check.get(), sizeof(*check));
    require(std::memcmp(before.get(), check.get(), sizeof(*before)) == 0,
            "post-migration verification failed; keep services stopped and inspect backup/segment");
    return plan;
}

}  // namespace edge_gateway
