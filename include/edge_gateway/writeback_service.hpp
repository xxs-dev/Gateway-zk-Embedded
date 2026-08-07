#pragma once

#include <vector>

#include "edge_gateway/command_executor.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"
#include "edge_gateway/priority_control_lease.hpp"

namespace edge_gateway {

std::vector<PendingWriteCommand> drainScheduledWriteCommands(
    MemoryPointStore& store,
    const PriorityControlLease* priorityControlLease,
    std::int64_t nowMs,
    std::size_t limit = 0
);

WritebackResultRecord beginWritebackResult(
    const PendingWriteCommand& command,
    std::int64_t startedAt
);

void completeWritebackResult(
    WritebackResultRecord& result,
    bool success,
    const std::string& message,
    const std::string& stage,
    std::int64_t completedAt,
    bool verifyAttempted = false,
    bool verifyPassed = false
);

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
