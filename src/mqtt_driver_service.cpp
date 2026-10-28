#include "edge_gateway/mqtt_driver_service.hpp"
#include "edge_gateway/dlt645_point_rules.hpp"
#include "edge_gateway/ems_control_mode.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#endif

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/diagnostic_output_json.hpp"
#include "edge_gateway/json_value.hpp"
#include "edge_gateway/ota_request_json.hpp"
#include "edge_gateway/mqtt_event_stats.hpp"
#include "edge_gateway/mqtt_full_snapshot.hpp"
#include "edge_gateway/legacy_telemetry_payload.hpp"
#include "edge_gateway/process_file_lock.hpp"
#include "edge_gateway/scada_control_lease.hpp"

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

void sleepInterruptibly(const std::atomic<bool>& running, int intervalMs) {
    int remaining = std::max(0, intervalMs);
    while (running.load() && remaining > 0) {
        const int slice = std::min(remaining, 50);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        remaining -= slice;
    }
}

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

double healthPercentile(std::vector<double> values, double ratio) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto boundedRatio = std::max(0.0, std::min(1.0, ratio));
    const auto index = boundedRatio <= 0.0
        ? 0
        : static_cast<std::size_t>(std::ceil(values.size() * boundedRatio)) - 1;
    return values[std::min(index, values.size() - 1)];
}

double healthMaximum(const std::vector<double>& values) {
    return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
}

void writeMqttHealthFile(const std::string& path, const std::string& payload) {
    if (path.empty()) return;
    const auto temporary = path + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) throw std::runtime_error("failed to open mqtt health file: " + temporary);
        output << payload << '\n';
        output.flush();
        if (!output) throw std::runtime_error("failed to write mqtt health file: " + temporary);
    }
    if (!replaceFileAtomically(temporary, path)) {
        std::remove(temporary.c_str());
        throw std::runtime_error("failed to replace mqtt health file: " + path);
    }
}

// JSON string escaping per RFC 8259 (control characters as \u00XX, invalid UTF-8 replaced).
std::string escapeJson(const std::string& value) {
    return escapeDiagnosticOutputJson(value);
}

void applyWritebackResultToReply(
    MqttCommandReply& reply,
    const WritebackResultRecord& result,
    const PointStoreRoute& route
) {
    reply.success = result.success;
    reply.message = result.message.empty() ? (result.success ? "ok" : "writeback failed") : result.message;
    reply.stage = result.stage;
    reply.machineCode = route.machineCode;
    reply.meterCode = route.meterCode;
    reply.pointCode = route.pointCode;
    reply.index = result.index;
    reply.value = result.value;
    reply.requestedAt = result.requestedAt;
    reply.acceptedAt = result.acceptedAt;
    reply.writeStartedAt = result.startedAt;
    reply.writeCompletedAt = result.completedAt;
    reply.queueDelayMs = result.queueDelayMs;
    reply.deviceWriteMs = result.deviceWriteMs;
    reply.edgeElapsedMs = result.edgeElapsedMs;
    reply.totalElapsedMs = result.totalElapsedMs;
    reply.verifyAttempted = result.verifyAttempted;
    reply.verifyPassed = result.verifyPassed;
    reply.highPriority = result.highPriority;
    reply.outcome = result.success ? "succeeded" : (result.outcomeUnknown ? "unknown" : "failed");
    reply.isFinal = true;
    reply.ts = result.completedAt > 0 ? result.completedAt : reply.ts;
}

// Event ownership consumes a complete object, including the optional versioned
// lease. Do not let malformed new fields fall back to the legacy Full lease.
class EventHealthReader {
public:
    explicit EventHealthReader(const std::string& text) {
        try {
            root_ = json::JsonParser(text, 32, 4096).parse();
            if (!root_.isObject()) return;
            std::unordered_set<std::string> keys;
            for (const auto& entry : root_.asObject().values) {
                if (!keys.insert(entry.key).second) return;
            }
            valid_ = true;
        } catch (...) {}
    }
    bool valid() const { return valid_; }
    bool contains(const char* key) const { return valid_ && root_.find(key); }
    bool tryGetString(const char* key, std::string* value) const {
        const auto* item = valid_ ? root_.find(key) : nullptr;
        if (!item || !item->isString()) return false;
        *value = item->asString();
        return true;
    }
    bool tryGetBool(const char* key, bool* value) const {
        const auto* item = valid_ ? root_.find(key) : nullptr;
        if (!item || !item->isBool()) return false;
        *value = item->asBool();
        return true;
    }
    bool tryGetInt64(const char* key, std::int64_t* value) const {
        const auto* item = valid_ ? root_.find(key) : nullptr;
        if (!item || !item->isNumber()) return false;
        const auto number = item->asNumber();
        if (!std::isfinite(number) || number < 0 || number > 9007199254740991.0 ||
            std::floor(number) != number) return false;
        *value = static_cast<std::int64_t>(number);
        return true;
    }
private:
    json::JsonValue root_;
    bool valid_ = false;
};

class FlatJsonReader {
public:
    explicit FlatJsonReader(const std::string& text) : text_(text) {
    }

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
                const char esc = text_[cursor++];
                switch (esc) {
                    case '"': value.push_back('"'); break;
                    case '\\': value.push_back('\\'); break;
                    case '/': value.push_back('/'); break;
                    case 'b': value.push_back('\b'); break;
                    case 'f': value.push_back('\f'); break;
                    case 'n': value.push_back('\n'); break;
                    case 'r': value.push_back('\r'); break;
                    case 't': value.push_back('\t'); break;
                    default: return false;
                }
            } else {
                value.push_back(ch);
            }
        }
        return false;
    }

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
        if (cursor < text_.size() && (text_[cursor] == 'e' || text_[cursor] == 'E')) {
            ++cursor;
            if (cursor < text_.size() && (text_[cursor] == '-' || text_[cursor] == '+')) {
                ++cursor;
            }
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

    bool tryGetUInt32(const char* key, std::uint32_t* out) const {
        double value = 0.0;
        if (!tryGetDouble(key, &value)) {
            return false;
        }
        if (value < 0.0) {
            return false;
        }
        *out = static_cast<std::uint32_t>(value);
        return true;
    }

    bool tryGetUInt64(const char* key, std::uint64_t* out) const {
        double value = 0.0;
        if (!tryGetDouble(key, &value)) {
            return false;
        }
        if (value < 0.0) {
            return false;
        }
        *out = static_cast<std::uint64_t>(value);
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

    bool tryGetBool(const char* key, bool* out) const {
        const auto pos = findKey(key);
        if (pos == std::string::npos) {
            return false;
        }
        auto cursor = skipWhitespace(pos);
        if (text_.compare(cursor, 4, "true") == 0) {
            *out = true;
            return true;
        }
        if (text_.compare(cursor, 5, "false") == 0) {
            *out = false;
            return true;
        }
        if (cursor < text_.size() && text_[cursor] == '"') {
            std::string textValue;
            if (!tryGetString(key, &textValue)) {
                return false;
            }
            std::transform(textValue.begin(), textValue.end(), textValue.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            if (textValue == "true" || textValue == "1" || textValue == "yes" || textValue == "high") {
                *out = true;
                return true;
            }
            if (textValue == "false" || textValue == "0" || textValue == "no" || textValue == "normal") {
                *out = false;
                return true;
            }
            return false;
        }
        double numericValue = 0.0;
        if (tryGetDouble(key, &numericValue)) {
            *out = numericValue != 0.0;
            return true;
        }
        return false;
    }

private:
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

// Commands drive physical setpoints: parse them with the strict JSON parser and
// read only root-level fields. Nested objects cannot shadow root fields and
// duplicate root keys are rejected instead of silently picking one.
constexpr std::size_t kMaxCommandPayloadBytes = 64 * 1024;

const json::JsonValue* commandField(const json::JsonValue& root, const char* key) {
    return root.find(key);
}

bool commandString(const json::JsonValue& root, const char* key, std::string* out) {
    const auto* item = commandField(root, key);
    if (item == nullptr || item->isNull()) {
        return false;
    }
    if (!item->isString()) {
        throw std::invalid_argument(std::string("command ") + key + " must be a string");
    }
    *out = item->asString();
    return true;
}

bool commandBool(const json::JsonValue& root, const char* key, bool* out) {
    const auto* item = commandField(root, key);
    if (item == nullptr || item->isNull()) {
        return false;
    }
    if (item->isBool()) {
        *out = item->asBool();
        return true;
    }
    if (item->isNumber()) {
        *out = item->asNumber() != 0.0;
        return true;
    }
    if (item->isString()) {
        auto text = item->asString();
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (text == "true" || text == "1" || text == "yes" || text == "high") {
            *out = true;
            return true;
        }
        if (text == "false" || text == "0" || text == "no" || text == "normal") {
            *out = false;
            return true;
        }
    }
    throw std::invalid_argument(std::string("command ") + key + " must be a boolean");
}

// Legacy aliases carry arbitrary priority levels ("P1", 3, ...). Only a real
// JSON boolean is interpreted; anything else is ignored and never grants
// high priority.
bool commandAliasBool(const json::JsonValue& root, const char* key, bool* out) {
    const auto* item = commandField(root, key);
    if (item == nullptr || !item->isBool()) {
        return false;
    }
    *out = item->asBool();
    return true;
}

MqttCommandRequest parseCommandRequest(const std::string& payload) {
    if (payload.size() > kMaxCommandPayloadBytes) {
        throw std::invalid_argument("command payload too large");
    }
    json::JsonValue root;
    try {
        root = json::JsonParser(payload, 16, 4096).parse();
    } catch (const std::exception& ex) {
        throw std::invalid_argument(std::string("command payload is not valid JSON: ") + ex.what());
    }
    if (!root.isObject()) {
        throw std::invalid_argument("command payload must be a JSON object");
    }
    std::unordered_set<std::string> keys;
    for (const auto& member : root.asObject().values) {
        if (!keys.insert(member.key).second) {
            throw std::invalid_argument("command payload contains duplicate field " + member.key);
        }
    }

    MqttCommandRequest request;
    if (!commandString(root, "cmdId", &request.cmdId) || request.cmdId.empty()) {
        throw std::invalid_argument("command cmdId is required");
    }
    commandString(root, "machineCode", &request.machineCode);
    commandString(root, "meterCode", &request.meterCode);
    commandString(root, "pointCode", &request.pointCode);
    const auto* index = commandField(root, "index");
    if (index == nullptr || !index->isNumber()) {
        throw std::invalid_argument("command index is required");
    }
    const auto indexValue = index->asNumber();
    if (!std::isfinite(indexValue) || std::floor(indexValue) != indexValue ||
        indexValue < 1.0 || indexValue > 4294967295.0) {
        throw std::invalid_argument("command index must be an integer in 1..4294967295");
    }
    request.index = static_cast<std::uint32_t>(indexValue);
    const auto* value = commandField(root, "value");
    if (value == nullptr || !value->isNumber() || !std::isfinite(value->asNumber())) {
        throw std::invalid_argument("command value is required");
    }
    request.value = value->asNumber();
    if (!commandString(root, "source", &request.source) || request.source.empty()) {
        request.source = "mqtt";
    }
    if (const auto* ts = commandField(root, "ts")) {
        if (!ts->isNull()) {
            double number = -1.0;
            if (ts->isNumber()) {
                number = ts->asNumber();
            } else if (ts->isString()) {
                // Legacy platforms send ts as a decimal string.
                const auto& text = ts->asString();
                if (!text.empty() && text.size() <= 16 &&
                    text.find_first_not_of("0123456789") == std::string::npos) {
                    number = static_cast<double>(std::stoll(text));
                }
            }
            if (!std::isfinite(number) || number < 0.0 || number > 9007199254740991.0) {
                throw std::invalid_argument("command ts must be a non-negative millisecond timestamp");
            }
            // Values below 1e11 cannot be epoch milliseconds after 1973: treat
            // them as epoch seconds.
            if (number > 0.0 && number < 1e11) {
                number *= 1000.0;
            }
            request.ts = static_cast<std::int64_t>(number);
        }
    }
    bool highPriority = false;
    if (commandBool(root, "highPriority", &highPriority) ||
        commandAliasBool(root, "priority", &highPriority) ||
        commandAliasBool(root, "priorityControl", &highPriority)) {
        request.highPriority = highPriority;
    }
    return request;
}

struct RealtimeSnapshotRequest {
    std::string sessionId;
    std::string action;
    std::string machineCode;
    std::string meterCode;
    std::vector<std::uint32_t> indexes;
    std::int64_t ttlSec = 0;
    int intervalMs = 0;
};

constexpr std::size_t kMaxRealtimeSessions = 16;
constexpr std::size_t kMaxRealtimeSelectorItems = 4096;
constexpr std::size_t kMaxRealtimeIdentityBytes = 128;
constexpr std::size_t kMaxRealtimeDefaultKeyBytes = 265;
constexpr int kMinRealtimeIntervalMs = 250;
constexpr int kMaxRealtimeIntervalMs = 60000;
constexpr std::int64_t kMinRealtimeTtlSec = 5;
constexpr std::int64_t kMaxRealtimeTtlSec = 300;

bool isRealtimeStopAction(const std::string& action);

std::int64_t realtimeDeadline(std::int64_t nowMs, std::int64_t delayMs) {
    if (nowMs > std::numeric_limits<std::int64_t>::max() - delayMs) {
        throw std::invalid_argument("realtime deadline overflow");
    }
    return nowMs + delayMs;
}

std::size_t skipJsonWhitespace(const std::string& text, std::size_t pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
    return pos;
}

void skipRealtimeString(const std::string& text, std::size_t& cursor) {
    if (cursor == text.size() || text[cursor++] != '"') {
        throw std::invalid_argument("realtime string required");
    }
    while (cursor < text.size()) {
        const auto ch = text[cursor++];
        if (ch == '"') return;
        if (ch == '\\' && cursor < text.size()) ++cursor;
    }
    throw std::invalid_argument("incomplete realtime string");
}

void skipRealtimeValue(const std::string& text, std::size_t& cursor) {
    if (cursor == text.size()) throw std::invalid_argument("missing realtime field value");
    if (text[cursor] == '"') {
        skipRealtimeString(text, cursor);
    } else if (text[cursor] == '[' || text[cursor] == '{') {
        std::vector<char> closing;
        do {
            if (cursor == text.size()) throw std::invalid_argument("incomplete realtime field value");
            const auto ch = text[cursor++];
            if (ch == '"') {
                --cursor;
                skipRealtimeString(text, cursor);
            } else if (ch == '[' || ch == '{') {
                closing.push_back(ch == '[' ? ']' : '}');
            } else if (ch == ']' || ch == '}') {
                if (closing.back() != ch) throw std::invalid_argument("invalid realtime field brackets");
                closing.pop_back();
            }
        } while (!closing.empty());
    } else {
        const auto begin = cursor;
        while (cursor < text.size() &&
               (std::isalnum(static_cast<unsigned char>(text[cursor])) ||
                text[cursor] == '-' || text[cursor] == '+' || text[cursor] == '.')) {
            ++cursor;
        }
        if (cursor == begin) throw std::invalid_argument("invalid realtime field value");
    }
}

std::size_t findJsonValueStart(const std::string& text, const char* key) {
    auto cursor = skipJsonWhitespace(text, 0);
    if (cursor == text.size() || text[cursor++] != '{') {
        throw std::invalid_argument("realtime request must be an object");
    }
    std::size_t result = std::string::npos;
    cursor = skipJsonWhitespace(text, cursor);
    if (cursor < text.size() && text[cursor] == '}' && skipJsonWhitespace(text, cursor + 1) == text.size()) {
        return result;
    }
    // Check root field boundaries without interpreting unrelated nested payloads.
    while (cursor < text.size()) {
        const auto begin = cursor + 1;
        skipRealtimeString(text, cursor);
        const auto end = cursor - 1;
        cursor = skipJsonWhitespace(text, cursor);
        if (text.find('\\', begin) < end || cursor == text.size() || text[cursor++] != ':') {
            throw std::invalid_argument("invalid realtime field name");
        }
        cursor = skipJsonWhitespace(text, cursor);
        if (text.compare(begin, end - begin, key) == 0) {
            if (result != std::string::npos) throw std::invalid_argument("duplicate realtime field");
            result = cursor;
        }
        skipRealtimeValue(text, cursor);
        cursor = skipJsonWhitespace(text, cursor);
        if (cursor == text.size()) break;
        if (text[cursor] == '}' && skipJsonWhitespace(text, cursor + 1) == text.size()) return result;
        if (text[cursor++] != ',') throw std::invalid_argument("realtime fields require a comma");
        cursor = skipJsonWhitespace(text, cursor);
    }
    throw std::invalid_argument("incomplete realtime request");
}

std::uint64_t readRealtimeUnsigned(
    const std::string& text, std::size_t& cursor, std::uint64_t minimum, std::uint64_t maximum,
    bool quoted = false
) {
    const auto begin = cursor;
    std::uint64_t value = 0;
    while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
        const auto digit = static_cast<unsigned>(text[cursor++] - '0');
        if (value > maximum / 10 || (value == maximum / 10 && digit > maximum % 10)) {
            throw std::invalid_argument("realtime integer out of range");
        }
        value = value * 10 + digit;
    }
    if (cursor == begin || value < minimum || (!quoted && cursor > begin + 1 && text[begin] == '0')) {
        throw std::invalid_argument("realtime integer out of range");
    }
    return value;
}

void requireRealtimeFieldEnd(const std::string& text, std::size_t cursor) {
    cursor = skipJsonWhitespace(text, cursor);
    if (cursor >= text.size() || (text[cursor] != ',' && text[cursor] != '}')) {
        throw std::invalid_argument("invalid realtime field value");
    }
}

std::string readRealtimeString(const std::string& text, const char* key, std::size_t maximum) {
    auto cursor = findJsonValueStart(text, key);
    if (cursor == std::string::npos) return {};
    if (cursor == text.size() || text[cursor++] != '"') {
        throw std::invalid_argument("realtime identity/action must be a string");
    }
    std::string value;
    while (cursor < text.size()) {
        char ch = text[cursor++];
        if (ch == '"') {
            requireRealtimeFieldEnd(text, cursor);
            return value;
        }
        if (static_cast<unsigned char>(ch) < 0x20 || value.size() >= maximum) {
            throw std::invalid_argument("invalid or oversized realtime string");
        }
        if (ch == '\\') {
            if (cursor == text.size()) break;
            switch (text[cursor++]) {
                case '"': ch = '"'; break;
                case '\\': ch = '\\'; break;
                case '/': ch = '/'; break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                default: throw std::invalid_argument("invalid realtime string escape");
            }
        }
        value.push_back(ch);
    }
    throw std::invalid_argument("incomplete realtime string");
}

bool isNormalRealtimeId(const std::string& id) {
    return id.size() <= kMaxRealtimeIdentityBytes &&
        std::all_of(id.begin(), id.end(), [](unsigned char ch) { return ch >= 0x21 && ch <= 0x7e; });
}

void validateRealtimeEnvelope(const std::string& payload) {
    // Use the existing strict parser only as an envelope/ambiguity gate. Keep
    // the realtime reader's legacy field semantics and selector limits below.
    if (payload.size() > 256 * 1024) throw std::invalid_argument("realtime payload too large");
    const auto root = json::JsonParser(payload, 32, 16384).parse();
    if (!root.isObject()) throw std::invalid_argument("realtime request must be an object");
    std::unordered_set<std::string> keys;
    for (const auto& member : root.asObject().values) {
        if (!keys.insert(member.key).second) {
            throw std::invalid_argument("duplicate realtime field");
        }
    }
}

std::int64_t readRealtimeIntegerField(
    const std::string& text, const char* key, std::uint64_t minimum, std::uint64_t maximum
) {
    auto cursor = findJsonValueStart(text, key);
    if (cursor == std::string::npos) {
        return 0;
    }
    const auto value = readRealtimeUnsigned(text, cursor, minimum, maximum);
    requireRealtimeFieldEnd(text, cursor);
    return static_cast<std::int64_t>(value);
}

void parseUInt32ArrayField(
    const std::string& text, const char* key, std::vector<std::uint32_t>& values
) {
    auto cursor = findJsonValueStart(text, key);
    if (cursor == std::string::npos) return;
    if (cursor >= text.size() || text[cursor++] != '[') {
        throw std::invalid_argument("realtime selector must be an array");
    }
    cursor = skipJsonWhitespace(text, cursor);
    if (cursor < text.size() && text[cursor] == ']') {
        requireRealtimeFieldEnd(text, cursor + 1);
        return;
    }
    while (true) {
        cursor = skipJsonWhitespace(text, cursor);
        if (values.size() >= kMaxRealtimeSelectorItems) {
            throw std::invalid_argument("realtime selector exceeds 4096 raw items");
        }
        const bool quoted = cursor < text.size() && text[cursor] == '"';
        if (quoted) ++cursor;
        const auto value = readRealtimeUnsigned(text, cursor, 1, UINT32_MAX, quoted);
        if (quoted && (cursor >= text.size() || text[cursor++] != '"')) {
            throw std::invalid_argument("invalid quoted realtime index");
        }
        values.push_back(static_cast<std::uint32_t>(value));
        cursor = skipJsonWhitespace(text, cursor);
        if (cursor < text.size() && text[cursor] == ']') {
            requireRealtimeFieldEnd(text, cursor + 1);
            return;
        }
        if (cursor < text.size() && text[cursor] == ',') {
            ++cursor;
            continue;
        }
        throw std::invalid_argument("invalid realtime selector item");
    }
}

RealtimeSnapshotRequest parseRealtimeSnapshotRequest(const std::string& payload) {
    RealtimeSnapshotRequest request;
    request.action = readRealtimeString(payload, "action", payload.size());
    if (request.action.empty()) {
        request.action = readRealtimeString(payload, "op", payload.size());
    }
    if (request.action.empty()) {
        request.action = readRealtimeString(payload, "mode", payload.size());
    }
    const bool stopRequest = isRealtimeStopAction(request.action);
    request.sessionId = readRealtimeString(payload, "sessionId",
        stopRequest ? kMaxRealtimeDefaultKeyBytes : kMaxRealtimeIdentityBytes);
    request.machineCode = readRealtimeString(payload, "machineCode", kMaxRealtimeIdentityBytes);
    if (stopRequest && !request.sessionId.empty() && isNormalRealtimeId(request.sessionId)) {
        return request;
    }
    request.meterCode = readRealtimeString(payload, "meterCode", kMaxRealtimeIdentityBytes);
    if (stopRequest) return request;
    request.ttlSec = readRealtimeIntegerField(payload, "ttlSec", kMinRealtimeTtlSec, kMaxRealtimeTtlSec);
    request.intervalMs = static_cast<int>(readRealtimeIntegerField(
        payload, "intervalMs", kMinRealtimeIntervalMs, kMaxRealtimeIntervalMs));

    const auto index = readRealtimeIntegerField(payload, "index", 1, UINT32_MAX);
    if (index != 0) {
        request.indexes.push_back(static_cast<std::uint32_t>(index));
    }
    parseUInt32ArrayField(payload, "indexes", request.indexes);
    parseUInt32ArrayField(payload, "indices", request.indexes);
    std::sort(request.indexes.begin(), request.indexes.end());
    request.indexes.erase(std::unique(request.indexes.begin(), request.indexes.end()), request.indexes.end());
    return request;
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool isRealtimeStopAction(const std::string& action) {
    const auto normalized = lowerAscii(action);
    return normalized == "stop" ||
           normalized == "close" ||
           normalized == "end" ||
           normalized == "cancel" ||
           normalized == "unsubscribe";
}

bool isRealtimeStartAction(const std::string& action) {
    const auto normalized = lowerAscii(action);
    return normalized == "start" ||
           normalized == "open" ||
           normalized == "subscribe" ||
           normalized == "renew";
}

std::vector<PointDefinition> effectiveMeterPoints(
    const DeviceConfig& config,
    const LogicalDeviceConfig& device,
    std::size_t meterIndex
) {
    if (!isDlt645Protocol(config.protocol.type)) {
        return device.points;
    }
    const auto standardPoints = device.points.empty() && !config.protocol.standardPointsFile.empty()
        ? ConfigLoader::loadDlt645StandardPointsFromFile(config.protocol.standardPointsFile)
        : std::vector<PointDefinition>();
    return expandDlt645MeterPoints(config, meterIndex, standardPoints);
}

std::vector<std::uint32_t> collectConfiguredFullUploadIndexes(const std::vector<DeviceConfig>& deviceConfigs) {
    std::vector<std::uint32_t> indexes;
    for (const auto& config : deviceConfigs) {
        if (!config.meters.empty()) {
            std::size_t meterIndex = 0;
            for (const auto& device : config.meters) {
                auto points = effectiveMeterPoints(config, device, meterIndex);
                for (const auto& point : points) {
                    if (point.fullUpload) {
                        indexes.push_back(point.index);
                    }
                }
                ++meterIndex;
            }
            continue;
        }
        for (const auto& point : config.points) {
            if (point.fullUpload) {
                indexes.push_back(point.index);
            }
        }
    }
    std::sort(indexes.begin(), indexes.end());
    indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
    return indexes;
}

}  // namespace

std::vector<std::uint32_t> MqttDriverService::resolveFullUploadIndexes(
    const MqttDriverConfig& driverConfig,
    const std::vector<DeviceConfig>& deviceConfigs,
    const PointStoreRouter& router
) {
    auto indexes = driverConfig.fullUploadIndexes;
    bool publishAll = driverConfig.publishAllOnFull;

    const auto configuredIndexes = collectConfiguredFullUploadIndexes(deviceConfigs);
    if (!configuredIndexes.empty()) {
        publishAll = false;
        indexes.insert(indexes.end(), configuredIndexes.begin(), configuredIndexes.end());
    }
    for (const auto& entry : router.routes()) {
        if (entry.second.fullUpload) {
            publishAll = false;
            indexes.push_back(entry.first);
        }
    }
    std::sort(indexes.begin(), indexes.end());
    indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
    if (publishAll || indexes.empty()) {
        return router.allIndexes();
    }
    return indexes;
}

std::int64_t currentMonotonicTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

// A queued write may still start until memoryStore.pendingWriteMaxAgeMs; the device daemon records
// the expiry on its next writeback cycle.
constexpr std::int64_t kPendingWriteExpirySettleMs = 2000;

// How long a command waits for its result before the "unknown" (non-final) reply: never shorter
// than the time a queued write can still start, so a timeout reply normally means "outcome unknown"
// and not "will run later".
int effectiveControlResultWaitMs(const MqttDriverConfig& driverConfig, const std::vector<DeviceConfig>& deviceConfigs) {
    std::int64_t waitMs = std::max(0, driverConfig.controlResultWaitTimeoutMs);
    for (const auto& config : deviceConfigs) {
        if (config.memoryStore.pendingWriteMaxAgeMs > 0) {
            waitMs = std::max(waitMs, config.memoryStore.pendingWriteMaxAgeMs + kPendingWriteExpirySettleMs);
        }
    }
    return static_cast<int>(std::min<std::int64_t>(waitMs, std::numeric_limits<int>::max() / 2));
}

MqttDriverService::MqttDriverService(
    MqttConfig mqttConfig,
    MqttDriverConfig driverConfig,
    std::vector<DeviceConfig> deviceConfigs,
    MemoryPointStore& store,
    std::shared_ptr<IMqttDriverPublisher> publisher,
    std::unique_ptr<MqttEventOutbox> eventOutbox,
    std::unique_ptr<OtaService> otaService,
    SystemMonitorConfig::ScadaUpperComputerSafetyConfig scadaSafetyConfig,
    std::unique_ptr<IEventStatsSource> eventStats,
    MqttEventReplayFactory businessReplayFactory,
    MqttEventReplayFactory managementReplayFactory,
    EventStoreIdentity eventStatsIdentity
) : mqttConfig_(std::move(mqttConfig)),
    driverConfig_(std::move(driverConfig)),
    ownedRouter_(new PointStoreRouter()),
    router_(*ownedRouter_),
    publisher_(std::move(publisher)),
    eventOutbox_(std::move(eventOutbox)),
    eventStats_(std::move(eventStats)),
    eventStatsIdentity_(std::move(eventStatsIdentity)),
    businessReplayFactory_(std::move(businessReplayFactory)),
    managementReplayFactory_(std::move(managementReplayFactory)),
    otaService_(std::move(otaService)),
    priorityControlLease_(driverConfig_.priorityControlLeaseFile, "mqtt-driver"),
    scadaSafetyConfig_(std::move(scadaSafetyConfig)) {
    router_.addStore(driverConfig_.sharedMemoryName, store);
    router_.addRoutesFromDeviceConfigs(deviceConfigs, driverConfig_.sharedMemoryName);
    resultWaitMs_ = effectiveControlResultWaitMs(driverConfig_, deviceConfigs);

    if (static_cast<bool>(businessReplayFactory_) != static_cast<bool>(managementReplayFactory_) ||
        (eventOutbox_ && businessReplayFactory_) ||
        (businessReplayFactory_ && (eventStatsIdentity_.storeId.empty() || eventStatsIdentity_.configGeneration.empty()))) {
        throw std::invalid_argument("MQTT driver requires both IPC replay roles and no legacy Outbox");
    }

    for (const auto& entry : router_.routes()) {
        PointRoute route;
        route.machineCode = entry.second.machineCode;
        route.meterCode = entry.second.meterCode;
        route.pointCode = entry.second.pointCode;
        route.writable = entry.second.writable;
        route.commandMailbox = entry.second.commandMailbox;
        pointRoutes_.emplace(entry.first, route);
    }
    for (const auto& config : deviceConfigs) {
        machineCodes_.insert(config.machineCode);
    }
    driverConfig_.fullUploadIndexes = resolveFullUploadIndexes(
        driverConfig_,
        deviceConfigs,
        router_
    );
    driverConfig_.publishAllOnFull = false;

    if (!publisher_) {
        throw std::invalid_argument("mqtt driver publisher is required");
    }
}

MqttDriverService::MqttDriverService(
    MqttConfig mqttConfig,
    MqttDriverConfig driverConfig,
    std::vector<DeviceConfig> deviceConfigs,
    PointStoreRouter& router,
    std::shared_ptr<IMqttDriverPublisher> publisher,
    std::unique_ptr<MqttEventOutbox> eventOutbox,
    std::unique_ptr<OtaService> otaService,
    SystemMonitorConfig::ScadaUpperComputerSafetyConfig scadaSafetyConfig,
    std::unique_ptr<IEventStatsSource> eventStats,
    MqttEventReplayFactory businessReplayFactory,
    MqttEventReplayFactory managementReplayFactory,
    EventStoreIdentity eventStatsIdentity
) : mqttConfig_(std::move(mqttConfig)),
    driverConfig_(std::move(driverConfig)),
    ownedRouter_(nullptr),
    router_(router),
    publisher_(std::move(publisher)),
    eventOutbox_(std::move(eventOutbox)),
    eventStats_(std::move(eventStats)),
    eventStatsIdentity_(std::move(eventStatsIdentity)),
    businessReplayFactory_(std::move(businessReplayFactory)),
    managementReplayFactory_(std::move(managementReplayFactory)),
    otaService_(std::move(otaService)),
    priorityControlLease_(driverConfig_.priorityControlLeaseFile, "mqtt-driver"),
    scadaSafetyConfig_(std::move(scadaSafetyConfig)) {
    if (static_cast<bool>(businessReplayFactory_) != static_cast<bool>(managementReplayFactory_) ||
        (eventOutbox_ && businessReplayFactory_) ||
        (businessReplayFactory_ && (eventStatsIdentity_.storeId.empty() || eventStatsIdentity_.configGeneration.empty()))) {
        throw std::invalid_argument("MQTT driver requires both IPC replay roles and no legacy Outbox");
    }
    if (!publisher_) {
        throw std::invalid_argument("mqtt driver publisher is required");
    }
    resultWaitMs_ = effectiveControlResultWaitMs(driverConfig_, deviceConfigs);
    for (const auto& config : deviceConfigs) {
        machineCodes_.insert(config.machineCode);
    }
    driverConfig_.fullUploadIndexes = resolveFullUploadIndexes(
        driverConfig_,
        deviceConfigs,
        router_
    );
    driverConfig_.publishAllOnFull = false;
    for (const auto& entry : router_.routes()) {
        PointRoute route;
        route.machineCode = entry.second.machineCode;
        route.meterCode = entry.second.meterCode;
        route.pointCode = entry.second.pointCode;
        route.writable = entry.second.writable;
        route.commandMailbox = entry.second.commandMailbox;
        pointRoutes_.emplace(entry.first, route);
    }
}

MqttDriverService::~MqttDriverService() {
    stop();
}

void MqttDriverService::setAgcAvcCommandMailboxRuntime(AgcAvcCommandMailboxRuntime runtime) {
    agcAvcCommandMailbox_ = std::move(runtime);
}

void MqttDriverService::start() {
    if (running_.load()) return;
    if (businessReplay_.active() || managementReplay_.active()) {
        throw std::logic_error("drain manual IPC replay before starting the worker");
    }
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    const auto& worker = driverConfig_.fullUploadWorker;
    if ((eventOutbox_ || businessReplayFactory_) && worker.mode == "isolated" && worker.eventForwardingEnabled &&
        !worker.eventDelegationReadyFile.empty()) {
        try {
            eventDelegationLiveLock_.reset(
                new ProcessFileLock(worker.eventDelegationReadyFile + ".lock")
            );
            if (!eventDelegationLiveLock_->tryAcquire()) {
                eventDelegationLiveLock_.reset();
                std::cerr << "mqtt event delegation live lock is busy" << std::endl;
            } else {
                publishEventDelegationReady(currentTimeMs());
            }
        } catch (const std::exception& ex) {
            eventDelegationLiveLock_.reset();
            std::cerr << "mqtt event delegation initialization failed error="
                      << ex.what() << std::endl;
        }
    }
    const auto ts = currentTimeMs();
    publishStatusEvent(
        "started",
        ts,
        std::string(R"("scanIntervalMs":)") + std::to_string(driverConfig_.scanIntervalMs) +
            R"(,"fullUploadIntervalMs":)" + std::to_string(driverConfig_.fullUploadIntervalMs) +
            R"(,"fullUploadMode":")" +
            escapeJson(driverConfig_.fullUploadWorker.mode) + R"(")"
    );
    replayPendingOtaStatuses(ts);
    scanThread_ = std::thread(&MqttDriverService::scanLoop, this);
    if (eventOutbox_ || businessReplayFactory_) {
        replayThread_ = std::thread(&MqttDriverService::replayLoop, this);
    }
}

void MqttDriverService::stop() {
    eventReplayStopping_.store(true);
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        if (!replayThread_.joinable()) drainIpcEvents();
        std::thread finishedThread;
        {
            std::lock_guard<std::mutex> otaLock(otaMutex_);
            if (otaThread_.joinable() && !otaInProgress_.load()) {
                finishedThread = std::move(otaThread_);
            }
        }
        if (finishedThread.joinable()) {
            finishedThread.join();
        }
        removeEventDelegationReady();
        eventDelegationLiveLock_.reset();
        eventReplayStopping_.store(false);
        return;
    }
    if (scanThread_.joinable()) {
        scanThread_.join();
    }
    if (replayThread_.joinable()) {
        replayThread_.join();
    }
    std::thread otaThread;
    {
        std::lock_guard<std::mutex> otaLock(otaMutex_);
        if (otaThread_.joinable()) {
            otaThread = std::move(otaThread_);
        }
    }
    if (otaThread.joinable()) {
        otaThread.join();
    }
    removeEventDelegationReady();
    eventDelegationLiveLock_.reset();
    eventReplayStopping_.store(false);
}

bool MqttDriverService::isRunning() const {
    return running_.load();
}

void MqttDriverService::runScanOnce(std::int64_t nowMs) {
    const auto started = std::chrono::steady_clock::now();
    try {
        runScanOnceInternal(nowMs, std::max(10, std::min(100, driverConfig_.scanIntervalMs)));
    } catch (...) {
        const auto durationMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started
        ).count();
        recordScanHealth(durationMs, true, nowMs);
        throw;
    }
    const auto durationMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started
    ).count();
    recordScanHealth(durationMs, false, nowMs);
}

void MqttDriverService::observeWallClock(std::int64_t wallMs, std::int64_t monotonicMs) {
    if (lastObservedWallMs_ != 0) {
        const auto stepMs = (wallMs - lastObservedWallMs_) - (monotonicMs - lastObservedMonotonicMs_);
        if (stepMs >= 1000 || stepMs <= -1000) {
            for (auto& pending : pendingCommandReplies_) {
                pending.deadlineMs += stepMs;
                pending.horizonMs += stepMs;
            }
            std::cerr << "mqtt driver wall clock stepped deltaMs=" << stepMs
                      << "; command reply deadlines shifted" << std::endl;
        }
    }
    lastObservedWallMs_ = wallMs;
    lastObservedMonotonicMs_ = monotonicMs;
}

void MqttDriverService::runScanOnceInternal(std::int64_t nowMs, int incomingTimeoutMs) {
    replayPendingOtaStatuses(nowMs);

    processIncomingMessages(nowMs, incomingTimeoutMs);
    processPendingCommandReplies(nowMs);
    cleanupExpiredRealtimeSessions(nowMs);
    publishDueRealtimeSessions(nowMs);

    const bool isolatedMode = driverConfig_.fullUploadWorker.mode == "isolated" &&
        driverConfig_.fullUploadIntervalMs > 0;
    if (isolatedMode) {
        if (lastFullUploadMs_ == 0 && !driverConfig_.publishFullOnStart) {
            lastFullUploadMs_ = nowMs;
        }
        const bool isolatedForwarderActive = isolatedFullForwarderIsActive(nowMs);
        if (isolatedForwarderActive) {
            isolatedFullForwarderWasActive_ = true;
            isolatedFallbackWaitStartedMs_ = 0;
            isolatedFallbackNextAttemptMs_ = 0;
            isolatedFallbackRetryDelayMs_ = driverConfig_.fullUploadWorker.retryMinMs;
            lastFullUploadMs_ = nowMs;
            if (isLegacyTelemetryDue(nowMs)) {
                publishLegacyTelemetryNow(nowMs);
            }
            return;
        }

        if (isolatedFallbackNextAttemptMs_ > 0) {
            if (isolatedFallbackNextAttemptMs_ <= nowMs &&
                !shouldDeferSnapshotForEventBacklog(nowMs)) {
                publishIsolatedFallback(nowMs);
            }
            if (isLegacyTelemetryDue(nowMs)) {
                publishLegacyTelemetryNow(nowMs);
            }
            return;
        }

        if (isolatedFullForwarderWasActive_) {
            isolatedFullForwarderWasActive_ = false;
            isolatedFallbackWaitStartedMs_ = nowMs;
            if (!shouldDeferSnapshotForEventBacklog(nowMs)) {
                publishIsolatedFallback(nowMs);
                return;
            }
        }

        // Give the worker one lease window to start before the inline path
        // publishes. Once that bounded bootstrap window expires, fail open.
        if (isolatedFallbackWaitStartedMs_ == 0) {
            isolatedFallbackWaitStartedMs_ = nowMs;
        }
        if (nowMs - isolatedFallbackWaitStartedMs_ <
            driverConfig_.fullUploadWorker.failoverTimeoutMs) {
            if (isLegacyTelemetryDue(nowMs)) {
                publishLegacyTelemetryNow(nowMs);
            }
            return;
        }
        if (lastFullUploadMs_ == 0 && !shouldDeferSnapshotForEventBacklog(nowMs)) {
            publishIsolatedFallback(nowMs);
            return;
        }
    } else {
        isolatedFullForwarderWasActive_ = false;
        isolatedFallbackWaitStartedMs_ = 0;
        isolatedFallbackNextAttemptMs_ = 0;
        isolatedFallbackRetryDelayMs_ = 0;
    }

    if (lastFullUploadMs_ == 0) {
        if (driverConfig_.publishFullOnStart) {
            if (shouldDeferSnapshotForEventBacklog(nowMs)) {
                if (isLegacyTelemetryDue(nowMs)) {
                    publishLegacyTelemetryNow(nowMs);
                }
                return;
            }
            publishFullSnapshotNow(nowMs);
        } else {
            lastFullUploadMs_ = nowMs;
            if (isLegacyTelemetryDue(nowMs)) {
                publishLegacyTelemetryNow(nowMs);
            }
        }
        return;
    }

    if (driverConfig_.fullUploadIntervalMs > 0 &&
        nowMs - lastFullUploadMs_ >= driverConfig_.fullUploadIntervalMs) {
        if (!shouldDeferSnapshotForEventBacklog(nowMs)) {
            if (isolatedMode) {
                publishIsolatedFallback(nowMs);
            } else {
                publishFullSnapshotNow(nowMs);
            }
            return;
        }
    }

    if (isLegacyTelemetryDue(nowMs)) {
        publishLegacyTelemetryNow(nowMs);
    }
}

void MqttDriverService::runEventReplayOnce(std::int64_t nowMs) {
    const auto started = std::chrono::steady_clock::now();
    try {
        replayEventOutboxIfNeeded(nowMs);
    } catch (...) {
        const auto durationMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started
        ).count();
        recordReplayHealth(durationMs, true);
        throw;
    }
    const auto durationMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started
    ).count();
    recordReplayHealth(durationMs, false);
}

void MqttDriverService::recordScanHealth(double durationMs, bool failed, std::int64_t nowMs) {
    std::lock_guard<std::mutex> lock(healthMutex_);
    const auto window = std::max<std::size_t>(10, driverConfig_.healthWindowCycles);
    scanDurationsMs_.push_back(durationMs);
    while (scanDurationsMs_.size() > window) scanDurationsMs_.pop_front();
    ++totalScanCycles_;
    if (failed) ++scanFailedCycles_;
    if (durationMs > std::max(1, driverConfig_.scanIntervalMs)) ++scanDeadlineMissCycles_;
    if (!driverConfig_.healthFile.empty() &&
        (lastHealthPublishMs_ == 0 ||
         nowMs - lastHealthPublishMs_ >= std::max(100, driverConfig_.healthPublishIntervalMs))) {
        try {
            publishHealthFileLocked(nowMs);
            lastHealthPublishMs_ = nowMs;
        } catch (const std::exception& ex) {
            std::cerr << "mqtt health publish failed error=" << ex.what() << std::endl;
        }
    }
    if (eventDelegationLiveLock_ &&
        (lastEventDelegationPublishMs_ == 0 ||
         nowMs - lastEventDelegationPublishMs_ >=
            std::max(100, driverConfig_.fullUploadWorker.healthHeartbeatMs))) {
        try {
            publishEventDelegationReady(nowMs);
        } catch (const std::exception& ex) {
            std::cerr << "mqtt event delegation renew failed error=" << ex.what() << std::endl;
        }
    }
}

void MqttDriverService::recordReplayHealth(double durationMs, bool failed) {
    std::lock_guard<std::mutex> lock(healthMutex_);
    const auto window = std::max<std::size_t>(10, driverConfig_.healthWindowCycles);
    replayDurationsMs_.push_back(durationMs);
    while (replayDurationsMs_.size() > window) replayDurationsMs_.pop_front();
    ++totalReplayCycles_;
    if (failed) ++replayFailedCycles_;
}

void MqttDriverService::publishHealthFileLocked(std::int64_t nowMs) {
    const std::vector<double> scans(scanDurationsMs_.begin(), scanDurationsMs_.end());
    const std::vector<double> replays(replayDurationsMs_.begin(), replayDurationsMs_.end());
    const auto scanP50Ms = healthPercentile(scans, 0.50);
    const auto scanP95Ms = healthPercentile(scans, 0.95);
    const auto scanP99Ms = healthPercentile(scans, 0.99);
    const auto scanFailurePercent = totalScanCycles_ == 0
        ? 100.0
        : 100.0 * static_cast<double>(scanFailedCycles_) / static_cast<double>(totalScanCycles_);
    const auto scanDeadlineMissPercent = totalScanCycles_ == 0
        ? 100.0
        : 100.0 * static_cast<double>(scanDeadlineMissCycles_) /
            static_cast<double>(totalScanCycles_);
    const auto replayFailurePercent = totalReplayCycles_ == 0
        ? 0.0
        : 100.0 * static_cast<double>(replayFailedCycles_) /
            static_cast<double>(totalReplayCycles_);
    std::int64_t eventLastAckAtMs = 0;
    std::string eventLastError;
    {
        std::lock_guard<std::mutex> eventLock(eventStateMutex_);
        eventLastAckAtMs = eventLastAckAtMs_;
        eventLastError = eventLastError_;
    }

    std::ostringstream payload;
    payload << std::fixed << std::setprecision(2)
            << "{\"schemaVersion\":\"1.0\",\"ts\":" << nowMs
            << ",\"healthy\":"
            << ((scanFailedCycles_ == 0 && scanDeadlineMissPercent < 20.0) ? "true" : "false")
            << ",\"windowCycles\":" << scans.size()
            << ",\"totalScanCycles\":" << totalScanCycles_
            << ",\"scanDeadlineMissCycles\":" << scanDeadlineMissCycles_
            << ",\"scanFailedCycles\":" << scanFailedCycles_
            << ",\"scanDeadlineMissPercent\":" << scanDeadlineMissPercent
            << ",\"scanFailurePercent\":" << scanFailurePercent
            << ",\"scanP50Ms\":" << scanP50Ms
            << ",\"scanP95Ms\":" << scanP95Ms
            << ",\"scanP99Ms\":" << scanP99Ms
            << ",\"scanMaxMs\":" << healthMaximum(scans)
            << ",\"scanUtilizationP95Percent\":"
            << (scanP95Ms * 100.0 / static_cast<double>(std::max(1, driverConfig_.scanIntervalMs)))
            << ",\"totalReplayCycles\":" << totalReplayCycles_
            << ",\"replayFailedCycles\":" << replayFailedCycles_
            << ",\"replayFailurePercent\":" << replayFailurePercent
            << ",\"replayP50Ms\":" << healthPercentile(replays, 0.50)
            << ",\"replayP95Ms\":" << healthPercentile(replays, 0.95)
            << ",\"replayP99Ms\":" << healthPercentile(replays, 0.99)
            << ",\"replayMaxMs\":" << healthMaximum(replays)
            << ",\"fullSnapshotsPublished\":" << fullSnapshotsPublished_.load()
            << ",\"eventForwarderActive\":"
            << (isolatedEventForwarderIsActive(nowMs) ? "true" : "false")
            << mqttEventStatsHealthFields(readMqttEventStats(eventStats_.get(), mqttDriverBusinessStatsQuery(), eventStatsIdentity_))
            << mqttEventStatsHealthFields(readMqttEventStats(eventStats_.get(), mqttDriverTotalStatsQuery(), eventStatsIdentity_),
                MqttStatsHealthSection::FullBacklog)
            << ",\"eventLastAckAtMs\":" << eventLastAckAtMs
            << ",\"eventLastError\":\"" << escapeJson(eventLastError) << "\""
            << '}';
    writeMqttHealthFile(driverConfig_.healthFile, payload.str());
}

void MqttDriverService::publishEventDelegationReady(std::int64_t nowMs) {
    if (!eventDelegationLiveLock_) {
        return;
    }
    const auto& worker = driverConfig_.fullUploadWorker;
    const auto monotonicNowMs = currentMonotonicTimeMs();
    const auto leaseUntilMonotonicMs = monotonicNowMs +
        std::max(100, worker.failoverTimeoutMs);
    std::ostringstream payload;
    payload << "{\"dataPlaneVersion\":2"
            << ",\"externalOutbox\":true"
            << ",\"eventReplayBackend\":\"" << (businessReplayFactory_ ? "ipc-lab" : "legacy") << "\""
            << ",\"eventStoreId\":\"" << escapeJson(eventStatsIdentity_.storeId) << "\""
            << ",\"eventStoreConfigGeneration\":\"" << escapeJson(eventStatsIdentity_.configGeneration) << "\""
            << ",\"eventFallbackCapable\":true"
            << ",\"machineCode\":\"" << escapeJson(primaryMachineCode()) << "\""
            << ",\"outboxPath\":\"" << escapeJson(mqttConfig_.eventOutboxSqlitePath) << "\""
            << ",\"changeTopic\":\"" << escapeJson(mqttConfig_.changeEventTopic) << "\""
            << ",\"alarmTopic\":\"" << escapeJson(mqttConfig_.alarmTopic) << "\""
            << ",\"changeTopicMachineScoped\":"
            << (mqttConfig_.changeEventTopicMachineScoped ? "true" : "false")
            << ",\"alarmTopicMachineScoped\":"
            << (mqttConfig_.alarmTopicMachineScoped ? "true" : "false")
            << ",\"eventReplayLockFile\":\""
            << escapeJson(worker.eventReplayLockFile) << "\""
            << ",\"heartbeatAtMs\":" << nowMs
            << ",\"heartbeatMonotonicMs\":" << monotonicNowMs
            << ",\"leaseUntilMonotonicMs\":" << leaseUntilMonotonicMs
            << '}';
    writeMqttHealthFile(worker.eventDelegationReadyFile, payload.str());
    lastEventDelegationPublishMs_ = nowMs;
}

void MqttDriverService::removeEventDelegationReady() {
    if (!eventDelegationLiveLock_) {
        return;
    }
    const auto& path = driverConfig_.fullUploadWorker.eventDelegationReadyFile;
    if (!path.empty()) {
        std::remove(path.c_str());
        std::remove((path + ".tmp").c_str());
    }
    lastEventDelegationPublishMs_ = 0;
}

bool MqttDriverService::isolatedEventForwarderIsActive(std::int64_t nowMs) const {
    const auto& worker = driverConfig_.fullUploadWorker;
    if ((!eventOutbox_ && !businessReplayFactory_) || worker.mode != "isolated" ||
        !worker.eventForwardingEnabled || worker.healthFile.empty()) {
        return false;
    }
    std::ifstream input(worker.healthFile.c_str(), std::ios::in | std::ios::binary);
    if (!input.is_open()) {
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const auto text = buffer.str();
    if (text.empty() || text.size() > 64 * 1024) {
        return false;
    }
    EventHealthReader health(text);
    if (!health.valid()) return false;
    std::string replayBackend;
    if (health.contains("eventReplayBackend")) {
        if (!health.tryGetString("eventReplayBackend", &replayBackend) ||
            replayBackend != (businessReplayFactory_ ? "ipc-lab" : "legacy")) return false;
    } else if (businessReplayFactory_) return false;
    if (businessReplayFactory_) {
        std::string storeId, generation;
        if (!health.tryGetString("eventStoreId", &storeId) || storeId != eventStatsIdentity_.storeId ||
            !health.tryGetString("eventStoreConfigGeneration", &generation) ||
            generation != eventStatsIdentity_.configGeneration) return false;
    }
    std::int64_t dataPlaneVersion = 0;
    std::int64_t heartbeatMonotonicMs = 0;
    std::int64_t leaseUntilMonotonicMs = 0;
    bool eventForwarding = false;
    bool eventOutboxHealthy = false;
    bool changeMachineScoped = true;
    bool alarmMachineScoped = true;
    std::string targetId;
    std::string machineCode;
    std::string clientId;
    std::string fullTopic;
    std::string outboxPath;
    std::string replayLockFile;
    std::string changeTopic;
    std::string alarmTopic;
    if (!health.tryGetInt64("dataPlaneVersion", &dataPlaneVersion) || dataPlaneVersion < 2 ||
        !health.tryGetBool("eventForwarding", &eventForwarding) || !eventForwarding ||
        !health.tryGetBool("eventOutboxHealthy", &eventOutboxHealthy) || !eventOutboxHealthy ||
        !health.tryGetString("eventTargetId", &targetId) || targetId != "main" ||
        !health.tryGetString("machineCode", &machineCode) ||
        !health.tryGetString("clientId", &clientId) ||
        !health.tryGetString("topic", &fullTopic) ||
        !health.tryGetString("eventOutboxPath", &outboxPath) ||
        !health.tryGetString("eventReplayLockFile", &replayLockFile) ||
        !health.tryGetString("changeTopic", &changeTopic) ||
        !health.tryGetString("alarmTopic", &alarmTopic) ||
        !health.tryGetBool("changeTopicMachineScoped", &changeMachineScoped) ||
        !health.tryGetBool("alarmTopicMachineScoped", &alarmMachineScoped)) {
        return false;
    }
    const bool independentLease = health.contains("eventLeaseVersion") ||
        health.contains("eventHeartbeatMonotonicMs") || health.contains("eventLeaseUntilMonotonicMs");
    if (independentLease) {
        std::int64_t version = 0;
        if (!health.tryGetInt64("eventLeaseVersion", &version) || version != 1 ||
            !health.tryGetInt64("eventHeartbeatMonotonicMs", &heartbeatMonotonicMs) ||
            !health.tryGetInt64("eventLeaseUntilMonotonicMs", &leaseUntilMonotonicMs)) return false;
    } else if (!health.tryGetInt64("heartbeatMonotonicMs", &heartbeatMonotonicMs) ||
        !health.tryGetInt64("leaseUntilMonotonicMs", &leaseUntilMonotonicMs)) {
        return false;
    }
    const auto expectedFullTopic = mqttConfig_.fullTelemetryTopic.empty()
        ? mqttConfig_.telemetryTopic
        : mqttConfig_.fullTelemetryTopic;
    if (machineCode != primaryMachineCode() ||
        clientId != mqttConfig_.clientId + worker.clientIdSuffix ||
        fullTopic != expectedFullTopic ||
        outboxPath != mqttConfig_.eventOutboxSqlitePath ||
        replayLockFile != worker.eventReplayLockFile ||
        changeTopic != mqttConfig_.changeEventTopic ||
        alarmTopic != mqttConfig_.alarmTopic ||
        changeMachineScoped != mqttConfig_.changeEventTopicMachineScoped ||
        alarmMachineScoped != mqttConfig_.alarmTopicMachineScoped) {
        return false;
    }
    const auto monotonicNowMs = currentMonotonicTimeMs();
    if (independentLease) {
        return heartbeatMonotonicMs > 0 && heartbeatMonotonicMs <= monotonicNowMs &&
            leaseUntilMonotonicMs > monotonicNowMs &&
            leaseUntilMonotonicMs - heartbeatMonotonicMs <= worker.failoverTimeoutMs &&
            monotonicNowMs - heartbeatMonotonicMs <= worker.failoverTimeoutMs && nowMs > 0;
    }
    return leaseUntilMonotonicMs >= monotonicNowMs &&
        heartbeatMonotonicMs <= monotonicNowMs + worker.failoverTimeoutMs &&
        monotonicNowMs - heartbeatMonotonicMs <= worker.failoverTimeoutMs && nowMs > 0;
}

void MqttDriverService::replayIpcEventsIfNeeded(std::int64_t nowMs) {
    auto intervalMs = std::max(10, driverConfig_.scanIntervalMs);
    if (driverConfig_.deliveryMaxLatencyMs > 0) intervalMs = std::min(intervalMs, driverConfig_.deliveryMaxLatencyMs);
    const bool due = lastEventOutboxReplayMs_ == 0 || nowMs < lastEventOutboxReplayMs_ ||
        nowMs - lastEventOutboxReplayMs_ >= intervalMs;
    if (due) lastEventOutboxReplayMs_ = nowMs;
    const bool isolated = driverConfig_.fullUploadWorker.mode == "isolated" &&
        driverConfig_.fullUploadWorker.eventForwardingEnabled;
    const auto businessAuthorized = [this, isolated] {
        return !eventReplayStopping_.load() && (!isolated ||
            (ipcBusinessReplayLock_ && !isolatedEventForwarderIsActive(currentTimeMs())));
    };
    const auto managementAuthorized = [this] { return !eventReplayStopping_.load(); };
    std::size_t consumed = 0;
    std::string error;
    bool progressed = false;
    const auto totalBudget = driverConfig_.eventReplayMaxBytes;
    MqttEventReplayRequest business;
    business.lane = MqttEventReplayLane::MainBusiness;
    business.targetId = "main";
    business.includeTypes = {"alarm", "change"};
    business.maxBytes = std::min<std::size_t>(32768, totalBudget);
    business.authorized = businessAuthorized;
    try {
        if (!businessReplay_.active() && due && !eventReplayStopping_.load() && business.maxBytes &&
            (!isolated || !isolatedEventForwarderIsActive(currentTimeMs()))) {
            if (isolated) {
                ipcBusinessReplayLock_.reset(new ProcessFileLock(driverConfig_.fullUploadWorker.eventReplayLockFile));
                if (!ipcBusinessReplayLock_->tryAcquire()) ipcBusinessReplayLock_.reset();
            }
            // Recheck health after taking the shared replay lock, before factory construction.
            if (businessAuthorized()) {
                const auto result = businessReplay_.run(businessReplayFactory_, business, false);
                consumed = result.attemptedBytes;
                error = result.error;
                if (!result.healthy && error.empty()) error = "business IPC replay incomplete or unauthorized";
                if (result.ackedCount) {
                    std::lock_guard<std::mutex> lock(eventStateMutex_);
                    eventLastAckAtMs_ = currentTimeMs();
                }
                progressed = true;
            }
        } else if (businessReplay_.active() && (due || !businessAuthorized())) {
            const auto result = businessReplay_.run(businessReplayFactory_, business, !businessAuthorized());
            consumed = result.attemptedBytes;
            error = result.error;
            if (!result.healthy && error.empty()) error = "business IPC replay incomplete or unauthorized";
            if (result.ackedCount) {
                std::lock_guard<std::mutex> lock(eventStateMutex_);
                eventLastAckAtMs_ = currentTimeMs();
            }
            progressed = true;
        }
    } catch (const std::exception& ex) {
        // An adapter exception cannot erase attempted network bytes. Reserve the
        // full business allowance until its next incremental report is available.
        consumed = business.maxBytes;
        error = ex.what();
        progressed = true;
    } catch (...) {
        consumed = business.maxBytes;
        error = "business IPC replay failed";
        progressed = true;
    }
    if (!businessReplay_.active()) ipcBusinessReplayLock_.reset();

    MqttEventReplayRequest management;
    management.lane = MqttEventReplayLane::MainManagement;
    management.targetId = "main";
    management.includeTypes = {"ota_status"};
    management.maxBytes = std::min<std::size_t>(32768, totalBudget > consumed ? totalBudget - consumed : 0);
    management.authorized = managementAuthorized;
    const bool drainManagement = !managementAuthorized() || management.maxBytes == 0;
    if ((managementReplay_.active() && (due || drainManagement)) ||
        (!managementReplay_.active() && due && !drainManagement)) {
        try {
            const auto result = managementReplay_.run(managementReplayFactory_, management, drainManagement);
            if (!result.error.empty()) {
                if (!error.empty()) error += "; ";
                error += result.error;
            } else if (!result.healthy && error.empty()) error = "management IPC replay incomplete or unauthorized";
        } catch (const std::exception& ex) {
            if (!error.empty()) error += "; ";
            error += ex.what();
        } catch (...) {
            if (!error.empty()) error += "; ";
            error += "management IPC replay failed";
        }
        progressed = true;
    }
    if (progressed) {
        std::lock_guard<std::mutex> lock(eventStateMutex_);
        eventLastError_ = std::move(error);
    }
}

void MqttDriverService::drainIpcEvents() {
    // Shutdown cannot destroy an unknown mutation. Keep ownership and reconcile
    // on its original thread; stop() waits while the IPC service is unavailable.
    while (businessReplay_.active() || managementReplay_.active()) {
        replayIpcEventsIfNeeded(currentTimeMs());
        if (businessReplay_.active() || managementReplay_.active())
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ipcBusinessReplayLock_.reset();
}

void MqttDriverService::replayEventOutboxIfNeeded(std::int64_t nowMs) {
    if (businessReplayFactory_) {
        replayIpcEventsIfNeeded(nowMs);
        return;
    }
    if (!eventOutbox_) {
        return;
    }
    auto intervalMs = std::max(10, driverConfig_.scanIntervalMs);
    if (driverConfig_.deliveryMaxLatencyMs > 0) {
        intervalMs = std::min(intervalMs, driverConfig_.deliveryMaxLatencyMs);
    }
    if (lastEventOutboxReplayMs_ > 0 && nowMs - lastEventOutboxReplayMs_ < intervalMs) {
        return;
    }
    lastEventOutboxReplayMs_ = nowMs;
    try {
        MqttEventOutbox::ReplayStats stats;
        const auto sendBatch = [this](const std::vector<MqttEventOutbox::ReplayMessage>& messages) {
            std::vector<MqttJsonMessage> publishes;
            publishes.reserve(messages.size());
            for (const auto& message : messages) {
                publishes.push_back(MqttJsonMessage{message.topic, message.payload});
            }
            publisher_->publishReliableJsonMessages(publishes);
        };
        const MqttEventOutbox::EventTypeFilter businessEvents{{"alarm", "change"}, {}};
        const MqttEventOutbox::EventTypeFilter managementEvents{{}, {"alarm", "change"}};
        const bool isolatedEvents = driverConfig_.fullUploadWorker.mode == "isolated" &&
            driverConfig_.fullUploadWorker.eventForwardingEnabled;

        bool forwarderActive = isolatedEvents && isolatedEventForwarderIsActive(nowMs);
        if (!forwarderActive) {
            std::unique_ptr<ProcessFileLock> eventLock;
            if (isolatedEvents) {
                eventLock.reset(new ProcessFileLock(
                    driverConfig_.fullUploadWorker.eventReplayLockFile
                ));
                if (!eventLock->tryAcquire()) {
                    forwarderActive = true;
                } else {
                    // Close the health-check/lock race before publishing.
                    forwarderActive = isolatedEventForwarderIsActive(currentTimeMs());
                }
            }
            if (!forwarderActive) {
                const auto businessStats = eventOutbox_->replayBatchWithStats(
                    "main",
                    businessEvents,
                    driverConfig_.eventReplayMaxBytes,
                    0,
                    sendBatch
                );
                stats.count += businessStats.count;
                stats.bytes += businessStats.bytes;
                stats.alarmCount += businessStats.alarmCount;
                stats.changeCount += businessStats.changeCount;
                stats.otherCount += businessStats.otherCount;
                if (businessStats.count > 0) {
                    std::lock_guard<std::mutex> eventStateLock(eventStateMutex_);
                    eventLastAckAtMs_ = currentTimeMs();
                }
            }
        }

        const auto remainingBytes = driverConfig_.eventReplayMaxBytes > stats.bytes
            ? driverConfig_.eventReplayMaxBytes - stats.bytes
            : 0;
        MqttEventOutbox::ReplayStats managementStats;
        if (remainingBytes > 0) {
            managementStats = eventOutbox_->replayBatchWithStats(
                "main",
                managementEvents,
                remainingBytes,
                0,
                sendBatch
            );
        }
        stats.count += managementStats.count;
        stats.bytes += managementStats.bytes;
        stats.alarmCount += managementStats.alarmCount;
        stats.changeCount += managementStats.changeCount;
        stats.otherCount += managementStats.otherCount;
        {
            std::lock_guard<std::mutex> eventStateLock(eventStateMutex_);
            eventLastError_.clear();
        }
        eventOutbox_->cleanupIfDue(nowMs);
        if (stats.count > 0) {
            publishStatusEvent(
                "event-outbox-replayed",
                nowMs,
                std::string(R"("count":)") + std::to_string(stats.count) +
                    R"(,"bytes":)" + std::to_string(stats.bytes) +
                    R"(,"alarmCount":)" + std::to_string(stats.alarmCount) +
                    R"(,"changeCount":)" + std::to_string(stats.changeCount) +
                    R"(,"otherCount":)" + std::to_string(stats.otherCount) +
                    R"(,"maxBytes":)" + std::to_string(driverConfig_.eventReplayMaxBytes)
            );
        }
    } catch (const std::exception& ex) {
        {
            std::lock_guard<std::mutex> eventStateLock(eventStateMutex_);
            eventLastError_ = ex.what();
        }
        publishStatusEvent(
            "event-outbox-replay-failed",
            nowMs,
            std::string(R"("message":")") + escapeJson(ex.what()) + R"(")"
        );
    }
}

bool MqttDriverService::shouldDeferSnapshotForEventBacklog(std::int64_t nowMs) {
    if ((!eventOutbox_ && !businessReplayFactory_) ||
        driverConfig_.snapshotBacklogThreshold == 0 ||
        driverConfig_.snapshotBackoffIntervalMs <= 0 ||
        lastFullUploadMs_ <= 0) {
        return false;
    }

    const auto snapshot = readMqttEventStats(eventStats_.get(), mqttDriverTotalStatsQuery(), eventStatsIdentity_);
    if (!snapshot.valid) {
        return false;
    }
    const auto pending = static_cast<std::uint64_t>(snapshot.value.pendingCount);

    if (pending <= driverConfig_.snapshotBacklogThreshold) {
        return false;
    }
    if (nowMs - lastFullUploadMs_ >= driverConfig_.snapshotBackoffIntervalMs) {
        return false;
    }
    if (lastSnapshotDeferredMs_ == 0 || nowMs - lastSnapshotDeferredMs_ >= 1000) {
        lastSnapshotDeferredMs_ = nowMs;
        publishStatusEvent(
            "full-snapshot-deferred",
            nowMs,
            std::string(R"("pendingEvents":)") + std::to_string(pending) +
                R"(,"threshold":)" + std::to_string(driverConfig_.snapshotBacklogThreshold) +
                R"(,"backoffMs":)" + std::to_string(driverConfig_.snapshotBackoffIntervalMs)
        );
    }
    return true;
}

void MqttDriverService::processIncomingMessages(std::int64_t nowMs) {
    processIncomingMessages(nowMs, std::max(10, std::min(100, driverConfig_.scanIntervalMs)));
}

void MqttDriverService::processIncomingMessages(std::int64_t nowMs, int timeoutMs) {
    std::thread finishedThread;
    {
        std::lock_guard<std::mutex> lock(otaMutex_);
        if (otaThread_.joinable() && !otaInProgress_.load()) {
            finishedThread = std::move(otaThread_);
        }
    }
    if (finishedThread.joinable()) {
        finishedThread.join();
    }

    std::vector<MqttIncomingMessage> messages;
    try {
        messages = publisher_->pollIncoming(std::max(0, timeoutMs));
    } catch (const std::exception& ex) {
        // Report the outage once per transition; a status event per scan
        // would flood the offline buffer during a long broker outage.
        if (!subscribeUnavailable_) {
            subscribeUnavailable_ = true;
            publishStatusEvent(
                "mqtt-subscribe-unavailable",
                nowMs,
                std::string(R"("message":")") + escapeJson(ex.what()) + R"(")"
            );
        }
        return;
    }
    if (subscribeUnavailable_) {
        subscribeUnavailable_ = false;
        publishStatusEvent("mqtt-subscribe-restored", nowMs, std::string());
    }
    std::size_t queryAttemptsThisPoll = 0;
    for (const auto& message : messages) {
        try {
            if (message.type == MqttIncomingType::CommandRequest) {
                handleCommandRequest(message.payload, nowMs, message.retained);
            } else if (message.type == MqttIncomingType::OtaRequest) {
                handleOtaRequest(message.payload, nowMs);
            } else if (message.type == MqttIncomingType::OtaQueryRequest) {
                if (message.retained) reportOtaQueryDiagnostic("RETAINED");
                else if (++queryAttemptsThisPoll <= 4) handleOtaQueryRequest(message.payload);
                else reportOtaQueryDiagnostic("POLL_LIMIT");
            } else if (message.type == MqttIncomingType::RealtimeRequest) {
                handleRealtimeRequest(message.payload, nowMs);
            }
        } catch (const std::exception& ex) {
            (void)ex;
        }
    }
}

void MqttDriverService::publishFullSnapshotNow(std::int64_t nowMs) {
    std::vector<StoredPointValue> values;
    if (driverConfig_.publishAllOnFull || driverConfig_.fullUploadIndexes.empty()) {
        values = enrichValues(router_.getAllLatest(nowMs));
    } else {
        values = filterValues(driverConfig_.fullUploadIndexes, nowMs);
    }
    const auto& topic = mqttConfig_.fullTelemetryTopic.empty()
        ? mqttConfig_.telemetryTopic
        : mqttConfig_.fullTelemetryTopic;
    const auto indexes = driverConfig_.publishAllOnFull || driverConfig_.fullUploadIndexes.empty()
        ? router_.allIndexes() : driverConfig_.fullUploadIndexes;
    const auto fullValues = completeMqttFullSnapshot(values, indexes, router_);
    publisher_->publishFullSnapshot(topic, fullValues, driverConfig_.fullUploadJsonFormat);
    if (isLegacyTelemetryDue(nowMs)) {
        publishLegacyTelemetry(values, nowMs);
    }
    fullSnapshotsPublished_.fetch_add(1, std::memory_order_relaxed);
    const auto finishedMs = currentTimeMs();
    publishStatusEvent(
        "full-snapshot",
        finishedMs,
        std::string(R"("valueCount":)") + std::to_string(fullValues.size()) +
            R"(,"durationMs":)" + std::to_string(std::max<std::int64_t>(0, finishedMs - nowMs))
    );
    lastFullUploadMs_ = finishedMs;
}

bool MqttDriverService::isolatedFullForwarderIsActive(std::int64_t nowMs) const {
    const auto& worker = driverConfig_.fullUploadWorker;
    if (worker.mode != "isolated" || driverConfig_.fullUploadIntervalMs <= 0 ||
        worker.healthFile.empty()) {
        return false;
    }
    std::ifstream input(worker.healthFile.c_str(), std::ios::in | std::ios::binary);
    if (!input.is_open()) {
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const auto text = buffer.str();
    if (text.empty() || text.size() > 64 * 1024) {
        return false;
    }
    FlatJsonReader health(text);
    bool primaryFullUpload = false;
    bool healthy = false;
    std::string state;
    std::string machineCode;
    std::string clientId;
    std::string topic;
    std::int64_t heartbeatAtMs = 0;
    std::int64_t leaseUntilMs = 0;
    if (!health.tryGetBool("primaryFullUpload", &primaryFullUpload) ||
        !primaryFullUpload ||
        !health.tryGetString("state", &state) ||
        !health.tryGetString("machineCode", &machineCode) ||
        !health.tryGetString("clientId", &clientId) ||
        !health.tryGetString("topic", &topic) ||
        !health.tryGetBool("healthy", &healthy) ||
        !health.tryGetInt64("heartbeatAtMs", &heartbeatAtMs) ||
        !health.tryGetInt64("leaseUntilMs", &leaseUntilMs)) {
        return false;
    }
    const auto expectedTopic = mqttConfig_.fullTelemetryTopic.empty()
        ? mqttConfig_.telemetryTopic
        : mqttConfig_.fullTelemetryTopic;
    const auto expectedClientId = mqttConfig_.clientId + worker.clientIdSuffix;
    if (machineCode != primaryMachineCode() || topic != expectedTopic ||
        clientId != expectedClientId) {
        return false;
    }
    const bool leaseState = state == "claiming" || (state == "active" && healthy);
    std::int64_t heartbeatMonotonicMs = 0;
    std::int64_t leaseUntilMonotonicMs = 0;
    const bool hasMonotonicLease =
        health.tryGetInt64("heartbeatMonotonicMs", &heartbeatMonotonicMs) &&
        health.tryGetInt64("leaseUntilMonotonicMs", &leaseUntilMonotonicMs);
    if (hasMonotonicLease) {
        const auto monotonicNowMs = currentMonotonicTimeMs();
        return leaseState && leaseUntilMonotonicMs >= monotonicNowMs &&
            heartbeatMonotonicMs <= monotonicNowMs + worker.failoverTimeoutMs &&
            monotonicNowMs - heartbeatMonotonicMs <= worker.failoverTimeoutMs;
    }
    return leaseState && leaseUntilMs >= nowMs &&
        heartbeatAtMs <= nowMs + worker.failoverTimeoutMs &&
        nowMs - heartbeatAtMs <= worker.failoverTimeoutMs;
}

bool MqttDriverService::publishIsolatedFallback(std::int64_t nowMs) {
    const auto& worker = driverConfig_.fullUploadWorker;
    if (isolatedFallbackNextAttemptMs_ > nowMs) {
        return false;
    }
    const auto recordFailure = [&](const std::string& error) {
        const auto retryMs = std::max(
            worker.retryMinMs,
            isolatedFallbackRetryDelayMs_
        );
        isolatedFallbackNextAttemptMs_ = nowMs + retryMs;
        isolatedFallbackRetryDelayMs_ = std::min(
            worker.retryMaxMs,
            std::max(worker.retryMinMs, retryMs * 2)
        );
        std::cerr << "mqtt driver isolated full fallback failed error=" << error
                  << " retryMs=" << retryMs << std::endl;
    };

    try {
        ProcessFileLock publishLock(worker.publishLockFile);
        if (!publishLock.tryAcquire()) {
            recordFailure("publish ownership is busy");
            return false;
        }
        // The Forwarder can refresh its lease between the caller's first
        // health check and our lock acquisition. Recheck while holding the
        // shared ownership lock so a completed handoff cannot be followed by
        // a duplicate fallback snapshot.
        const auto lockedAtMs = currentTimeMs();
        if (isolatedFullForwarderIsActive(lockedAtMs)) {
            isolatedFallbackNextAttemptMs_ = 0;
            isolatedFallbackRetryDelayMs_ = worker.retryMinMs;
            return false;
        }
        publishFullSnapshotNow(lockedAtMs);
        isolatedFallbackNextAttemptMs_ = 0;
        isolatedFallbackRetryDelayMs_ = worker.retryMinMs;
        return true;
    } catch (const std::exception& ex) {
        recordFailure(ex.what());
        return false;
    }
}

bool MqttDriverService::isLegacyTelemetryDue(std::int64_t nowMs) const {
    return mqttConfig_.legacyTelemetryEnabled &&
        !mqttConfig_.legacyTelemetryTopic.empty() &&
        (lastLegacyTelemetryMs_ == 0 ||
         nowMs - lastLegacyTelemetryMs_ >= mqttConfig_.legacyTelemetryIntervalMs);
}

void MqttDriverService::publishLegacyTelemetryNow(std::int64_t nowMs) {
    std::vector<StoredPointValue> values;
    if (driverConfig_.publishAllOnFull || driverConfig_.fullUploadIndexes.empty()) {
        values = enrichValues(router_.getAllLatest(nowMs));
    } else {
        values = filterValues(driverConfig_.fullUploadIndexes, nowMs);
    }
    publishLegacyTelemetry(values, nowMs);
}

void MqttDriverService::publishLegacyTelemetry(
    const std::vector<StoredPointValue>& values,
    std::int64_t nowMs
) {
    publisher_->publishJsonMessage(
        mqttConfig_.legacyTelemetryTopic,
        edge_gateway::buildLegacyTelemetryPayload(
            values,
            mqttConfig_.legacyTelemetryPointMappings,
            mqttConfig_.legacyTelemetryMappedOnly,
            nowMs
        )
    );
    lastLegacyTelemetryMs_ = nowMs;
}

void MqttDriverService::publishOnDemandNow(const std::vector<std::uint32_t>& indexes, std::int64_t nowMs) {
    const auto values = indexes.empty() ? enrichValues(router_.getAllLatest(nowMs)) : filterValues(indexes, nowMs);
    publishRealtimeValues(values, indexes.size(), nowMs, std::string());
}

void MqttDriverService::publishRealtimeValues(
    std::vector<StoredPointValue> values,
    std::size_t requestedCount,
    std::int64_t nowMs,
    const std::string& sessionId
) {
    const auto& topic = mqttConfig_.realtimeTelemetryTopic.empty()
        ? mqttConfig_.telemetryTopic
        : mqttConfig_.realtimeTelemetryTopic;
    publisher_->publishRealtime(topic, values, driverConfig_.fullUploadJsonFormat, sessionId);
    publishStatusEvent(
        "on-demand",
        nowMs,
        std::string(R"("requestedCount":)") + std::to_string(requestedCount) +
            R"(,"publishedCount":)" + std::to_string(values.size())
    );
}

bool MqttDriverService::hasActiveRealtimeSessions(std::int64_t nowMs) const {
    for (const auto& entry : realtimeSessions_) {
        if (entry.second.expireAtMs > nowMs) {
            return true;
        }
    }
    return false;
}

void MqttDriverService::cleanupExpiredRealtimeSessions(std::int64_t nowMs) {
    for (auto it = realtimeSessions_.begin(); it != realtimeSessions_.end();) {
        if (it->second.expireAtMs <= nowMs) {
            it = realtimeSessions_.erase(it);
        } else {
            ++it;
        }
    }
}

void MqttDriverService::publishDueRealtimeSessions(std::int64_t nowMs) {
    for (auto& entry : realtimeSessions_) {
        auto& session = entry.second;
        if (session.expireAtMs <= nowMs || session.nextPublishMs > nowMs) {
            continue;
        }
        try {
            if (!session.indexes.empty()) {
                publishRealtimeValues(
                    filterValues(session.indexes, nowMs),
                    session.requestedCount,
                    nowMs,
                    session.sessionId
                );
            } else if (!session.meterCode.empty()) {
                publishRealtimeValues(
                    filterValuesByMeter(session.machineCode, session.meterCode, nowMs),
                    session.requestedCount,
                    nowMs,
                    session.sessionId
                );
            } else {
                publishRealtimeValues(
                    enrichValues(router_.getAllLatest(nowMs)),
                    session.requestedCount,
                    nowMs,
                    session.sessionId
                );
            }
        } catch (...) {
        }
        session.nextPublishMs = nowMs > std::numeric_limits<std::int64_t>::max() - session.intervalMs
            ? session.expireAtMs : nowMs + session.intervalMs;
    }
}

// A command without a result by its deadline gets a non-final "unknown" reply (it may still be
// executing or about to); the service keeps watching until the horizon and publishes the final
// result under the same cmdId when it appears, or a final "unknown" reply at the horizon.
void MqttDriverService::processPendingCommandReplies(std::int64_t nowMs) {
    for (auto it = pendingCommandReplies_.begin(); it != pendingCommandReplies_.end();) {
        auto reply = it->reply;
        const auto writeback = it->durableControl
            ? router_.getDurableWritebackResult(it->route, reply.cmdId, reply.acceptedAt)
            : router_.getWritebackResult(it->route, reply.cmdId);
        if (writeback) {
            applyWritebackResultToReply(reply, *writeback, it->route);
            reply.highPriority = it->reply.highPriority;
            if (it->timeoutReported) {
                publishStatusEvent(
                    "command-late-result",
                    nowMs,
                    std::string(R"("cmdId":")") + escapeJson(reply.cmdId) +
                        R"(","index":)" + std::to_string(reply.index) +
                        R"(,"outcome":")" + reply.outcome + R"(")"
                );
            }
        } else if (!it->timeoutReported && nowMs >= it->deadlineMs) {
            reply.success = false;
            reply.outcome = "unknown";
            reply.isFinal = false;
            reply.message = "no writeback result yet; the command may still take effect, the final result "
                            "follows with the same cmdId (do not retry with a new cmdId)";
            reply.stage = "writeback-timeout";
            reply.ts = nowMs;
            reply.edgeElapsedMs = std::max<std::int64_t>(0, nowMs - reply.acceptedAt);
            reply.totalElapsedMs = reply.edgeElapsedMs;
            publishStatusEvent(
                "command-writeback-timeout",
                nowMs,
                std::string(R"("cmdId":")") + escapeJson(reply.cmdId) +
                    R"(","index":)" + std::to_string(reply.index) +
                    R"(,"meterCode":")" + escapeJson(reply.meterCode) +
                    R"(","totalElapsedMs":)" + std::to_string(reply.totalElapsedMs)
            );
            it->timeoutReported = true;
            ++it;
            publisher_->publishCommandReply(mqttConfig_.commandReplyTopic, reply);
            continue;
        } else if (nowMs >= it->horizonMs) {
            reply.success = false;
            reply.outcome = "unknown";
            reply.isFinal = true;
            reply.message = "no writeback result; outcome unknown (resend the same cmdId to query it)";
            reply.stage = "writeback-timeout";
            reply.ts = nowMs;
            reply.edgeElapsedMs = std::max<std::int64_t>(0, nowMs - reply.acceptedAt);
            reply.totalElapsedMs = reply.edgeElapsedMs;
        } else {
            ++it;
            continue;
        }

        it = pendingCommandReplies_.erase(it);
        publisher_->publishCommandReply(mqttConfig_.commandReplyTopic, reply);
    }
}

int MqttDriverService::scanLoopIncomingTimeoutMs(std::int64_t nowMs) const {
    int waitMs = 1000;
    const bool isolatedMode = driverConfig_.fullUploadWorker.mode == "isolated" &&
        driverConfig_.fullUploadIntervalMs > 0;
    if (isolatedMode && isolatedFallbackNextAttemptMs_ > nowMs) {
        waitMs = std::min(
            waitMs,
            static_cast<int>(isolatedFallbackNextAttemptMs_ - nowMs)
        );
    } else if (isolatedMode && isolatedFallbackWaitStartedMs_ > 0 &&
               nowMs - isolatedFallbackWaitStartedMs_ <
                   driverConfig_.fullUploadWorker.failoverTimeoutMs) {
        waitMs = std::min(
            waitMs,
            static_cast<int>(driverConfig_.fullUploadWorker.failoverTimeoutMs -
                (nowMs - isolatedFallbackWaitStartedMs_))
        );
    } else if (lastFullUploadMs_ == 0) {
        waitMs = isolatedMode && isolatedFullForwarderWasActive_
            ? std::max(10, driverConfig_.scanIntervalMs)
            : 0;
    } else if (driverConfig_.fullUploadIntervalMs > 0) {
        const auto dueIn = driverConfig_.fullUploadIntervalMs - static_cast<int>(nowMs - lastFullUploadMs_);
        waitMs = std::min(waitMs, std::max(0, dueIn));
    }

    if (mqttConfig_.legacyTelemetryEnabled && !mqttConfig_.legacyTelemetryTopic.empty()) {
        if (lastLegacyTelemetryMs_ == 0) {
            waitMs = 0;
        } else {
            const auto dueIn = mqttConfig_.legacyTelemetryIntervalMs -
                static_cast<int>(nowMs - lastLegacyTelemetryMs_);
            waitMs = std::min(waitMs, std::max(0, dueIn));
        }
    }

    for (const auto& entry : realtimeSessions_) {
        const auto& session = entry.second;
        if (session.expireAtMs <= nowMs) {
            continue;
        }
        waitMs = std::min(waitMs, std::max(0, static_cast<int>(session.nextPublishMs - nowMs)));
        waitMs = std::min(waitMs, std::max(10, session.intervalMs));
    }

    for (const auto& pending : pendingCommandReplies_) {
        const auto dueMs = pending.timeoutReported ? pending.horizonMs : pending.deadlineMs;
        waitMs = std::min(waitMs, std::max(0, static_cast<int>(std::min<std::int64_t>(dueMs - nowMs, 1000))));
        waitMs = std::min(waitMs, 20);
    }

    waitMs = std::min(waitMs, std::max(10, driverConfig_.scanIntervalMs));
    return std::max(0, std::min(waitMs, 1000));
}

void MqttDriverService::scanLoop() {
    while (running_.load()) {
        const auto nowMs = currentTimeMs();
        observeWallClock(nowMs, currentMonotonicTimeMs());
        const auto timeoutMs = scanLoopIncomingTimeoutMs(nowMs);
        const auto started = std::chrono::steady_clock::now();
        bool failed = false;
        try {
            runScanOnceInternal(nowMs, std::min(250, timeoutMs));
        } catch (...) {
            failed = true;
        }
        const auto durationMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started
        ).count();
        recordScanHealth(durationMs, failed, nowMs);
        const auto nextWaitMs = scanLoopIncomingTimeoutMs(currentTimeMs());
        sleepInterruptibly(running_, nextWaitMs);
    }
}

void MqttDriverService::replayLoop() {
    int intervalMs = std::max(10, std::min(200, driverConfig_.scanIntervalMs));
    if (driverConfig_.deliveryMaxLatencyMs > 0) {
        intervalMs = std::max(10, std::min(intervalMs, driverConfig_.deliveryMaxLatencyMs));
    }
    while (running_.load()) {
        const auto nowMs = currentTimeMs();
        const auto started = std::chrono::steady_clock::now();
        bool failed = false;
        try {
            replayEventOutboxIfNeeded(nowMs);
        } catch (...) {
            failed = true;
        }
        const auto durationMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started
        ).count();
        recordReplayHealth(durationMs, failed);
        int waitMs = intervalMs;
        if (businessReplayFactory_) {
            // IPC already gates Claim by start time. Long batches must not pay
            // another full interval, but every iteration still yields.
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            waitMs = elapsedMs >= intervalMs ? 10 : std::max(10, intervalMs - static_cast<int>(elapsedMs));
        }
        sleepInterruptibly(running_, waitMs);
    }
    drainIpcEvents();
}

bool MqttDriverService::admitMailboxCommand(const std::string& meterCode, std::int64_t nowMs) {
    if (driverConfig_.mailboxRateLimitPerWindow <= 0) {
        return true;
    }
    const int windowMs = std::max(1, driverConfig_.commandRateWindowMs);
    std::lock_guard<std::mutex> lock(commandRateMutex_);
    auto& window = mailboxWindowByMeter_[meterCode];
    while (!window.empty() && nowMs - window.front() > windowMs) {
        window.pop_front();
    }
    if (window.size() >= static_cast<std::size_t>(driverConfig_.mailboxRateLimitPerWindow)) {
        return false;
    }
    window.push_back(nowMs);
    return true;
}

MqttDriverService::CommandAdmission MqttDriverService::admitCommand(
    const std::string& meterCode,
    const std::string& cmdId,
    std::int64_t nowMs,
    bool durableControl
) {
    const int windowMs = std::max(1, driverConfig_.commandRateWindowMs);
    const std::size_t maxInWindow = driverConfig_.commandRateMaxPerWindow > 0
        ? static_cast<std::size_t>(driverConfig_.commandRateMaxPerWindow)
        : 0;

    std::lock_guard<std::mutex> lock(commandRateMutex_);

    const auto ttl = std::max<std::int64_t>(0, driverConfig_.commandDedupTtlMs);
    if (!durableControl && !cmdId.empty() && ttl > 0) {
        for (auto it = recentCommandIds_.begin(); it != recentCommandIds_.end();) {
            if (nowMs - it->second > ttl) it = recentCommandIds_.erase(it);
            else ++it;
        }
        if (recentCommandIds_.find(cmdId) != recentCommandIds_.end()) return CommandAdmission::Duplicate;
    }

    // Sliding-window rate limit per meterCode (0 disables the limit).
    if (maxInWindow > 0) {
        auto& window = commandWindowByMeter_[meterCode];
        while (!window.empty() && nowMs - window.front() > windowMs) {
            window.pop_front();
        }
        if (window.size() >= maxInWindow) {
            return CommandAdmission::RateLimited;
        }
        window.push_back(nowMs);
    }
    if (!durableControl && !cmdId.empty() && ttl > 0) recentCommandIds_[cmdId] = nowMs;
    return CommandAdmission::Admitted;
}

void MqttDriverService::handleCommandRequest(const std::string& payload, std::int64_t nowMs, bool retained) {
    MqttCommandReply reply;
    reply.ts = nowMs;

    try {
        const auto request = parseCommandRequest(payload);
        reply.cmdId = request.cmdId;
        reply.machineCode = request.machineCode;
        reply.meterCode = request.meterCode;
        reply.pointCode = request.pointCode;
        reply.index = request.index;
        reply.highPriority = request.highPriority;
        // Transport origin cannot be overridden by a payload's source field.
        EmsControlMode(driverConfig_.emsControlModeFile).admitPlatform(nowMs);
        if (request.machineCode.empty()) {
            throw std::invalid_argument("machineCode is required");
        }
        if (retained && driverConfig_.rejectRetainedCommands) {
            throw std::invalid_argument("retained control commands are rejected");
        }
        if (request.ts <= 0 && driverConfig_.requireCommandTimestamp) {
            throw std::invalid_argument("command ts is required");
        }
        if (request.ts > 0 && driverConfig_.commandMaxSkewMs > 0) {
            const auto skew = request.ts > nowMs ? request.ts - nowMs : nowMs - request.ts;
            if (skew > driverConfig_.commandMaxSkewMs) {
                throw std::invalid_argument("command ts is outside the accepted clock skew window");
            }
        }
        if (request.highPriority && !driverConfig_.allowRemoteHighPriority) {
            // highPriority bypasses power-control ownership arbitration; it
            // must be explicitly enabled for remote (MQTT) ingress.
            throw std::invalid_argument("high-priority control is not permitted over MQTT");
        }
        if (scadaSafetyConfig_.enabled && request.source.rfind("scada-windows:", 0) == 0) {
            std::string leaseMessage;
            if (!ScadaControlLease::isFresh(
                    scadaSafetyConfig_.projectDirectory,
                    scadaSafetyConfig_.leaseFile,
                    request.machineCode,
                    nowMs,
                    &leaseMessage)) {
                throw std::invalid_argument(leaseMessage);
            }
        }

        const auto routeIt = pointRoutes_.find(request.index);
        if (routeIt == pointRoutes_.end()) {
            throw std::invalid_argument("command index not found");
        }

        const auto& route = routeIt->second;
        if (!request.machineCode.empty() && request.machineCode != route.machineCode) {
            throw std::invalid_argument("machineCode mismatch");
        }
        if (!request.meterCode.empty() && request.meterCode != route.meterCode) {
            throw std::invalid_argument("meterCode mismatch");
        }
        if (!request.pointCode.empty() && request.pointCode != route.pointCode) {
            throw std::invalid_argument("pointCode mismatch");
        }
        const bool commandMailbox = route.commandMailbox;
        if (!route.writable && !commandMailbox) {
            throw std::invalid_argument("point write is disabled");
        }
        if (commandMailbox) {
            if (!agcAvcCommandMailbox_.contains(request.index)) {
                throw std::invalid_argument("AGC/AVC command mailbox is not configured for this index");
            }
            if (!agcAvcCommandMailbox_.allowsSource(request.source)) {
                throw std::invalid_argument("AGC/AVC command mailbox rejected the source or current runtime mode");
            }
            if (request.highPriority) {
                throw std::invalid_argument("AGC/AVC command mailbox does not accept high-priority device writes");
            }
            // The mailbox carries its own dispatch sequence, so no cmdId dedup;
            // it gets a separate, generous flood limit instead of the device-write one.
            if (!admitMailboxCommand(route.meterCode, nowMs)) {
                throw std::invalid_argument("AGC/AVC mailbox rate limit exceeded");
            }
            PendingWriteCommand mailboxCommand;
            mailboxCommand.cmdId = request.cmdId;
            mailboxCommand.index = request.index;
            mailboxCommand.value = request.value;
            mailboxCommand.source = request.source;
            mailboxCommand.ts = request.ts > 0 ? request.ts : nowMs;
            mailboxCommand.acceptedAt = nowMs;
            mailboxCommand.platformControl = true;
            const auto modePermit = EmsControlMode(driverConfig_.emsControlModeFile).dispatchPermit(mailboxCommand);
            const auto submitted = router_.submitCommandMailbox(mailboxCommand);
            if (!submitted.accepted) {
                throw std::invalid_argument(submitted.message);
            }
            reply.machineCode = route.machineCode;
            reply.meterCode = route.meterCode;
            reply.pointCode = route.pointCode;
            reply.value = request.value;
            reply.success = true;
            reply.outcome = "succeeded";
            reply.message = submitted.message;
            reply.stage = "mailbox-committed";
            reply.requestedAt = mailboxCommand.ts;
            reply.acceptedAt = nowMs;
            reply.writeStartedAt = nowMs;
            reply.writeCompletedAt = nowMs;
            reply.ts = nowMs;
            publisher_->publishCommandReply(mqttConfig_.commandReplyTopic, reply);
            return;
        }
        // The same cmdId is still being tracked here: never submit it again. Its tracked entry
        // publishes the result under this cmdId.
        for (const auto& inFlight : pendingCommandReplies_) {
            if (inFlight.reply.cmdId != request.cmdId) {
                continue;
            }
            if (inFlight.reply.index != request.index || inFlight.reply.value != request.value) {
                throw std::invalid_argument("cmdId is already in use by a different command");
            }
            auto duplicate = inFlight.reply;
            duplicate.success = false;
            duplicate.outcome = "unknown";
            duplicate.isFinal = false;
            duplicate.stage = "duplicate-in-flight";
            duplicate.message = "same cmdId is already being executed; its final result follows with this cmdId";
            duplicate.ts = nowMs;
            publishStatusEvent(
                "command-duplicate",
                nowMs,
                std::string(R"("cmdId":")") + escapeJson(request.cmdId) +
                    R"(","index":)" + std::to_string(request.index) + R"(,"state":"in-flight")"
            );
            publisher_->publishCommandReply(mqttConfig_.commandReplyTopic, duplicate);
            return;
        }
        const auto activePriorityLease = priorityControlLease_.activeLease(nowMs);
        if (activePriorityLease && activePriorityLease->cmdId != request.cmdId) {
            throw std::invalid_argument("priority control in progress");
        }
        const bool durableControl = router_.isOrdinaryPhysicalWrite(request.index);
        const auto admission = admitCommand(route.meterCode, request.cmdId, nowMs, durableControl);
        if (admission != CommandAdmission::Admitted) {
            reply.machineCode = route.machineCode;
            reply.meterCode = route.meterCode;
            reply.pointCode = route.pointCode;
            reply.value = request.value;
            reply.success = false;
            const auto storeRoute = router_.routeByIndex(request.index);
            Optional<WritebackResultRecord> previous;
            if (admission == CommandAdmission::Duplicate && storeRoute) {
                previous = router_.getWritebackResult(*storeRoute, request.cmdId);
            }
            if (previous) {
                // Duplicate within commandDedupTtlMs: replay the original result, never re-execute.
                applyWritebackResultToReply(reply, *previous, *storeRoute);
            } else if (admission == CommandAdmission::Duplicate) {
                reply.outcome = "unknown";
                reply.stage = "duplicate";
                reply.message = "duplicate cmdId within commandDedupTtlMs; not executed again, original result unavailable";
            } else {
                reply.outcome = "failed";
                reply.stage = "rate-limited";
                reply.message = "command rate limit exceeded (commandRateMaxPerWindow); not executed";
            }
            publishStatusEvent(
                admission == CommandAdmission::Duplicate ? "command-duplicate" : "command-rate-limited",
                nowMs,
                std::string(R"("cmdId":")") + escapeJson(request.cmdId) +
                    R"(","index":)" + std::to_string(request.index) +
                    R"(,"stage":")" + escapeJson(reply.stage) + R"(")"
            );
            publisher_->publishCommandReply(mqttConfig_.commandReplyTopic, reply);
            return;
        }
        if (request.highPriority) {
            priorityControlLease_.acquire(
                request.cmdId,
                route.meterCode,
                request.index,
                nowMs,
                driverConfig_.priorityControlLeaseTtlMs
            );
        }
        PendingWriteCommand command;
        command.cmdId = request.cmdId;
        command.index = request.index;
        command.value = request.value;
        command.source = request.source;
        command.ts = request.ts > 0 ? request.ts : nowMs;
        command.acceptedAt = nowMs;
        command.highPriority = request.highPriority;
        command.durableControl = durableControl;
        command.platformControl = true;
        const auto submitResult = router_.submitWriteCommand(command);
        if (!submitResult.accepted) {
            if (request.highPriority) {
                priorityControlLease_.release(request.cmdId);
            }
            throw std::invalid_argument(submitResult.message);
        }

        reply.machineCode = route.machineCode;
        reply.meterCode = route.meterCode;
        reply.pointCode = route.pointCode;
        reply.value = request.value;
        reply.requestedAt = command.ts;
        reply.acceptedAt = command.acceptedAt;
        if (submitResult.writeback) {
            applyWritebackResultToReply(reply, *submitResult.writeback, submitResult.route);
            if (request.highPriority) priorityControlLease_.release(request.cmdId);
            publisher_->publishCommandReply(mqttConfig_.commandReplyTopic, reply);
            return;
        }
        std::cout << "mqtt command accepted"
                  << " cmdId=" << reply.cmdId
                  << " index=" << reply.index
                  << " device=" << reply.meterCode
                  << " highPriority=" << request.highPriority
                  << std::endl;
        publishStatusEvent(
            "command-accepted",
            nowMs,
            std::string(R"("cmdId":")") + escapeJson(reply.cmdId) +
                R"(","index":)" + std::to_string(reply.index) +
                R"(,"meterCode":")" + escapeJson(reply.meterCode) +
                R"(","highPriority":)" + (request.highPriority ? "true" : "false")
        );
        PendingCommandReply pending;
        pending.reply = reply;
        pending.route = submitResult.route;
        pending.durableControl = durableControl;
        pending.deadlineMs = nowMs + resultWaitMs_;
        pending.horizonMs = pending.deadlineMs + resultWaitMs_;
        pendingCommandReplies_.push_back(std::move(pending));
        return;
    } catch (const std::exception& ex) {
        reply.success = false;
        reply.message = ex.what();
        reply.stage = "rejected";
        std::cerr << "mqtt command rejected"
                  << " cmdId=" << reply.cmdId
                  << " index=" << reply.index
                  << " error=" << reply.message
                  << std::endl;
        publishStatusEvent(
            "command-rejected",
            nowMs,
            std::string(R"("cmdId":")") + escapeJson(reply.cmdId) +
                R"(","index":)" + std::to_string(reply.index) +
                R"(,"message":")" + escapeJson(reply.message) + R"(")"
        );
    }

    publisher_->publishCommandReply(mqttConfig_.commandReplyTopic, reply);
}

void MqttDriverService::handleOtaRequest(const std::string& payload, std::int64_t nowMs) {
    OtaReply reply;
    reply.ts = nowMs;
    try {
        // Identify this device's safe job before validating the rest of a duplicate.
        const auto root = ota_request_json::parseRoot(payload);
        reply.machineCode = ota_request_json::stringField(root, "machineCode");
        if (reply.machineCode.empty() || machineCodes_.find(reply.machineCode) == machineCodes_.end())
            throw std::invalid_argument("machineCode mismatch");
        reply.jobId = ota_request_json::stringField(root, "jobId");
        if (otaService_) otaService_->rejectDuplicateRequest(reply.jobId);
        {
            std::lock_guard<std::mutex> lock(otaMutex_);
            if (otaInProgress_.load() && otaJobId_ == reply.jobId)
                throw OtaRequestRejected(OtaRejectionReason::Duplicate, "OTA job already queued");
        }
        const auto request = ota_request_json::request(root);
        if (!otaService_ || !otaService_->enabled()) throw std::runtime_error("ota is disabled");
        std::string validateError;
        OtaRejectionReason reason;
        if (!otaService_->validateRequest(request, &validateError, &reason))
            throw OtaRequestRejected(reason, validateError.empty() ? "invalid ota request" : validateError);
        std::thread finishedThread;
        {
            std::lock_guard<std::mutex> lock(otaMutex_);
            if (otaThread_.joinable() && !otaInProgress_.load()) finishedThread = std::move(otaThread_);
        }
        if (finishedThread.joinable()) finishedThread.join();
        {
            std::lock_guard<std::mutex> lock(otaMutex_);
            if (otaInProgress_.load() || otaThread_.joinable())
                throw OtaRequestRejected(otaJobId_ == reply.jobId ? OtaRejectionReason::Duplicate :
                    OtaRejectionReason::NotAdmitted, "ota job already running");
            otaJobId_ = reply.jobId;
            otaInProgress_.store(true);
        }
        try { startOtaJob(request, reply.machineCode, nowMs); }
        catch (...) { otaInProgress_.store(false); throw; }
        // Accepted reply/status belong to execute's owned accepted callback only.
        std::cout << "mqtt ota request queued jobId=" << reply.jobId << std::endl;
        return;
    } catch (const OtaRequestRejected& ex) {
        std::cerr << "mqtt ota request rejected jobId=" << reply.jobId << " error=" << ex.what() << std::endl;
        try {
            publishStatusEvent("ota-rejected", nowMs, std::string(R"("jobId":")") + escapeJson(reply.jobId) +
                R"(","duplicate":)" + (ex.reason() == OtaRejectionReason::Duplicate ? "true" : "false") +
                R"(,"message":")" + escapeJson(ex.what()) + R"(")");
        } catch (...) {}
        // Shared reply has no attempt ID: negative duplicate would fail the original task.
        if (ex.reason() == OtaRejectionReason::Duplicate) return;
        reply.message = ex.what();
    } catch (const std::exception& ex) {
        reply.message = ex.what();
        std::cerr << "mqtt ota request rejected jobId=" << reply.jobId << " error=" << ex.what() << std::endl;
        try {
            publishStatusEvent("ota-rejected", nowMs, std::string(R"("jobId":")") + escapeJson(reply.jobId) +
                R"(","message":")" + escapeJson(ex.what()) + R"(")");
        } catch (...) {}
    }
    reply.accepted = false;
    publisher_->publishOtaReply(mqttConfig_.otaReplyTopic, reply);
}

void MqttDriverService::handleOtaQueryRequest(const std::string& payload) {
    if (!otaService_ || mqttConfig_.topicMachineCode.empty()) return;
    if (payload.size() > 8192) { reportOtaQueryDiagnostic("SIZE"); return; }
    const auto nowMs = currentMonotonicTimeMs();
    if (otaQueryWindowStartedMs_ == 0 || nowMs < otaQueryWindowStartedMs_ ||
        nowMs - otaQueryWindowStartedMs_ >= 1000) {
        otaQueryWindowStartedMs_ = nowMs;
        otaQueryAttemptsInWindow_ = 0;
    }
    if (otaQueryAttemptsInWindow_ >= 8) { reportOtaQueryDiagnostic("RATE_LIMIT"); return; }
    ++otaQueryAttemptsInWindow_;
    const char* phase = "SCHEMA";
    try {
        const auto root = ota_request_json::parseRoot(payload);
        if (ota_request_json::stringField(root, "format") == ota_full_query_protocol::requestFormat) {
            const auto challenge = ota_full_query_protocol::parseChallenge(payload);
            // Gateway identity survives initial commissioning or removal of the
            // last device. Read-only job queries do not require point routes.
            if (challenge.machineCode != mqttConfig_.topicMachineCode ||
                (!machineCodes_.empty() && machineCodes_.find(challenge.machineCode) == machineCodes_.end())) {
                reportOtaQueryDiagnostic("TARGET");
                return;
            }
            phase = "EVIDENCE";
            const auto observation = otaService_->answerOriginalJobQuery(challenge, challenge.machineCode);
            const auto encoded = ota_full_query_protocol::encodeObservation(observation);
            phase = "PUBLISH";
            publisher_->publishJsonMessage(mqttConfig_.otaQueryReplyTopic, encoded);
            return;
        }
        if (root.asObject().values.size() != 10) {
            reportOtaQueryDiagnostic("SCHEMA");
            return;
        }
        const auto decimal = [&](const char* name) -> std::uint64_t {
            const auto text = ota_request_json::stringField(root, name);
            if (text.empty() || text.size() > 19 || (text.size() > 1 && text.front() == '0') ||
                !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }))
                throw std::invalid_argument("invalid OTA query decimal");
            return std::stoull(text);
        };
        OtaQueryChallenge challenge;
        challenge.jobId = ota_request_json::stringField(root, "jobId");
        challenge.machineCode = ota_request_json::stringField(root, "machineCode");
        challenge.nonce = ota_request_json::stringField(root, "nonce");
        challenge.version = ota_request_json::stringField(root, "version");
        challenge.artifactSha256 = ota_request_json::stringField(root, "artifactSha256");
        challenge.artifactSize = decimal("artifactSize");
        challenge.candidateSha256 = ota_request_json::stringField(root, "candidateSha256");
        const auto issuedAtMs = decimal("issuedAtMs");
        const auto expiresAtMs = decimal("expiresAtMs");
        challenge.signature = ota_request_json::stringField(root, "signature");
        if (challenge.artifactSize > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            issuedAtMs > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            expiresAtMs > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            challenge.machineCode != mqttConfig_.topicMachineCode ||
            (!machineCodes_.empty() && machineCodes_.find(challenge.machineCode) == machineCodes_.end())) {
            reportOtaQueryDiagnostic("TARGET");
            return;
        }
        challenge.issuedAtMs = static_cast<std::int64_t>(issuedAtMs);
        challenge.expiresAtMs = static_cast<std::int64_t>(expiresAtMs);
        phase = "EVIDENCE";
        const auto observation = otaService_->answerOriginalJobQuery(challenge, challenge.machineCode);
        const auto field = [](const std::string& value) { return "\"" + escapeJson(value) + "\""; };
        std::ostringstream encoded;
        encoded << "{\"format\":\"edge-original-job-query-response-v1\",\"jobId\":" << field(observation.jobId)
            << ",\"machineCode\":" << field(observation.machineCode)
            << ",\"nonce\":" << field(observation.nonce)
            << ",\"outcome\":" << field(observation.outcome)
            << ",\"occurrenceId\":" << field(observation.occurrenceId)
            << ",\"version\":" << field(observation.version)
            << ",\"artifactSha256\":" << field(observation.artifactSha256)
            << ",\"artifactSize\":" << field(std::to_string(observation.artifactSize))
            << ",\"candidateSha256\":" << field(observation.candidateSha256)
            << ",\"transaction\":" << field(observation.transaction)
            << ",\"intentSha256\":" << field(observation.intentSha256)
            << ",\"healthSha256\":" << field(observation.healthSha256)
            << ",\"healthTs\":" << field(std::to_string(observation.healthTs))
            << ",\"signature\":" << field(observation.signature) << '}';
        phase = "PUBLISH";
        publisher_->publishJsonMessage(mqttConfig_.otaQueryReplyTopic, encoded.str());
    } catch (const OtaQueryRejected& rejected) {
        const char* reason = "EVIDENCE";
        switch (rejected.reason()) {
        case OtaQueryFailure::Unauthenticated: reason = "AUTH"; break;
        case OtaQueryFailure::Expired: reason = "EXPIRED"; break;
        case OtaQueryFailure::EvidenceUnavailable: reason = "EVIDENCE"; break;
        case OtaQueryFailure::SigningUnavailable: reason = "SIGNING"; break;
        case OtaQueryFailure::KeyConfiguration: reason = "KEY_CONFIG"; break;
        case OtaQueryFailure::CryptoLibrary: reason = "CRYPTO_LIB"; break;
        case OtaQueryFailure::CryptoOperation: reason = "CRYPTO_OP"; break;
        }
        reportOtaQueryDiagnostic(reason);
    } catch (const std::invalid_argument&) {
        reportOtaQueryDiagnostic("SCHEMA");
    } catch (const std::out_of_range&) {
        reportOtaQueryDiagnostic("SCHEMA");
    } catch (const std::exception&) {
        reportOtaQueryDiagnostic(phase);
    }
}

void MqttDriverService::reportOtaQueryDiagnostic(const char* reason) {
    const auto nowMs = currentMonotonicTimeMs();
    if (lastOtaQueryDiagnosticMs_ > 0 && nowMs - lastOtaQueryDiagnosticMs_ < 10000) {
        ++suppressedOtaQueryDiagnostics_;
        return;
    }
    std::cerr << "mqtt OTA query rejected code=" << reason
              << " suppressed=" << suppressedOtaQueryDiagnostics_ << std::endl;
    lastOtaQueryDiagnosticMs_ = nowMs;
    suppressedOtaQueryDiagnostics_ = 0;
}

void MqttDriverService::handleRealtimeRequest(const std::string& payload, std::int64_t nowMs) {
    // Never recover correlation from malformed JSON or ambiguous root fields.
    validateRealtimeEnvelope(payload);
    std::string feedbackSessionId;
    try {
        const auto id = readRealtimeString(payload, "sessionId", kMaxRealtimeIdentityBytes);
        const auto machine = readRealtimeString(payload, "machineCode", kMaxRealtimeIdentityBytes);
        const auto primary = primaryMachineCode();
        if (!id.empty() && isNormalRealtimeId(id) && !primary.empty() &&
            (machine.empty() || machine == primary) &&
            (machineCodes_.empty() || machineCodes_.count(primary) != 0)) {
            feedbackSessionId = id;
        }
    } catch (const std::exception&) {
        // Invalid/legacy synthetic IDs cannot identify a rejection recipient.
    }
    bool admitted = false;
    try {
        processRealtimeRequest(payload, nowMs, admitted);
    } catch (const std::invalid_argument&) {
        if (!admitted && !feedbackSessionId.empty()) {
            publishStatusEvent("realtime-session-rejected", nowMs,
                std::string(R"("sessionId":")") + escapeJson(feedbackSessionId) +
                R"(","reasonCode":"INVALID_REQUEST")");
        }
        throw;
    }
}

void MqttDriverService::processRealtimeRequest(
    const std::string& payload, std::int64_t nowMs, bool& admitted
) {
    const auto request = parseRealtimeSnapshotRequest(payload);
    const auto primaryMachine = primaryMachineCode();
    const auto machineCode = request.machineCode.empty() ? primaryMachine : request.machineCode;
    if (machineCode.empty() || machineCode.size() > kMaxRealtimeIdentityBytes) {
        throw std::invalid_argument("machineCode is required");
    }
    if (!primaryMachine.empty() && machineCode != primaryMachine) {
        throw std::invalid_argument("machineCode mismatch");
    }
    bool hasKnownMachine = false;
    bool knownMachine = false;
    for (const auto& code : machineCodes_) {
        if (code.empty()) {
            continue;
        }
        hasKnownMachine = true;
        if (code == machineCode) {
            knownMachine = true;
            break;
        }
    }
    if (hasKnownMachine && !knownMachine) {
        throw std::invalid_argument("machineCode mismatch");
    }

    const bool stopRequest = isRealtimeStopAction(request.action);
    const bool sessionRequest =
        !request.sessionId.empty() ||
        request.ttlSec > 0 ||
        request.intervalMs > 0 ||
        isRealtimeStartAction(request.action) ||
        stopRequest;
    const std::string defaultKey = "default:" + machineCode + ":" +
        (request.meterCode.empty() ? std::string("*") : request.meterCode);
    if (!isNormalRealtimeId(request.sessionId) &&
        !(stopRequest && request.sessionId == defaultKey && defaultKey.size() <= kMaxRealtimeDefaultKeyBytes)) {
        throw std::invalid_argument("realtime sessionId must be at most 128 visible ASCII bytes");
    }
    const std::string sessionId = request.sessionId.empty() ? defaultKey : request.sessionId;

    if (stopRequest) {
        admitted = true;
        realtimeSessions_.erase(sessionId);
        publishStatusEvent(
            "realtime-session-stopped",
            nowMs,
            std::string(R"("sessionId":")") + escapeJson(sessionId) + R"(")"
        );
        return;
    }

    bool publishImmediately = true;
    if (sessionRequest) {
        const auto ttlSec = request.ttlSec > 0 ? request.ttlSec : 30;
        RealtimeSession session;
        session.sessionId = request.sessionId;
        session.machineCode = machineCode;
        session.meterCode = request.meterCode;
        session.indexes = request.indexes;
        session.expireAtMs = realtimeDeadline(nowMs, ttlSec * 1000);
        session.intervalMs = request.intervalMs > 0 ? request.intervalMs
            : std::max(kMinRealtimeIntervalMs, std::min(kMaxRealtimeIntervalMs, driverConfig_.scanIntervalMs));
        session.nextPublishMs = realtimeDeadline(nowMs, session.intervalMs);
        session.requestedCount = request.indexes.size();

        cleanupExpiredRealtimeSessions(nowMs);
        const auto existing = realtimeSessions_.find(sessionId);
        publishImmediately = existing == realtimeSessions_.end();
        if (publishImmediately && realtimeSessions_.size() >= kMaxRealtimeSessions) {
            if (!request.sessionId.empty() && !primaryMachine.empty()) {
                publishStatusEvent("realtime-session-rejected", nowMs,
                    std::string(R"("sessionId":")") + escapeJson(request.sessionId) +
                    R"(","reasonCode":"CAPACITY_REACHED")");
            }
            return;
        }
        if (!publishImmediately) {
            session.nextPublishMs = existing->second.nextPublishMs;
        }
        // Admission and allocation precede the first PointStore read, including renewals.
        realtimeSessions_[sessionId] = std::move(session);
        admitted = true;
        const auto& accepted = realtimeSessions_.at(sessionId);
        publishStatusEvent(
            "realtime-session-started",
            nowMs,
            std::string(R"("sessionId":")") + escapeJson(sessionId) +
                R"(","meterCode":")" + escapeJson(request.meterCode) +
                R"(","indexCount":)" + std::to_string(request.indexes.size()) +
                R"(,"intervalMs":)" + std::to_string(accepted.intervalMs) +
                R"(,"expireAtMs":)" + std::to_string(accepted.expireAtMs)
        );
    }

    if (publishImmediately && !request.indexes.empty()) {
        publishRealtimeValues(
            filterValues(request.indexes, nowMs),
            request.indexes.size(),
            nowMs,
            request.sessionId
        );
    } else if (publishImmediately && !request.meterCode.empty()) {
        auto values = filterValuesByMeter(machineCode, request.meterCode, nowMs);
        publishRealtimeValues(std::move(values), 0, nowMs, request.sessionId);
    } else if (publishImmediately) {
        publishRealtimeValues(enrichValues(router_.getAllLatest(nowMs)), 0, nowMs, request.sessionId);
    }
}

void MqttDriverService::startOtaJob(const OtaRequest& request, const std::string& machineCode, std::int64_t nowMs) {
    std::lock_guard<std::mutex> lock(otaMutex_);
    if (otaThread_.joinable()) {
        throw std::runtime_error("ota job already running");
    }

    otaThread_ = std::thread([this, request, machineCode, nowMs]() {
        struct ClearBusy {
            std::atomic<bool>& busy;
            ~ClearBusy() { busy.store(false); }
        } clearBusy{otaInProgress_};
        OtaReply reply;
        OtaStatus status;
        try {
            otaService_->execute(
                request,
                machineCode,
                nowMs,
                &reply,
                &status,
                [this, &reply](const OtaStatus& stageStatus) {
                    try {
                        if (stageStatus.stage == "accepted")
                            publisher_->publishOtaReply(mqttConfig_.otaReplyTopic, reply);
                        publisher_->publishOtaStatus(mqttConfig_.otaStatusTopic, stageStatus);
                    } catch (const std::exception& ex) {
                        // Status is already durable; transport failure is not job failure.
                        std::cerr << "mqtt ota publication failed: " << ex.what() << std::endl;
                    } catch (...) {}
                }
            );
            std::cout << "mqtt ota job returned"
                      << " jobId=" << reply.jobId
                      << " stage=" << status.stage
                      << " message=" << status.message
                      << std::endl;
            try { publishStatusEvent(
                status.stage == "completed" ? "ota-completed" : "ota-applying",
                currentTimeMs(),
                std::string(R"("jobId":")") + escapeJson(reply.jobId) +
                    R"(","stage":")" + escapeJson(status.stage) +
                    R"(","message":")" + escapeJson(status.message) + R"(")"
            ); } catch (...) {}
        } catch (const OtaRequestRejected& ex) {
            try {
                publishStatusEvent("ota-rejected", currentTimeMs(), std::string(R"("jobId":")") +
                    escapeJson(request.jobId) + R"(","duplicate":)" +
                    (ex.reason() == OtaRejectionReason::Duplicate ? "true" : "false") +
                    R"(,"message":")" + escapeJson(ex.what()) + R"(")");
            } catch (...) {}
            if (ex.reason() != OtaRejectionReason::Duplicate) {
                try {
                    OtaReply rejected;
                    rejected.jobId = request.jobId;
                    rejected.machineCode = machineCode;
                    rejected.accepted = false;
                    rejected.message = ex.what();
                    rejected.ts = currentTimeMs();
                    publisher_->publishOtaReply(mqttConfig_.otaReplyTopic, rejected);
                } catch (...) {}
            }
        } catch (const std::exception& ex) {
            std::cerr << "mqtt ota job failed"
                      << " jobId=" << request.jobId
                      << " error=" << ex.what()
                      << std::endl;
            try { publishStatusEvent(
                "ota-failed",
                currentTimeMs(),
                std::string(R"("jobId":")") + escapeJson(request.jobId) +
                    R"(","message":")" + escapeJson(ex.what()) + R"(")"
            ); } catch (...) {}
        }
    });
}

void MqttDriverService::publishStatusEvent(
    const std::string& event,
    std::int64_t ts,
    const std::string& detailsJson,
    bool includeExceptionDetails
) const {
    if (mqttConfig_.statusTopic.empty()) {
        return;
    }
    std::ostringstream payload;
    payload << "{\"service\":\"mqtt-driver\",\"event\":\""
            << escapeJson(event)
            << "\",\"machineCode\":\""
            << escapeJson(primaryMachineCode())
            << "\",\"ts\":"
            << ts;
    if (!detailsJson.empty()) {
        payload << "," << detailsJson;
    }
    payload << "}";
    try {
        publisher_->publishJsonMessage(mqttConfig_.statusTopic, payload.str());
    } catch (const std::exception& ex) {
        std::cerr << "mqtt status event publish failed"
                  << " event=" << event
                  << " error=" << (includeExceptionDetails ? ex.what() : "publication failed")
                  << std::endl;
    } catch (...) {
        std::cerr << "mqtt status event publish failed"
                  << " event=" << event
                  << " error=unknown"
                  << std::endl;
    }
}

std::string MqttDriverService::primaryMachineCode() const {
    if (!mqttConfig_.topicMachineCode.empty()) {
        return mqttConfig_.topicMachineCode;
    }
    if (!machineCodes_.empty()) {
        return *machineCodes_.begin();
    }
    return "";
}

std::vector<StoredPointValue> MqttDriverService::filterValues(
    const std::vector<std::uint32_t>& indexes,
    std::int64_t nowMs
) const {
    return enrichValues(router_.getLatestByIndexes(indexes, nowMs));
}

std::vector<StoredPointValue> MqttDriverService::filterValuesByMeter(
    const std::string& machineCode,
    const std::string& meterCode,
    std::int64_t nowMs
) const {
    return enrichValues(router_.getLatestByMeter(machineCode, meterCode, nowMs));
}

void MqttDriverService::enrichValue(StoredPointValue& value) const {
    const auto routeIt = pointRoutes_.find(value.index);
    if (routeIt == pointRoutes_.end()) {
        return;
    }
    const auto& route = routeIt->second;
    if (value.machineCode.empty()) {
        value.machineCode = route.machineCode;
    }
    if (value.meterCode.empty()) {
        value.meterCode = route.meterCode;
    }
    if (value.pointCode.empty()) {
        value.pointCode = route.pointCode;
    }
}

void MqttDriverService::replayPendingOtaStatuses(std::int64_t nowMs) noexcept {
    if (!otaService_) {
        return;
    }
    if (lastOtaReplayAttemptMs_ != 0 && nowMs >= lastOtaReplayAttemptMs_ &&
        nowMs - lastOtaReplayAttemptMs_ < otaReplayRetryDelayMs_) return;
    lastOtaReplayAttemptMs_ = nowMs;
    const char* phase = "reconcile";
    std::size_t published = 0;
    std::size_t pending = 0;
    bool cleared = false;
    try {
        otaService_->reconcilePendingJobs();
        phase = "journal-read";
        const auto statuses = otaService_->loadPendingStatuses();
        pending = statuses.size();
        if (statuses.empty()) {
            otaReplayFirstSuccessMs_ = 0;
            otaReplaySuccessRounds_ = 0;
            otaReplayRetryDelayMs_ = 5000;
            return;
        }
        phase = "publish";
        for (const auto& status : statuses) {
            publisher_->publishOtaStatus(mqttConfig_.otaStatusTopic, status);
            ++published;
        }
        if (otaReplaySuccessRounds_ == 0) {
            otaReplayFirstSuccessMs_ = nowMs;
        }
        ++otaReplaySuccessRounds_;
        if (otaReplaySuccessRounds_ >= 6 && nowMs - otaReplayFirstSuccessMs_ >= 30000) {
            phase = "journal-clear";
            otaService_->clearPendingStatuses();
            otaReplayFirstSuccessMs_ = 0;
            otaReplaySuccessRounds_ = 0;
            cleared = true;
        }
        otaReplayRetryDelayMs_ = 5000;
    } catch (...) {
        otaReplayFirstSuccessMs_ = 0;
        otaReplaySuccessRounds_ = 0;
        otaReplayRetryDelayMs_ = std::min(30000, otaReplayRetryDelayMs_ * 2);
        // OTA storage/transport must not take the telemetry worker down. Neither
        // original exception text nor a diagnostic publication may escape here.
        try {
            std::cerr << "mqtt OTA replay deferred phase=" << phase
                      << " retryAfterMs=" << otaReplayRetryDelayMs_ << std::endl;
            publishStatusEvent(std::string(phase) == "reconcile" ? "ota-reconcile-failed" : "ota-replay-failed",
                nowMs, std::string(R"("phase":")") + phase + R"(","retryAfterMs":)" +
                    std::to_string(otaReplayRetryDelayMs_), false);
        } catch (...) {}
        return;
    }
    try { publishStatusEvent(
        "ota-status-replayed",
        nowMs,
        std::string(R"("count":)") + std::to_string(published) +
            R"(,"pending":)" + std::to_string(pending) +
            R"(,"rounds":)" + std::to_string(otaReplaySuccessRounds_) +
            R"(,"cleared":)" + (cleared ? "true" : "false"), false
    ); } catch (...) {}
}

std::vector<StoredPointValue> MqttDriverService::enrichValues(std::vector<StoredPointValue> values) const {
    for (auto& value : values) {
        enrichValue(value);
    }
    return values;
}

}  // namespace edge_gateway
