#pragma once

#include <vector>

#include "edge_gateway/command_executor.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"
#include "edge_gateway/priority_control_lease.hpp"

namespace edge_gateway {

class WritebackService {
public:
    WritebackService(MemoryPointStore& store, CommandExecutor& executor);
    WritebackService(
        MemoryPointStore& store,
        CommandExecutor& executor,
        const PriorityControlLease* priorityControlLease
    );

    std::vector<CommandResult> processPendingWrites(std::int64_t nowMs, std::size_t limit = 0);

private:
    MemoryPointStore& store_;
    CommandExecutor& executor_;
    const PriorityControlLease* priorityControlLease_ = nullptr;
};

}  // namespace edge_gateway
