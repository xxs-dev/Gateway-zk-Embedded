#include "../src/memory_point_store_layout_v10.hpp"
#include "../src/memory_point_store_layout.hpp"
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
}

int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: fixture create|pending|owner|verify NAME");
        const std::string action(argv[1]), name(argv[2]);
        require(name.size() < 64 && name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos, "unsafe name");
        if (action == "verify") {
            using namespace edge_gateway::memory_layout;
            const int fd = shm_open(("/" + name).c_str(), O_RDONLY, 0);
            require(fd >= 0, "open target");
            auto value = std::make_unique<SharedStoreLayout>();
            require(pread(fd, value.get(), sizeof(*value), 0) == sizeof(*value), "read target");
            close(fd);
            require(value->header.magic == kSharedStoreMagic && value->header.version == 11, "ABI11");
            require(value->latest[5].occupied && value->latest[5].index == 1234 && value->latest[5].value == 42.25, "latest retained");
            require(value->persistent[0].occupied && value->persistent[0].sequence == 19, "history retained");
            require(value->writebackResults[0].occupied && value->writebackResults[0].success, "receipt retained");
            for (const auto& slot : value->pendingWrites) require(!slot.occupied, "no pending replay");
            for (const auto& slot : value->owners) require(!slot.occupied, "no old owner");
            for (const auto& slot : value->claims) require(!slot.occupied, "no old claims");
            require(!value->clusterAuthority.valid && !value->clusterAuthority.occupied, "no old authorization");
        } else {
            using namespace edge_gateway::memory_layout_v10;
            require(action == "create" || action == "pending" || action == "owner", "unknown mode");
            auto value = std::make_unique<SharedStoreLayout>();
            value->header.magic = kSharedStoreMagic;
            value->header.version = 10;
            pthread_mutexattr_t attr{};
            require(pthread_mutexattr_init(&attr) == 0 && pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0 &&
                    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0 && pthread_mutex_init(&value->header.mutex, &attr) == 0, "mutex");
            pthread_mutexattr_destroy(&attr);
            value->header.latestCount = 1;
            value->latest[5].occupied = 1;
            value->latest[5].index = 1234;
            value->latest[5].value = 42.25;
            value->latest[5].ts = 1000;
            value->latest[5].expireAt = 900000;
            value->header.persistentTail = 1;
            value->header.persistentSequence = 19;
            value->persistent[0].occupied = 1;
            value->persistent[0].sequence = 19;
            value->header.writebackResultTail = 1;
            value->header.writebackResultSequence = 5;
            value->writebackResults[0].occupied = 1;
            value->writebackResults[0].success = 1;
            value->writebackResults[0].sequence = 5;
            std::strcpy(value->writebackResults[0].cmdId, "old-completed");
            if (action == "pending") value->pendingWrites[7].occupied = 1;
            if (action == "owner") value->owners[0].occupied = 1, value->owners[0].heartbeatMs = INT64_MAX;
            const int fd = shm_open(("/" + name).c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
            require(fd >= 0 && ftruncate(fd, sizeof(*value)) == 0, "create source");
            require(pwrite(fd, value.get(), sizeof(*value), 0) == sizeof(*value), "write source");
            close(fd);
        }
        std::cout << action << " PASS " << name << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
