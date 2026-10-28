#pragma once

#include <memory>

#include "edge_gateway/common/collector_base.hpp"
#include "edge_gateway/iec_client.hpp"

namespace edge_gateway {

class IecCollector : public CollectorBase {
public:
    IecCollector(
        DeviceConfig config,
        MemoryPointStore& store,
        std::shared_ptr<IecClient> client,
        std::shared_ptr<IMqttPublisher> mqttPublisher = nullptr
    );

    CollectCycleResult collectOnce(std::int64_t nowMs, bool realtimeFocused = false) override;

private:
    // Device quality and time tag applied; a double point in DPI 0/3 mapped to a boolean
    // point keeps the previously stored value with bad quality.
    PointValue buildIecPointValue(const PointDefinition& point, const IecDataValue& dataValue, std::int64_t receivedMs) const;

    std::shared_ptr<IecClient> client_;
};

}  // namespace edge_gateway
