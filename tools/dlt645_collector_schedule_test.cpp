// DL/T 645 collector regressions: due-time scheduling with realtime priority on the shipped
// national standard template, block reads, failure thresholds and offline marking, dead meters not
// starving the bus, clock steps.
#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dlt645_test_support.hpp"
#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/dlt645_client.hpp"
#include "edge_gateway/dlt645_collector.hpp"
#include "edge_gateway/memory_point_store.hpp"

using dlt645_test::MeterPort;
using dlt645_test::require;
using namespace edge_gateway;

namespace {

std::string g_root;

struct StoreScope {
    std::string name;
    explicit StoreScope(std::string value) : name(std::move(value)) { MemoryPointStore::cleanupOrphanedSegment(name); }
    ~StoreScope() { MemoryPointStore::cleanupOrphanedSegment(name); }
};

SerialPortOptions fastOptions() {
    SerialPortOptions options;
    options.device = "test";
    options.timeoutMs = 3;
    return options;
}

DeviceConfig meterConfig(const std::string& store, const std::string& address, std::vector<PointDefinition> points) {
    DeviceConfig config;
    config.machineCode = "GW_DLT645_TEST";
    config.meterCode = "METER_" + address;
    config.address = address;
    config.protocol.type = "dlt645_2007";
    config.memoryStore.sharedMemoryName = store;
    config.points = std::move(points);
    return config;
}

std::vector<PointDefinition> standardTemplate(std::uint32_t indexBase) {
    auto points = ConfigLoader::loadDlt645StandardPointsFromFile(
        g_root + "/config/templates/dlt645_2007_standard_points.json");
    for (std::size_t i = 0; i < points.size(); ++i) points[i].index = indexBase + static_cast<std::uint32_t>(i);
    return points;
}

PointDefinition readPoint(std::uint32_t index, const std::string& code, const std::string& di,
                          const std::string& type, int bytes, double scale, int offset = 0) {
    PointDefinition point;
    point.index = index;
    point.pointCode = code;
    point.name = code;
    point.enabled = true;
    point.read.enable = true;
    point.read.dataType = type;
    point.read.dlt645Di = di;
    point.read.dlt645ByteCount = bytes;
    point.read.dlt645Offset = offset;
    point.read.scale = scale;
    point.read.intervalMs = 1000;
    point.read.cachePolicy.storeLatest = true;
    return point;
}

PointDefinition onlinePoint(std::uint32_t index) {
    PointDefinition point;
    point.index = index;
    point.pointCode = "device_online";
    point.enabled = true;
    point.read.enable = true;
    point.read.dataType = "device_online";
    point.read.cachePolicy.storeLatest = true;
    return point;
}

std::int64_t wallNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// The reviewed defect: 276 DIs round-robin at 2 per cycle -> every DI every ~137 s.
void standardTemplateSchedule() {
    StoreScope scope("dlt645_schedule_standard_test");
    MemoryPointStore store(scope.name);
    auto port = std::make_shared<MeterPort>();
    std::int64_t now = 0;
    port->clock = &now;
    auto client = std::make_shared<Dlt645Client>(port, fastOptions());
    Dlt645Collector collector(meterConfig(scope.name, "000000000001", standardTemplate(200000)), store, client);
    std::map<std::string, std::vector<std::int64_t>> reads;
    for (int cycle = 0; cycle < 600; ++cycle) {
        now = 1000000 + cycle * 1000LL;
        const auto before = port->requests.size();
        collector.collectOnce(now);
        for (std::size_t i = before; i < port->requests.size(); ++i) {
            reads[port->requests[i].substr(13, 8)].push_back(now);
        }
    }
    const auto maxGap = [&reads](const std::string& di) {
        const auto& times = reads[di];
        require(times.size() >= 2, "DI " + di + " was not read repeatedly");
        std::int64_t gap = times.front() - 1000000;
        for (std::size_t i = 1; i < times.size(); ++i) gap = std::max(gap, times[i] - times[i - 1]);
        return gap;
    };
    // 23 realtime DIs (U, I, P, Q, S, PF, f) at 2 per cycle: ~12 s each.
    for (const auto* di : {"02010100", "02020300", "02030000", "02050000", "02060000", "02800002"}) {
        require(maxGap(di) <= 13000, std::string("realtime DI ") + di + " refreshed too slowly: " +
                std::to_string(maxGap(di)) + " ms");
    }
    // Energy registers are background (collectPriority 1, 60 s): one slot every 5 s for 12 DIs.
    for (const auto* di : {"00010000", "00020000", "00040000"}) {
        require(maxGap(di) <= 70000, std::string("energy DI ") + di + " starved: " + std::to_string(maxGap(di)));
    }
    // Settlement-day data is disabled by default; phase angles too.
    require(reads.find("00010001") == reads.end() && reads.find("0004040C") == reads.end(),
            "settlement-day DIs must not be polled by default");
    require(reads.find("02070100") == reads.end(), "phase angle points are disabled by default");
    // 2 realtime + 1 background slot every 5 s per cycle at most.
    require(port->requests.size() <= 600 * 3, "per-cycle budget exceeded");

    // Values decode with the corrected DIs and signs (mock answers like a standard meter).
    const auto at = [&store](std::uint32_t index) { return store.getLatestByIndex(index, wallNow()); };
    const auto points = standardTemplate(200000);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto& p = points[i];
        if (p.pointCode == "KY02100110") require(at(200000 + i) && std::abs(at(200000 + i)->value - 1.646) < 1e-9, "total apparent power from 02050000");
        if (p.pointCode == "KY02100710") require(at(200000 + i) && std::abs(at(200000 + i)->value - 0.925) < 1e-9, "total power factor from 02060000");
        if (p.pointCode == "KY02100026") require(at(200000 + i) && std::abs(at(200000 + i)->value + 10.345) < 1e-9, "C phase current is signed");
        if (p.pointCode == "KY02100911") require(at(200000 + i) && at(200000 + i)->value < 0.0, "combined reactive 2 energy is signed");
    }
}

void realtimeBeforeBackground() {
    StoreScope scope("dlt645_schedule_priority_test");
    MemoryPointStore store(scope.name);
    auto port = std::make_shared<MeterPort>();
    auto client = std::make_shared<Dlt645Client>(port, fastOptions());
    auto energy = readPoint(1, "energy", "00010000", "dlt645_bcd", 4, 0.01);
    energy.collectPriority = 1;
    auto config = meterConfig(scope.name, "1", {energy, readPoint(2, "ua", "02010100", "dlt645_bcd", 2, 0.1),
                                                 readPoint(3, "ub", "02010200", "dlt645_bcd", 2, 0.1)});
    config.collect.maxTasksPerMeterPerCycle = 2;
    config.collect.maxBackgroundTasksPerMeterPerCycle = 0;
    Dlt645Collector collector(config, store, client);
    collector.collectOnce(1000);
    require(port->requests.size() == 2 && port->count("02010100") == 1 && port->count("02010200") == 1,
            "realtime DIs come first even when the background one is listed first");
    collector.collectOnce(1500);  // realtime not due yet: the unused slots serve background
    require(port->count("00010000") == 1, "unused realtime slots go to due background DIs");
}

void blockRead() {
    StoreScope scope("dlt645_schedule_block_test");
    MemoryPointStore store(scope.name);
    auto port = std::make_shared<MeterPort>();
    auto client = std::make_shared<Dlt645Client>(port, fastOptions());
    Dlt645Collector collector(meterConfig(scope.name, "1", {
        readPoint(11, "ua", "0201FF00", "dlt645_bcd", 2, 0.1, 0),
        readPoint(12, "ub", "0201FF00", "dlt645_bcd", 2, 0.1, 2),
        readPoint(13, "uc", "0201FF00", "dlt645_bcd", 2, 0.1, 4)}), store, client);
    const auto result = collector.collectOnce(1000);
    require(port->requests.size() == 1, "one block request serves all three points");
    require(result.values.size() == 3, "three values from one block");
    require(std::abs(result.values[0].value - 220.5) < 1e-9 && std::abs(result.values[1].value - 221.3) < 1e-9 &&
            std::abs(result.values[2].value - 219.8) < 1e-9, "Ua/Ub/Uc must come from their own offsets");
}

void offlineThresholds() {
    StoreScope scope("dlt645_schedule_offline_test");
    MemoryPointStore store(scope.name);
    auto port = std::make_shared<MeterPort>();
    auto client = std::make_shared<Dlt645Client>(port, fastOptions());
    auto config = meterConfig(scope.name, "1", {readPoint(21, "ua", "02010100", "dlt645_bcd", 2, 0.1),
                                                readPoint(22, "ia", "02020100", "dlt645_bcd_signed", 3, 0.001),
                                                readPoint(23, "f", "02800002", "dlt645_bcd", 2, 0.01),
                                                onlinePoint(29)});
    config.collect.maxTasksPerMeterPerCycle = 3;
    config.collect.offlineFailureThreshold = 3;
    config.collect.failureBadQualityThreshold = 3;
    config.collect.slaveFailureBackoffThreshold = 100;
    config.protocol.transport.readRetryCount = 0;
    Dlt645Collector collector(config, store, client);
    collector.collectOnce(1000);
    const auto quality = [&store](std::uint32_t index) {
        const auto value = store.getLatestByIndex(index, wallNow());
        return value ? value->quality : -1;
    };
    const auto onlineValue = [&store]() {
        const auto value = store.getLatestByIndex(29, wallNow());
        return value ? value->value : -1.0;
    };
    require(quality(21) == 1 && quality(22) == 1 && quality(23) == 1 && onlineValue() == 1.0, "meter online");

    port->alive = false;
    std::int64_t now = 2000;
    for (int cycle = 1; cycle <= 2; ++cycle, now += 1000) {
        bool threw = false;
        try { collector.collectOnce(now); } catch (const std::exception&) { threw = true; }
        require(!threw, "failures below offlineFailureThreshold must not be reported as offline");
        require(port->requests.size() == 3 + static_cast<std::size_t>(cycle), "dead meter: one request per cycle");
        require(quality(21) == 1 && quality(22) == 1 && quality(23) == 1, "good values kept below the thresholds");
        require(onlineValue() == 1.0, "device_online stays 1 below offlineFailureThreshold");
    }
    bool threw = false;
    try { collector.collectOnce(now); } catch (const std::exception&) { threw = true; }
    require(threw && !collector.meterOnline(), "offlineFailureThreshold reached: meter offline");
    require(quality(21) == 0 && quality(22) == 0 && quality(23) == 0, "every point of an offline meter is bad quality");
    require(onlineValue() == 0.0, "device_online=0 when offline");

    port->alive = true;
    now += 1000;
    collector.collectOnce(now);
    require(collector.meterOnline() && onlineValue() == 1.0, "meter back online after recoverySuccessThreshold");
}

void deadMeterDoesNotStarveBus() {
    StoreScope scopeA("dlt645_schedule_dead_a_test");
    StoreScope scopeB("dlt645_schedule_dead_b_test");
    MemoryPointStore storeA(scopeA.name);
    MemoryPointStore storeB(scopeB.name);
    auto port = std::make_shared<MeterPort>();
    auto client = std::make_shared<Dlt645Client>(port, fastOptions());
    auto dead = meterConfig(scopeA.name, "1", {readPoint(31, "ua", "02010100", "dlt645_bcd", 2, 0.1)});
    auto alive = meterConfig(scopeB.name, "2", {readPoint(41, "ua", "02010100", "dlt645_bcd", 2, 0.1)});
    Dlt645Collector deadCollector(dead, storeA, client);
    Dlt645Collector aliveCollector(alive, storeB, client);
    port->alive = true;
    for (int cycle = 0; cycle < 10; ++cycle) {
        const auto now = 1000 + cycle * 1000LL;
        port->alive = false;  // meter 1 never answers
        try { deadCollector.collectOnce(now); } catch (const std::exception&) {}
        port->alive = true;
        aliveCollector.collectOnce(now);
    }
    std::size_t deadRequests = 0;
    for (const auto& request : port->requests) {
        if (request.compare(0, 12, "000000000001") == 0) ++deadRequests;
    }
    require(port->count("02010100") - deadRequests == 10, "live meter must be read every cycle");
    require(deadRequests <= 10, "dead meter costs at most one request per cycle");
}

void clockStepShiftsBackgroundSchedule() {
    StoreScope scope("dlt645_schedule_clock_test");
    MemoryPointStore store(scope.name);
    auto port = std::make_shared<MeterPort>();
    auto client = std::make_shared<Dlt645Client>(port, fastOptions());
    auto energy = readPoint(51, "energy", "00010000", "dlt645_bcd", 4, 0.01);
    energy.collectPriority = 1;
    energy.read.intervalMs = 60000;
    auto energy2 = readPoint(52, "energy2", "00020000", "dlt645_bcd", 4, 0.01);
    energy2.collectPriority = 1;
    energy2.read.intervalMs = 60000;
    // Two realtime DIs that are always due keep the realtime budget busy, so background reads only
    // get the gated slot (backgroundTaskIntervalMs).
    auto ua = readPoint(53, "ua", "02010100", "dlt645_bcd", 2, 0.1);
    auto ub = readPoint(54, "ub", "02010200", "dlt645_bcd", 2, 0.1);
    ua.read.intervalMs = 1;
    ub.read.intervalMs = 1;
    auto config = meterConfig(scope.name, "1", {ua, ub, energy, energy2});
    config.collect.maxTasksPerMeterPerCycle = 1;
    config.collect.maxBackgroundTasksPerMeterPerCycle = 1;
    config.collect.backgroundTaskIntervalMs = 5000;
    Dlt645Collector collector(config, store, client);
    const std::int64_t start = 10000000;
    collector.collectOnce(start);      // ua + energy (gated background slot)
    collector.collectOnce(start + 1);  // ub, background gate closed
    require(port->count("00010000") == 1 && port->count("00020000") == 0, "one background slot per interval");
    // Wall clock steps back by one hour: the background gate moves with it.
    const auto stepped = start - 3600000;
    collector.collectOnce(stepped);
    require(port->count("00020000") == 0, "gate interval still applies right after the step");
    collector.collectOnce(stepped + 5000);
    require(port->count("00020000") == 1, "background reads must not freeze for an hour after the clock steps back");
}

void retryThenMoveOn() {
    StoreScope scope("dlt645_schedule_retry_test");
    MemoryPointStore store(scope.name);
    auto port = std::make_shared<MeterPort>();
    port->alive = false;
    auto client = std::make_shared<Dlt645Client>(port, fastOptions());
    auto config = meterConfig(scope.name, "1", {readPoint(61, "a", "02010100", "dlt645_bcd", 2, 0.1),
                                                readPoint(62, "b", "02010200", "dlt645_bcd", 2, 0.1)});
    config.points[0].read.intervalMs = 100;
    config.points[1].read.intervalMs = 100;
    config.protocol.transport.readRetryCount = 1;
    config.collect.maxTasksPerMeterPerCycle = 3;
    config.collect.slaveFailureBackoffThreshold = 100;
    Dlt645Collector collector(config, store, client);
    for (const auto now : {1000LL, 1100LL, 1200LL}) {
        try { collector.collectOnce(now); } catch (const std::exception&) {}
    }
    require(port->requests.size() == 3, "one request per failed cycle");
    require(port->requests[0] == port->requests[1], "readRetryCount retries the same DI next cycle");
    require(port->requests[1] != port->requests[2], "after the retries the next DI gets its turn");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: dlt645_collector_schedule_test <repo root>" << std::endl;
        return 2;
    }
    g_root = argv[1];
    try {
        standardTemplateSchedule();
        realtimeBeforeBackground();
        blockRead();
        offlineThresholds();
        deadMeterDoesNotStarveBus();
        clockStepShiftsBackgroundSchedule();
        retryThenMoveOn();
        std::cout << "dlt645_collector_schedule_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "dlt645_collector_schedule_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
