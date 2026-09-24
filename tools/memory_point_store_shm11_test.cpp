#include "edge_gateway/memory_point_store.hpp"
#include "../src/memory_point_store_layout_v10.hpp"

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
    store.submitWriteCommand(command);
    const auto verify = [&](const std::vector<PendingWriteCommand>& commands) {
        require(commands.size() == 1, "one queued command expected");
        const auto& received = commands.front();
        require(received.clusterAuthorization.has_value(), "cluster authorization disappeared in shared queue");
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

int main() {
    try {
#ifndef _WIN32
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
