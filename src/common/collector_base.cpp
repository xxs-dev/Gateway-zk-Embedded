#include "edge_gateway/common/collector_base.hpp"

#include <algorithm>
#include <utility>

namespace edge_gateway {

CollectorBase::CollectorBase(
    DeviceConfig config,
    MemoryPointStore& store,
    std::shared_ptr<IMqttPublisher> mqttPublisher
) : config_(std::move(config)),
    store_(store),
    mqttPublisher_(std::move(mqttPublisher)) {
    std::vector<PointDefinition> enabledPoints;
    enabledPoints.reserve(config_.points.size());
    for (const auto& point : config_.points) {
        if (point.enabled) {
            enabledPoints.push_back(point);
        }
    }
    store_.registerPoints(config_.machineCode, config_.meterCode, enabledPoints);
}

PointValue CollectorBase::buildFailedPointValue(
    const PointDefinition& point,
    const std::string& message,
    std::int64_t nowMs
) const {
    PointValue value;
    value.index = point.index;
    value.machineCode = config_.machineCode;
    value.meterCode = config_.meterCode;
    value.pointCode = point.pointCode;
    value.pointName = point.name;
    value.category = point.category;
    value.unit = point.read.unit;
    value.value = 0.0;
    value.text = message;
    value.rawHex.clear();
    value.quality = 0;
    value.qualityMsg = message;
    value.ts = nowMs;
    value.expireAt = nowMs + point.read.cachePolicy.ttlMs;
    value.stale = false;
    value.function = point.read.function;
    value.address = point.address;
    value.length = point.read.length;
    value.isStore = false;
    value.persistIntervalSec = point.persistIntervalSec;
    return value;
}

PointValue CollectorBase::buildPointValue(
    const PointDefinition& point,
    const DecodedValue& decoded,
    std::int64_t nowMs
) const {
    PointValue value;
    value.index = point.index;
    value.machineCode = config_.machineCode;
    value.meterCode = config_.meterCode;
    value.pointCode = point.pointCode;
    value.pointName = point.name;
    value.category = point.category;
    value.unit = point.read.unit;
    value.value = decoded.value;
    value.text = decoded.text;
    value.rawHex = decoded.rawHex;
    value.ts = nowMs;
    value.expireAt = nowMs + point.read.cachePolicy.ttlMs;
    value.function = point.read.function;
    value.address = point.address;
    value.length = point.read.length;
    value.isStore = point.isStore;
    value.persistIntervalSec = point.persistIntervalSec;
    return value;
}

PointValue CollectorBase::buildDeviceOnlineValue(
    const PointDefinition& point,
    bool online,
    std::int64_t nowMs
) const {
    PointValue value;
    value.index = point.index;
    value.machineCode = config_.machineCode;
    value.meterCode = config_.meterCode;
    value.pointCode = point.pointCode;
    value.pointName = point.name;
    value.category = point.category;
    value.unit = point.read.unit;
    value.value = online ? 1.0 : 0.0;
    value.text = online ? "online" : "offline";
    value.rawHex.clear();
    value.quality = 1;
    value.qualityMsg = "ok";
    value.ts = nowMs;
    value.expireAt = nowMs + point.read.cachePolicy.ttlMs;
    value.function = 0;
    value.address = point.address;
    value.length = 0;
    value.isStore = point.isStore;
    value.persistIntervalSec = point.persistIntervalSec;
    return value;
}

std::vector<PointDefinition> CollectorBase::duePoints(std::int64_t nowMs, bool forceDue) {
    std::vector<PointDefinition> points;
    points.reserve(config_.points.size());
    for (const auto& point : config_.points) {
        if (!point.enabled || !point.read.enable || point.read.dataType == "device_online") {
            continue;
        }
        if (!forceDue) {
            const auto intervalMs = effectiveIntervalMs(point);
            const auto last = lastReadMs_.find(point.index);
            if (last != lastReadMs_.end() && nowMs - last->second < intervalMs) {
                continue;
            }
        }
        points.push_back(point);
    }
    return points;
}

int CollectorBase::effectiveIntervalMs(const PointDefinition& point) const {
    if (point.read.intervalMs > 0) {
        return std::max(100, point.read.intervalMs);
    }
    return std::max(100, config_.collect.defaultIntervalMs);
}

void CollectorBase::publishDeviceOnlineStatus(bool online, std::int64_t nowMs) const {
    for (const auto& point : config_.points) {
        if (!point.enabled || !point.read.enable || point.read.dataType != "device_online") {
            continue;
        }
        auto value = buildDeviceOnlineValue(point, online, nowMs);
        if (point.read.cachePolicy.storeLatest) {
            store_.putLatest(value);
        }
    }
}

}  // namespace edge_gateway
