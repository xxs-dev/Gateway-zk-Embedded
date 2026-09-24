#include "edge_gateway/ems_cluster_points.hpp"
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unistd.h>

using namespace edge_gateway;
namespace {
const std::vector<std::uint32_t> offsets{0, 20, 21, 22, 23, 24, 25, 26, 27, 28, 40, 41, 42, 43, 44, 45};
constexpr std::uint32_t base = 924000;
constexpr std::int64_t now = 100000;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void emit(const std::string& name, const EmsClusterCapability& c) {
    std::cout << std::setprecision(17) << "CASE " << name << ' '
        << c.socPercent << ' ' << c.ratedActivePowerKw << ' ' << c.ratedApparentPowerKva << ' '
        << c.availableChargePowerKw << ' ' << c.availableDischargePowerKw << ' '
        << c.availableReactivePowerKvar << ' ' << c.controlEnabled << ' ' << c.ready << ' '
        << c.interlocked << ' ' << c.manualOverride << ' ' << c.actual.paKw << ' '
        << c.actual.pbKw << ' ' << c.actual.pcKw << ' ' << c.actual.qaKvar << ' '
        << c.actual.qbKvar << ' ' << c.actual.qcKvar << '\n';
}
PointValue point(std::uint32_t offset) {
    PointValue p;
    p.index = base + offset;
    p.machineCode = "TEST_MACHINE";
    p.meterCode = "EMS_CLUSTER";
    p.pointCode = "POINT_" + std::to_string(offset);
    p.value = offset == 0 || offset == 26 ? 1 : (offset == 27 || offset == 28 ? 0 : offset);
    p.quality = 1;
    p.ts = now;
    p.expireAt = now + 10000;
    return p;
}
struct Segment {
    std::string name = "capability_snapshot_" + std::to_string(getpid());
    ~Segment() { MemoryPointStore::cleanupOrphanedSegment(name); }
};
}

#ifdef GATEWAY_CAPABILITY_READ_PROBE
static bool observe = false;
static unsigned singles = 0, batches = 0;
static std::vector<std::uint32_t> sampledIndexes;
extern "C" Optional<StoredPointValue> realSingle(const MemoryPointStore*, std::uint32_t, std::int64_t)
    asm("__real__ZNK12edge_gateway16MemoryPointStore16getLatestByIndexEjl");
extern "C" Optional<StoredPointValue> wrapSingle(const MemoryPointStore*, std::uint32_t, std::int64_t)
    asm("__wrap__ZNK12edge_gateway16MemoryPointStore16getLatestByIndexEjl");
extern "C" Optional<StoredPointValue> wrapSingle(const MemoryPointStore* store, std::uint32_t index, std::int64_t at) {
    if (observe) { ++singles; sampledIndexes.push_back(index); }
    return realSingle(store, index, at);
}
extern "C" std::vector<StoredPointValue> realBatch(const MemoryPointStore*, const std::vector<std::uint32_t>&, std::int64_t)
    asm("__real__ZNK12edge_gateway16MemoryPointStore18getLatestByIndexesERKSt6vectorIjSaIjEEl");
extern "C" std::vector<StoredPointValue> wrapBatch(const MemoryPointStore*, const std::vector<std::uint32_t>&, std::int64_t)
    asm("__wrap__ZNK12edge_gateway16MemoryPointStore18getLatestByIndexesERKSt6vectorIjSaIjEEl");
extern "C" std::vector<StoredPointValue> wrapBatch(const MemoryPointStore* store,
    const std::vector<std::uint32_t>& indexes, std::int64_t at) {
    if (observe) { ++batches; sampledIndexes.insert(sampledIndexes.end(), indexes.begin(), indexes.end()); }
    return realBatch(store, indexes, at);
}
#endif

int main() {
    try {
        Segment segment;
        EmsClusterConfig config;
        config.enabled = config.controlEnabled = true;
        config.virtualPointBaseIndex = base;
        config.virtualSharedMemoryName = segment.name;
        config.capabilityTtlMs = 1000;
        EmsClusterPointBridge bridge(config, "TEST_MACHINE");
        MemoryPointStore writer(segment.name);
#ifdef GATEWAY_CAPABILITY_READ_PROBE
        observe = true;
#endif
        const auto absent = bridge.sampleCapability(now);
#ifdef GATEWAY_CAPABILITY_READ_PROBE
        observe = false;
#endif
        require(!absent.ready && !absent.controlEnabled && absent.interlocked &&
            !absent.manualOverride && absent.actual.paKw == 0, "missing safety defaults");
        emit("all_missing", absent);
        for (auto offset : offsets) {
            writer.putLatest(point(offset));
            emit("late_" + std::to_string(offset), bridge.sampleCapability(now));
        }
        const auto full = bridge.sampleCapability(now);
        require(full.ready && full.controlEnabled && !full.interlocked && !full.manualOverride,
            "valid capability flags");
        require(full.actual.paKw == 40 && full.actual.pbKw == 41 && full.actual.pcKw == 42 &&
            full.actual.qaKvar == 43 && full.actual.qbKvar == 44 && full.actual.qcKvar == 45,
            "feedback index mapping");
        for (auto offset : offsets) {
            for (int variant = 0; variant < 18; ++variant) {
                auto p = point(offset);
                switch (variant) {
                case 0: p.quality = 0; break;
                case 1: p.quality = -1; break;
                case 2: p.value = std::numeric_limits<double>::quiet_NaN(); break;
                case 3: p.value = std::numeric_limits<double>::infinity(); break;
                case 4: p.value = -std::numeric_limits<double>::infinity(); break;
                case 5: p.ts = 0; break;
                case 6: p.ts = now + 1000; break;
                case 7: p.ts = now + 1001; break;
                case 8: p.ts = now - 1000; break;
                case 9: p.ts = now - 1001; break;
                case 10: p.expireAt = now; break;
                case 11: p.expireAt = now - 1; break;
                case 12: p.stale = true; break;
                case 13: p.value = -2; break;
                case 14: p.value = 120; break;
                case 15: p.value = 0.499; break;
                case 16: p.value = 0.5; break;
                case 17: p.expireAt = 0; p.ts = -1; break;
                }
                writer.putLatest(p);
                emit(std::to_string(offset) + "_variant_" + std::to_string(variant), bridge.sampleCapability(now));
                writer.putLatest(point(offset));
            }
        }
        // A different numeric index must not satisfy a capability by textual identity.
        writer.removeExpired(now + 20000);
        emit("deleted_cached_slots", bridge.sampleCapability(now));
        for (auto offset : offsets) {
            auto unrelated = point(offset);
            unrelated.index += 100;
            writer.putLatest(unrelated);
        }
        const auto reused = bridge.sampleCapability(now);
        require(!reused.ready && reused.interlocked && reused.actual.paKw == 0,
            "reused slot leaked unrelated index");
        emit("reused_by_unrelated_indexes", reused);
        for (auto offset : offsets) writer.putLatest(point(offset));
        require(bridge.sampleCapability(now).ready, "late reinsert was negatively cached");
        emit("reinsert_after_reuse", bridge.sampleCapability(now));
        config.controlEnabled = false;
        EmsClusterPointBridge disabled(config, "TEST_MACHINE");
        require(!disabled.sampleCapability(now).controlEnabled, "configuration disable bypass");
        emit("config_disabled", disabled.sampleCapability(now));

        bool valid = true;
        auto target = bridge.sampleStationTarget(now, valid);
        require(!valid && target.qcKvar == 0, "missing target short circuit");
        for (std::uint32_t offset = 60; offset <= 65; ++offset) writer.putLatest(point(offset));
        auto bad = point(62);
        bad.quality = 0;
        writer.putLatest(bad);
        target = bridge.sampleStationTarget(now, valid);
        require(!valid && target.paKw == 60 && target.pbKw == 61 && target.pcKw == 0 &&
            target.qaKvar == 0 && target.qcKvar == 0, "target partial short circuit changed");

#ifdef GATEWAY_CAPABILITY_READ_PROBE
        std::vector<std::uint32_t> expected;
        for (auto offset : offsets) expected.push_back(base + offset);
        require(sampledIndexes == expected, "capability requested wrong indexes/order");
        std::cout << "READ_COUNTS single=" << singles << " batch=" << batches << '\n';
        require(singles == 0 && batches == 1, "capability must issue one batch and zero single reads");
#endif
        std::cout << "PASS capability snapshot semantics\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
