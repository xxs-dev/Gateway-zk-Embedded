#pragma once
#include "edge_gateway/compute_execution_observation.hpp"

namespace edge_gateway {
namespace compute_observation_detail {
// Sanitize an optional extension without accepting arbitrary journal paths or
// returning unrelated graph state. No disk state is interpreted as a live owner.
inline std::string commands(const json::JsonValue& value, std::int64_t sampledAt, std::size_t& total) {
    const auto saved = number(value, "stateSavedAt", true, 0);
    if (saved > sampledAt) throw std::runtime_error("future command persistence time");
    const auto faulted = required(value, "storageFault").asBool();
    const auto& nodes = required(value, "nodes").asArray().values;
    total += nodes.size();
    if (nodes.size() > 1024 || total > 4096) throw std::runtime_error("command observation limit exceeded");
    std::ostringstream out;
    out.imbue(std::locale::classic()); out << std::setprecision(17);
    out << "{\"stateSavedAt\":" << saved << ",\"storageFault\":" << (faulted ? "true" : "false") << ",\"nodes\":[";
    std::set<std::string> seen;
    bool comma = false;
    for (const auto& ptr : nodes) {
        const auto& n = *ptr;
        const auto id = text(n, "nodeId", 256);
        const auto cmd = required(n, "cmdId").asString();
        for (unsigned char c : cmd) if (c < 32) throw std::runtime_error("invalid command identity");
        const auto target = number(n, "targetIndex", true, 1), output = number(n, "outputIndex", true, 1);
        const auto index = number(n, "index", true, 0), requested = number(n, "requestedAt", true, 0);
        const auto last = number(n, "lastScan", true, 0), gap = number(n, "maxGapMs", true, 1);
        const auto status = number(n, "status"), fault = number(n, "fault");
        const auto pending = required(n, "pending").asBool(), armed = required(n, "armed").asBool(),
            confirmed = required(n, "confirmed").asBool();
        const auto v = required(n, "value").asNumber();
        const auto expected = fault ? fault : pending ? 2 : !armed ? 0 : confirmed ? 3 : 1;
        if (!seen.insert(id).second || target > 4294967295.0 || output > 4294967295.0 || target == output ||
            cmd.size() > 63 || (fault && armed) ||
            last > sampledAt || requested > sampledAt || !std::isfinite(v) || fault < -4 || fault > 0 || status != expected ||
            (pending && confirmed) || (cmd.empty() ? (index || requested || pending || confirmed) : (index != target || requested <= 0)))
            throw std::runtime_error("invalid command owner observation");
        if (comma) out << ','; comma = true;
        out << "{\"nodeId\":\"" << escapeDiagnosticOutputJson(id) << "\",\"cmdId\":\"" << escapeDiagnosticOutputJson(cmd)
            << "\",\"targetIndex\":" << target << ",\"outputIndex\":" << output << ",\"index\":" << index
            << ",\"value\":" << v << ",\"requestedAt\":" << requested << ",\"lastScan\":" << last
            << ",\"maxGapMs\":" << gap << ",\"status\":" << status << ",\"fault\":" << fault
            << ",\"pending\":" << (pending ? "true" : "false") << ",\"armed\":" << (armed ? "true" : "false")
            << ",\"confirmed\":" << (confirmed ? "true" : "false") << '}';
    }
    out << "]}";
    return out.str();
}
} // namespace compute_observation_detail
} // namespace edge_gateway
