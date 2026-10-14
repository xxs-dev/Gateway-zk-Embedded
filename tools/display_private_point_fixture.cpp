#include "edge_gateway/memory_point_store.hpp"
#include "../src/memory_point_store_layout.hpp"

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace edge_gateway;
using Clock = std::chrono::steady_clock;
constexpr const char* machine = "COMM202600998";
constexpr const char* meter = "DISPLAY_PROBE";
constexpr std::uint32_t temperatureIndex = 880001, counterIndex = 880002;
constexpr int refreshMs = 200, ttlMs = 1000;
volatile std::sig_atomic_t stopped = 0;

void signalStop(int) { stopped = 1; }

void require(bool ok, const char* message) {
    if (!ok) throw std::invalid_argument(message);
}

struct Options {
    std::string name;
    int phaseSeconds = 5;
};

double number(const std::string& text) {
    try {
        std::size_t used = 0;
        const double value = std::stod(text, &used);
        require(used == text.size() && std::isfinite(value), "FINITE_CANONICAL_VALUE_REQUIRED");
        return value;
    } catch (const std::exception&) {
        throw std::invalid_argument("FINITE_CANONICAL_VALUE_REQUIRED");
    }
}

std::uint64_t integer(const std::string& text) {
    require(!text.empty() && text.find_first_not_of("0123456789") == std::string::npos,
            "UNSIGNED_INTEGER_REQUIRED");
    try { return std::stoull(text); }
    catch (const std::exception&) { throw std::invalid_argument("UNSIGNED_INTEGER_RANGE"); }
}

Options parse(int argc, char** argv) {
    require(argc >= 4 && argc <= 20 && std::string(argv[1]) == "--run", "EXACT_RUN_ENTRY_REQUIRED");
    require(argc % 2 == 0, "OPTION_VALUE_PAIRS_REQUIRED");
    Options result;
    std::set<std::string> seen;
    for (int i = 2; i < argc; i += 2) {
        const std::string key = argv[i], value = argv[i+1];
        require(key.size() <= 32 && value.size() <= 64 && seen.insert(key).second, "BOUNDED_UNIQUE_OPTIONS_REQUIRED");
        if (key == "--name") result.name = value;
        else if (key == "--phase-seconds") {
            const auto seconds = integer(value);
            require(seconds >= 1 && seconds <= 40, "BOUNDED_PHASE_REQUIRED");
            result.phaseSeconds = static_cast<int>(seconds);
        } else if (key == "--temperature-index") require(integer(value) == temperatureIndex, "FIXED_INDEX_REQUIRED");
        else if (key == "--counter-index") require(integer(value) == counterIndex, "FIXED_INDEX_REQUIRED");
        else if (key == "--a-temperature") require(number(value) == 12.5, "FIXED_VALUE_REQUIRED");
        else if (key == "--a-counter") require(number(value) == 1.0, "FIXED_VALUE_REQUIRED");
        else if (key == "--b-temperature") require(number(value) == 37.75, "FIXED_VALUE_REQUIRED");
        else if (key == "--b-counter") require(number(value) == 2.0, "FIXED_VALUE_REQUIRED");
        else throw std::invalid_argument("UNKNOWN_OPTION");
    }
    require(result.name == "d24_display_20261011_01_live" || result.name == "d24_display_20261011_01_test",
            "EXACT_SCOPE_PRIVATE_NAME_REQUIRED");
    return result;
}

void admitResources() {
    rlimit address{}, file{};
    require(getrlimit(RLIMIT_AS, &address) == 0 && address.rlim_cur <= 512ULL*1024*1024 &&
            address.rlim_max <= 512ULL*1024*1024,
            "ADDRESS_LIMIT_512M_REQUIRED");
    require(getrlimit(RLIMIT_FSIZE, &file) == 0 &&
            file.rlim_cur >= sizeof(memory_layout::SharedStoreLayout), "FSIZE_BELOW_REAL_V11_LAYOUT");
    require(file.rlim_cur <= 16ULL*1024*1024 && file.rlim_max <= 16ULL*1024*1024, "FSIZE_LIMIT_16M_REQUIRED");
    require(sizeof(memory_layout::SharedStoreLayout) < 64ULL*1024*1024, "SHM_SCOPE_LIMIT");
    struct statvfs space{};
    require(statvfs("/dev/shm", &space) == 0 &&
            space.f_bavail * static_cast<std::uint64_t>(space.f_frsize) >=
                sizeof(memory_layout::SharedStoreLayout) + 4ULL*1024*1024, "PRIVATE_SHM_SPACE_FLOOR");
}

PointDefinition definition(std::uint32_t index, const char* code) {
    PointDefinition point;
    point.index = index;
    point.pointCode = code;
    point.name = code;
    point.enabled = true;
    point.isStore = false;
    point.read.cachePolicy.storeLatest = true;
    point.read.cachePolicy.ttlMs = ttlMs;
    point.write.enable = false;
    return point;
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

PointValue value(std::uint32_t index, const char* code, double numeric, int quality, std::int64_t timestamp) {
    require((index == temperatureIndex || index == counterIndex) && std::isfinite(numeric), "FIXED_FINITE_POINT_REQUIRED");
    PointValue result;
    result.index = index;
    result.machineCode = machine;
    result.meterCode = meter;
    result.pointCode = code;
    result.value = numeric;
    result.quality = quality;
    result.ts = timestamp;
    result.expireAt = timestamp + ttlMs;
    result.isStore = false;
    return result;
}

void phase(MemoryPointStore& store, const Options& options, const char* name,
           double temperature, double counter, int quality, Clock::time_point wholeEnd) {
    const auto end = Clock::now() + std::chrono::seconds(options.phaseSeconds);
    bool first = true;
    auto tick = Clock::now();
    while (Clock::now() < end && !stopped) {
        require(Clock::now() < wholeEnd, "WHOLE_RUN_180S_DEADLINE");
        const auto timestamp = nowMs();
        store.heartbeatRegisteredPoints(timestamp);
        store.putLatest(value(temperatureIndex, "temperature", temperature, quality, timestamp));
        store.putLatest(value(counterIndex, "counter", counter, quality, timestamp));
        if (first) {
            std::cout << std::setprecision(17) << "{\"phase\":\"" << name << "\",\"store\":\"" << options.name
                      << "\",\"machineCode\":\"" << machine << "\",\"meterCode\":\"" << meter
                      << "\",\"temperatureIndex\":" << temperatureIndex << ",\"counterIndex\":" << counterIndex
                      << ",\"temperature\":" << temperature << ",\"counter\":" << counter
                      << ",\"quality\":" << quality << ",\"ts\":" << timestamp
                      << ",\"expireAt\":" << timestamp+ttlMs << "}" << std::endl;
            first = false;
        }
        tick += std::chrono::milliseconds(refreshMs);
        std::this_thread::sleep_until(tick);
    }
    require(!stopped, "INTERRUPTED_PRIVATE_OBJECT_RETAINED");
}

int run(const Options& options, Clock::time_point wholeEnd) {
    admitResources();
    std::signal(SIGTERM, signalStop);
    std::signal(SIGINT, signalStop);
    MemoryPointStore store(options.name, MemoryStoreOpenMode::CreateExclusive);
    store.registerPoint(machine, meter, definition(temperatureIndex, "temperature"));
    store.registerPoint(machine, meter, definition(counterIndex, "counter"));
    const int identityFd = shm_open(("/"+options.name).c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
    require(identityFd >= 0, "PRIVATE_OBJECT_IDENTITY_UNAVAILABLE");
    struct stat identity{};
    const bool identityOk = fstat(identityFd, &identity) == 0 && S_ISREG(identity.st_mode) &&
        identity.st_uid == geteuid() && identity.st_size == static_cast<off_t>(sizeof(memory_layout::SharedStoreLayout));
    close(identityFd);
    require(identityOk, "PRIVATE_OBJECT_IDENTITY_MISMATCH");
    std::cout << "{\"phase\":\"READY\",\"version\":11,\"layoutBytes\":"
              << sizeof(memory_layout::SharedStoreLayout) << ",\"mutexBytes\":" << sizeof(pthread_mutex_t)
              << ",\"device\":" << identity.st_dev << ",\"inode\":" << identity.st_ino
              << ",\"uid\":" << identity.st_uid << ",\"mode\":" << (identity.st_mode & 0777)
              << ",\"refreshMs\":200,\"ttlMs\":1000,\"wholeLimitSeconds\":180}" << std::endl;
    phase(store, options, "A", 12.5, 1.0, 1, wholeEnd);
    phase(store, options, "B", 37.75, 2.0, 1, wholeEnd);
    phase(store, options, "BAD", 37.75, 2.0, 0, wholeEnd);
    // Staleness is a last-good sample whose refresh actually stops, not a synthetic stale flag.
    const auto timestamp = nowMs();
    store.putLatest(value(temperatureIndex, "temperature", 37.75, 1, timestamp));
    store.putLatest(value(counterIndex, "counter", 2.0, 1, timestamp));
    std::cout << "{\"phase\":\"STALE_WAIT\",\"lastGoodTs\":" << timestamp
              << ",\"expireAt\":" << timestamp+ttlMs << "}" << std::endl;
    const auto expiredEnd = Clock::now() + std::chrono::milliseconds(ttlMs+1200);
    while (Clock::now() < expiredEnd) {
        require(!stopped && Clock::now() < wholeEnd, "INTERRUPTED_OR_RUN_DEADLINE");
        store.heartbeatRegisteredPoints(nowMs());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::cout << "{\"phase\":\"DONE_RETAINED\",\"store\":\"" << options.name << "\"}" << std::endl;
    return 0;
}
}

int main(int argc, char** argv) {
    const auto deadline = Clock::now() + std::chrono::seconds(180);
    try {
        return run(parse(argc, argv), deadline);
    } catch (const std::invalid_argument& error) {
        std::cerr << "PRIVATE_FIXTURE_REJECTED " << error.what() << '\n';
    } catch (const std::exception&) {
        std::cerr << "PRIVATE_FIXTURE_OPERATION_FAILED_OBJECT_RETAINED\n";
    }
    return 2;
}
