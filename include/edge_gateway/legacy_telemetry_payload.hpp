#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

std::string buildLegacyTelemetryPayload(
    const std::vector<StoredPointValue>& values,
    const std::vector<LegacyTelemetryPointMapping>& pointMappings,
    bool mappedOnly,
    std::int64_t nowMs
);

}  // namespace edge_gateway
