// Diagnostic-only translation unit: never invoke the coordinator entrypoint.
#define main unusedCoordinatorEntrypoint
#include "../../ems_cluster_main.cpp"
#undef main

#include <ctime>
#include <iomanip>
#include <vector>

using namespace edge_gateway;

namespace probe {
bool countReads = false;
std::uint64_t reads = 0, misses = 0;
volatile double sink = 0;
constexpr std::int64_t now = 100000;
const std::vector<std::uint32_t> capability{724000, 724020, 724021, 724022,
    724023, 724024, 724025, 724026, 724027, 724028,
    724040, 724041, 724042, 724043, 724044, 724045};
const std::vector<std::uint32_t> target{724060, 724061, 724062, 724063, 724064, 724065};

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

double cpuUs() {
    timespec value{};
    require(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) == 0, "CPU clock failed");
    return value.tv_sec * 1e6 + value.tv_nsec / 1e3;
}

template<class Action>
void measure(const std::string& scenario, const std::string& operation, int n, Action action) {
    for (int i = 0; i < 5; ++i) action();
    for (int repeat = 0; repeat < 3; ++repeat) {
        const auto startWall = std::chrono::steady_clock::now();
        const auto startCpu = cpuUs();
        for (int i = 0; i < n; ++i) action();
        const auto cpu = cpuUs() - startCpu;
        const auto wall = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - startWall).count();
        std::cout << std::setprecision(12) << "{\"scenario\":\"" << scenario
            << "\",\"operation\":\"" << operation << "\",\"repeat\":" << repeat
            << ",\"iterations\":" << n << ",\"cpuUsPerCall\":" << cpu / n
            << ",\"wallUsPerCall\":" << wall / n << "}\n";
    }
}

std::vector<StoredPointValue> singles(MemoryPointStore& store,
    const std::vector<std::uint32_t>& indexes, std::int64_t at) {
    std::vector<StoredPointValue> result;
    for (auto index : indexes) {
        auto value = store.getLatestByIndex(index, at);
        if (value) result.push_back(*value);
    }
    return result;
}

void equal(const std::vector<StoredPointValue>& a, const std::vector<StoredPointValue>& b) {
    require(a.size() == b.size(), "single/batch size mismatch");
    for (std::size_t i = 0; i < a.size(); ++i) {
        require(a[i].index == b[i].index && a[i].value == b[i].value &&
            a[i].quality == b[i].quality && a[i].stale == b[i].stale &&
            a[i].ts == b[i].ts && a[i].expireAt == b[i].expireAt &&
            a[i].machineCode == b[i].machineCode && a[i].meterCode == b[i].meterCode &&
            a[i].pointCode == b[i].pointCode, "single/batch value or identity mismatch");
    }
}

struct Segment {
    std::string name = "s2_cpu_probe_" + std::to_string(getpid());
    ~Segment() { MemoryPointStore::cleanupOrphanedSegment(name); }
};

void points() {
    Segment segment;
    EmsClusterConfig config;
    config.enabled = true;
    config.controlEnabled = false;
    config.virtualSharedMemoryName = segment.name;
    config.virtualPointBaseIndex = 724000;
    config.capabilityTtlMs = 1000;
    config.stationTargetTtlMs = 1000;
    EmsClusterPointBridge bridge(config, "LOCAL_DIAGNOSTIC_ONLY");
    bridge.publish({}, {}, now, monotonicNowMs());
    MemoryPointStore store(segment.name);
    auto all = capability;
    all.insert(all.end(), target.begin(), target.end());
    for (int stage = 0; stage < 2; ++stage) {
        const std::string scenario = stage ? "helper_inputs_feedback_absent" : "before_helper_inputs";
        if (stage) {
            for (auto index : all) {
                if (index >= 724040 && index <= 724045) continue;
                PointValue p;
                p.index = index;
                p.value = 1;
                p.quality = 1;
                p.ts = now;
                p.expireAt = now + 1000;
                store.putLatest(p);
            }
        }
        for (auto at : {now, now + 1001}) {
            equal(singles(store, all, at), store.getLatestByIndexes(all, at));
        }
        countReads = true;
        reads = misses = 0;
        bridge.sampleCapability(now);
#ifdef BENCHMARK_BATCH_CAPABILITY
        require(reads == 0 && misses == 0, "batch capability fell back to single API");
#else
        require(reads == 16 && misses == (stage ? 6 : 16), "unexpected capability reads");
#endif
        std::cout << "{\"scenario\":\"" << scenario
            << "\",\"operation\":\"count_actual_capability\",\"reads\":" << reads
            << ",\"misses\":" << misses << "}\n";
        reads = misses = 0;
        bool valid = false;
        bridge.sampleStationTarget(now, valid);
        require(reads == (stage ? 6 : 1) && misses == (stage ? 0 : 1), "unexpected target reads");
        require(valid == (stage != 0), "unexpected target validity");
        std::cout << "{\"scenario\":\"" << scenario
            << "\",\"operation\":\"count_actual_target\",\"reads\":" << reads
            << ",\"misses\":" << misses << "}\n";
        countReads = false;
        measure(scenario, "actual_bridge_capability_plus_target", 200, [&] {
            auto c = bridge.sampleCapability(now);
            bool ok = false;
            auto t = bridge.sampleStationTarget(now, ok);
            sink += c.socPercent + t.paKw + ok;
        });
#ifdef BENCHMARK_BATCH_CAPABILITY
        continue;
#endif
        measure(scenario, "single_16_capability", 200, [&] {
            sink += singles(store, capability, now).size();
        });
        measure(scenario, "batch_16_capability", 200, [&] {
            sink += store.getLatestByIndexes(capability, now).size();
        });
        measure(scenario, "batch_22_primitives_only", 200, [&] {
            sink += store.getLatestByIndexes(all, now).size();
        });
        if (stage) {
            measure(scenario, "single_6_present_targets", 1000, [&] {
                sink += singles(store, target, now).size();
            });
            measure(scenario, "batch_6_present_targets", 1000, [&] {
                sink += store.getLatestByIndexes(target, now).size();
            });
        }
    }
}

void regexes() {
    const std::string health = R"({"ts":100000,"healthy":true,"timeoutPercent":0,"controlQueueP95Ms":2})";
    const std::regex ts("\\\"ts\\\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)");
    const std::regex timeout("\\\"timeoutPercent\\\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)");
    const std::regex queue("\\\"controlQueueP95Ms\\\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)");
    const std::regex healthy("\\\"healthy\\\"\\s*:\\s*(true|false)");
    for (int fresh = 0; fresh < 2; ++fresh) {
        const std::string text = fresh ? health : "";
        auto original = [&] {
            return jsonNumber(text, "ts", 0) + jsonNumber(text, "timeoutPercent", 100) +
                jsonNumber(text, "controlQueueP95Ms", 1000) + (fresh && jsonBool(text, "healthy", false));
        };
        auto reused = [&] {
            std::smatch match;
            auto number = [&](const std::regex& pattern, double fallback) {
                return std::regex_search(text, match, pattern) ? std::stod(match[1].str()) : fallback;
            };
            double result = number(ts, 0) + number(timeout, 100) + number(queue, 1000);
            return result + (fresh && std::regex_search(text, match, healthy) && match[1].str() == "true");
        };
        require(original() == reused(), "regex result mismatch");
        const std::string scenario = fresh ? "health_fresh_4_regexes" : "health_absent_3_regexes";
        measure(scenario, "original_construct_and_search", 500, [&] { sink += original(); });
        measure(scenario, "reused_same_patterns_search", 500, [&] { sink += reused(); });
    }
}
}  // namespace probe

// Linker wrapping observes real bridge calls without changing product sources.
extern "C" Optional<StoredPointValue> realSingle(const MemoryPointStore*, std::uint32_t, std::int64_t)
    asm("__real__ZNK12edge_gateway16MemoryPointStore16getLatestByIndexEjl");
extern "C" Optional<StoredPointValue> wrappedSingle(const MemoryPointStore*, std::uint32_t, std::int64_t)
    asm("__wrap__ZNK12edge_gateway16MemoryPointStore16getLatestByIndexEjl");
extern "C" Optional<StoredPointValue> wrappedSingle(
    const MemoryPointStore* store, std::uint32_t index, std::int64_t at) {
    auto result = realSingle(store, index, at);
    if (probe::countReads) {
        ++probe::reads;
        if (!result) ++probe::misses;
    }
    return result;
}

int main() {
    try {
        probe::points();
#ifndef BENCHMARK_BATCH_CAPABILITY
        probe::regexes();
#endif
        std::cout << "{\"checksPassed\":true,\"sink\":" << probe::sink << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
