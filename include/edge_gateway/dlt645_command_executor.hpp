#pragma once

#include <memory>

#include "edge_gateway/common/command_executor_interface.hpp"
#include "edge_gateway/dlt645_client.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {

// C=14H writes. Writes are off unless dlt645.write.enabled and the point's write.enable are set.
// Points with a read-back (write.verifyAfterWrite + verifyByRead + write.dlt645.verifyPointCode;
// mandatory for trip/close) are checked before and confirmed after execution:
//  - before: the read-back point is read; unreadable -> rejected, nothing sent; already at the
//    target value -> success without sending;
//  - after the 94H acknowledgement: polled until it shows the target value (verifyPassed) or
//    write.dlt645.verifyTimeoutMs passes (failure; outcomeUnknown when it could not be read).
// A sent request without a conclusive reply reports outcomeUnknown. Nothing is retried.
class Dlt645CommandExecutor : public ICommandExecutor {
public:
    Dlt645CommandExecutor(
        DeviceConfig config,
        MemoryPointStore& store,
        std::shared_ptr<Dlt645Client> client
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
    const PointDefinition* readBackPoint(const PointDefinition& point) const;
    double readValue(const PointDefinition& point) const;
    void confirmByReadBack(
        const PointDefinition& point,
        const PointDefinition& readBack,
        double expected,
        CommandResult& result
    ) const;
    void validateWrite(const PointDefinition& point, double value) const;

    DeviceConfig config_;
    MemoryPointStore& store_;
    std::shared_ptr<Dlt645Client> client_;
};

}  // namespace edge_gateway
