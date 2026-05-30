#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
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
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

std::atomic<bool> g_running{true};

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
    bool enabled = false;
    std::string listenHost = "192.168.100.1";
    int listenPort = 9443;
    std::string identityConfigFile = "/opt/modbus-gateway/config/runtime/device_identity.json";
    std::string appConfigFile = "/opt/modbus-gateway/config/runtime/apps/monitor-service.json";
    std::string otaAppConfigFile = "/opt/modbus-gateway/config/runtime/apps/mqtt-service.json";
    std::string authStateFile = "/opt/modbus-gateway/config/runtime/direct-agent-state.json";
    std::string otaStatusFile = "/opt/modbus-gateway/ota/direct-agent-status.jsonl";
    std::string defaultPasswordSha256 = "5c2358ee05dbd6bc6d52939f51a45c315533ad9191eecf1631fd788ec8ab76b3";
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

struct RealtimeContext {
    edge_gateway::AppConfig appConfig;
    std::vector<edge_gateway::DeviceConfig> deviceConfigs;
    std::vector<std::unique_ptr<edge_gateway::MemoryPointStore>> stores;
    edge_gateway::PointStoreRouter router;
};

DirectAgentConfig loadConfig(const std::string& path) {
    DirectAgentConfig config;
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
    config.defaultPasswordSha256 = jsonString(text, "defaultPasswordSha256", config.defaultPasswordSha256);
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

        std::string raw;
        char buffer[4096];
        const auto readCount = ::recv(client, buffer, sizeof(buffer), 0);
        if (readCount > 0) {
            raw.assign(buffer, buffer + readCount);
        }

        std::string method;
        std::string path;
        std::string body;
        std::string out;
        if (parseRequest(raw, &method, &path, &body)) {
            out = handleRequest(config, method, path, body);
        } else {
            out = response(400, "Bad Request", "{\"success\":false,\"message\":\"invalid request\"}");
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
