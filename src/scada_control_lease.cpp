#include "edge_gateway/scada_control_lease.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace edge_gateway {

namespace {

std::string readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::size_t valuePosition(const std::string& text, const std::string& key, std::size_t start = 0) {
    const auto marker = std::string("\"") + key + "\"";
    const auto keyPos = text.find(marker, start);
    if (keyPos == std::string::npos) return std::string::npos;
    const auto colon = text.find(':', keyPos + marker.size());
    if (colon == std::string::npos) return std::string::npos;
    return text.find_first_not_of(" \t\r\n", colon + 1);
}

std::string stringValue(const std::string& text, const std::string& key, std::size_t start = 0) {
    const auto pos = valuePosition(text, key, start);
    if (pos == std::string::npos || pos >= text.size() || text[pos] != '"') return {};
    const auto end = text.find('"', pos + 1);
    return end == std::string::npos ? std::string() : text.substr(pos + 1, end - pos - 1);
}

bool boolValue(const std::string& text, const std::string& key, bool fallback) {
    const auto pos = valuePosition(text, key);
    if (pos == std::string::npos) return fallback;
    if (text.compare(pos, 4, "true") == 0) return true;
    if (text.compare(pos, 5, "false") == 0) return false;
    return fallback;
}

std::int64_t int64Value(const std::string& text, const std::string& key, std::int64_t fallback) {
    const auto pos = valuePosition(text, key);
    if (pos == std::string::npos) return fallback;
    char* end = nullptr;
    const auto value = std::strtoll(text.c_str() + pos, &end, 10);
    return end == text.c_str() + pos ? fallback : value;
}

bool containsMachineCode(const std::string& nodes, const std::string& machineCode) {
    std::size_t search = 0;
    while (true) {
        const auto marker = nodes.find("\"machineCode\"", search);
        if (marker == std::string::npos) return false;
        if (stringValue(nodes, "machineCode", marker) == machineCode) return true;
        search = marker + 13;
    }
}

}  // namespace

bool ScadaControlLease::isFresh(
    const std::string& projectDirectory,
    const std::string& leaseFile,
    const std::string& machineCode,
    std::int64_t nowMs,
    std::string* message
) {
    const auto assignMessage = [&](const std::string& value) {
        if (message != nullptr) *message = value;
    };
    const auto topology = readFile(projectDirectory + "/topology.json");
    if (topology.empty()) {
        assignMessage("SCADA topology is unavailable");
        return false;
    }
    if (stringValue(topology, "mode") != "upperComputer" ||
        !boolValue(topology, "requireFreshLeaseForControl", true)) {
        assignMessage("fresh upper-computer lease is not required");
        return true;
    }
    const auto nodes = readFile(projectDirectory + "/nodes.json");
    if (!containsMachineCode(nodes, machineCode)) {
        assignMessage("SCADA project does not contain this machineCode");
        return false;
    }
    const auto timeoutMs = int64Value(topology, "timeoutMs", 10000);
    const auto lease = readFile(leaseFile);
    const auto leaseMachineCode = stringValue(lease, "machineCode");
    const auto updatedAtMs = int64Value(lease, "updatedAtMs", 0);
    if (leaseMachineCode != machineCode || updatedAtMs <= 0 || nowMs < updatedAtMs ||
        nowMs - updatedAtMs > timeoutMs) {
        assignMessage("SCADA upper-computer control lease is missing or expired");
        return false;
    }
    assignMessage("SCADA upper-computer control lease is fresh");
    return true;
}

}  // namespace edge_gateway
