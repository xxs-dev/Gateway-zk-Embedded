#include "../src/memory_point_store_layout.hpp"
#include "../src/memory_point_store_layout_v10.hpp"
#include "../src/memory_point_store_migration.hpp"
#include "edge_gateway/memory_point_store.hpp"

#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <memory>
#include <limits>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace edge_gateway;
using namespace edge_gateway::memory_layout_v10;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    std::string name;
    std::string backup;
    int fd = -1;
    explicit Fixture(std::uint32_t version) {
        static int next = 0;
        name = "gateway_migration_test_" + std::to_string(getpid()) + "_" + std::to_string(++next);
        const char* directory = std::getenv("GATEWAY_MIGRATION_TEST_BACKUP_DIR");
        require(directory && *directory, "set GATEWAY_MIGRATION_TEST_BACKUP_DIR to an isolated persistent directory");
        backup = std::string(directory) + "/" + name + ".bak";
        // Frozen legacy fixture; the v11 runtime must never create or attach v8/v9/v10.
        fd = shm_open(("/" + name).c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        require(fd >= 0, "open test segment");
        require(ftruncate(fd, sizeof(SharedStoreLayout)) == 0, "legacy fixture size");
        auto layout = std::make_unique<SharedStoreLayout>();
        layout->header.magic = kSharedStoreMagic;
        layout->header.version = version;
        pthread_mutexattr_t attr{};
        require(pthread_mutexattr_init(&attr) == 0, "mutex attr");
        require(pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0, "mutex shared");
        require(pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0, "mutex robust");
        require(pthread_mutex_init(&layout->header.mutex, &attr) == 0, "mutex init");
        pthread_mutexattr_destroy(&attr);
        layout->latest[0].index = 1001;
        layout->latest[0].value = 42.25;
        layout->latest[0].ts = 1000;
        layout->latest[0].expireAt = 9999999999999;
        layout->latest[0].occupied = 1;
        layout->persistent[0].index = 1001;
        layout->persistent[0].value = 42.25;
        layout->persistent[0].ts = 1000;
        layout->persistent[0].occupied = 1;
        // A real offline placeholder and expired ownership survive byte-for-byte.
        layout->latest[1].index = 1002;
        layout->latest[1].occupied = 1;
        layout->latest[1].quality = 0;
        layout->latest[1].stale = 1;
        layout->latest[1].ts = 0;
        layout->header.latestCount = 2;
        layout->owners[0].ownerId = 77;
        layout->owners[0].occupied = 1;
        layout->owners[0].heartbeatMs = 1000;
        std::strcpy(layout->owners[0].source, "retired-interface");
        layout->claims[0].occupied = 1;
        layout->claims[0].index = 1002;
        layout->claims[0].ownerId = 77;
        layout->claims[0].heartbeatMs = 1000;
        // Wrapped history ring and update/result cursors exercise nonzero offsets.
        layout->header.persistentHead = kMaxPersistentSlots - 1;
        layout->header.persistentTail = 1;
        layout->header.persistentSequence = 19;
        layout->persistent[kMaxPersistentSlots - 1] = layout->persistent[0];
        layout->persistent[kMaxPersistentSlots - 1].sequence = 18;
        layout->persistent[0].sequence = 19;
        layout->persistent[0].value = 43.5;
        layout->header.writebackResultHead = 9;
        layout->header.writebackResultTail = 10;
        layout->header.writebackResultSequence = 5;
        layout->writebackResults[9].occupied = 1;
        layout->writebackResults[9].sequence = 5;
        std::strcpy(layout->writebackResults[9].cmdId, "completed-before-migration");
        layout->writebackResults[9].success = 1;
        save(*layout);
    }
    ~Fixture() {
        if (fd >= 0) close(fd);
        shm_unlink(("/" + name).c_str());
        unlink(backup.c_str());
        shm_unlink(("/" + name + "_v11").c_str());
        unlink((backup + ".v11").c_str());
    }
    std::unique_ptr<SharedStoreLayout> read() const {
        auto layout = std::make_unique<SharedStoreLayout>();
        require(pread(fd, layout.get(), sizeof(*layout), 0) == sizeof(*layout), "read real layout");
        return layout;
    }
    void save(const SharedStoreLayout& layout) {
        require(pwrite(fd, &layout, sizeof(layout), 0) == sizeof(layout), "write real layout fixture");
    }
    std::string copyToCurrent() {
        copyOfflinePointStoreV10ToV11(name, name + "_v11", backup + ".v11", true);
        return name + "_v11";
    }
};

struct LegacyMapping {
    void* view;
    explicit LegacyMapping(int fd) : view(mmap(nullptr, sizeof(SharedStoreLayout), PROT_READ, MAP_SHARED, fd, 0)) {
        require(view != MAP_FAILED, "map frozen legacy fixture");
    }
    ~LegacyMapping() { munmap(view, sizeof(SharedStoreLayout)); }
};

void refusal(Fixture& fixture, const std::string& expected, const std::function<void()>& action) {
    auto before = fixture.read();
    bool refused = false;
    try { action(); }
    catch (const std::exception& error) {
        refused = true;
        require(std::string(error.what()).find(expected) != std::string::npos,
                "unexpected refusal: " + std::string(error.what()));
    }
    require(refused, "expected migration refusal: " + expected);
    auto after = fixture.read();
    require(std::memcmp(before.get(), after.get(), sizeof(*before)) == 0, "refusal changed original bytes");
    int named = shm_open(("/" + fixture.name).c_str(), O_RDONLY, 0);
    require(named >= 0, "refusal removed segment");
    close(named);
}

void success(std::uint32_t version) {
    Fixture f(version);
    auto before = f.read();
    struct stat original{};
    fstat(f.fd, &original);
    require(!MemoryPointStore::cleanupOrphanedSegment(f.name), "legacy cleanup must preserve original segment");
    require(migrateOfflinePointStore(f.name, f.backup, true) == version, "wrong original version");
    int backup = open(f.backup.c_str(), O_RDONLY);
    require(backup >= 0, "backup missing");
    auto archived = std::make_unique<SharedStoreLayout>();
    require(pread(backup, archived.get(), sizeof(*archived), 0) == sizeof(*archived), "backup size");
    struct stat backupStat{};
    fstat(backup, &backupStat);
    close(backup);
    require((backupStat.st_mode & 0777) == 0600, "backup permission");
    require(std::memcmp(before.get(), archived.get(), sizeof(*before)) == 0, "backup not exact original");
    before->header.version = 10;
    auto after = f.read();
    require(std::memcmp(before.get(), after.get(), sizeof(*before)) == 0, "migration changed payload or mutex");
    int named = shm_open(("/" + f.name).c_str(), O_RDONLY, 0);
    struct stat migrated{};
    fstat(named, &migrated);
    close(named);
    require(original.st_ino == migrated.st_ino && original.st_dev == migrated.st_dev, "segment recreated");
    refusal(f, "requires SHM v8 or v9", [&] { migrateOfflinePointStore(f.name, f.backup, true); });
    {
        MemoryStoreConfig restarted;
        restarted.sharedMemoryName = f.copyToCurrent();
        MemoryPointStore reader(restarted);
        const auto latest = reader.getLatestByIndex(1001, 2000);
        require(latest && latest->value == 42.25, "latest value lost");
        const auto placeholder = reader.getLatestByIndex(1002, 2000);
        require(placeholder && placeholder->ts == 0, "offline placeholder lost");
        const auto samples = reader.peekPersistentSamples();
        require(samples.size() == 2 && samples[0].sequence == 18 && samples[1].sequence == 19 &&
                samples[1].value == 43.5, "wrapped history lost/reordered");
        const auto result = reader.getWritebackResult("completed-before-migration");
        require(result && result->success, "completed writeback lost");
    }
    std::cout << "PASS v" << version << " preservation, runtime reopen, no unlink and repeated migration refusal\n";
}

void rejectionCases() {
    Fixture f(9);
    const auto migrate = [&] { migrateOfflinePointStore(f.name, f.backup, true); };
    refusal(f, "offline-confirmed", [&] { migrateOfflinePointStore(f.name, f.backup, false); });
    require(flock(f.fd, LOCK_EX | LOCK_NB) == 0, "lock fixture");
    refusal(f, "locked by", migrate);
    require(flock(f.fd, LOCK_UN) == 0, "unlock fixture");
    {
        LegacyMapping reader(f.fd);
        refusal(f, "still mapped", migrate);
    }
    const auto valid = f.read();
    const auto corrupt = [&](const std::string& reason, const std::function<void(SharedStoreLayout&)>& modify) {
        auto changed = f.read();
        modify(*changed);
        f.save(*changed);
        refusal(f, reason, migrate);
        f.save(*valid);
    };
    corrupt("magic", [](auto& x) { x.header.magic = 0; });
    corrupt("v8 or v9", [](auto& x) { x.header.version = 7; });
    corrupt("ring cursors", [](auto& x) { x.header.persistentTail = kMaxPersistentSlots; });
    corrupt("pending writes", [](auto& x) { x.header.pendingWriteTail = 1; });
    corrupt("occupied pending write", [](auto& x) { x.pendingWrites[7].occupied = 1; });
    corrupt("owner lease", [](auto& x) { x.owners[0].occupied = 1; x.owners[0].heartbeatMs = INT64_MAX; });
    corrupt("point claim", [](auto& x) { x.claims[0].occupied = 1; x.claims[0].heartbeatMs = INT64_MAX; });
    corrupt("duplicate latest", [](auto& x) { x.latest[1].index = x.latest[0].index; });
    corrupt("count mismatch", [](auto& x) { x.header.latestCount = 1; });
    corrupt("mutex", [](auto& x) { std::memset(&x.header.mutex, 0, sizeof(x.header.mutex)); });
    refusal(f, "open failed", [&] { migrateOfflinePointStore(f.name, f.backup + "/missing/file", true); });
    const auto tmpfsBackup = "/dev/shm/" + f.name + ".bak";
    refusal(f, "persistent storage", [&] { migrateOfflinePointStore(f.name, tmpfsBackup, true); });
    require(symlink("/dev/null", f.backup.c_str()) == 0, "create backup symlink fixture");
    refusal(f, "open failed", migrate);
    unlink(f.backup.c_str());
    const int existing = open(f.backup.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    require(existing >= 0, "create existing backup");
    require(write(existing, "KEEP", 4) == 4, "seed existing backup");
    close(existing);
    refusal(f, "open failed", migrate);
    char bytes[4]{};
    int check = open(f.backup.c_str(), O_RDONLY);
    require(read(check, bytes, 4) == 4 && std::memcmp(bytes, "KEEP", 4) == 0, "overwrote existing backup");
    close(check);
    unlink(f.backup.c_str());
    // A partial backup write must retain the original and leave evidence, not proceed.
    struct rlimit originalLimit{};
    require(getrlimit(RLIMIT_FSIZE, &originalLimit) == 0, "read file size limit");
    auto smallLimit = originalLimit;
    smallLimit.rlim_cur = 4096;
    const auto oldSignal = std::signal(SIGXFSZ, SIG_IGN);
    require(setrlimit(RLIMIT_FSIZE, &smallLimit) == 0, "set file size limit");
    try {
        refusal(f, "short/failed write", migrate);
    } catch (...) {
        setrlimit(RLIMIT_FSIZE, &originalLimit);
        std::signal(SIGXFSZ, oldSignal);
        throw;
    }
    require(setrlimit(RLIMIT_FSIZE, &originalLimit) == 0, "restore file size limit");
    std::signal(SIGXFSZ, oldSignal);
    struct stat partial{};
    require(stat(f.backup.c_str(), &partial) == 0 && partial.st_size == 4096,
            "partial backup evidence missing");
    unlink(f.backup.c_str());
    require(ftruncate(f.fd, sizeof(SharedStoreLayout) + 1) == 0, "resize fixture");
    refusal(f, "type/size", migrate);
    require(ftruncate(f.fd, sizeof(SharedStoreLayout)) == 0, "restore fixture size");

    // Abandoned robust mutex must not trigger the normal runtime recovery that clears payload.
    const auto child = fork();
    require(child >= 0, "fork");
    if (child == 0) {
        auto* mapped = static_cast<SharedStoreLayout*>(mmap(nullptr, sizeof(SharedStoreLayout),
            PROT_READ | PROT_WRITE, MAP_SHARED, f.fd, 0));
        if (mapped == MAP_FAILED) _exit(2);
        if (pthread_mutex_lock(&mapped->header.mutex) != 0) _exit(3);
        _exit(0);
    }
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "abandoned mutex fixture");
    refusal(f, "mutex", migrate);
    std::cout << "PASS fail-closed cases, exact original bytes retained\n";
}

void deduplication(std::uint32_t version) {
    Fixture f(version);
    auto before = f.read();
    struct stat original{};
    require(fstat(f.fd, &original) == 0, "stat original");
    before->latest[5] = before->latest[0];
    before->latest[5].ts += 1;
    before->latest[7] = before->latest[5];
    before->latest[7].reserved[0] = 99;
    before->header.latestCount = 4;
    f.save(*before);
    refusal(f, "duplicate latest", [&] { migrateOfflinePointStore(f.name, f.backup, true); });
    OfflineMigrationOptions options;
    options.checkOnly = true;
    options.deduplicateLatest = true;
    const auto plan = migrateOfflinePointStore(f.name, "", true, options);
    require(plan.oldVersion == version && plan.occupiedBefore == 4 && plan.occupiedAfter == 2 &&
        plan.duplicateGroups.size() == 1 && plan.removedCount == 2, "incorrect dedup summary");
    const auto& group = plan.duplicateGroups[0];
    require(group.index == 1001 && group.winnerSlot == 5 &&
        group.removedSlots == std::vector<std::uint32_t>({0, 7}), "incorrect original winner/loser slots");
    auto checked = f.read();
    require(std::memcmp(before.get(), checked.get(), sizeof(*before)) == 0 &&
        access(f.backup.c_str(), F_OK) != 0, "check wrote bytes or backup");
    options.checkOnly = false;
    const auto result = migrateOfflinePointStore(f.name, f.backup, true, options);
    require(result.removedCount == plan.removedCount, "execute differs from preflight");
    int backup = open(f.backup.c_str(), O_RDONLY);
    require(backup >= 0, "dedup backup missing");
    require(pread(backup, checked.get(), sizeof(*checked), 0) == sizeof(*checked), "dedup backup size");
    close(backup);
    require(std::memcmp(before.get(), checked.get(), sizeof(*before)) == 0, "dedup backup mismatch");
    before->header.version = 10;
    before->header.latestCount = 2;
    before->latest[0].occupied = 0;
    before->latest[7].occupied = 0;
    checked = f.read();
    require(std::memcmp(before.get(), checked.get(), sizeof(*before)) == 0, "dedup changed forbidden bytes");
    int named = shm_open(("/" + f.name).c_str(), O_RDONLY, 0);
    struct stat after{};
    require(named >= 0 && fstat(named, &after) == 0 && original.st_ino == after.st_ino &&
        original.st_dev == after.st_dev, "dedup replaced inode");
    close(named);
    refusal(f, "v8 or v9", [&] { migrateOfflinePointStore(f.name, f.backup, true, options); });
    {
        MemoryStoreConfig config;
        config.sharedMemoryName = f.copyToCurrent();
        MemoryPointStore writer(config);
        MemoryPointStore second(config);
        MemoryPointStore reader(config.sharedMemoryName, MemoryStoreOpenMode::OpenExisting);
        auto value = reader.getLatestByIndex(1001, 2000);
        require(value && value->ts == 1001 && value->value == 42.25, "dedup runtime winner lost");
        PointValue update;
        update.index = 1001;
        update.ts = 2000;
        update.value = 88;
        writer.putLatest(update);
        update.ts = 2001;
        second.putLatest(update);
        require(reader.getLatestByIndex(1001, 2001)->ts == 2001, "reader cache after dedup write");
        int currentFd = shm_open(("/" + config.sharedMemoryName).c_str(), O_RDONLY, 0);
        require(currentFd >= 0, "open copied current layout");
        auto state = std::make_unique<edge_gateway::memory_layout::SharedStoreLayout>();
        require(pread(currentFd, state.get(), sizeof(*state), 0) == sizeof(*state), "read copied current layout");
        close(currentFd);
        std::uint32_t count = 0;
        for (const auto& slot : state->latest) if (slot.occupied && slot.index == 1001) ++count;
        require(count == 1 && state->header.latestCount == 2 && state->latest[5].ts == 2001,
            "runtime duplicated or moved winner");
    }
    std::cout << "PASS v" << version << " dedup preflight, original slots and exact allowed bytes\n";
}

void cliPreflight() {
    const char* cli = std::getenv("GATEWAY_MIGRATION_TEST_CLI");
    std::string sibling;
    if (!cli || !*cli) {
        char path[4096]{};
        const auto size = readlink("/proc/self/exe", path, sizeof(path) - 1);
        require(size > 0, "locate sibling CLI");
        sibling = std::string(path, size);
        sibling = sibling.substr(0, sibling.find_last_of('/') + 1) + "memory_point_store_migrate";
        cli = sibling.c_str();
    }
    Fixture good(9), bad(8);
    auto original = good.read();
    auto corrupt = bad.read();
    corrupt->latest[1].index = corrupt->latest[0].index;
    corrupt->latest[1].ts = corrupt->latest[0].ts;
    corrupt->latest[1].value = 99;
    bad.save(*corrupt);
    const auto run = [&](bool conflict) {
        const auto pid = fork();
        require(pid >= 0, "fork CLI");
        if (pid == 0) {
            execl(cli, cli, "--check", "--deduplicate-latest", "--offline-confirmed",
                "--shm", good.name.c_str(), "--shm", bad.name.c_str(), static_cast<char*>(nullptr));
            _exit(126);
        }
        int status = 0;
        require(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
            WEXITSTATUS(status) == (conflict ? 1 : 0), "CLI all-segment preflight result");
        auto current = good.read();
        require(std::memcmp(original.get(), current.get(), sizeof(*original)) == 0, "preflight changed earlier segment");
        current = bad.read();
        require(std::memcmp(corrupt.get(), current.get(), sizeof(*corrupt)) == 0, "preflight changed last segment");
        require(access(good.backup.c_str(), F_OK) != 0 && access(bad.backup.c_str(), F_OK) != 0,
            "preflight created backup");
    };
    run(true);
    corrupt->latest[1] = corrupt->latest[0];
    bad.save(*corrupt);
    run(false);
    std::cout << "PASS CLI all-segment preflight success and late conflict, zero writes/backups\n";
}

void dedupSafety() {
    Fixture f(9);
    auto valid = f.read();
    valid->latest[1] = valid->latest[0];
    const std::uint64_t nan = UINT64_C(0x7ff8000000000042);
    std::memcpy(&valid->latest[0].value, &nan, sizeof(nan));
    std::memcpy(&valid->latest[1].value, &nan, sizeof(nan));
    f.save(*valid);
    const auto rejectBoth = [&](const std::string& reason) {
        for (bool check : {true, false}) {
            OfflineMigrationOptions options;
            options.checkOnly = check;
            options.deduplicateLatest = true;
            refusal(f, reason, [&] { migrateOfflinePointStore(f.name, f.backup, true, options); });
            require(access(f.backup.c_str(), F_OK) != 0, "validation refusal created backup");
        }
    };
    const auto corrupt = [&](const std::string& reason, const std::function<void(SharedStoreLayout&)>& modify) {
        auto changed = f.read();
        modify(*changed);
        f.save(*changed);
        rejectBoth(reason);
        f.save(*valid);
    };
    corrupt("conflicting highest-ts", [](auto& x) { x.latest[1].value = 1; });
    corrupt("conflicting highest-ts", [](auto& x) {
        const std::uint64_t otherNan = UINT64_C(0x7ff8000000000043);
        std::memcpy(&x.latest[1].value, &otherNan, sizeof(otherNan));
    });
    corrupt("conflicting highest-ts", [](auto& x) { x.latest[0].value = 0.0; x.latest[1].value = -0.0; });
    corrupt("conflicting highest-ts", [](auto& x) { ++x.latest[1].quality; });
    corrupt("conflicting highest-ts", [](auto& x) { ++x.latest[1].expireAt; });
    corrupt("conflicting highest-ts", [](auto& x) { x.latest[1].stale = 1; });
    corrupt("count mismatch", [](auto& x) { x.header.latestCount = 1; });
    corrupt("latest occupancy", [](auto& x) { x.latest[1].occupied = 2; });
    corrupt("pending writes", [](auto& x) { x.header.pendingWriteTail = 1; });
    corrupt("occupied pending write", [](auto& x) { x.pendingWrites[7].occupied = 1; });
    corrupt("owner lease", [](auto& x) { x.owners[0].heartbeatMs = INT64_MAX; });
    corrupt("point claim", [](auto& x) { x.claims[0].heartbeatMs = INT64_MAX; });
    corrupt("mutex", [](auto& x) { std::memset(&x.header.mutex, 0, sizeof(x.header.mutex)); });
    require(flock(f.fd, LOCK_EX | LOCK_NB) == 0, "lock dedup fixture");
    rejectBoth("locked by");
    require(flock(f.fd, LOCK_UN) == 0, "unlock dedup fixture");
    {
        LegacyMapping reader(f.fd);
        rejectBoth("still mapped");
    }
    // Runtime open/close can change mutex bookkeeping; restore the offline fixture.
    f.save(*valid);
    OfflineMigrationOptions options;
    options.checkOnly = true;
    options.deduplicateLatest = true;
    const auto plan = migrateOfflinePointStore(f.name, "", true, options);
    require(plan.removedCount == 1 && plan.duplicateGroups[0].winnerSlot == 0, "identical NaN bits refused");
    auto same = f.read();
    require(std::memcmp(valid.get(), same.get(), sizeof(*valid)) == 0, "NaN preflight wrote bytes");
    options.checkOnly = false;
    refusal(f, "open failed", [&] { migrateOfflinePointStore(f.name, f.backup + "/missing", true, options); });
    migrateOfflinePointStore(f.name, f.backup, true, options);
    valid->header.version = 10;
    valid->header.latestCount = 1;
    valid->latest[1].occupied = 0;
    same = f.read();
    require(std::memcmp(valid.get(), same.get(), sizeof(*valid)) == 0, "NaN dedup altered sample bits");
    std::cout << "PASS dedup/check safety gates, field conflicts, signed zero and NaN bit comparison\n";
}
}  // namespace

int main() {
    if (geteuid() != 0 || !std::getenv("GATEWAY_MIGRATION_TEST_BACKUP_DIR")) {
        std::cout << "SKIP: root plus isolated persistent GATEWAY_MIGRATION_TEST_BACKUP_DIR required\n";
        return 77;
    }
    try {
        success(8);
        success(9);
        rejectionCases();
        deduplication(8);
        deduplication(9);
        cliPreflight();
        dedupSafety();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
