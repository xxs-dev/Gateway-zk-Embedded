#pragma once
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#ifdef __linux__
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include "edge_gateway/compute_process_identity.hpp"
#include "edge_gateway/compute_execution_observation.hpp"
#include "edge_gateway/compute_command_observation.hpp"
#include "edge_gateway/diagnostic_output_json.hpp"
#include "edge_gateway/json_value.hpp"

namespace edge_gateway {
inline std::string observeComputeGraphs(const std::string& healthPath, std::int64_t now) {
    // Only the configured main application's health file is accepted by the caller.
#ifdef __linux__
    struct File {
        int fd;
        ~File() { if (fd >= 0) close(fd); }
    } file{open(healthPath.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    struct stat info{};
    if (file.fd < 0 || fstat(file.fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size > 1024 * 1024)
        throw std::runtime_error("compute health file unavailable");
    std::string bytes;
    char buffer[4096];
    for (;;) {
        const auto count = read(file.fd, buffer, sizeof(buffer));
        if (count < 0) throw std::runtime_error("compute health file read failed");
        if (count == 0) break;
        bytes.append(buffer, static_cast<std::size_t>(count));
        if (bytes.size() > 1024 * 1024) throw std::runtime_error("compute health file too large");
    }
#else
    std::ifstream input(healthPath, std::ios::binary);
    std::string bytes(1024 * 1024 + 1, '\0');
    input.read(&bytes[0], bytes.size());
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    if (bytes.empty() || bytes.size() > 1024 * 1024 || input.bad())
        throw std::runtime_error("compute graph observation unavailable");
#endif
    // Execution settings add nested fields per graph. Keep the existing 1024
    // graph ceiling usable while retaining the 1 MiB file and depth limits.
    const auto root = json::JsonParser(bytes, 16, 65536).parse();
    auto field = [](const json::JsonValue& object, const std::string& key) -> const json::JsonValue& {
        const auto* value = object.find(key);
        if (!value) throw std::runtime_error("compute graph observation field missing");
        return *value;
    };
    const auto timestamp = field(root, "ts").asNumber();
    const auto identity = field(root, "processIdentity").asString();
    const auto& graphs = field(root, "loadedGraphs").asArray().values;
    if (field(root, "graphObservationVersion").asNumber() != 1 ||
        !std::isfinite(timestamp) || timestamp != std::floor(timestamp) || timestamp <= 0 || timestamp > now ||
        identity.size() > 128 || graphs.size() > 1024)
        throw std::runtime_error("invalid compute graph observation");
    const auto colon = identity.find(':');
    const auto second = colon == std::string::npos ? colon : identity.find(':', colon + 1);
    bool processPresent = false;
    if (colon == 36 && second != std::string::npos) {
        const auto pidText = identity.substr(colon + 1, second - colon - 1);
        if (!pidText.empty() && pidText.size() <= 10 && pidText.find_first_not_of("0123456789") == std::string::npos) {
            const auto pid = std::stoll(pidText);
            processPresent = pid > 0 && computeProcessIdentity(pid) == identity;
        }
    }
    std::ostringstream out;
    const auto* executionVersion = root.find("executionObservationVersion");
    if (executionVersion && executionVersion->asNumber() != 1)
        throw std::runtime_error("unsupported execution observation version");
    out << "{\"sampledAt\":" << static_cast<std::int64_t>(timestamp)
        << ",\"ageMs\":" << (now - static_cast<std::int64_t>(timestamp))
        << ",\"healthy\":" << (field(root, "healthy").asBool() ? "true" : "false")
        << ",\"processPresent\":" << (processPresent ? "true" : "false")
        << ",\"processIdentity\":\"" << escapeDiagnosticOutputJson(identity) << '"';
    if (executionVersion)
        out << ",\"executionObservationVersion\":1,\"execution\":"
            << compute_observation_detail::engine(field(root, "execution"));
    const auto* commandVersion = root.find("commandObservationVersion");
    if (commandVersion) {
        if (commandVersion->asNumber() != 1) throw std::runtime_error("unsupported command observation version");
        out << ",\"commandObservationVersion\":1";
    }
    std::size_t commandCount = 0;
    out << ",\"graphs\":[";
    std::set<std::string> rules;
    bool first = true;
    for (const auto& graph : graphs) {
        const auto rule = field(*graph, "ruleCode").asString();
        const auto path = field(*graph, "graphFile").asString();
        const auto sha = field(*graph, "sha256").asString();
        if (rule.empty() || rule.size() > 256 || path.empty() || path.size() > 4096 ||
            sha.size() != 64 || sha.find_first_not_of("0123456789abcdef") != std::string::npos || !rules.insert(rule).second)
            throw std::runtime_error("invalid loaded graph identity");
        if (!first) out << ',';
        first = false;
        out << "{\"ruleCode\":\"" << escapeDiagnosticOutputJson(rule) << "\",\"graphFile\":\""
            << escapeDiagnosticOutputJson(path) << "\",\"sha256\":\"" << sha << '"';
        if (executionVersion)
            out << ",\"execution\":" << compute_observation_detail::graph(field(*graph, "execution"));
        if (commandVersion)
            out << ",\"commands\":" << compute_observation_detail::commands(field(*graph, "commands"), static_cast<std::int64_t>(timestamp), commandCount);
        out << '}';
    }
    out << "]}";
    return out.str();
}
} // namespace edge_gateway
