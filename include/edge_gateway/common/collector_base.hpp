#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {

struct CollectCycleResult {
    std::vector<ReadTask> executedTasks;
    std::vector<PointValue> values;
};

class ICollector {
public:
    virtual ~ICollector() = default;

    virtual CollectCycleResult collectOnce(std::int64_t nowMs, bool realtimeFocused = false) = 0;
    virtual void publishDeviceOnlineStatus(bool online, std::int64_t nowMs) const = 0;
};

class CollectorBase : public ICollector {
public:
    CollectorBase(
        DeviceConfig config,
        MemoryPointStore& store,
        std::shared_ptr<IMqttPublisher> mqttPublisher = nullptr
    );

    void publishDeviceOnlineStatus(bool online, std::int64_t nowMs) const override;

protected:
    PointValue buildFailedPointValue(
        const PointDefinition& point,
        const std::string& message,
        std::int64_t nowMs
    ) const;
    PointValue buildPointValue(
        const PointDefinition& point,
        const DecodedValue& decoded,
        std::int64_t nowMs
    ) const;
    PointValue buildDeviceOnlineValue(
        const PointDefinition& point,
        bool online,
        std::int64_t nowMs
    ) const;
    std::vector<PointDefinition> duePoints(std::int64_t nowMs, bool forceDue = false);
    int effectiveIntervalMs(const PointDefinition& point) const;

    DeviceConfig config_;
    MemoryPointStore& store_;
    std::shared_ptr<IMqttPublisher> mqttPublisher_;
    std::unordered_map<std::uint32_t, std::int64_t> lastReadMs_;
    std::unordered_map<std::uint32_t, std::int64_t> lastValueUpdateMs_;
};

}  // namespace edge_gateway
