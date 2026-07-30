#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "edge_gateway/compat.hpp"
#include "edge_gateway/models.hpp"
#include "edge_gateway/scada_models.hpp"

struct ScadaSceneResolvedTag {
    edge_gateway::ScadaTag tag;
    edge_gateway::ScadaRuntimeMapping mapping;
};

struct ScadaSceneWriteResult {
    bool accepted = false;
    std::string message;
};

// Rendering is shared by edge and Windows. Runtime data and control dispatch stay transport-specific.
class ScadaSceneRuntimeSource {
public:
    virtual ~ScadaSceneRuntimeSource() = default;

    virtual const std::string& nodeId() const = 0;
    virtual edge_gateway::Optional<ScadaSceneResolvedTag> resolveTag(const std::string& tagId) const = 0;
    virtual edge_gateway::Optional<edge_gateway::StoredPointValue> readTag(
        const std::string& tagId,
        std::int64_t nowMs
    ) const = 0;
    virtual std::vector<edge_gateway::StoredPointValue> readIndexes(
        const std::vector<std::uint32_t>& indexes,
        std::int64_t nowMs
    ) const = 0;
    virtual ScadaSceneWriteResult submitWrite(
        const std::string& tagId,
        edge_gateway::PendingWriteCommand command
    ) = 0;
};
