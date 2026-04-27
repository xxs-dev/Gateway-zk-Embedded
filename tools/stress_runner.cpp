#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/mqtt_driver_service.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace {

using namespace edge_gateway;

struct RuntimePoint {
    std::string machineCode;
    std::string meterCode;
    std::string sharedMemoryName;
    PointDefinition point;
    MemoryPointStore* store = nullptr;
};

struct TimingStats {
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint64_t> totalUs{0};
    std::atomic<std::uint64_t> maxUs{0};

    void record(std::uint64_t us) {
        count.fetch_add(1, std::memory_order_relaxed);
        totalUs.fetch_add(us, std::memory_order_relaxed);
        std::uint64_t observed = maxUs.load(std::memory_order_relaxed);
        while (observed < us &&
               !maxUs.compare_exchange_weak(observed, us, std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
    }
};

class NullMqttDriverPublisher : public IMqttDriverPublisher {
public:
    void publishFullSnapshot(
        const std::string&,
        const std::vector<StoredPointValue>&
    ) override {
    }

    void publishAlarm(
        const std::string&,
        std::uint32_t,
        const StoredPointValue&,
        const std::string&,
        bool
    ) override {
    }

    void publishOnDemand(
        const std::string&,
        const std::vector<StoredPointValue>&
    ) override {
    }

    void publishChangeEvent(
        const std::string&,
        const StoredPointValue&
    ) override {
    }

    void publishCommandReply(
        const std::string&,
        const MqttCommandReply&
    ) override {
    }

    void publishOtaReply(
        const std::string&,
        const OtaReply&
    ) override {
    }

    void publishOtaStatus(
        const std::string&,
        const OtaStatus&
    ) override {
    }

    void publishJsonMessage(
        const std::string&,
        const std::string&
    ) override {
    }

    std::vector<MqttIncomingMessage> pollIncoming(int) override {
        return {};
    }
};

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::uint64_t elapsedUs(std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start
        ).count()
    );
}

void appendPoints(const DeviceConfig& config, std::vector<RuntimePoint>& runtimePoints) {
    const auto sharedMemoryName = config.memoryStore.sharedMemoryName;
    if (!config.meters.empty()) {
        for (const auto& device : config.meters) {
            for (const auto& point : device.points) {
                RuntimePoint item;
                item.machineCode = config.machineCode;
                item.meterCode = device.meterCode;
                item.sharedMemoryName = sharedMemoryName;
                item.point = point;
                runtimePoints.push_back(item);
            }
        }
    } else {
        for (const auto& point : config.points) {
            RuntimePoint item;
            item.machineCode = config.machineCode;
            item.meterCode = config.meterCode;
            item.sharedMemoryName = sharedMemoryName;
            item.point = point;
            runtimePoints.push_back(item);
        }
    }
}

std::vector<RuntimePoint> expandPoints(const std::vector<RuntimePoint>& seedPoints, std::size_t targetCount) {
    if (seedPoints.empty() || targetCount == 0 || targetCount <= seedPoints.size()) {
        return seedPoints;
    }

    std::vector<RuntimePoint> expanded;
    expanded.reserve(targetCount);
    std::unordered_map<std::uint32_t, bool> usedIndexes;
    usedIndexes.reserve(targetCount);

    std::uint32_t maxIndex = 0;
    for (std::size_t i = 0; i < seedPoints.size(); ++i) {
        expanded.push_back(seedPoints[i]);
        usedIndexes[seedPoints[i].point.index] = true;
        maxIndex = std::max(maxIndex, seedPoints[i].point.index);
    }

    std::size_t generated = seedPoints.size();
    std::size_t round = 1;
    while (generated < targetCount) {
        for (std::size_t i = 0; i < seedPoints.size() && generated < targetCount; ++i) {
            RuntimePoint item = seedPoints[i];
            std::uint32_t candidateIndex = maxIndex + 1;
            while (usedIndexes.find(candidateIndex) != usedIndexes.end()) {
                ++candidateIndex;
            }
            maxIndex = candidateIndex;
            usedIndexes[candidateIndex] = true;

            item.point.index = candidateIndex;
            item.point.pointCode = item.point.pointCode + "_x" + std::to_string(round) + "_" + std::to_string(i);
            item.point.name = item.point.name + " x" + std::to_string(round);
            item.point.desc = item.point.desc + " x" + std::to_string(round);
            item.point.address = item.point.address + static_cast<int>((generated / seedPoints.size()) % 1000);
            item.meterCode = item.meterCode + "_X" + std::to_string(round);
            expanded.push_back(item);
            ++generated;
        }
        ++round;
    }

    return expanded;
}

void registerPoints(MemoryPointStore& store, const std::vector<RuntimePoint*>& points) {
    for (std::size_t i = 0; i < points.size(); ++i) {
        store.registerPoint(points[i]->machineCode, points[i]->meterCode, points[i]->point);
    }
}

double makeValue(std::uint32_t index, std::uint64_t sequence) {
    return static_cast<double>((index % 1000U) + ((sequence * 17U) % 500U));
}

void printUsage() {
    std::cout
        << "stress_runner --device-config <path> [--device-config <path> ...] "
        << "[--app-config <path>] [--duration-sec <n>] [--writer-threads <n>] "
        << "[--points <n>] [--snapshot-interval-ms <n>] [--mqtt-driver-cycle-interval-ms <n>] "
        << "[--shm <name>]" << std::endl;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::vector<std::string> deviceConfigPaths;
    std::string appConfigPath;
    std::string shmOverride;
    int durationSec = 30;
    int writerThreads = 1;
    std::size_t pointLimit = 0;
    std::size_t expandPointCount = 0;
    int snapshotIntervalMs = 1000;
    int mqttDriverCycleIntervalMs = 1000;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--device-config" && i + 1 < argc) {
            deviceConfigPaths.push_back(argv[++i]);
        } else if (arg == "--app-config" && i + 1 < argc) {
            appConfigPath = argv[++i];
        } else if (arg == "--duration-sec" && i + 1 < argc) {
            durationSec = std::atoi(argv[++i]);
        } else if (arg == "--writer-threads" && i + 1 < argc) {
            writerThreads = std::atoi(argv[++i]);
        } else if (arg == "--points" && i + 1 < argc) {
            pointLimit = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--expand-points" && i + 1 < argc) {
            expandPointCount = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--snapshot-interval-ms" && i + 1 < argc) {
            snapshotIntervalMs = std::atoi(argv[++i]);
        } else if ((arg == "--mqtt-driver-cycle-interval-ms" || arg == "--mqtt-scan-interval-ms") && i + 1 < argc) {
            mqttDriverCycleIntervalMs = std::atoi(argv[++i]);
        } else if (arg == "--shm" && i + 1 < argc) {
            shmOverride = argv[++i];
        } else if (arg == "--help") {
            printUsage();
            return 0;
        }
    }

    if (deviceConfigPaths.empty()) {
        printUsage();
        return 1;
    }

    auto deviceConfigs = ConfigLoader::loadMany(deviceConfigPaths);
    if (!shmOverride.empty()) {
        for (auto& config : deviceConfigs) {
            config.memoryStore.sharedMemoryName = shmOverride;
        }
    }
    std::vector<RuntimePoint> points;
    for (std::size_t i = 0; i < deviceConfigs.size(); ++i) {
        appendPoints(deviceConfigs[i], points);
    }
    if (points.empty()) {
        throw std::runtime_error("no points loaded from device config");
    }

    if (expandPointCount > 0) {
        points = expandPoints(points, expandPointCount);
    }

    if (pointLimit > 0 && pointLimit < points.size()) {
        points.resize(pointLimit);
    }

    std::unordered_map<std::string, MemoryStoreConfig> storeConfigByShm;
    for (const auto& config : deviceConfigs) {
        auto storeConfig = config.memoryStore;
        if (!shmOverride.empty()) {
            storeConfig.sharedMemoryName = shmOverride;
        }
        if (storeConfig.sharedMemoryName.empty()) {
            storeConfig.sharedMemoryName = "gateway_stress_store";
        }
        storeConfigByShm.emplace(storeConfig.sharedMemoryName, storeConfig);
    }
    for (auto& point : points) {
        if (!shmOverride.empty()) {
            point.sharedMemoryName = shmOverride;
        }
        if (point.sharedMemoryName.empty()) {
            point.sharedMemoryName = deviceConfigs.front().memoryStore.sharedMemoryName.empty()
                ? "gateway_stress_store"
                : deviceConfigs.front().memoryStore.sharedMemoryName;
        }
    }

    std::unordered_map<std::string, std::vector<RuntimePoint*>> pointsByShm;
    for (auto& point : points) {
        pointsByShm[point.sharedMemoryName].push_back(&point);
    }

    std::map<std::string, std::unique_ptr<MemoryPointStore>> stores;
    PointStoreRouter router;
    for (const auto& entry : pointsByShm) {
        MemoryPointStore::cleanupOrphanedSegment(entry.first);
        auto storeConfigIt = storeConfigByShm.find(entry.first);
        MemoryStoreConfig storeConfig = storeConfigIt == storeConfigByShm.end()
            ? deviceConfigs.front().memoryStore
            : storeConfigIt->second;
        storeConfig.sharedMemoryName = entry.first;
        storeConfig.maxLatestPoints = std::max<std::size_t>(storeConfig.maxLatestPoints, entry.second.size());
        stores.emplace(entry.first, std::unique_ptr<MemoryPointStore>(new MemoryPointStore(storeConfig)));
        registerPoints(*stores[entry.first], entry.second);
        for (auto* point : entry.second) {
            point->store = stores[entry.first].get();
        }
        router.addStore(entry.first, *stores[entry.first]);
    }
    router.addRoutesFromDeviceConfigs(deviceConfigs, deviceConfigs.front().memoryStore.sharedMemoryName);

    std::unique_ptr<MqttDriverService> mqttService;
    if (!appConfigPath.empty()) {
        const auto appConfig = ConfigLoader::loadAppConfigFromFile(appConfigPath);
        mqttService.reset(new MqttDriverService(
            appConfig.mqtt,
            appConfig.mqttDriver,
            deviceConfigs,
            router,
            std::shared_ptr<IMqttDriverPublisher>(new NullMqttDriverPublisher())
        ));
    }

    const auto ttlMs = points.front().point.read.cachePolicy.ttlMs > 0
        ? points.front().point.read.cachePolicy.ttlMs
        : deviceConfigs.front().memoryStore.defaultTtlMs;

    std::atomic<bool> running{true};
    std::atomic<std::uint64_t> writeOps{0};
    std::atomic<std::uint64_t> snapshotPoints{0};
    std::atomic<std::uint64_t> mqttDriverCycles{0};
    TimingStats writeStats;
    TimingStats snapshotStats;
    TimingStats mqttStats;

    std::vector<std::thread> workers;
    const auto safeWriterThreads = std::max(1, writerThreads);
    for (int threadIndex = 0; threadIndex < safeWriterThreads; ++threadIndex) {
        workers.push_back(std::thread([&, threadIndex]() {
            std::uint64_t sequence = static_cast<std::uint64_t>(threadIndex) * 1000000ULL;
            while (running.load(std::memory_order_relaxed)) {
                const auto start = std::chrono::steady_clock::now();
                for (std::size_t i = static_cast<std::size_t>(threadIndex); i < points.size(); i += static_cast<std::size_t>(safeWriterThreads)) {
                    const auto ts = nowMs();
                    PointValue value;
                    value.index = points[i].point.index;
                    value.machineCode = points[i].machineCode;
                    value.meterCode = points[i].meterCode;
                    value.pointCode = points[i].point.pointCode;
                    value.pointName = points[i].point.name;
                    value.category = points[i].point.category;
                    value.unit = points[i].point.read.unit;
                    value.value = makeValue(points[i].point.index, sequence);
                    value.quality = 1;
                    value.ts = ts;
                    value.expireAt = ts + ttlMs;
                    value.isStore = points[i].point.isStore;
                    value.persistIntervalSec = points[i].point.persistIntervalSec;
                    value.function = points[i].point.read.function;
                    value.address = points[i].point.address;
                    value.length = points[i].point.read.length;
                    if (points[i].store == nullptr) {
                        throw std::runtime_error("runtime point has no target store");
                    }
                    points[i].store->putLatest(value);
                    writeOps.fetch_add(1, std::memory_order_relaxed);
                    ++sequence;
                }
                writeStats.record(elapsedUs(start));
            }
        }));
    }

    std::thread snapshotThread([&]() {
        const auto interval = std::chrono::milliseconds(std::max(100, snapshotIntervalMs));
        while (running.load(std::memory_order_relaxed)) {
            const auto start = std::chrono::steady_clock::now();
            const auto values = router.getAllLatest(nowMs());
            snapshotPoints.fetch_add(values.size(), std::memory_order_relaxed);
            snapshotStats.record(elapsedUs(start));
            std::this_thread::sleep_for(interval);
        }
    });

    std::thread mqttThread;
    if (mqttService) {
        mqttThread = std::thread([&]() {
            const auto interval = std::chrono::milliseconds(std::max(100, mqttDriverCycleIntervalMs));
            while (running.load(std::memory_order_relaxed)) {
                const auto start = std::chrono::steady_clock::now();
                mqttService->runScanOnce(nowMs());
                mqttDriverCycles.fetch_add(1, std::memory_order_relaxed);
                mqttStats.record(elapsedUs(start));
                std::this_thread::sleep_for(interval);
            }
        });
    }

    std::cout << "stress started"
              << " points=" << points.size()
              << " writers=" << safeWriterThreads
              << " durationSec=" << durationSec
              << " shmCount=" << stores.size()
              << " mqttDriverCycle=" << (mqttService ? "on" : "off")
              << std::endl;

    std::this_thread::sleep_for(std::chrono::seconds(std::max(1, durationSec)));
    running.store(false, std::memory_order_relaxed);

    for (std::size_t i = 0; i < workers.size(); ++i) {
        workers[i].join();
    }
    snapshotThread.join();
    if (mqttThread.joinable()) {
        mqttThread.join();
    }

    for (const auto& entry : pointsByShm) {
        MemoryPointStore::cleanupOrphanedSegment(entry.first);
    }

    const auto writeCount = writeStats.count.load();
    const auto snapshotCount = snapshotStats.count.load();
    const auto mqttCount = mqttStats.count.load();

    std::cout << "stress result" << std::endl;
    std::cout << "  writeOps=" << writeOps.load()
              << " opsPerSec=" << (writeOps.load() / static_cast<std::uint64_t>(std::max(1, durationSec)))
              << " batchAvgUs=" << (writeCount == 0 ? 0 : writeStats.totalUs.load() / writeCount)
              << " batchMaxUs=" << writeStats.maxUs.load()
              << std::endl;
    std::cout << "  snapshots=" << snapshotCount
              << " snapshotAvgUs=" << (snapshotCount == 0 ? 0 : snapshotStats.totalUs.load() / snapshotCount)
              << " snapshotMaxUs=" << snapshotStats.maxUs.load()
              << " snapshotPoints=" << snapshotPoints.load()
              << std::endl;
    if (mqttService) {
        std::cout << "  mqttDriverCycles=" << mqttDriverCycles.load()
                  << " mqttDriverAvgUs=" << (mqttCount == 0 ? 0 : mqttStats.totalUs.load() / mqttCount)
                  << " mqttDriverMaxUs=" << mqttStats.maxUs.load()
                  << std::endl;
    }

    return 0;
}
