#pragma once

// Helpers shared by can_driver_service.cpp and can_driver_writeback.cpp.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>
#include <thread>

#include "edge_gateway/models.hpp"

namespace edge_gateway {
namespace can_driver_detail {

inline std::string escapeJson(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const auto ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

inline void sleepInterruptibly(const std::atomic<bool>& running, int intervalMs) {
    int remaining = std::max(0, intervalMs);
    while (running.load() && remaining > 0) {
        const auto slice = std::min(100, remaining);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        remaining -= slice;
    }
}

inline bool usesUdpTestTransport(const CanProtocolConfig& config) {
    return config.transportMode == "udp_test";
}

inline bool isOnlinePoint(const PointDefinition& point) {
    return point.pointCode == "device_online" || point.read.dataType == "device_online";
}

inline bool isInterfacePoint(const PointDefinition& point) {
    return point.read.dataType == "interface_online";
}

inline void validateWriteValue(const PointDefinition& point, double value) {
    if (!point.write.enable) {
        throw std::invalid_argument("point write is disabled");
    }
    if (point.write.minValue && value < *point.write.minValue) {
        throw std::invalid_argument("value below min");
    }
    if (point.write.maxValue && value > *point.write.maxValue) {
        throw std::invalid_argument("value above max");
    }
    if (!point.write.allowedValues.empty() &&
        std::find_if(point.write.allowedValues.begin(), point.write.allowedValues.end(), [value](double candidate) {
            return std::abs(candidate - value) <= 1e-9;
        }) == point.write.allowedValues.end()) {
        throw std::invalid_argument("value is not in allowedValues");
    }
}

}  // namespace can_driver_detail
}  // namespace edge_gateway
