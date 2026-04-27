#include "edge_gateway/system_monitor_service.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/statvfs.h>

namespace edge_gateway {

namespace {

class FlatJsonReader {
public:
    explicit FlatJsonReader(const std::string& text) : text_(text) {}

    bool tryGetString(const char* key, std::string* out) const {
        const auto pos = findKey(key);
        if (pos == std::string::npos) {
            return false;
        }
        auto cursor = skipWhitespace(pos);
        if (cursor >= text_.size() || text_[cursor] != '"') {
            return false;
        }
        ++cursor;
        std::string value;
        while (cursor < text_.size()) {
            const char ch = text_[cursor++];
            if (ch == '"') {
                *out = value;
                return true;
            }
            if (ch == '\\') {
                if (cursor >= text_.size()) {
                    return false;
                }
                value.push_back(text_[cursor++]);
            } else {
                value.push_back(ch);
            }
        }
        return false;
    }

    bool tryGetInt(const char* key, int* out) const {
        double value = 0.0;
        if (!tryGetDouble(key, &value)) {
            return false;
        }
        *out = static_cast<int>(value);
        return true;
    }

    bool tryGetInt64(const char* key, std::int64_t* out) const {
        double value = 0.0;
        if (!tryGetDouble(key, &value)) {
            return false;
        }
        *out = static_cast<std::int64_t>(value);
        return true;
    }

private:
    bool tryGetDouble(const char* key, double* out) const {
        const auto pos = findKey(key);
        if (pos == std::string::npos) {
            return false;
        }
        auto cursor = skipWhitespace(pos);
        const auto begin = cursor;
        if (cursor < text_.size() && (text_[cursor] == '-' || text_[cursor] == '+')) {
            ++cursor;
        }
        while (cursor < text_.size() && std::isdigit(static_cast<unsigned char>(text_[cursor])) != 0) {
            ++cursor;
        }
        if (cursor < text_.size() && text_[cursor] == '.') {
            ++cursor;
            while (cursor < text_.size() && std::isdigit(static_cast<unsigned char>(text_[cursor])) != 0) {
                ++cursor;
            }
        }
        if (begin == cursor) {
            return false;
        }
        *out = std::strtod(text_.c_str() + begin, nullptr);
        return true;
    }

    std::size_t findKey(const char* key) const {
        const std::string needle = std::string("\"") + key + "\"";
        auto keyPos = text_.find(needle);
        if (keyPos == std::string::npos) {
            return std::string::npos;
        }
        keyPos += needle.size();
        keyPos = skipWhitespace(keyPos);
        if (keyPos >= text_.size() || text_[keyPos] != ':') {
            return std::string::npos;
        }
        return keyPos + 1;
    }

    std::size_t skipWhitespace(std::size_t pos) const {
        while (pos < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos])) != 0) {
            ++pos;
        }
        return pos;
    }

    const std::string& text_;
};

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

void sleepInterruptibly(const std::atomic<bool>& running, int intervalMs) {
    int remaining = std::max(0, intervalMs);
    while (running.load() && remaining > 0) {
        const int slice = std::min(remaining, 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        remaining -= slice;
    }
}

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

int countProcesses() {
    int count = 0;
    DIR* dir = opendir("/proc");
    if (dir == nullptr) {
        return 0;
    }
    while (const auto* entry = readdir(dir)) {
        if (entry->d_name == nullptr || entry->d_name[0] == '\0') {
            continue;
        }
        bool numeric = true;
        for (const char* p = entry->d_name; *p != '\0'; ++p) {
            if (*p < '0' || *p > '9') {
                numeric = false;
                break;
            }
        }
        if (numeric) {
            ++count;
        }
    }
    closedir(dir);
    return count;
}

std::string readFileText(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open config file: " + path);
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::int64_t modifiedAtMs(const std::string& path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) {
        return 0;
    }
    return static_cast<std::int64_t>(st.st_mtime) * 1000;
}

std::string extractJsonStringField(const std::string& payload, const char* key) {
    FlatJsonReader json(payload);
    std::string value;
    json.tryGetString(key, &value);
    return value;
}

std::string toHex(const char* data, std::size_t size) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        const unsigned char value = static_cast<unsigned char>(data[i]);
        encoded.push_back(kHex[(value >> 4) & 0x0F]);
        encoded.push_back(kHex[value & 0x0F]);
    }
    return encoded;
}

}  // namespace

SystemMonitorService::SystemMonitorService(
    SystemMonitorConfig monitorConfig,
    MqttConfig mqttConfig,
    std::shared_ptr<IMqttDriverPublisher> publisher,
    std::string machineCode,
    std::vector<std::string> configFiles,
    PointStoreRouter* router
) : monitorConfig_(std::move(monitorConfig)),
    mqttConfig_(std::move(mqttConfig)),
    publisher_(std::move(publisher)),
    machineCode_(std::move(machineCode)),
    configFiles_(std::move(configFiles)),
    router_(router) {
    if (!publisher_) {
        throw std::invalid_argument("system monitor publisher is required");
    }
}

SystemMonitorService::~SystemMonitorService() {
    stop();
}

void SystemMonitorService::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    publishStatusEvent("started", currentTimeMs());
    thread_ = std::thread(&SystemMonitorService::loop, this);
}

void SystemMonitorService::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool SystemMonitorService::isRunning() const {
    return running_.load();
}

void SystemMonitorService::runOnce(std::int64_t nowMs) {
    processIncomingMessages(nowMs);
    const auto sample = collectSample();
    if (hasActiveLease(nowMs) && (lastTelemetryMs_ == 0 || nowMs - lastTelemetryMs_ >= effectiveIntervalMs(nowMs))) {
        publishTelemetry(sample, nowMs);
        lastTelemetryMs_ = nowMs;
    }
    evaluateAlerts(sample, nowMs);
}

void SystemMonitorService::loop() {
    while (running_.load()) {
        runOnce(currentTimeMs());
        sleepInterruptibly(running_, 500);
    }
    publishStatusEvent("stopped", currentTimeMs());
}

void SystemMonitorService::processIncomingMessages(std::int64_t nowMs) {
    std::vector<MqttIncomingMessage> messages;
    try {
        messages = publisher_->pollIncoming(100);
    } catch (const std::exception& ex) {
        publishStatusEvent("poll-failed", nowMs, std::string(R"("message":")") + escapeJson(ex.what()) + R"(")");
        return;
    }
    for (const auto& message : messages) {
        try {
            if (message.type == MqttIncomingType::SystemMonitorRequest) {
                handleMonitorRequest(message.payload, nowMs);
            } else if (message.type == MqttIncomingType::DiagRequest) {
                handleDiagRequest(message.payload, nowMs);
            } else if (message.type == MqttIncomingType::ConfigPullRequest) {
                handleConfigPullRequest(message.payload, nowMs);
            }
        } catch (const std::exception& ex) {
            publishStatusEvent("request-failed", nowMs, std::string(R"("message":")") + escapeJson(ex.what()) + R"(")");
        }
    }
}

void SystemMonitorService::handleMonitorRequest(const std::string& payload, std::int64_t nowMs) {
    FlatJsonReader json(payload);
    std::string action;
    std::string sessionId;
    std::string machineCode;
    int intervalMs = monitorConfig_.defaultIntervalMs;
    int ttlSec = monitorConfig_.subscriptionTtlSec;
    json.tryGetString("action", &action);
    json.tryGetString("sessionId", &sessionId);
    json.tryGetString("machineCode", &machineCode);
    json.tryGetInt("intervalMs", &intervalMs);
    json.tryGetInt("ttlSec", &ttlSec);
    if (machineCode.empty()) {
        throw std::runtime_error("machineCode is required");
    }
    if (machineCode != machineCode_) {
        throw std::runtime_error("machineCode mismatch");
    }
    if (sessionId.empty()) {
        sessionId = "system-monitor";
    }
    if (action == "unsubscribe") {
        leases_.erase(sessionId);
    } else {
        MonitorLease lease;
        lease.sessionId = sessionId;
        lease.intervalMs = std::max(monitorConfig_.minIntervalMs, intervalMs);
        lease.expireAtMs = nowMs + static_cast<std::int64_t>(std::max(1, ttlSec)) * 1000;
        leases_[sessionId] = lease;
        publishPointSnapshot(nowMs);
    }
    publishStatusEvent(
        "monitor-request",
        nowMs,
        std::string(R"("sessionId":")") + escapeJson(sessionId) +
            R"(","action":")" + escapeJson(action.empty() ? "subscribe" : action) +
            R"(")"
    );
    std::ostringstream reply;
    reply << "{\"machineCode\":\"" << escapeJson(machineCode_) << "\",\"sessionId\":\"" << escapeJson(sessionId)
          << "\",\"action\":\"" << escapeJson(action.empty() ? "subscribe" : action)
          << "\",\"accepted\":true,\"ts\":" << nowMs << "}";
    publishReply(reply.str());
}

void SystemMonitorService::handleDiagRequest(const std::string& payload, std::int64_t nowMs) {
    if (!monitorConfig_.diagEnabled) {
        throw std::runtime_error("diag disabled");
    }
    FlatJsonReader json(payload);
    std::string cmdId;
    std::string command;
    std::string machineCode;
    std::string operatorName;
    std::string service;
    int tailLines = 100;
    std::int64_t requestTs = nowMs;
    json.tryGetString("cmdId", &cmdId);
    json.tryGetString("command", &command);
    json.tryGetString("machineCode", &machineCode);
    json.tryGetString("operator", &operatorName);
    json.tryGetString("service", &service);
    json.tryGetInt("tailLines", &tailLines);
    json.tryGetInt64("requestTs", &requestTs);
    if (machineCode.empty()) {
        throw std::runtime_error("machineCode is required");
    }
    if (machineCode != machineCode_) {
        throw std::runtime_error("machineCode mismatch");
    }
    if (cmdId.empty()) {
        cmdId = "DIAG_" + std::to_string(nowMs);
    }
    publishStatusEvent(
        "diag-request",
        nowMs,
        std::string(R"("cmdId":")") + escapeJson(cmdId) +
            R"(","command":")" + escapeJson(command) +
            R"(")"
    );
    if (!isCommandAllowed(command)) {
        throw std::runtime_error("diag command not allowed");
    }
    int exitCode = 0;
    std::string arg;
    if (command == "journal_tail") {
        arg = std::to_string(std::max(1, std::min(500, tailLines)));
    } else if (command == "systemctl_status") {
        arg = service;
    }
    const auto stdoutText = executeDiagCommand(command, arg, &exitCode);
    std::ostringstream reply;
    reply << "{\"cmdId\":\"" << escapeJson(cmdId)
          << "\",\"machineCode\":\"" << escapeJson(machineCode)
          << "\",\"operator\":\"" << escapeJson(operatorName)
          << "\",\"command\":\"" << escapeJson(command)
          << "\",\"success\":" << (exitCode == 0 ? "true" : "false")
          << ",\"exitCode\":" << exitCode
          << ",\"stdout\":\"" << escapeJson(stdoutText)
          << "\",\"stderr\":\"\""
          << ",\"requestTs\":" << requestTs
          << ",\"ts\":" << nowMs
          << "}";
    publishReply(reply.str());
    publishStatusEvent(
        "diag-replied",
        nowMs,
        std::string(R"("cmdId":")") + escapeJson(cmdId) +
            R"(","exitCode":)" + std::to_string(exitCode)
    );
}

void SystemMonitorService::handleConfigPullRequest(const std::string& payload, std::int64_t nowMs) {
    FlatJsonReader json(payload);
    std::string requestId;
    std::string machineCode;
    json.tryGetString("requestId", &requestId);
    json.tryGetString("machineCode", &machineCode);
    if (machineCode.empty()) {
        throw std::runtime_error("machineCode is required");
    }
    if (machineCode != machineCode_) {
        throw std::runtime_error("machineCode mismatch");
    }
    if (requestId.empty()) {
        requestId = "CFG_PULL_" + std::to_string(nowMs);
    }

    std::ostringstream reply;
    reply << "{\"requestId\":\"" << escapeJson(requestId)
          << "\",\"machineCode\":\"" << escapeJson(machineCode_)
          << "\",\"success\":true,\"files\":[";
    for (std::size_t i = 0; i < configFiles_.size(); ++i) {
        const auto& path = configFiles_[i];
        const auto content = readFileText(path);
        if (i > 0) {
            reply << ",";
        }
        reply << "{\"path\":\"" << escapeJson(path)
              << "\",\"sizeBytes\":" << static_cast<long long>(content.size())
              << ",\"modifiedAtMs\":" << static_cast<long long>(modifiedAtMs(path))
              << ",\"content\":\"" << escapeJson(content) << "\"}";
    }
    reply << "],\"ts\":" << nowMs << "}";
    publishConfigPullReply(reply.str());
    publishStatusEvent(
        "config-pull-replied",
        nowMs,
        std::string(R"("requestId":")") + escapeJson(requestId) +
            R"(","fileCount":)" + std::to_string(configFiles_.size())
    );
}

SystemMonitorService::Sample SystemMonitorService::collectSample() const {
    Sample sample;

    std::ifstream stat("/proc/stat");
    std::string cpuLabel;
    std::uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    if (stat >> cpuLabel >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) {
        const auto total = user + nice + system + idle + iowait + irq + softirq + steal;
        const auto idleTotal = idle + iowait;
        if (cpuBaselineReady_ && total > lastCpuTotal_) {
            const auto totalDiff = total - lastCpuTotal_;
            const auto idleDiff = idleTotal - lastCpuIdle_;
            if (totalDiff > 0) {
                sample.cpuUsage = 100.0 * static_cast<double>(totalDiff - idleDiff) / static_cast<double>(totalDiff);
            }
        }
        const_cast<SystemMonitorService*>(this)->lastCpuTotal_ = total;
        const_cast<SystemMonitorService*>(this)->lastCpuIdle_ = idleTotal;
        const_cast<SystemMonitorService*>(this)->cpuBaselineReady_ = true;
    }

    std::ifstream meminfo("/proc/meminfo");
    std::string key;
    std::uint64_t totalMem = 0, availMem = 0;
    while (meminfo >> key) {
        std::uint64_t value = 0;
        meminfo >> value;
        if (key == "MemTotal:") {
            totalMem = value;
        } else if (key == "MemAvailable:") {
            availMem = value;
        }
        meminfo.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    if (totalMem > 0 && availMem <= totalMem) {
        sample.memUsage = 100.0 * static_cast<double>(totalMem - availMem) / static_cast<double>(totalMem);
    }

    struct statvfs vfs {};
    if (statvfs("/", &vfs) == 0 && vfs.f_blocks > 0) {
        const auto used = vfs.f_blocks - vfs.f_bavail;
        sample.diskUsage = 100.0 * static_cast<double>(used) / static_cast<double>(vfs.f_blocks);
    }

    std::ifstream loadavg("/proc/loadavg");
    if (loadavg) {
        loadavg >> sample.load1;
    }
    sample.processCount = countProcesses();
    return sample;
}

void SystemMonitorService::publishTelemetry(const Sample& sample, std::int64_t nowMs) {
    if (mqttConfig_.systemMonitorTelemetryTopic.empty()) {
        return;
    }
    std::ostringstream payload;
    payload << "{\"type\":\"system-monitor\",\"machineCode\":\"" << escapeJson(machineCode_) << "\""
            << ",\"cpuUsage\":" << sample.cpuUsage
            << ",\"memUsage\":" << sample.memUsage
            << ",\"diskUsage\":" << sample.diskUsage
            << ",\"load1\":" << sample.load1
            << ",\"processCount\":" << sample.processCount
            << ",\"ts\":" << nowMs
            << "}";
    publisher_->publishJsonMessage(mqttConfig_.systemMonitorTelemetryTopic, payload.str());
}

void SystemMonitorService::evaluateAlerts(const Sample& sample, std::int64_t nowMs) {
    if (sample.cpuUsage >= monitorConfig_.cpuAlertThreshold) {
        publishAlert("cpuUsage", sample.cpuUsage, monitorConfig_.cpuAlertThreshold, true, nowMs, "cpu usage too high");
    }
    if (sample.memUsage >= monitorConfig_.memAlertThreshold) {
        publishAlert("memUsage", sample.memUsage, monitorConfig_.memAlertThreshold, true, nowMs, "memory usage too high");
    }
    if (sample.diskUsage >= monitorConfig_.diskAlertThreshold) {
        publishAlert("diskUsage", sample.diskUsage, monitorConfig_.diskAlertThreshold, true, nowMs, "disk usage too high");
    }
}

void SystemMonitorService::publishAlert(
    const std::string& metric,
    double value,
    double threshold,
    bool active,
    std::int64_t nowMs,
    const std::string& message
) {
    if (mqttConfig_.systemMonitorAlertTopic.empty()) {
        return;
    }
    const auto it = lastAlertPublishMs_.find(metric);
    const auto minInterval = static_cast<std::int64_t>(std::max(1, monitorConfig_.alertRepeatIntervalSec)) * 1000;
    if (it != lastAlertPublishMs_.end() && nowMs - it->second < minInterval) {
        return;
    }
    lastAlertPublishMs_[metric] = nowMs;
    std::ostringstream payload;
    payload << "{\"type\":\"system-alert\",\"machineCode\":\"" << escapeJson(machineCode_) << "\""
            << ",\"metric\":\"" << escapeJson(metric) << "\""
            << ",\"level\":\"critical\""
            << ",\"value\":" << value
            << ",\"threshold\":" << threshold
            << ",\"active\":" << (active ? "true" : "false")
            << ",\"message\":\"" << escapeJson(message) << "\""
            << ",\"ts\":" << nowMs
            << "}";
    publisher_->publishJsonMessage(mqttConfig_.systemMonitorAlertTopic, payload.str());
}

void SystemMonitorService::publishReply(const std::string& payload) {
    if (payload.find("\"cmdId\"") != std::string::npos) {
        publisher_->publishJsonMessage(mqttConfig_.diagReplyTopic, payload);
    } else {
        publisher_->publishJsonMessage(mqttConfig_.systemMonitorReplyTopic, payload);
    }
}

void SystemMonitorService::publishConfigPullReply(const std::string& payload) const {
    static const std::size_t kConfigPullChunkBytes = 16 * 1024;
    if (payload.size() <= kConfigPullChunkBytes) {
        publisher_->publishJsonMessage(mqttConfig_.configPullReplyTopic, payload);
        return;
    }

    const std::string requestId = extractJsonStringField(payload, "requestId");
    const std::size_t chunkCount = (payload.size() + kConfigPullChunkBytes - 1) / kConfigPullChunkBytes;
    for (std::size_t i = 0; i < chunkCount; ++i) {
        const std::size_t begin = i * kConfigPullChunkBytes;
        const std::size_t partSize = std::min(kConfigPullChunkBytes, payload.size() - begin);
        const std::string partHex = toHex(payload.data() + begin, partSize);
        std::ostringstream chunk;
        chunk << "{\"requestId\":\"" << escapeJson(requestId)
              << "\",\"machineCode\":\"" << escapeJson(machineCode_)
              << "\",\"success\":true"
              << ",\"chunked\":true"
              << ",\"chunkIndex\":" << static_cast<long long>(i + 1)
              << ",\"chunkCount\":" << static_cast<long long>(chunkCount)
              << ",\"totalBytes\":" << static_cast<long long>(payload.size())
              << ",\"payloadHex\":\"" << partHex << "\""
              << ",\"ts\":" << currentTimeMs()
              << "}";
        publisher_->publishJsonMessage(mqttConfig_.configPullReplyTopic, chunk.str());
    }
}

void SystemMonitorService::publishPointSnapshot(std::int64_t nowMs) {
    if (router_ == nullptr || mqttConfig_.systemMonitorPointTopic.empty()) {
        return;
    }
    const auto values = router_->getAllLatest(nowMs);
    if (values.empty()) {
        return;
    }
    std::map<std::string, std::vector<std::string>> grouped;
    for (const auto& value : values) {
        std::ostringstream item;
        item << "{\"index\":" << value.index
             << ",\"pointCode\":\"" << escapeJson(value.pointCode) << "\""
             << ",\"value\":" << value.value
             << ",\"quality\":" << value.quality
             << ",\"ts\":" << value.ts
             << ",\"stale\":" << (value.stale ? "true" : "false")
             << "}";
        grouped[value.meterCode].push_back(item.str());
    }
    std::ostringstream payload;
    payload << "{\"type\":\"snapshot\",\"machineCode\":\"" << escapeJson(machineCode_) << "\",\"meters\":[";
    bool firstMeter = true;
    for (const auto& entry : grouped) {
        if (!firstMeter) {
            payload << ",";
        }
        firstMeter = false;
        payload << "{\"meterCode\":\"" << escapeJson(entry.first) << "\",\"values\":[";
        for (std::size_t i = 0; i < entry.second.size(); ++i) {
            if (i > 0) {
                payload << ",";
            }
            payload << entry.second[i];
        }
        payload << "]}";
    }
    payload << "]}";
    publisher_->publishJsonMessage(mqttConfig_.systemMonitorPointTopic, payload.str());
}

void SystemMonitorService::publishStatusEvent(const std::string& event, std::int64_t ts, const std::string& detailsJson) const {
    if (mqttConfig_.statusTopic.empty()) {
        return;
    }
    std::ostringstream payload;
    payload << "{\"service\":\"system-monitor\",\"event\":\"" << event << "\",\"ts\":" << ts;
    if (!detailsJson.empty()) {
        payload << "," << detailsJson;
    }
    payload << "}";
    publisher_->publishJsonMessage(mqttConfig_.statusTopic, payload.str());
}

bool SystemMonitorService::hasActiveLease(std::int64_t nowMs) const {
    for (const auto& entry : leases_) {
        if (entry.second.expireAtMs > nowMs) {
            return true;
        }
    }
    return false;
}

int SystemMonitorService::effectiveIntervalMs(std::int64_t nowMs) const {
    int interval = monitorConfig_.defaultIntervalMs;
    for (const auto& entry : leases_) {
        if (entry.second.expireAtMs <= nowMs) {
            continue;
        }
        interval = std::min(interval, entry.second.intervalMs);
    }
    return std::max(monitorConfig_.minIntervalMs, interval);
}

bool SystemMonitorService::isCommandAllowed(const std::string& command) const {
    return std::find(
        monitorConfig_.allowedCommands.begin(),
        monitorConfig_.allowedCommands.end(),
        command
    ) != monitorConfig_.allowedCommands.end();
}

std::string SystemMonitorService::executeDiagCommand(const std::string& command, const std::string& arg, int* exitCode) const {
    std::string shellCommand;
    if (command == "uptime") {
        shellCommand = "uptime 2>&1";
    } else if (command == "free_m") {
        shellCommand = "free -m 2>&1";
    } else if (command == "df_root") {
        shellCommand = "df -h / 2>&1";
    } else if (command == "top_once") {
        shellCommand = "top -b -n 1 2>&1";
    } else if (command == "ps_gateway") {
        shellCommand = "ps | grep -E 'ModbusRtu|Dlt645Driver|MqttDriver|EventEngine|SystemMonitor' 2>&1";
    } else if (command == "journal_tail") {
        shellCommand = "journalctl -n " + arg + " 2>&1";
    } else if (command == "systemctl_status") {
        shellCommand = "systemctl status " + arg + " 2>&1";
    } else {
        throw std::runtime_error("unsupported diag command");
    }

    std::string output;
    FILE* pipe = popen(shellCommand.c_str(), "r");
    if (pipe == nullptr) {
        throw std::runtime_error("popen failed");
    }
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    const int rc = pclose(pipe);
    if (exitCode != nullptr) {
        *exitCode = rc;
    }
    return output;
}

}  // namespace edge_gateway
