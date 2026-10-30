#include "edge_gateway/memory_point_store.hpp"
#include "../src/memory_point_store_layout_v10.hpp"
#include "../src/memory_point_store_layout.hpp"
#include "edge_gateway/writeback_outcome.hpp"

#include <iostream>
#include <memory>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

#ifndef _WIN32
void authorizationSurvivesSharedQueue() {
    using namespace edge_gateway;
    const auto name = "shm11_context_" + std::to_string(getpid());
    struct Cleanup { std::string name; ~Cleanup() { shm_unlink(("/" + name).c_str()); } } cleanup{name};
    MemoryPointStore store(name);
    PendingWriteCommand command;
    command.index = 1234;
    command.value = 18;
    command.cmdId = "cluster-metadata";
    command.source = "graph-ems";
    command.controlGeneration = 42;
    ClusterWriteAuthorization auth;
    auth.kernelBootId.fill(0xa5);
    auth.authorityEpoch.fill(0x5a);
    auth.notAfterMonotonicMs = 9007199254740993LL;
    auth.dispatchSequence = 9007199254740995ULL;
    auth.authorityStoreName = "cluster_authority_test";
    command.clusterAuthorization = auth;
    ClusterAuthoritySnapshot snapshot;
    snapshot.authorization = auth;
    snapshot.valid = true;
    snapshot.targets = {{18, 19, 20, 1, 2, 3}};
    snapshot.targetIndexes = {1234, 1235};
    store.publishClusterAuthority(snapshot);
    {
        MemoryPointStore observer(name, MemoryStoreOpenMode::OpenExisting);
        const auto actual = observer.clusterAuthority();
        require(actual && actual->valid && actual->authorization &&
                actual->authorization->dispatchSequence == auth.dispatchSequence &&
                actual->targets == snapshot.targets && actual->targetIndexes == snapshot.targetIndexes,
                "atomic authority snapshot must cross process mappings without scalar encoding");
    }
    store.submitWriteCommand(command);
    const auto verify = [&](const std::vector<PendingWriteCommand>& commands) {
        require(commands.size() == 1, "one queued command expected");
        const auto& received = commands.front();
        require(static_cast<bool>(received.clusterAuthorization), "cluster authorization disappeared in shared queue");
        const auto& got = *received.clusterAuthorization;
        require(got.version == 1 && got.flags == 0 && got.kernelBootId == auth.kernelBootId &&
                got.authorityEpoch == auth.authorityEpoch && got.notAfterMonotonicMs == auth.notAfterMonotonicMs &&
                got.dispatchSequence == auth.dispatchSequence && got.authorityStoreName == auth.authorityStoreName,
                "cluster authorization changed during shared queue transport");
        require(received.controlGeneration == 42, "MQTT generation must remain independent");
    };
    verify(store.peekPendingWriteCommands());
    auto other = command;
    other.cmdId = "ordinary";
    other.clusterAuthorization = NullOpt;
    store.submitWriteCommand(other);
    auto first = store.drainPendingWriteCommands(1);
    verify(first);
    const auto ordinary = store.drainPendingWriteCommands();
    require(ordinary.size() == 1 && !ordinary.front().clusterAuthorization, "ordinary command stays untagged");
    store.submitWriteCommand(command);
    const auto pid = fork();
    require(pid >= 0, "fork reader");
    if (pid == 0) {
        try {
            MemoryPointStore reader(name, MemoryStoreOpenMode::OpenExisting);
            verify(reader.drainPendingWriteCommands());
            _exit(0);
        } catch (...) { _exit(1); }
    }
    int status = 0;
    require(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "independent process must receive complete authorization");
    require(store.peekPendingWriteCommands().empty(), "child must drain shared queue");
    store.publishClusterAuthority(ClusterAuthoritySnapshot{});
    require(!store.clusterAuthority()->valid, "invalid authority must revoke prior snapshot");
    for (int invalid = 0; invalid < 5; ++invalid) {
        auto bad = command;
        auto& context = *bad.clusterAuthorization;
        if (invalid == 0) context.version = 2;
        if (invalid == 1) context.authorityStoreName = "../unbound";
        if (invalid == 2) context.authorityStoreName.assign(64, 'a');
        if (invalid == 3) context.kernelBootId.fill(0);
        if (invalid == 4) context.notAfterMonotonicMs = 0;
        bool refused = false;
        try { store.submitWriteCommand(bad); } catch (const std::exception&) { refused = true; }
        require(refused && store.peekPendingWriteCommands().empty(), "invalid metadata must not enter queue");
    }
}

void legacySegmentIsRejectedWithoutMutation() {
    using namespace edge_gateway::memory_layout_v10;
    const auto name = "shm11_legacy_" + std::to_string(getpid());
    const auto path = "/" + name;
    const int fd = shm_open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    require(fd >= 0, "create legacy fixture");
    struct Cleanup {
        int fd;
        std::string path;
        ~Cleanup() { close(fd); shm_unlink(path.c_str()); }
    } cleanup{fd, path};
    auto original = std::make_unique<SharedStoreLayout>();
    original->header.magic = kSharedStoreMagic;
    original->header.version = 10;
    pthread_mutexattr_t attr{};
    require(pthread_mutexattr_init(&attr) == 0, "mutex attributes");
    require(pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0, "shared mutex");
    require(pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0, "robust mutex");
    require(pthread_mutex_init(&original->header.mutex, &attr) == 0, "mutex init");
    pthread_mutexattr_destroy(&attr);
    require(ftruncate(fd, sizeof(*original)) == 0, "legacy length");
    require(pwrite(fd, original.get(), sizeof(*original), 0) == sizeof(*original), "legacy data");
    bool rejected = false;
    try { edge_gateway::MemoryPointStore store(name); }
    catch (const std::exception&) { rejected = true; }
    auto after = std::make_unique<SharedStoreLayout>();
    struct stat state{};
    require(fstat(fd, &state) == 0, "legacy stat");
    require(pread(fd, after.get(), sizeof(*after), 0) == sizeof(*after), "legacy read");
    require(state.st_size == sizeof(*original) && std::memcmp(original.get(), after.get(), sizeof(*after)) == 0,
            "v11 runtime must not change any legacy segment byte");
    require(rejected, "v11 runtime must reject legacy v10 even when its ABI looks compatible");
}

void incompatibleSegmentIsNeverResized() {
    const auto name = "shm11_preserve_" + std::to_string(getpid());
    const auto path = "/" + name;
    const int fd = shm_open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    require(fd >= 0, "create isolated fixture");
    struct Cleanup {
        int fd;
        std::string path;
        ~Cleanup() { close(fd); shm_unlink(path.c_str()); }
    } cleanup{fd, path};
    const std::vector<unsigned char> original(4096, 0x5a);
    require(ftruncate(fd, original.size()) == 0, "fixture length");
    require(pwrite(fd, original.data(), original.size(), 0) == static_cast<ssize_t>(original.size()),
            "fixture bytes");
    bool rejected = false;
    try {
        edge_gateway::MemoryPointStore store(name, edge_gateway::MemoryStoreOpenMode::CreateOrOpen);
    } catch (const std::exception&) { rejected = true; }
    struct stat state{};
    require(fstat(fd, &state) == 0, "stat fixture");
    std::vector<unsigned char> after(original.size());
    require(pread(fd, after.data(), after.size(), 0) == static_cast<ssize_t>(after.size()), "read fixture");
    require(state.st_size == static_cast<off_t>(original.size()) && after == original,
            "CreateOrOpen must preserve every byte and length of incompatible existing SHM");
    require(rejected, "CreateOrOpen must reject incompatible existing SHM");
}
#endif
}

// Queued writes carry the producer's CLOCK_MONOTONIC stamp; the age check uses only that stamp.
void monotonicStampCrossesSharedQueue() {
    using namespace edge_gateway;
    const auto name = "shm11_stamp_" + std::to_string(getpid());
    struct Cleanup { std::string name; ~Cleanup() { shm_unlink(("/" + name).c_str()); } } cleanup{name};
    MemoryPointStore store(name);
    PendingWriteCommand command;
    command.index = 77;
    command.cmdId = "stamped";
    command.acceptedAt = 1;  // wall time from another epoch: irrelevant to the age
    store.submitWriteCommand(command);
    const auto peeked = store.peekPendingWriteCommands();
    require(peeked.size() == 1 && peeked.front().acceptedMonotonicMs != 0, "the producer must stamp an unstamped write");
    const auto age = pendingWriteAgeMs(peeked.front(), pendingWriteMonotonicNowMs());
    require(age >= 0 && age < 1000, "a just-queued write must be fresh");
    require(!isPendingWriteExpired(peeked.front(), pendingWriteMonotonicNowMs(), 10000), "fresh write expired");
    const auto pid = fork();
    require(pid >= 0, "fork consumer");
    if (pid == 0) {
        // CLOCK_MONOTONIC is system-wide: another process measures the same age.
        try {
            MemoryPointStore consumer(name, MemoryStoreOpenMode::OpenExisting);
            const auto drained = consumer.drainPendingWriteCommands();
            const auto childAge = drained.size() == 1 ? pendingWriteAgeMs(drained.front(), pendingWriteMonotonicNowMs()) : -1;
            _exit(childAge >= 0 && childAge < 1000 ? 0 : 1);
        } catch (...) { _exit(1); }
    }
    int status = 0;
    require(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "another process must see the producer's stamp as fresh");
    command.acceptedMonotonicMs = 12345;
    store.submitWriteCommand(command);
    require(store.drainPendingWriteCommands().front().acceptedMonotonicMs == 12345, "an explicit stamp must be kept");

    PendingWriteCommand probe;
    require(isPendingWriteExpired(probe, 5000, 10000), "an unstamped write must never run");
    require(!isPendingWriteExpired(probe, 5000, 0), "maxAge 0 disables expiry");
    probe.acceptedMonotonicMs = 0xFFFFFF00u;
    require(pendingWriteAgeMs(probe, 0x00000010u) == 0x110 && !isPendingWriteExpired(probe, 0x00000010u, 10000),
            "the age must survive the 32-bit wrap");
    probe.acceptedMonotonicMs = 1000;
    require(isPendingWriteExpired(probe, 1000 + 10001, 10000), "older than maxAge expires");
    require(!isPendingWriteExpired(probe, 1000 + 10000, 10000), "exactly maxAge is still valid");
    require(isPendingWriteExpired(probe, 1000 - 10001 + 0x100000000ull, 10000),
            "a stamp ahead of now by more than maxAge (not this boot) expires");
}

// A slot queued by a release that does not stamp has occupied=1 and arbitrary padding where the stamp
// now lives. Even a padding value that looks like a fresh stamp must never make such a command run.
void unstampedSlotIsNeverTrusted() {
    using namespace edge_gateway;
    namespace layout = edge_gateway::memory_layout;
    const auto name = "shm11_unstamped_" + std::to_string(getpid());
    struct Cleanup { std::string name; ~Cleanup() { shm_unlink(("/" + name).c_str()); } } cleanup{name};
    MemoryPointStore store(name);
    PendingWriteCommand command;
    command.index = 78;
    command.cmdId = "queued-by-old-release";
    store.submitWriteCommand(command);
    const int fd = shm_open(("/" + name).c_str(), O_RDWR, 0);
    require(fd >= 0, "map segment");
    void* base = mmap(nullptr, sizeof(layout::SharedStoreLayout), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    require(base != MAP_FAILED, "mmap segment");
    auto* mapped = static_cast<layout::SharedStoreLayout*>(base);
    auto& slot = mapped->pendingWrites[mapped->header.pendingWriteHead];
    require(slot.occupied == layout::kPendingSlotStamped && slot.acceptedMonotonicMs != 0,
            "a stamping producer marks its slot stamped");
    const auto freshLooking = pendingWriteMonotonicNowMs();
    slot.occupied = layout::kPendingSlotUnstamped;  // what every earlier release writes
    slot.acceptedMonotonicMs = freshLooking;       // padding that happens to look fresh
    const auto peeked = store.peekPendingWriteCommands();
    require(peeked.size() == 1 && peeked.front().acceptedMonotonicMs == 0,
            "the padding of an unstamped slot must never be read as a stamp");
    require(isPendingWriteExpired(peeked.front(), freshLooking, 10000), "an unstamped write must expire");
    munmap(base, sizeof(layout::SharedStoreLayout));
    const auto drained = store.drainPendingWriteCommands();
    require(drained.size() == 1 && drained.front().acceptedMonotonicMs == 0, "drain decodes the same way as peek");
}

// Receipts are marked occupied=2 when stage and flags follow writeback_outcome.hpp. A receipt from an
// earlier release (occupied=1) only knows success and the stage; its writeback-failed also covered a
// request sent without a conclusive reply and a read-back mismatch, so it must never read as a definite,
// retry-safe failure. Stages that never reach a device stay definite failures.
void legacyReceiptsNeverReadAsDefiniteFailure() {
    using namespace edge_gateway;
    namespace layout = edge_gateway::memory_layout;
    const auto name = "shm11_receipt_" + std::to_string(getpid());
    struct Cleanup { std::string name; ~Cleanup() { shm_unlink(("/" + name).c_str()); } } cleanup{name};
    MemoryPointStore store(name);
    const auto record = [&](const std::string& cmdId, const std::string& stage, bool success) {
        WritebackResultRecord result;
        result.cmdId = cmdId;
        result.index = 91;
        result.stage = stage;
        result.success = success;
        store.recordWritebackResult(result);
    };
    record("NEW_FAILED", "writeback-failed", false);
    record("OLD_FAILED", "writeback-failed", false);
    record("OLD_EXPIRED", "expired", false);
    record("OLD_REJECTED", "control-rejected", false);
    record("OLD_OK", "writeback-succeeded", true);
    const int fd = shm_open(("/" + name).c_str(), O_RDWR, 0);
    require(fd >= 0, "map segment");
    void* base = mmap(nullptr, sizeof(layout::SharedStoreLayout), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    require(base != MAP_FAILED, "mmap segment");
    auto* mapped = static_cast<layout::SharedStoreLayout*>(base);
    for (auto head = mapped->header.writebackResultHead; head != mapped->header.writebackResultTail;
         head = (head + 1) % layout::kMaxWritebackResultSlots) {
        auto& slot = mapped->writebackResults[head];
        require(slot.occupied == layout::kReceiptSlotThreeState, "receipts are written three-state");
        if (std::string(slot.cmdId).compare(0, 4, "OLD_") == 0) slot.occupied = layout::kReceiptSlotLegacy;
    }
    munmap(base, sizeof(layout::SharedStoreLayout));
    const auto outcome = [&](const std::string& cmdId) {
        const auto found = store.getWritebackResult(cmdId, 91);
        require(static_cast<bool>(found), "receipt " + cmdId);
        std::string recent;
        for (const auto& item : store.recentWritebackResults(91, 20)) {
            if (item.cmdId == cmdId) recent = writebackOutcome(item);
        }
        require(recent == writebackOutcome(*found), "both decode paths agree for " + cmdId);
        return std::string(writebackOutcome(*found)) + "/" + found->stage;
    };
    require(outcome("NEW_FAILED") == "failed/writeback-failed", "a three-state writeback-failed is definite");
    require(outcome("OLD_FAILED") == "unknown/writeback-failed", "a legacy writeback-failed may have taken effect");
    require(outcome("OLD_EXPIRED") == "failed/expired", "a legacy expired never reached a device");
    require(outcome("OLD_REJECTED") == "failed/control-rejected", "a legacy control-rejected never reached a device");
    require(outcome("OLD_OK") == "succeeded/writeback-succeeded", "a legacy success stays a success");
}

int main() {
    try {
#ifndef _WIN32
        monotonicStampCrossesSharedQueue();
        legacyReceiptsNeverReadAsDefiniteFailure();
        unstampedSlotIsNeverTrusted();
        incompatibleSegmentIsNeverResized();
        legacySegmentIsRejectedWithoutMutation();
        authorizationSurvivesSharedQueue();
#endif
        std::cout << "memory_point_store_shm11_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "memory_point_store_shm11_test failed: " << error.what() << '\n';
        return 1;
    }
}
