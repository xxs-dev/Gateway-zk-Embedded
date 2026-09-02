#include "edge_gateway/legacy_telemetry_payload.hpp"

#include <cmath>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace edge_gateway {

namespace {

std::string escapeJson(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const auto ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

}  // namespace

std::string buildLegacyTelemetryPayload(
    const std::vector<StoredPointValue>& values,
    const std::vector<LegacyTelemetryPointMapping>& pointMappings,
    bool mappedOnly,
    std::int64_t nowMs
) {
    using LegacyPoint = std::pair<std::string, const StoredPointValue*>;
    std::vector<std::pair<std::string, std::vector<LegacyPoint>>> meters;
    std::unordered_map<std::string, std::size_t> meterPositions;
    std::unordered_map<std::uint32_t, const LegacyTelemetryPointMapping*> mappingsByIndex;
    for (const auto& mapping : pointMappings) {
        mappingsByIndex[mapping.index] = &mapping;
    }

    const auto appendPoint = [&](const std::string& meterCode,
                                 const std::string& pointCode,
                                 const StoredPointValue* value) {
        const auto inserted = meterPositions.emplace(meterCode, meters.size());
        if (inserted.second) {
            meters.emplace_back(meterCode, std::vector<LegacyPoint>());
        }
        meters[inserted.first->second].second.emplace_back(pointCode, value);
    };

    if (mappedOnly && !pointMappings.empty()) {
        std::unordered_map<std::uint32_t, const StoredPointValue*> valuesByIndex;
        for (const auto& value : values) {
            valuesByIndex[value.index] = &value;
        }
        for (const auto& mapping : pointMappings) {
            const auto valueIt = valuesByIndex.find(mapping.index);
            appendPoint(
                mapping.meterCode,
                mapping.pointCode,
                valueIt == valuesByIndex.end() ? nullptr : valueIt->second
            );
        }
    } else {
        for (const auto& value : values) {
            if (value.meterCode.empty() || value.pointCode.empty()) {
                continue;
            }
            std::string meterCode = value.meterCode;
            std::string pointCode = value.pointCode;
            const auto mappingIt = mappingsByIndex.find(value.index);
            if (mappingIt != mappingsByIndex.end()) {
                meterCode = mappingIt->second->meterCode;
                pointCode = mappingIt->second->pointCode;
            }
            appendPoint(meterCode, pointCode, &value);
        }
    }

    std::ostringstream payload;
    payload << R"({"data":[)";
    for (std::size_t meterIndex = 0; meterIndex < meters.size(); ++meterIndex) {
        if (meterIndex > 0) payload << ',';
        payload << R"({"meterid":")" << escapeJson(meters[meterIndex].first)
                << R"(","metrics":[{)";
        const auto& points = meters[meterIndex].second;
        for (std::size_t pointIndex = 0; pointIndex < points.size(); ++pointIndex) {
            if (pointIndex > 0) payload << ',';
            const auto& point = points[pointIndex];
            payload << '"' << escapeJson(point.first) << R"(":")";
            if (point.second == nullptr) {
                payload << "0.0000";
            } else if (std::isfinite(point.second->value)) {
                payload << std::fixed << std::setprecision(4) << point.second->value;
            }
            payload << '"';
        }
        payload << "}]}";
    }
    payload << R"(],"msgid":)" << nowMs
            << R"(,"split":"false","timestamp":)" << nowMs << '}';
    return payload.str();
}

}  // namespace edge_gateway
