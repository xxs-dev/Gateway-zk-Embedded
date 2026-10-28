#include "edge_gateway/memory_point_store.hpp"
#include "../src/memory_point_store_layout.hpp"

#include <cerrno>
#include <chrono>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace edge_gateway;
using namespace memory_layout;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

using Clock = std::chrono::steady_clock;

struct FileDescriptor {
    int fd = -1;
    explicit FileDescriptor(int value = -1) : fd(value) {}
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    ~FileDescriptor() { reset(); }
    void reset() {
        if (fd >= 0) close(fd);
        fd = -1;
    }
};

struct Pipe {
    FileDescriptor reader, writer;
    Pipe() {
        int fds[2];
        require(pipe(fds) == 0, "pipe failed");
        reader.fd = fds[0];
        writer.fd = fds[1];
    }
};

struct Children {
    std::vector<pid_t> pids;
    Children() { pids.reserve(4); }
    Children(const Children&) = delete;
    Children& operator=(const Children&) = delete;
    ~Children() {
        // Kill every unreaped child before waiting, including blocked writers.
        for (auto pid : pids) if (pid > 0) kill(pid, SIGKILL);
        for (auto pid : pids) {
            if (pid <= 0) continue;
            while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
        }
    }
    void wait(Clock::time_point deadline) {
        bool succeeded = true;
        for (auto& pid : pids) {
            int status = 0;
            for (;;) {
                const auto result = waitpid(pid, &status, WNOHANG);
                if (result == pid) {
                    pid = -1;
                    succeeded &= WIFEXITED(status) && WEXITSTATUS(status) == 0;
                    break;
                }
                if (result < 0 && errno == ECHILD) pid = -1;
                require(result >= 0 || errno == EINTR, "waitpid failed");
                require(Clock::now() < deadline, "writer exit timed out");
                usleep(1000);
            }
        }
        require(succeeded, "writer process failed");
    }
};

struct Segment {
    std::string name = "/gateway_multiwriter_" + std::to_string(getpid()) + "_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    ~Segment() { shm_unlink(name.c_str()); }
};

PointValue sample(std::uint32_t index, double number) {
    PointValue value;
    value.index = index;
    value.machineCode = "FIXTURE";
    value.meterCode = "EMS_FIXTURE";
    value.pointCode = "P" + std::to_string(index);
    value.value = number;
    value.ts = 1000;
    value.quality = 1;
    return value;
}

void byte(int fd, bool send, Clock::time_point deadline) {
    char token = 'x';
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        require(remaining > 0, "process barrier timed out");
        pollfd event{fd, static_cast<short>(send ? POLLOUT : POLLIN), 0};
        const auto ready = poll(&event, 1, static_cast<int>(remaining));
        if (ready < 0 && errno == EINTR) continue;
        require(ready > 0, "process barrier timed out or poll failed");
        const auto count = send ? write(fd, &token, 1) : read(fd, &token, 1);
        if (count < 0 && errno == EINTR) continue;
        require(count == 1, "process barrier failed");
        return;
    }
}

void verifyChildCleanup(Clock::time_point deadline) {
    for (const std::string mode : {"parent exception", "exit timeout", "barrier EOF"}) {
        std::vector<pid_t> spawned;
        spawned.reserve(2);
        bool caught = false;
        try {
            Pipe ready;
            Children children;
            for (unsigned worker = 0; worker < 2; ++worker) {
                const auto pid = fork();
                require(pid >= 0, "cleanup fixture fork failed");
                if (pid == 0) {
                    ready.reader.reset();
                    if (mode == "barrier EOF" && worker == 0) _exit(2);
                    try { byte(ready.writer.fd, true, deadline); }
                    catch (...) { _exit(3); }
                    ready.writer.reset();
                    for (;;) pause();
                }
                children.pids.push_back(pid);
                spawned.push_back(pid);
            }
            ready.writer.reset();
            for (unsigned worker = 0; worker < 2; ++worker) byte(ready.reader.fd, false, deadline);
            if (mode == "parent exception") throw std::runtime_error("injected parent exception");
            children.wait(Clock::now() + std::chrono::milliseconds(25));
        } catch (const std::runtime_error& ex) {
            const std::string expected = mode == "parent exception" ? "injected parent exception" :
                mode == "exit timeout" ? "writer exit timed out" : "process barrier failed";
            require(ex.what() == expected, "unexpected cleanup failure: " + std::string(ex.what()));
            caught = true;
        }
        require(caught && spawned.size() == 2, "cleanup failure was not exercised");
        for (auto pid : spawned) {
            errno = 0;
            require(waitpid(pid, nullptr, WNOHANG) == -1 && errno == ECHILD,
                    "cleanup left a live or unreaped child");
        }
        std::cout << "child cleanup: " << mode << " passed" << std::endl;
    }
}

// Inspect physical slots: getAllLatest alone hides duplicate indexes.
void verifyPhysical(const std::string& name, const std::set<std::uint32_t>& expected,
                    std::uint32_t version, ino_t inode) {
    FileDescriptor fd(shm_open(name.c_str(), O_RDONLY, 0600));
    require(fd.fd >= 0, "fixture SHM missing");
    struct stat st{};
    require(fstat(fd.fd, &st) == 0 && st.st_ino == inode, "restart replaced SHM inode");
    require(st.st_size == static_cast<off_t>(sizeof(SharedStoreLayout)), "restart changed SHM size");
    struct Mapping {
        void* view;
        ~Mapping() { if (view != MAP_FAILED) munmap(view, sizeof(SharedStoreLayout)); }
    } mapping{mmap(nullptr, sizeof(SharedStoreLayout), PROT_READ, MAP_SHARED, fd.fd, 0)};
    require(mapping.view != MAP_FAILED, "fixture mmap failed");
    // All writers have exited or closed their stores before this read-only check.
    const auto* layout = static_cast<const SharedStoreLayout*>(mapping.view);
    std::set<std::uint32_t> actual;
    std::size_t occupied = 0;
    for (const auto& slot : layout->latest) {
        if (slot.occupied) { ++occupied; actual.insert(slot.index); }
    }
    const auto count = layout->header.latestCount;
    const auto actualVersion = layout->header.version;
    require(occupied == actual.size(), "duplicate physical latest index: occupied=" +
        std::to_string(occupied) + " unique=" + std::to_string(actual.size()));
    require(actual == expected && count == expected.size(), "latest identity/count mismatch");
    require(actualVersion == version, "reattach changed SHM version");
}

void concurrentRestartWaves(std::uint32_t version, Clock::time_point deadline) {
    Segment segment;
    MemoryStoreConfig config;
    config.sharedMemoryName = segment.name;
    config.sharedMemoryCreateVersion = version;
    {
        MemoryPointStore seed(config);
        seed.putLatest(sample(900, 123.0));
    }
    FileDescriptor fd(shm_open(segment.name.c_str(), O_RDONLY, 0600));
    require(fd.fd >= 0, "cannot stat fixture");
    struct stat st{};
    require(fstat(fd.fd, &st) == 0, "cannot stat fixture inode");
    fd.reset();
    std::set<std::uint32_t> expected{900};
    const std::vector<std::uint32_t> indexes{402,407,416,417,418,420,422,424,427,431,434,440,443};

    for (unsigned wave = 0; wave < 3; ++wave) {
        Pipe ready, start;
        Children children;
        for (unsigned worker = 0; worker < 4; ++worker) {
            const auto pid = fork();
            require(pid >= 0, "fork failed");
            if (pid == 0) {
                ready.reader.reset(); start.writer.reset();
                try {
                    {
                        MemoryPointStore store(config);
                        byte(ready.writer.fd, true, deadline);
                        ready.writer.reset();
                        byte(start.reader.fd, false, deadline);
                        start.reader.reset();
                        for (unsigned round = 0; round < 32; ++round) {
                            for (auto index : indexes) {
                                store.putLatest(sample(index + wave * 1000, worker * 100 + round));
                            }
                        }
                        store.putLatest(sample(10000 + wave * 10 + worker, worker));
                    }
                    _exit(0);
                } catch (const std::exception& ex) {
                    std::cerr << "writer child: " << ex.what() << std::endl;
                    _exit(1);
                }
            }
            children.pids.push_back(pid);
        }
        ready.writer.reset(); start.reader.reset();
        // All four processes have independent caches before any new index exists.
        for (unsigned worker = 0; worker < 4; ++worker) byte(ready.reader.fd, false, deadline);
        for (unsigned worker = 0; worker < 4; ++worker) byte(start.writer.fd, true, deadline);
        ready.reader.reset(); start.writer.reset();
        children.wait(deadline);
        for (auto index : indexes) expected.insert(index + wave * 1000);
        for (unsigned worker = 0; worker < 4; ++worker) expected.insert(10000 + wave * 10 + worker);
        verifyPhysical(segment.name, expected, version, st.st_ino);
        {
            // Reconstruct after every writer exits, then update using rebuilt caches.
            MemoryPointStore restarted(config);
            const auto seed = restarted.getLatestByIndex(900, 1000);
            require(seed && seed->value == 123.0, "restart lost seed value");
            for (auto index : expected) restarted.putLatest(sample(index, index));
            for (auto index : expected) {
                const auto latest = restarted.getLatestByIndex(index, 1000);
                require(latest && latest->value == index, "restarted writer/readback mismatch");
            }
            restarted.putLatest(sample(900, 123.0));
        }
        verifyPhysical(segment.name, expected, version, st.st_ino);
        std::cout << "v" << version << " wave=" << wave + 1 << " writers=4 unique="
                  << expected.size() << " restart/inode/version preserved" << std::endl;
    }
}
}  // namespace

int main() {
    // Convert a closed barrier pipe to an exception so RAII can reap children.
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) return 1;
    const auto deadline = Clock::now() + std::chrono::seconds(25);
    try { verifyChildCleanup(deadline); }
    catch (const std::exception& ex) {
        std::cerr << "child cleanup failed: " << ex.what() << std::endl;
        return 1;
    }
    int failures = 0;
    for (std::uint32_t version = kMinimumCompatibleSharedStoreVersion;
         version <= kSharedStoreVersion; ++version) {
        try {
            require(Clock::now() < deadline, "test deadline exceeded");
            concurrentRestartWaves(version, deadline);
            std::cout << "multiwriter/restart v" << version << " passed" << std::endl;
        } catch (const std::exception& ex) {
            ++failures;
            std::cerr << "multiwriter/restart v" << version << " failed: " << ex.what() << std::endl;
        }
    }
    return failures == 0 ? 0 : 1;
}
