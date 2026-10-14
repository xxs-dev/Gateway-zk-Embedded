#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "edge_gateway/compat.hpp"
#include "edge_gateway/models.hpp"
#include "edge_gateway/scada_models.hpp"

struct ScadaSceneResolvedTag {
    edge_gateway::ScadaTag tag;
    edge_gateway::ScadaRuntimeMapping mapping;
    edge_gateway::Optional<double> writeMinValue;
    edge_gateway::Optional<double> writeMaxValue;
    double writeStep = 0.0;
};

struct ScadaSceneWriteResult {
    bool accepted = false;
    std::string message;
};

struct ScadaSceneWriteTarget {
    std::string tagId;
    double value = 0.0;
};

// Rendering is shared by edge and Windows. Runtime data and control dispatch stay transport-specific.
class ScadaSceneRuntimeSource {
public:
    virtual ~ScadaSceneRuntimeSource() = default;

    void setOperatorAuthorization(std::function<bool()> authorize) {
        operatorAuthorization_ = std::move(authorize);
    }
    ScadaSceneWriteResult submitOperatorWrite(
        const std::string& tagId, edge_gateway::PendingWriteCommand command
    ) {
        if (operatorAuthorization_ && !operatorAuthorization_())
            return {false, "Local operator session expired; log in again"};
        return submitWrite(tagId, std::move(command));
    }
    ScadaSceneWriteResult submitOperatorWriteGroup(
        const std::vector<ScadaSceneWriteTarget>& targets, edge_gateway::PendingWriteCommand command
    ) {
        if (operatorAuthorization_ && !operatorAuthorization_())
            return {false, "Local operator session expired; log in again"};
        return submitWriteGroup(targets, std::move(command));
    }

    virtual const std::string& nodeId() const = 0;
    // Optional read-only EMS membership snapshot; unsupported transports stay unknown.
    virtual std::string clusterStatusJson() const { return {}; }
    virtual std::int64_t clusterStatusTtlMs() const { return 5000; }
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
    virtual ScadaSceneWriteResult submitWriteGroup(
        const std::vector<ScadaSceneWriteTarget>& targets,
        edge_gateway::PendingWriteCommand command
    ) = 0;
    virtual edge_gateway::Optional<edge_gateway::WritebackResultRecord> getWritebackResult(
        const std::string& tagId,
        const std::string& cmdId
    ) const = 0;

private:
    std::function<bool()> operatorAuthorization_;
};
