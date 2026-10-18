#pragma once

#include <cmath>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>
#include "edge_gateway/diagnostic_output_json.hpp"
#include "edge_gateway/json_value.hpp"

namespace edge_gateway {
namespace compute_observation_detail {
inline const json::JsonValue& required(const json::JsonValue& object, const char* key) {
    const auto* value = object.find(key);
    if (!value) throw std::runtime_error("execution observation field missing");
    return *value;
}
inline std::string text(const json::JsonValue& object, const char* key, std::size_t max = 4096) {
    const auto value = required(object, key).asString();
    if (value.empty() || value.size() > max) throw std::runtime_error("invalid execution text");
    for (unsigned char c : value) if (c < 32) throw std::runtime_error("invalid execution text");
    return value;
}
inline double number(const json::JsonValue& object, const char* key, bool integer = true, double minimum = -9007199254740991.0) {
    const auto value = required(object, key).asNumber();
    if (!std::isfinite(value) || value < minimum || value > 9007199254740991.0 || (integer && value != std::floor(value)))
        throw std::runtime_error("invalid execution number");
    return value;
}
inline std::string engine(const json::JsonValue& value) {
    std::ostringstream out;
    out.imbue(std::locale::classic()); out << std::setprecision(17);
    out << "{\"scanIntervalMs\":" << number(value, "scanIntervalMs", true, 1)
        << ",\"maxRuleEvalPerScan\":" << number(value, "maxRuleEvalPerScan", true, 0)
        << ",\"maxWritesPerScan\":" << number(value, "maxWritesPerScan", true, 0)
        << ",\"graphNodeErrorsUnhealthy\":" << (required(value, "graphNodeErrorsUnhealthy").asBool() ? "true" : "false")
        << ",\"enabledRuleOrder\":[";
    const auto& rules = required(value, "enabledRuleOrder").asArray().values;
    if (rules.size() > 1024) throw std::runtime_error("too many execution rules");
    std::set<std::string> seen;
    bool first = true;
    for (const auto& rule : rules) {
        const auto code = text(*rule, "ruleCode", 256);
        if (!seen.insert(code).second) throw std::runtime_error("duplicate execution rule");
        if (!first) out << ',';
        first = false;
        out << "{\"ruleCode\":\"" << escapeDiagnosticOutputJson(code)
            << "\",\"type\":\"" << escapeDiagnosticOutputJson(text(*rule, "type", 256)) << "\"}";
    }
    out << "]}";
    return out.str();
}
inline std::string graph(const json::JsonValue& value) {
    std::ostringstream out;
    out.imbue(std::locale::classic()); out << std::setprecision(17);
    const auto profile = text(value, "profileSha256", 64);
    if (profile.size() != 64 || profile.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::runtime_error("invalid execution profile digest");
    out << "{\"triggerType\":\"" << escapeDiagnosticOutputJson(text(value, "triggerType", 256))
        << "\",\"intervalMs\":" << number(value, "intervalMs", true, 1)
        << ",\"minIntervalMs\":" << number(value, "minIntervalMs", true, 0)
        << ",\"deadband\":" << number(value, "deadband", false)
        << ",\"outputTtlMs\":" << number(value, "outputTtlMs")
        << ",\"rateLimitMaxGapMs\":" << number(value, "rateLimitMaxGapMs", true, 0)
        << ",\"restoreSetpointOutputs\":" << (required(value, "restoreSetpointOutputs").asBool() ? "true" : "false")
        << ",\"inputMaxFutureSkewMs\":" << number(value, "inputMaxFutureSkewMs")
        << ",\"stateFile\":\"" << escapeDiagnosticOutputJson(text(value, "stateFile"))
        << "\",\"profileSha256\":\"" << profile << "\",\"inputIndexes\":[";
    const auto& indexes = required(value, "inputIndexes").asArray().values;
    if (indexes.size() > 4096) throw std::runtime_error("too many trigger indexes");
    double previous = 0;
    for (const auto& index : indexes) {
        const auto n = index->asNumber();
        if (!std::isfinite(n) || n != std::floor(n) || n <= previous || n > 4294967295.0)
            throw std::runtime_error("invalid trigger index order");
        if (previous) out << ',';
        out << n;
        previous = n;
    }
    out << "]}";
    return out.str();
}
} // namespace compute_observation_detail
} // namespace edge_gateway
