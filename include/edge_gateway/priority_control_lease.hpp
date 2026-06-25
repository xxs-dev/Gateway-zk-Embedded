#pragma once

#include <cstdint>
#include <string>

#include "edge_gateway/compat.hpp"

namespace edge_gateway {

struct PriorityControlLeaseState {
    std::string owner;
    std::string cmdId;
    std::string meterCode;
    std::uint32_t index = 0;
    std::int64_t createdAtMs = 0;
    std::int64_t expireAtMs = 0;
};

class PriorityControlLease {
public:
    PriorityControlLease(std::string path, std::string owner);

    bool enabled() const;
    Optional<PriorityControlLeaseState> activeLease(std::int64_t nowMs) const;
    bool isBlocked(std::int64_t nowMs, const std::string& owner = std::string()) const;
    void acquire(
        const std::string& cmdId,
        const std::string& meterCode,
        std::uint32_t index,
        std::int64_t nowMs,
        int ttlMs
    ) const;
    void release(const std::string& cmdId = std::string()) const;

private:
    std::string path_;
    std::string owner_;
};

}  // namespace edge_gateway
