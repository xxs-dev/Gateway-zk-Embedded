#pragma once

#include <memory>

#include "edge_gateway/common/command_executor_interface.hpp"
#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {

class DioCommandExecutor : public ICommandExecutor {
public:
    DioCommandExecutor(
        DeviceConfig config,
        MemoryPointStore& store,
        std::shared_ptr<IGpioPort> gpioPort
    );

    CommandResult executePending(const PendingWriteCommand& command, std::int64_t nowMs,
        const BeforePhysicalWrite& beforeWrite = BeforePhysicalWrite()) const override {
        if (clusterAuthorizationRequired(config_.emsCluster, command))
            throw std::runtime_error("cluster physical write is unsupported by this executor");
        return ICommandExecutor::executePending(command, nowMs, beforeWrite);
    }

    CommandResult executeByIndex(
        const std::string& cmdId,
        std::uint32_t index,
        double value,
        std::int64_t nowMs
    ) const override;

private:
    const PointDefinition& findPointByIndex(std::uint32_t index) const;
    void dispatchLocalDioWrite(const PointDefinition& point, double value, std::int64_t nowMs) const;

    DeviceConfig config_;
    MemoryPointStore& store_;
    std::shared_ptr<IGpioPort> gpioPort_;
};

}  // namespace edge_gateway
