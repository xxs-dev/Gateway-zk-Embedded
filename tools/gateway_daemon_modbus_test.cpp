// driver-review: GatewayDaemon with Modbus collectors.
// - one failed cycle does not publish device_online=0 below offlineFailureThreshold
// - with runtimeMeterBatchSize < meters the daemon finishes the round before sleeping
// - a slave waiting for its frameIntervalMs does not block the other slaves on the line
// - the collect loop keeps running when the wall clock is stepped back (loop runs on steady_clock)
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "edge_gateway/gateway_daemon.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/modbus_error.hpp"
#include "edge_gateway/modbus_rtu_client.hpp"
#include "virtual_rtu_line.hpp"

// Wall clock offset for this process (CLOCK_REALTIME only); lets the test step the clock back like NTP.
static std::atomic<long long> gRealtimeOffsetSeconds{0};

extern "C" int clock_gettime(clockid_t clock, struct timespec* tp) {
    const auto rc = static_cast<int>(syscall(SYS_clock_gettime, clock, tp));
    if (rc == 0 && clock == CLOCK_REALTIME) {
        tp->tv_sec += static_cast<time_t>(gRealtimeOffsetSeconds.load());
    }
    return rc;
}

using namespace edge_gateway;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class CountingClient : public IModbusClient {
public:
    std::set<int> failingCycles;
    int cycle = 0;
    std::mutex mutex;
    std::map<int, int> readsBySlave;

    std::vector<std::uint16_t> read(int slave, int count) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++readsBySlave[slave];
        }
        if (failingCycles.count(cycle)) throw ModbusError(ModbusFailureKind::Timeout, "modbus response timeout", true);
        return std::vector<std::uint16_t>(static_cast<std::size_t>(count), 7);
    }
    int reads(int slave) {
        std::lock_guard<std::mutex> lock(mutex);
        return readsBySlave[slave];
    }
    std::vector<std::uint16_t> readCoils(int s, int, int c) override { return read(s, c); }
    std::vector<std::uint16_t> readDiscreteInputs(int s, int, int c) override { return read(s, c); }
    std::vector<std::uint16_t> readHoldingRegisters(int s, int, int c) override { return read(s, c); }
    std::vector<std::uint16_t> readInputRegisters(int s, int, int c) override { return read(s, c); }
    void writeSingleCoil(int, int, bool) override {}
    void writeSingleRegister(int, int, std::uint16_t) override {}
    void writeMultipleRegisters(int, int, const std::vector<std::uint16_t>&) override {}
};

PointDefinition point(std::uint32_t index, const std::string& code, const std::string& type, int address, int intervalMs) {
    PointDefinition p;
    p.index = index;
    p.pointCode = code;
    p.name = code;
    p.enabled = true;
    p.address = address;
    p.read.enable = true;
    p.read.function = type == "device_online" ? 0 : 3;
    p.read.length = type == "device_online" ? 0 : 1;
    p.read.dataType = type;
    p.read.intervalMs = intervalMs;
    p.read.cachePolicy.storeLatest = true;
    p.read.cachePolicy.ttlMs = 600000;
    return p;
}

DeviceConfig baseConfig(const std::string& shm) {
    DeviceConfig cfg;
    cfg.machineCode = "GW";
    cfg.meterCode = "ROOT";
    cfg.protocol.type = "modbus_rtu";
    cfg.protocol.slave = 1;
    cfg.collect.defaultIntervalMs = 100;
    cfg.memoryStore.sharedMemoryName = shm;
    cfg.memoryStore.sqlitePath.clear();
    cfg.memoryStore.maxLatestPoints = 256;
    cfg.memoryStore.maxPendingWrites = 16;
    cfg.memoryStore.maxPersistentSamples = 64;
    return cfg;
}

LogicalDeviceConfig meter(int slave, std::uint32_t indexBase, int points, int intervalMs) {
    LogicalDeviceConfig m;
    m.meterCode = "M" + std::to_string(slave);
    m.deviceName = m.meterCode;
    m.slave = slave;
    m.points.push_back(point(indexBase, m.meterCode + "_online", "device_online", 0, intervalMs));
    for (int i = 0; i < points; ++i) {
        // Two registers apart: every point is its own request.
        m.points.push_back(point(indexBase + 1 + i, m.meterCode + "_p" + std::to_string(i), "uint16", i * 2, intervalMs));
    }
    return m;
}

struct Shm {
    explicit Shm(std::string n) : name(std::move(n)) { MemoryPointStore::cleanupOrphanedSegment(name); }
    ~Shm() { MemoryPointStore::cleanupOrphanedSegment(name); }
    std::string name;
};

void verifyOneFailedCycleKeepsDeviceOnline() {
    Shm shm("gw_daemon_modbus_online_" + std::to_string(getpid()));
    auto cfg = baseConfig(shm.name);
    cfg.collect.offlineFailureThreshold = 3;
    cfg.collect.failureBadQualityThreshold = 3;
    cfg.collect.slaveFailureBackoffThreshold = 100;
    cfg.collect.taskFailureBackoffThreshold = 100;
    cfg.meters = {meter(1, 1, 1, 100)};
    MemoryPointStore store(cfg.memoryStore);
    auto client = std::make_shared<CountingClient>();
    client->failingCycles = {3};
    GatewayDaemon daemon(cfg, store, client);
    for (int i = 1; i <= 5; ++i) {
        client->cycle = i;
        const std::int64_t now = 1000000 + i * 200;
        daemon.collectOnce(now);
        const auto online = store.getLatestByIndex(1, now);
        require(online && online->value == 1.0,
                "cycle " + std::to_string(i) + ": a single timeout below offlineFailureThreshold must keep device_online=1");
    }
}

void verifyRoundFinishesBeforeSleeping() {
    Shm shm("gw_daemon_modbus_round_" + std::to_string(getpid()));
    auto cfg = baseConfig(shm.name);
    cfg.collect.runtimeMeterBatchSize = 2;
    for (int slave = 1; slave <= 5; ++slave) cfg.meters.push_back(meter(slave, static_cast<std::uint32_t>(slave * 100), 1, 100));
    MemoryPointStore store(cfg.memoryStore);
    auto client = std::make_shared<CountingClient>();
    GatewayDaemon daemon(cfg, store, client);
    daemon.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(650));
    daemon.stop();
    for (int slave = 1; slave <= 5; ++slave) {
        // 100 ms intervals over 650 ms: about 6 reads. Batches of 2 meters per 100 ms tick gave about 2.
        require(client->reads(slave) >= 5, "meter " + std::to_string(slave) + " waited for later batches: " +
                std::to_string(client->reads(slave)) + " reads");
    }
}

void verifySlavesInterleaveDuringFrameInterval() {
    Shm shm("gw_daemon_modbus_interleave_" + std::to_string(getpid()));
    auto cfg = baseConfig(shm.name);
    cfg.protocol.transport.baudRate = 115200;
    cfg.protocol.transport.frameIntervalMs = 300;
    cfg.collect.maxTasksPerMeterPerCycle = 2;
    for (int slave = 1; slave <= 4; ++slave) cfg.meters.push_back(meter(slave, static_cast<std::uint32_t>(slave * 100), 4, 100));
    auto line = std::make_shared<virtual_rtu::VirtualLine>();
    line->charMs = 10.0 / 115200 * 1000;
    line->alive = {1, 2, 3, 4};
    line->latency = [](int) { return 10; };
    SerialPortOptions options;
    options.device = "virtual";
    options.baudRate = 115200;
    options.timeoutMs = 200;
    options.frameIntervalMs = 300;
    options.readRetryCount = 0;
    auto client = std::make_shared<ModbusRtuClient>(line, options);
    MemoryPointStore store(cfg.memoryStore);
    GatewayDaemon daemon(cfg, store, client);
    daemon.start();
    std::this_thread::sleep_for(std::chrono::seconds(3));
    daemon.stop();
    int total = 0;
    for (int slave = 1; slave <= 4; ++slave) {
        const auto requests = line->requestsBySlave[slave];
        total += requests;
        // Each slave is limited to one request per 300 ms (10 in 3 s); the line itself is nearly idle.
        require(requests >= 8, "slave " + std::to_string(slave) + " was blocked by another slave's frame interval: " +
                std::to_string(requests) + " requests");
    }
    // Sleeping on the line for each slave's frame interval allowed about 20 requests in 3 s.
    require(total >= 32, "slaves did not interleave: " + std::to_string(total) + " requests in 3 s");
}

void verifyWallClockStepDoesNotFreezeCollection() {
    Shm shm("gw_daemon_modbus_clock_" + std::to_string(getpid()));
    auto cfg = baseConfig(shm.name);
    cfg.meters = {meter(1, 1, 1, 100)};
    MemoryPointStore store(cfg.memoryStore);
    auto client = std::make_shared<CountingClient>();
    GatewayDaemon daemon(cfg, store, client);
    daemon.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    gRealtimeOffsetSeconds = -3600;  // NTP or an operator sets the clock back one hour
    const auto before = client->reads(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    const auto after = client->reads(1);
    daemon.stop();
    gRealtimeOffsetSeconds = 0;
    require(after - before >= 5, "collection froze after the wall clock stepped back: " +
            std::to_string(after - before) + " reads in 800 ms");
}

}  // namespace

int main() {
    try {
        verifyOneFailedCycleKeepsDeviceOnline();
        verifyRoundFinishesBeforeSleeping();
        verifySlavesInterleaveDuringFrameInterval();
        verifyWallClockStepDoesNotFreezeCollection();
    } catch (const std::exception& ex) {
        std::cerr << "gateway_daemon_modbus_test failed: " << ex.what() << std::endl;
        return 1;
    }
    std::cout << "gateway_daemon_modbus_test passed" << std::endl;
    return 0;
}
