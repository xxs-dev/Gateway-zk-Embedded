#pragma once

#include <memory>
#include <string>

#include "edge_gateway/common/command_executor_interface.hpp"
#include "edge_gateway/dlt645_client.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {

// Writes (2007 C=14H, 1997 C=04H) and DL/T 645-2007 remote control (C=1CH, dlt645_remote_control).
// Off unless dlt645.write.enabled and the point's write.enable are set.
// Points with a read-back (write.verifyAfterWrite + verifyByRead; mandatory for trip/close) are
// checked before and confirmed after execution. The read-back is either a DL/T 645 point of the same
// meter (write.dlt645.verifyPointCode, read from the meter) or a point of another device on this
// gateway (write.dlt645.verifyIndex, e.g. a DIO auxiliary contact, read from the shared point store):
//  - before: unreadable (store: missing, stale or bad quality) -> rejected, nothing sent; already at
//    the target value -> success without sending;
//  - after the acknowledgement (94H/84H/9CH): polled until it shows the target value (verifyPassed) or
//    write.dlt645.verifyTimeoutMs passes: another value read -> feedbackMismatch (the meter may have
//    acted; not a definite failure), nothing read -> outcomeUnknown; for the store only values
//    collected after the command count. Both report success=false.
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
    struct ReadBackSample {
        double value = 0.0;
        bool fresh = false;  // collected after the requested time
    };

    const PointDefinition& findPointByIndex(std::uint32_t index) const;
    static std::int64_t wallNowMs();
    static bool hasReadBack(const PointDefinition& point);
    static std::string readBackLabel(const PointDefinition& point);
    const PointDefinition& localReadBackPoint(const PointDefinition& point) const;
    ReadBackSample readBackValue(const PointDefinition& point, std::int64_t notBeforeMs) const;
    void confirmByReadBack(
        const PointDefinition& point,
        const std::string& label,
        double expected,
        std::int64_t sentAtMs,
        CommandResult& result
    ) const;
    void validateWrite(const PointDefinition& point, double value) const;

    DeviceConfig config_;
    MemoryPointStore& store_;
    std::shared_ptr<Dlt645Client> client_;
};

}  // namespace edge_gateway
