#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <stdexcept>

#include "edge_gateway/models.hpp"
#include "edge_gateway/cluster_write_authorization.hpp"

namespace edge_gateway {

class ICommandExecutor {
public:
    virtual ~ICommandExecutor() = default;

    virtual CommandResult executePending(
        const PendingWriteCommand& command, std::int64_t nowMs,
        const BeforePhysicalWrite& beforeWrite = BeforePhysicalWrite()
    ) const {
        if (command.clusterAuthorization || beforeWrite)
            throw std::runtime_error("cluster physical write is unsupported by this executor");
        return executeByIndex(command.cmdId, command.index, command.value, nowMs);
    }

    virtual CommandResult executeByIndex(
        const std::string& cmdId,
        std::uint32_t index,
        double value,
        std::int64_t nowMs
    ) const = 0;
};

class UnsupportedCommandExecutor : public ICommandExecutor {
public:
    explicit UnsupportedCommandExecutor(DeviceConfig config) : config_(std::move(config)) {
    }

    CommandResult executeByIndex(
        const std::string& cmdId,
        std::uint32_t index,
        double value,
        std::int64_t nowMs
    ) const override {
        CommandResult result;
        result.cmdId = cmdId;
        result.machineCode = config_.machineCode;
        result.meterCode = config_.meterCode;
        result.index = index;
        result.requestedValue = value;
        result.ts = nowMs;
        result.success = false;
        result.message = "writeback is not supported for protocol: " + config_.protocol.type;
        for (const auto& point : config_.points) {
            if (point.index == index) {
                result.pointCode = point.pointCode;
                break;
            }
        }
        return result;
    }

private:
    DeviceConfig config_;
};

}  // namespace edge_gateway
