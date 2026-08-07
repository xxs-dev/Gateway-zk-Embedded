#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/ems_cluster.hpp"
#include "edge_gateway/ems_cluster_network.hpp"
#include "edge_gateway/ems_cluster_points.hpp"
#include "edge_gateway/ems_cluster_transport.hpp"

namespace {

volatile std::sig_atomic_t g_running = 1;

void handleSignal(int) {
    g_running = 0;
}

std::int64_t monotonicNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

std::int64_t wallNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::string readText(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    std::ostringstream out;
    out << input.rdbuf();
    auto value = out.str();
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
    return value;
}

double jsonNumber(const std::string& text, const std::string& name, double fallback) {
    std::smatch match;
    const std::regex pattern("\\\"" + name + "\\\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)");
    return std::regex_search(text, match, pattern) ? std::stod(match[1].str()) : fallback;
}

bool jsonBool(const std::string& text, const std::string& name, bool fallback) {
    std::smatch match;
    const std::regex pattern("\\\"" + name + "\\\"\\s*:\\s*(true|false)");
    return std::regex_search(text, match, pattern) ? match[1].str() == "true" : fallback;
}

std::string directoryOf(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

void ensureDirectory(const std::string& path) {
#ifndef _WIN32
    if (path.empty()) return;
    std::string current;
    for (const auto ch : path) {
        current.push_back(ch);
        if (ch == '/' && current.size() > 1) mkdir(current.c_str(), 0775);
    }
    mkdir(path.c_str(), 0775);
#else
    (void)path;
#endif
}

void writeStatus(const std::string& path, const std::string& payload) {
    if (path.empty()) return;
    ensureDirectory(directoryOf(path));
#ifndef _WIN32
    const auto temporary = path + ".tmp." + std::to_string(static_cast<long long>(getpid()));
#else
    const auto temporary = path + ".tmp";
#endif
    std::ofstream output(temporary.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
    if (!output) throw std::runtime_error("failed to write EMS cluster status");
    output << payload << '\n';
    output.close();
    if (!output || std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        throw std::runtime_error("failed to install EMS cluster status");
    }
}

class LoadSampler {
public:
    explicit LoadSampler(std::string computeHealthFile) : computeHealthFile_(std::move(computeHealthFile)) {}

    edge_gateway::EmsClusterLoadSample sample() {
        edge_gateway::EmsClusterLoadSample result;
#ifndef _WIN32
        std::ifstream stat("/proc/stat");
        std::string label;
        std::uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
        if (stat >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) {
            const auto total = user + nice + system + idle + iowait + irq + softirq + steal;
            const auto idleTotal = idle + iowait;
            if (lastTotal_ > 0 && total > lastTotal_) {
                const auto totalDelta = total - lastTotal_;
                const auto idleDelta = idleTotal - lastIdle_;
                result.cpuPercent = 100.0 * static_cast<double>(totalDelta - std::min(totalDelta, idleDelta)) /
                    static_cast<double>(totalDelta);
            }
            lastTotal_ = total;
            lastIdle_ = idleTotal;
        }
        std::ifstream meminfo("/proc/meminfo");
        std::string name;
        std::uint64_t value = 0;
        std::string unit;
        std::uint64_t totalKb = 0;
        std::uint64_t availableKb = 0;
        while (meminfo >> name >> value >> unit) {
            if (name == "MemTotal:") totalKb = value;
            else if (name == "MemAvailable:") availableKb = value;
        }
        if (totalKb > 0) result.memoryPercent = 100.0 * static_cast<double>(totalKb - availableKb) / totalKb;
#endif
        const auto health = readText(computeHealthFile_);
        const auto healthTimestampMs = static_cast<std::int64_t>(jsonNumber(health, "ts", 0.0));
        const auto healthAgeMs = wallNowMs() - healthTimestampMs;
        const bool healthFresh = !health.empty() && healthTimestampMs > 0 &&
            healthAgeMs >= -60000 && healthAgeMs <= 5000;
        result.computeMetricsAvailable = healthFresh;
        result.computeHealthy = healthFresh && jsonBool(health, "healthy", false);
        result.computeTimeoutPercent = jsonNumber(health, "timeoutPercent", 100.0);
        result.controlQueueP95Ms = jsonNumber(health, "controlQueueP95Ms", 1000.0);
        result.packetLossPercent = 0.0;
        return result;
    }

private:
    std::string computeHealthFile_;
    std::uint64_t lastTotal_ = 0;
    std::uint64_t lastIdle_ = 0;
};

}  // namespace

int main(int argc, char* argv[]) {
    using namespace edge_gateway;

    std::string appConfigPath = "config/runtime/apps/mqtt-service.json";
    bool once = false;
    bool validateOnly = false;
    bool prepareNetwork = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--app-config" && i + 1 < argc) appConfigPath = argv[++i];
        else if (argument == "--once") once = true;
        else if (argument == "--validate") validateOnly = true;
        else if (argument == "--prepare-network") prepareNetwork = true;
    }

    try {
        auto appConfig = ConfigLoader::loadAppConfigFromFile(appConfigPath);
        if (appConfig.runtimeMode != "ems") {
            throw std::invalid_argument("EmsClusterCoordinator requires runtimeMode=ems");
        }
        EmsClusterNode::validateConfig(appConfig.emsCluster);
        if (!appConfig.emsCluster.enabled) {
            std::cout << "EMS cluster disabled appConfig=" << appConfigPath << std::endl;
            return 0;
        }
        const auto identity = ConfigLoader::loadDeviceIdentityFromFile(appConfig.identityConfigFile);

        EmsClusterNetworkCheck network;
        if (prepareNetwork) {
            network = prepareEmsClusterNetwork(appConfig.emsCluster, identity.machineCode);
        } else {
            network = checkEmsClusterNetwork(appConfig.emsCluster, identity.machineCode);
            if (appConfig.emsCluster.autoConfigureAddress) {
                network.warnings.push_back(
                    "emsCluster.autoConfigureAddress 已忽略；普通服务启动不会修改网卡，请显式执行 --prepare-network"
                );
            }
        }
        for (const auto& warning : network.warnings) std::cerr << "EMS cluster network warning: " << warning << std::endl;
        if (!network.ready) {
            for (const auto& error : network.errors) std::cerr << "EMS cluster network error: " << error << std::endl;
            return 2;
        }
        if (prepareNetwork) {
            std::cout << "EMS cluster network ready interface=" << appConfig.emsCluster.clusterInterface
                      << " address=" << network.selectedAddress << std::endl;
            return 0;
        }

        const auto bootId = readText("/proc/sys/kernel/random/boot_id");
        EmsClusterNode node(appConfig.emsCluster, identity.machineCode, bootId.empty() ? "unknown-boot" : bootId);
        if (validateOnly) {
            std::cout << "EMS cluster configuration valid node=" << identity.machineCode
                      << " address=" << network.selectedAddress << std::endl;
            return 0;
        }

        auto transport = makeEthernetClusterTransport(appConfig.emsCluster, identity.machineCode);
        transport->start();
        EmsClusterPointBridge pointBridge(appConfig.emsCluster, identity.machineCode);
        LoadSampler loadSampler(appConfig.emsCluster.computeHealthFile);
        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);
        std::int64_t lastStatusAt = 0;
        bool pointStatePublished = false;
        bool lastDispatchValid = false;
        bool lastQuorumValid = false;
        EmsClusterRole lastRole = EmsClusterRole::Disabled;
        EmsClusterDispatchCode lastDispatchCode = EmsClusterDispatchCode::ControlDisabled;
        std::uint64_t lastDispatchSequence = 0;
        do {
            const auto now = monotonicNowMs();
            const auto wallNow = wallNowMs();
            for (const auto& inbound : transport->poll(50)) node.receive(inbound, now);
            bool stationTargetValid = false;
            const auto capability = pointBridge.sampleCapability(wallNow);
            const auto stationTarget = pointBridge.sampleStationTarget(wallNow, stationTargetValid);
            node.updateControlInputs(capability, stationTarget, stationTargetValid, now);
            node.tick(now, loadSampler.sample());
            for (const auto& outbound : node.drainOutgoing()) transport->send(outbound);
            const auto status = node.status(now);
            const auto dispatch = node.activeDispatch(now);
            const bool pointStateChanged = !pointStatePublished ||
                dispatch.valid != lastDispatchValid ||
                dispatch.code != lastDispatchCode ||
                dispatch.sequence != lastDispatchSequence ||
                status.quorumValid != lastQuorumValid ||
                status.role != lastRole;
            const bool statusDue = lastStatusAt == 0 ||
                now - lastStatusAt >= appConfig.emsCluster.statusIntervalMs;
            if (pointStateChanged || statusDue) {
                pointBridge.publish(status, dispatch, wallNow);
                pointStatePublished = true;
                lastDispatchValid = dispatch.valid;
                lastDispatchCode = dispatch.code;
                lastDispatchSequence = dispatch.sequence;
                lastQuorumValid = status.quorumValid;
                lastRole = status.role;
            }
            if (statusDue) {
                const auto payload = emsClusterStatusJson(status, wallNow);
                writeStatus(appConfig.emsCluster.statusFile, payload);
                std::cout << "EMS cluster state role=" << EmsClusterNode::roleName(status.role)
                          << " term=" << status.term
                          << " leader=" << (status.leaderNodeId.empty() ? "none" : status.leaderNodeId)
                          << " online=" << status.onlineMembers
                          << " quorum=" << (status.quorumValid ? "valid" : "invalid")
                          << " control=" << (status.controlActive ? "active" : "inactive")
                          << " dispatch=" << EmsClusterNode::dispatchCodeName(dispatch.code)
                          << std::endl;
                lastStatusAt = now;
            }
        } while (g_running && !once);
        transport->stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "EmsClusterCoordinator failed: " << error.what() << std::endl;
        return 1;
    }
}
