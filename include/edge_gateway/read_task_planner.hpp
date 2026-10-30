#pragma once

#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

class ReadTaskPlanner {
public:
    // maxGapRegisters > 0 lets one request span up to that many unconfigured registers between two
    // points (collect.maxGapRegisters); 0 merges only adjacent or overlapping points.
    static std::vector<ReadTask> build(
        const std::vector<PointDefinition>& points,
        int maxBatchRegisters,
        bool batchOptimize = true,
        int maxGapRegisters = 0
    );
    // Readable points sorted by function/address; definitions are copied only into output tasks.
    static std::vector<ReadTask> buildSorted(
        const std::vector<const PointDefinition*>& points,
        int maxBatchRegisters,
        bool batchOptimize = true,
        int maxGapRegisters = 0
    );
};

}  // namespace edge_gateway
