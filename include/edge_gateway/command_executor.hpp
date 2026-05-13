#pragma once

#include <cstdint>
#include <memory>

#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {

class CommandExecutor {
public:
    CommandExecutor(
        DeviceConfig config,
        MemoryPointStore& store,
        std::shared_ptr<IModbusClient> modbusClient,
        std::shared_ptr<IMqttPublisher> mqttPublisher = nullptr,
        std::shared_ptr<IGpioPort> gpioPort = nullptr
    );

    CommandResult execute(const CommandRequest& request, std::int64_t nowMs) const;
    CommandResult executeByIndex(
        const std::string& cmdId,
        std::uint32_t index,
        double value,
        std::int64_t nowMs
    ) const;

private:
    const PointDefinition& findPoint(const std::string& pointCode) const;
    const PointDefinition& findPointByIndex(std::uint32_t index) const;
    void dispatchWrite(const PointDefinition& point, const std::vector<std::uint16_t>& encoded) const;
    void dispatchLocalDioWrite(const PointDefinition& point, double value, std::int64_t nowMs) const;
    std::uint16_t mergeBitWrite(const PointDefinition& point, bool value) const;
    void verifyWrite(
        const CommandRequest& request,
        const PointDefinition& point,
        std::int64_t nowMs
    ) const;

    DeviceConfig config_;
    MemoryPointStore& store_;
    std::shared_ptr<IModbusClient> modbusClient_;
    std::shared_ptr<IMqttPublisher> mqttPublisher_;
    std::shared_ptr<IGpioPort> gpioPort_;
};

}  // namespace edge_gateway
