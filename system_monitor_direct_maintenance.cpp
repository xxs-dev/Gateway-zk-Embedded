#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/system_monitor_direct_maintenance.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/ota_service.hpp"
#include "edge_gateway/point_store_router.hpp"
#include "edge_gateway/priority_control_lease.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

std::atomic<bool> g_running{true};

constexpr const char* kFixedMaintenancePasswordSha256 =
    "5c2358ee05dbd6bc6d52939f51a45c315533ad9191eecf1631fd788ec8ab76b3";
constexpr std::size_t kMaxConfigSnapshotFiles = 256;
constexpr std::size_t kMaxConfigSnapshotFileBytes = 5 * 1024 * 1024;
constexpr std::size_t kMaxConfigSnapshotTotalBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxConfigSnapshotReplyBytes = 24 * 1024 * 1024;
constexpr std::size_t kMaxConfigApplyFiles = 64;
constexpr std::size_t kMaxBatchControlCommands = 128;
constexpr std::size_t kMaxDiagOutputBytes = 64 * 1024;
constexpr std::size_t kMaxHttpRequestBytes = 24 * 1024 * 1024;

void handleSignal(int) {
    g_running = false;
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::string readFile(const std::string& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string dirnameOf(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
}

void addUniquePath(std::vector<std::string>& paths, const std::string& path) {
    if (path.empty()) {
        return;
    }
    if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
        paths.push_back(path);
    }
}

std::vector<std::string> discoverSiblingAppConfigFiles(const std::string& appConfigPath) {
    std::vector<std::string> files;
#ifndef _WIN32
    const auto dir = dirnameOf(appConfigPath);
    DIR* handle = opendir(dir.c_str());
    if (handle == nullptr) {
        return files;
    }
    while (dirent* entry = readdir(handle)) {
        const std::string name = entry->d_name;
        if (name.size() < 6 || name.substr(name.size() - 5) != ".json") {
            continue;
        }
        files.push_back(dir + "/" + name);
    }
    closedir(handle);
    std::sort(files.begin(), files.end());
#else
    (void)appConfigPath;
#endif
    return files;
}

bool isRegularFile(const std::string& path) {
#ifndef _WIN32
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#else
    (void)path;
    return false;
#endif
}

std::uint64_t fileSizeBytes(const std::string& path) {
#ifndef _WIN32
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0 || st.st_size < 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(st.st_size);
#else
    (void)path;
    return 0;
#endif
}

std::int64_t modifiedAtMs(const std::string& path) {
#ifndef _WIN32
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        return 0;
    }
    return static_cast<std::int64_t>(st.st_mtime) * 1000;
#else
    (void)path;
    return 0;
#endif
}

std::string jsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char ch : value) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                out += "?";
            } else {
                out += ch;
            }
            break;
        }
    }
    return out;
}

std::string trim(std::string value) {
    auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char ch) {
        return !isSpace(static_cast<unsigned char>(ch));
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](char ch) {
        return !isSpace(static_cast<unsigned char>(ch));
    }).base(), value.end());
    return value;
}

std::string urlDecode(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto ch = value[i];
        if (ch == '+') {
            out += ' ';
            continue;
        }
        if (ch == '%' && i + 2 < value.size()) {
            const auto hex = value.substr(i + 1, 2);
            char* end = nullptr;
            const auto decoded = std::strtoul(hex.c_str(), &end, 16);
            if (end != hex.c_str() + 2) {
                out += ch;
                continue;
            }
            out += static_cast<char>(decoded);
            i += 2;
            continue;
        }
        out += ch;
    }
    return out;
}

std::string queryValue(const std::string& query, const std::string& name) {
    std::size_t pos = 0;
    while (pos < query.size()) {
        const auto amp = query.find('&', pos);
        const auto end = amp == std::string::npos ? query.size() : amp;
        const auto eq = query.find('=', pos);
        const auto keyEnd = eq == std::string::npos || eq > end ? end : eq;
        const auto key = urlDecode(query.substr(pos, keyEnd - pos));
        if (key == name) {
            if (eq == std::string::npos || eq > end) {
                return "";
            }
            return trim(urlDecode(query.substr(eq + 1, end - eq - 1)));
        }
        if (amp == std::string::npos) {
            break;
        }
        pos = amp + 1;
    }
    return "";
}

int hexDigit(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

void appendUtf8Codepoint(std::string& out, unsigned int codepoint) {
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

bool parseJsonUnicodeEscape(const std::string& text, std::size_t* pos, unsigned int* codepoint) {
    if (*pos + 4 > text.size()) {
        return false;
    }
    unsigned int value = 0;
    for (int i = 0; i < 4; ++i) {
        const int digit = hexDigit(text[*pos + i]);
        if (digit < 0) {
            return false;
        }
        value = (value << 4) | static_cast<unsigned int>(digit);
    }
    *pos += 4;
    *codepoint = value;
    return true;
}

std::size_t findJsonValue(const std::string& text, const std::string& key) {
    const auto keyPattern = "\"" + key + "\"";
    const auto keyPos = text.find(keyPattern);
    if (keyPos == std::string::npos) {
        return std::string::npos;
    }
    const auto colonPos = text.find(':', keyPos + keyPattern.size());
    if (colonPos == std::string::npos) {
        return std::string::npos;
    }
    auto valuePos = colonPos + 1;
    while (valuePos < text.size() && std::isspace(static_cast<unsigned char>(text[valuePos])) != 0) {
        ++valuePos;
    }
    return valuePos;
}

std::string parseJsonStringAt(const std::string& text, std::size_t pos) {
    if (pos == std::string::npos || pos >= text.size() || text[pos] != '"') {
        return "";
    }
    ++pos;
    std::string out;
    while (pos < text.size()) {
        const char ch = text[pos++];
        if (ch == '"') {
            return out;
        }
        if (ch == '\\' && pos < text.size()) {
            const char esc = text[pos++];
            switch (esc) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned int codepoint = 0;
                if (!parseJsonUnicodeEscape(text, &pos, &codepoint)) {
                    out += 'u';
                    break;
                }
                if (codepoint >= 0xD800 && codepoint <= 0xDBFF &&
                    pos + 6 <= text.size() &&
                    text[pos] == '\\' &&
                    text[pos + 1] == 'u') {
                    pos += 2;
                    unsigned int low = 0;
                    if (parseJsonUnicodeEscape(text, &pos, &low) &&
                        low >= 0xDC00 &&
                        low <= 0xDFFF) {
                        codepoint = 0x10000 + (((codepoint - 0xD800) << 10) | (low - 0xDC00));
                    }
                }
                appendUtf8Codepoint(out, codepoint);
                break;
            }
            default: out += esc; break;
            }
        } else {
            out += ch;
        }
    }
    return out;
}

std::string jsonString(const std::string& text, const std::string& key, const std::string& fallback = "") {
    const auto pos = findJsonValue(text, key);
    const auto value = parseJsonStringAt(text, pos);
    return value.empty() ? fallback : value;
}

bool jsonBool(const std::string& text, const std::string& key, bool fallback) {
    const auto pos = findJsonValue(text, key);
    if (pos == std::string::npos) {
        return fallback;
    }
    if (text.compare(pos, 4, "true") == 0) {
        return true;
    }
    if (text.compare(pos, 5, "false") == 0) {
        return false;
    }
    return fallback;
}

int jsonInt(const std::string& text, const std::string& key, int fallback) {
    const auto pos = findJsonValue(text, key);
    if (pos == std::string::npos) {
        return fallback;
    }
    char* end = nullptr;
    const long value = std::strtol(text.c_str() + pos, &end, 10);
    if (end == text.c_str() + pos) {
        return fallback;
    }
    return static_cast<int>(value);
}

bool tryJsonDouble(const std::string& text, const std::string& key, double* output) {
    const auto pos = findJsonValue(text, key);
    if (pos == std::string::npos) {
        return false;
    }
    char* end = nullptr;
    const auto value = std::strtod(text.c_str() + pos, &end);
    if (end == text.c_str() + pos) {
        return false;
    }
    if (output != nullptr) {
        *output = value;
    }
    return true;
}

std::uint64_t jsonUint64(const std::string& text, const std::string& key, std::uint64_t fallback = 0) {
    const auto pos = findJsonValue(text, key);
    if (pos == std::string::npos) {
        return fallback;
    }
    char* end = nullptr;
    const auto value = std::strtoull(text.c_str() + pos, &end, 10);
    if (end == text.c_str() + pos) {
        return fallback;
    }
    return static_cast<std::uint64_t>(value);
}

std::vector<std::string> jsonStringArray(const std::string& text, const std::string& key) {
    std::vector<std::string> values;
    auto pos = findJsonValue(text, key);
    if (pos == std::string::npos || pos >= text.size() || text[pos] != '[') {
        return values;
    }
    ++pos;
    while (pos < text.size()) {
        while (pos < text.size() &&
            (std::isspace(static_cast<unsigned char>(text[pos])) != 0 || text[pos] == ',')) {
            ++pos;
        }
        if (pos >= text.size() || text[pos] == ']') {
            break;
        }
        if (text[pos] != '"') {
            break;
        }
        ++pos;
        std::string value;
        while (pos < text.size()) {
            const char ch = text[pos++];
            if (ch == '"') {
                break;
            }
            if (ch == '\\' && pos < text.size()) {
                const char esc = text[pos++];
                switch (esc) {
                case '"': value += '"'; break;
                case '\\': value += '\\'; break;
                case '/': value += '/'; break;
                case 'b': value += '\b'; break;
                case 'f': value += '\f'; break;
                case 'n': value += '\n'; break;
                case 'r': value += '\r'; break;
                case 't': value += '\t'; break;
                case 'u': {
                    unsigned int codepoint = 0;
                    if (parseJsonUnicodeEscape(text, &pos, &codepoint)) {
                        appendUtf8Codepoint(value, codepoint);
                    } else {
                        value += 'u';
                    }
                    break;
                }
                default: value += esc; break;
                }
            } else {
                value += ch;
            }
        }
        value = trim(value);
        if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end()) {
            values.push_back(value);
        }
    }
    return values;
}

std::string joinStrings(const std::vector<std::string>& values, const std::string& separator) {
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            out << separator;
        }
        out << values[i];
    }
    return out.str();
}

std::int64_t nonNegativeDuration(std::int64_t endMs, std::int64_t startMs) {
    if (endMs <= 0 || startMs <= 0 || endMs < startMs) {
        return 0;
    }
    return endMs - startMs;
}

std::vector<std::string> normalizeStringList(const std::vector<std::string>& values) {
    std::vector<std::string> normalized;
    for (auto value : values) {
        value = trim(value);
        if (!value.empty() && std::find(normalized.begin(), normalized.end(), value) == normalized.end()) {
            normalized.push_back(value);
        }
    }
    return normalized;
}

bool constantTimeEquals(const std::string& a, const std::string& b) {
    const std::size_t maxSize = std::max(a.size(), b.size());
    unsigned char diff = static_cast<unsigned char>(a.size() ^ b.size());
    for (std::size_t i = 0; i < maxSize; ++i) {
        const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned char>(ca ^ cb);
    }
    return diff == 0;
}

std::string response(int code, const std::string& status, const std::string& body);

using SystemMonitorDirectMaintenanceConfig = edge_gateway::SystemMonitorConfig::DirectMaintenanceConfig;

struct AgentDeviceIdentity {
    std::string machineCode;
    std::string imei;
    std::string serialNumber;
    std::string model;
    std::string hardwareVersion;
    std::string firmwareVersion;
};

struct AuthState {
    bool paired = false;
    bool usingDefaultPassword = true;
    bool bootstrapMode = true;
    bool sensitiveApiEnabled = false;
};

struct ConfigApplyFile {
    std::string path;
    std::string content;
};

struct DirectControlCommand {
    std::string cmdId;
    std::string meterCode;
    std::string pointCode;
    std::uint32_t index = 0;
    double value = 0.0;
    std::string source;
    std::int64_t ts = 0;
    bool highPriority = false;
};

struct RealtimeContext {
    edge_gateway::AppConfig appConfig;
    std::vector<edge_gateway::DeviceConfig> deviceConfigs;
    std::vector<std::unique_ptr<edge_gateway::MemoryPointStore>> stores;
    edge_gateway::PointStoreRouter router;
};

edge_gateway::Optional<edge_gateway::WritebackResultRecord> waitForWritebackResult(
    const edge_gateway::PointStoreRouter& router,
    const edge_gateway::PointStoreRoute& route,
    const std::string& cmdId,
    int timeoutMs
) {
    const auto deadline = nowMs() + std::max(0, timeoutMs);
    do {
        auto result = router.getWritebackResult(route, cmdId);
        if (result) {
            return result;
        }
        if (timeoutMs <= 0 || nowMs() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (true);
    return edge_gateway::NullOpt;
}

void appendControlTimingJson(
    std::ostringstream& out,
    const edge_gateway::WritebackResultRecord& result
) {
    out << ",\"stage\":\"" << jsonEscape(result.stage) << "\""
        << ",\"requestedAt\":" << result.requestedAt
        << ",\"acceptedAt\":" << result.acceptedAt
        << ",\"writeStartedAt\":" << result.startedAt
        << ",\"writeCompletedAt\":" << result.completedAt
        << ",\"queueDelayMs\":" << result.queueDelayMs
        << ",\"deviceWriteMs\":" << result.deviceWriteMs
        << ",\"edgeElapsedMs\":" << result.edgeElapsedMs
        << ",\"totalElapsedMs\":" << result.totalElapsedMs
        << ",\"verifyAttempted\":" << (result.verifyAttempted ? "true" : "false")
        << ",\"verifyPassed\":" << (result.verifyPassed ? "true" : "false")
        << ",\"highPriority\":" << (result.highPriority ? "true" : "false");
}

std::unique_ptr<RealtimeContext> createRealtimeContext(const SystemMonitorDirectMaintenanceConfig& config) {
    using namespace edge_gateway;
    std::unique_ptr<RealtimeContext> context(new RealtimeContext());
    context->appConfig = ConfigLoader::loadAppConfigFromFile(config.appConfigFile);

    edge_gateway::DeviceIdentity identity;
    if (!context->appConfig.identityConfigFile.empty()) {
        identity = ConfigLoader::loadDeviceIdentityFromFile(context->appConfig.identityConfigFile);
    }
    context->deviceConfigs = ConfigLoader::loadMany(context->appConfig.deviceConfigFiles, identity);

    std::vector<std::string> sharedMemoryNames = context->appConfig.mqttDriver.sharedMemoryNames;
    if (sharedMemoryNames.empty() && !context->appConfig.mqttDriver.sharedMemoryName.empty()) {
        sharedMemoryNames.push_back(context->appConfig.mqttDriver.sharedMemoryName);
    }
    std::unordered_set<std::string> seen(sharedMemoryNames.begin(), sharedMemoryNames.end());
    for (const auto& deviceConfig : context->deviceConfigs) {
        const auto& name = deviceConfig.memoryStore.sharedMemoryName;
        if (!name.empty() && seen.insert(name).second) {
            sharedMemoryNames.push_back(name);
        }
    }
    if (!context->appConfig.cameraService.sharedMemoryName.empty() &&
        seen.insert(context->appConfig.cameraService.sharedMemoryName).second) {
        sharedMemoryNames.push_back(context->appConfig.cameraService.sharedMemoryName);
    }
    if (sharedMemoryNames.empty()) {
        sharedMemoryNames.push_back("gateway_point_store");
    }

    context->stores.reserve(sharedMemoryNames.size());
    for (const auto& name : sharedMemoryNames) {
        try {
            context->stores.emplace_back(new MemoryPointStore(name));
            context->router.addStore(name, *context->stores.back());
        } catch (const std::exception& ex) {
            std::cerr << "SystemMonitor direct maintenance skipped shared memory "
                      << name
                      << ": "
                      << ex.what()
                      << std::endl;
        }
    }
    context->router.addRoutesFromDeviceConfigs(
        context->deviceConfigs,
        context->appConfig.mqttDriver.sharedMemoryName
    );

    const auto machineCode = !identity.machineCode.empty()
        ? identity.machineCode
        : context->appConfig.mqtt.clientId;
    context->router.addRoutesFromCameraServiceConfig(context->appConfig.cameraService, machineCode);
    return context;
}

AgentDeviceIdentity loadIdentity(const std::string& path) {
    AgentDeviceIdentity identity;
    const auto text = readFile(path);
    if (text.empty()) {
        return identity;
    }
    identity.machineCode = jsonString(text, "machineCode");
    identity.imei = jsonString(text, "imei");
    identity.serialNumber = jsonString(text, "serialNumber");
    identity.model = jsonString(text, "model");
    identity.hardwareVersion = jsonString(text, "hardwareVersion");
    identity.firmwareVersion = jsonString(text, "firmwareVersion");
    return identity;
}

std::string resolveMachineCode(
    const SystemMonitorDirectMaintenanceConfig& config,
    const edge_gateway::AppConfig& appConfig
) {
    auto machineCode = appConfig.mqtt.clientId;
    try {
        const auto identity = loadIdentity(config.identityConfigFile);
        if (!identity.machineCode.empty()) {
            machineCode = identity.machineCode;
        }
    } catch (const std::exception&) {
    }
    return machineCode;
}

AuthState loadAuthState(const SystemMonitorDirectMaintenanceConfig& config) {
    AuthState state;
    const auto text = readFile(config.authStateFile);
    if (text.empty()) {
        return state;
    }
    state.paired = jsonBool(text, "paired", state.paired);
    state.usingDefaultPassword = jsonBool(text, "usingDefaultPassword", state.usingDefaultPassword);
    state.bootstrapMode = jsonBool(text, "bootstrapMode", !state.paired);
    state.sensitiveApiEnabled = jsonBool(
        text,
        "sensitiveApiEnabled",
        state.paired && !state.usingDefaultPassword
    );
    return state;
}

std::string healthJson() {
    std::ostringstream out;
    out << "{\"success\":true,\"status\":\"ok\",\"ts\":" << nowMs()
        << ",\"message\":\"SystemMonitor direct maintenance is running\"}";
    return out.str();
}

std::string identityJson(const AgentDeviceIdentity& identity) {
    std::ostringstream out;
    out << "{\"machineCode\":\"" << jsonEscape(identity.machineCode)
        << "\",\"imei\":\"" << jsonEscape(identity.imei)
        << "\",\"serialNumber\":\"" << jsonEscape(identity.serialNumber)
        << "\",\"model\":\"" << jsonEscape(identity.model)
        << "\",\"hardwareVersion\":\"" << jsonEscape(identity.hardwareVersion)
        << "\",\"firmwareVersion\":\"" << jsonEscape(identity.firmwareVersion)
        << "\"}";
    return out.str();
}

std::string authStateJson(const AuthState& state) {
    std::ostringstream out;
    out << "{\"paired\":" << (state.paired ? "true" : "false")
        << ",\"usingDefaultPassword\":" << (state.usingDefaultPassword ? "true" : "false")
        << ",\"bootstrapMode\":" << (state.bootstrapMode ? "true" : "false")
        << ",\"sensitiveApiEnabled\":" << (state.sensitiveApiEnabled ? "true" : "false")
        << "}";
    return out.str();
}

std::string realtimePointsJson(const SystemMonitorDirectMaintenanceConfig& config, const std::string& meterCode) {
    auto context = createRealtimeContext(config);
    const auto ts = nowMs();
    const auto machineCode = resolveMachineCode(config, context->appConfig);
    const auto values = meterCode.empty()
        ? context->router.getAllLatest(ts)
        : context->router.getLatestByMeter(machineCode, meterCode, ts);
    const auto limit = config.maxRealtimePoints <= 0
        ? values.size()
        : std::min(values.size(), static_cast<std::size_t>(config.maxRealtimePoints));

    std::ostringstream out;
    out << "{\"ts\":" << ts << ",\"count\":" << limit << ",\"points\":[";
    for (std::size_t i = 0; i < limit; ++i) {
        const auto& item = values[i];
        if (i > 0) {
            out << ",";
        }
        out << "{\"index\":" << item.index
            << ",\"machineCode\":\"" << jsonEscape(item.machineCode)
            << "\",\"meterCode\":\"" << jsonEscape(item.meterCode)
            << "\",\"pointCode\":\"" << jsonEscape(item.pointCode)
            << "\",\"value\":" << item.value
            << ",\"quality\":" << item.quality
            << ",\"ts\":" << item.ts
            << ",\"stale\":" << (item.stale ? "true" : "false")
            << "}";
    }
    out << "]}";
    return out.str();
}

std::string fullTelemetryJson(const SystemMonitorDirectMaintenanceConfig& config) {
    auto context = createRealtimeContext(config);
    auto machineCode = resolveMachineCode(config, context->appConfig);
    const auto ts = nowMs();
    const auto values = context->router.getAllLatest(ts);
    const auto limit = config.maxRealtimePoints <= 0
        ? values.size()
        : std::min(values.size(), static_cast<std::size_t>(config.maxRealtimePoints));

    std::ostringstream points;
    points << "[";
    for (std::size_t i = 0; i < limit; ++i) {
        const auto& item = values[i];
        if (i > 0) {
            points << ",";
        }
        points << "{\"index\":" << item.index
               << ",\"machineCode\":\"" << jsonEscape(item.machineCode)
               << "\",\"meterCode\":\"" << jsonEscape(item.meterCode)
               << "\",\"pointCode\":\"" << jsonEscape(item.pointCode)
               << "\",\"value\":" << item.value
               << ",\"quality\":" << item.quality
               << ",\"ts\":" << item.ts
               << ",\"stale\":" << (item.stale ? "true" : "false")
               << "}";
    }
    points << "]";

    const auto pointsJson = points.str();
    std::ostringstream raw;
    raw << "{\"type\":\"telemetry/snapshot\",\"machineCode\":\"" << jsonEscape(machineCode)
        << "\",\"ts\":" << ts
        << ",\"count\":" << limit
        << ",\"points\":" << pointsJson
        << "}";

    std::ostringstream out;
    out << "{\"ts\":" << ts
        << ",\"count\":" << limit
        << ",\"topic\":\"direct:/api/v1/telemetry/full\""
        << ",\"rawPayload\":\"" << jsonEscape(raw.str()) << "\""
        << ",\"points\":" << pointsJson
        << "}";
    return out.str();
}

edge_gateway::OtaRequest parseOtaRequest(const std::string& body) {
    edge_gateway::OtaRequest request;
    request.jobId = jsonString(body, "jobId", "DIRECT_OTA_" + std::to_string(nowMs()));
    request.artifactUrl = jsonString(body, "artifactUrl");
    request.version = jsonString(body, "version");
    request.sha256 = jsonString(body, "sha256");
    request.size = jsonUint64(body, "size", 0);
    request.upgradeMode = jsonString(body, "upgradeMode", "download_install_reboot");
    request.ts = nowMs();
    return request;
}

void appendOtaStatus(const SystemMonitorDirectMaintenanceConfig& config, const edge_gateway::OtaStatus& status) {
#ifndef _WIN32
    const auto slash = config.otaStatusFile.find_last_of('/');
    if (slash != std::string::npos) {
        const auto dir = config.otaStatusFile.substr(0, slash);
        std::string current;
        for (const auto ch : dir) {
            current += ch;
            if (ch == '/' && current.size() > 1) {
                (void)::mkdir(current.c_str(), 0755);
            }
        }
        if (!current.empty()) {
            (void)::mkdir(current.c_str(), 0755);
        }
    }
#endif

    std::ofstream output(config.otaStatusFile, std::ios::app);
    if (!output.is_open()) {
        return;
    }
    output << "{\"jobId\":\"" << jsonEscape(status.jobId)
           << "\",\"machineCode\":\"" << jsonEscape(status.machineCode)
           << "\",\"stage\":\"" << jsonEscape(status.stage)
           << "\",\"progress\":" << status.progress
           << ",\"downloadedBytes\":" << status.downloadedBytes
           << ",\"totalBytes\":" << status.totalBytes
           << ",\"message\":\"" << jsonEscape(status.message)
           << "\",\"ts\":" << status.ts
           << "}\n";
}

bool maintenancePasswordAccepted(const SystemMonitorDirectMaintenanceConfig& config, const std::string& body) {
    (void)config;
    const auto providedHash = trim(jsonString(body, "passwordSha256"));
    return !providedHash.empty() && constantTimeEquals(providedHash, kFixedMaintenancePasswordSha256);
}

void collectConfigFilesFromAppConfig(std::vector<std::string>& files, const std::string& appConfigPath) {
    if (appConfigPath.empty()) {
        return;
    }
    addUniquePath(files, appConfigPath);
    for (const auto& sibling : discoverSiblingAppConfigFiles(appConfigPath)) {
        addUniquePath(files, sibling);
    }

    const auto appConfig = edge_gateway::ConfigLoader::loadAppConfigFromFile(appConfigPath);
    addUniquePath(files, appConfig.identityConfigFile);
    for (const auto& file : appConfig.deviceConfigFiles) {
        addUniquePath(files, file);
    }
}

std::vector<std::string> collectConfigSnapshotFiles(const SystemMonitorDirectMaintenanceConfig& config) {
    std::vector<std::string> files;
    addUniquePath(files, config.identityConfigFile);
    collectConfigFilesFromAppConfig(files, config.appConfigFile);
    collectConfigFilesFromAppConfig(files, config.otaAppConfigFile);
    return files;
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool containsPathSegment(const std::string& path, const std::string& segment) {
    return path.find(segment) != std::string::npos;
}

std::string baseNameOf(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string readRequiredFile(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open file: " + path);
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

int makeDir(const std::string& path) {
#ifndef _WIN32
    return ::mkdir(path.c_str(), 0755);
#else
    (void)path;
    return 0;
#endif
}

std::string parentDirectory(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

void ensureDirectory(const std::string& path) {
    if (path.empty()) {
        return;
    }
    std::string current;
    std::size_t cursor = 0;
    if (path.front() == '/' || path.front() == '\\') {
        current = path.substr(0, 1);
        cursor = 1;
    }
    while (cursor <= path.size()) {
        const auto next = path.find_first_of("/\\", cursor);
        const auto part = path.substr(cursor, next == std::string::npos ? std::string::npos : next - cursor);
        if (!part.empty()) {
            if (!current.empty() && current.back() != '/' && current.back() != '\\') {
                current += "/";
            }
            current += part;
            if (makeDir(current) != 0 && errno != EEXIST) {
                throw std::runtime_error("failed to create directory: " + current);
            }
        }
        if (next == std::string::npos) {
            break;
        }
        cursor = next + 1;
    }
}

void writeTextFileAtomically(const std::string& path, const std::string& content) {
    ensureDirectory(parentDirectory(path));
    const auto tempPath = path + ".tmp." + std::to_string(nowMs());
    {
        std::ofstream output(tempPath.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            throw std::runtime_error("failed to open temp file: " + tempPath);
        }
        output << content;
        if (!output) {
            throw std::runtime_error("failed to write temp file: " + tempPath);
        }
    }
    if (std::rename(tempPath.c_str(), path.c_str()) != 0) {
        std::remove(tempPath.c_str());
        throw std::runtime_error("failed to replace file: " + path);
    }
}

std::string backupFileIfExistsWithPath(const std::string& path) {
    if (!isRegularFile(path)) {
        return std::string();
    }
    const auto backupPath = path + ".bak." + std::to_string(nowMs());
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    std::ofstream output(backupPath.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!input.is_open() || !output.is_open()) {
        throw std::runtime_error("failed to create backup for: " + path);
    }
    output << input.rdbuf();
    if (!output) {
        throw std::runtime_error("failed to write backup for: " + path);
    }
    return backupPath;
}

void backupFileIfExists(const std::string& path) {
    (void)backupFileIfExistsWithPath(path);
}

std::string latestBackupPathFor(const std::string& path) {
    const auto dir = dirnameOf(path);
    const auto base = baseNameOf(path);
    const auto prefix = base + ".bak.";
    std::string bestName;
#ifndef _WIN32
    DIR* handle = ::opendir(dir.c_str());
    if (handle == nullptr) {
        return std::string();
    }
    while (auto* entry = ::readdir(handle)) {
        const std::string name = entry->d_name;
        if (startsWith(name, prefix) && (bestName.empty() || name > bestName)) {
            bestName = name;
        }
    }
    ::closedir(handle);
#else
    (void)prefix;
#endif
    if (bestName.empty()) {
        return std::string();
    }
    return dir == "." ? bestName : dir + "/" + bestName;
}

bool isAllowedConfigPath(const SystemMonitorDirectMaintenanceConfig& config, const std::string& path) {
    if (path.empty() || path.find("..") != std::string::npos) {
        return false;
    }
    const auto allowed = collectConfigSnapshotFiles(config);
    return std::find(allowed.begin(), allowed.end(), path) != allowed.end();
}

void validateConfigContentForPath(const SystemMonitorDirectMaintenanceConfig& config, const std::string& path, const std::string& content) {
    const auto normalized = path;
    const auto name = baseNameOf(path);
    if (containsPathSegment(normalized, "/devices/")) {
        (void)edge_gateway::ConfigLoader::loadFromText(content);
        return;
    }

    const auto tempPath = path + ".validate." + std::to_string(nowMs());
    writeTextFileAtomically(tempPath, content);
    try {
        if (path == config.identityConfigFile || name == "device_identity.json") {
            (void)edge_gateway::ConfigLoader::loadDeviceIdentityFromFile(tempPath);
        } else if (path == config.appConfigFile ||
                   path == config.otaAppConfigFile ||
                   containsPathSegment(normalized, "/apps/")) {
            (void)edge_gateway::ConfigLoader::loadAppConfigFromFile(tempPath);
        } else {
            (void)edge_gateway::ConfigLoader::loadFromText(content);
        }
    } catch (...) {
        std::remove(tempPath.c_str());
        throw;
    }
    std::remove(tempPath.c_str());
}

std::size_t skipWhitespace(const std::string& text, std::size_t pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
    return pos;
}

std::size_t skipJsonString(const std::string& text, std::size_t pos) {
    if (pos >= text.size() || text[pos] != '"') {
        return std::string::npos;
    }
    ++pos;
    while (pos < text.size()) {
        const char ch = text[pos++];
        if (ch == '"') {
            return pos;
        }
        if (ch == '\\' && pos < text.size()) {
            ++pos;
        }
    }
    return std::string::npos;
}

std::size_t findMatchingJsonToken(const std::string& text, std::size_t begin, char openToken, char closeToken) {
    if (begin >= text.size() || text[begin] != openToken) {
        return std::string::npos;
    }
    int depth = 0;
    for (std::size_t pos = begin; pos < text.size();) {
        if (text[pos] == '"') {
            pos = skipJsonString(text, pos);
            if (pos == std::string::npos) {
                return std::string::npos;
            }
            continue;
        }
        if (text[pos] == openToken) {
            ++depth;
        } else if (text[pos] == closeToken) {
            --depth;
            if (depth == 0) {
                return pos;
            }
        }
        ++pos;
    }
    return std::string::npos;
}

std::vector<ConfigApplyFile> parseConfigApplyFiles(const std::string& body) {
    const auto filesPos = findJsonValue(body, "files");
    auto pos = skipWhitespace(body, filesPos);
    if (pos == std::string::npos || pos >= body.size() || body[pos] != '[') {
        throw std::runtime_error("config apply files array is required");
    }
    const auto arrayEnd = findMatchingJsonToken(body, pos, '[', ']');
    if (arrayEnd == std::string::npos) {
        throw std::runtime_error("config apply files array is malformed");
    }

    std::vector<ConfigApplyFile> files;
    ++pos;
    while (pos < arrayEnd) {
        pos = skipWhitespace(body, pos);
        if (pos >= arrayEnd) {
            break;
        }
        if (body[pos] == ',') {
            ++pos;
            continue;
        }
        if (body[pos] != '{') {
            throw std::runtime_error("config apply file item must be object");
        }
        const auto objectEnd = findMatchingJsonToken(body, pos, '{', '}');
        if (objectEnd == std::string::npos || objectEnd > arrayEnd) {
            throw std::runtime_error("config apply file item is malformed");
        }
        const auto objectText = body.substr(pos, objectEnd - pos + 1);
        ConfigApplyFile file;
        file.path = jsonString(objectText, "path");
        file.content = jsonString(objectText, "content");
        if (file.path.empty()) {
            throw std::runtime_error("config apply file path is required");
        }
        if (file.content.size() > kMaxConfigSnapshotFileBytes) {
            throw std::runtime_error("config apply file is too large: " + file.path);
        }
        files.push_back(std::move(file));
        if (files.size() > kMaxConfigApplyFiles) {
            throw std::runtime_error("too many config apply files");
        }
        pos = objectEnd + 1;
    }
    if (files.empty()) {
        throw std::runtime_error("config apply files cannot be empty");
    }
    return files;
}

std::vector<DirectControlCommand> parseBatchControlCommands(const std::string& body) {
    const auto commandsPos = findJsonValue(body, "commands");
    auto pos = skipWhitespace(body, commandsPos);
    if (pos == std::string::npos || pos >= body.size() || body[pos] != '[') {
        throw std::runtime_error("batch control commands array is required");
    }
    const auto arrayEnd = findMatchingJsonToken(body, pos, '[', ']');
    if (arrayEnd == std::string::npos) {
        throw std::runtime_error("batch control commands array is malformed");
    }

    std::vector<DirectControlCommand> commands;
    ++pos;
    while (pos < arrayEnd) {
        pos = skipWhitespace(body, pos);
        if (pos >= arrayEnd) {
            break;
        }
        if (body[pos] == ',') {
            ++pos;
            continue;
        }
        if (body[pos] != '{') {
            throw std::runtime_error("batch control command item must be object");
        }
        const auto objectEnd = findMatchingJsonToken(body, pos, '{', '}');
        if (objectEnd == std::string::npos || objectEnd > arrayEnd) {
            throw std::runtime_error("batch control command item is malformed");
        }
        const auto objectText = body.substr(pos, objectEnd - pos + 1);
        DirectControlCommand command;
        command.cmdId = jsonString(objectText, "cmdId");
        command.meterCode = jsonString(objectText, "meterCode");
        command.pointCode = jsonString(objectText, "pointCode");
        const auto rawIndex = jsonUint64(objectText, "index", 0);
        if (rawIndex == 0 || rawIndex > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("batch control command index is required");
        }
        command.index = static_cast<std::uint32_t>(rawIndex);
        if (!tryJsonDouble(objectText, "value", &command.value)) {
            throw std::runtime_error("batch control command value is required");
        }
        command.source = jsonString(objectText, "source", "system-monitor-direct-maintenance");
        command.ts = static_cast<std::int64_t>(jsonUint64(objectText, "ts", static_cast<std::uint64_t>(nowMs())));
        command.highPriority = jsonBool(objectText, "highPriority", false) ||
            jsonBool(objectText, "priority", false) ||
            jsonBool(objectText, "priorityControl", false);
        if (command.cmdId.empty()) {
            command.cmdId = "DIRECT_CTRL_" + std::to_string(nowMs()) + "_" + std::to_string(commands.size() + 1);
        }
        commands.push_back(std::move(command));
        if (commands.size() > kMaxBatchControlCommands) {
            throw std::runtime_error("too many batch control commands");
        }
        pos = objectEnd + 1;
    }
    if (commands.empty()) {
        throw std::runtime_error("batch control commands cannot be empty");
    }
    return commands;
}

std::string runShellCommand(const std::string& command, int* exitCode = nullptr, std::size_t maxOutputBytes = 0) {
    std::string output;
    bool truncated = false;
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        if (exitCode != nullptr) {
            *exitCode = -1;
        }
        return output;
    }
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        const std::size_t chunkSize = std::strlen(buffer);
        if (maxOutputBytes == 0) {
            output.append(buffer, chunkSize);
            continue;
        }
        if (output.size() < maxOutputBytes) {
            const auto remaining = maxOutputBytes - output.size();
            output.append(buffer, std::min(remaining, chunkSize));
            if (chunkSize > remaining) {
                truncated = true;
            }
        } else if (chunkSize > 0) {
            truncated = true;
        }
    }
    const int rc = pclose(pipe);
    if (exitCode != nullptr) {
#ifndef _WIN32
        *exitCode = WIFEXITED(rc) ? WEXITSTATUS(rc) : rc;
#else
        *exitCode = rc;
#endif
    }
    if (truncated) {
        output += "\n[truncated]";
    }
    return output;
}

bool isSafeSystemdUnitName(const std::string& value) {
    if (value.empty() || value.size() > 128) {
        return false;
    }
    const std::string suffix = ".service";
    if (value.size() <= suffix.size() ||
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }
    for (const auto ch : value) {
        const bool ok = std::isalnum(static_cast<unsigned char>(ch)) != 0 ||
            ch == '_' || ch == '-' || ch == '.' || ch == '@' || ch == ':';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool readCpuCounters(std::uint64_t* total, std::uint64_t* idleTotal) {
    std::ifstream stat("/proc/stat");
    std::string label;
    std::uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    if (!(stat >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal)) {
        return false;
    }
    *total = user + nice + system + idle + iowait + irq + softirq + steal;
    *idleTotal = idle + iowait;
    return true;
}

double cpuUsagePercent() {
    std::uint64_t total1 = 0, idle1 = 0, total2 = 0, idle2 = 0;
    if (!readCpuCounters(&total1, &idle1)) {
        return 0.0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!readCpuCounters(&total2, &idle2) || total2 <= total1) {
        return 0.0;
    }
    const auto totalDiff = total2 - total1;
    const auto idleDiff = idle2 >= idle1 ? idle2 - idle1 : 0;
    return totalDiff == 0 ? 0.0 : 100.0 * static_cast<double>(totalDiff - idleDiff) / static_cast<double>(totalDiff);
}

int countProcesses() {
    int count = 0;
#ifndef _WIN32
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
#endif
    return count;
}

double memUsagePercent() {
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
    if (totalMem == 0 || availMem > totalMem) {
        return 0.0;
    }
    return 100.0 * static_cast<double>(totalMem - availMem) / static_cast<double>(totalMem);
}

double diskUsagePercent() {
#ifndef _WIN32
    struct statvfs vfs {};
    if (statvfs("/", &vfs) != 0 || vfs.f_blocks == 0) {
        return 0.0;
    }
    const auto used = vfs.f_blocks - vfs.f_bavail;
    return 100.0 * static_cast<double>(used) / static_cast<double>(vfs.f_blocks);
#else
    return 0.0;
#endif
}

double load1Value() {
    std::ifstream loadavg("/proc/loadavg");
    double load1 = 0.0;
    if (loadavg) {
        loadavg >> load1;
    }
    return load1;
}

std::string serviceState(const std::string& service) {
    if (!isSafeSystemdUnitName(service)) {
        return "unknown";
    }
    int exitCode = 0;
    auto state = trim(runShellCommand("systemctl is-active " + service + " 2>/dev/null", &exitCode, 256));
    if (state.empty()) {
        state = exitCode == 0 ? "active" : "unknown";
    }
    return state;
}

std::string systemStatusJson(const SystemMonitorDirectMaintenanceConfig& config) {
    const auto identity = loadIdentity(config.identityConfigFile);
    auto machineCode = identity.machineCode;
    if (machineCode.empty()) {
        try {
            const auto appConfig = edge_gateway::ConfigLoader::loadAppConfigFromFile(config.appConfigFile);
            machineCode = appConfig.mqtt.clientId;
        } catch (...) {
        }
    }

    const std::vector<std::string> services = {
        "gateway-services.service",
        "mqtt-driver@mqtt-service.service",
        "system-monitor@monitor-service.service",
        "compute-engine@mqtt-service.service",
        "event-engine@mqtt-service.service"
    };

    std::ostringstream out;
    out << "{\"machineCode\":\"" << jsonEscape(machineCode)
        << "\",\"cpuUsage\":" << cpuUsagePercent()
        << ",\"memUsage\":" << memUsagePercent()
        << ",\"diskUsage\":" << diskUsagePercent()
        << ",\"load1\":" << load1Value()
        << ",\"processCount\":" << countProcesses()
        << ",\"cellular\":{"
        << "\"enabled\":false,\"present\":false,\"registered\":false,\"connected\":false,\"simReady\":false,\"toolsAvailable\":false"
        << ",\"operator\":\"\",\"accessTech\":\"\",\"interfaceName\":\"\",\"ipAddress\":\"\",\"gateway\":\"\",\"dns\":\"\""
        << ",\"simStatus\":\"\",\"imei\":\"\",\"imsi\":\"\",\"iccid\":\"\",\"modemDevice\":\"\",\"lastError\":\"\""
        << ",\"signalPercent\":-1,\"rssiDbm\":0,\"rsrpDbm\":0,\"rsrqDb\":0,\"sinrDb\":0"
        << ",\"rxBytes\":0,\"txBytes\":0,\"rxRateBps\":0,\"txRateBps\":0,\"ts\":" << nowMs() << "}"
        << ",\"services\":[";
    for (std::size_t i = 0; i < services.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        out << "{\"name\":\"" << jsonEscape(services[i])
            << "\",\"state\":\"" << jsonEscape(serviceState(services[i]))
            << "\",\"message\":\"\"}";
    }
    out << "],\"ts\":" << nowMs() << "}";
    return out.str();
}

std::string executeDiagCommand(const std::string& command, const std::string& service, int tailLines, int* exitCode) {
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
        shellCommand = "ps | grep -E 'ModbusRtu|Dlt645Driver|MqttDriver|EventEngine|SystemMonitor|SystemMonitor direct maintenance' 2>&1";
    } else if (command == "journal_tail") {
        const int boundedLines = std::max(1, std::min(500, tailLines));
        shellCommand = "journalctl -n " + std::to_string(boundedLines) + " --no-pager 2>&1";
    } else if (command == "systemctl_status") {
        if (!isSafeSystemdUnitName(service)) {
            throw std::runtime_error("invalid service name");
        }
        shellCommand = "systemctl status " + service + " --no-pager 2>&1";
    } else if (command == "cellular_status") {
        shellCommand =
            "sh -c 'echo \"# network interfaces\"; ip -br addr 2>&1 || true; "
            "echo; echo \"# route\"; ip route 2>&1 || true; "
            "echo; echo \"# dns\"; cat /etc/resolv.conf 2>&1 || true; "
            "echo; echo \"# modem devices\"; ls -l /dev/ttyUSB* /dev/cdc-wdm* 2>/dev/null || true; "
            "echo; echo \"# traffic\"; cat /proc/net/dev 2>&1 || true'";
    } else {
        throw std::runtime_error("unsupported diag command");
    }
    return runShellCommand(shellCommand, exitCode, kMaxDiagOutputBytes);
}

std::string diagResponseJson(const SystemMonitorDirectMaintenanceConfig& config, const std::string& body) {
    if (!maintenancePasswordAccepted(config, body)) {
        throw std::runtime_error("invalid maintenance password");
    }
    const auto command = jsonString(body, "command");
    const auto cmdId = jsonString(body, "cmdId", "DIAG_" + std::to_string(nowMs()));
    const auto machineCode = jsonString(body, "machineCode");
    const auto operatorName = jsonString(body, "operator");
    const auto service = jsonString(body, "service");
    const auto tailLines = jsonInt(body, "tailLines", 100);
    const auto requestTs = static_cast<std::int64_t>(jsonUint64(body, "requestTs", static_cast<std::uint64_t>(nowMs())));
    int exitCode = 0;
    const auto stdoutText = executeDiagCommand(command, service, tailLines, &exitCode);
    std::ostringstream out;
    out << "{\"cmdId\":\"" << jsonEscape(cmdId)
        << "\",\"machineCode\":\"" << jsonEscape(machineCode)
        << "\",\"operator\":\"" << jsonEscape(operatorName)
        << "\",\"command\":\"" << jsonEscape(command)
        << "\",\"success\":" << (exitCode == 0 ? "true" : "false")
        << ",\"exitCode\":" << exitCode
        << ",\"stdout\":\"" << jsonEscape(stdoutText)
        << "\",\"stderr\":\"\""
        << ",\"requestTs\":" << requestTs
        << ",\"ts\":" << nowMs()
        << "}";
    return out.str();
}

void restartGatewayServicesLater() {
    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        (void)runShellCommand(
            "sh -c 'if command -v systemctl >/dev/null 2>&1; then systemctl restart gateway-services.service; "
            "elif [ -x /opt/modbus-gateway/bin/gateway-services.sh ]; then /opt/modbus-gateway/bin/gateway-services.sh restart; fi' "
            ">/dev/null 2>&1"
        );
    }).detach();
}

std::string configApplyJson(const SystemMonitorDirectMaintenanceConfig& config, const std::string& body, bool requirePassword) {
    if (requirePassword && !maintenancePasswordAccepted(config, body)) {
        throw std::runtime_error("invalid maintenance password");
    }
    const auto now = nowMs();
    const auto requestId = jsonString(body, "requestId", "CFG_APPLY_" + std::to_string(now));
    const auto requestedMachineCode = trim(jsonString(body, "machineCode"));
    const auto dryRun = jsonBool(body, "dryRun", true);
    const auto restartServices = jsonBool(body, "restartServices", false);
    const auto appConfig = edge_gateway::ConfigLoader::loadAppConfigFromFile(config.appConfigFile);
    const auto identity = loadIdentity(config.identityConfigFile);
    const auto machineCode = identity.machineCode.empty() ? appConfig.mqtt.clientId : identity.machineCode;
    if (!requestedMachineCode.empty() &&
        !machineCode.empty() &&
        requestedMachineCode != machineCode) {
        throw std::runtime_error("machineCode mismatch");
    }

    const auto files = parseConfigApplyFiles(body);
    bool allSuccess = true;
    int changedFiles = 0;
    int appliedFiles = 0;
    std::ostringstream results;
    results << "[";
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& file = files[i];
        bool success = true;
        bool changed = false;
        std::string message = "ok";
        try {
            if (!isAllowedConfigPath(config, file.path)) {
                throw std::runtime_error("path is not allowed");
            }
            validateConfigContentForPath(config, file.path, file.content);
            const auto current = readFile(file.path);
            changed = current != file.content;
            if (changed) {
                ++changedFiles;
            }
            if (!dryRun && changed) {
                backupFileIfExists(file.path);
                writeTextFileAtomically(file.path, file.content);
                ++appliedFiles;
            }
        } catch (const std::exception& ex) {
            success = false;
            allSuccess = false;
            message = ex.what();
        }
        if (i > 0) {
            results << ",";
        }
        results << "{\"path\":\"" << jsonEscape(file.path)
                << "\",\"success\":" << (success ? "true" : "false")
                << ",\"changed\":" << (changed ? "true" : "false")
                << ",\"message\":\"" << jsonEscape(message) << "\"}";
    }
    results << "]";

    if (!dryRun && restartServices && allSuccess && changedFiles > 0) {
        restartGatewayServicesLater();
    }

    std::ostringstream out;
    out << "{\"requestId\":\"" << jsonEscape(requestId)
        << "\",\"machineCode\":\"" << jsonEscape(machineCode)
        << "\",\"success\":" << (allSuccess ? "true" : "false")
        << ",\"dryRun\":" << (dryRun ? "true" : "false")
        << ",\"appliedFiles\":" << appliedFiles
        << ",\"changedFiles\":" << changedFiles
        << ",\"message\":\"" << (allSuccess ? (dryRun ? "config apply dry-run passed" : "config apply completed") : "config apply failed")
        << "\",\"files\":" << results.str()
        << ",\"ts\":" << now << "}";
    return out.str();
}

std::string configFileOperationJson(const SystemMonitorDirectMaintenanceConfig& config, const std::string& body, const std::string& operation) {
    const auto now = nowMs();
    const auto requestId = jsonString(body, "requestId", "CFG_" + operation + "_" + std::to_string(now));
    const auto requestedMachineCode = trim(jsonString(body, "machineCode"));
    const auto path = trim(jsonString(body, "path"));
    const auto dryRun = jsonBool(body, "dryRun", true);
    const auto restartServices = jsonBool(body, "restartServices", false);
    const auto appConfig = edge_gateway::ConfigLoader::loadAppConfigFromFile(config.appConfigFile);
    const auto identity = loadIdentity(config.identityConfigFile);
    const auto machineCode = identity.machineCode.empty() ? appConfig.mqtt.clientId : identity.machineCode;
    if (!requestedMachineCode.empty() &&
        !machineCode.empty() &&
        requestedMachineCode != machineCode) {
        throw std::runtime_error("machineCode mismatch");
    }
    if (!isAllowedConfigPath(config, path)) {
        throw std::runtime_error("path is not allowed");
    }

    bool changed = false;
    std::string backupPath;
    std::string message;
    if (operation == "delete") {
        changed = isRegularFile(path);
        if (!dryRun && changed) {
            backupPath = backupFileIfExistsWithPath(path);
            if (std::remove(path.c_str()) != 0) {
                throw std::runtime_error("failed to delete file: " + path);
            }
        }
        message = dryRun
            ? (changed ? "config delete dry-run passed" : "config file already missing")
            : (changed ? "config file deleted" : "config file already missing");
    } else if (operation == "restore") {
        const auto restorePath = latestBackupPathFor(path);
        if (restorePath.empty() || !isRegularFile(restorePath)) {
            throw std::runtime_error("backup file not found for: " + path);
        }
        const auto content = readRequiredFile(restorePath);
        validateConfigContentForPath(config, path, content);
        changed = true;
        if (!dryRun) {
            backupPath = backupFileIfExistsWithPath(path);
            writeTextFileAtomically(path, content);
        }
        message = dryRun ? "config restore dry-run passed" : "config backup restored from " + restorePath;
    } else {
        throw std::runtime_error("unsupported config operation");
    }

    if (!dryRun && restartServices && changed) {
        restartGatewayServicesLater();
    }

    std::ostringstream out;
    out << "{\"requestId\":\"" << jsonEscape(requestId)
        << "\",\"machineCode\":\"" << jsonEscape(machineCode)
        << "\",\"success\":true"
        << ",\"dryRun\":" << (dryRun ? "true" : "false")
        << ",\"operation\":\"" << jsonEscape(operation)
        << "\",\"path\":\"" << jsonEscape(path)
        << "\",\"changed\":" << (changed ? "true" : "false")
        << ",\"message\":\"" << jsonEscape(message)
        << "\",\"backupPath\":\"" << jsonEscape(backupPath)
        << "\",\"ts\":" << now << "}";
    return out.str();
}

std::string configFileOperationResponse(
    const SystemMonitorDirectMaintenanceConfig& config,
    const std::string& body,
    const std::string& operation
) {
    if (!maintenancePasswordAccepted(config, body)) {
        return response(
            401,
            "Unauthorized",
            "{\"success\":false,\"message\":\"invalid maintenance password\"}"
        );
    }

    try {
        return response(200, "OK", configFileOperationJson(config, body, operation));
    } catch (const std::exception& ex) {
        return response(400, "Bad Request", "{\"success\":false,\"message\":\"" + jsonEscape(ex.what()) + "\"}");
    }
}

std::string configSnapshotJson(const SystemMonitorDirectMaintenanceConfig& config, const std::string& body) {
    const auto now = nowMs();
    const auto requestId = jsonString(body, "requestId", "DIRECT_CFG_PULL_" + std::to_string(now));
    const auto requestedMachineCode = trim(jsonString(body, "machineCode"));
    const auto appConfig = edge_gateway::ConfigLoader::loadAppConfigFromFile(config.appConfigFile);
    const auto identity = loadIdentity(config.identityConfigFile);
    const auto machineCode = identity.machineCode.empty() ? appConfig.mqtt.clientId : identity.machineCode;
    if (!requestedMachineCode.empty() &&
        !machineCode.empty() &&
        requestedMachineCode != machineCode) {
        throw std::runtime_error("machineCode mismatch");
    }

    const auto files = collectConfigSnapshotFiles(config);
    std::size_t emittedFiles = 0;
    std::size_t skippedFiles = 0;
    std::size_t totalBytes = 0;

    std::ostringstream out;
    out << "{\"requestId\":\"" << jsonEscape(requestId)
        << "\",\"machineCode\":\"" << jsonEscape(machineCode)
        << "\",\"success\":true"
        << ",\"message\":\"config snapshot exported\""
        << ",\"files\":[";

    for (const auto& path : files) {
        if (path.empty()) {
            continue;
        }
        if (emittedFiles >= kMaxConfigSnapshotFiles) {
            ++skippedFiles;
            continue;
        }
        if (!isRegularFile(path)) {
            ++skippedFiles;
            continue;
        }
        const auto size = static_cast<std::size_t>(fileSizeBytes(path));
        if (size > kMaxConfigSnapshotFileBytes || totalBytes > kMaxConfigSnapshotTotalBytes - size) {
            ++skippedFiles;
            continue;
        }

        const auto content = readFile(path);
        if (content.size() > kMaxConfigSnapshotFileBytes ||
            totalBytes > kMaxConfigSnapshotTotalBytes - content.size()) {
            ++skippedFiles;
            continue;
        }
        totalBytes += content.size();
        if (emittedFiles > 0) {
            out << ",";
        }
        out << "{\"path\":\"" << jsonEscape(path)
            << "\",\"sizeBytes\":" << static_cast<long long>(content.size())
            << ",\"modifiedAtMs\":" << static_cast<long long>(modifiedAtMs(path))
            << ",\"content\":\"" << jsonEscape(content) << "\"}";
        ++emittedFiles;
        if (out.tellp() > static_cast<std::streampos>(kMaxConfigSnapshotReplyBytes)) {
            throw std::runtime_error("config snapshot reply is too large");
        }
    }

    out << "],\"fileCount\":" << static_cast<long long>(emittedFiles)
        << ",\"skippedFiles\":" << static_cast<long long>(skippedFiles)
        << ",\"totalBytes\":" << static_cast<long long>(totalBytes)
        << ",\"ts\":" << now << "}";
    if (out.tellp() > static_cast<std::streampos>(kMaxConfigSnapshotReplyBytes)) {
        throw std::runtime_error("config snapshot reply is too large");
    }
    return out.str();
}

std::string configSnapshotResponse(const SystemMonitorDirectMaintenanceConfig& config, const std::string& body) {
    if (!maintenancePasswordAccepted(config, body)) {
        return response(
            401,
            "Unauthorized",
            "{\"success\":false,\"message\":\"invalid maintenance password\"}"
        );
    }

    try {
        return response(200, "OK", configSnapshotJson(config, body));
    } catch (const std::exception& ex) {
        return response(400, "Bad Request", "{\"success\":false,\"message\":\"" + jsonEscape(ex.what()) + "\"}");
    }
}

std::string batchControlJson(const SystemMonitorDirectMaintenanceConfig& config, const std::string& body) {
    const auto requestTs = nowMs();
    auto context = createRealtimeContext(config);
    const auto resolvedMachineCode = resolveMachineCode(config, context->appConfig);
    const auto requestedMachineCode = trim(jsonString(body, "machineCode"));
    const auto machineCode = requestedMachineCode.empty() ? resolvedMachineCode : requestedMachineCode;
    if (!requestedMachineCode.empty() &&
        !resolvedMachineCode.empty() &&
        requestedMachineCode != resolvedMachineCode) {
        throw std::runtime_error("machineCode mismatch");
    }

    const auto requestId = jsonString(body, "requestId", "CTRL_BATCH_" + std::to_string(requestTs));
    const auto commands = parseBatchControlCommands(body);
    edge_gateway::PriorityControlLease priorityLease(
        context->appConfig.mqttDriver.priorityControlLeaseFile,
        "system-monitor-direct-maintenance"
    );
    int accepted = 0;
    std::ostringstream results;
    results << "[";
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const auto& item = commands[i];
        bool success = false;
        std::string message;
        auto route = context->router.routeByIndex(item.index);
        edge_gateway::Optional<edge_gateway::WritebackResultRecord> writeback = edge_gateway::NullOpt;
        std::string stage = "rejected";
        std::int64_t itemTs = nowMs();
        std::int64_t requestedAt = item.ts > 0 ? item.ts : itemTs;
        std::int64_t acceptedAt = 0;
        try {
            if (!route) {
                throw std::runtime_error("command index not found");
            }
            if (!machineCode.empty() && machineCode != route->machineCode) {
                throw std::runtime_error("machineCode mismatch");
            }
            if (!item.meterCode.empty() && item.meterCode != route->meterCode) {
                throw std::runtime_error("meterCode mismatch");
            }
            if (!item.pointCode.empty() && item.pointCode != route->pointCode) {
                throw std::runtime_error("pointCode mismatch");
            }
            const auto activePriorityLease = priorityLease.activeLease(nowMs());
            if (activePriorityLease && activePriorityLease->cmdId != item.cmdId) {
                throw std::runtime_error("priority control in progress");
            }

            if (item.highPriority) {
                priorityLease.acquire(
                    item.cmdId,
                    route->meterCode,
                    item.index,
                    nowMs(),
                    context->appConfig.mqttDriver.priorityControlLeaseTtlMs
                );
            }

            edge_gateway::PendingWriteCommand command;
            command.cmdId = item.cmdId;
            command.index = item.index;
            command.value = item.value;
            command.source = item.source.empty() ? "system-monitor-direct-maintenance" : item.source;
            command.ts = requestedAt;
            command.acceptedAt = nowMs();
            command.highPriority = item.highPriority;
            acceptedAt = command.acceptedAt;
            const auto submitResult = context->router.submitWriteCommand(command);
            if (!submitResult.accepted) {
                if (item.highPriority) {
                    priorityLease.release(item.cmdId);
                }
                throw std::runtime_error(submitResult.message);
            }

            route = submitResult.route;
            writeback = waitForWritebackResult(
                context->router,
                *route,
                item.cmdId,
                context->appConfig.mqttDriver.controlResultWaitTimeoutMs
            );
            if (writeback) {
                success = writeback->success;
                message = writeback->message.empty() ? (success ? "ok" : "writeback failed") : writeback->message;
                stage = writeback->stage;
                writeback->highPriority = item.highPriority;
                itemTs = writeback->completedAt > 0 ? writeback->completedAt : nowMs();
            } else {
                itemTs = nowMs();
                success = false;
                message = "writeback result timeout";
                stage = "writeback-timeout";
            }
            if (success) {
                ++accepted;
            }
        } catch (const std::exception& ex) {
            message = ex.what();
        }

        if (i > 0) {
            results << ",";
        }
        results << "{\"cmdId\":\"" << jsonEscape(item.cmdId)
                << "\",\"machineCode\":\"" << jsonEscape(route ? route->machineCode : machineCode)
                << "\",\"meterCode\":\"" << jsonEscape(route ? route->meterCode : item.meterCode)
                << "\",\"pointCode\":\"" << jsonEscape(route ? route->pointCode : item.pointCode)
                << "\",\"index\":" << item.index
                << ",\"value\":" << item.value
                << ",\"success\":" << (success ? "true" : "false")
                << ",\"message\":\"" << jsonEscape(message)
                << "\",\"ts\":" << itemTs;
        if (writeback) {
            appendControlTimingJson(results, *writeback);
        } else {
            results << ",\"stage\":\"" << jsonEscape(stage) << "\""
                    << ",\"requestedAt\":" << requestedAt
                    << ",\"acceptedAt\":" << acceptedAt
                    << ",\"writeStartedAt\":0"
                    << ",\"writeCompletedAt\":0"
                    << ",\"queueDelayMs\":0"
                    << ",\"deviceWriteMs\":0"
                    << ",\"edgeElapsedMs\":" << nonNegativeDuration(itemTs, acceptedAt)
                    << ",\"totalElapsedMs\":" << nonNegativeDuration(itemTs, requestedAt)
                    << ",\"verifyAttempted\":false"
                    << ",\"verifyPassed\":false"
                    << ",\"highPriority\":" << (item.highPriority ? "true" : "false");
        }
        results << "}";
    }
    results << "]";

    const auto total = static_cast<int>(commands.size());
    const auto failed = total - accepted;
    std::ostringstream out;
    out << "{\"requestId\":\"" << jsonEscape(requestId)
        << "\",\"machineCode\":\"" << jsonEscape(machineCode)
        << "\",\"success\":" << (failed == 0 ? "true" : "false")
        << ",\"total\":" << total
        << ",\"accepted\":" << accepted
        << ",\"failed\":" << failed
        << ",\"message\":\""
        << (failed == 0 ? "direct batch control accepted" : "direct batch control partially failed")
        << "\",\"results\":" << results.str()
        << ",\"ts\":" << requestTs
        << "}";
    return out.str();
}

std::string batchControlResponse(const SystemMonitorDirectMaintenanceConfig& config, const std::string& body) {
    if (!maintenancePasswordAccepted(config, body)) {
        return response(
            401,
            "Unauthorized",
            "{\"success\":false,\"message\":\"invalid maintenance password\"}"
        );
    }

    try {
        return response(200, "OK", batchControlJson(config, body));
    } catch (const std::exception& ex) {
        return response(400, "Bad Request", "{\"success\":false,\"message\":\"" + jsonEscape(ex.what()) + "\"}");
    }
}

std::string otaStatusJson(const SystemMonitorDirectMaintenanceConfig& config) {
    std::ifstream input(config.otaStatusFile);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        lines.push_back(line);
        if (lines.size() > 100) {
            lines.erase(lines.begin());
        }
    }

    std::ostringstream out;
    out << "{\"ts\":" << nowMs() << ",\"count\":" << lines.size() << ",\"items\":[";
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        out << lines[i];
    }
    out << "]}";
    return out.str();
}

std::string startOtaJob(const SystemMonitorDirectMaintenanceConfig& config, const std::string& body) {
    if (!maintenancePasswordAccepted(config, body)) {
        return response(
            401,
            "Unauthorized",
            "{\"success\":false,\"message\":\"invalid maintenance password\"}"
        );
    }

    auto request = parseOtaRequest(body);
    const auto packageType = jsonString(body, "packageType", "full");
    if (packageType != "full" && packageType != "config") {
        return response(400, "Bad Request", "{\"success\":false,\"message\":\"unsupported packageType\"}");
    }

    auto appConfig = edge_gateway::ConfigLoader::loadAppConfigFromFile(config.otaAppConfigFile);
    edge_gateway::OtaService service(appConfig.ota);
    std::string error;
    if (!service.validateRequest(request, &error)) {
        return response(400, "Bad Request", "{\"success\":false,\"message\":\"" + jsonEscape(error) + "\"}");
    }

    const auto identity = loadIdentity(config.identityConfigFile);
    const auto machineCode = identity.machineCode.empty() ? appConfig.mqtt.clientId : identity.machineCode;
    const auto responseBody = "{\"success\":true,\"jobId\":\"" + jsonEscape(request.jobId) +
        "\",\"packageType\":\"" + jsonEscape(packageType) +
        "\",\"message\":\"direct OTA job accepted\"}";

    std::thread([config, request, machineCode]() {
        try {
            const auto appConfig = edge_gateway::ConfigLoader::loadAppConfigFromFile(config.otaAppConfigFile);
            edge_gateway::OtaService service(appConfig.ota);
            edge_gateway::OtaReply reply;
            edge_gateway::OtaStatus status;
            service.execute(
                request,
                machineCode,
                nowMs(),
                &reply,
                &status,
                [&](const edge_gateway::OtaStatus& item) {
                    appendOtaStatus(config, item);
                }
            );
        } catch (const std::exception& ex) {
            edge_gateway::OtaStatus status;
            status.jobId = request.jobId;
            status.machineCode = machineCode;
            status.stage = "failed";
            status.progress = 0;
            status.downloadedBytes = 0;
            status.totalBytes = request.size;
            status.message = ex.what();
            status.ts = nowMs();
            appendOtaStatus(config, status);
        }
    }).detach();

    return response(202, "Accepted", responseBody);
}

std::string response(int code, const std::string& status, const std::string& body) {
    std::ostringstream out;
    out << "HTTP/1.1 " << code << " " << status << "\r\n"
        << "Content-Type: application/json; charset=utf-8\r\n"
        << "Cache-Control: no-store\r\n"
        << "Connection: close\r\n"
        << "Content-Length: " << body.size() << "\r\n\r\n"
        << body;
    return out.str();
}

std::string handleRequest(
    const SystemMonitorDirectMaintenanceConfig& config,
    const std::string& method,
    const std::string& path,
    const std::string& query,
    const std::string& body
) {
    if (method == "GET" && path == "/api/v1/health") {
        return response(200, "OK", healthJson());
    }
    if (method == "GET" && path == "/api/v1/identity") {
        return response(200, "OK", identityJson(loadIdentity(config.identityConfigFile)));
    }
    if (method == "GET" && path == "/api/v1/auth/state") {
        return response(200, "OK", authStateJson(loadAuthState(config)));
    }
    if (method == "POST" && path == "/api/v1/config/snapshot") {
        return configSnapshotResponse(config, body);
    }
    if (method == "POST" && path == "/api/v1/control/batch") {
        return batchControlResponse(config, body);
    }
    if (method == "GET" && path == "/api/v1/system/status") {
        try {
            return response(200, "OK", systemStatusJson(config));
        } catch (const std::exception& ex) {
            return response(503, "Service Unavailable", "{\"success\":false,\"message\":\"" + jsonEscape(ex.what()) + "\"}");
        }
    }
    if (method == "POST" && path == "/api/v1/diag/execute") {
        if (!maintenancePasswordAccepted(config, body)) {
            return response(401, "Unauthorized", "{\"success\":false,\"message\":\"invalid maintenance password\"}");
        }
        try {
            return response(200, "OK", diagResponseJson(config, body));
        } catch (const std::exception& ex) {
            return response(400, "Bad Request", "{\"success\":false,\"message\":\"" + jsonEscape(ex.what()) + "\"}");
        }
    }
    if (method == "POST" && path == "/api/v1/config/apply") {
        if (!maintenancePasswordAccepted(config, body)) {
            return response(401, "Unauthorized", "{\"success\":false,\"message\":\"invalid maintenance password\"}");
        }
        try {
            return response(200, "OK", configApplyJson(config, body, false));
        } catch (const std::exception& ex) {
            return response(400, "Bad Request", "{\"success\":false,\"message\":\"" + jsonEscape(ex.what()) + "\"}");
        }
    }
    if (method == "POST" && path == "/api/v1/config/delete") {
        return configFileOperationResponse(config, body, "delete");
    }
    if (method == "POST" && path == "/api/v1/config/restore") {
        return configFileOperationResponse(config, body, "restore");
    }
    if (method == "GET" && path == "/api/v1/realtime/points") {
        try {
            return response(200, "OK", realtimePointsJson(config, queryValue(query, "meterCode")));
        } catch (const std::exception& ex) {
            return response(503, "Service Unavailable", "{\"success\":false,\"message\":\"" + jsonEscape(ex.what()) + "\"}");
        }
    }
    if (method == "GET" && path == "/api/v1/telemetry/full") {
        try {
            return response(200, "OK", fullTelemetryJson(config));
        } catch (const std::exception& ex) {
            return response(503, "Service Unavailable", "{\"success\":false,\"message\":\"" + jsonEscape(ex.what()) + "\"}");
        }
    }
    if (method == "GET" && path == "/api/v1/ota/capabilities") {
        return response(
            200,
            "OK",
            "{\"supportedPackageTypes\":[\"config\",\"full\"],\"directUpload\":false,\"message\":\"full OTA job can be started from an artifact URL with the maintenance password\"}"
        );
    }
    if (method == "GET" && path == "/api/v1/ota/status") {
        return response(200, "OK", otaStatusJson(config));
    }
    if (method == "POST" && path == "/api/v1/ota/jobs") {
        try {
            return startOtaJob(config, body);
        } catch (const std::exception& ex) {
            return response(400, "Bad Request", "{\"success\":false,\"message\":\"" + jsonEscape(ex.what()) + "\"}");
        }
    }
    if (method == "POST" && path == "/api/v1/auth/bootstrap-login") {
        const auto providedHash = trim(jsonString(body, "passwordSha256"));
        if (!providedHash.empty() && constantTimeEquals(providedHash, kFixedMaintenancePasswordSha256)) {
            return response(200, "OK", "{\"success\":true,\"message\":\"bootstrap password accepted\",\"sensitiveApiEnabled\":false}");
        }
        return response(401, "Unauthorized", "{\"success\":false,\"message\":\"invalid bootstrap password\"}");
    }
    return response(404, "Not Found", "{\"success\":false,\"message\":\"endpoint not found\"}");
}

bool parseRequest(
    const std::string& raw,
    std::string* method,
    std::string* path,
    std::string* query,
    std::string* body
) {
    const auto lineEnd = raw.find("\r\n");
    if (lineEnd == std::string::npos) {
        return false;
    }
    std::istringstream line(raw.substr(0, lineEnd));
    std::string version;
    line >> *method >> *path >> version;
    const auto queryPos = path->find('?');
    if (queryPos != std::string::npos) {
        *query = path->substr(queryPos + 1);
        *path = path->substr(0, queryPos);
    } else {
        query->clear();
    }
    const auto bodyPos = raw.find("\r\n\r\n");
    *body = bodyPos == std::string::npos ? std::string() : raw.substr(bodyPos + 4);
    return !method->empty() && !path->empty();
}

std::size_t parseContentLength(const std::string& raw) {
    const auto headerEnd = raw.find("\r\n\r\n");
    const auto limit = headerEnd == std::string::npos ? raw.size() : headerEnd;
    std::size_t pos = 0;
    while (pos < limit) {
        const auto lineEnd = raw.find("\r\n", pos);
        const auto end = lineEnd == std::string::npos ? limit : std::min(lineEnd, limit);
        auto line = raw.substr(pos, end - pos);
        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            auto name = line.substr(0, colon);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            if (name == "content-length") {
                const auto value = trim(line.substr(colon + 1));
                char* parsedEnd = nullptr;
                const auto parsed = std::strtoull(value.c_str(), &parsedEnd, 10);
                if (parsedEnd != value.c_str()) {
                    return static_cast<std::size_t>(parsed);
                }
            }
        }
        if (lineEnd == std::string::npos || lineEnd >= limit) {
            break;
        }
        pos = lineEnd + 2;
    }
    return 0;
}

std::string readHttpRequest(int client) {
    std::string raw;
#ifndef _WIN32
    char buffer[4096];
    while (true) {
        const auto readCount = ::recv(client, buffer, sizeof(buffer), 0);
        if (readCount <= 0) {
            break;
        }
        raw.append(buffer, buffer + readCount);
        if (raw.size() > kMaxHttpRequestBytes) {
            throw std::runtime_error("http request is too large");
        }
        const auto bodyPos = raw.find("\r\n\r\n");
        if (bodyPos == std::string::npos) {
            continue;
        }
        const auto contentLength = parseContentLength(raw);
        const auto expectedSize = bodyPos + 4 + contentLength;
        if (expectedSize > kMaxHttpRequestBytes) {
            throw std::runtime_error("http request body is too large");
        }
        if (raw.size() >= expectedSize) {
            break;
        }
    }
#else
    (void)client;
#endif
    return raw;
}

void sendAll(int fd, const std::string& data) {
#ifndef _WIN32
    const char* ptr = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
        const auto written = ::send(fd, ptr, remaining, 0);
        if (written <= 0) {
            return;
        }
        ptr += written;
        remaining -= static_cast<std::size_t>(written);
    }
#else
    (void)fd;
    (void)data;
#endif
}

#ifndef _WIN32
struct ListenSocket {
    int fd = -1;
    std::string host;
};

bool parseIpv4HostOrder(const std::string& value, std::uint32_t* output) {
    in_addr addr{};
    if (::inet_pton(AF_INET, value.c_str(), &addr) != 1) {
        return false;
    }
    if (output != nullptr) {
        *output = ntohl(addr.s_addr);
    }
    return true;
}

bool parseClientCidr(const std::string& value, std::uint32_t* network, std::uint32_t* mask) {
    const auto cidr = trim(value);
    if (cidr.empty()) {
        return false;
    }
    const auto slash = cidr.find('/');
    const auto ipText = slash == std::string::npos ? cidr : cidr.substr(0, slash);
    int prefix = slash == std::string::npos ? 32 : -1;
    if (slash != std::string::npos) {
        const auto prefixText = cidr.substr(slash + 1);
        char* end = nullptr;
        prefix = static_cast<int>(std::strtol(prefixText.c_str(), &end, 10));
        if (end == prefixText.c_str() || prefix < 0 || prefix > 32) {
            return false;
        }
    }

    std::uint32_t ip = 0;
    if (!parseIpv4HostOrder(ipText, &ip)) {
        return false;
    }

    const std::uint32_t parsedMask = prefix == 0
        ? 0
        : static_cast<std::uint32_t>(0xFFFFFFFFu << (32 - prefix));
    if (network != nullptr) {
        *network = ip & parsedMask;
    }
    if (mask != nullptr) {
        *mask = parsedMask;
    }
    return true;
}

bool isClientAllowed(const SystemMonitorDirectMaintenanceConfig& config, const sockaddr_in& clientAddr) {
    if (config.allowedClientCidrs.empty()) {
        return true;
    }

    const auto client = ntohl(clientAddr.sin_addr.s_addr);
    for (const auto& cidr : config.allowedClientCidrs) {
        std::uint32_t network = 0;
        std::uint32_t mask = 0;
        if (!parseClientCidr(cidr, &network, &mask)) {
            continue;
        }
        if ((client & mask) == network) {
            return true;
        }
    }
    return false;
}

std::string sockaddrToString(const sockaddr_in& addr) {
    char buffer[INET_ADDRSTRLEN] = {};
    if (::inet_ntop(AF_INET, &addr.sin_addr, buffer, sizeof(buffer)) == nullptr) {
        return "";
    }
    return buffer;
}

void validateServerConfig(const SystemMonitorDirectMaintenanceConfig& config) {
    if (config.listenHosts.empty()) {
        throw std::runtime_error("listenHosts cannot be empty");
    }
    for (const auto& cidr : config.allowedClientCidrs) {
        std::uint32_t network = 0;
        std::uint32_t mask = 0;
        if (!parseClientCidr(cidr, &network, &mask)) {
            throw std::runtime_error("invalid allowedClientCidrs item: " + cidr);
        }
    }
}

ListenSocket openListenSocket(const std::string& host, int port) {
    const int server = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        throw std::runtime_error("failed to create socket");
    }

    int yes = 1;
    ::setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(server);
        throw std::runtime_error("invalid listenHost: " + host);
    }

    if (::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const std::string message = std::strerror(errno);
        ::close(server);
        throw std::runtime_error("failed to bind " + host + ":" +
            std::to_string(port) + ": " + message);
    }

    if (::listen(server, 16) != 0) {
        const std::string message = std::strerror(errno);
        ::close(server);
        throw std::runtime_error("failed to listen on " + host + ":" +
            std::to_string(port) + ": " + message);
    }

    ListenSocket result;
    result.fd = server;
    result.host = host;
    return result;
}
#endif

int runServer(const SystemMonitorDirectMaintenanceConfig& config) {
#ifdef _WIN32
    (void)config;
    std::cerr << "SystemMonitor direct maintenance is supported on Linux edge devices only" << std::endl;
    return 2;
#else
    validateServerConfig(config);
    std::vector<ListenSocket> servers;
    for (const auto& host : config.listenHosts) {
        servers.push_back(openListenSocket(host, config.listenPort));
        std::cout << "SystemMonitor direct maintenance listening on " << host << ":"
                  << config.listenPort << std::endl;
    }
    if (config.allowedClientCidrs.empty()) {
        std::cout << "SystemMonitor direct maintenance allowed clients: all" << std::endl;
    } else {
        std::cout << "SystemMonitor direct maintenance allowed clients: "
                  << joinStrings(config.allowedClientCidrs, ",") << std::endl;
    }

    while (g_running) {
        fd_set readSet;
        FD_ZERO(&readSet);
        int maxFd = -1;
        for (const auto& server : servers) {
            FD_SET(server.fd, &readSet);
            maxFd = std::max(maxFd, server.fd);
        }
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 500000;
        const int ready = ::select(maxFd + 1, &readSet, nullptr, nullptr, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
            continue;
        }

        for (const auto& server : servers) {
            if (!FD_ISSET(server.fd, &readSet)) {
                continue;
            }
            sockaddr_in clientAddr{};
            socklen_t clientLen = sizeof(clientAddr);
            const int client = ::accept(server.fd, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
            if (client < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }

            if (!isClientAllowed(config, clientAddr)) {
                const auto peer = sockaddrToString(clientAddr);
                std::cerr << "SystemMonitor direct maintenance rejected client " << peer
                          << " on " << server.host << ":" << config.listenPort << std::endl;
                sendAll(client, response(
                    403,
                    "Forbidden",
                    "{\"success\":false,\"message\":\"client is not allowed for maintenance mode\"}"
                ));
                ::close(client);
                continue;
            }

            std::string method;
            std::string path;
            std::string query;
            std::string body;
            std::string out;
            try {
                const auto raw = readHttpRequest(client);
                if (parseRequest(raw, &method, &path, &query, &body)) {
                    out = handleRequest(config, method, path, query, body);
                } else {
                    out = response(400, "Bad Request", "{\"success\":false,\"message\":\"invalid request\"}");
                }
            } catch (const std::exception& ex) {
                out = response(400, "Bad Request", "{\"success\":false,\"message\":\"" + jsonEscape(ex.what()) + "\"}");
            }
            sendAll(client, out);
            ::close(client);
        }
    }

    for (const auto& server : servers) {
        ::close(server.fd);
    }
    return 0;
#endif
}

}  // namespace

namespace edge_gateway {
namespace system_monitor_direct_maintenance {

void requestStop() {
    g_running = false;
}

int runFromConfig(const SystemMonitorConfig::DirectMaintenanceConfig& config) {
    try {
        if (!config.enabled) {
            std::cerr << "SystemMonitor direct maintenance is disabled by monitor-service config" << std::endl;
            return 3;
        }
        g_running = true;
        return runServer(config);
    } catch (const std::exception& ex) {
        std::cerr << "embedded SystemMonitor direct maintenance failed: " << ex.what() << std::endl;
        return 1;
    }
}

}  // namespace system_monitor_direct_maintenance
}  // namespace edge_gateway

