#pragma once

#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

class ReadTaskPlanner {
public:
    static std::vector<ReadTask> build(
        const std::vector<PointDefinition>& points,
        int maxBatchRegisters,
        bool batchOptimize = true
    );
};

}  // namespace edge_gateway
