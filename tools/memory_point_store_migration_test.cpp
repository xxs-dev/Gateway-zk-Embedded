#include "../src/memory_point_store_layout.hpp"
#include "../src/memory_point_store_migration.hpp"
#include "edge_gateway/memory_point_store.hpp"

#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace edge_gateway;
using namespace edge_gateway::memory_layout;

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
        // Create through the real v8/v9 runtime constructor, then release its mmap.
        MemoryStoreConfig config;
        config.sharedMemoryName = name;
        config.sharedMemoryCreateVersion = version;
        {
            MemoryPointStore store(config);
            PointValue value;
            value.index = 1001;
            value.value = 42.25;
            value.ts = 1000;
            value.expireAt = 9999999999999;
            value.isStore = true;
            value.persistIntervalSec = 1;
            store.putLatest(value);
        }
        fd = shm_open(("/" + name).c_str(), O_RDWR, 0);
        require(fd >= 0, "open test segment");
        auto layout = read();
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
    }
    std::unique_ptr<SharedStoreLayout> read() const {
        auto layout = std::make_unique<SharedStoreLayout>();
        require(pread(fd, layout.get(), sizeof(*layout), 0) == sizeof(*layout), "read real layout");
        return layout;
    }
    void save(const SharedStoreLayout& layout) {
        require(pwrite(fd, &layout, sizeof(layout), 0) == sizeof(layout), "write real layout fixture");
    }
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
        restarted.sharedMemoryName = f.name;
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
        MemoryPointStore reader(f.name, MemoryStoreOpenMode::OpenExisting);
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
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
