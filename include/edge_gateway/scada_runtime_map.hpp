#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "edge_gateway/compat.hpp"
#include "edge_gateway/point_store_router.hpp"
#include "edge_gateway/scada_models.hpp"

namespace edge_gateway {

struct ScadaResolvedTag {
    ScadaTag tag;
    ScadaRuntimeMapping mapping;
};

class ScadaTagResolver {
public:
    ScadaTagResolver(const ScadaProject& project, const std::string& machineCode);

    const std::string& nodeId() const;
    Optional<ScadaResolvedTag> resolveTag(const std::string& tagId) const;
    Optional<ScadaResolvedTag> resolveMeterPoint(
        const std::string& meterCode,
        const std::string& pointCode
    ) const;
    Optional<ScadaResolvedTag> resolveSemantic(
        const std::string& deviceId,
        const std::string& semanticRole
    ) const;
    std::vector<std::string> tagIdsForScreen(const ScadaScreen& screen) const;
    std::vector<std::uint32_t> indexesForScreen(const ScadaScreen& screen) const;

private:
    std::string nodeId_;
    std::unordered_map<std::string, ScadaResolvedTag> byTag_;
    std::unordered_map<std::string, std::string> byMeterPoint_;
    std::unordered_map<std::string, std::string> bySemantic_;
};

class ScadaRuntimeMap {
public:
    ScadaRuntimeMap(
        const ScadaProject& project,
        const std::string& machineCode,
        PointStoreRouter& router
    );

    const ScadaTagResolver& resolver() const;
    Optional<StoredPointValue> readTag(const std::string& tagId, std::int64_t nowMs) const;
    std::vector<StoredPointValue> readTags(
        const std::vector<std::string>& tagIds,
        std::int64_t nowMs
    ) const;
    std::vector<StoredPointValue> readIndexes(
        const std::vector<std::uint32_t>& indexes,
        std::int64_t nowMs
    ) const;
    std::vector<StoredPointValue> readScreen(const ScadaScreen& screen, std::int64_t nowMs) const;
    CommandSubmitResult submitWrite(const std::string& tagId, PendingWriteCommand command);

private:
    ScadaTagResolver resolver_;
    PointStoreRouter& router_;
};

}  // namespace edge_gateway
