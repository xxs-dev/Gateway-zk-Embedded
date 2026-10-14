#include "edge_gateway/memory_point_store.hpp"
#include "../src/memory_point_store_layout.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <iomanip>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <vector>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

void collision(const std::string& name) {
    const int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    require(fd >= 0, "test private name is not fresh; no retry or foreign cleanup");
    const char sentinel[] = "scope-owned-existing-object-do-not-change";
    bool passed = false;
    try {
        require(write(fd, sentinel, sizeof(sentinel)) == static_cast<ssize_t>(sizeof(sentinel)), "sentinel write");
        struct stat before{}, after{};
        require(fstat(fd, &before) == 0, "initial stat");
        bool existsError = false;
        try {
            edge_gateway::MemoryPointStore store(name, edge_gateway::MemoryStoreOpenMode::CreateExclusive);
        } catch (const std::system_error& error) {
            existsError = error.code() == std::errc::file_exists;
        } catch (const std::exception&) {
            // A size/version failure is not proof of exclusive admission.
        }
        require(existsError, "exclusive admission did not report EEXIST");
        char actual[sizeof(sentinel)]{};
        require(pread(fd, actual, sizeof(actual), 0) == static_cast<ssize_t>(sizeof(actual)), "sentinel read");
        require(fstat(fd, &after) == 0 && before.st_size == after.st_size &&
                before.st_ino == after.st_ino && before.st_dev == after.st_dev &&
                std::memcmp(actual, sentinel, sizeof(actual)) == 0, "existing bytes/identity changed");
        struct stat current{};
        const std::string path = "/dev/shm/" + name.substr(1);
        require(lstat(path.c_str(), &current) == 0 && current.st_ino == before.st_ino &&
                current.st_dev == before.st_dev && current.st_uid == geteuid(), "owned cleanup identity");
        require(shm_unlink(name.c_str()) == 0, "owned successful test cleanup");
        passed = true;
        std::cout << "PASS exclusive_existing_exact_bytes_unchanged name=" << name
                  << " sentinelBytes=" << sizeof(sentinel) << " uid=" << geteuid() << '\n';
    } catch (...) {
        close(fd);
        std::cerr << "RETAINED_OWNED_TEST_OBJECT " << name << '\n';
        throw;
    }
    close(fd);
    require(passed, "collision result");
}

void realLayoutAdmission() {
    rlimit file{}, address{};
    require(getrlimit(RLIMIT_FSIZE, &file) == 0 &&
            file.rlim_cur >= sizeof(edge_gateway::memory_layout::SharedStoreLayout),
            "real API positive/replacement blocked: FSIZE below v11 layout; no allocation attempted");
    require(file.rlim_cur <= 16ULL*1024*1024 && file.rlim_max <= 16ULL*1024*1024, "FSIZE must be bounded to16MiB");
    require(getrlimit(RLIMIT_AS, &address) == 0 && address.rlim_cur <= 512ULL*1024*1024 &&
            address.rlim_max <= 512ULL*1024*1024, "bounded AS required");
}

edge_gateway::PointDefinition point(std::uint32_t index, const char* code) {
    edge_gateway::PointDefinition result;
    result.index = index;
    result.pointCode = code;
    result.read.cachePolicy.ttlMs = 1000;
    result.write.enable = false;
    result.isStore = false;
    return result;
}

std::int64_t clockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

edge_gateway::PointValue sample(std::uint32_t index, const char* code, double number, int quality, std::int64_t ts) {
    edge_gateway::PointValue value;
    value.index = index; value.machineCode = "COMM202600998"; value.meterCode = "DISPLAY_PROBE";
    value.pointCode = code; value.value = number; value.quality = quality;
    value.ts = ts; value.expireAt = ts+1000; value.isStore = false;
    return value;
}

void assertOwnedUnlink(const std::string& name, int identityFd) {
    struct stat original{}, current{};
    require(fstat(identityFd, &original) == 0 && lstat(("/dev/shm/"+name.substr(1)).c_str(), &current) == 0 &&
            original.st_dev == current.st_dev && original.st_ino == current.st_ino &&
            current.st_uid == geteuid(), "exact owned test cleanup binding");
    require(shm_unlink(name.c_str()) == 0, "owned test cleanup");
}

void apiLatest() {
    realLayoutAdmission();
    const std::string name = "/d24_display_20261011_01_api";
    edge_gateway::MemoryPointStore writer(name, edge_gateway::MemoryStoreOpenMode::CreateExclusive);
    writer.registerPoint("COMM202600998", "DISPLAY_PROBE", point(880001, "temperature"));
    writer.registerPoint("COMM202600998", "DISPLAY_PROBE", point(880002, "counter"));
    edge_gateway::MemoryPointStore reader(name, edge_gateway::MemoryStoreOpenMode::OpenExisting);
    const int fd = shm_open(name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
    require(fd >= 0, "owned API store descriptor");
    try {
        for (int phase = 0; phase < 3; ++phase) {
            const double temperature = phase == 0 ? 12.5 : 37.75, counter = phase == 0 ? 1.0 : 2.0;
            const int quality = phase == 2 ? 0 : 1;
            const auto now = clockMs();
            writer.putLatest(sample(880001, "temperature", temperature, quality, now));
            writer.putLatest(sample(880002, "counter", counter, quality, now));
            const auto a = reader.getLatestByIndex(880001, now), b = reader.getLatestByIndex(880002, now);
            require(a && b && a->value == temperature && b->value == counter &&
                    a->quality == quality && b->quality == quality && !a->stale && !b->stale &&
                    a->ts == now && b->ts == now && a->expireAt == now+1000 && b->expireAt == now+1000,
                    "independent API A/B/bad-quality sample");
        }
        const auto now = clockMs();
        writer.putLatest(sample(880001, "temperature", 37.75, 1, now));
        writer.putLatest(sample(880002, "counter", 2.0, 1, now));
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        const auto a = reader.getLatestByIndex(880001, clockMs()), b = reader.getLatestByIndex(880002, clockMs());
        require(a && b && a->stale && b->stale && a->value == 37.75 && b->value == 2.0, "actual elapsed TTL stale");
        assertOwnedUnlink(name, fd);
        close(fd);
        std::cout << "PASS independent_api_A_B_quality_actual_expiry\n";
    } catch (...) { close(fd); throw; }
}

std::vector<char> snapshot(int fd) {
    struct stat info{};
    require(fstat(fd, &info) == 0 && info.st_size > 0 && info.st_size <= 16*1024*1024, "bounded original snapshot");
    std::vector<char> result(static_cast<std::size_t>(info.st_size));
    std::size_t used = 0;
    while (used < result.size()) {
        const auto read = pread(fd, result.data()+used, result.size()-used, static_cast<off_t>(used));
        require(read > 0, "original snapshot read");
        used += static_cast<std::size_t>(read);
    }
    return result;
}

void replacement() {
    realLayoutAdmission();
    const std::string name = "/d24_display_20261011_01_replace", kept = "/d24_display_20261011_01_replace_kept";
    edge_gateway::MemoryPointStore writer(name, edge_gateway::MemoryStoreOpenMode::CreateExclusive);
    writer.registerPoint("COMM202600998", "DISPLAY_PROBE", point(880001, "temperature"));
    writer.putLatest(sample(880001, "temperature", 12.5, 1, clockMs()));
    const int original = shm_open(name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
    require(original >= 0, "original descriptor");
    int substitute = -1;
    try {
        const auto before = snapshot(original);
        // link fails if the scope-bound backup already exists; never overwrite a foreign path.
        require(link(("/dev/shm/"+name.substr(1)).c_str(), ("/dev/shm/"+kept.substr(1)).c_str()) == 0, "retain owned original link");
        assertOwnedUnlink(name, original);
        edge_gateway::MemoryPointStore replacementStore(name, edge_gateway::MemoryStoreOpenMode::CreateExclusive);
        replacementStore.registerPoint("COMM202600998", "DISPLAY_PROBE", point(880001, "temperature"));
        replacementStore.putLatest(sample(880001, "temperature", 99.0, 1, clockMs()));
        substitute = shm_open(name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
        require(substitute >= 0, "real initialized replacement descriptor");
        const auto replacementBefore = snapshot(substitute);
        bool readRejected = false, writeRejected = false;
        try { writer.getLatestByIndex(880001, clockMs()); } catch (const std::runtime_error&) { readRejected = true; }
        try { writer.putLatest(sample(880001, "temperature", 37.75, 1, clockMs())); }
        catch (const std::runtime_error&) { writeRejected = true; }
        require(readRejected && writeRejected && snapshot(original) == before, "replacement followed or original mutated");
        require(snapshot(substitute) == replacementBefore, "real initialized replacement bytes changed");
        assertOwnedUnlink(name, substitute);
        assertOwnedUnlink(kept, original);
        close(substitute); close(original);
        std::cout << "PASS replacement_rejected_original_and_substitute_unchanged\n";
    } catch (...) {
        if (substitute >= 0) close(substitute);
        close(original);
        std::cerr << "RETAINED_OWNED_TEST_OBJECTS " << name << ' ' << kept << '\n';
        throw;
    }
}

void truncateOwned() {
    realLayoutAdmission();
    const std::string name = "/d24_display_20261011_01_truncate";
    {
        edge_gateway::MemoryPointStore writer(name, edge_gateway::MemoryStoreOpenMode::CreateExclusive);
        writer.registerPoint("COMM202600998", "DISPLAY_PROBE", point(880001, "temperature"));
        writer.putLatest(sample(880001, "temperature", 12.5, 1, clockMs()));
        const int fd = shm_open(name.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
        require(fd >= 0, "owned shrink descriptor");
        try {
            require(ftruncate(fd, 32) == 0, "bounded own object shrink");
            bool readRejected = false, writeRejected = false;
            try { writer.getLatestByIndex(880001, clockMs()); } catch (const std::runtime_error&) { readRejected = true; }
            try { writer.putLatest(sample(880001, "temperature", 37.75, 1, clockMs())); }
            catch (const std::runtime_error&) { writeRejected = true; }
            require(readRejected && writeRejected, "size change not rejected before mapped access");
            assertOwnedUnlink(name, fd);
            close(fd);
        } catch (...) { close(fd); throw; }
    }
    // The scope has ended: this line also proves the destructor did not dereference the shortened mapping.
    std::cout << "PASS size_change_rejected_and_destructor_safe\n";
}

void readFixture(const std::string& name = "/d24_display_20261011_01_test", bool leases = false, bool sweep = false) {
    edge_gateway::MemoryPointStore reader(name, edge_gateway::MemoryStoreOpenMode::OpenExisting);
    const int fd = shm_open(name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
    require(fd >= 0, "fixture read descriptor");
    struct stat identity{};
    const bool valid = fstat(fd, &identity) == 0 && identity.st_uid == geteuid() &&
        identity.st_size == static_cast<off_t>(sizeof(edge_gateway::memory_layout::SharedStoreLayout));
    close(fd);
    require(valid, "fixture read identity");
    std::cout << std::setprecision(17) << "{\"store\":\"" << name.substr(1) << "\",\"device\":" << identity.st_dev
              << ",\"inode\":" << identity.st_ino;
    if (leases) {
        const auto timestamp = clockMs();
        const auto statuses = reader.getAllLeaseStatus(timestamp);
        std::cout << ",\"observedAt\":" << timestamp << ",\"leases\":[";
        bool firstLease = true;
        for (const auto& status : statuses) {
            if (!firstLease) std::cout << ',';
            firstLease = false;
            std::cout << "{\"index\":" << status.index << ",\"hasActiveOwner\":"
                      << (status.hasActiveOwner ? "true" : "false") << ",\"ownerCount\":" << status.ownerCount
                      << ",\"lastClaimTs\":" << status.lastClaimTs << '}';
        }
        std::cout << ']';
        if (sweep) reader.removeExpired(timestamp);
    }
    std::cout << ",\"points\":[";
    bool first = true;
    for (const auto index : {880001U, 880002U}) {
        const auto result = reader.getLatestByIndex(index, clockMs());
        if (!first) std::cout << ',';
        first = false;
        if (leases && !result) { std::cout << "null"; continue; }
        require(static_cast<bool>(result), "fixture sample missing");
        std::cout << "{\"index\":" << result->index << ",\"value\":" << result->value
                  << ",\"quality\":" << result->quality << ",\"ts\":" << result->ts
                  << ",\"expireAt\":" << result->expireAt << ",\"stale\":" << (result->stale ? "true" : "false") << '}';
    }
    std::cout << "]}" << std::endl;
}
}

int main(int argc, char** argv) {
    try {
        require(geteuid() != 0, "nonroot local test required");
        require(argc == 2, "exact selector required");
        const std::string selector = argv[1];
        if (selector == "--collision-red") collision("/d24_display_20261011_01_collision_red");
        else if (selector == "--collision") collision("/d24_display_20261011_01_collision_green");
        else if (selector == "--api") apiLatest();
        else if (selector == "--replacement") replacement();
        else if (selector == "--truncate") truncateOwned();
        else if (selector == "--read-private") readFixture();
        else if (selector == "--read-test-lease") readFixture("/d24_display_20261011_01_test", true);
        else if (selector == "--read-test-lease-sweep") readFixture("/d24_display_20261011_01_test", true, true);
        else if (selector == "--read-live-lease") readFixture("/d24_display_20261011_01_live", true);
        else if (selector == "--read-live-lease-sweep") readFixture("/d24_display_20261011_01_live", true, true);
        else if (selector == "--layout") {
            std::cout << "layoutBytes=" << sizeof(edge_gateway::memory_layout::SharedStoreLayout)
                      << " mutexBytes=" << sizeof(pthread_mutex_t) << " version="
                      << edge_gateway::memory_layout::kSharedStoreVersion << '\n';
        } else throw std::invalid_argument("unknown selector");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
