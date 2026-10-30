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
    // point (or an IEC 103 DPI octet with spare bits set) keeps the previously stored value
    // with bad quality. IEC 103 status values (ASDU 1/2 DPI, ASDU 44 status bit) also carry
    // their diagnostic in text (IecCodec::iec103StatusDiagnostic); rawHex keeps the raw ASDU.
    PointValue buildIecPointValue(const PointDefinition& point, const IecDataValue& dataValue, std::int64_t receivedMs) const;

    std::shared_ptr<IecClient> client_;
    // GATEWAY_IEC103_DEBUG: log the status diagnostic of every ASDU 1/2 value and of every
    // ASDU 44 value whose stored state or quality changes. Off by default.
    bool debugStatusValues_ = false;
};

}  // namespace edge_gateway
