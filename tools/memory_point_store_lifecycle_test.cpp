#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
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
std::string normalizeName(const std::string& name) {
    return name.empty() || name.front() == '/' ? name : "/" + name;
}

void unlinkSegment(const std::string& name) {
    if (::shm_unlink(normalizeName(name).c_str()) != 0 && errno != ENOENT) {
        throw std::runtime_error("failed to unlink test segment: " + std::string(std::strerror(errno)));
    }
}

void verifyStrictReaderRejectsZeroHeaderWithoutMutatingIt() {
    const std::string storeName = "gateway_memory_zero_header_test";
    unlinkSegment(storeName);

    std::size_t storeSize = 0;
    {
        edge_gateway::MemoryPointStore writer(storeName);
        const int fd = ::shm_open(normalizeName(storeName).c_str(), O_RDWR, 0600);
        require(fd >= 0, "failed to inspect initialized segment");
        struct stat info{};
        require(::fstat(fd, &info) == 0, "failed to stat initialized segment");
        storeSize = static_cast<std::size_t>(info.st_size);
        ::close(fd);
    }
    unlinkSegment(storeName);

    const int fd = ::shm_open(normalizeName(storeName).c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    require(fd >= 0, "failed to create zero-header segment");
    require(::ftruncate(fd, static_cast<off_t>(storeSize)) == 0, "failed to size zero-header segment");
    ::close(fd);

    bool rejected = false;
    try {
        edge_gateway::MemoryPointStore reader(
            storeName,
            edge_gateway::MemoryStoreOpenMode::OpenExisting
        );
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "strict reader accepted a full-size zero-header segment");

    const int inspectFd = ::shm_open(normalizeName(storeName).c_str(), O_RDONLY, 0600);
    require(inspectFd >= 0, "failed to reopen zero-header segment");
    unsigned char header[8]{};
    require(::read(inspectFd, header, sizeof(header)) == static_cast<ssize_t>(sizeof(header)),
            "failed to read zero-header segment");
    ::close(inspectFd);
    for (const auto byte : header) {
        require(byte == 0, "strict reader mutated the zero header");
    }
    unlinkSegment(storeName);
}

void verifyStrictReaderRejectsUnlinkedMapping() {
    const std::string storeName = "gateway_memory_strict_unlink_test";
    unlinkSegment(storeName);
    std::unique_ptr<edge_gateway::MemoryPointStore> reader;
    {
        edge_gateway::MemoryPointStore writer(storeName);
        const auto point = buildPoint();
        writer.registerPoint("GW_TEST", "METER_TEST", point);
        writer.putLatest(buildValue(1000));
        reader.reset(new edge_gateway::MemoryPointStore(
            storeName,
            edge_gateway::MemoryStoreOpenMode::OpenExisting
        ));
        require(static_cast<bool>(reader->getLatestByIndex(point.index, 1000)),
                "strict reader should see the initial value");
    }

    unlinkSegment(storeName);
    bool rejected = false;
    try {
        (void)reader->getLatestByIndex(610001, 1000);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "strict reader returned stale data after shm_unlink");
    reader.reset();
}
#endif

}  // namespace

int main() {
    try {
        verifyReaderDoesNotUnlinkNamedSegment();
        verifyReaderRemapsRecreatedNamedSegment();
#ifndef _WIN32
        verifyStrictReaderRejectsZeroHeaderWithoutMutatingIt();
        verifyStrictReaderRejectsUnlinkedMapping();
#endif
        std::cout << "memory_point_store_lifecycle_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "memory_point_store_lifecycle_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
