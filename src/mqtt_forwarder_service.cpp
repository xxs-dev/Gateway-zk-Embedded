#include "edge_gateway/mqtt_forwarder_service.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

#include "edge_gateway/legacy_telemetry_payload.hpp"
#include "edge_gateway/mqtt_event_stats.hpp"
#include "edge_gateway/power_control_ownership.hpp"
#include "edge_gateway/process_file_lock.hpp"

namespace edge_gateway {

namespace {

bool replaceFileAtomically(const std::string& temporary, const std::string& target) {
#ifdef _WIN32
    return MoveFileExA(
        temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
    ) != 0;
#else
    return std::rename(temporary.c_str(), target.c_str()) == 0;
#endif
}

const char kMqttForwarderOwner[] = "mqtt-forwarder";
constexpr std::size_t kMaxControlPayloadBytes = 4096;
// SharedPendingWriteSlot reserves one byte for the terminating NUL.
constexpr std::size_t kMaxControlIdBytes = 63;

class Sha256 {
public:
    Sha256() { reset(); }

    void update(const std::uint8_t* data, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) {
            buffer_[bufferSize_++] = data[i];
            bitLength_ += 8;
            if (bufferSize_ == buffer_.size()) {
                transform();
                bufferSize_ = 0;
            }
        }
    }

    std::array<std::uint8_t, 32> finish() {
        const auto originalBitLength = bitLength_;
        buffer_[bufferSize_++] = 0x80;
        if (bufferSize_ > 56) {
            while (bufferSize_ < buffer_.size()) buffer_[bufferSize_++] = 0;
            transform();
            bufferSize_ = 0;
        }
        while (bufferSize_ < 56) buffer_[bufferSize_++] = 0;
        for (int shift = 56; shift >= 0; shift -= 8) {
            buffer_[bufferSize_++] =
                static_cast<std::uint8_t>((originalBitLength >> shift) & 0xffU);
        }
        transform();

        std::array<std::uint8_t, 32> result{};
        for (std::size_t i = 0; i < state_.size(); ++i) {
            result[i * 4] = static_cast<std::uint8_t>(state_[i] >> 24U);
            result[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16U);
            result[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8U);
            result[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
        }
        return result;
    }

private:
    static std::uint32_t rotateRight(std::uint32_t value, int bits) {
        return (value >> bits) | (value << (32 - bits));
    }

    void reset() {
        state_ = {{
            0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
            0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
        }};
        bufferSize_ = 0;
        bitLength_ = 0;
    }

    void transform() {
        static const std::uint32_t constants[64] = {
            0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
            0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
            0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
            0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
            0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
            0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
            0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
            0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
        };
        std::uint32_t words[64]{};
        for (int i = 0; i < 16; ++i) {
            const auto offset = static_cast<std::size_t>(i * 4);
            words[i] = (static_cast<std::uint32_t>(buffer_[offset]) << 24U) |
                (static_cast<std::uint32_t>(buffer_[offset + 1]) << 16U) |
                (static_cast<std::uint32_t>(buffer_[offset + 2]) << 8U) |
                static_cast<std::uint32_t>(buffer_[offset + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const auto s0 = rotateRight(words[i - 15], 7) ^
                rotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3U);
            const auto s1 = rotateRight(words[i - 2], 17) ^
                rotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10U);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        auto a = state_[0]; auto b = state_[1]; auto c = state_[2]; auto d = state_[3];
        auto e = state_[4]; auto f = state_[5]; auto g = state_[6]; auto h = state_[7];
        for (int i = 0; i < 64; ++i) {
            const auto s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
            const auto choice = (e & f) ^ (~e & g);
            const auto temp1 = h + s1 + choice + constants[i] + words[i];
            const auto s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = s0 + majority;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t bufferSize_ = 0;
    std::uint64_t bitLength_ = 0;
};

std::string sha256Hex(const std::string& value) {
    Sha256 hash;
    hash.update(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
    const auto digest = hash.finish();
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : digest) {
        out << std::setw(2) << static_cast<unsigned>(byte);
    }
    return out.str();
}

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

bool readJsonBool(const StrictFlatJsonObject& json, const char* key, bool* value) {
    const auto* field = json.find(key);
    if (field == nullptr || field->kind != JsonPrimitiveKind::Literal ||
        (field->text != "true" && field->text != "false")) {
        return false;
    }
    *value = field->text == "true";
    return true;
}

bool readJsonInt64(const StrictFlatJsonObject& json, const char* key, std::int64_t* value) {
    const auto* field = json.find(key);
    if (field == nullptr || field->kind != JsonPrimitiveKind::Number || !field->integer) {
        return false;
    }
    *value = field->integerValue;
    return true;
}

bool readJsonString(const StrictFlatJsonObject& json, const char* key, std::string* value) {
    const auto* field = json.find(key);
    if (field == nullptr || field->kind != JsonPrimitiveKind::String) {
        return false;
    }
    *value = field->text;
    return true;
}

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

std::string controlCommandFingerprint(
    const ParsedControlCommand& command,
    const MqttForwardControlConfig& config
) {
    auto ownershipIndexes = config.ownershipIndexes;
    std::sort(ownershipIndexes.begin(), ownershipIndexes.end());
    auto targets = config.targets;
    std::sort(targets.begin(), targets.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.index < rhs.index;
    });

    std::ostringstream normalized;
    normalized << "mqtt-forward-control:v1|type=" << command.type
               << "|target=" << std::hexfloat << command.targetKw
               << "|scope=" << config.scope << "|ownership=";
    for (const auto index : ownershipIndexes) {
        normalized << index << ',';
    }
    normalized << "|targets=";
    for (const auto& target : targets) {
        normalized << target.index << ':' << std::hexfloat << target.scale
                   << ':' << target.offset << ',';
    }
    return sha256Hex(normalized.str());
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
    std::string healthFile,
    std::unique_ptr<MqttEventOutbox> eventOutbox,
    std::string eventOutboxPath,
    std::string eventReplayLockFile,
    std::string eventDelegationReadyFile,
    std::unique_ptr<IEventStatsSource> eventStats,
    MqttEventReplayFactory eventReplayFactory,
    EventStoreIdentity eventStatsIdentity
)
    : forwardConfig_(std::move(forwardConfig)),
      router_(router),
      publisher_(std::move(publisher)),
      healthFile_(std::move(healthFile)),
      eventOutbox_(std::move(eventOutbox)),
      eventStats_(std::move(eventStats)),
      eventStatsIdentity_(std::move(eventStatsIdentity)),
      eventReplayFactory_(std::move(eventReplayFactory)),
      eventOutboxPath_(std::move(eventOutboxPath)),
      eventReplayLockFile_(std::move(eventReplayLockFile)),
      eventDelegationReadyFile_(std::move(eventDelegationReadyFile)),
      retryDelayMs_(forwardConfig_.retryMinMs),
      eventStatsAwaitingDelegation_(forwardConfig_.primaryFullUpload) {
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
    if (eventReplayFactory_ && (eventOutbox_ || eventStatsIdentity_.storeId.empty() ||
        eventStatsIdentity_.configGeneration.empty())) {
        throw std::invalid_argument("IPC replay requires exact statistics identity and no legacy Outbox");
    }
    if (forwardConfig_.events.enabled) {
        if (forwardConfig_.qos < 1) {
            throw std::invalid_argument("mqttForward.events requires qos 1 or 2");
        }
        if ((!eventOutbox_ && !eventReplayFactory_) || (eventOutbox_ && eventReplayFactory_)) {
            throw std::invalid_argument("mqttForward.events requires an event Outbox");
        }
        if (forwardConfig_.events.targetId.empty()) {
            throw std::invalid_argument("mqttForward.events.targetId must not be empty");
        }
        if (forwardConfig_.events.changeTopic.empty() && forwardConfig_.events.alarmTopic.empty()) {
            throw std::invalid_argument("mqttForward.events requires changeTopic or alarmTopic");
        }
        if (forwardConfig_.primaryFullUpload &&
            (forwardConfig_.events.targetId != "main" || eventReplayLockFile_.empty() ||
             eventDelegationReadyFile_.empty() || eventOutboxPath_.empty())) {
            throw std::invalid_argument("primary MQTT event forwarding ownership is incomplete");
        }
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
        if (forwardConfig_.qos < 1 || forwardConfig_.qos > 2) {
            throw std::invalid_argument("mqttForward.control requires qos 1 or 2");
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
        controlResultStore_.reset(new MqttControlResultStore(
            MqttControlResultStore::defaultPathForOwnershipFile(control.ownershipFile)
        ));
        for (const auto& record : controlResultStore_->loadPending()) {
            PendingControlResult pending;
            pending.id = record.id;
            pending.fingerprint = record.fingerprint;
            pending.type = record.type;
            pending.targetKw = record.targetKw;
            pending.generation = record.generation;
            pending.acceptedAtMs = record.acceptedAtMs;
            pending.deadlineMs = record.deadlineMs;
            pending.routes = record.routes;
            pending.submitted = record.submitted;
            pendingControlResults_.push_back(std::move(pending));
        }
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
    if (forwardConfig.events.enabled) {
        mqtt.changeEventTopic = forwardConfig.events.changeTopic;
        mqtt.changeEventTopicMachineScoped = forwardConfig.events.changeTopicMachineScoped;
        mqtt.alarmTopic = forwardConfig.events.alarmTopic;
        mqtt.alarmTopicMachineScoped = forwardConfig.events.alarmTopicMachineScoped;
    }
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
    if (workerConfig.eventForwardingEnabled) {
        mqtt.changeEventTopic = primaryConfig.changeEventTopic;
        mqtt.changeEventTopicMachineScoped = primaryConfig.changeEventTopicMachineScoped;
        mqtt.alarmTopic = primaryConfig.alarmTopic;
        mqtt.alarmTopicMachineScoped = primaryConfig.alarmTopicMachineScoped;
    }
    return mqtt;
}

void MqttForwarderService::start() {
    if (running_.load()) return;
    if (eventReplay_.active()) throw std::logic_error("drain manual IPC replay before starting the worker");
    if (!forwardConfig_.enabled || running_.exchange(true)) {
        return;
    }
    loopThread_ = std::thread([this]() { publishLoop(); });
}

void MqttForwarderService::stop() {
    eventReplayStopping_.store(true);
    running_.store(false);
    if (loopThread_.joinable()) {
        loopThread_.join();
    }
    drainIpcEvents();
    eventReplayStopping_.store(false);
    eventDelegationActive_ = false;
    eventHeartbeatMonotonicMs_ = 0;
    eventLeaseUntilMonotonicMs_ = 0;
    lastEventReplayMs_ = 0;
    releaseOwnSession();
}

bool MqttForwarderService::isRunning() const {
    return running_.load();
}

bool MqttForwarderService::eventDelegationReady(std::int64_t nowMs) const {
    if (!forwardConfig_.primaryFullUpload) {
        return true;
    }
    if (eventDelegationReadyFile_.empty()) {
        return false;
    }

    try {
        // The ready file is paired with a process-lifetime lock. A stale file
        // from a stopped or downgraded Driver must never transfer ownership.
        ProcessFileLock liveLock(eventDelegationReadyFile_ + ".lock");
        if (liveLock.tryAcquire()) {
            return false;
        }

        std::ifstream input(eventDelegationReadyFile_.c_str(), std::ios::in | std::ios::binary);
        if (!input.is_open()) {
            return false;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        const auto text = buffer.str();
        if (text.empty() || text.size() > 16 * 1024) {
            return false;
        }
        const StrictFlatJsonObject ready(text);
        std::string replayBackend;
        if (ready.find("eventReplayBackend")) {
            if (!readJsonString(ready, "eventReplayBackend", &replayBackend) ||
                replayBackend != (eventReplayFactory_ ? "ipc-lab" : "legacy")) return false;
        } else if (eventReplayFactory_) return false;
        if (eventReplayFactory_) {
            std::string storeId, generation;
            if (!readJsonString(ready, "eventStoreId", &storeId) || storeId != eventStatsIdentity_.storeId ||
                !readJsonString(ready, "eventStoreConfigGeneration", &generation) ||
                generation != eventStatsIdentity_.configGeneration) return false;
        }
        std::int64_t dataPlaneVersion = 0;
        std::int64_t heartbeatMonotonicMs = 0;
        std::int64_t leaseUntilMonotonicMs = 0;
        bool externalOutbox = false;
        bool eventFallbackCapable = false;
        bool changeMachineScoped = true;
        bool alarmMachineScoped = true;
        std::string machineCode;
        std::string outboxPath;
        std::string changeTopic;
        std::string alarmTopic;
        std::string replayLockFile;
        if (!readJsonInt64(ready, "dataPlaneVersion", &dataPlaneVersion) ||
            dataPlaneVersion < 2 ||
            !readJsonBool(ready, "externalOutbox", &externalOutbox) || !externalOutbox ||
            !readJsonBool(ready, "eventFallbackCapable", &eventFallbackCapable) ||
            !eventFallbackCapable ||
            !readJsonString(ready, "machineCode", &machineCode) ||
            !readJsonString(ready, "outboxPath", &outboxPath) ||
            !readJsonString(ready, "changeTopic", &changeTopic) ||
            !readJsonString(ready, "alarmTopic", &alarmTopic) ||
            !readJsonBool(ready, "changeTopicMachineScoped", &changeMachineScoped) ||
            !readJsonBool(ready, "alarmTopicMachineScoped", &alarmMachineScoped) ||
            !readJsonString(ready, "eventReplayLockFile", &replayLockFile) ||
            !readJsonInt64(ready, "heartbeatMonotonicMs", &heartbeatMonotonicMs) ||
            !readJsonInt64(ready, "leaseUntilMonotonicMs", &leaseUntilMonotonicMs)) {
            return false;
        }
        if (machineCode != forwardConfig_.primaryMachineCode ||
            outboxPath != eventOutboxPath_ ||
            changeTopic != forwardConfig_.events.changeTopic ||
            alarmTopic != forwardConfig_.events.alarmTopic ||
            changeMachineScoped != forwardConfig_.events.changeTopicMachineScoped ||
            alarmMachineScoped != forwardConfig_.events.alarmTopicMachineScoped ||
            replayLockFile != eventReplayLockFile_) {
            return false;
        }
        const auto monotonicNowMs = currentMonotonicTimeMs();
        const auto toleranceMs = std::max(100, forwardConfig_.healthLeaseTtlMs);
        return leaseUntilMonotonicMs >= monotonicNowMs &&
            heartbeatMonotonicMs <= monotonicNowMs + toleranceMs &&
            monotonicNowMs - heartbeatMonotonicMs <= toleranceMs && nowMs > 0;
    } catch (...) {
        return false;
    }
}

void MqttForwarderService::replayIpcEventsIfDue(std::int64_t nowMs) {
    if (!forwardConfig_.events.enabled) return;
    const auto intervalMs = std::max(10, forwardConfig_.events.replayIntervalMs);
    const bool due = lastEventReplayMs_ == 0 || nowMs < lastEventReplayMs_ || nowMs - lastEventReplayMs_ >= intervalMs;
    const auto delegated = [this] {
        return !eventReplayStopping_.load() && eventDelegationReady(currentTimeMs());
    };
    const bool hasDelegation = delegated();
    if (!due && hasDelegation) return;
    if (due) lastEventReplayMs_ = nowMs;
    eventDelegationActive_ = false;
    eventHeartbeatMonotonicMs_ = 0;
    eventLeaseUntilMonotonicMs_ = 0;
    eventStatsAwaitingDelegation_ = forwardConfig_.primaryFullUpload;
    MqttEventReplayRequest request;
    request.lane = MqttEventReplayLane::ForwardEvents;
    request.targetId = forwardConfig_.events.targetId;
    if (forwardConfig_.primaryFullUpload || !forwardConfig_.events.alarmTopic.empty()) request.includeTypes.push_back("alarm");
    if (forwardConfig_.primaryFullUpload || !forwardConfig_.events.changeTopic.empty()) request.includeTypes.push_back("change");
    request.maxBytes = std::min<std::size_t>(32768, forwardConfig_.events.replayMaxBytes);
    request.maxMessages = forwardConfig_.control.enabled ? 8 : 16;
    request.authorized = [this, delegated] {
        return delegated() && (!forwardConfig_.primaryFullUpload || ipcEventReplayLock_);
    };
    try {
        if (!eventReplay_.active()) {
            if (!hasDelegation) {
                eventLastError_ = "mqtt driver event delegation is not ready";
                eventOutboxHealthy_ = true;
                return;
            }
            if (forwardConfig_.primaryFullUpload) {
                ipcEventReplayLock_.reset(new ProcessFileLock(eventReplayLockFile_));
                if (!ipcEventReplayLock_->tryAcquire()) {
                    ipcEventReplayLock_.reset();
                    eventLastError_ = "primary event replay ownership is busy";
                    eventOutboxHealthy_ = true;
                    return;
                }
            }
            if (!request.authorized()) {
                ipcEventReplayLock_.reset();
                eventLastError_ = "mqtt driver event delegation expired";
                eventOutboxHealthy_ = true;
                return;
            }
        }
        const bool drainOnly = !request.authorized();
        eventStatsAwaitingDelegation_ = forwardConfig_.primaryFullUpload && drainOnly;
        const auto result = eventReplay_.run(eventReplayFactory_, request, drainOnly);
        if (result.ackedCount) eventLastAckAtMs_ = currentTimeMs();
        eventOutboxHealthy_ = result.healthy && !result.pending && result.error.empty();
        eventLastError_ = result.error;
        // An IPC success while draining cannot confer a new event health lease.
        if (eventOutboxHealthy_ && !drainOnly && request.authorized()) {
            eventDelegationActive_ = true;
            eventHeartbeatMonotonicMs_ = currentMonotonicTimeMs();
            eventLeaseUntilMonotonicMs_ = eventHeartbeatMonotonicMs_ + std::max(100, forwardConfig_.healthLeaseTtlMs);
        } else if (eventLastError_.empty()) {
            eventLastError_ = "IPC event replay incomplete or delegation lost";
        }
    } catch (const std::exception& ex) {
        eventOutboxHealthy_ = false;
        eventLastError_ = ex.what();
    } catch (...) {
        eventOutboxHealthy_ = false;
        eventLastError_ = "IPC event replay failed";
    }
    if (!eventReplay_.active()) ipcEventReplayLock_.reset();
}

void MqttForwarderService::drainIpcEvents() {
    while (eventReplay_.active()) {
        replayIpcEventsIfDue(currentTimeMs());
        if (eventReplay_.active()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ipcEventReplayLock_.reset();
}

void MqttForwarderService::replayEventsIfDue(std::int64_t nowMs) {
    if (eventReplayFactory_) {
        replayIpcEventsIfDue(nowMs);
        return;
    }
    if (!forwardConfig_.events.enabled || !eventOutbox_) {
        return;
    }
    const auto intervalMs = std::max(10, forwardConfig_.events.replayIntervalMs);
    if (lastEventReplayMs_ > 0 && nowMs >= lastEventReplayMs_ &&
        nowMs - lastEventReplayMs_ < intervalMs) {
        return;
    }
    lastEventReplayMs_ = nowMs;
    eventDelegationActive_ = false;
    eventHeartbeatMonotonicMs_ = 0;
    eventLeaseUntilMonotonicMs_ = 0;
    eventStatsAwaitingDelegation_ = forwardConfig_.primaryFullUpload;

    try {
        if (forwardConfig_.primaryFullUpload && !eventDelegationReady(nowMs)) {
            eventLastError_ = "mqtt driver event delegation is not ready";
            eventOutboxHealthy_ = true;
            return;
        }

        std::unique_ptr<ProcessFileLock> eventLock;
        if (forwardConfig_.primaryFullUpload) {
            eventLock.reset(new ProcessFileLock(eventReplayLockFile_));
            if (!eventLock->tryAcquire()) {
                eventLastError_ = "primary event replay ownership is busy";
                eventOutboxHealthy_ = true;
                return;
            }
            // Driver may have renewed its capability while the event lock was
            // contended. Recheck before querying or publishing any row.
            if (!eventDelegationReady(currentTimeMs())) {
                eventLastError_ = "mqtt driver event delegation expired";
                eventOutboxHealthy_ = true;
                return;
            }
        }

        eventStatsAwaitingDelegation_ = false;
        MqttEventOutbox::EventTypeFilter eventTypes;
        if (!forwardConfig_.events.alarmTopic.empty()) {
            eventTypes.include.push_back("alarm");
        }
        if (!forwardConfig_.events.changeTopic.empty()) {
            eventTypes.include.push_back("change");
        }
        const auto stats = eventOutbox_->replayBatchWithStats(
            forwardConfig_.events.targetId,
            eventTypes,
            forwardConfig_.events.replayMaxBytes,
            forwardConfig_.control.enabled ? 8U : 0U,
            [this](const std::vector<MqttEventOutbox::ReplayMessage>& messages) {
                std::vector<MqttJsonMessage> publishes;
                publishes.reserve(messages.size());
                for (const auto& message : messages) {
                    publishes.push_back(MqttJsonMessage{message.topic, message.payload});
                }
                publisher_->publishReliableJsonMessages(publishes);
            }
        );
        eventOutbox_->cleanupIfDue(nowMs);
        eventOutboxHealthy_ = true;
        eventDelegationActive_ = true;
        eventHeartbeatMonotonicMs_ = currentMonotonicTimeMs();
        eventLeaseUntilMonotonicMs_ = eventHeartbeatMonotonicMs_ +
            std::max(100, forwardConfig_.healthLeaseTtlMs);
        eventLastError_.clear();
        if (stats.count > 0) {
            eventLastAckAtMs_ = currentTimeMs();
        }
    } catch (const std::exception& ex) {
        eventOutboxHealthy_ = false;
        eventDelegationActive_ = false;
        eventLastError_ = ex.what();
        std::cerr << "mqtt forwarder event replay failed target="
                  << forwardConfig_.events.targetId
                  << " error=" << ex.what() << std::endl;
    }
}

void MqttForwarderService::runOnce(std::int64_t nowMs) {
    if (!forwardConfig_.enabled) {
        return;
    }
    if (forwardConfig_.control.enabled) {
        try {
            controlResultStore_->cleanupIfDue(nowMs);
            pollIncomingCommands(nowMs);
            processPendingControlResults(nowMs);
            publishUndeliveredControlResults(nowMs);
        } catch (const std::exception& ex) {
            writeHealth(false, std::string("control poll failed: ") + ex.what(), 0, nowMs);
            std::cerr << "mqtt forwarder control poll failed error=" << ex.what() << std::endl;
        }
    }
    replayEventsIfDue(nowMs);

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
        if (forwardConfig_.events.enabled) {
            cadenceMs = std::min(cadenceMs, forwardConfig_.events.replayIntervalMs);
        }
        sleepInterruptibly(running_, std::max(1, cadenceMs));
    }
    drainIpcEvents();
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
    // Full snapshot publication and event replay use independent delivery
    // paths. A transient Full claim/failure must not revoke a healthy event
    // delegation and trigger the Driver fallback to contend for the outbox.
    // Only a completed event replay renews this lease. Full or diagnostic
    // health writes must neither revoke a live lease nor revive an expired one.
    const bool eventForwarding = forwardConfig_.events.enabled &&
        eventDelegationActive_ && eventOutboxHealthy_ &&
        eventHeartbeatMonotonicMs_ > 0 &&
        eventHeartbeatMonotonicMs_ <= monotonicNowMs &&
        eventLeaseUntilMonotonicMs_ > monotonicNowMs;
    const auto eventStats = readMqttEventStats(
        eventStats_.get(),
        mqttForwarderStatsQuery(forwardConfig_.events, eventStatsAwaitingDelegation_),
        eventStatsIdentity_
    );
    const std::string payload =
        std::string("{\"healthy\":") + (healthy ? "true" : "false") +
        ",\"dataPlaneVersion\":2" +
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
        ",\"eventForwarding\":" + (eventForwarding ? "true" : "false") +
        ",\"eventReplayBackend\":\"" + (eventReplayFactory_ ? "ipc-lab" : "legacy") + "\"" +
        ",\"eventStoreId\":\"" + escapeJson(eventStatsIdentity_.storeId) + "\"" +
        ",\"eventStoreConfigGeneration\":\"" + escapeJson(eventStatsIdentity_.configGeneration) + "\"" +
        ",\"eventOutboxHealthy\":" + (eventOutboxHealthy_ ? "true" : "false") +
        ",\"eventLeaseVersion\":1" +
        ",\"eventHeartbeatMonotonicMs\":" + std::to_string(eventHeartbeatMonotonicMs_) +
        ",\"eventLeaseUntilMonotonicMs\":" + std::to_string(eventLeaseUntilMonotonicMs_) +
        mqttEventStatsHealthFields(eventStats) +
        ",\"eventLastAckAtMs\":" + std::to_string(eventLastAckAtMs_) +
        ",\"eventTargetId\":\"" + escapeJson(forwardConfig_.events.targetId) + "\"" +
        ",\"eventOutboxPath\":\"" + escapeJson(eventOutboxPath_) + "\"" +
        ",\"eventReplayLockFile\":\"" + escapeJson(eventReplayLockFile_) + "\"" +
        ",\"changeTopic\":\"" + escapeJson(forwardConfig_.events.changeTopic) + "\"" +
        ",\"alarmTopic\":\"" + escapeJson(forwardConfig_.events.alarmTopic) + "\"" +
        ",\"changeTopicMachineScoped\":" +
            (forwardConfig_.events.changeTopicMachineScoped ? "true" : "false") +
        ",\"alarmTopicMachineScoped\":" +
            (forwardConfig_.events.alarmTopicMachineScoped ? "true" : "false") +
        ",\"eventLastError\":\"" + escapeJson(eventLastError_) + "\"" +
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
    if (!replaceFileAtomically(tempPath, healthFile_)) {
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

    const auto commandFingerprint = controlCommandFingerprint(
        command,
        forwardConfig_.control
    );
    const auto durableResult = controlResultStore_->find(command.id);
    if (durableResult) {
        const bool payloadMatches = durableResult->fingerprint == commandFingerprint;
        if (!payloadMatches) {
            const auto active = ownership_->active(nowMs);
            publishControlReply(
                command.id,
                command.type,
                false,
                true,
                active ? "remote" : "local",
                command.hasTargetKw,
                command.targetKw,
                active ? active->generation : durableResult->generation,
                "command id payload mismatch",
                nowMs
            );
        } else if (!durableResult->finalPayload.empty()) {
            (void)publishStoredControlResult(*durableResult, nowMs);
        } else if (durableResult->submitted) {
            const auto active = ownership_->active(nowMs);
            publishControlReply(
                command.id,
                command.type,
                true,
                true,
                active ? "remote" : "local",
                command.hasTargetKw,
                command.targetKw,
                active ? active->generation : durableResult->generation,
                "duplicate accepted command pending device result",
                nowMs
            );
        } else {
            const auto active = ownership_->active(nowMs);
            publishControlReply(
                command.id,
                command.type,
                false,
                true,
                active ? "remote" : "local",
                command.hasTargetKw,
                command.targetKw,
                active ? active->generation : durableResult->generation,
                "control submission was not confirmed before restart",
                nowMs
            );
        }
        return;
    }
    const auto receipt = ownership_->lookupReceipt(command.id);
    if (receipt.found) {
        const auto active = ownership_->active(nowMs);
        const bool payloadMatches = receipt.fingerprint == commandFingerprint;
        publishControlReply(
            command.id,
            command.type,
            payloadMatches && receipt.accepted,
            true,
            active ? "remote" : "local",
            command.hasTargetKw,
            command.targetKw,
            active ? active->generation : 0,
            !payloadMatches
                ? "command id payload mismatch"
                : (receipt.accepted
                    ? "duplicate accepted command ignored"
                    : "duplicate incomplete or rejected command ignored"),
            nowMs
        );
        return;
    }

    if (command.type == 0) {
        if (initialState &&
            (initialState->owner != kMqttForwarderOwner ||
             initialState->sessionId != forwardConfig_.control.sessionId)) {
            ownership_->recordDetachedReceipt(command.id, false, commandFingerprint);
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
        if (!ownership_->recordDetachedReceipt(command.id, false, commandFingerprint)) {
            publishControlReply(
                command.id,
                command.type,
                false,
                false,
                initialState ? "remote" : "local",
                false,
                0.0,
                initialState ? initialState->generation : 0,
                "failed to reserve control command id",
                nowMs
            );
            return;
        }
        const auto releasedGeneration = ownership_->releaseAndAdvance(
            forwardConfig_.control.sessionId
        );
        const bool released = !initialState || releasedGeneration != 0;
        const bool receiptRecorded = ownership_->recordDetachedReceipt(
            command.id,
            released,
            commandFingerprint
        );
        publishControlReply(
            command.id,
            command.type,
            released && receiptRecorded,
            false,
            released ? "local" : "remote",
            false,
            0.0,
            releasedGeneration != 0
                ? releasedGeneration
                : (initialState ? initialState->generation : 0),
            !released
                ? "failed to release remote control"
                : (receiptRecorded
                    ? "local control restored"
                    : "local control restored but command receipt finalization failed"),
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
        forwardConfig_.control.leaseTtlMs,
        commandFingerprint
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

    PendingControlResult pendingResult;
    pendingResult.id = command.id;
    pendingResult.fingerprint = commandFingerprint;
    pendingResult.type = command.type;
    pendingResult.targetKw = command.targetKw;
    pendingResult.generation = takeover.generation;
    pendingResult.acceptedAtMs = nowMs;
    pendingResult.deadlineMs = nowMs + std::max(1000, forwardConfig_.control.leaseTtlMs);
    pendingResult.routes.reserve(forwardConfig_.control.targets.size());
    for (const auto& target : forwardConfig_.control.targets) {
        const auto route = router_.routeByIndex(target.index);
        if (!route) {
            ownership_->recordReceipt(
                forwardConfig_.control.sessionId,
                command.id,
                takeover.generation,
                false,
                commandFingerprint
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
                "control result route is unavailable",
                nowMs
            );
            return;
        }
        pendingResult.routes.push_back(*route);
    }

    MqttControlResultRecord durablePending;
    durablePending.id = pendingResult.id;
    durablePending.fingerprint = pendingResult.fingerprint;
    durablePending.type = pendingResult.type;
    durablePending.targetKw = pendingResult.targetKw;
    durablePending.generation = pendingResult.generation;
    durablePending.acceptedAtMs = pendingResult.acceptedAtMs;
    durablePending.deadlineMs = pendingResult.deadlineMs;
    durablePending.routes = pendingResult.routes;
    MqttControlReserveStatus reserveStatus;
    try {
        reserveStatus = controlResultStore_->reservePending(durablePending);
    } catch (const std::exception& ex) {
        ownership_->recordReceipt(
            forwardConfig_.control.sessionId,
            command.id,
            takeover.generation,
            false,
            commandFingerprint
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
            std::string("failed to persist control result: ") + ex.what(),
            nowMs
        );
        return;
    }
    if (reserveStatus != MqttControlReserveStatus::Inserted) {
        const auto persisted = controlResultStore_->find(command.id);
        if (reserveStatus == MqttControlReserveStatus::FingerprintMismatch ||
            !persisted || persisted->fingerprint != commandFingerprint) {
            publishControlReply(
                command.id,
                command.type,
                false,
                true,
                ownership_->active(nowMs) ? "remote" : "local",
                true,
                command.targetKw,
                takeover.generation,
                "command id payload mismatch",
                nowMs
            );
        } else if (!persisted->finalPayload.empty()) {
            (void)publishStoredControlResult(*persisted, nowMs);
        } else {
            publishControlReply(
                command.id,
                command.type,
                true,
                true,
                ownership_->active(nowMs) ? "remote" : "local",
                true,
                command.targetKw,
                persisted->generation,
                "duplicate accepted command pending device result",
                nowMs
            );
        }
        return;
    }

    const auto submitted = router_.submitWriteCommands(commands);
    if (!submitted.accepted) {
        try {
            (void)controlResultStore_->discardPending(command.id, commandFingerprint);
        } catch (const std::exception& ex) {
            writeHealth(false, std::string("failed to discard rejected control result: ") + ex.what(), 0, nowMs);
        }
        ownership_->recordReceipt(
            forwardConfig_.control.sessionId,
            command.id,
            takeover.generation,
            false,
            commandFingerprint
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

    pendingResult.routes = submitted.routes;
    pendingResult.submitted = true;
    bool submittedPersisted = false;
    try {
        submittedPersisted = controlResultStore_->markSubmitted(command.id, commandFingerprint);
    } catch (const std::exception& ex) {
        writeHealth(false, std::string("failed to persist submitted control phase: ") + ex.what(), 0, nowMs);
    }
    if (!submittedPersisted) {
        writeHealth(false, "control write queued but submitted phase was not persisted", 0, nowMs);
    }

    const bool receiptRecorded = ownership_->recordReceipt(
            forwardConfig_.control.sessionId,
            command.id,
            takeover.generation,
            true,
            commandFingerprint);
    if (!receiptRecorded) {
        writeHealth(false, "control write queued but ownership receipt was not persisted", 0, nowMs);
    }
    pendingControlResults_.push_back(std::move(pendingResult));
    publishControlReply(
        command.id,
        command.type,
        true,
        false,
        "remote",
        true,
        command.targetKw,
        takeover.generation,
        !submittedPersisted
            ? "remote control accepted; durable submission marker is degraded"
            : (!receiptRecorded
                ? "remote control accepted; ownership receipt persistence is degraded"
                : (takeover.message.empty() ? "remote control accepted" : takeover.message)),
        nowMs
    );
}

void MqttForwarderService::processPendingControlResults(std::int64_t nowMs) {
    for (auto it = pendingControlResults_.begin(); it != pendingControlResults_.end();) {
        std::vector<WritebackResultRecord> results;
        results.reserve(it->routes.size());
        bool complete = true;
        for (const auto& route : it->routes) {
            const auto result = router_.getWritebackResult(route, it->id, route.index);
            if (result) {
                results.push_back(*result);
            } else {
                complete = false;
            }
        }
        const bool timedOut = !complete && nowMs >= it->deadlineMs;
        if (!complete && !timedOut) {
            ++it;
            continue;
        }
        const auto payload = buildFinalControlResultPayload(*it, results, timedOut, nowMs);
        if (!controlResultStore_->storeFinalPayload(it->id, it->fingerprint, payload)) {
            writeHealth(false, "failed to persist final control result", 0, nowMs);
            ++it;
            continue;
        }
        it = pendingControlResults_.erase(it);
    }
}

void MqttForwarderService::publishUndeliveredControlResults(std::int64_t nowMs) {
    const auto records = controlResultStore_->loadUndelivered(1);
    if (!records.empty()) {
        (void)publishStoredControlResult(records.front(), nowMs);
    }
}

std::string MqttForwarderService::buildFinalControlResultPayload(
    const PendingControlResult& pending,
    const std::vector<WritebackResultRecord>& results,
    bool timedOut,
    std::int64_t nowMs
) const {
    std::unordered_map<std::uint32_t, const WritebackResultRecord*> resultsByIndex;
    const bool submissionConfirmed = pending.submitted || !results.empty();
    bool success = submissionConfirmed && !timedOut && results.size() == pending.routes.size();
    std::int64_t requestedAt = pending.acceptedAtMs;
    std::int64_t writeStartedAt = 0;
    std::int64_t writeCompletedAt = 0;
    std::int64_t queueDelayMs = 0;
    std::int64_t deviceWriteMs = 0;
    std::int64_t edgeElapsedMs = 0;
    std::int64_t totalElapsedMs = 0;
    std::string message = timedOut
        ? (submissionConfirmed ? "writeback result timeout" : "control submission outcome is uncertain")
        : "device write completed";
    for (const auto& result : results) {
        resultsByIndex[result.index] = &result;
        success = success && result.success;
        if (!result.success && message == "device write completed") {
            message = result.message.empty() ? "device write failed" : result.message;
        }
        if (result.requestedAt > 0) {
            requestedAt = std::min(requestedAt, result.requestedAt);
        }
        if (result.startedAt > 0 && (writeStartedAt == 0 || result.startedAt < writeStartedAt)) {
            writeStartedAt = result.startedAt;
        }
        writeCompletedAt = std::max(writeCompletedAt, result.completedAt);
        queueDelayMs = std::max(queueDelayMs, result.queueDelayMs);
        deviceWriteMs = std::max(deviceWriteMs, result.deviceWriteMs);
        edgeElapsedMs = std::max(edgeElapsedMs, result.edgeElapsedMs);
        totalElapsedMs = std::max(totalElapsedMs, result.totalElapsedMs);
    }
    if (timedOut) {
        totalElapsedMs = std::max<std::int64_t>(
            totalElapsedMs,
            std::max<std::int64_t>(0, nowMs - pending.acceptedAtMs)
        );
        edgeElapsedMs = std::max(edgeElapsedMs, totalElapsedMs);
    }

    std::ostringstream payload;
    payload << "{\"id\":\"" << escapeJson(pending.id) << "\""
            << ",\"type\":" << pending.type
            << ",\"accepted\":" << (submissionConfirmed ? "true" : "false")
            << ",\"duplicate\":false"
            << ",\"mode\":\"" << (ownership_->active(nowMs) ? "remote" : "local") << "\""
            << ",\"targetKw\":" << pending.targetKw
            << ",\"generation\":" << pending.generation
            << ",\"stage\":\""
            << (timedOut
                ? (submissionConfirmed ? "writeback-timeout" : "submission-uncertain")
                : "device-result")
            << "\""
            << ",\"success\":" << (success ? "true" : "false")
            << ",\"message\":\"" << escapeJson(message) << "\""
            << ",\"requestedAt\":" << requestedAt
            << ",\"acceptedAt\":" << pending.acceptedAtMs
            << ",\"writeStartedAt\":" << writeStartedAt
            << ",\"writeCompletedAt\":" << writeCompletedAt
            << ",\"queueDelayMs\":" << queueDelayMs
            << ",\"deviceWriteMs\":" << deviceWriteMs
            << ",\"edgeElapsedMs\":" << edgeElapsedMs
            << ",\"totalElapsedMs\":" << totalElapsedMs
            << ",\"results\":[";
    for (std::size_t i = 0; i < pending.routes.size(); ++i) {
        const auto& route = pending.routes[i];
        const auto found = resultsByIndex.find(route.index);
        payload << (i == 0 ? "" : ",") << "{\"index\":" << route.index;
        if (found == resultsByIndex.end()) {
            payload << ",\"success\":false,\"stage\":\"writeback-timeout\""
                    << ",\"message\":\"writeback result timeout\"";
        } else {
            const auto& result = *found->second;
            payload << ",\"value\":" << result.value
                    << ",\"success\":" << (result.success ? "true" : "false")
                    << ",\"stage\":\"" << escapeJson(result.stage) << "\""
                    << ",\"message\":\"" << escapeJson(result.message) << "\""
                    << ",\"queueDelayMs\":" << result.queueDelayMs
                    << ",\"deviceWriteMs\":" << result.deviceWriteMs
                    << ",\"edgeElapsedMs\":" << result.edgeElapsedMs
                    << ",\"totalElapsedMs\":" << result.totalElapsedMs
                    << ",\"verifyAttempted\":" << (result.verifyAttempted ? "true" : "false")
                    << ",\"verifyPassed\":" << (result.verifyPassed ? "true" : "false");
        }
        payload << "}";
    }
    payload << "],\"ts\":" << nowMs << "}";
    return payload.str();
}

bool MqttForwarderService::publishStoredControlResult(
    const MqttControlResultRecord& record,
    std::int64_t nowMs
) const {
    if (record.finalPayload.empty()) {
        return false;
    }
    if (forwardConfig_.control.replyTopic.empty()) {
        return controlResultStore_->markDelivered(record.id, record.fingerprint, nowMs);
    }
    try {
        publisher_->publishReliableJsonMessage(
            forwardConfig_.control.replyTopic,
            record.finalPayload
        );
        if (!controlResultStore_->markDelivered(record.id, record.fingerprint, currentTimeMs())) {
            writeHealth(false, "final control reply acknowledged but delivery state was not persisted", 0, nowMs);
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        writeHealth(false, std::string("final control reply failed: ") + ex.what(), 0, nowMs);
        std::cerr << "mqtt forwarder final control reply failed error=" << ex.what() << std::endl;
        return false;
    }
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
