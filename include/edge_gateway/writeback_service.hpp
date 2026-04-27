#pragma once

#include <vector>

#include "edge_gateway/command_executor.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {

class WritebackService {
public:
    WritebackService(MemoryPointStore& store, CommandExecutor& executor);

    std::vector<CommandResult> processPendingWrites(std::int64_t nowMs, std::size_t limit = 0);

private:
    MemoryPointStore& store_;
    CommandExecutor& executor_;
};

}  // namespace edge_gateway
