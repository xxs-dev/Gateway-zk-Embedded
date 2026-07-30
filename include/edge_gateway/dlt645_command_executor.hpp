#pragma once

#include <memory>

#include "edge_gateway/common/command_executor_interface.hpp"
#include "edge_gateway/dlt645_client.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {

class Dlt645CommandExecutor : public ICommandExecutor {
public:
    Dlt645CommandExecutor(
        DeviceConfig config,
        MemoryPointStore& store,
        std::shared_ptr<Dlt645Client> client
    );

    CommandResult executeByIndex(
        const std::string& cmdId,
        std::uint32_t index,
        double value,
        std::int64_t nowMs
    ) const override;

private:
    const PointDefinition& findPointByIndex(std::uint32_t index) const;
    void validateWrite(const PointDefinition& point, double value) const;

    DeviceConfig config_;
    MemoryPointStore& store_;
    std::shared_ptr<Dlt645Client> client_;
};

}  // namespace edge_gateway
