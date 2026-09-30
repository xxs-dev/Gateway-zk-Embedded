#pragma once

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include "edge_gateway/json_value.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {
namespace ota_request_json {

inline json::JsonValue parseRoot(const std::string& payload) {
    auto root = json::JsonParser(payload, 8, 128).parse();
    if (!root.isObject()) throw std::invalid_argument("OTA request must be a JSON object");
    std::unordered_set<std::string> keys;
    for (const auto& member : root.asObject().values)
        if (!keys.insert(member.key).second) throw std::invalid_argument("duplicate OTA root field");
    return root;
}

inline std::string stringField(const json::JsonValue& root, const char* name, const std::string& fallback = {}) {
    const auto* value = root.find(name);
    if (!value) return fallback;
    if (!value->isString()) throw std::invalid_argument(std::string("invalid OTA string field: ") + name);
    return value->asString();
}

inline OtaRequest request(const json::JsonValue& root) {
    OtaRequest result;
    result.jobId = stringField(root, "jobId");
    result.machineCode = stringField(root, "machineCode");
    result.packageType = stringField(root, "packageType");
    result.artifactUrl = stringField(root, "artifactUrl");
    result.version = stringField(root, "version");
    result.sha256 = stringField(root, "sha256");
    result.upgradeMode = stringField(root, "upgradeMode");
    result.signature = stringField(root, "signature");
    if (const auto* size = root.find("size")) {
        if (!size->isNumber() || !std::isfinite(size->asNumber()) || size->asNumber() < 0 ||
            size->asNumber() > 9007199254740991.0 || std::floor(size->asNumber()) != size->asNumber())
            throw std::invalid_argument("invalid OTA size");
        result.size = static_cast<std::uint64_t>(size->asNumber());
    }
    if (const auto* ts = root.find("ts")) {
        if (!ts->isNumber() || !std::isfinite(ts->asNumber()) || std::abs(ts->asNumber()) > 9007199254740991.0 ||
            std::floor(ts->asNumber()) != ts->asNumber())
            throw std::invalid_argument("invalid OTA ts");
        result.ts = static_cast<std::int64_t>(ts->asNumber());
    }
    return result;
}

}  // namespace ota_request_json
}  // namespace edge_gateway
