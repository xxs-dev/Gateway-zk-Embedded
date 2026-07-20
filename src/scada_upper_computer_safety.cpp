#include "edge_gateway/scada_upper_computer_safety.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "edge_gateway/scada_project_loader.hpp"
#include "edge_gateway/scada_runtime_map.hpp"

namespace edge_gateway {

namespace {

std::string jsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const auto ch : value) {
        if (ch == '"' || ch == '\\') out.push_back('\\');
        if (ch == '\n') {
            out += "\\n";
        } else if (ch != '\r') {
            out.push_back(ch);
        }
    }
    return out;
}

void ensureParentDirectory(const std::string& path) {
#ifndef _WIN32
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return;
    const auto directory = path.substr(0, slash);
    std::string current;
    for (const auto ch : directory) {
        current.push_back(ch);
        if (ch == '/' && current.size() > 1) (void)::mkdir(current.c_str(), 0755);
    }
    if (!current.empty()) (void)::mkdir(current.c_str(), 0755);
#else
    (void)path;
#endif
}

std::string readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string jsonString(const std::string& text, const std::string& key) {
    const auto marker = std::string("\"") + key + "\"";
    auto pos = text.find(marker);
    if (pos == std::string::npos) return {};
    pos = text.find(':', pos + marker.size());
    if (pos == std::string::npos) return {};
    pos = text.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    const auto end = text.find('"', pos + 1);
    return end == std::string::npos ? std::string() : text.substr(pos + 1, end - pos - 1);
}

std::int64_t jsonInt64(const std::string& text, const std::string& key) {
    const auto marker = std::string("\"") + key + "\"";
    auto pos = text.find(marker);
    if (pos == std::string::npos) return 0;
    pos = text.find(':', pos + marker.size());
    if (pos == std::string::npos) return 0;
    char* end = nullptr;
    const auto value = std::strtoll(text.c_str() + pos + 1, &end, 10);
    return end == text.c_str() + pos + 1 ? 0 : value;
}

}  // namespace

ScadaUpperComputerSafetyMonitor::ScadaUpperComputerSafetyMonitor(
    SystemMonitorConfig::ScadaUpperComputerSafetyConfig config,
    std::string machineCode,
    PointStoreRouter& router
) : config_(std::move(config)),
    machineCode_(std::move(machineCode)),
    router_(router),
    priorityControlLease_(config_.priorityControlLeaseFile, "scada-offline-safety") {
}

void ScadaUpperComputerSafetyMonitor::writeHeartbeat(
    const std::string& leaseFile,
    const std::string& machineCode,
    std::int64_t nowMs
) {
    if (leaseFile.empty() || machineCode.empty() || nowMs <= 0) return;
    ensureParentDirectory(leaseFile);
    std::ostringstream threadId;
    threadId << std::this_thread::get_id();
#ifndef _WIN32
    const auto processId = static_cast<long>(::getpid());
#else
    const auto processId = 0L;
#endif
    const auto temporary = leaseFile + ".tmp." + std::to_string(processId) + "." + threadId.str();
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot write SCADA upper-computer lease");
        output << "{\"machineCode\":\"" << jsonEscape(machineCode)
               << "\",\"updatedAtMs\":" << nowMs << "}";
        output.flush();
        if (!output) throw std::runtime_error("cannot flush SCADA upper-computer lease");
    }
    if (std::rename(temporary.c_str(), leaseFile.c_str()) != 0) {
        std::remove(temporary.c_str());
        throw std::runtime_error("cannot activate SCADA upper-computer lease");
    }
}

ScadaSafetyEvaluation ScadaUpperComputerSafetyMonitor::runOnce(std::int64_t nowMs) {
    ScadaSafetyEvaluation result;
    if (!config_.enabled || machineCode_.empty()) {
        result.message = "SCADA upper-computer safety monitor is disabled";
        return result;
    }
    try {
        reloadProject(nowMs);
    } catch (const std::exception& ex) {
        result.message = ex.what();
        return result;
    }
    if (!project_ || project_->topology.mode != ScadaDeploymentMode::UpperComputer) {
        result.message = "active SCADA project is not upper-computer mode";
        return result;
    }

    const auto node = std::find_if(project_->nodes.begin(), project_->nodes.end(), [&](const ScadaNode& item) {
        return item.machineCode == machineCode_;
    });
    if (node == project_->nodes.end()) {
        result.message = "active SCADA project does not contain this machineCode";
        return result;
    }
    std::vector<ScadaOfflineSafetyAction> actions;
    for (const auto& action : project_->topology.safetyActions) {
        if (action.nodeId == node->nodeId) actions.push_back(action);
    }
    if (actions.empty()) {
        result.message = "active edge node has no SCADA offline safety action";
        return result;
    }

    result.active = true;
    const auto heartbeat = readHeartbeat();
    const auto effectiveHeartbeat = std::max(heartbeat, activatedAtMs_);
    result.heartbeatFresh = effectiveHeartbeat > 0 && nowMs >= effectiveHeartbeat &&
        nowMs - effectiveHeartbeat <= project_->topology.offlineTimeoutMs;
    if (result.heartbeatFresh) {
        if (heartbeat >= activatedAtMs_) {
            completed_ = false;
            acceptedActions_.clear();
        }
        result.message = "upper-computer heartbeat is fresh";
        return result;
    }
    if (completed_) {
        result.completed = true;
        result.message = "offline safety actions already submitted";
        return result;
    }

    ScadaRuntimeMap runtime(*project_, machineCode_, router_);
    for (const auto& action : actions) {
        if (acceptedActions_.count(action.actionId) != 0) continue;
        const auto resolved = runtime.resolver().resolveTag(action.tagId);
        if (!resolved) {
            result.actions.push_back({action.actionId, action.tagId, false, "SCADA tag not found"});
            result.triggered = true;
            break;
        }
        PendingWriteCommand command;
        command.cmdId = "SCADA_OFFLINE_" + action.actionId + "_" + std::to_string(nowMs);
        command.value = action.value;
        command.source = "scada-offline-safety";
        command.ts = nowMs;
        command.acceptedAt = nowMs;
        command.highPriority = action.highPriority;
        if (command.highPriority) {
            const auto activeLease = priorityControlLease_.activeLease(nowMs);
            if (activeLease && activeLease->owner == "scada-offline-safety") {
                result.message = "previous offline safety action is still in progress";
                return result;
            }
            priorityControlLease_.acquire(
                command.cmdId,
                resolved->tag.meterCode,
                resolved->mapping.index,
                nowMs,
                config_.priorityControlLeaseTtlMs
            );
        }
        const auto submitted = runtime.submitWrite(action.tagId, command);
        result.actions.push_back({action.actionId, action.tagId, submitted.accepted, submitted.message});
        if (submitted.accepted) {
            acceptedActions_.insert(action.actionId);
        } else if (command.highPriority) {
            priorityControlLease_.release(command.cmdId);
        }
        break;
    }
    result.triggered = !result.actions.empty();
    completed_ = acceptedActions_.size() == actions.size();
    result.completed = completed_;
    result.message = completed_
        ? "offline safety actions submitted"
        : "one or more offline safety actions were rejected";
    return result;
}

void ScadaUpperComputerSafetyMonitor::reloadProject(std::int64_t nowMs) {
    if (lastReloadAtMs_ != 0 && nowMs - lastReloadAtMs_ < config_.reloadIntervalMs) return;
    lastReloadAtMs_ = nowMs;
    const auto loaded = ScadaProjectLoader::loadFromDirectory(config_.projectDirectory);
    const auto nextSignature = projectSignature(loaded);
    if (nextSignature != signature_) {
        project_.reset(new ScadaProject(loaded));
        signature_ = nextSignature;
        activatedAtMs_ = nowMs;
        completed_ = false;
        acceptedActions_.clear();
    } else if (!project_) {
        project_.reset(new ScadaProject(loaded));
    }
}

std::int64_t ScadaUpperComputerSafetyMonitor::readHeartbeat() const {
    const auto text = readFile(config_.leaseFile);
    if (text.empty() || jsonString(text, "machineCode") != machineCode_) return 0;
    return jsonInt64(text, "updatedAtMs");
}

std::string ScadaUpperComputerSafetyMonitor::projectSignature(const ScadaProject& project) const {
    std::ostringstream value;
    value << project.manifest.projectId << '\x1f' << project.manifest.packageVersion
          << '\x1f' << project.topology.offlineTimeoutMs;
    for (const auto& action : project.topology.safetyActions) {
        value << '\x1f' << action.actionId << ':' << action.nodeId << ':' << action.tagId
              << ':' << action.value << ':' << action.highPriority;
    }
    return value.str();
}

}  // namespace edge_gateway
