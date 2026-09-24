#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include "edge_gateway/memory_point_store.hpp"

namespace edge_gateway {
using BeforePhysicalWrite = std::function<void()>;
using ClusterBootId = std::array<std::uint8_t, 16>;

std::int64_t clusterMonotonicNowMs();
ClusterBootId localKernelBootId();
ClusterBootId newClusterAuthorityEpoch();
bool clusterStationStrategyAllowed(const ClusterAuthoritySnapshot&, const ClusterBootId&, std::int64_t);
bool clusterAuthorizationRequired(const EmsClusterConfig&, const PendingWriteCommand&);
bool clusterAuthorizationValid(const EmsClusterConfig&, const PendingWriteCommand&,
                               const ClusterAuthoritySnapshot&, const ClusterBootId&, std::int64_t);

class ClusterWriteGuard {
public:
    explicit ClusterWriteGuard(EmsClusterConfig config);
    Optional<ClusterAuthoritySnapshot> snapshot() const;
    void check(const PendingWriteCommand& command) const;
private:
    EmsClusterConfig config_;
    mutable std::mutex mutex_;
    mutable std::unique_ptr<MemoryPointStore> authorityStore_;
};
}
