#include "../src/memory_point_store_layout_v10.hpp"
#include "../src/memory_point_store_migration.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace edge_gateway;
using namespace edge_gateway::memory_layout_v10;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

void copyPreservesSourceAndData() {
    const auto source = "shm11_copy_src_" + std::to_string(getpid());
    const auto target = "shm11_copy_dst_" + std::to_string(getpid());
    const char* directory = std::getenv("GATEWAY_MIGRATION_TEST_BACKUP_DIR");
    require(directory != nullptr, "isolated backup directory required");
    const auto backup = std::string(directory) + "/" + source + ".bak";
    int fd = shm_open(("/" + source).c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    require(fd >= 0, "create source");
    struct Cleanup {
        int fd;
        std::string source, target, backup;
        ~Cleanup() { close(fd); shm_unlink(("/"+source).c_str()); shm_unlink(("/"+target).c_str()); unlink(backup.c_str()); }
    } cleanup{fd, source, target, backup};
    auto original = std::make_unique<SharedStoreLayout>();
    original->header.magic = kSharedStoreMagic;
    original->header.version = 10;
    pthread_mutexattr_t attr{};
    require(pthread_mutexattr_init(&attr) == 0, "mutex attributes");
    require(pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0, "shared mutex");
    require(pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0, "robust mutex");
    require(pthread_mutex_init(&original->header.mutex, &attr) == 0, "mutex init");
    pthread_mutexattr_destroy(&attr);
    original->header.latestCount = 1;
    original->latest[5].index = 1234;
    original->latest[5].value = 42.25;
    original->latest[5].occupied = 1;
    original->latest[5].ts = 1000;
    original->latest[5].expireAt = 900000;
    original->header.persistentHead = kMaxPersistentSlots-1;
    original->header.persistentTail = 1;
    original->header.persistentSequence = 19;
    original->persistent[kMaxPersistentSlots-1].occupied = 1;
    original->persistent[kMaxPersistentSlots-1].sequence = 18;
    original->persistent[0].occupied = 1;
    original->persistent[0].sequence = 19;
    original->persistent[0].value = 43.5;
    original->header.writebackResultHead = 9;
    original->header.writebackResultTail = 10;
    original->header.writebackResultSequence = 5;
    original->writebackResults[9].occupied = 1;
    original->writebackResults[9].sequence = 5;
    original->writebackResults[9].success = 1;
    std::strcpy(original->writebackResults[9].cmdId, "old-completed");
    require(ftruncate(fd, sizeof(*original)) == 0, "source size");
    require(pwrite(fd, original.get(), sizeof(*original), 0) == sizeof(*original), "source bytes");
    for (int scenario = 0; scenario < 5; ++scenario) {
        auto invalid = std::make_unique<SharedStoreLayout>(*original);
        if (scenario == 1) invalid->header.pendingWriteTail = 1;
        if (scenario == 2) invalid->pendingWrites[7].occupied = 1;
        if (scenario == 3) invalid->header.version = 12;
        if (scenario == 4) invalid->owners[0].occupied = 1, invalid->owners[0].heartbeatMs = INT64_MAX;
        require(pwrite(fd, invalid.get(), sizeof(*invalid), 0) == sizeof(*invalid), "invalid fixture");
        bool refused = false;
        try { copyOfflinePointStoreV10ToV11(source, target, backup, scenario != 0); }
        catch (const std::exception&) { refused = true; }
        require(refused, "unsafe conversion must be refused");
        auto unchanged = std::make_unique<SharedStoreLayout>();
        require(pread(fd, unchanged.get(), sizeof(*unchanged), 0) == sizeof(*unchanged), "refused source read");
        require(std::memcmp(invalid.get(), unchanged.get(), sizeof(*unchanged)) == 0, "refusal changed source");
        require(access(backup.c_str(), F_OK) != 0, "preflight refusal must not create backup");
        const int unexpected = shm_open(("/" + target).c_str(), O_RDONLY, 0);
        if (unexpected >= 0) close(unexpected);
        require(unexpected < 0, "preflight refusal must not create target");
    }
    require(pwrite(fd, original.get(), sizeof(*original), 0) == sizeof(*original), "restore source fixture");
    const auto result = copyOfflinePointStoreV10ToV11(source, target, backup, true);
    require(result.oldVersion == 10 && result.occupiedAfter == 1, "copy result");
    auto after = std::make_unique<SharedStoreLayout>();
    require(pread(fd, after.get(), sizeof(*after), 0) == sizeof(*after), "source read");
    require(std::memcmp(original.get(), after.get(), sizeof(*after)) == 0, "source must remain byte-for-byte unchanged");
    int backupFd = open(backup.c_str(), O_RDONLY);
    require(backupFd >= 0, "backup present");
    require(pread(backupFd, after.get(), sizeof(*after), 0) == sizeof(*after), "backup read");
    close(backupFd);
    require(std::memcmp(original.get(), after.get(), sizeof(*after)) == 0, "backup must match source");
    MemoryPointStore reader(target, MemoryStoreOpenMode::OpenExisting);
    const auto point = reader.getLatestByIndex(1234, 1100);
    require(point && point->value == 42.25 && point->ts == 1000, "latest data retained");
    const auto history = reader.peekPersistentSamples();
    require(history.size() == 2 && history[0].sequence == 18 && history[1].sequence == 19 &&
            history[1].value == 43.5, "wrapped history retained");
    const auto receipt = reader.getWritebackResult("old-completed");
    require(receipt && receipt->success, "completed receipt retained");
    require(reader.peekPendingWriteCommands().empty(), "copy must not replay pending commands");
}
}
int main() {
    try { copyPreservesSourceAndData(); std::cout << "memory_point_store_v11_migration_test passed\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
