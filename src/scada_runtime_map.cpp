#include "edge_gateway/scada_runtime_map.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace edge_gateway {

namespace {

std::string pairKey(const std::string& first, const std::string& second) {
    return first + '\x1f' + second;
}

std::string semanticKey(const std::string& deviceId, const std::string& semanticRole) {
    return deviceId + '\x1f' + semanticRole;
}

void addUniqueLookup(
    std::unordered_map<std::string, std::string>& lookup,
    const std::string& key,
    const std::string& tagId
) {
    const auto inserted = lookup.emplace(key, tagId);
    if (!inserted.second && inserted.first->second != tagId) {
        inserted.first->second.clear();
    }
}

}  // namespace

ScadaTagResolver::ScadaTagResolver(const ScadaProject& project, const std::string& machineCode) {
    const auto node = std::find_if(project.nodes.begin(), project.nodes.end(), [&](const ScadaNode& item) {
        return item.machineCode == machineCode;
    });
    if (node == project.nodes.end()) {
        throw std::runtime_error("SCADA project does not contain machineCode: " + machineCode);
    }
    nodeId_ = node->nodeId;

    std::unordered_map<std::string, ScadaRuntimeMapping> mappings;
    for (const auto& mapping : project.runtimeMappings) {
        if (mapping.nodeId == nodeId_) {
            mappings[mapping.tagId] = mapping;
        }
    }

    for (const auto& tag : project.tags) {
        if (tag.nodeId != nodeId_) continue;
        ScadaResolvedTag resolved;
        resolved.tag = tag;
        const auto mapping = mappings.find(tag.tagId);
        if (mapping != mappings.end()) {
            resolved.mapping = mapping->second;
        } else {
            resolved.mapping.nodeId = tag.nodeId;
            resolved.mapping.tagId = tag.tagId;
            resolved.mapping.index = tag.indexFallback;
            resolved.mapping.writable = tag.access != ScadaTagAccess::Read;
            resolved.mapping.dataType = tag.dataType;
            resolved.mapping.unit = tag.unit;
        }
        byTag_[tag.tagId] = resolved;
        if (!tag.meterCode.empty() && !tag.pointCode.empty()) {
            addUniqueLookup(byMeterPoint_, pairKey(tag.meterCode, tag.pointCode), tag.tagId);
        }
        if (!tag.semanticRole.empty()) {
            addUniqueLookup(bySemantic_, semanticKey(tag.deviceId, tag.semanticRole), tag.tagId);
        }
    }
}

const std::string& ScadaTagResolver::nodeId() const {
    return nodeId_;
}

Optional<ScadaResolvedTag> ScadaTagResolver::resolveTag(const std::string& tagId) const {
    const auto item = byTag_.find(tagId);
    return item == byTag_.end() ? Optional<ScadaResolvedTag>(NullOpt) : Optional<ScadaResolvedTag>(item->second);
}

Optional<ScadaResolvedTag> ScadaTagResolver::resolveMeterPoint(
    const std::string& meterCode,
    const std::string& pointCode
) const {
    const auto item = byMeterPoint_.find(pairKey(meterCode, pointCode));
    if (item == byMeterPoint_.end() || item->second.empty()) return NullOpt;
    return resolveTag(item->second);
}

Optional<ScadaResolvedTag> ScadaTagResolver::resolveSemantic(
    const std::string& deviceId,
    const std::string& semanticRole
) const {
    const auto item = bySemantic_.find(semanticKey(deviceId, semanticRole));
    if (item == bySemantic_.end() || item->second.empty()) return NullOpt;
    return resolveTag(item->second);
}

std::vector<std::uint32_t> ScadaTagResolver::indexesForScreen(const ScadaScreen& screen) const {
    std::vector<std::uint32_t> indexes;
    std::unordered_set<std::uint32_t> seen;
    for (const auto& tagId : tagIdsForScreen(screen)) {
        const auto resolved = resolveTag(tagId);
        if (resolved && resolved->mapping.index > 0 && seen.insert(resolved->mapping.index).second) {
            indexes.push_back(resolved->mapping.index);
        }
    }
    std::sort(indexes.begin(), indexes.end());
    return indexes;
}

std::vector<std::string> ScadaTagResolver::tagIdsForScreen(const ScadaScreen& screen) const {
    std::vector<std::string> tagIds;
    std::unordered_set<std::string> seen;
    const auto addTag = [&](const std::string& nodeId, const std::string& tagId) {
        if (nodeId == nodeId_ && !tagId.empty() && seen.insert(tagId).second) {
            tagIds.push_back(tagId);
        }
    };
    for (const auto& widget : screen.widgets) {
        if (!widget.visible) continue;
        for (const auto& binding : widget.bindings) {
            addTag(binding.nodeId, binding.tagId);
        }
        for (const auto& rule : widget.stateRules) {
            for (const auto& condition : rule.conditions) {
                addTag(condition.nodeId, condition.tagId);
            }
        }
        addTag(widget.action.nodeId, widget.action.tagId);
    }
    return tagIds;
}

ScadaRuntimeMap::ScadaRuntimeMap(
    const ScadaProject& project,
    const std::string& machineCode,
    PointStoreRouter& router
) : resolver_(project, machineCode), router_(router) {
}

const ScadaTagResolver& ScadaRuntimeMap::resolver() const {
    return resolver_;
}

Optional<StoredPointValue> ScadaRuntimeMap::readTag(const std::string& tagId, std::int64_t nowMs) const {
    const auto resolved = resolver_.resolveTag(tagId);
    if (!resolved || resolved->mapping.index == 0) return NullOpt;
    return router_.getLatestByIndex(resolved->mapping.index, nowMs);
}

std::vector<StoredPointValue> ScadaRuntimeMap::readTags(
    const std::vector<std::string>& tagIds,
    std::int64_t nowMs
) const {
    std::vector<std::uint32_t> indexes;
    std::unordered_set<std::uint32_t> seen;
    for (const auto& tagId : tagIds) {
        const auto resolved = resolver_.resolveTag(tagId);
        if (resolved && resolved->mapping.index > 0 && seen.insert(resolved->mapping.index).second) {
            indexes.push_back(resolved->mapping.index);
        }
    }
    return router_.getLatestByIndexes(indexes, nowMs);
}

std::vector<StoredPointValue> ScadaRuntimeMap::readIndexes(
    const std::vector<std::uint32_t>& indexes,
    std::int64_t nowMs
) const {
    return router_.getLatestByIndexes(indexes, nowMs);
}

std::vector<StoredPointValue> ScadaRuntimeMap::readScreen(
    const ScadaScreen& screen,
    std::int64_t nowMs
) const {
    return router_.getLatestByIndexes(resolver_.indexesForScreen(screen), nowMs);
}

CommandSubmitResult ScadaRuntimeMap::submitWrite(
    const std::string& tagId,
    PendingWriteCommand command
) {
    const auto resolved = resolver_.resolveTag(tagId);
    if (!resolved) {
        CommandSubmitResult result;
        result.message = "SCADA tag not found";
        return result;
    }
    if (resolved->tag.access == ScadaTagAccess::Read || !resolved->mapping.writable) {
        CommandSubmitResult result;
        result.message = "SCADA tag is read-only";
        return result;
    }
    const auto route = router_.routeByIndex(resolved->mapping.index);
    if (!route) {
        CommandSubmitResult result;
        result.message = "SCADA runtime route was not found on this edge";
        return result;
    }
    if (!route->writable) {
        CommandSubmitResult result;
        result.route = *route;
        result.message = "edge point route is read-only";
        return result;
    }
    if (!resolved->mapping.sharedMemoryName.empty() &&
        route->sharedMemoryName != resolved->mapping.sharedMemoryName) {
        CommandSubmitResult result;
        result.route = *route;
        result.message = "SCADA runtime mapping does not match edge shared memory route";
        return result;
    }
    command.index = resolved->mapping.index;
    return router_.submitWriteCommand(command);
}

}  // namespace edge_gateway
