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
    // Readable points sorted by function/address; definitions are copied only into output tasks.
    static std::vector<ReadTask> buildSorted(
        const std::vector<const PointDefinition*>& points,
        int maxBatchRegisters,
        bool batchOptimize = true
    );
};

}  // namespace edge_gateway
