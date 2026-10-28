// Local DI/DO fixes from the driver review, on a fake sysfs GPIO tree:
//  - DI debounce keeps working when the wall clock steps back (fire / door / e-stop inputs),
//  - every DO starts as an output in its startup state; an undriven DO is never reported,
//  - failed sysfs control writes are reported, a failed export is retried (bounded),
//  - load-time validation of GPIO ownership and DO definitions.
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/dio_collector.hpp"
#include "edge_gateway/dio_command_executor.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/sysfs_gpio_port.hpp"

// Wall-clock interposer: simulates an NTP / manual step of CLOCK_REALTIME only.
static long long g_wallOffsetMs = 0;
extern "C" int clock_gettime(clockid_t id, struct timespec* ts) {
    using Real = int (*)(clockid_t, struct timespec*);
    static Real real = reinterpret_cast<Real>(dlsym(RTLD_NEXT, "clock_gettime"));
    const int result = real(id, ts);
    if (result == 0 && id == CLOCK_REALTIME && g_wallOffsetMs != 0) {
        long long ns = static_cast<long long>(ts->tv_sec) * 1000000000LL + ts->tv_nsec + g_wallOffsetMs * 1000000LL;
        ts->tv_sec = static_cast<time_t>(ns / 1000000000LL);
        ts->tv_nsec = static_cast<long>(ns % 1000000000LL);
    }
    return result;
}

namespace {

using namespace edge_gateway;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << std::endl;
    }
}

std::int64_t wallNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string slurp(const std::string& path) {
    std::ifstream input(path.c_str());
    std::stringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void spit(const std::string& path, const std::string& text) {
    std::ofstream output(path.c_str(), std::ios::trunc);
    output << text;
}

struct FakeSysfs {
    std::string root;
    explicit FakeSysfs(const std::string& base) : root(base) {
        ::mkdir(root.c_str(), 0755);
        spit(root + "/export", "");
    }
    void addGpio(int gpio, const std::string& direction, const std::string& value) const {
        const auto dir = root + "/gpio" + std::to_string(gpio);
        ::mkdir(dir.c_str(), 0755);
        spit(dir + "/direction", direction);
        spit(dir + "/value", value);
    }
    std::string file(int gpio, const std::string& name) const {
        return root + "/gpio" + std::to_string(gpio) + "/" + name;
    }
};

PointDefinition diPoint(std::uint32_t index, int gpio, int debounceMs) {
    PointDefinition point;
    point.index = index;
    point.pointCode = "di_" + std::to_string(gpio);
    point.enabled = true;
    point.read.enable = true;
    point.read.dataType = "digital_input";
    point.read.gpio = gpio;
    point.read.activeHigh = false;
    point.read.debounceMs = debounceMs;
    point.read.intervalMs = 1;
    point.read.cachePolicy.ttlMs = 600000;
    return point;
}

PointDefinition doPoint(std::uint32_t index, int gpio, bool activeHigh) {
    PointDefinition point;
    point.index = index;
    point.pointCode = "do_" + std::to_string(gpio);
    point.enabled = true;
    point.read.enable = true;
    point.read.dataType = "digital_output";
    point.read.gpio = gpio;
    point.read.activeHigh = activeHigh;
    point.read.intervalMs = 1;
    point.read.cachePolicy.ttlMs = 600000;
    point.write.enable = true;
    point.write.dataType = "digital_output";
    point.write.allowedValues = {0, 1};
    return point;
}

DeviceConfig dioConfig(const std::string& storeName, const std::string& sysfs) {
    DeviceConfig config;
    config.machineCode = "GW";
    config.meterCode = "LOCAL_DIO";
    config.protocol.type = "local_dio";
    config.protocol.gpioBasePath = sysfs;
    config.memoryStore.sharedMemoryName = storeName;
    config.memoryStore.sqlitePath.clear();
    config.memoryStore.maxLatestPoints = 32;
    config.memoryStore.maxPendingWrites = 8;
    config.memoryStore.maxPersistentSamples = 8;
    return config;
}

// DI1: a closing fire contact must be accepted after debounceMs even if NTP steps the clock back.
void debounceSurvivesWallClockStep(const std::string& base) {
    FakeSysfs sysfs(base + "/clock");
    sysfs.addGpio(137, "in\n", "1\n");  // active-low input, contact open
    const auto storeName = "dio_review_clock_" + std::to_string(getpid());
    MemoryPointStore::cleanupOrphanedSegment(storeName);
    {
        auto config = dioConfig(storeName, sysfs.root);
        config.points.push_back(diPoint(992, 137, 50));
        MemoryPointStore store(config.memoryStore);
        DioCollector collector(config, store, std::make_shared<SysfsGpioPort>(sysfs.root));
        auto sample = [&]() {
            const auto result = collector.collectOnce(wallNow());
            return result.values.empty() ? -1.0 : result.values.front().value;
        };
        check(sample() == 0.0, "contact open reads 0");
        check(slurp(sysfs.file(137, "direction")) == "in", "DI configured as input");
        spit(sysfs.file(137, "value"), "0\n");  // contact closes (active low)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));  // next 1 ms interval due
        check(sample() == 0.0, "change pending during debounce");
        g_wallOffsetMs = -3600 * 1000LL;  // wall clock steps back one hour
        bool accepted = false;
        for (int i = 0; i < 20 && !accepted; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            accepted = sample() == 1.0;
        }
        g_wallOffsetMs = 0;
        check(accepted, "DI change accepted after the debounce time despite the clock step");
        // And a step forward must not accept a bounce early.
        spit(sysfs.file(137, "value"), "1\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        g_wallOffsetMs = 3600 * 1000LL;
        check(sample() == 1.0, "a forward clock step does not shortcut the debounce");
        g_wallOffsetMs = 0;
    }
    MemoryPointStore::cleanupOrphanedSegment(storeName);
}

void doStartupState(const std::string& base) {
    FakeSysfs sysfs(base + "/startup");
    // Power-on: every pin is an input with an arbitrary (floating) level.
    sysfs.addGpio(231, "in\n", "0\n");  // active-low, no startupValue -> inactive -> high
    sysfs.addGpio(238, "in\n", "0\n");  // active-low, startupValue 1 -> low
    sysfs.addGpio(129, "in\n", "1\n");  // active-high, no startupValue -> low
    sysfs.addGpio(130, "in\n", "1\n");  // not available at startup (direction write fails)
    sysfs.addGpio(232, "out\n", "0\n");  // driver restart: active-low DO held active by EMS
    const auto storeName = "dio_review_startup_" + std::to_string(getpid());
    MemoryPointStore::cleanupOrphanedSegment(storeName);
    {
        auto config = dioConfig(storeName, sysfs.root);
        config.points.push_back(doPoint(984, 231, false));
        auto lit = doPoint(985, 238, false);
        lit.write.startupValue = 1.0;
        config.points.push_back(lit);
        config.points.push_back(doPoint(986, 129, true));
        config.points.push_back(doPoint(987, 130, false));
        config.points.push_back(doPoint(988, 232, false));
        // A directory in place of the control file: every write to it fails (EISDIR).
        ::unlink(sysfs.file(130, "direction").c_str());
        ::mkdir(sysfs.file(130, "direction").c_str(), 0755);

        auto port = std::make_shared<SysfsGpioPort>(sysfs.root);
        const auto failed = applyDioStartupOutputs(config, *port);
        check(failed.size() == 1 && failed.front().find("do_130") != std::string::npos,
              "the unavailable DO is reported, the others are set");
        check(slurp(sysfs.file(231, "direction")) == "high" && slurp(sysfs.file(231, "value"))[0] == '1',
              "active-low DO starts inactive (high), glitch-free direction");
        check(slurp(sysfs.file(238, "direction")) == "low" && slurp(sysfs.file(238, "value"))[0] == '0',
              "startupValue 1 drives the active-low DO low");
        check(slurp(sysfs.file(129, "direction")) == "low" && slurp(sysfs.file(129, "value"))[0] == '0',
              "active-high DO starts inactive (low)");
        check(slurp(sysfs.file(232, "direction")).compare(0, 3, "out") == 0 && slurp(sysfs.file(232, "value"))[0] == '0',
              "driver restart keeps a DO that is already an output (relay stays as EMS left it)");

        MemoryPointStore store(config.memoryStore);
        DioCollector collector(config, store, port);
        (void)collector.collectOnce(wallNow());
        const auto do1 = store.getLatestByIndex(984, wallNow());
        check(do1 && do1->quality == 1 && do1->value == 0.0, "DO reports its startup state, not an input level");
        const auto missing = store.getLatestByIndex(987, wallNow());
        check(missing && missing->quality == 0, "a DO that cannot be driven is bad quality, not a floating level");

        // The pin becomes available: the collector drives it to its startup state, once.
        ::rmdir(sysfs.file(130, "direction").c_str());
        spit(sysfs.file(130, "direction"), "in\n");
        (void)collector.collectOnce(wallNow() + 10);
        check(slurp(sysfs.file(130, "direction")) == "high", "late DO driven to its startup state by collection");
        check(store.getLatestByIndex(987, wallNow())->quality == 1, "late DO reported once driven");

        // A commanded output is never overridden by collection.
        DioCommandExecutor executor(config, store, port);
        check(executor.executeByIndex("C1", 984, 1.0, wallNow()).success, "DO write");
        check(slurp(sysfs.file(231, "value"))[0] == '0', "DO1=1 drives the active-low pin low");
        (void)collector.collectOnce(wallNow() + 20);
        check(slurp(sysfs.file(231, "value"))[0] == '0', "collection leaves a commanded DO alone");
        check(store.getLatestByIndex(984, wallNow())->value == 1.0, "DO state read back from the output latch");
    }
    MemoryPointStore::cleanupOrphanedSegment(storeName);
}

void sysfsErrorsAndExportRetry(const std::string& base) {
    FakeSysfs sysfs(base + "/errors");
    sysfs.addGpio(500, "in\n", "0\n");
    ::unlink(sysfs.file(500, "direction").c_str());
    check(::symlink("/dev/full", sysfs.file(500, "direction").c_str()) == 0, "fixture symlink");
    SysfsGpioPort port(sysfs.root);
    bool reported = false;
    try {
        port.setDirection(500, "in");
    } catch (const std::runtime_error& ex) {
        reported = std::string(ex.what()).find("direction") != std::string::npos;
    }
    check(reported, "a rejected direction write is reported, not swallowed");

    // export writes are accepted by the fake file but no gpio directory appears: not remembered.
    bool failedFirst = false;
    try {
        port.setDirection(501, "in");
    } catch (const std::runtime_error& ex) {
        failedFirst = std::string(ex.what()).find("export") != std::string::npos;
    }
    check(failedFirst, "missing gpio directory after export is an error");
    check(slurp(sysfs.root + "/export") == "501", "export was attempted");
    const auto before = std::chrono::steady_clock::now();
    bool backoff = false;
    try {
        port.setDirection(501, "in");
    } catch (const std::runtime_error& ex) {
        backoff = std::string(ex.what()).find("retrying") != std::string::npos;
    }
    check(backoff && std::chrono::steady_clock::now() - before < std::chrono::milliseconds(50),
          "within the backoff the failure is immediate (no stall of other inputs)");
    // The kernel exports it now: the next access works without waiting for the backoff.
    sysfs.addGpio(501, "in\n", "1\n");
    bool recovered = true;
    try {
        port.setDirection(501, "out");
        recovered = slurp(sysfs.file(501, "direction")) == "out";
    } catch (const std::exception&) {
        recovered = false;
    }
    check(recovered, "export retried after a failure");
}

std::string dioJson(const std::string& points) {
    return R"({"schemaVersion":"1.0.0","machineCode":"GW","protocol":{"type":"local_dio","backend":"sysfs_gpio"},)"
           R"("memoryStore":{"sharedMemoryName":"dio_review_validation"},"meters":[{"meterCode":"LOCAL_DIO","deviceName":"d","points":[)" +
        points + "]}]}";
}

std::string jsonPoint(int index, const std::string& code, const std::string& readType, int gpio, const std::string& write) {
    return R"({"index":)" + std::to_string(index) + R"(,"pointCode":")" + code +
        R"(","enabled":true,"read":{"enable":true,"dataType":")" + readType + R"(","gpio":)" + std::to_string(gpio) +
        R"(,"activeHigh":false},"write":)" + write + "}";
}

void expectRejected(const std::string& json, const std::string& needle, const std::string& what) {
    try {
        (void)ConfigLoader::loadFromText(json);
        check(false, what + ": accepted");
    } catch (const std::invalid_argument& ex) {
        check(std::string(ex.what()).find(needle) != std::string::npos, what + ": message \"" + ex.what() + "\"");
    }
}

void loadTimeValidation() {
    const std::string doWrite = R"({"enable":true,"dataType":"digital_output","allowedValues":[0,1]})";
    const std::string off = R"({"enable":false})";
    (void)ConfigLoader::loadFromText(dioJson(jsonPoint(1, "di_1", "digital_input", 134, off) + "," +
                                              jsonPoint(2, "do_1", "digital_output", 231, doWrite)));
    expectRejected(dioJson(jsonPoint(1, "di_1", "digital_input", 134, off) + "," +
                           jsonPoint(2, "do_1", "digital_output", 134, doWrite)),
                   "gpio 134 is also a DI of meter LOCAL_DIO point di_1", "same GPIO as DI and DO");
    expectRejected(dioJson(jsonPoint(1, "di_1", "digital_input", 134, doWrite)), "cannot be written", "writable DI");
    expectRejected(dioJson(jsonPoint(2, "do_1", "digital_output", 231, doWrite) + "," +
                           jsonPoint(3, "do_2", "digital_output", 231, doWrite)),
                   "already driven", "two DOs on one GPIO");
    expectRejected(dioJson(jsonPoint(2, "do_1", "digital_output", 231,
                           R"({"enable":true,"dataType":"digital_output","startupValue":2})")),
                   "startupValue", "startupValue outside 0/1");
    expectRejected(dioJson(jsonPoint(2, "do_1", "digital_output", -1, doWrite)), "gpio", "DO without gpio");
}

}  // namespace

int main() {
    char pattern[] = "/tmp/dio_driver_review_XXXXXX";
    const char* base = ::mkdtemp(pattern);
    if (base == nullptr) {
        std::cerr << "mkdtemp failed" << std::endl;
        return 1;
    }
    try {
        debounceSurvivesWallClockStep(base);
        doStartupState(base);
        sysfsErrorsAndExportRetry(base);
        loadTimeValidation();
    } catch (const std::exception& ex) {
        std::cerr << "dio_driver_review_test exception: " << ex.what() << std::endl;
        ++failures;
    }
    if (std::system((std::string("rm -rf ") + base).c_str()) != 0) {
        std::cerr << "cleanup of " << base << " failed" << std::endl;
    }
    if (failures != 0) {
        std::cerr << failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "dio_driver_review_test passed" << std::endl;
    return 0;
}
