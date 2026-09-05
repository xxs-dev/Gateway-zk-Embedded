#include "edge_gateway/mqtt_forwarder_service.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "edge_gateway/legacy_telemetry_payload.hpp"
#include "edge_gateway/power_control_ownership.hpp"
#include "edge_gateway/process_file_lock.hpp"

namespace edge_gateway {

namespace {

const char kMqttForwarderOwner[] = "mqtt-forwarder";
constexpr std::size_t kMaxControlPayloadBytes = 4096;
// SharedPendingWriteSlot reserves one byte for the terminating NUL.
constexpr std::size_t kMaxControlIdBytes = 63;

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::int64_t currentMonotonicTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

enum class JsonPrimitiveKind {
    String,
    Number,
    Literal
};

struct JsonPrimitive {
    JsonPrimitiveKind kind = JsonPrimitiveKind::Literal;
    std::string text;
    double number = 0.0;
    bool integer = false;
    std::int64_t integerValue = 0;
};

class StrictFlatJsonObject {
public:
    explicit StrictFlatJsonObject(const std::string& text) {
        parse(text);
    }

    const JsonPrimitive* find(const std::string& key) const {
        const auto it = values_.find(key);
        return it == values_.end() ? nullptr : &it->second;
    }

    const std::unordered_map<std::string, JsonPrimitive>& values() const {
        return values_;
    }

private:
    static void skipWhitespace(const std::string& text, std::size_t* cursor) {
        while (*cursor < text.size()) {
            const char ch = text[*cursor];
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
                break;
            }
            ++*cursor;
        }
    }

    static unsigned hexDigit(char ch) {
        if (ch >= '0' && ch <= '9') return static_cast<unsigned>(ch - '0');
        if (ch >= 'a' && ch <= 'f') return static_cast<unsigned>(ch - 'a' + 10);
        if (ch >= 'A' && ch <= 'F') return static_cast<unsigned>(ch - 'A' + 10);
        throw std::invalid_argument("invalid JSON unicode escape");
    }

    static void appendUtf8(std::string* output, unsigned codepoint) {
        if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
            throw std::invalid_argument("JSON surrogate escapes are not supported in control fields");
        }
        if (codepoint <= 0x7FU) {
            output->push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FFU) {
            output->push_back(static_cast<char>(0xC0U | (codepoint >> 6)));
            output->push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else {
            output->push_back(static_cast<char>(0xE0U | (codepoint >> 12)));
            output->push_back(static_cast<char>(0x80U | ((codepoint >> 6) & 0x3FU)));
            output->push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        }
    }

    static std::string parseString(const std::string& text, std::size_t* cursor) {
        if (*cursor >= text.size() || text[*cursor] != '"') {
            throw std::invalid_argument("expected JSON string");
        }
        ++*cursor;
        std::string value;
        while (*cursor < text.size()) {
            const unsigned char ch = static_cast<unsigned char>(text[(*cursor)++]);
            if (ch == '"') {
                return value;
            }
            if (ch < 0x20U) {
                throw std::invalid_argument("control JSON contains an invalid string character");
            }
            if (ch != '\\') {
                value.push_back(static_cast<char>(ch));
                continue;
            }
            if (*cursor >= text.size()) {
                throw std::invalid_argument("unterminated JSON escape");
            }
            const char escaped = text[(*cursor)++];
            switch (escaped) {
                case '"': value.push_back('"'); break;
                case '\\': value.push_back('\\'); break;
                case '/': value.push_back('/'); break;
                case 'b': value.push_back('\b'); break;
                case 'f': value.push_back('\f'); break;
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                case 'u': {
                    if (*cursor + 4 > text.size()) {
                        throw std::invalid_argument("truncated JSON unicode escape");
                    }
                    unsigned codepoint = 0;
                    for (int i = 0; i < 4; ++i) {
                        codepoint = (codepoint << 4) | hexDigit(text[(*cursor)++]);
                    }
                    appendUtf8(&value, codepoint);
                    break;
                }
                default:
                    throw std::invalid_argument("invalid JSON escape");
            }
        }
        throw std::invalid_argument("unterminated JSON string");
    }

    static JsonPrimitive parseNumber(const std::string& text, std::size_t* cursor) {
        const auto begin = *cursor;
        if (text[*cursor] == '-') {
            ++*cursor;
        }
        if (*cursor >= text.size() || text[*cursor] < '0' || text[*cursor] > '9') {
            throw std::invalid_argument("invalid JSON number");
        }
        if (text[*cursor] == '0') {
            ++*cursor;
            if (*cursor < text.size() && text[*cursor] >= '0' && text[*cursor] <= '9') {
                throw std::invalid_argument("invalid JSON number with a leading zero");
            }
        } else {
            while (*cursor < text.size() && text[*cursor] >= '0' && text[*cursor] <= '9') {
                ++*cursor;
            }
        }
        bool integer = true;
        if (*cursor < text.size() && text[*cursor] == '.') {
            integer = false;
            ++*cursor;
            const auto fractionBegin = *cursor;
            while (*cursor < text.size() && text[*cursor] >= '0' && text[*cursor] <= '9') {
                ++*cursor;
            }
            if (fractionBegin == *cursor) {
                throw std::invalid_argument("invalid JSON number fraction");
            }
        }
        if (*cursor < text.size() && (text[*cursor] == 'e' || text[*cursor] == 'E')) {
            integer = false;
            ++*cursor;
            if (*cursor < text.size() && (text[*cursor] == '+' || text[*cursor] == '-')) {
                ++*cursor;
            }
            const auto exponentBegin = *cursor;
            while (*cursor < text.size() && text[*cursor] >= '0' && text[*cursor] <= '9') {
                ++*cursor;
            }
            if (exponentBegin == *cursor) {
                throw std::invalid_argument("invalid JSON number exponent");
            }
        }
        const auto token = text.substr(begin, *cursor - begin);
        char* end = nullptr;
        const double number = std::strtod(token.c_str(), &end);
        if (end != token.c_str() + token.size() || !std::isfinite(number)) {
            throw std::invalid_argument("control JSON number must be finite");
        }
        JsonPrimitive value;
        value.kind = JsonPrimitiveKind::Number;
        value.number = number;
        value.integer = integer &&
            number >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
            number <= static_cast<double>(std::numeric_limits<std::int64_t>::max());
        if (value.integer) {
            value.integerValue = static_cast<std::int64_t>(number);
            value.integer = static_cast<double>(value.integerValue) == number;
        }
        return value;
    }

    static JsonPrimitive parsePrimitive(const std::string& text, std::size_t* cursor) {
        skipWhitespace(text, cursor);
        if (*cursor >= text.size()) {
            throw std::invalid_argument("missing JSON value");
        }
        JsonPrimitive value;
        if (text[*cursor] == '"') {
            value.kind = JsonPrimitiveKind::String;
            value.text = parseString(text, cursor);
            return value;
        }
        if (text[*cursor] == '-' || (text[*cursor] >= '0' && text[*cursor] <= '9')) {
            return parseNumber(text, cursor);
        }
        static const char* literals[] = {"true", "false", "null"};
        for (const auto* literal : literals) {
            const std::string candidate(literal);
            if (text.compare(*cursor, candidate.size(), candidate) == 0) {
                *cursor += candidate.size();
                value.kind = JsonPrimitiveKind::Literal;
                value.text = candidate;
                return value;
            }
        }
        if (text[*cursor] == '{' || text[*cursor] == '[') {
            throw std::invalid_argument("nested control JSON values are not allowed");
        }
        throw std::invalid_argument("invalid JSON primitive");
    }

    void parse(const std::string& text) {
        if (text.size() > kMaxControlPayloadBytes) {
            throw std::invalid_argument("control payload exceeds 4096 bytes");
        }
        std::size_t cursor = 0;
        skipWhitespace(text, &cursor);
        if (cursor >= text.size() || text[cursor++] != '{') {
            throw std::invalid_argument("control payload must be one flat JSON object");
        }
        skipWhitespace(text, &cursor);
        if (cursor < text.size() && text[cursor] == '}') {
            ++cursor;
        } else {
            while (true) {
                skipWhitespace(text, &cursor);
                const auto key = parseString(text, &cursor);
                skipWhitespace(text, &cursor);
                if (cursor >= text.size() || text[cursor++] != ':') {
                    throw std::invalid_argument("control JSON key must be followed by ':'");
                }
                const auto inserted = values_.emplace(key, parsePrimitive(text, &cursor));
                if (!inserted.second) {
                    throw std::invalid_argument("control JSON contains duplicate field " + key);
                }
                skipWhitespace(text, &cursor);
                if (cursor >= text.size()) {
                    throw std::invalid_argument("unterminated control JSON object");
                }
                if (text[cursor] == '}') {
                    ++cursor;
                    break;
                }
                if (text[cursor++] != ',') {
                    throw std::invalid_argument("control JSON fields must be comma-separated");
                }
            }
        }
        skipWhitespace(text, &cursor);
        if (cursor != text.size()) {
            throw std::invalid_argument("control JSON contains trailing content");
        }
    }

    std::unordered_map<std::string, JsonPrimitive> values_;
};

struct ParsedControlCommand {
    std::string id;
    int type = -1;
    bool hasTargetKw = false;
    double targetKw = 0.0;
};

double parseNumericString(const std::string& text, const char* field) {
    if (text.empty() ||
        std::isspace(static_cast<unsigned char>(text.front())) != 0 ||
        std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        throw std::invalid_argument(std::string(field) + " must be numeric");
    }
    std::size_t cursor = 0;
    if (text[cursor] == '+') {
        throw std::invalid_argument(std::string(field) + " must use JSON decimal syntax");
    }
    if (text[cursor] == '-') ++cursor;
    if (cursor >= text.size() || text[cursor] < '0' || text[cursor] > '9') {
        throw std::invalid_argument(std::string(field) + " must use JSON decimal syntax");
    }
    if (text[cursor] == '0') {
        ++cursor;
        if (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
            throw std::invalid_argument(std::string(field) + " must use JSON decimal syntax");
        }
    } else {
        while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') ++cursor;
    }
    if (cursor < text.size() && text[cursor] == '.') {
        ++cursor;
        const auto fractionBegin = cursor;
        while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') ++cursor;
        if (fractionBegin == cursor) {
            throw std::invalid_argument(std::string(field) + " must use JSON decimal syntax");
        }
    }
    if (cursor < text.size() && (text[cursor] == 'e' || text[cursor] == 'E')) {
        ++cursor;
        if (cursor < text.size() && (text[cursor] == '+' || text[cursor] == '-')) ++cursor;
        const auto exponentBegin = cursor;
        while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') ++cursor;
        if (exponentBegin == cursor) {
            throw std::invalid_argument(std::string(field) + " must use JSON decimal syntax");
        }
    }
    if (cursor != text.size()) {
        throw std::invalid_argument(std::string(field) + " must use JSON decimal syntax");
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || !std::isfinite(value)) {
        throw std::invalid_argument(std::string(field) + " must be a finite number");
    }
    return value;
}

ParsedControlCommand parseControlCommand(const std::string& payload) {
    const StrictFlatJsonObject json(payload);
    static const std::unordered_set<std::string> allowedFields = {"id", "type", "target"};
    for (const auto& entry : json.values()) {
        if (allowedFields.find(entry.first) == allowedFields.end()) {
            throw std::invalid_argument("unsupported control field " + entry.first);
        }
    }

    ParsedControlCommand command;
    const auto* id = json.find("id");
    if (id == nullptr || id->kind != JsonPrimitiveKind::String || id->text.empty()) {
        throw std::invalid_argument("control id must be a non-empty string");
    }
    if (id->text.size() > kMaxControlIdBytes) {
        throw std::invalid_argument("control id exceeds 63 bytes");
    }
    command.id = id->text;

    const auto* type = json.find("type");
    if (type == nullptr) {
        throw std::invalid_argument("control type is required");
    }
    if (type->kind == JsonPrimitiveKind::String && (type->text == "0" || type->text == "1")) {
        command.type = type->text == "1" ? 1 : 0;
    } else if (type->kind == JsonPrimitiveKind::Number && type->integer &&
               (type->integerValue == 0 || type->integerValue == 1)) {
        command.type = static_cast<int>(type->integerValue);
    } else {
        throw std::invalid_argument("control type must be 0 or 1");
    }

    const auto* target = json.find("target");
    if (target != nullptr && command.type == 1) {
        if (target->kind == JsonPrimitiveKind::Number) {
            command.targetKw = target->number;
        } else if (target->kind == JsonPrimitiveKind::String) {
            command.targetKw = parseNumericString(target->text, "control target");
        } else {
            throw std::invalid_argument("control target must be numeric");
        }
        command.hasTargetKw = true;
    }
    if (command.type == 1 && !command.hasTargetKw) {
        throw std::invalid_argument("control target is required for remote mode");
    }
    return command;
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

void sleepInterruptibly(const std::atomic<bool>& running, int intervalMs) {
    int remaining = std::max(0, intervalMs);
    while (running.load() && remaining > 0) {
        const int slice = std::min(remaining, 50);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        remaining -= slice;
    }
}

void clearControlTopics(MqttConfig& mqtt) {
    mqtt.telemetryTopic.clear();
    mqtt.realtimeTelemetryTopic.clear();
    mqtt.realtimeRequestTopic.clear();
    mqtt.changeEventTopic.clear();
    mqtt.alarmTopic.clear();
    mqtt.statusTopic.clear();
    mqtt.commandRequestTopic.clear();
    mqtt.commandReplyTopic.clear();
    mqtt.otaRequestTopic.clear();
    mqtt.otaReplyTopic.clear();
    mqtt.otaStatusTopic.clear();
    mqtt.systemMonitorRequestTopic.clear();
    mqtt.systemMonitorReplyTopic.clear();
    mqtt.systemMonitorTelemetryTopic.clear();
    mqtt.systemMonitorAlertTopic.clear();
    mqtt.systemMonitorPointTopic.clear();
    mqtt.diagRequestTopic.clear();
    mqtt.diagReplyTopic.clear();
    mqtt.configPullRequestTopic.clear();
    mqtt.configPullReplyTopic.clear();
    mqtt.configApplyRequestTopic.clear();
    mqtt.configApplyReplyTopic.clear();
    mqtt.configDeleteRequestTopic.clear();
    mqtt.configDeleteReplyTopic.clear();
    mqtt.configRestoreRequestTopic.clear();
    mqtt.configRestoreReplyTopic.clear();
    mqtt.recordingRequestTopic.clear();
    mqtt.recordingReplyTopic.clear();
    mqtt.recordingStatusTopic.clear();
    mqtt.recordingAckTopic.clear();
}

}  // namespace

MqttForwarderService::MqttForwarderService(
    MqttForwardConfig forwardConfig,
    PointStoreRouter& router,
    std::shared_ptr<IMqttDriverPublisher> publisher,
    std::string healthFile
)
    : forwardConfig_(std::move(forwardConfig)),
      router_(router),
      publisher_(std::move(publisher)),
      healthFile_(std::move(healthFile)),
      retryDelayMs_(forwardConfig_.retryMinMs) {
    if (!publisher_) {
        throw std::invalid_argument("mqtt forwarder requires a publisher");
    }
    if (forwardConfig_.payloadFormat != "compactArray" &&
        forwardConfig_.payloadFormat != "object" &&
        forwardConfig_.payloadFormat != "legacy") {
        throw std::invalid_argument("mqttForward.payloadFormat must be compactArray, object or legacy");
    }
    if (forwardConfig_.enabled && forwardConfig_.pointIndexes.empty()) {
        throw std::invalid_argument("mqttForward.pointIndexes must not be empty when enabled");
    }
    std::unordered_set<std::uint32_t> uniqueIndexes;
    for (const auto index : forwardConfig_.pointIndexes) {
        if (!uniqueIndexes.insert(index).second) {
            throw std::invalid_argument(
                "mqttForward.pointIndexes must not contain duplicate index " + std::to_string(index)
            );
        }
        if (forwardConfig_.enabled && router_.routes().find(index) == router_.routes().end()) {
            throw std::invalid_argument(
                "mqttForward.pointIndexes contains unrouted index " + std::to_string(index)
            );
        }
    }
    std::unordered_set<std::uint32_t> mappedIndexes;
    for (const auto& mapping : forwardConfig_.legacyTelemetryPointMappings) {
        if (!mappedIndexes.insert(mapping.index).second) {
            throw std::invalid_argument(
                "mqttForward.legacyTelemetryPointMappings must not contain duplicate index " +
                std::to_string(mapping.index)
            );
        }
        if (uniqueIndexes.find(mapping.index) == uniqueIndexes.end()) {
            throw std::invalid_argument(
                "mqttForward.legacyTelemetryPointMappings index is missing from pointIndexes: " +
                std::to_string(mapping.index)
            );
        }
    }
    if (forwardConfig_.payloadFormat != "legacy" &&
        (forwardConfig_.legacyTelemetryMappedOnly ||
         !forwardConfig_.legacyTelemetryPointMappings.empty())) {
        throw std::invalid_argument(
            "mqttForward legacy mapping options require payloadFormat=legacy"
        );
    }
    if (forwardConfig_.enabled &&
        forwardConfig_.payloadFormat == "legacy" &&
        forwardConfig_.legacyTelemetryMappedOnly &&
        forwardConfig_.legacyTelemetryPointMappings.empty()) {
        throw std::invalid_argument(
            "mqttForward.legacyTelemetryPointMappings must not be empty when legacyTelemetryMappedOnly=true"
        );
    }
    if (forwardConfig_.control.enabled) {
        const auto& control = forwardConfig_.control;
        if (!forwardConfig_.enabled) {
            throw std::invalid_argument("mqttForward.control requires mqttForward.enabled");
        }
        if (control.commandTopic.empty() || control.ownershipFile.empty() ||
            control.scope.empty() || control.sessionId.empty() || control.targets.empty() ||
            control.ownershipIndexes.empty()) {
            throw std::invalid_argument("mqttForward.control is incomplete");
        }
        if (control.leaseTtlMs < 1000 || control.leaseTtlMs > 60000 ||
            control.pollIntervalMs < 10 || control.pollIntervalMs > 1000 ||
            !std::isfinite(control.minTargetKw) || !std::isfinite(control.maxTargetKw) ||
            control.minTargetKw > control.maxTargetKw) {
            throw std::invalid_argument("mqttForward.control timing or target range is invalid");
        }

        std::unordered_set<std::uint32_t> ownershipIndexes;
        for (const auto index : control.ownershipIndexes) {
            if (index == 0 || !ownershipIndexes.insert(index).second) {
                throw std::invalid_argument("mqttForward.control ownership indexes must be unique and positive");
            }
            if (router_.routes().find(index) == router_.routes().end()) {
                throw std::invalid_argument(
                    "mqttForward.control ownership index is unrouted: " + std::to_string(index)
                );
            }
        }

        std::unordered_set<std::uint32_t> targetIndexes;
        std::string targetStore;
        for (const auto& target : control.targets) {
            if (target.index == 0 || !targetIndexes.insert(target.index).second) {
                throw std::invalid_argument("mqttForward.control target indexes must be unique and positive");
            }
            if (!std::isfinite(target.scale) || target.scale == 0.0 ||
                !std::isfinite(target.offset)) {
                throw std::invalid_argument("mqttForward.control target mapping is invalid");
            }
            if (ownershipIndexes.find(target.index) == ownershipIndexes.end()) {
                throw std::invalid_argument(
                    "mqttForward.control ownershipIndexes must include target index " +
                    std::to_string(target.index)
                );
            }
            const auto route = router_.routeByIndex(target.index);
            if (!route) {
                throw std::invalid_argument(
                    "mqttForward.control target index is unrouted: " + std::to_string(target.index)
                );
            }
            if (!route->writable) {
                throw std::invalid_argument(
                    "mqttForward.control target index is not writable: " + std::to_string(target.index)
                );
            }
            if (targetStore.empty()) {
                targetStore = route->sharedMemoryName;
            } else if (targetStore != route->sharedMemoryName) {
                throw std::invalid_argument(
                    "mqttForward.control targets must use one shared-memory store"
                );
            }
        }

        router_.setPowerControlOwnershipFile(control.ownershipFile, kMqttForwarderOwner);
        ownership_.reset(new PowerControlOwnership(control.ownershipFile, kMqttForwarderOwner));
        // A restart must return this forwarder to local mode even when its
        // configured session id changed since the previous process run.
        ownership_->release();
    }
}

MqttForwarderService::~MqttForwarderService() {
    stop();
}

MqttConfig MqttForwarderService::makeTxOnlyMqttConfig(
    const MqttForwardConfig& forwardConfig,
    const std::string& machineCode
) {
    MqttConfig mqtt;
    mqtt.enabled = forwardConfig.enabled;
    mqtt.protocolVersion = forwardConfig.protocolVersion.empty() ? "mqtt3" : forwardConfig.protocolVersion;
    mqtt.broker = forwardConfig.broker;
    mqtt.clientId = forwardConfig.clientId.empty()
        ? (machineCode.empty() ? "forward" : machineCode + "-forward")
        : forwardConfig.clientId;
    mqtt.topicMachineCode = machineCode;
    mqtt.username = forwardConfig.username;
    mqtt.password = forwardConfig.password;
    mqtt.fullTelemetryTopic = forwardConfig.fullTelemetryTopic;
    mqtt.fullTelemetryTopicMachineScoped = forwardConfig.fullTelemetryTopicMachineScoped;
    mqtt.qos = forwardConfig.qos;
    mqtt.controlQos = forwardConfig.qos;
    mqtt.tls = forwardConfig.tls;
    mqtt.offlineBufferEnabled = false;
    clearControlTopics(mqtt);
    return mqtt;
}

MqttConfig MqttForwarderService::makeMqttConfig(
    const MqttForwardConfig& forwardConfig,
    const std::string& machineCode
) {
    auto mqtt = makeTxOnlyMqttConfig(forwardConfig, machineCode);
    if (forwardConfig.control.enabled) {
        mqtt.commandRequestTopic = forwardConfig.control.commandTopic;
        mqtt.commandReplyTopic = forwardConfig.control.replyTopic;
        mqtt.commandRequestTopicMachineScoped = false;
        mqtt.commandReplyTopicMachineScoped = false;
    }
    return mqtt;
}

MqttConfig MqttForwarderService::makePrimaryFullMqttConfig(
    const MqttConfig& primaryConfig,
    const MqttFullUploadWorkerConfig& workerConfig,
    const std::string& machineCode
) {
    auto mqtt = primaryConfig;
    mqtt.topicMachineCode = machineCode;
    const auto baseClientId = machineCode.empty() ? primaryConfig.clientId : machineCode;
    mqtt.clientId = baseClientId + workerConfig.clientIdSuffix;
    mqtt.legacyTelemetryEnabled = false;
    mqtt.legacyTelemetryTopic.clear();
    mqtt.legacyTelemetryPointMappings.clear();
    mqtt.offlineBufferEnabled = false;
    mqtt.fullSnapshotOfflineBufferEnabled = false;
    clearControlTopics(mqtt);
    return mqtt;
}

void MqttForwarderService::start() {
    if (!forwardConfig_.enabled || running_.exchange(true)) {
        return;
    }
    loopThread_ = std::thread([this]() { publishLoop(); });
}

void MqttForwarderService::stop() {
    running_.store(false);
    if (loopThread_.joinable()) {
        loopThread_.join();
    }
    releaseOwnSession();
}

bool MqttForwarderService::isRunning() const {
    return running_.load();
}

void MqttForwarderService::runOnce(std::int64_t nowMs) {
    if (!forwardConfig_.enabled) {
        return;
    }
    if (forwardConfig_.control.enabled) {
        try {
            pollIncomingCommands(nowMs);
        } catch (const std::exception& ex) {
            writeHealth(false, std::string("control poll failed: ") + ex.what(), 0, nowMs);
            std::cerr << "mqtt forwarder control poll failed error=" << ex.what() << std::endl;
        }
    }

    if (!scheduleInitialized_) {
        scheduleInitialized_ = true;
        if (!forwardConfig_.publishOnStart) {
            lastPublishMs_ = nowMs;
        }
    }

    const auto recordFailure = [&](const std::string& error) {
        publishInProgress_ = false;
        lastHealthy_ = false;
        lastValueCount_ = 0;
        lastError_ = error;
        ++consecutiveFailures_;
        const auto retryMs = std::max(forwardConfig_.retryMinMs, retryDelayMs_);
        nextAttemptMs_ = nowMs + retryMs;
        retryDelayMs_ = std::min(
            forwardConfig_.retryMaxMs,
            std::max(forwardConfig_.retryMinMs, retryMs * 2)
        );
        writeHealth(false, error, 0, nowMs);
    };

    bool publishDue = lastPublishMs_ == 0 ||
        nowMs - lastPublishMs_ >= forwardConfig_.intervalMs;
    const bool healthDue = lastHealthMs_ == 0 ||
        nowMs - lastHealthMs_ >= forwardConfig_.healthHeartbeatMs;
    bool connectionProbed = false;

    // The primary worker renews its short lease only after a broker
    // round-trip, independently from the configured full upload interval.
    if (forwardConfig_.primaryFullUpload && !publishDue &&
        (healthDue || (!lastHealthy_ && nextAttemptMs_ <= nowMs))) {
        if (nextAttemptMs_ > nowMs) {
            if (healthDue) {
                writeHealth(false, lastError_, 0, nowMs);
            }
            return;
        }
        const bool wasHealthy = lastHealthy_;
        lastAttemptMs_ = nowMs;
        try {
            publisher_->probeConnection();
            connectionProbed = true;
            nextAttemptMs_ = 0;
            retryDelayMs_ = forwardConfig_.retryMinMs;
            consecutiveFailures_ = 0;
            lastHealthy_ = true;
            lastError_.clear();
            if (!writeHealth(true, {}, lastValueCount_, nowMs)) {
                recordFailure("failed to renew primary full upload lease");
                return;
            }
            if (!wasHealthy) {
                publishDue = true;
            } else {
                return;
            }
        } catch (const std::exception& ex) {
            recordFailure(std::string("mqtt probe failed: ") + ex.what());
            return;
        }
    }

    if (!publishDue) {
        if (healthDue) {
            writeHealth(lastHealthy_, lastError_, lastValueCount_, nowMs);
        }
        return;
    }
    if (nextAttemptMs_ > nowMs) {
        if (healthDue) {
            writeHealth(false, lastError_, 0, nowMs);
        }
        return;
    }

    // DNS lookup and TCP/TLS establishment can block longer than the lease on
    // degraded field networks. Complete that work before taking the shared
    // publish lock so MqttDriver can still provide bounded-time failover.
    if (forwardConfig_.primaryFullUpload && !connectionProbed) {
        lastAttemptMs_ = nowMs;
        try {
            publisher_->probeConnection();
        } catch (const std::exception& ex) {
            recordFailure(std::string("mqtt probe failed: ") + ex.what());
            return;
        }
    }

    std::unique_ptr<ProcessFileLock> publishLock;
    if (forwardConfig_.primaryFullUpload) {
        try {
            publishLock.reset(new ProcessFileLock(forwardConfig_.publishLockFile));
            if (!publishLock->tryAcquire()) {
                recordFailure("primary full upload ownership is busy");
                return;
            }
        } catch (const std::exception& ex) {
            recordFailure(ex.what());
            return;
        }
    }

    lastAttemptMs_ = nowMs;
    publishInProgress_ = forwardConfig_.primaryFullUpload;
    if (publishInProgress_ && !writeHealth(false, "publishing", 0, nowMs)) {
        publishInProgress_ = false;
        recordFailure("failed to persist primary full upload claim");
        return;
    }
    const auto publishStarted = std::chrono::steady_clock::now();
    try {
        const auto values = forwardConfig_.failOnStoreError
            ? router_.getLatestByIndexesStrict(forwardConfig_.pointIndexes, nowMs)
            : router_.getLatestByIndexes(forwardConfig_.pointIndexes, nowMs);
        if (forwardConfig_.payloadFormat == "legacy") {
            publisher_->publishJsonMessage(
                forwardConfig_.fullTelemetryTopic,
                buildLegacyTelemetryPayload(
                    values,
                    forwardConfig_.legacyTelemetryPointMappings,
                    forwardConfig_.legacyTelemetryMappedOnly,
                    nowMs
                )
            );
        } else {
            publisher_->publishFullSnapshot(
                forwardConfig_.fullTelemetryTopic,
                values,
                forwardConfig_.payloadFormat
            );
        }
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - publishStarted
        ).count();
        const auto completedAtMs = nowMs + std::max<std::int64_t>(0, elapsedMs);
        publishInProgress_ = false;
        lastPublishMs_ = completedAtMs;
        nextAttemptMs_ = 0;
        retryDelayMs_ = forwardConfig_.retryMinMs;
        consecutiveFailures_ = 0;
        lastHealthy_ = true;
        lastValueCount_ = values.size();
        lastError_.clear();
        if (!writeHealth(true, {}, values.size(), completedAtMs)) {
            recordFailure("full upload succeeded but active lease could not be persisted");
        }
    } catch (const std::exception& ex) {
        recordFailure(ex.what());
        std::cerr << "mqtt forwarder full snapshot failed error=" << ex.what() << std::endl;
    }
}

void MqttForwarderService::publishLoop() {
    while (running_.load()) {
        runOnce(currentTimeMs());
        int cadenceMs = std::min(
            forwardConfig_.healthHeartbeatMs,
            forwardConfig_.retryMinMs
        );
        if (forwardConfig_.control.enabled) {
            cadenceMs = std::min(cadenceMs, forwardConfig_.control.pollIntervalMs);
        }
        sleepInterruptibly(running_, std::max(1, cadenceMs));
    }
}

bool MqttForwarderService::writeHealth(
    bool healthy,
    const std::string& error,
    std::size_t valueCount,
    std::int64_t nowMs
) const {
    if (healthFile_.empty()) {
        return !forwardConfig_.primaryFullUpload;
    }
    const std::string state = publishInProgress_
        ? "claiming"
        : (healthy ? "active" : "retrying");
    const auto leaseUntilMs = forwardConfig_.primaryFullUpload &&
        (publishInProgress_ || healthy)
        ? nowMs + std::max(100, forwardConfig_.healthLeaseTtlMs)
        : 0;
    const auto monotonicNowMs = currentMonotonicTimeMs();
    const auto leaseUntilMonotonicMs = forwardConfig_.primaryFullUpload &&
        (publishInProgress_ || healthy)
        ? monotonicNowMs + std::max(100, forwardConfig_.healthLeaseTtlMs)
        : 0;
    const std::string payload =
        std::string("{\"healthy\":") + (healthy ? "true" : "false") +
        ",\"state\":\"" + state + "\"" +
        ",\"ts\":" + std::to_string(nowMs) +
        ",\"heartbeatAtMs\":" + std::to_string(nowMs) +
        ",\"leaseUntilMs\":" + std::to_string(leaseUntilMs) +
        ",\"heartbeatMonotonicMs\":" + std::to_string(monotonicNowMs) +
        ",\"leaseUntilMonotonicMs\":" + std::to_string(leaseUntilMonotonicMs) +
        ",\"lastAttemptAtMs\":" + std::to_string(lastAttemptMs_) +
        ",\"lastSuccessAtMs\":" + std::to_string(lastPublishMs_) +
        ",\"nextAttemptAtMs\":" + std::to_string(nextAttemptMs_) +
        ",\"consecutiveFailures\":" + std::to_string(consecutiveFailures_) +
        ",\"primaryFullUpload\":" + (forwardConfig_.primaryFullUpload ? "true" : "false") +
        ",\"machineCode\":\"" + escapeJson(forwardConfig_.primaryMachineCode) + "\"" +
        ",\"clientId\":\"" + escapeJson(forwardConfig_.primaryClientId) + "\"" +
        ",\"topic\":\"" + escapeJson(forwardConfig_.fullTelemetryTopic) + "\"" +
        ",\"valueCount\":" + std::to_string(valueCount) +
        ",\"error\":\"" + escapeJson(error) + "\"}";
    const std::string tempPath = healthFile_ + ".tmp";
    {
        std::ofstream output(tempPath.c_str(), std::ios::binary | std::ios::trunc);
        if (!output) {
            return false;
        }
        output << payload;
        if (!output) {
            return false;
        }
    }
#ifdef _WIN32
    std::remove(healthFile_.c_str());
#endif
    if (std::rename(tempPath.c_str(), healthFile_.c_str()) != 0) {
        std::remove(tempPath.c_str());
        return false;
    }
    lastHealthMs_ = nowMs;
    return true;
}

void MqttForwarderService::pollIncomingCommands(std::int64_t nowMs) {
    const auto incoming = publisher_->pollIncoming(0);
    for (const auto& message : incoming) {
        if (message.type != MqttIncomingType::CommandRequest ||
            message.topic != forwardConfig_.control.commandTopic) {
            continue;
        }
        handleCommandMessage(message, nowMs);
    }
}

void MqttForwarderService::handleCommandMessage(
    const MqttIncomingMessage& message,
    std::int64_t nowMs
) {
    ParsedControlCommand command;
    try {
        command = parseControlCommand(message.payload);
    } catch (const std::exception& ex) {
        const auto active = ownership_->active(nowMs);
        publishControlReply(
            {},
            -1,
            false,
            false,
            active ? "remote" : "local",
            false,
            0.0,
            active ? active->generation : 0,
            ex.what(),
            nowMs
        );
        return;
    }

    const auto initialState = ownership_->active(nowMs);
    if (message.retained) {
        publishControlReply(
            command.id,
            command.type,
            false,
            false,
            initialState ? "remote" : "local",
            command.hasTargetKw,
            command.targetKw,
            initialState ? initialState->generation : 0,
            "retained control commands are rejected",
            nowMs
        );
        return;
    }

    if (command.type == 0) {
        if (initialState &&
            (initialState->owner != kMqttForwarderOwner ||
             initialState->sessionId != forwardConfig_.control.sessionId)) {
            publishControlReply(
                command.id,
                command.type,
                false,
                false,
                "remote",
                false,
                0.0,
                initialState->generation,
                "power control is owned by another controller",
                nowMs
            );
            return;
        }
        const auto releasedGeneration = ownership_->releaseAndAdvance(
            forwardConfig_.control.sessionId
        );
        const bool released = !initialState || releasedGeneration != 0;
        publishControlReply(
            command.id,
            command.type,
            released,
            false,
            released ? "local" : "remote",
            false,
            0.0,
            releasedGeneration != 0
                ? releasedGeneration
                : (initialState ? initialState->generation : 0),
            released ? "local control restored" : "failed to release remote control",
            nowMs
        );
        return;
    }

    if (command.targetKw < forwardConfig_.control.minTargetKw ||
        command.targetKw > forwardConfig_.control.maxTargetKw) {
        publishControlReply(
            command.id,
            command.type,
            false,
            false,
            initialState ? "remote" : "local",
            true,
            command.targetKw,
            initialState ? initialState->generation : 0,
            "control target is outside the configured kW range",
            nowMs
        );
        return;
    }

    const auto receipt = ownership_->lookupReceipt(command.id);
    if (receipt.found) {
        const auto active = ownership_->active(nowMs);
        publishControlReply(
            command.id,
            command.type,
            receipt.accepted,
            true,
            active ? "remote" : "local",
            true,
            command.targetKw,
            active ? active->generation : 0,
            receipt.accepted
                ? "duplicate accepted command ignored"
                : "duplicate incomplete or rejected command ignored",
            nowMs
        );
        return;
    }

    std::vector<PendingWriteCommand> commands;
    commands.reserve(forwardConfig_.control.targets.size());
    for (const auto& target : forwardConfig_.control.targets) {
        PendingWriteCommand pending;
        pending.cmdId = command.id;
        pending.index = target.index;
        pending.value = command.targetKw * target.scale + target.offset;
        if (!std::isfinite(pending.value)) {
            publishControlReply(
                command.id,
                command.type,
                false,
                false,
                initialState ? "remote" : "local",
                true,
                command.targetKw,
                initialState ? initialState->generation : 0,
                "mapped control value is not finite",
                nowMs
            );
            return;
        }
        pending.source = kMqttForwarderOwner;
        pending.ts = nowMs;
        pending.acceptedAt = nowMs;
        pending.highPriority = false;
        commands.push_back(pending);
    }

    const auto takeover = ownership_->acquireOrRenew(
        forwardConfig_.control.scope,
        forwardConfig_.control.sessionId,
        forwardConfig_.control.ownershipIndexes,
        command.id,
        nowMs,
        forwardConfig_.control.leaseTtlMs
    );
    if (takeover.duplicate) {
        publishControlReply(
            command.id,
            command.type,
            takeover.accepted,
            true,
            ownership_->active(nowMs) ? "remote" : "local",
            true,
            command.targetKw,
            takeover.generation,
            takeover.message,
            nowMs
        );
        return;
    }
    if (!takeover.accepted) {
        publishControlReply(
            command.id,
            command.type,
            false,
            false,
            initialState ? "remote" : "local",
            true,
            command.targetKw,
            takeover.generation,
            takeover.message,
            nowMs
        );
        return;
    }

    for (auto& pending : commands) {
        pending.controlGeneration = takeover.generation;
    }
    const auto submitted = router_.submitWriteCommands(commands);
    if (!submitted.accepted) {
        ownership_->recordReceipt(
            forwardConfig_.control.sessionId,
            command.id,
            takeover.generation,
            false
        );
        const auto releasedGeneration = ownership_->releaseAndAdvance(
            forwardConfig_.control.sessionId
        );
        publishControlReply(
            command.id,
            command.type,
            false,
            false,
            "local",
            true,
            command.targetKw,
            releasedGeneration,
            submitted.message.empty() ? "control write was rejected" : submitted.message,
            nowMs
        );
        return;
    }

    if (!ownership_->recordReceipt(
            forwardConfig_.control.sessionId,
            command.id,
            takeover.generation,
            true)) {
        const auto releasedGeneration = ownership_->releaseAndAdvance(
            forwardConfig_.control.sessionId
        );
        publishControlReply(
            command.id,
            command.type,
            false,
            false,
            "local",
            true,
            command.targetKw,
            releasedGeneration,
            "control write queued but accepted receipt could not be persisted; command invalidated",
            nowMs
        );
        return;
    }
    publishControlReply(
        command.id,
        command.type,
        true,
        false,
        "remote",
        true,
        command.targetKw,
        takeover.generation,
        takeover.message.empty() ? "remote control accepted" : takeover.message,
        nowMs
    );
}

void MqttForwarderService::publishControlReply(
    const std::string& id,
    int type,
    bool accepted,
    bool duplicate,
    const std::string& mode,
    bool hasTargetKw,
    double targetKw,
    std::uint32_t generation,
    const std::string& message,
    std::int64_t nowMs
) const {
    if (forwardConfig_.control.replyTopic.empty()) {
        return;
    }
    std::string payload = "{\"id\":\"" + escapeJson(id) + "\"";
    if (type >= 0) {
        payload += ",\"type\":" + std::to_string(type);
    }
    payload += std::string(",\"accepted\":") + (accepted ? "true" : "false") +
        ",\"duplicate\":" + (duplicate ? std::string("true") : std::string("false")) +
        ",\"mode\":\"" + escapeJson(mode) + "\"";
    if (hasTargetKw) {
        payload += ",\"targetKw\":" + std::to_string(targetKw);
    }
    payload += ",\"generation\":" + std::to_string(generation) +
        ",\"message\":\"" + escapeJson(message) + "\"" +
        ",\"ts\":" + std::to_string(nowMs) + "}";
    try {
        publisher_->publishJsonMessage(forwardConfig_.control.replyTopic, payload);
    } catch (const std::exception& ex) {
        writeHealth(false, std::string("control reply failed: ") + ex.what(), 0, nowMs);
        std::cerr << "mqtt forwarder control reply failed error=" << ex.what() << std::endl;
    }
}

void MqttForwarderService::releaseOwnSession() {
    if (ownership_ && forwardConfig_.control.enabled) {
        ownership_->releaseAndAdvance(forwardConfig_.control.sessionId);
    }
}

}  // namespace edge_gateway
