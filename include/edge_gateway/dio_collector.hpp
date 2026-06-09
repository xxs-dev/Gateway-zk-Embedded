#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "edge_gateway/common/collector_base.hpp"

namespace edge_gateway {

class DioCollector : public CollectorBase {
public:
    DioCollector(
        DeviceConfig config,
        MemoryPointStore& store,
        std::shared_ptr<IGpioPort> gpioPort,
        std::shared_ptr<IMqttPublisher> mqttPublisher = nullptr
    );

    CollectCycleResult collectOnce(std::int64_t nowMs, bool realtimeFocused = false) override;

private:
    PointValue collectLocalDioPoint(const PointDefinition& point, std::int64_t nowMs);

    std::shared_ptr<IGpioPort> gpioPort_;
    std::unordered_map<std::uint32_t, double> lastDioRawValues_;
    std::unordered_map<std::uint32_t, double> lastDioStableValues_;
    std::unordered_map<std::uint32_t, std::int64_t> lastDioRawChangeMs_;
};

}  // namespace edge_gateway
