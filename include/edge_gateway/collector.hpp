#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"
#include "edge_gateway/dlt645_client.hpp"

namespace edge_gateway {

struct CollectCycleResult {
    std::vector<ReadTask> executedTasks;
    std::vector<PointValue> values;
};

class Collector {
public:
    Collector(
        DeviceConfig config,
        MemoryPointStore& store,
        std::shared_ptr<IModbusClient> modbusClient,
        std::shared_ptr<Dlt645Client> dlt645Client = nullptr,
        std::shared_ptr<IMqttPublisher> mqttPublisher = nullptr
    );

    CollectCycleResult collectOnce(std::int64_t nowMs);
    void publishDeviceOnlineStatus(bool online, std::int64_t nowMs) const;

private:
    std::vector<std::uint16_t> executeReadTask(const ReadTask& task) const;
    PointValue collectDlt645Point(const PointDefinition& point, std::int64_t nowMs) const;
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
    std::vector<PointDefinition> duePoints(std::int64_t nowMs);
    int effectiveIntervalMs(const PointDefinition& point) const;

    DeviceConfig config_;
    MemoryPointStore& store_;
    std::shared_ptr<IModbusClient> modbusClient_;
    std::shared_ptr<Dlt645Client> dlt645Client_;
    std::shared_ptr<IMqttPublisher> mqttPublisher_;
    std::unordered_map<std::uint32_t, std::int64_t> lastReadMs_;
};

}  // namespace edge_gateway
