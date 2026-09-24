#include "edge_gateway/cluster_write_authorization.hpp"
#include "edge_gateway/ems_cluster.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace edge_gateway {
namespace {
bool contains(const std::vector<std::uint32_t>& indexes, std::uint32_t index) {
    return std::find(indexes.begin(), indexes.end(), index) != indexes.end();
}
bool nonzero(const ClusterBootId& id) {
    return std::any_of(id.begin(), id.end(), [](std::uint8_t b) { return b != 0; });
}
bool validStoreName(const std::string& name) {
    return !name.empty() && name.size() < 64 &&
        name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos;
}
}

std::int64_t clusterMonotonicNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

ClusterBootId localKernelBootId() {
    static const auto boot = [] {
        std::ifstream in("/proc/sys/kernel/random/boot_id");
        std::string uuid;
        in >> uuid;
        if (uuid.size() != 36 || uuid[8] != '-' || uuid[13] != '-' || uuid[18] != '-' || uuid[23] != '-')
            throw std::runtime_error("cluster authority requires a valid local kernel boot ID");
        uuid.erase(std::remove(uuid.begin(), uuid.end(), '-'), uuid.end());
        if (uuid.size() != 32 || uuid.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            throw std::runtime_error("invalid kernel boot ID");
        ClusterBootId result{};
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = static_cast<std::uint8_t>(std::stoul(uuid.substr(i * 2, 2), nullptr, 16));
        if (!nonzero(result)) throw std::runtime_error("zero kernel boot ID");
        return result;
    }();
    return boot;
}

ClusterBootId newClusterAuthorityEpoch() {
    ClusterBootId result{};
    std::ifstream random("/dev/urandom", std::ios::binary);
    if (!random.read(reinterpret_cast<char*>(result.data()), result.size()) || !nonzero(result))
        throw std::runtime_error("cannot generate cluster authority epoch");
    return result;
}

bool clusterAuthorizationRequired(const EmsClusterConfig& config, const PendingWriteCommand& command) {
    return static_cast<bool>(command.clusterAuthorization) ||
        (config.enabled && config.controlEnabled && command.source == "graph-ems" &&
         (config.controlTargetIndexes.empty() || contains(config.controlTargetIndexes, command.index)));
}

bool clusterStationStrategyAllowed(const ClusterAuthoritySnapshot& snapshot, const ClusterBootId& boot,
                                   std::int64_t nowMs) {
    return snapshot.stationStrategyActive && snapshot.role == static_cast<int>(EmsClusterRole::Leader) &&
        nonzero(boot) && snapshot.strategyKernelBootId == boot && nowMs > 0 &&
        nowMs < snapshot.strategyNotAfterMonotonicMs;
}

bool clusterAuthorizationValid(const EmsClusterConfig& config, const PendingWriteCommand& command,
                               const ClusterAuthoritySnapshot& snapshot, const ClusterBootId& boot,
                               std::int64_t nowMs) {
    if (!config.enabled || !config.controlEnabled || !command.clusterAuthorization ||
        !snapshot.valid || !snapshot.authorization || !nonzero(boot) || nowMs <= 0 ||
        !std::isfinite(command.value) || !contains(config.controlTargetIndexes, command.index) ||
        !contains(snapshot.targetIndexes, command.index) || !validStoreName(config.virtualSharedMemoryName)) return false;
    const auto& cmd = *command.clusterAuthorization;
    const auto& current = *snapshot.authorization;
    return cmd.version == 1 && current.version == 1 && cmd.flags == 0 && current.flags == 0 &&
        cmd.authorityStoreName == config.virtualSharedMemoryName && current.authorityStoreName == config.virtualSharedMemoryName &&
        cmd.kernelBootId == boot && current.kernelBootId == boot && nonzero(cmd.authorityEpoch) &&
        cmd.authorityEpoch == current.authorityEpoch && cmd.dispatchSequence != 0 &&
        cmd.dispatchSequence == current.dispatchSequence && nowMs < cmd.notAfterMonotonicMs &&
        nowMs < current.notAfterMonotonicMs;
}

ClusterWriteGuard::ClusterWriteGuard(EmsClusterConfig config) : config_(std::move(config)) {}

void ClusterWriteGuard::check(const PendingWriteCommand& command) const {
    if (!clusterAuthorizationRequired(config_, command)) return;
    if (!command.clusterAuthorization || !config_.enabled || !config_.controlEnabled ||
        !validStoreName(config_.virtualSharedMemoryName) ||
        command.clusterAuthorization->authorityStoreName != config_.virtualSharedMemoryName ||
        !contains(config_.controlTargetIndexes, command.index))
        throw std::runtime_error("cluster authority missing or outside configured target/store scope");
    std::lock_guard<std::mutex> lock(mutex_);
    if (!authorityStore_)
        authorityStore_.reset(new MemoryPointStore(config_.virtualSharedMemoryName, MemoryStoreOpenMode::OpenExisting));
    const auto snapshot = authorityStore_->clusterAuthority();
    if (!snapshot || !clusterAuthorizationValid(config_, command, *snapshot, localKernelBootId(), clusterMonotonicNowMs()))
        throw std::runtime_error("cluster authority expired, revoked or changed");
}
}
