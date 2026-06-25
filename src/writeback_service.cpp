#include "edge_gateway/writeback_service.hpp"

namespace edge_gateway {

WritebackService::WritebackService(MemoryPointStore& store, CommandExecutor& executor)
    : store_(store), executor_(executor), priorityControlLease_(nullptr) {
}

WritebackService::WritebackService(
    MemoryPointStore& store,
    CommandExecutor& executor,
    const PriorityControlLease* priorityControlLease
) : store_(store),
    executor_(executor),
    priorityControlLease_(priorityControlLease) {
}

std::vector<CommandResult> WritebackService::processPendingWrites(
    std::int64_t nowMs,
    std::size_t limit
) {
    const auto activeLease = priorityControlLease_ == nullptr
        ? NullOpt
        : priorityControlLease_->activeLease(nowMs);
    const auto commands = activeLease
        ? store_.drainPendingWriteCommandsByCmdId(activeLease->cmdId, limit)
        : store_.drainPendingWriteCommands(limit);
    std::vector<CommandResult> results;
    results.reserve(commands.size());
    for (const auto& command : commands) {
        results.push_back(executor_.executeByIndex(command.cmdId, command.index, command.value, nowMs));
    }
    return results;
}

}  // namespace edge_gateway
