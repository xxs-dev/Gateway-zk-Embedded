#pragma once

#include <memory>

#include "edge_gateway/common/collector_base.hpp"
#include "edge_gateway/common/command_executor_interface.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {

struct RuntimeDevice {
    DeviceConfig config;
    std::unique_ptr<ICollector> collector;
    std::unique_ptr<ICommandExecutor> executor;
};

}  // namespace edge_gateway
