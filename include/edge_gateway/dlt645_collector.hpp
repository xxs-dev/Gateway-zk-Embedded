#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "edge_gateway/common/collector_base.hpp"

namespace edge_gateway {

class Dlt645Client;

class Dlt645Collector : public CollectorBase {
public:
    Dlt645Collector(
        DeviceConfig config,
        MemoryPointStore& store,
        std::shared_ptr<Dlt645Client> dlt645Client,
        std::shared_ptr<IMqttPublisher> mqttPublisher = nullptr
    );

    CollectCycleResult collectOnce(std::int64_t nowMs, bool realtimeFocused = false) override;

private:
    std::shared_ptr<Dlt645Client> dlt645Client_;
    std::vector<std::string> dataIdOrder_;
    std::unordered_map<std::string, int> failedRetryRoundsByDataId_;
    std::size_t dataIdCursor_ = 0;
};

}  // namespace edge_gateway
