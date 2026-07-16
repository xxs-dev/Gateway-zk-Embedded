#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "edge_gateway/compat.hpp"

namespace edge_gateway {

struct PowerControlOwnershipState {
    std::string scope;
    std::string owner;
    std::string sessionId;
    std::vector<std::uint32_t> targetIndexes;
    std::int64_t heartbeatAtMs = 0;
    std::int64_t expireAtMs = 0;
};

class PowerControlOwnership {
public:
    PowerControlOwnership(std::string path, std::string owner);

    bool enabled() const;
    Optional<PowerControlOwnershipState> active(std::int64_t nowMs) const;
    bool isBlocked(std::uint32_t index, const std::string& source, std::int64_t nowMs) const;
    bool acquire(
        const std::string& scope,
        const std::string& sessionId,
        const std::vector<std::uint32_t>& targetIndexes,
        std::int64_t nowMs,
        int ttlMs
    ) const;
    bool renew(const std::string& sessionId, std::int64_t nowMs, int ttlMs) const;
    void release(const std::string& sessionId = std::string()) const;

private:
    std::string path_;
    std::string owner_;
};

}  // namespace edge_gateway
