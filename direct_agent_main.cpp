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
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/ota_service.hpp"
#include "edge_gateway/point_store_router.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
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

struct DirectAgentConfig {
    std::string configFile;
    bool enabled = false;
    std::string listenHost = "192.168.100.1";
    int listenPort = 9443;
    std::string identityConfigFile = "/opt/modbus-gateway/config/runtime/device_identity.json";
    std::string appConfigFile = "/opt/modbus-gateway/config/runtime/apps/monitor-service.json";
    std::string otaAppConfigFile = "/opt/modbus-gateway/config/runtime/apps/mqtt-service.json";
    std::string authStateFile = "/opt/modbus-gateway/config/runtime/direct-agent-state.json";
    std::string otaStatusFile = "/opt/modbus-gateway/ota/direct-agent-status.jsonl";
    std::string defaultPasswordSha256 = kFixedMaintenancePasswordSha256;
    int maxRealtimePoints = 2000;
};

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

struct RealtimeContext {
    edge_gateway::AppConfig appConfig;
    std::vector<edge_gateway::DeviceConfig> deviceConfigs;
    std::vector<std::unique_ptr<edge_gateway::MemoryPointStore>> stores;
    edge_gateway::PointStoreRouter router;
};

DirectAgentConfig loadConfig(const std::string& path) {
    DirectAgentConfig config;
    config.configFile = path;
    const auto text = readFile(path);
    if (text.empty()) {
        return config;
    }
    config.enabled = jsonBool(text, "enabled", config.enabled);
    config.listenHost = jsonString(text, "listenHost", config.listenHost);
    config.listenPort = jsonInt(text, "listenPort", config.listenPort);
    config.identityConfigFile = jsonString(text, "identityConfigFile", config.identityConfigFile);
    config.appConfigFile = jsonString(text, "appConfigFile", config.appConfigFile);
    config.otaAppConfigFile = jsonString(text, "otaAppConfigFile", config.otaAppConfigFile);
    config.authStateFile = jsonString(text, "authStateFile", config.authStateFile);
    config.otaStatusFile = jsonString(text, "otaStatusFile", config.otaStatusFile);
    config.defaultPasswordSha256 = kFixedMaintenancePasswordSha256;
    config.maxRealtimePoints = jsonInt(text, "maxRealtimePoints", config.maxRealtimePoints);
    return config;
}

std::unique_ptr<RealtimeContext> createRealtimeContext(const DirectAgentConfig& config) {
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
        context->stores.emplace_back(new MemoryPointStore(name));
        context->router.addStore(name, *context->stores.back());
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

AuthState loadAuthState(const DirectAgentConfig& config) {
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
        << ",\"message\":\"direct agent is running\"}";
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

std::string realtimePointsJson(const DirectAgentConfig& config) {
    auto context = createRealtimeContext(config);
    const auto ts = nowMs();
    const auto values = context->router.getAllLatest(ts);
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

void appendOtaStatus(const DirectAgentConfig& config, const edge_gateway::OtaStatus& status) {
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

bool maintenancePasswordAccepted(const DirectAgentConfig& config, const std::string& body) {
    const auto providedHash = trim(jsonString(body, "passwordSha256"));
    return !providedHash.empty() && constantTimeEquals(providedHash, config.defaultPasswordSha256);
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

std::vector<std::string> collectConfigSnapshotFiles(const DirectAgentConfig& config) {
    std::vector<std::string> files;
    addUniquePath(files, config.configFile);
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

void backupFileIfExists(const std::string& path) {
    if (!isRegularFile(path)) {
        return;
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
}

bool isAllowedConfigPath(const DirectAgentConfig& config, const std::string& path) {
    if (path.empty() || path.find("..") != std::string::npos) {
        return false;
    }
    const auto allowed = collectConfigSnapshotFiles(config);
    return std::find(allowed.begin(), allowed.end(), path) != allowed.end();
}

void validateConfigContentForPath(const DirectAgentConfig& config, const std::string& path, const std::string& content) {
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
        } else if (path == config.configFile || name == "direct-agent.json") {
            (void)loadConfig(tempPath);
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

std::string systemStatusJson(const DirectAgentConfig& config) {
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
        shellCommand = "ps | grep -E 'ModbusRtu|Dlt645Driver|MqttDriver|EventEngine|SystemMonitor|DirectAgent' 2>&1";
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

std::string diagResponseJson(const DirectAgentConfig& config, const std::string& body) {
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

std::string configApplyJson(const DirectAgentConfig& config, const std::string& body, bool requirePassword) {
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

std::string configSnapshotJson(const DirectAgentConfig& config, const std::string& body) {
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

std::string configSnapshotResponse(const DirectAgentConfig& config, const std::string& body) {
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

std::string otaStatusJson(const DirectAgentConfig& config) {
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

std::string startOtaJob(const DirectAgentConfig& config, const std::string& body) {
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
    const DirectAgentConfig& config,
    const std::string& method,
    const std::string& path,
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
    if (method == "GET" && path == "/api/v1/realtime/points") {
        try {
            return response(200, "OK", realtimePointsJson(config));
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
        if (!providedHash.empty() && constantTimeEquals(providedHash, config.defaultPasswordSha256)) {
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
        *path = path->substr(0, queryPos);
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

int runServer(const DirectAgentConfig& config) {
#ifdef _WIN32
    (void)config;
    std::cerr << "DirectAgent is supported on Linux edge devices only" << std::endl;
    return 2;
#else
    const int server = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        throw std::runtime_error("failed to create socket");
    }

    int yes = 1;
    ::setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(config.listenPort));
    if (::inet_pton(AF_INET, config.listenHost.c_str(), &addr.sin_addr) != 1) {
        ::close(server);
        throw std::runtime_error("invalid listenHost: " + config.listenHost);
    }

    if (::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const std::string message = std::strerror(errno);
        ::close(server);
        throw std::runtime_error("failed to bind " + config.listenHost + ":" +
            std::to_string(config.listenPort) + ": " + message);
    }

    if (::listen(server, 16) != 0) {
        const std::string message = std::strerror(errno);
        ::close(server);
        throw std::runtime_error("failed to listen: " + message);
    }

    std::cout << "DirectAgent listening on " << config.listenHost << ":"
              << config.listenPort << std::endl;

    while (g_running) {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        const int client = ::accept(server, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        std::string method;
        std::string path;
        std::string body;
        std::string out;
        try {
            const auto raw = readHttpRequest(client);
            if (parseRequest(raw, &method, &path, &body)) {
                out = handleRequest(config, method, path, body);
            } else {
                out = response(400, "Bad Request", "{\"success\":false,\"message\":\"invalid request\"}");
            }
        } catch (const std::exception& ex) {
            out = response(400, "Bad Request", "{\"success\":false,\"message\":\"" + jsonEscape(ex.what()) + "\"}");
        }
        sendAll(client, out);
        ::close(client);
    }

    ::close(server);
    return 0;
#endif
}

void printUsage() {
    std::cout
        << "Usage:\n"
        << "  DirectAgent --config <path> [--check]\n"
        << "\n"
        << "The service is intended to bind only to the maintenance Ethernet address.\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string configPath = "/opt/modbus-gateway/config/runtime/apps/direct-agent.json";
    bool checkOnly = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "--check") {
            checkOnly = true;
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        }
    }

    try {
        const auto config = loadConfig(configPath);
        if (checkOnly) {
            std::cout << "enabled=" << (config.enabled ? "true" : "false")
                      << " listen=" << config.listenHost << ":" << config.listenPort
                      << " identityConfigFile=" << config.identityConfigFile
                      << " appConfigFile=" << config.appConfigFile
                      << " otaAppConfigFile=" << config.otaAppConfigFile
                      << std::endl;
            return 0;
        }
        if (!config.enabled) {
            std::cerr << "DirectAgent is disabled by config: " << configPath << std::endl;
            return 3;
        }
        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);
        return runServer(config);
    } catch (const std::exception& ex) {
        std::cerr << "DirectAgent failed: " << ex.what() << std::endl;
        return 1;
    }
}
