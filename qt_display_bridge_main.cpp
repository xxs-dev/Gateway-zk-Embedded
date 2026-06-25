#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/prctl.h>
#include <sys/shm.h>
#include <unistd.h>
#endif

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace {

constexpr int kLegacyShmKey = 0x1234;
constexpr int kLegacyShmWordSize = 10000;
constexpr int kLegacyShmLockCount = 10;
constexpr int kLegacyShmDataPerLock = 1000;
constexpr int kLegacyShmUpdateFlagWords = kLegacyShmWordSize / 32 + 1;
constexpr int kBridgePhysicalIndexBase = 1000;

std::atomic<bool> g_running(true);

struct LegacySharedMemory {
    pthread_rwlock_t rwlocks[kLegacyShmLockCount];
    pthread_mutex_t di_mutex;
    int initialized;
    double data[kLegacyShmWordSize];
    int initial_polling_done;
    std::atomic<int> dataUpdateFlag;
    std::atomic<int> dataIndexFlag[kLegacyShmUpdateFlagWords];
} __attribute__((aligned(64)));

struct Mapping {
    int appIndex = 0;
    int physicalIndex = 0;
    std::vector<std::uint32_t> sourceIndexes;
    std::string mode = "direct";
    double defaultValue = 0.0;
    bool useDefaultWhenBad = true;
};

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

void handleSignal(int) {
    g_running = false;
}

void setProcessName(const std::string& name) {
#ifndef _WIN32
    prctl(PR_SET_NAME, name.substr(0, 15).c_str(), 0, 0, 0);
#else
    (void)name;
#endif
}

std::string getOption(
    const std::vector<std::string>& args,
    const std::string& name,
    const std::string& defaultValue = ""
) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == name) {
            return args[i + 1];
        }
    }
    return defaultValue;
}

bool hasOption(const std::vector<std::string>& args, const std::string& name) {
    return std::find(args.begin(), args.end(), name) != args.end();
}

std::vector<std::uint32_t> sources(std::initializer_list<std::uint32_t> indexes) {
    return std::vector<std::uint32_t>(indexes);
}

Mapping mapDirect(int appIndex, std::uint32_t sourceIndex, double defaultValue = 0.0) {
    return Mapping{appIndex, 0, sources({sourceIndex}), "direct", defaultValue, true};
}

Mapping mapSum(int appIndex, std::initializer_list<std::uint32_t> sourceIndexes, double defaultValue = 0.0) {
    return Mapping{appIndex, 0, sources(sourceIndexes), "sum", defaultValue, true};
}

Mapping mapAverage(int appIndex, std::initializer_list<std::uint32_t> sourceIndexes, double defaultValue = 0.0) {
    return Mapping{appIndex, 0, sources(sourceIndexes), "avg", defaultValue, true};
}

Mapping mapAbs(int appIndex, std::uint32_t sourceIndex, double defaultValue = 0.0) {
    return Mapping{appIndex, 0, sources({sourceIndex}), "abs", defaultValue, true};
}

Mapping mapConstant(int appIndex, double value) {
    return Mapping{appIndex, 0, {}, "constant", value, true};
}

std::vector<Mapping> buildMappings() {
    std::vector<Mapping> mappings = {
        // 首页
        mapDirect(1, 1569, 0.0),
        mapDirect(2, 1590, 0.0),
        mapDirect(3, 1591, 0.0),
        mapDirect(4, 1247, 0.0),
        mapDirect(5, 1248, 0.0),
        mapConstant(6, 0.0),
        mapAbs(7, 1139, 0.0),
        mapAbs(8, 1039, 0.0),
        mapAbs(9, 1143, 0.0),
        mapAbs(10, 1043, 0.0),
        mapDirect(1039, 1039, 0.0),
        mapDirect(1043, 1043, 0.0),
        mapDirect(1136, 1136, 0.0),
        mapDirect(1137, 1137, 0.0),
        mapDirect(1138, 1138, 0.0),
        mapDirect(1139, 1139, 0.0),
        mapDirect(1143, 1143, 0.0),

        // 运行监控/台区/储能/PCS/BMS 汇总
        mapConstant(100, 1000.0),
        mapDirect(101, 1036, 0.0),
        mapDirect(102, 1037, 0.0),
        mapDirect(103, 1038, 0.0),
        mapDirect(104, 1039, 0.0),
        mapDirect(105, 1040, 0.0),
        mapDirect(106, 1041, 0.0),
        mapDirect(107, 1042, 0.0),
        mapDirect(108, 1043, 0.0),
        mapDirect(109, 1036, 0.0),
        mapDirect(110, 1037, 0.0),
        mapDirect(111, 1038, 0.0),
        mapDirect(112, 1039, 0.0),
        mapDirect(113, 1040, 0.0),
        mapDirect(114, 1041, 0.0),
        mapDirect(115, 1042, 0.0),
        mapDirect(116, 1043, 0.0),
        mapDirect(117, 1030, 0.0),
        mapDirect(118, 1031, 0.0),
        mapDirect(119, 1032, 0.0),
        mapDirect(120, 1136, 0.0),
        mapDirect(121, 1137, 0.0),
        mapDirect(122, 1138, 0.0),
        mapDirect(123, 1139, 0.0),
        mapDirect(124, 1140, 0.0),
        mapDirect(125, 1141, 0.0),
        mapDirect(126, 1142, 0.0),
        mapDirect(127, 1143, 0.0),
        mapDirect(128, 1130, 0.0),
        mapDirect(129, 1131, 0.0),
        mapDirect(130, 1132, 0.0),
        mapDirect(131, 1227, 0.0),
        mapDirect(132, 1228, 0.0),
        mapDirect(133, 1229, 0.0),
        mapDirect(134, 1230, 0.0),
        mapDirect(135, 1231, 0.0),
        mapDirect(136, 1232, 0.0),
        mapDirect(137, 1233, 0.0),
        mapDirect(138, 1234, 0.0),
        mapDirect(139, 1566, 0.0),
        mapDirect(140, 1567, 0.0),
        mapDirect(141, 1574, 0.0),
        mapDirect(142, 1577, 0.0),
        mapDirect(143, 1580, 0.0),
        mapDirect(144, 1583, 0.0),
        mapDirect(145, 1569, 0.0),
        mapDirect(146, 1399, 0.0),
        mapDirect(147, 1550, 0.0),

        // PCS 页面
        mapDirect(201, 1214, 0.0),
        mapDirect(202, 1211, 0.0),
        mapConstant(203, 0.0),
        mapDirect(204, 1212, 0.0),
        mapDirect(205, 1213, 0.0),
        mapDirect(206, 1217, 0.0),
        mapDirect(207, 1215, 0.0),
        mapDirect(208, 1218, 0.0),
        mapDirect(209, 1310, 0.0),
        mapDirect(210, 1324, 0.0),
        mapDirect(211, 1318, 0.0),
        mapDirect(212, 1319, 0.0),
        mapDirect(213, 1320, 0.0),
        mapDirect(214, 1321, 0.0),
        mapDirect(215, 1322, 0.0),
        mapDirect(216, 1323, 0.0),
        mapDirect(217, 1261, 0.0),
        mapDirect(218, 1262, 0.0),
        mapDirect(219, 1263, 0.0),
        mapDirect(220, 1264, 0.0),
        mapDirect(221, 1265, 0.0),
        mapDirect(222, 1230, 0.0),
        mapDirect(223, 1227, 0.0),
        mapDirect(224, 1228, 0.0),
        mapDirect(225, 1229, 0.0),
        mapDirect(226, 1234, 0.0),
        mapDirect(227, 1231, 0.0),
        mapDirect(228, 1232, 0.0),
        mapDirect(229, 1233, 0.0),
        mapDirect(230, 1238, 0.0),
        mapDirect(231, 1235, 0.0),
        mapDirect(232, 1236, 0.0),
        mapDirect(233, 1237, 0.0),
        mapDirect(234, 1242, 0.0),
        mapDirect(235, 1239, 0.0),
        mapDirect(236, 1240, 0.0),
        mapDirect(237, 1241, 0.0),
        mapAverage(238, {1220, 1221, 1222}, 0.0),
        mapDirect(239, 1220, 0.0),
        mapDirect(240, 1221, 0.0),
        mapDirect(241, 1222, 0.0),
        mapAverage(242, {1223, 1224, 1225}, 0.0),
        mapDirect(243, 1223, 0.0),
        mapDirect(244, 1224, 0.0),
        mapDirect(245, 1225, 0.0),
        mapDirect(246, 1244, 0.0),
        mapDirect(247, 1245, 0.0),
        mapDirect(248, 1243, 0.0),
        mapAverage(249, {1270, 1271, 1272, 1273}, 0.0),
        mapDirect(250, 1271, 0.0),
        mapDirect(251, 1272, 0.0),
        mapDirect(252, 1273, 0.0),
        mapConstant(253, 0.0),
        mapConstant(254, 0.0),
        mapConstant(255, 0.0),
        mapDirect(256, 1247, 0.0),
        mapDirect(257, 1248, 0.0),
        mapDirect(258, 1249, 0.0),
        mapDirect(259, 1250, 0.0),
        mapDirect(260, 1573, 0.0),
        mapDirect(261, 401552, 0.0),
        mapDirect(262, 401553, 0.0),
        mapDirect(263, 1318, 0.0),
        mapDirect(264, 1319, 0.0),
        mapDirect(265, 1320, 0.0),
        mapDirect(266, 1321, 0.0),
        mapDirect(267, 1322, 0.0),
        mapDirect(268, 1323, 0.0),

        // BMS 页面
        mapDirect(300, 1569, 0.0),
        mapDirect(301, 1570, 0.0),
        mapConstant(302, 0.0),
        mapConstant(303, 1.0),
        mapConstant(304, 0.0),
        mapConstant(305, 0.0),
        mapConstant(306, 0.0),
        mapConstant(307, 0.0),
        mapConstant(308, 0.0),
        mapConstant(309, 0.0),
        mapConstant(310, 0.0),
        mapConstant(311, 0.0),
        mapDirect(312, 1615, 0.0),
        mapDirect(313, 1616, 0.0),
        mapDirect(314, 1588, 0.0),
        mapDirect(315, 1586, 0.0),
        mapDirect(316, 1589, 0.0),
        mapDirect(317, 1587, 0.0),
        mapConstant(318, 0.0),
        mapConstant(319, 0.0),
        mapConstant(320, 0.0),
        mapConstant(321, 0.0),
        mapConstant(322, 0.0),
        mapConstant(323, 0.0),
        mapConstant(324, 0.0),
        mapConstant(325, 0.0),
        mapConstant(326, 0.0),
        mapConstant(327, 0.0),
        mapConstant(328, 0.0),
        mapConstant(329, 0.0),
        mapConstant(330, 0.0),
        mapDirect(331, 1566, 0.0),
        mapDirect(332, 1574, 0.0),
        mapDirect(333, 1577, 0.0),
        mapDirect(334, 1580, 0.0),
        mapDirect(335, 1583, 0.0),
        mapDirect(336, 1556, 0.0),
        mapDirect(337, 1572, 0.0),
        mapDirect(338, 1571, 0.0),
        mapDirect(339, 1571, 0.0),
        mapConstant(340, 0.0),
        mapDirect(341, 1567, 0.0),
        mapDirect(342, 1576, 0.0),
        mapDirect(343, 1579, 0.0),
        mapDirect(344, 1582, 0.0),
        mapDirect(345, 1585, 0.0),
        mapDirect(346, 1557, 0.0),
        mapDirect(347, 1573, 0.0),
        mapDirect(348, 1571, 0.0),
        mapConstant(349, 0.0),
        mapConstant(350, 0.0),
        mapConstant(351, 0.0),
        mapConstant(352, 0.0),
        mapConstant(353, 0.0),
        mapConstant(354, 0.0),
        mapConstant(355, 0.0),
        mapConstant(356, 0.0),
        mapConstant(357, 0.0),
        mapConstant(358, 0.0),
        mapConstant(359, 0.0),
        mapConstant(360, 0.0),
        mapConstant(361, 0.0),
        mapConstant(362, 0.0),
        mapConstant(363, 0.0),
        mapConstant(364, 0.0),
        mapConstant(365, 0.0),
        mapConstant(366, 0.0),
        mapConstant(367, 0.0),
        mapConstant(368, 0.0),

        // 消防/温控/除湿/CAN 探测器
        mapDirect(370, 131, 0.0),
        mapDirect(371, 132, 0.0),
        mapConstant(372, 0.0),
        mapConstant(373, 0.0),
        mapConstant(374, 0.0),
        mapConstant(375, 0.0),
        mapConstant(376, 0.0),
        mapConstant(377, 0.0),
        mapDirect(378, 189, 0.0),
        mapDirect(379, 190, 0.0),
        mapDirect(380, 310102, 0.0),
        mapDirect(381, 310105, 0.0),
        mapDirect(382, 310104, 0.0),
        mapDirect(383, 310103, 0.0),
        mapConstant(384, 0.0),
        mapConstant(385, 0.0)
    };

    std::set<int> seen;
    for (auto& mapping : mappings) {
        if (!seen.insert(mapping.appIndex).second) {
            throw std::runtime_error("duplicate legacy appDataIndex in bridge mapping: " + std::to_string(mapping.appIndex));
        }
    }

    int physical = kBridgePhysicalIndexBase;
    for (auto& mapping : mappings) {
        mapping.physicalIndex = physical++;
    }
    return mappings;
}

struct RouterContext {
    edge_gateway::AppConfig appConfig;
    std::vector<edge_gateway::DeviceConfig> deviceConfigs;
    std::vector<std::unique_ptr<edge_gateway::MemoryPointStore>> stores;
    edge_gateway::PointStoreRouter router;
};

std::vector<std::string> collectSharedMemoryNames(
    const edge_gateway::AppConfig& appConfig,
    const std::vector<edge_gateway::DeviceConfig>& deviceConfigs
) {
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    auto add = [&](const std::string& name) {
        if (!name.empty() && seen.insert(name).second) {
            result.push_back(name);
        }
    };

    for (const auto& name : appConfig.mqttDriver.sharedMemoryNames) {
        add(name);
    }
    add(appConfig.mqttDriver.sharedMemoryName);
    for (const auto& name : appConfig.localDisplay.sharedMemoryNames) {
        add(name);
    }
    for (const auto& config : deviceConfigs) {
        add(config.memoryStore.sharedMemoryName);
    }
    add(appConfig.cameraService.sharedMemoryName);
    if (result.empty()) {
        add("gateway_point_store");
    }
    return result;
}

std::unique_ptr<RouterContext> createRouterContext(const std::string& appConfigPath) {
    std::unique_ptr<RouterContext> context(new RouterContext());
    context->appConfig = edge_gateway::ConfigLoader::loadAppConfigFromFile(appConfigPath);

    edge_gateway::DeviceIdentity identity;
    if (!context->appConfig.identityConfigFile.empty()) {
        identity = edge_gateway::ConfigLoader::loadDeviceIdentityFromFile(context->appConfig.identityConfigFile);
    }

    context->deviceConfigs = context->appConfig.identityConfigFile.empty()
        ? edge_gateway::ConfigLoader::loadMany(context->appConfig.deviceConfigFiles)
        : edge_gateway::ConfigLoader::loadMany(context->appConfig.deviceConfigFiles, identity);

    const auto names = collectSharedMemoryNames(context->appConfig, context->deviceConfigs);
    context->stores.reserve(names.size());
    for (const auto& name : names) {
        context->stores.emplace_back(new edge_gateway::MemoryPointStore(name));
        context->router.addStore(name, *context->stores.back());
    }

    const auto fallbackSharedMemoryName = names.empty()
        ? std::string("gateway_point_store")
        : names.front();
    context->router.addRoutesFromDeviceConfigs(context->deviceConfigs, fallbackSharedMemoryName);
    const auto machineCode = !identity.machineCode.empty()
        ? identity.machineCode
        : context->appConfig.mqtt.clientId;
    context->router.addRoutesFromCameraServiceConfig(context->appConfig.cameraService, machineCode);
    return context;
}

void writeVarList(const std::string& path, const std::vector<Mapping>& mappings) {
    std::ofstream output(path.c_str(), std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("open var list failed: " + path);
    }
    output << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    output << "<VarList>\n";
    for (const auto& mapping : mappings) {
        output << "  <Data note=\"QtDisplayBridge\" unit=\"\" rwType=\"R\" appDataIndex=\""
               << mapping.appIndex
               << "\" index=\""
               << mapping.physicalIndex
               << "\" startbit=\"0\" datalen=\"64\" dType=\"DOUBLE\" saveMode=\"notSave\" name=\"\" periodSaveInterval=\"0\" initValue=\"\" formula=\"\"/>\n";
    }
    output << "</VarList>\n";
}

class LegacyQramWriter {
public:
    LegacyQramWriter() = default;
    ~LegacyQramWriter() {
        if (shm_ != nullptr && shm_ != reinterpret_cast<LegacySharedMemory*>(-1)) {
            shmdt(shm_);
        }
    }

    void initialize() {
        const auto size = sizeof(LegacySharedMemory);
        bool created = false;
        shmid_ = shmget(kLegacyShmKey, size, IPC_CREAT | IPC_EXCL | 0666);
        if (shmid_ >= 0) {
            created = true;
        } else if (errno == EEXIST) {
            shmid_ = shmget(kLegacyShmKey, size, 0666);
        }
        if (shmid_ < 0) {
            throw std::runtime_error("shmget failed: " + std::string(std::strerror(errno)));
        }

        void* attached = shmat(shmid_, nullptr, 0);
        if (attached == reinterpret_cast<void*>(-1)) {
            throw std::runtime_error("shmat failed: " + std::string(std::strerror(errno)));
        }
        shm_ = static_cast<LegacySharedMemory*>(attached);

        if (created || shm_->initialized != 1) {
            initSharedMemory();
        }
    }

    void write(int physicalIndex, double value) {
        if (shm_ == nullptr || physicalIndex < 0 || physicalIndex >= kLegacyShmWordSize) {
            return;
        }
        const int lockIndex = physicalIndex / kLegacyShmDataPerLock;
        if (lockIndex < 0 || lockIndex >= kLegacyShmLockCount) {
            return;
        }
        pthread_rwlock_wrlock(&shm_->rwlocks[lockIndex]);
        shm_->data[physicalIndex] = value;
        pthread_rwlock_unlock(&shm_->rwlocks[lockIndex]);
    }

    double read(int physicalIndex) const {
        if (shm_ == nullptr || physicalIndex < 0 || physicalIndex >= kLegacyShmWordSize) {
            return 0.0;
        }
        const int lockIndex = physicalIndex / kLegacyShmDataPerLock;
        if (lockIndex < 0 || lockIndex >= kLegacyShmLockCount) {
            return 0.0;
        }
        pthread_rwlock_rdlock(&shm_->rwlocks[lockIndex]);
        const double value = shm_->data[physicalIndex];
        pthread_rwlock_unlock(&shm_->rwlocks[lockIndex]);
        return value;
    }

private:
    void initSharedMemory() {
        pthread_rwlockattr_t rwlockAttr;
        if (pthread_rwlockattr_init(&rwlockAttr) != 0) {
            throw std::runtime_error("pthread_rwlockattr_init failed");
        }
        if (pthread_rwlockattr_setpshared(&rwlockAttr, PTHREAD_PROCESS_SHARED) != 0) {
            pthread_rwlockattr_destroy(&rwlockAttr);
            throw std::runtime_error("pthread_rwlockattr_setpshared failed");
        }
        for (int i = 0; i < kLegacyShmLockCount; ++i) {
            pthread_rwlock_init(&shm_->rwlocks[i], &rwlockAttr);
        }
        pthread_rwlockattr_destroy(&rwlockAttr);

        pthread_mutexattr_t mutexAttr;
        if (pthread_mutexattr_init(&mutexAttr) != 0) {
            throw std::runtime_error("pthread_mutexattr_init failed");
        }
        if (pthread_mutexattr_setpshared(&mutexAttr, PTHREAD_PROCESS_SHARED) != 0) {
            pthread_mutexattr_destroy(&mutexAttr);
            throw std::runtime_error("pthread_mutexattr_setpshared failed");
        }
        pthread_mutex_init(&shm_->di_mutex, &mutexAttr);
        pthread_mutexattr_destroy(&mutexAttr);

        std::memset(shm_->data, 0, sizeof(shm_->data));
        for (int i = 0; i < kLegacyShmUpdateFlagWords; ++i) {
            shm_->dataIndexFlag[i].store(0);
        }
        shm_->dataUpdateFlag.store(0);
        shm_->initial_polling_done = 1;
        shm_->initialized = 1;
    }

    int shmid_ = -1;
    LegacySharedMemory* shm_ = nullptr;
};

bool goodValue(const edge_gateway::StoredPointValue& value) {
    return value.quality == 1 && !value.stale;
}

double resolveMappingValue(
    const Mapping& mapping,
    edge_gateway::PointStoreRouter& router,
    std::int64_t ts
) {
    if (mapping.mode == "constant") {
        return mapping.defaultValue;
    }

    std::vector<double> values;
    values.reserve(mapping.sourceIndexes.size());
    for (const auto sourceIndex : mapping.sourceIndexes) {
        const auto latest = router.getLatestByIndex(sourceIndex, ts);
        if (!latest || !goodValue(*latest)) {
            if (mapping.useDefaultWhenBad) {
                return mapping.defaultValue;
            }
            continue;
        }
        values.push_back(latest->value);
    }

    if (values.empty()) {
        return mapping.defaultValue;
    }
    if (mapping.mode == "sum") {
        double total = 0.0;
        for (const auto value : values) {
            total += value;
        }
        return total;
    }
    if (mapping.mode == "avg") {
        double total = 0.0;
        for (const auto value : values) {
            total += value;
        }
        return total / static_cast<double>(values.size());
    }
    if (mapping.mode == "abs") {
        return std::fabs(values.front());
    }
    return values.front();
}

void printUsage(const char* argv0) {
    std::cout
        << "usage: " << argv0
        << " --app-config <app.json>"
        << " [--var-list /root/VarList.xml]"
        << " [--interval-ms 1000]"
        << " [--once]"
        << " [--dump]"
        << std::endl;
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (hasOption(args, "--help") || hasOption(args, "-h")) {
        printUsage(argv[0]);
        return 0;
    }

    const auto appConfigPath = getOption(args, "--app-config", "/opt/modbus-gateway/config/runtime/apps/mqtt-service.json");
    const auto varListPath = getOption(args, "--var-list", "/root/VarList.xml");
    const auto intervalMs = std::max(100, std::stoi(getOption(args, "--interval-ms", "1000")));
    const bool once = hasOption(args, "--once");
    const bool dump = hasOption(args, "--dump");

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    setProcessName("qt_disp_bridge");

    try {
        auto mappings = buildMappings();
        writeVarList(varListPath, mappings);

        auto context = createRouterContext(appConfigPath);
        LegacyQramWriter writer;
        writer.initialize();

        std::size_t iteration = 0;
        do {
            const auto ts = nowMs();
            for (const auto& mapping : mappings) {
                const auto value = resolveMappingValue(mapping, context->router, ts);
                writer.write(mapping.physicalIndex, value);
            }

            if (dump || iteration == 0) {
                std::cout << "qt display bridge updated " << mappings.size()
                          << " points varList=" << varListPath
                          << " appConfig=" << appConfigPath
                          << std::endl;
                const int sampleIndexes[] = {1, 1139, 145, 222, 300, 378, 380};
                std::unordered_map<int, int> physicalByApp;
                for (const auto& mapping : mappings) {
                    physicalByApp[mapping.appIndex] = mapping.physicalIndex;
                }
                for (const auto appIndex : sampleIndexes) {
                    const auto it = physicalByApp.find(appIndex);
                    if (it != physicalByApp.end()) {
                        std::cout << "  appDataIndex=" << appIndex
                                  << " physical=" << it->second
                                  << " value=" << writer.read(it->second)
                                  << std::endl;
                    }
                }
            }

            ++iteration;
            if (once) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
        } while (g_running.load());

        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "qt display bridge failed: " << ex.what() << std::endl;
        return 1;
    }
}
