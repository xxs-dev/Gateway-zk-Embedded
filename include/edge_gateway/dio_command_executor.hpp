#pragma once

#include <memory>
#include <string>
#include <vector>

#include "edge_gateway/common/command_executor_interface.hpp"
#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {

// A DO the gateway drives: write.enable with write.dataType "digital_output".
bool isDioOutputPoint(const PointDefinition& point);
// GPIO level of a DO's startup state: write.startupValue (0/1) when configured, else 0 (inactive);
// activeHigh=false inverts it.
bool dioStartupGpioHigh(const PointDefinition& point);
// Drives every DO of the config (enabled meters, or the top-level points) to its startup state as an
// output. Call once at driver start, before collection and writeback run. A pin that fails is
// logged and listed in the result; the collector drives it once it becomes available. Outputs are
// left as they are when the driver exits.
std::vector<std::string> applyDioStartupOutputs(const DeviceConfig& config, IGpioPort& gpioPort);

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
