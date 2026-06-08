#pragma once

#include <cstdint>
#include <string>

namespace edge_gateway {

class PriorityControlLease {
public:
    PriorityControlLease(std::string path, std::string owner);

    bool enabled() const;
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
