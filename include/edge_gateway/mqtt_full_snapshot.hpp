#pragma once

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <utility>

#include "edge_gateway/point_store_router.hpp"

namespace edge_gateway {

// Full identity is configuration-driven, even after TTL removes a sample slot.
// These publication-only placeholders must never be written back to PointStore.
inline std::vector<StoredPointValue> completeMqttFullSnapshot(
    std::vector<StoredPointValue> values,
    const std::vector<std::uint32_t>& indexes,
    const PointStoreRouter& router
) {
    std::unordered_set<std::uint32_t> present;
    present.reserve(values.size() + indexes.size());
    for (const auto& value : values) {
        present.insert(value.index);
    }
    const auto originalSize = values.size();
    for (const auto index : indexes) {
        if (!present.insert(index).second) {
            continue;
        }
        const auto route = router.routeByIndex(index);
        if (!route) {
            continue;
        }
        StoredPointValue missing;
        missing.index = route->index;
        missing.machineCode = route->machineCode;
        missing.meterCode = route->meterCode;
        missing.pointCode = route->pointCode;
        missing.value = std::numeric_limits<double>::quiet_NaN(); // Existing JSON encoder emits null.
        missing.quality = 0;
        missing.stale = true;
        missing.ts = 0;
        missing.expireAt = 0;
        values.push_back(std::move(missing));
    }
    if (values.size() != originalSize) {
        std::sort(values.begin(), values.end(), [](const StoredPointValue& a, const StoredPointValue& b) {
            return a.index < b.index;
        });
    }
    return values;
}

}  // namespace edge_gateway
