#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

edge_gateway::PointDefinition buildPoint() {
    edge_gateway::PointDefinition point;
    point.index = 610001;
    point.pointCode = "KY_TEST_LATEST";
    point.enabled = true;
    point.read.cachePolicy.storeLatest = true;
    point.read.cachePolicy.ttlMs = 600000;
    return point;
}

edge_gateway::PointValue buildValue(std::int64_t ts, double numericValue = 42.0) {
    edge_gateway::PointValue value;
    value.index = 610001;
    value.machineCode = "GW_TEST";
    value.meterCode = "METER_TEST";
    value.pointCode = "KY_TEST_LATEST";
    value.value = numericValue;
    value.quality = 1;
    value.ts = ts;
    value.expireAt = ts + 600000;
    return value;
}

void verifyReaderDoesNotUnlinkNamedSegment() {
    const std::string storeName = "gateway_memory_lifecycle_test";
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);

    std::unique_ptr<edge_gateway::MemoryPointStore> reader;
    {
        edge_gateway::MemoryStoreConfig config;
        config.sharedMemoryName = storeName;
        config.maxLatestPoints = 32;

        edge_gateway::MemoryPointStore writer(config);
        const auto point = buildPoint();
        writer.registerPoint("GW_TEST", "METER_TEST", point);
        writer.putLatest(buildValue(1000));

        reader.reset(new edge_gateway::MemoryPointStore(storeName));
        const auto readerLatest = reader->getLatestByIndex(point.index, 1000);
        require(static_cast<bool>(readerLatest), "reader should see writer latest value");
    }

    {
        edge_gateway::MemoryPointStore reopened(storeName);
        const auto latest = reopened.getLatestByIndex(610001, 1000);
        require(static_cast<bool>(latest), "reopened store should attach to the existing named segment");
        require(latest->value == 42.0, "reopened store value mismatch");
    }

    reader.reset();
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
}

void verifyReaderRemapsRecreatedNamedSegment() {
    const std::string storeName = "gateway_memory_remap_test";
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);

    auto reader = std::unique_ptr<edge_gateway::MemoryPointStore>();
    {
        edge_gateway::MemoryStoreConfig config;
        config.sharedMemoryName = storeName;
        config.maxLatestPoints = 32;

        edge_gateway::MemoryPointStore writer(config);
        const auto point = buildPoint();
        writer.registerPoint("GW_TEST", "METER_TEST", point);
        writer.putLatest(buildValue(1000, 42.0));

        reader.reset(new edge_gateway::MemoryPointStore(storeName));
        const auto first = reader->getLatestByIndex(point.index, 1000);
        require(static_cast<bool>(first), "reader should see initial segment value");
        require(first->value == 42.0, "initial reader value mismatch");
    }

    require(edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName),
            "orphaned segment should be unlinked while reader still has old mmap");

    {
        edge_gateway::MemoryStoreConfig config;
        config.sharedMemoryName = storeName;
        config.maxLatestPoints = 32;

        edge_gateway::MemoryPointStore writer(config);
        const auto point = buildPoint();
        writer.registerPoint("GW_TEST", "METER_TEST", point);
        writer.putLatest(buildValue(2000, 84.0));

        const auto remapped = reader->getLatestByIndex(point.index, 2000);
        require(static_cast<bool>(remapped), "reader should remap to recreated segment");
        require(remapped->value == 84.0, "remapped reader value mismatch");
        require(remapped->ts == 2000, "remapped reader timestamp mismatch");
    }

    reader.reset();
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
}

#ifndef _WIN32
struct SharedStoreHeaderProbe {
    std::uint32_t magic;
    std::uint32_t version;
    pthread_mutex_t mutex;
};

void verifyRobustMutexRecoversAfterOwnerDeath() {
    const std::string storeName = "gateway_memory_robust_mutex_test";
    const std::string posixName = "/" + storeName;
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
    setenv("GATEWAY_SHARED_MUTEX_LOCK_TIMEOUT_SEC", "1", 1);

    {
        edge_gateway::MemoryPointStore store(storeName);
        const auto point = buildPoint();
        store.registerPoint("GW_TEST", "METER_TEST", point);
        store.putLatest(buildValue(1000));
        require(store.getStats().latestCount == 1, "robust mutex test should start with one latest value");

        int readyPipe[2] = {-1, -1};
        require(pipe(readyPipe) == 0, "failed to create robust mutex test pipe");

        const auto child = fork();
        require(child >= 0, "failed to fork robust mutex test child");
        if (child == 0) {
            close(readyPipe[0]);
            const int fd = shm_open(posixName.c_str(), O_RDWR, 0600);
            if (fd < 0) {
                _exit(10);
            }
            void* view = mmap(nullptr, sizeof(SharedStoreHeaderProbe), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (view == MAP_FAILED) {
                close(fd);
                _exit(11);
            }
            auto* header = static_cast<SharedStoreHeaderProbe*>(view);
            if (header->magic != 0x4D505354 || header->version != 8) {
                _exit(12);
            }
            if (pthread_mutex_lock(&header->mutex) != 0) {
                munmap(view, sizeof(SharedStoreHeaderProbe));
                close(fd);
                _exit(13);
            }
            const char ready = '1';
            if (write(readyPipe[1], &ready, 1) != 1) {
                _exit(14);
            }
            _exit(0);
        }

        close(readyPipe[1]);
        char ready = 0;
        require(read(readyPipe[0], &ready, 1) == 1 && ready == '1', "child failed to acquire shared mutex");
        close(readyPipe[0]);

        int childStatus = 0;
        require(waitpid(child, &childStatus, 0) == child, "failed to wait for robust mutex test child");
        require(WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 0, "robust mutex test child failed");

        const auto stats = store.getStats();
        require(stats.latestCount == 0, "owner-death recovery should discard possibly partial shared state");
        require(!store.getLatestByIndex(point.index, 1000), "owner-death recovery should discard stale latest values");
    }

    unsetenv("GATEWAY_SHARED_MUTEX_LOCK_TIMEOUT_SEC");

    require(
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName),
        "robust mutex test segment should be removable after recovery"
    );
}
#endif

}  // namespace

int main() {
    try {
        verifyReaderDoesNotUnlinkNamedSegment();
        verifyReaderRemapsRecreatedNamedSegment();
#ifndef _WIN32
        verifyRobustMutexRecoversAfterOwnerDeath();
#endif
        std::cout << "memory_point_store_lifecycle_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "memory_point_store_lifecycle_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
