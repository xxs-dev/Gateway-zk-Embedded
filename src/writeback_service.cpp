#include "edge_gateway/writeback_service.hpp"

#include <algorithm>

namespace edge_gateway {

namespace {

std::int64_t nonNegativeDuration(std::int64_t endMs, std::int64_t startMs) {
    if (endMs <= 0 || startMs <= 0 || endMs < startMs) {
        return 0;
    }
    return endMs - startMs;
}

}  // namespace

std::vector<PendingWriteCommand> drainScheduledWriteCommands(
    MemoryPointStore& store,
    const PriorityControlLease* priorityControlLease,
    std::int64_t nowMs,
    std::size_t limit
) {
    const auto activeLease = priorityControlLease == nullptr
        ? NullOpt
        : priorityControlLease->activeLease(nowMs);
    return activeLease
        ? store.drainPendingWriteCommandsByCmdId(activeLease->cmdId, limit)
        : store.drainPendingWriteCommands(limit);
}

WritebackResultRecord beginWritebackResult(
    const PendingWriteCommand& command,
    std::int64_t startedAt
) {
    WritebackResultRecord result;
    result.cmdId = command.cmdId;
    result.index = command.index;
    result.value = command.value;
    result.highPriority = command.highPriority;
    result.requestedAt = command.ts;
    result.acceptedAt = command.acceptedAt > 0 ? command.acceptedAt : command.ts;
    result.startedAt = startedAt;
    result.queueDelayMs = nonNegativeDuration(startedAt, result.acceptedAt);
    return result;
}

void completeWritebackResult(
    WritebackResultRecord& result,
    bool success,
    const std::string& message,
    const std::string& stage,
    std::int64_t completedAt,
    bool verifyAttempted,
    bool verifyPassed
) {
    result.completedAt = completedAt;
    result.success = success;
    result.message = message;
    result.stage = stage;
    result.deviceWriteMs = nonNegativeDuration(completedAt, result.startedAt);
    result.edgeElapsedMs = nonNegativeDuration(completedAt, result.acceptedAt);
    result.totalElapsedMs = std::max(result.edgeElapsedMs, result.queueDelayMs + result.deviceWriteMs);
    result.verifyAttempted = verifyAttempted;
    result.verifyPassed = verifyPassed;
}

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
    const auto commands = drainScheduledWriteCommands(store_, priorityControlLease_, nowMs, limit);
    std::vector<CommandResult> results;
    results.reserve(commands.size());
    for (const auto& command : commands) {
        results.push_back(executor_.executeByIndex(command.cmdId, command.index, command.value, nowMs));
    }
    return results;
}

}  // namespace edge_gateway
