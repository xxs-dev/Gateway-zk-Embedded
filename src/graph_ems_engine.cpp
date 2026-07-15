#include "edge_gateway/graph_ems_engine.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace edge_gateway {

namespace {

class JsonValue;

struct JsonMember {
    std::string key;
    std::shared_ptr<JsonValue> value;
};

struct JsonObject {
    std::vector<JsonMember> values;
};

struct JsonArray {
    std::vector<std::shared_ptr<JsonValue>> values;
};

std::string directoryOf(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

std::string graphCmdId(std::uint32_t index, std::int64_t nowMs) {
    return "GRAPH_EMS_" + std::to_string(index) + "_" + std::to_string(nowMs);
}

bool directoryExists(const std::string& path) {
    if (path.empty()) {
        return true;
    }
#ifdef _WIN32
    return _access(path.c_str(), 0) == 0;
#else
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool valueMatches(double actual, double target, double tolerance = 0.5) {
    return std::abs(actual - target) <= tolerance;
}

void makeDirectory(const std::string& path) {
    if (path.empty() || directoryExists(path)) {
        return;
    }
    makeDirectory(directoryOf(path));
#ifdef _WIN32
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0755);
#endif
}

class JsonValue {
public:
    enum class Type {
        Null,
        Bool,
        Number,
        String,
        Object,
        Array
    };

    static JsonValue makeNull() {
        return JsonValue();
    }

    static JsonValue makeBool(bool value) {
        JsonValue result;
        result.type_ = Type::Bool;
        result.boolValue_ = value;
        return result;
    }

    static JsonValue makeNumber(double value) {
        JsonValue result;
        result.type_ = Type::Number;
        result.numberValue_ = value;
        return result;
    }

    static JsonValue makeString(std::string value) {
        JsonValue result;
        result.type_ = Type::String;
        result.stringValue_ = std::move(value);
        return result;
    }

    static JsonValue makeObject(JsonObject value) {
        JsonValue result;
        result.type_ = Type::Object;
        result.objectValue_.reset(new JsonObject(std::move(value)));
        return result;
    }

    static JsonValue makeArray(JsonArray value) {
        JsonValue result;
        result.type_ = Type::Array;
        result.arrayValue_.reset(new JsonArray(std::move(value)));
        return result;
    }

    bool isNull() const {
        return type_ == Type::Null;
    }

    bool isBool() const {
        return type_ == Type::Bool;
    }

    bool isNumber() const {
        return type_ == Type::Number;
    }

    bool isString() const {
        return type_ == Type::String;
    }

    bool isObject() const {
        return type_ == Type::Object;
    }

    bool isArray() const {
        return type_ == Type::Array;
    }

    bool asBool() const {
        if (!isBool()) {
            throw std::runtime_error("json value is not bool");
        }
        return boolValue_;
    }

    double asNumber() const {
        if (!isNumber()) {
            throw std::runtime_error("json value is not number");
        }
        return numberValue_;
    }

    const std::string& asString() const {
        if (!isString()) {
            throw std::runtime_error("json value is not string");
        }
        return stringValue_;
    }

    const JsonObject& asObject() const {
        if (!isObject()) {
            throw std::runtime_error("json value is not object");
        }
        return *objectValue_;
    }

    const JsonArray& asArray() const {
        if (!isArray()) {
            throw std::runtime_error("json value is not array");
        }
        return *arrayValue_;
    }

    const JsonValue* find(const std::string& key) const {
        if (!isObject()) {
            return nullptr;
        }
        for (const auto& entry : objectValue_->values) {
            if (entry.key == key) {
                return entry.value.get();
            }
        }
        return nullptr;
    }

private:
    Type type_ = Type::Null;
    bool boolValue_ = false;
    double numberValue_ = 0.0;
    std::string stringValue_;
    std::shared_ptr<JsonObject> objectValue_;
    std::shared_ptr<JsonArray> arrayValue_;
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : text_(text) {
    }

    JsonValue parse() {
        skipWhitespace();
        auto value = parseValue();
        skipWhitespace();
        if (!isEnd()) {
            fail("unexpected trailing characters in json");
        }
        return value;
    }

private:
    [[noreturn]] void fail(const std::string& message) const {
        throw std::runtime_error(message);
    }

    JsonValue parseValue() {
        skipWhitespace();
        if (isEnd()) {
            fail("unexpected end of json");
        }

        const char ch = peek();
        if (ch == '{') {
            return parseObject();
        }
        if (ch == '[') {
            return parseArray();
        }
        if (ch == '"') {
            return JsonValue::makeString(parseString());
        }
        if (ch == 't') {
            consumeLiteral("true");
            return JsonValue::makeBool(true);
        }
        if (ch == 'f') {
            consumeLiteral("false");
            return JsonValue::makeBool(false);
        }
        if (ch == 'n') {
            consumeLiteral("null");
            return JsonValue::makeNull();
        }
        if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch)) != 0) {
            return JsonValue::makeNumber(parseNumber());
        }
        fail("unexpected json token");
    }

    JsonValue parseObject() {
        consume('{');
        JsonObject object;
        skipWhitespace();
        if (tryConsume('}')) {
            return JsonValue::makeObject(std::move(object));
        }
        while (true) {
            skipWhitespace();
            const auto key = parseString();
            skipWhitespace();
            consume(':');
            auto value = parseValue();
            object.values.push_back(JsonMember{key, std::make_shared<JsonValue>(std::move(value))});
            skipWhitespace();
            if (tryConsume('}')) {
                break;
            }
            consume(',');
        }
        return JsonValue::makeObject(std::move(object));
    }

    JsonValue parseArray() {
        consume('[');
        JsonArray array;
        skipWhitespace();
        if (tryConsume(']')) {
            return JsonValue::makeArray(std::move(array));
        }
        while (true) {
            auto value = parseValue();
            array.values.push_back(std::make_shared<JsonValue>(std::move(value)));
            skipWhitespace();
            if (tryConsume(']')) {
                break;
            }
            consume(',');
        }
        return JsonValue::makeArray(std::move(array));
    }

    std::string parseString() {
        consume('"');
        std::string result;
        while (!isEnd()) {
            const char ch = get();
            if (ch == '"') {
                return result;
            }
            if (ch == '\\') {
                if (isEnd()) {
                    fail("unterminated json escape");
                }
                const char escaped = get();
                switch (escaped) {
                    case '"':
                    case '\\':
                    case '/':
                        result.push_back(escaped);
                        break;
                    case 'b':
                        result.push_back('\b');
                        break;
                    case 'f':
                        result.push_back('\f');
                        break;
                    case 'n':
                        result.push_back('\n');
                        break;
                    case 'r':
                        result.push_back('\r');
                        break;
                    case 't':
                        result.push_back('\t');
                        break;
                    default:
                        fail("unsupported json escape");
                }
            } else {
                result.push_back(ch);
            }
        }
        fail("unterminated json string");
    }

    double parseNumber() {
        const auto start = pos_;
        if (peek() == '-') {
            ++pos_;
        }
        while (!isEnd() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
            ++pos_;
        }
        if (!isEnd() && peek() == '.') {
            ++pos_;
            while (!isEnd() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                ++pos_;
            }
        }
        if (!isEnd() && (peek() == 'e' || peek() == 'E')) {
            ++pos_;
            if (!isEnd() && (peek() == '-' || peek() == '+')) {
                ++pos_;
            }
            while (!isEnd() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                ++pos_;
            }
        }
        return std::stod(text_.substr(start, pos_ - start));
    }

    void consumeLiteral(const char* literal) {
        for (const char* ch = literal; *ch != '\0'; ++ch) {
            consume(*ch);
        }
    }

    void consume(char expected) {
        if (isEnd() || get() != expected) {
            fail("unexpected json character");
        }
    }

    bool tryConsume(char expected) {
        if (!isEnd() && peek() == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    char get() {
        return text_[pos_++];
    }

    char peek() const {
        return text_[pos_];
    }

    bool isEnd() const {
        return pos_ >= text_.size();
    }

    void skipWhitespace() {
        while (!isEnd() && std::isspace(static_cast<unsigned char>(peek())) != 0) {
            ++pos_;
        }
    }

    const std::string& text_;
    std::size_t pos_ = 0;
};

std::string stringValue(const JsonObject& object, const char* key, const std::string& defaultValue = "") {
    for (const auto& entry : object.values) {
        if (entry.key == key) {
            if (entry.value->isNull()) {
                return defaultValue;
            }
            return entry.value->asString();
        }
    }
    return defaultValue;
}

bool boolValue(const JsonObject& object, const char* key, bool defaultValue) {
    for (const auto& entry : object.values) {
        if (entry.key == key) {
            if (entry.value->isNull()) {
                return defaultValue;
            }
            return entry.value->asBool();
        }
    }
    return defaultValue;
}

std::uint32_t uint32Value(const JsonObject& object, const char* key, std::uint32_t defaultValue = 0) {
    for (const auto& entry : object.values) {
        if (entry.key == key) {
            if (entry.value->isNull()) {
                return defaultValue;
            }
            return static_cast<std::uint32_t>(entry.value->asNumber());
        }
    }
    return defaultValue;
}

double numberValue(const JsonObject& object, const char* key, double defaultValue = 0.0) {
    for (const auto& entry : object.values) {
        if (entry.key == key) {
            if (entry.value->isNull()) {
                return defaultValue;
            }
            return entry.value->asNumber();
        }
    }
    return defaultValue;
}

std::int64_t restoredTimestamp(std::int64_t nowMs, double ageMs) {
    if (!std::isfinite(ageMs) || ageMs <= 0.0) {
        return nowMs;
    }
    const auto maxAge = static_cast<double>(std::numeric_limits<std::int64_t>::max());
    const auto boundedAge = static_cast<std::int64_t>(std::min(ageMs, maxAge));
    return boundedAge > nowMs ? 0 : nowMs - boundedAge;
}

std::int64_t elapsedAt(std::int64_t nowMs, std::int64_t startedAt) {
    return startedAt > 0 && nowMs >= startedAt ? nowMs - startedAt : 0;
}

std::uint32_t paramIndex(const GraphEmsNodeConfig& node, const std::string& key, std::uint32_t defaultValue = 0) {
    const auto it = node.params.find(key);
    if (it == node.params.end() || it->second.empty()) {
        return defaultValue;
    }
    return static_cast<std::uint32_t>(std::stoul(it->second));
}

bool paramBool(const GraphEmsNodeConfig& node, const std::string& key, bool defaultValue = false) {
    const auto it = node.params.find(key);
    if (it == node.params.end() || it->second.empty()) {
        return defaultValue;
    }
    return it->second == "1" || it->second == "true" || it->second == "TRUE";
}

Optional<double> paramDouble(const GraphEmsNodeConfig& node, const std::string& key) {
    const auto it = node.params.find(key);
    if (it == node.params.end() || it->second.empty()) {
        return NullOpt;
    }
    return std::stod(it->second);
}

std::string normalizedToken(const std::string& value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const auto ch : value) {
        if (std::isspace(static_cast<unsigned char>(ch)) == 0) {
            normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    return normalized;
}

std::size_t paramCount(const GraphEmsNodeConfig& node, const std::string& key) {
    const auto it = node.params.find(key + ".count");
    if (it == node.params.end() || it->second.empty()) {
        return 0;
    }
    return static_cast<std::size_t>(std::stoul(it->second));
}

std::vector<std::uint32_t> paramIndexes(const GraphEmsNodeConfig& node, const std::string& key) {
    std::vector<std::uint32_t> indexes;
    const auto count = paramCount(node, key);
    indexes.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        indexes.push_back(paramIndex(node, key + "." + std::to_string(i), 0));
    }
    return indexes;
}

Optional<double> paramOrLatestValue(
    const GraphEmsNodeConfig& node,
    const std::string& valueKey,
    const std::string& indexKey,
    const std::function<Optional<double>(std::uint32_t)>& latestByIndex
) {
    const auto literal = paramDouble(node, valueKey);
    if (literal) {
        return literal;
    }
    const auto index = paramIndex(node, indexKey, 0);
    if (index == 0) {
        return NullOpt;
    }
    return latestByIndex(index);
}

const JsonValue* findValue(const JsonObject& object, const char* key) {
    for (const auto& entry : object.values) {
        if (entry.key == key) {
            return entry.value.get();
        }
    }
    return nullptr;
}

std::tm localTimeFromEpochMs(std::int64_t nowMs) {
    std::time_t seconds = static_cast<std::time_t>(nowMs / 1000LL);
    std::tm localTime{};
#if defined(_WIN32)
    localtime_s(&localTime, &seconds);
#else
    localtime_r(&seconds, &localTime);
#endif
    return localTime;
}

int localHourFromEpochMs(std::int64_t nowMs) {
    const auto localTime = localTimeFromEpochMs(nowMs);
    if (localTime.tm_hour < 0 || localTime.tm_hour > 23) {
        return 0;
    }
    return localTime.tm_hour;
}

int localTimeComponentFromEpochMs(std::int64_t nowMs, const std::string& component) {
    const auto localTime = localTimeFromEpochMs(nowMs);
    if (component == "minute") {
        return localTime.tm_min;
    }
    if (component == "second") {
        return localTime.tm_sec;
    }
    if (component == "minuteofday") {
        return localTime.tm_hour * 60 + localTime.tm_min;
    }
    if (component == "weekday") {
        return localTime.tm_wday == 0 ? 7 : localTime.tm_wday;
    }
    return localTime.tm_hour;
}

bool isFiniteNonZero(double value) {
    return std::isfinite(value) && value != 0.0;
}

double apparentPower(double p, double q) {
    return std::sqrt(p * p + q * q);
}

double powerFactor(double p, double s) {
    if (!isFiniteNonZero(s)) {
        return 0.0;
    }
    return std::abs(p) / s;
}

bool stringEnabled(const std::string& value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const auto ch : value) {
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    const auto first = std::find_if(normalized.begin(), normalized.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    });
    const auto last = std::find_if(normalized.rbegin(), normalized.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base();
    if (first >= last) {
        return false;
    }
    normalized = std::string(first, last);
    if (normalized == "true" || normalized == "yes" || normalized == "on") {
        return true;
    }
    if (normalized == "false" || normalized == "no" || normalized == "off" || normalized.empty()) {
        return false;
    }
    try {
        return std::stod(normalized) != 0.0;
    } catch (...) {
        return false;
    }
}

std::string jsonEscape(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const auto ch : value) {
        switch (ch) {
            case '"':
                result += "\\\"";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                result.push_back(ch);
                break;
        }
    }
    return result;
}

std::string scalarToString(const JsonValue& value) {
    if (value.isString()) {
        return value.asString();
    }
    if (value.isBool()) {
        return value.asBool() ? "true" : "false";
    }
    if (value.isNumber()) {
        const auto number = value.asNumber();
        const auto integer = static_cast<long long>(number);
        if (static_cast<double>(integer) == number) {
            return std::to_string(integer);
        }
        return std::to_string(number);
    }
    return "";
}

std::unordered_map<std::string, std::string> parseParams(const JsonObject& nodeObject) {
    std::unordered_map<std::string, std::string> params;
    const auto* paramsValue = findValue(nodeObject, "params");
    if (paramsValue == nullptr || paramsValue->isNull()) {
        return params;
    }

    const auto& paramsObject = paramsValue->asObject();
    for (const auto& entry : paramsObject.values) {
        if (entry.value->isObject() || entry.value->isArray()) {
            continue;
        }
        params[entry.key] = scalarToString(*entry.value);
    }

    const auto flattenNumberArray = [&](const std::string& name) {
        const auto* value = paramsValue->find(name);
        if (value == nullptr || value->isNull()) {
            return;
        }
        const auto& values = value->asArray().values;
        params[name + ".count"] = std::to_string(values.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            params[name + "." + std::to_string(i)] = scalarToString(*values[i]);
        }
    };
    const char* numberArrays[] = {
        "activeBaseIndexes", "reactiveBaseIndexes",
        "activeInputIndexes", "reactiveInputIndexes",
        "activeOutputIndexes", "reactiveOutputIndexes",
        "loadIndexes", "lowStateClearIndexes", "highStateClearIndexes"
    };
    for (const auto* name : numberArrays) {
        flattenNumberArray(name);
    }

    if (const auto* candidatesValue = paramsValue->find("candidates")) {
        const auto& candidates = candidatesValue->asArray().values;
        params["candidates.count"] = std::to_string(candidates.size());
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const auto prefix = "candidates." + std::to_string(i) + ".";
            const auto& candidate = candidates[i]->asObject();
            const char* fields[] = {
                "name", "target", "merge", "direction", "enableIndex", "enableValue",
                "totalIndex", "defaultValue"
            };
            for (const auto* field : fields) {
                if (const auto* value = findValue(candidate, field)) {
                    params[prefix + field] = scalarToString(*value);
                }
            }
            if (const auto* indexesValue = findValue(candidate, "indexes")) {
                const auto& indexes = indexesValue->asArray().values;
                params[prefix + "indexes.count"] = std::to_string(indexes.size());
                for (std::size_t j = 0; j < indexes.size(); ++j) {
                    params[prefix + "indexes." + std::to_string(j)] = scalarToString(*indexes[j]);
                }
            }
        }
    }

    if (const auto* overrideValue = paramsValue->find("override")) {
        const auto& overrideObject = overrideValue->asObject();
        const char* fields[] = {
            "enableIndex", "enableValue", "activeTotalIndex", "reactiveTotalIndex"
        };
        for (const auto* field : fields) {
            if (const auto* value = findValue(overrideObject, field)) {
                params[std::string("override.") + field] = scalarToString(*value);
            }
        }
    }

    if (const auto* mappingsValue = paramsValue->find("mappings")) {
        const auto& mappings = mappingsValue->asArray().values;
        params["mappings.count"] = std::to_string(mappings.size());
        for (std::size_t i = 0; i < mappings.size(); ++i) {
            const auto& mappingObject = mappings[i]->asObject();
            params["mappings." + std::to_string(i) + ".input"] =
                std::to_string(uint32Value(mappingObject, "input"));
            params["mappings." + std::to_string(i) + ".output"] =
                std::to_string(uint32Value(mappingObject, "output"));
        }
    }

    if (const auto* scheduleValue = paramsValue->find("scheduleCurve")) {
        const auto& schedule = scheduleValue->asArray().values;
        params["scheduleCurve.count"] = std::to_string(schedule.size());
        for (std::size_t i = 0; i < schedule.size(); ++i) {
            const auto& itemObject = schedule[i]->asObject();
            params["scheduleCurve." + std::to_string(i) + ".hour"] =
                std::to_string(uint32Value(itemObject, "hour", static_cast<std::uint32_t>(i)));
            if (const auto* power = findValue(itemObject, "power")) {
                params["scheduleCurve." + std::to_string(i) + ".power"] = scalarToString(*power);
            }
            if (const auto* soc = findValue(itemObject, "soc")) {
                params["scheduleCurve." + std::to_string(i) + ".soc"] = scalarToString(*soc);
            }
            if (const auto* mode = findValue(itemObject, "mode")) {
                params["scheduleCurve." + std::to_string(i) + ".mode"] = scalarToString(*mode);
            }
            if (const auto* chargePower = findValue(itemObject, "chargePower")) {
                params["scheduleCurve." + std::to_string(i) + ".chargePower"] = scalarToString(*chargePower);
            }
            if (const auto* dischargePower = findValue(itemObject, "dischargePower")) {
                params["scheduleCurve." + std::to_string(i) + ".dischargePower"] = scalarToString(*dischargePower);
            }
            if (const auto* targetSoc = findValue(itemObject, "targetSoc")) {
                params["scheduleCurve." + std::to_string(i) + ".targetSoc"] = scalarToString(*targetSoc);
            }
        }
    }

    if (const auto* inputsValue = paramsValue->find("inputs")) {
        const auto& inputs = inputsValue->asArray().values;
        params["inputs.count"] = std::to_string(inputs.size());
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const auto prefix = "inputs." + std::to_string(i) + ".";
            const auto& input = *inputs[i];
            if (input.isObject()) {
                const auto& inputObject = input.asObject();
                if (const auto* index = findValue(inputObject, "index")) {
                    params[prefix + "index"] = scalarToString(*index);
                }
                if (const auto* value = findValue(inputObject, "value")) {
                    params[prefix + "value"] = scalarToString(*value);
                }
                if (const auto* defaultValue = findValue(inputObject, "defaultValue")) {
                    params[prefix + "defaultValue"] = scalarToString(*defaultValue);
                }
            } else if (!input.isNull()) {
                params[prefix + "value"] = scalarToString(input);
            }
        }
    }

    if (const auto* conditionsValue = paramsValue->find("conditions")) {
        const auto& conditions = conditionsValue->asArray().values;
        params["conditions.count"] = std::to_string(conditions.size());
        for (std::size_t i = 0; i < conditions.size(); ++i) {
            const auto prefix = "conditions." + std::to_string(i) + ".";
            const auto& conditionObject = conditions[i]->asObject();
            const char* fields[] = {
                "index",
                "operator",
                "value",
                "valueIndex",
                "tolerance",
                "invert",
                "name"
            };
            for (const auto* field : fields) {
                if (const auto* value = findValue(conditionObject, field)) {
                    params[prefix + field] = scalarToString(*value);
                }
            }
        }
    }

    if (const auto* statesValue = paramsValue->find("states")) {
        const auto& states = statesValue->asArray().values;
        params["states.count"] = std::to_string(states.size());
        for (std::size_t i = 0; i < states.size(); ++i) {
            const auto prefix = "states." + std::to_string(i) + ".";
            const auto& stateObject = states[i]->asObject();
            if (const auto* id = findValue(stateObject, "id")) {
                params[prefix + "id"] = scalarToString(*id);
            }
            if (const auto* name = findValue(stateObject, "name")) {
                params[prefix + "name"] = scalarToString(*name);
            }
        }
    }

    if (const auto* transitionsValue = paramsValue->find("transitions")) {
        const auto& transitions = transitionsValue->asArray().values;
        params["transitions.count"] = std::to_string(transitions.size());
        for (std::size_t i = 0; i < transitions.size(); ++i) {
            const auto prefix = "transitions." + std::to_string(i) + ".";
            const auto& transitionObject = transitions[i]->asObject();
            const char* fields[] = {"from", "to", "name", "combine", "minDurationMs"};
            for (const auto* field : fields) {
                if (const auto* value = findValue(transitionObject, field)) {
                    params[prefix + field] = scalarToString(*value);
                }
            }
            if (const auto* transitionConditions = findValue(transitionObject, "conditions")) {
                const auto& conditions = transitionConditions->asArray().values;
                params[prefix + "conditions.count"] = std::to_string(conditions.size());
                for (std::size_t j = 0; j < conditions.size(); ++j) {
                    const auto conditionPrefix = prefix + "conditions." + std::to_string(j) + ".";
                    const auto& conditionObject = conditions[j]->asObject();
                    const char* conditionFields[] = {
                        "index", "operator", "value", "valueIndex", "tolerance", "invert", "name"
                    };
                    for (const auto* field : conditionFields) {
                        if (const auto* value = findValue(conditionObject, field)) {
                            params[conditionPrefix + field] = scalarToString(*value);
                        }
                    }
                }
            }
        }
    }

    return params;
}

bool isKnownFormulaOperation(const std::string& operation) {
    static const std::set<std::string> known = {
        "add",
        "sum",
        "subtract",
        "multiply",
        "divide",
        "safedivide",
        "min",
        "max",
        "average",
        "abs",
        "negate",
        "square",
        "sqrt",
        "acos",
        "tan",
        "sin",
        "cos",
        "clamp"
    };
    return known.find(normalizedToken(operation)) != known.end();
}

bool isKnownSwitchOperator(const std::string& operation) {
    static const std::set<std::string> known = {
        "gt", ">",
        "gte", ">=",
        "lt", "<",
        "lte", "<=",
        "eq", "==",
        "ne", "!="
    };
    return known.find(normalizedToken(operation)) != known.end();
}

bool evaluateComparison(double left, double right, const std::string& operation, double tolerance) {
    const auto normalized = normalizedToken(operation);
    if (normalized == "gt" || normalized == ">") {
        return left > right;
    }
    if (normalized == "gte" || normalized == ">=") {
        return left >= right;
    }
    if (normalized == "lt" || normalized == "<") {
        return left < right;
    }
    if (normalized == "lte" || normalized == "<=") {
        return left <= right;
    }
    if (normalized == "eq" || normalized == "==") {
        return std::abs(left - right) <= tolerance;
    }
    if (normalized == "ne" || normalized == "!=") {
        return std::abs(left - right) > tolerance;
    }
    throw std::runtime_error("unsupported comparison operator");
}

bool hasParam(const GraphEmsNodeConfig& node, const std::string& key) {
    const auto it = node.params.find(key);
    return it != node.params.end() && !it->second.empty();
}

void validatePositiveIndex(const GraphEmsNodeConfig& node, const std::string& key) {
    const auto it = node.params.find(key);
    if (it == node.params.end() || it->second.empty()) {
        return;
    }
    try {
        std::size_t consumed = 0;
        const auto value = std::stoll(it->second, &consumed);
        if (consumed != it->second.size() || value <= 0 ||
            static_cast<unsigned long long>(value) > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("out of range");
        }
    } catch (...) {
        throw std::runtime_error("generic node " + key + " must be a positive uint32 index");
    }
}

void validateIndexArray(
    const GraphEmsNodeConfig& node,
    const std::string& key,
    std::size_t minCount = 1,
    std::size_t maxCount = 16
) {
    const auto count = paramCount(node, key);
    if (count < minCount || count > maxCount) {
        throw std::runtime_error(key + " requires " + std::to_string(minCount) + " to " +
            std::to_string(maxCount) + " indexes");
    }
    for (std::size_t i = 0; i < count; ++i) {
        validatePositiveIndex(node, key + "." + std::to_string(i));
    }
}

void validateGenericNode(const GraphEmsNodeConfig& node) {
    if (node.type == "timeSource") {
        const auto component = normalizedToken(
            node.params.count("component") ? node.params.at("component") : "hour"
        );
        static const std::set<std::string> supportedComponents = {
            "hour", "minute", "second", "minuteofday", "weekday"
        };
        if (supportedComponents.find(component) == supportedComponents.end()) {
            throw std::runtime_error(
                "timeSource component must be hour, minute, second, minuteOfDay, or weekday"
            );
        }
        if (!hasParam(node, "outputIndex")) {
            throw std::runtime_error("timeSource outputIndex is required");
        }
        validatePositiveIndex(node, "outputIndex");
        return;
    }

    if (node.type == "formula") {
        const auto operationIt = node.params.find("operation");
        if (operationIt == node.params.end() || !isKnownFormulaOperation(operationIt->second)) {
            throw std::runtime_error("formula node has unsupported operation");
        }
        const auto count = paramCount(node, "inputs");
        if (count == 0) {
            throw std::runtime_error("formula node requires inputs");
        }
        const auto operation = normalizedToken(operationIt->second);
        if ((operation == "abs" || operation == "negate" || operation == "square" ||
             operation == "sqrt" || operation == "acos" || operation == "tan" ||
             operation == "sin" || operation == "cos" || operation == "clamp") && count != 1) {
            throw std::runtime_error("formula unary operation requires exactly one input");
        }
        if ((operation == "subtract" || operation == "divide" || operation == "safedivide") && count < 2) {
            throw std::runtime_error("formula operation requires at least two inputs");
        }
        for (std::size_t i = 0; i < count; ++i) {
            const auto prefix = "inputs." + std::to_string(i) + ".";
            const auto hasIndex = hasParam(node, prefix + "index");
            const auto hasValue = hasParam(node, prefix + "value");
            if (hasIndex == hasValue) {
                throw std::runtime_error("formula input requires exactly one index or value");
            }
            validatePositiveIndex(node, prefix + "index");
        }
        if (operation == "clamp") {
            const auto hasLower = hasParam(node, "lower") != hasParam(node, "lowerIndex");
            const auto hasUpper = hasParam(node, "upper") != hasParam(node, "upperIndex");
            if (!hasLower || !hasUpper) {
                throw std::runtime_error("formula clamp requires lower and upper bounds");
            }
            validatePositiveIndex(node, "lowerIndex");
            validatePositiveIndex(node, "upperIndex");
        }
        if (!hasParam(node, "outputIndex")) {
            throw std::runtime_error("formula node outputIndex is required");
        }
        validatePositiveIndex(node, "outputIndex");
        return;
    }

    if (node.type == "windowAggregate") {
        if (!hasParam(node, "inputIndex") || !hasParam(node, "outputIndex")) {
            throw std::runtime_error("windowAggregate inputIndex and outputIndex are required");
        }
        validatePositiveIndex(node, "inputIndex");
        validatePositiveIndex(node, "outputIndex");
        if (paramIndex(node, "inputIndex") == paramIndex(node, "outputIndex")) {
            throw std::runtime_error("windowAggregate inputIndex and outputIndex must differ");
        }
        const auto operationIt = node.params.find("operation");
        const auto operation = operationIt == node.params.end() ? std::string("average") : normalizedToken(operationIt->second);
        if (operation != "average" && operation != "sum" && operation != "min" && operation != "max") {
            throw std::runtime_error("windowAggregate has unsupported operation");
        }
        if (hasParam(node, "windowSize") && hasParam(node, "windowSizeIndex")) {
            throw std::runtime_error("windowAggregate requires only one window size source");
        }
        validatePositiveIndex(node, "windowSizeIndex");
        const auto windowSize = paramDouble(node, "windowSize").value_or(10.0);
        if (!std::isfinite(windowSize) || windowSize < 1.0 || windowSize > 4096.0 || std::floor(windowSize) != windowSize) {
            throw std::runtime_error("windowAggregate windowSize must be an integer from 1 to 4096");
        }
        return;
    }

    if (node.type == "scheduleSelect") {
        const auto count = paramCount(node, "scheduleCurve");
        if (count == 0 || count > 24) {
            throw std::runtime_error("scheduleSelect requires 1 to 24 schedule entries");
        }
        std::set<int> hours;
        for (std::size_t i = 0; i < count; ++i) {
            const auto prefix = "scheduleCurve." + std::to_string(i) + ".";
            const auto hour = paramDouble(node, prefix + "hour");
            if (!hour || !std::isfinite(*hour) || *hour < 0.0 || *hour > 23.0 || std::floor(*hour) != *hour) {
                throw std::runtime_error("scheduleSelect hour must be an integer from 0 to 23");
            }
            if (!hours.insert(static_cast<int>(*hour)).second) {
                throw std::runtime_error("scheduleSelect hours must be unique");
            }
            const char* values[] = {"power", "targetSoc", "soc", "mode"};
            for (const auto* field : values) {
                const auto value = paramDouble(node, prefix + field);
                if (value && !std::isfinite(*value)) {
                    throw std::runtime_error(std::string("scheduleSelect ") + field + " must be finite");
                }
            }
        }
        const bool hasPowerOutput = hasParam(node, "powerOutputIndex");
        const bool hasSocOutput = hasParam(node, "socOutputIndex");
        const bool hasModeOutput = hasParam(node, "modeOutputIndex");
        if (!hasPowerOutput && !hasSocOutput && !hasModeOutput) {
            throw std::runtime_error("scheduleSelect requires at least one output index");
        }
        validatePositiveIndex(node, "powerOutputIndex");
        validatePositiveIndex(node, "socOutputIndex");
        validatePositiveIndex(node, "modeOutputIndex");
        return;
    }

    if (node.type == "phaseArbiter") {
        validateIndexArray(node, "activeOutputIndexes");
        validateIndexArray(node, "reactiveOutputIndexes");
        const auto phaseCount = paramCount(node, "activeOutputIndexes");
        if (paramCount(node, "reactiveOutputIndexes") != phaseCount) {
            throw std::runtime_error("phaseArbiter active/reactive output counts must match");
        }
        const auto validateOptionalPhases = [&](const std::string& key) {
            const auto count = paramCount(node, key);
            if (count == 0) {
                return;
            }
            validateIndexArray(node, key);
            if (count != phaseCount) {
                throw std::runtime_error("phaseArbiter " + key + " count must match outputs");
            }
        };
        validateOptionalPhases("activeBaseIndexes");
        validateOptionalPhases("reactiveBaseIndexes");
        const auto candidateCount = paramCount(node, "candidates");
        if (candidateCount > 64) {
            throw std::runtime_error("phaseArbiter supports at most 64 candidates");
        }
        static const std::set<std::string> targets = {"active", "reactive"};
        static const std::set<std::string> merges = {"max", "min", "add", "override", "stronger"};
        static const std::set<std::string> directions = {"any", "positive", "negative"};
        for (std::size_t i = 0; i < candidateCount; ++i) {
            const auto prefix = "candidates." + std::to_string(i) + ".";
            const auto target = normalizedToken(node.params.count(prefix + "target") ? node.params.at(prefix + "target") : "active");
            const auto merge = normalizedToken(node.params.count(prefix + "merge") ? node.params.at(prefix + "merge") : "stronger");
            const auto direction = normalizedToken(node.params.count(prefix + "direction") ? node.params.at(prefix + "direction") : "any");
            if (targets.find(target) == targets.end() || merges.find(merge) == merges.end() || directions.find(direction) == directions.end()) {
                throw std::runtime_error("phaseArbiter candidate has unsupported target, merge or direction");
            }
            validatePositiveIndex(node, prefix + "enableIndex");
            validatePositiveIndex(node, prefix + "totalIndex");
            const auto indexesCount = paramCount(node, prefix + "indexes");
            const bool hasTotal = hasParam(node, prefix + "totalIndex");
            if (hasTotal == (indexesCount > 0)) {
                throw std::runtime_error("phaseArbiter candidate requires one totalIndex or indexes array");
            }
            if (indexesCount > 0) {
                validateIndexArray(node, prefix + "indexes");
                if (indexesCount != phaseCount) {
                    throw std::runtime_error("phaseArbiter candidate index count must match outputs");
                }
            }
        }
        validatePositiveIndex(node, "override.enableIndex");
        validatePositiveIndex(node, "override.activeTotalIndex");
        validatePositiveIndex(node, "override.reactiveTotalIndex");
        return;
    }

    if (node.type == "powerConstraint") {
        validateIndexArray(node, "activeInputIndexes");
        validateIndexArray(node, "reactiveInputIndexes");
        validateIndexArray(node, "activeOutputIndexes");
        validateIndexArray(node, "reactiveOutputIndexes");
        const auto phaseCount = paramCount(node, "activeInputIndexes");
        if (paramCount(node, "reactiveInputIndexes") != phaseCount ||
            paramCount(node, "activeOutputIndexes") != phaseCount ||
            paramCount(node, "reactiveOutputIndexes") != phaseCount) {
            throw std::runtime_error("powerConstraint input/output phase counts must match");
        }
        if (paramCount(node, "loadIndexes") > 0) {
            validateIndexArray(node, "loadIndexes");
            if (paramCount(node, "loadIndexes") != phaseCount) {
                throw std::runtime_error("powerConstraint loadIndexes count must match phases");
            }
        }
        const char* optionalIndexes[] = {
            "positiveLimitEnableIndex", "positiveLimitIndex",
            "negativeLimitEnableIndex", "negativeLimitIndex",
            "reserveEnableIndex", "reserveMarginIndex", "reserveRunOutputIndex",
            "activeAbsLimitIndex", "reactiveAbsLimitIndex", "apparentTotalLimitIndex",
            "positiveTotalLimitIndex", "negativeTotalLimitIndex",
            "stateIndex", "stateUpperIndex", "stateLowerIndex"
        };
        for (const auto* key : optionalIndexes) {
            validatePositiveIndex(node, key);
        }
        const auto validateClearIndexes = [&](const std::string& key) {
            const auto count = paramCount(node, key);
            if (count > 32) {
                throw std::runtime_error("powerConstraint " + key + " supports at most 32 indexes");
            }
            for (std::size_t i = 0; i < count; ++i) {
                validatePositiveIndex(node, key + "." + std::to_string(i));
            }
        };
        validateClearIndexes("lowStateClearIndexes");
        validateClearIndexes("highStateClearIndexes");
        return;
    }

    if (node.type == "switch") {
        const auto hasConditionIndex = hasParam(node, "conditionIndex");
        validatePositiveIndex(node, "conditionIndex");
        if (!hasConditionIndex) {
            const auto operationIt = node.params.find("operator");
            if (operationIt == node.params.end() || !isKnownSwitchOperator(operationIt->second)) {
                throw std::runtime_error("switch node has unsupported operator");
            }
            if (hasParam(node, "leftIndex") == hasParam(node, "leftValue") ||
                hasParam(node, "rightIndex") == hasParam(node, "rightValue")) {
                throw std::runtime_error("switch comparison requires left and right operands");
            }
            validatePositiveIndex(node, "leftIndex");
            validatePositiveIndex(node, "rightIndex");
        }
        if (hasParam(node, "trueIndex") == hasParam(node, "trueValue") ||
            hasParam(node, "falseIndex") == hasParam(node, "falseValue")) {
            throw std::runtime_error("switch node requires true and false values");
        }
        validatePositiveIndex(node, "trueIndex");
        validatePositiveIndex(node, "falseIndex");
        if (!hasParam(node, "outputIndex")) {
            throw std::runtime_error("switch node outputIndex is required");
        }
        validatePositiveIndex(node, "outputIndex");
        return;
    }

    if (node.type == "controlGate") {
        const auto combineIt = node.params.find("combine");
        const auto combine = combineIt == node.params.end() ? std::string("all") : normalizedToken(combineIt->second);
        if (combine != "all" && combine != "any") {
            throw std::runtime_error("controlGate combine must be all or any");
        }
        const auto count = paramCount(node, "conditions");
        if (count == 0 || count > 64) {
            throw std::runtime_error("controlGate requires 1 to 64 conditions");
        }
        for (std::size_t i = 0; i < count; ++i) {
            const auto prefix = "conditions." + std::to_string(i) + ".";
            if (!hasParam(node, prefix + "index")) {
                throw std::runtime_error("controlGate condition index is required");
            }
            validatePositiveIndex(node, prefix + "index");
            const auto operationIt = node.params.find(prefix + "operator");
            if (operationIt == node.params.end() || !isKnownSwitchOperator(operationIt->second)) {
                throw std::runtime_error("controlGate condition has unsupported operator");
            }
            if (hasParam(node, prefix + "value") == hasParam(node, prefix + "valueIndex")) {
                throw std::runtime_error("controlGate condition requires one value or valueIndex");
            }
            validatePositiveIndex(node, prefix + "valueIndex");
            if (const auto tolerance = paramDouble(node, prefix + "tolerance")) {
                if (!std::isfinite(*tolerance) || *tolerance < 0.0) {
                    throw std::runtime_error("controlGate tolerance must be a non-negative number");
                }
            }
        }
        if (!hasParam(node, "outputIndex")) {
            throw std::runtime_error("controlGate outputIndex is required");
        }
        validatePositiveIndex(node, "outputIndex");
        return;
    }

    if (node.type == "feedbackVerify") {
        const char* requiredIndexes[] = {"targetIndex", "feedbackIndex", "outputIndex"};
        for (const auto* key : requiredIndexes) {
            if (!hasParam(node, key)) {
                throw std::runtime_error(std::string("feedbackVerify ") + key + " is required");
            }
            validatePositiveIndex(node, key);
        }
        const auto tolerance = paramDouble(node, "tolerance").value_or(0.0);
        const auto targetChangeTolerance = paramDouble(node, "targetChangeTolerance").value_or(1e-9);
        const auto timeoutMs = paramDouble(node, "timeoutMs").value_or(5000.0);
        if (!std::isfinite(tolerance) || tolerance < 0.0) {
            throw std::runtime_error("feedbackVerify tolerance must be a non-negative number");
        }
        if (!std::isfinite(targetChangeTolerance) || targetChangeTolerance < 0.0) {
            throw std::runtime_error("feedbackVerify targetChangeTolerance must be a non-negative number");
        }
        if (!std::isfinite(timeoutMs) || timeoutMs < 1.0 || timeoutMs > 86400000.0 ||
            std::floor(timeoutMs) != timeoutMs) {
            throw std::runtime_error("feedbackVerify timeoutMs must be an integer from 1 to 86400000");
        }
        return;
    }

    if (node.type == "controlWrite") {
        const char* requiredIndexes[] = {"inputIndex", "targetIndex"};
        for (const auto* key : requiredIndexes) {
            if (!hasParam(node, key)) {
                throw std::runtime_error(std::string("controlWrite ") + key + " is required");
            }
            validatePositiveIndex(node, key);
        }
        if (!hasParam(node, "minValue") || !hasParam(node, "maxValue")) {
            throw std::runtime_error("controlWrite minValue and maxValue are required");
        }
        const auto minValue = paramDouble(node, "minValue").value();
        const auto maxValue = paramDouble(node, "maxValue").value();
        if (!std::isfinite(minValue) || !std::isfinite(maxValue) || minValue > maxValue) {
            throw std::runtime_error("controlWrite bounds must be finite and minValue <= maxValue");
        }
        const auto deadband = paramDouble(node, "deadband").value_or(0.0);
        if (!std::isfinite(deadband) || deadband < 0.0) {
            throw std::runtime_error("controlWrite deadband must be a non-negative number");
        }
        validatePositiveIndex(node, "permitIndex");
        if (const auto permitValue = paramDouble(node, "permitValue")) {
            if (!std::isfinite(*permitValue)) {
                throw std::runtime_error("controlWrite permitValue must be finite");
            }
        }
        if (const auto permitTolerance = paramDouble(node, "permitTolerance")) {
            if (!std::isfinite(*permitTolerance) || *permitTolerance < 0.0) {
                throw std::runtime_error("controlWrite permitTolerance must be a non-negative number");
            }
        }
        const auto valueModeIt = node.params.find("valueMode");
        if (valueModeIt != node.params.end()) {
            const auto valueMode = normalizedToken(valueModeIt->second);
            if (valueMode != "none" && valueMode != "truncate" && valueMode != "round") {
                throw std::runtime_error("controlWrite valueMode must be none, truncate or round");
            }
        }
        return;
    }

    if (node.type == "rateLimit") {
        const char* requiredIndexes[] = {"inputIndex", "outputIndex"};
        for (const auto* key : requiredIndexes) {
            if (!hasParam(node, key)) {
                throw std::runtime_error(std::string("rateLimit ") + key + " is required");
            }
            validatePositiveIndex(node, key);
        }
        if (paramIndex(node, "inputIndex") == paramIndex(node, "outputIndex")) {
            throw std::runtime_error("rateLimit inputIndex and outputIndex must differ");
        }
        const char* requiredNumbers[] = {"risePerSecond", "fallPerSecond", "minValue", "maxValue"};
        for (const auto* key : requiredNumbers) {
            if (!hasParam(node, key)) {
                throw std::runtime_error(std::string("rateLimit ") + key + " is required");
            }
        }
        const auto rise = paramDouble(node, "risePerSecond").value();
        const auto fall = paramDouble(node, "fallPerSecond").value();
        const auto minValue = paramDouble(node, "minValue").value();
        const auto maxValue = paramDouble(node, "maxValue").value();
        const auto initialValue = paramDouble(node, "initialValue").value_or(0.0);
        if (!std::isfinite(rise) || rise <= 0.0 || !std::isfinite(fall) || fall <= 0.0) {
            throw std::runtime_error("rateLimit risePerSecond and fallPerSecond must be positive numbers");
        }
        if (!std::isfinite(minValue) || !std::isfinite(maxValue) || minValue > maxValue) {
            throw std::runtime_error("rateLimit bounds must be finite and minValue <= maxValue");
        }
        if (!std::isfinite(initialValue) || initialValue < minValue || initialValue > maxValue) {
            throw std::runtime_error("rateLimit initialValue must be within configured bounds");
        }
        return;
    }

    if (node.type == "hysteresis") {
        const char* requiredIndexes[] = {"inputIndex", "outputIndex"};
        for (const auto* key : requiredIndexes) {
            if (!hasParam(node, key)) {
                throw std::runtime_error(std::string("hysteresis ") + key + " is required");
            }
            validatePositiveIndex(node, key);
        }
        if (!hasParam(node, "lowThreshold") || !hasParam(node, "highThreshold")) {
            throw std::runtime_error("hysteresis lowThreshold and highThreshold are required");
        }
        const auto low = paramDouble(node, "lowThreshold").value();
        const auto high = paramDouble(node, "highThreshold").value();
        if (!std::isfinite(low) || !std::isfinite(high) || low >= high) {
            throw std::runtime_error("hysteresis thresholds must be finite and lowThreshold < highThreshold");
        }
        return;
    }

    if (node.type == "debounce") {
        const char* requiredIndexes[] = {"inputIndex", "outputIndex"};
        for (const auto* key : requiredIndexes) {
            if (!hasParam(node, key)) {
                throw std::runtime_error(std::string("debounce ") + key + " is required");
            }
            validatePositiveIndex(node, key);
        }
        const char* delayKeys[] = {"onDelayMs", "offDelayMs"};
        for (const auto* key : delayKeys) {
            const auto delay = paramDouble(node, key).value_or(0.0);
            if (!std::isfinite(delay) || delay < 0.0 || delay > 86400000.0 || std::floor(delay) != delay) {
                throw std::runtime_error(std::string("debounce ") + key + " must be an integer from 0 to 86400000");
            }
        }
        return;
    }

    if (node.type == "sequence") {
        if (!hasParam(node, "stateOutputIndex")) {
            throw std::runtime_error("sequence stateOutputIndex is required");
        }
        validatePositiveIndex(node, "stateOutputIndex");
        const auto stateCount = paramCount(node, "states");
        if (stateCount == 0 || stateCount > 32) {
            throw std::runtime_error("sequence requires 1 to 32 states");
        }
        std::set<int> stateIds;
        for (std::size_t i = 0; i < stateCount; ++i) {
            const auto key = "states." + std::to_string(i) + ".id";
            if (!hasParam(node, key)) {
                throw std::runtime_error("sequence state id is required");
            }
            const auto raw = paramDouble(node, key).value();
            if (!std::isfinite(raw) || raw < 0.0 || raw > 65535.0 || std::floor(raw) != raw ||
                !stateIds.insert(static_cast<int>(raw)).second) {
                throw std::runtime_error("sequence state ids must be unique integers from 0 to 65535");
            }
        }
        const auto initialRaw = paramDouble(node, "initialState").value_or(static_cast<double>(*stateIds.begin()));
        if (!std::isfinite(initialRaw) || std::floor(initialRaw) != initialRaw ||
            stateIds.find(static_cast<int>(initialRaw)) == stateIds.end()) {
            throw std::runtime_error("sequence initialState must reference a configured state");
        }
        const auto transitionCount = paramCount(node, "transitions");
        if (transitionCount > 128) {
            throw std::runtime_error("sequence supports at most 128 transitions");
        }
        for (std::size_t i = 0; i < transitionCount; ++i) {
            const auto prefix = "transitions." + std::to_string(i) + ".";
            const auto from = paramDouble(node, prefix + "from");
            const auto to = paramDouble(node, prefix + "to");
            if (!from || !to || std::floor(*from) != *from || std::floor(*to) != *to ||
                stateIds.find(static_cast<int>(*from)) == stateIds.end() ||
                stateIds.find(static_cast<int>(*to)) == stateIds.end()) {
                throw std::runtime_error("sequence transition from/to must reference configured states");
            }
            const auto combineIt = node.params.find(prefix + "combine");
            const auto combine = combineIt == node.params.end() ? std::string("all") : normalizedToken(combineIt->second);
            if (combine != "all" && combine != "any") {
                throw std::runtime_error("sequence transition combine must be all or any");
            }
            const auto duration = paramDouble(node, prefix + "minDurationMs").value_or(0.0);
            if (!std::isfinite(duration) || duration < 0.0 || duration > 86400000.0 || std::floor(duration) != duration) {
                throw std::runtime_error("sequence minDurationMs must be an integer from 0 to 86400000");
            }
            const auto conditionCount = paramCount(node, prefix + "conditions");
            if (conditionCount == 0 || conditionCount > 64) {
                throw std::runtime_error("sequence transition requires 1 to 64 conditions");
            }
            for (std::size_t j = 0; j < conditionCount; ++j) {
                const auto conditionPrefix = prefix + "conditions." + std::to_string(j) + ".";
                if (!hasParam(node, conditionPrefix + "index")) {
                    throw std::runtime_error("sequence transition condition index is required");
                }
                validatePositiveIndex(node, conditionPrefix + "index");
                const auto operationIt = node.params.find(conditionPrefix + "operator");
                if (operationIt == node.params.end() || !isKnownSwitchOperator(operationIt->second)) {
                    throw std::runtime_error("sequence transition condition has unsupported operator");
                }
                if (hasParam(node, conditionPrefix + "value") == hasParam(node, conditionPrefix + "valueIndex")) {
                    throw std::runtime_error("sequence transition condition requires one value or valueIndex");
                }
                validatePositiveIndex(node, conditionPrefix + "valueIndex");
                const auto tolerance = paramDouble(node, conditionPrefix + "tolerance").value_or(1e-9);
                if (!std::isfinite(tolerance) || tolerance < 0.0) {
                    throw std::runtime_error("sequence transition tolerance must be non-negative");
                }
            }
        }
        return;
    }

    if (node.type == "pcsWriteback" || node.type == "pcsPowerSolve") {
        validatePositiveIndex(node, "permitIndex");
        if (const auto permitValue = paramDouble(node, "permitValue")) {
            if (!std::isfinite(*permitValue)) {
                throw std::runtime_error("PCS permitValue must be finite");
            }
        }
        if (const auto permitTolerance = paramDouble(node, "permitTolerance")) {
            if (!std::isfinite(*permitTolerance) || *permitTolerance < 0.0) {
                throw std::runtime_error("PCS permitTolerance must be a non-negative number");
            }
        }
    }
}

bool isKnownNodeType(const std::string& type) {
    static const std::set<std::string> known = {
        "pointInput",
        "meterAverage",
        "derivedLoad",
        "bmsDerived",
        "cosCompensation",
        "voltageCompensation",
        "chargeDischarge",
        "chargeDischargeCycleTest",
        "timedChargeDischarge",
        "photovoltaicCharge",
        "phaseBalance",
        "skOverride",
        "reserveCapacity",
        "pcsPowerSolve",
        "pcsWriteback",
        "formula",
        "timeSource",
        "windowAggregate",
        "scheduleSelect",
        "phaseArbiter",
        "powerConstraint",
        "switch",
        "controlGate",
        "feedbackVerify",
        "controlWrite",
        "rateLimit",
        "hysteresis",
        "debounce",
        "sequence"
    };
    return known.find(type) != known.end();
}

void visitNode(
    const std::string& id,
    const std::unordered_map<std::string, std::vector<std::string>>& adjacency,
    std::unordered_set<std::string>& visiting,
    std::unordered_set<std::string>& visited
) {
    if (visited.find(id) != visited.end()) {
        return;
    }
    if (visiting.find(id) != visiting.end()) {
        throw std::runtime_error("graph contains cycle");
    }
    visiting.insert(id);
    const auto it = adjacency.find(id);
    if (it != adjacency.end()) {
        for (const auto& next : it->second) {
            visitNode(next, adjacency, visiting, visited);
        }
    }
    visiting.erase(id);
    visited.insert(id);
}

std::vector<const GraphEmsNodeConfig*> executionOrder(const GraphEmsConfig& config) {
    std::vector<const GraphEmsNodeConfig*> ordered;
    ordered.reserve(config.nodes.size());
    if (config.edges.empty()) {
        for (const auto& node : config.nodes) {
            ordered.push_back(&node);
        }
        return ordered;
    }

    std::unordered_map<std::string, const GraphEmsNodeConfig*> nodeById;
    std::unordered_map<std::string, std::vector<std::string>> outgoing;
    std::unordered_map<std::string, std::size_t> indegree;
    for (const auto& node : config.nodes) {
        nodeById[node.id] = &node;
        indegree[node.id] = 0;
    }
    for (const auto& edge : config.edges) {
        outgoing[edge.from].push_back(edge.to);
        ++indegree[edge.to];
    }

    std::vector<std::string> ready;
    ready.reserve(config.nodes.size());
    for (const auto& node : config.nodes) {
        if (indegree[node.id] == 0) {
            ready.push_back(node.id);
        }
    }

    std::size_t cursor = 0;
    while (cursor < ready.size()) {
        const auto id = ready[cursor++];
        ordered.push_back(nodeById[id]);
        const auto outgoingIt = outgoing.find(id);
        if (outgoingIt == outgoing.end()) {
            continue;
        }
        for (const auto& next : outgoingIt->second) {
            auto& nextIndegree = indegree[next];
            if (nextIndegree > 0) {
                --nextIndegree;
            }
            if (nextIndegree == 0) {
                ready.push_back(next);
            }
        }
    }

    if (ordered.size() != config.nodes.size()) {
        throw std::runtime_error("graph contains cycle");
    }
    return ordered;
}

void validateGraph(const GraphEmsConfig& config) {
    if (config.schemaVersion.empty() || (config.schemaVersion != "1" && config.schemaVersion.rfind("1.", 0) != 0)) {
        throw std::runtime_error("unsupported graph schemaVersion; only major version 1 is supported");
    }
    if (config.maxNodes == 0 || config.maxNodes > 1024 || config.maxEdges == 0 || config.maxEdges > 4096) {
        throw std::runtime_error("graph limits exceed supported range");
    }
    if (config.nodes.size() > config.maxNodes) {
        throw std::runtime_error("graph node count exceeds maxNodes");
    }
    if (config.edges.size() > config.maxEdges) {
        throw std::runtime_error("graph edge count exceeds maxEdges");
    }
    std::unordered_set<std::string> ids;
    for (const auto& node : config.nodes) {
        if (node.id.empty()) {
            throw std::runtime_error("graph node id is required");
        }
        if (!ids.insert(node.id).second) {
            throw std::runtime_error("duplicate graph node id");
        }
        if (!isKnownNodeType(node.type)) {
            throw std::runtime_error("unknown graph node type");
        }
        validateGenericNode(node);
    }

    std::unordered_map<std::string, std::vector<std::string>> adjacency;
    for (const auto& edge : config.edges) {
        if (ids.find(edge.from) == ids.end() || ids.find(edge.to) == ids.end()) {
            throw std::runtime_error("graph edge references unknown node");
        }
        adjacency[edge.from].push_back(edge.to);
    }

    std::unordered_set<std::string> visiting;
    std::unordered_set<std::string> visited;
    for (const auto& id : ids) {
        visitNode(id, adjacency, visiting, visited);
    }
}

}  // namespace

GraphEmsConfig GraphEmsConfig::loadFromFile(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open graph EMS config file: " + path);
    }

    std::stringstream buffer;
    buffer << input.rdbuf();
    const auto root = JsonParser(buffer.str()).parse();
    const auto& object = root.asObject();

    GraphEmsConfig config;
    config.schemaVersion = stringValue(object, "schemaVersion", config.schemaVersion);
    config.graphCode = stringValue(object, "graphCode", config.graphCode);
    if (const auto* limitsValue = findValue(object, "limits")) {
        const auto& limits = limitsValue->asObject();
        config.maxNodes = static_cast<std::size_t>(uint32Value(
            limits,
            "maxNodes",
            static_cast<std::uint32_t>(config.maxNodes)
        ));
        config.maxEdges = static_cast<std::size_t>(uint32Value(
            limits,
            "maxEdges",
            static_cast<std::uint32_t>(config.maxEdges)
        ));
    }

    if (const auto* nodes = findValue(object, "nodes")) {
        for (const auto& item : nodes->asArray().values) {
            const auto& nodeObject = item->asObject();
            GraphEmsNodeConfig node;
            node.id = stringValue(nodeObject, "id");
            node.type = stringValue(nodeObject, "type");
            node.enabled = boolValue(nodeObject, "enabled", node.enabled);
            node.params = parseParams(nodeObject);
            config.nodes.push_back(std::move(node));
        }
    }

    if (const auto* edges = findValue(object, "edges")) {
        for (const auto& item : edges->asArray().values) {
            const auto& edgeObject = item->asObject();
            GraphEmsEdgeConfig edge;
            edge.from = stringValue(edgeObject, "from");
            edge.to = stringValue(edgeObject, "to");
            config.edges.push_back(std::move(edge));
        }
    }

    validateGraph(config);
    return config;
}

GraphEmsEngine::GraphEmsEngine(
    GraphEmsConfig config,
    PointStoreRouter& router,
    std::int64_t defaultTtlMs,
    std::string stateFile,
    std::unordered_map<std::string, std::string> profile
) : config_(std::move(config)),
    router_(router),
    defaultTtlMs_(defaultTtlMs),
    stateFile_(std::move(stateFile)),
    profile_(std::move(profile)) {
    executionOrder_ = executionOrder(config_);

    std::set<std::uint32_t> indexes;
    for (const auto& node : config_.nodes) {
        if (!shouldRunNode(node)) {
            continue;
        }
        for (const auto& param : node.params) {
            try {
                std::size_t consumed = 0;
                const auto candidate = std::stoull(param.second, &consumed);
                if (consumed == param.second.size() && candidate > 0 &&
                    candidate <= std::numeric_limits<std::uint32_t>::max() &&
                    router_.routeByIndex(static_cast<std::uint32_t>(candidate))) {
                    indexes.insert(static_cast<std::uint32_t>(candidate));
                }
            } catch (...) {
            }
        }
    }
    const auto outputIndexes = stateOutputIndexes();
    indexes.insert(outputIndexes.begin(), outputIndexes.end());
    snapshotIndexes_.assign(indexes.begin(), indexes.end());
}

GraphEmsRunResult GraphEmsEngine::runOnce(std::int64_t nowMs) {
    GraphEmsRunResult result;
    if (!stateRestored_) {
        try {
            restoreState(nowMs);
        } catch (const std::exception& ex) {
            result.errors.push_back(std::string("restoreState: ") + ex.what());
        }
        stateRestored_ = true;
    }
    snapshotValues_.clear();
    snapshotResolvedIndexes_.clear();
    const auto snapshot = router_.getLatestByIndexes(snapshotIndexes_, nowMs);
    snapshotResolvedIndexes_.insert(snapshotIndexes_.begin(), snapshotIndexes_.end());
    for (const auto& value : snapshot) {
        if (value.quality == 1 && !value.stale) {
            snapshotValues_[value.index] = value.value;
        }
    }
    snapshotActive_ = true;

    for (const auto* nodePtr : executionOrder_) {
        const auto& node = *nodePtr;
        if (!shouldRunNode(node)) {
            continue;
        }
        try {
            if (node.type == "meterAverage") {
                runMeterAverage(node, nowMs, result);
            } else if (node.type == "derivedLoad") {
                runDerivedLoad(node, nowMs, result);
            } else if (node.type == "bmsDerived") {
                runBmsDerived(node, nowMs, result);
            } else if (node.type == "cosCompensation") {
                runCosCompensation(node, nowMs, result);
            } else if (node.type == "voltageCompensation") {
                runVoltageCompensation(node, nowMs, result);
            } else if (node.type == "chargeDischarge") {
                runChargeDischarge(node, nowMs, result);
            } else if (node.type == "chargeDischargeCycleTest") {
                runChargeDischargeCycleTest(node, nowMs, result);
            } else if (node.type == "timedChargeDischarge") {
                runTimedChargeDischarge(node, nowMs, result);
            } else if (node.type == "photovoltaicCharge") {
                runPhotovoltaicCharge(node, nowMs, result);
            } else if (node.type == "phaseBalance") {
                runPhaseBalance(node, nowMs, result);
            } else if (node.type == "skOverride") {
                runSkOverride(node, nowMs, result);
            } else if (node.type == "reserveCapacity") {
                runReserveCapacity(node, nowMs, result);
            } else if (node.type == "formula") {
                runFormula(node, nowMs, result);
            } else if (node.type == "timeSource") {
                runTimeSource(node, nowMs, result);
            } else if (node.type == "windowAggregate") {
                runWindowAggregate(node, nowMs, result);
            } else if (node.type == "scheduleSelect") {
                runScheduleSelect(node, nowMs, result);
            } else if (node.type == "phaseArbiter") {
                runPhaseArbiter(node, nowMs, result);
            } else if (node.type == "powerConstraint") {
                runPowerConstraint(node, nowMs, result);
            } else if (node.type == "switch") {
                runSwitch(node, nowMs, result);
            } else if (node.type == "controlGate") {
                runControlGate(node, nowMs, result);
            } else if (node.type == "feedbackVerify") {
                runFeedbackVerify(node, nowMs, result);
            } else if (node.type == "controlWrite") {
                runControlWrite(node, nowMs, result);
            } else if (node.type == "rateLimit") {
                runRateLimit(node, nowMs, result);
            } else if (node.type == "hysteresis") {
                runHysteresis(node, nowMs, result);
            } else if (node.type == "debounce") {
                runDebounce(node, nowMs, result);
            } else if (node.type == "sequence") {
                runSequence(node, nowMs, result);
            } else if (node.type == "pcsPowerSolve") {
                runPcsPowerSolve(node, nowMs, result);
            } else if (node.type == "pcsWriteback") {
                runPcsWriteback(node, nowMs, result);
            }
        } catch (const std::exception& ex) {
            result.errors.push_back(node.id + ": " + ex.what());
        }
    }
    try {
        saveState(nowMs);
    } catch (const std::exception& ex) {
        result.errors.push_back(std::string("saveState: ") + ex.what());
    }
    snapshotActive_ = false;
    return result;
}

Optional<double> GraphEmsEngine::latestValue(std::uint32_t index, std::int64_t nowMs) const {
    if (snapshotActive_) {
        const auto cached = snapshotValues_.find(index);
        if (cached != snapshotValues_.end()) {
            return cached->second;
        }
        if (snapshotResolvedIndexes_.find(index) != snapshotResolvedIndexes_.end()) {
            return NullOpt;
        }
    }
    const auto latest = router_.getLatestByIndex(index, nowMs);
    if (!latest || latest->quality != 1 || latest->stale) {
        if (snapshotActive_) {
            snapshotResolvedIndexes_.insert(index);
        }
        return NullOpt;
    }
    if (snapshotActive_) {
        snapshotResolvedIndexes_.insert(index);
        snapshotValues_[index] = latest->value;
    }
    return latest->value;
}

CommandSubmitResult GraphEmsEngine::set(std::uint32_t index, double value, std::int64_t nowMs) {
    PointValue point;
    point.index = index;
    point.value = value;
    point.quality = 1;
    point.ts = nowMs;
    point.expireAt = nowMs + defaultTtlMs_;
    auto result = router_.putLatestByIndex(point);
    if (result.accepted && snapshotActive_) {
        snapshotResolvedIndexes_.insert(index);
        snapshotValues_[index] = value;
    }
    return result;
}

bool GraphEmsEngine::profileEnabled(const std::string& key, bool defaultValue) const {
    const auto it = profile_.find(key);
    if (it == profile_.end()) {
        return defaultValue;
    }
    return stringEnabled(it->second);
}

int GraphEmsEngine::profileInt(const std::string& key, int defaultValue) const {
    const auto it = profile_.find(key);
    if (it == profile_.end()) {
        return defaultValue;
    }
    try {
        return std::stoi(it->second);
    } catch (...) {
        return defaultValue;
    }
}

bool GraphEmsEngine::shouldRunNode(const GraphEmsNodeConfig& node) const {
    if (!node.enabled) {
        return false;
    }
    const char* keys[] = {"profileKey", "profileKey2", "profileKey3", "profileKey4"};
    for (const auto* key : keys) {
        const auto keyIt = node.params.find(key);
        if (keyIt != node.params.end() && !keyIt->second.empty() &&
            !profileEnabled(keyIt->second, true)) {
            return false;
        }
    }
    const char* optionalKeys[] = {
        "optionalProfileKey",
        "optionalProfileKey2",
        "optionalProfileKey3",
        "optionalProfileKey4"
    };
    for (const auto* key : optionalKeys) {
        const auto keyIt = node.params.find(key);
        if (keyIt != node.params.end() && !keyIt->second.empty() &&
            !profileEnabled(keyIt->second, false)) {
            return false;
        }
    }
    const char* disabledKeys[] = {
        "profileDisabledKey",
        "profileDisabledKey2",
        "profileDisabledKey3",
        "profileDisabledKey4"
    };
    for (const auto* key : disabledKeys) {
        const auto keyIt = node.params.find(key);
        if (keyIt != node.params.end() && !keyIt->second.empty() &&
            profileEnabled(keyIt->second, false)) {
            return false;
        }
    }
    const auto profileIntKey = node.params.find("profileIntKey");
    if (profileIntKey != node.params.end() && !profileIntKey->second.empty()) {
        const auto actual = profileInt(profileIntKey->second, 0);
        bool matches = false;
        const char* valueKeys[] = {"profileIntValue", "profileIntValue2", "profileIntValue3", "profileIntValue4"};
        for (const auto* valueKey : valueKeys) {
            const auto value = node.params.find(valueKey);
            if (value != node.params.end() && !value->second.empty() && actual == std::stoi(value->second)) {
                matches = true;
            }
        }
        if (!matches) {
            return false;
        }
    }
    return true;
}

void GraphEmsEngine::restoreState(std::int64_t nowMs) {
    if (stateFile_.empty()) {
        return;
    }
    std::ifstream input(stateFile_.c_str(), std::ios::in | std::ios::binary);
    if (!input.is_open()) {
        return;
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    const auto root = JsonParser(buffer.str()).parse();
    const auto& object = root.asObject();
    const auto graphCode = stringValue(object, "graphCode");
    if (!graphCode.empty() && !config_.graphCode.empty() && graphCode != config_.graphCode) {
        return;
    }
    const auto* pointsValue = findValue(object, "points");
    const auto allowedIndexes = stateOutputIndexes();
    const std::set<std::uint32_t> allowed(allowedIndexes.begin(), allowedIndexes.end());
    if (pointsValue != nullptr && !pointsValue->isNull()) {
        for (const auto& item : pointsValue->asArray().values) {
            const auto& pointObject = item->asObject();
            const auto index = uint32Value(pointObject, "index");
            const auto* value = findValue(pointObject, "value");
            if (index == 0 || allowed.find(index) == allowed.end() || value == nullptr || !value->isNumber() ||
                !std::isfinite(value->asNumber())) {
                continue;
            }
            set(index, value->asNumber(), nowMs);
        }
    }

    const auto* runtimeValue = findValue(object, "runtime");
    if (runtimeValue == nullptr || runtimeValue->isNull()) {
        stateSaved_ = true;
        lastStateSaveAt_ = nowMs;
        return;
    }
    const auto& runtime = runtimeValue->asObject();
    const auto findNode = [&](const std::string& id, const std::string& type) -> const GraphEmsNodeConfig* {
        for (const auto& node : config_.nodes) {
            if (node.id == id && node.type == type && shouldRunNode(node)) {
                return &node;
            }
        }
        return nullptr;
    };

    if (const auto* windows = findValue(runtime, "averageWindows")) {
        for (const auto& item : windows->asArray().values) {
            const auto& windowObject = item->asObject();
            const auto index = uint32Value(windowObject, "index");
            const auto* values = findValue(windowObject, "values");
            if (index == 0 || allowed.find(index) == allowed.end() || values == nullptr) {
                continue;
            }
            std::vector<double> restored;
            for (const auto& value : values->asArray().values) {
                if (restored.size() >= 4096 || !value->isNumber() || !std::isfinite(value->asNumber())) {
                    continue;
                }
                restored.push_back(value->asNumber());
            }
            if (!restored.empty()) {
                averageWindows_[index] = std::move(restored);
            }
        }
    }

    if (const auto* states = findValue(runtime, "feedbackVerify")) {
        for (const auto& item : states->asArray().values) {
            const auto& stateObject = item->asObject();
            const auto nodeId = stringValue(stateObject, "nodeId");
            const auto target = numberValue(stateObject, "target");
            if (findNode(nodeId, "feedbackVerify") == nullptr || !std::isfinite(target)) {
                continue;
            }
            FeedbackVerifyState state;
            state.initialized = boolValue(stateObject, "initialized", true);
            state.target = target;
            state.targetChangedAt = restoredTimestamp(nowMs, numberValue(stateObject, "targetAgeMs"));
            feedbackVerifyStates_[nodeId] = state;
        }
    }

    if (const auto* states = findValue(runtime, "rateLimit")) {
        for (const auto& item : states->asArray().values) {
            const auto& stateObject = item->asObject();
            const auto nodeId = stringValue(stateObject, "nodeId");
            const auto output = numberValue(stateObject, "output");
            if (findNode(nodeId, "rateLimit") == nullptr || !std::isfinite(output)) {
                continue;
            }
            RateLimitState state;
            state.initialized = boolValue(stateObject, "initialized", true);
            state.output = output;
            state.lastRunAt = restoredTimestamp(nowMs, numberValue(stateObject, "lastRunAgeMs"));
            rateLimitStates_[nodeId] = state;
        }
    }

    const auto restoreBooleanStates = [&](const char* key, const std::string& type,
                                          std::unordered_map<std::string, BooleanFilterState>& target) {
        const auto* states = findValue(runtime, key);
        if (states == nullptr) {
            return;
        }
        for (const auto& item : states->asArray().values) {
            const auto& stateObject = item->asObject();
            const auto nodeId = stringValue(stateObject, "nodeId");
            if (findNode(nodeId, type) == nullptr) {
                continue;
            }
            BooleanFilterState state;
            state.initialized = boolValue(stateObject, "initialized", true);
            state.output = boolValue(stateObject, "output", false);
            state.candidate = boolValue(stateObject, "candidate", state.output);
            state.candidateSince = restoredTimestamp(nowMs, numberValue(stateObject, "candidateAgeMs"));
            target[nodeId] = state;
        }
    };
    restoreBooleanStates("hysteresis", "hysteresis", hysteresisStates_);
    restoreBooleanStates("debounce", "debounce", debounceStates_);

    if (const auto* states = findValue(runtime, "sequence")) {
        for (const auto& item : states->asArray().values) {
            const auto& stateObject = item->asObject();
            const auto nodeId = stringValue(stateObject, "nodeId");
            const auto* node = findNode(nodeId, "sequence");
            const auto current = static_cast<int>(numberValue(stateObject, "current", -1.0));
            if (node == nullptr) {
                continue;
            }
            bool configured = false;
            const auto count = paramCount(*node, "states");
            for (std::size_t i = 0; i < count; ++i) {
                if (static_cast<int>(paramDouble(*node, "states." + std::to_string(i) + ".id").value()) == current) {
                    configured = true;
                    break;
                }
            }
            if (!configured) {
                continue;
            }
            SequenceState state;
            state.initialized = boolValue(stateObject, "initialized", true);
            state.current = current;
            state.enteredAt = restoredTimestamp(nowMs, numberValue(stateObject, "enteredAgeMs"));
            sequenceStates_[nodeId] = state;
        }
    }
    stateSaved_ = true;
    lastStateSaveAt_ = nowMs;
}

void GraphEmsEngine::saveState(std::int64_t nowMs) {
    if (stateFile_.empty()) {
        return;
    }
    const auto saveIntervalMs = std::max(0, profileInt("stateSaveIntervalMs", 5000));
    if (stateSaved_ && nowMs >= lastStateSaveAt_ && nowMs - lastStateSaveAt_ < saveIntervalMs) {
        return;
    }
    const auto indexes = stateOutputIndexes();
    std::vector<StoredPointValue> points;
    const auto latestValues = router_.getLatestByIndexes(indexes, nowMs);
    points.reserve(latestValues.size());
    for (const auto& latest : latestValues) {
        if (latest.quality == 1 && !latest.stale) {
            points.push_back(latest);
        }
    }
    std::ostringstream output;
    output << "{\n";
    output << "  \"schemaVersion\": \"1.0.0\",\n";
    output << "  \"graphCode\": \"" << jsonEscape(config_.graphCode) << "\",\n";
    output << "  \"savedAt\": " << nowMs << ",\n";
    output << "  \"points\": [\n";
    output << std::setprecision(15);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto& point = points[i];
        output << "    {\"index\": " << point.index
               << ", \"value\": " << point.value
               << ", \"ts\": " << point.ts << "}";
        if (i + 1 < points.size()) {
            output << ",";
        }
        output << "\n";
    }
    output << "  ],\n";
    output << "  \"runtime\": {\n";

    output << "    \"averageWindows\": [";
    bool first = true;
    for (const auto& entry : averageWindows_) {
        if (!first) {
            output << ",";
        }
        first = false;
        output << "\n      {\"index\": " << entry.first << ", \"values\": [";
        for (std::size_t i = 0; i < entry.second.size(); ++i) {
            if (i > 0) {
                output << ", ";
            }
            output << entry.second[i];
        }
        output << "]}";
    }
    output << (first ? "]" : "\n    ]") << ",\n";

    output << "    \"feedbackVerify\": [";
    first = true;
    for (const auto& entry : feedbackVerifyStates_) {
        if (!entry.second.initialized) {
            continue;
        }
        output << (first ? "\n" : ",\n")
               << "      {\"nodeId\": \"" << jsonEscape(entry.first)
               << "\", \"initialized\": true, \"target\": " << entry.second.target
               << ", \"targetAgeMs\": " << elapsedAt(nowMs, entry.second.targetChangedAt) << "}";
        first = false;
    }
    output << (first ? "]" : "\n    ]") << ",\n";

    output << "    \"rateLimit\": [";
    first = true;
    for (const auto& entry : rateLimitStates_) {
        if (!entry.second.initialized) {
            continue;
        }
        output << (first ? "\n" : ",\n")
               << "      {\"nodeId\": \"" << jsonEscape(entry.first)
               << "\", \"initialized\": true, \"output\": " << entry.second.output
               << ", \"lastRunAgeMs\": " << elapsedAt(nowMs, entry.second.lastRunAt) << "}";
        first = false;
    }
    output << (first ? "]" : "\n    ]") << ",\n";

    const auto writeBooleanStates = [&](const char* key,
                                        const std::unordered_map<std::string, BooleanFilterState>& states,
                                        bool trailingComma) {
        output << "    \"" << key << "\": [";
        bool firstState = true;
        for (const auto& entry : states) {
            if (!entry.second.initialized) {
                continue;
            }
            output << (firstState ? "\n" : ",\n")
                   << "      {\"nodeId\": \"" << jsonEscape(entry.first)
                   << "\", \"initialized\": true, \"output\": " << (entry.second.output ? "true" : "false")
                   << ", \"candidate\": " << (entry.second.candidate ? "true" : "false")
                   << ", \"candidateAgeMs\": " << elapsedAt(nowMs, entry.second.candidateSince) << "}";
            firstState = false;
        }
        output << (firstState ? "]" : "\n    ]") << (trailingComma ? ",\n" : "\n");
    };
    writeBooleanStates("hysteresis", hysteresisStates_, true);
    writeBooleanStates("debounce", debounceStates_, true);

    output << "    \"sequence\": [";
    first = true;
    for (const auto& entry : sequenceStates_) {
        if (!entry.second.initialized) {
            continue;
        }
        output << (first ? "\n" : ",\n")
               << "      {\"nodeId\": \"" << jsonEscape(entry.first)
               << "\", \"initialized\": true, \"current\": " << entry.second.current
               << ", \"enteredAgeMs\": " << elapsedAt(nowMs, entry.second.enteredAt) << "}";
        first = false;
    }
    output << (first ? "]\n" : "\n    ]\n");
    output << "  }\n";
    output << "}\n";

    makeDirectory(directoryOf(stateFile_));
    const auto temporaryFile = stateFile_ + ".tmp";
    {
        std::ofstream stateOutput(temporaryFile.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
        if (!stateOutput.is_open()) {
            throw std::runtime_error("failed to open graph EMS temporary state file: " + temporaryFile);
        }
        stateOutput << output.str();
        stateOutput.flush();
        if (!stateOutput.good()) {
            stateOutput.close();
            std::remove(temporaryFile.c_str());
            throw std::runtime_error("failed to write graph EMS state file: " + temporaryFile);
        }
    }
#if defined(_WIN32)
    std::remove(stateFile_.c_str());
#endif
    if (std::rename(temporaryFile.c_str(), stateFile_.c_str()) != 0) {
        std::remove(temporaryFile.c_str());
        throw std::runtime_error("failed to replace graph EMS state file: " + stateFile_);
    }
    stateSaved_ = true;
    lastStateSaveAt_ = nowMs;
}

std::vector<std::uint32_t> GraphEmsEngine::stateOutputIndexes() const {
    std::set<std::uint32_t> indexes;
    const auto add = [&](std::uint32_t index) {
        if (index != 0) {
            indexes.insert(index);
        }
    };
    for (const auto& node : config_.nodes) {
        if (!shouldRunNode(node)) {
            continue;
        }
        if (node.type == "meterAverage") {
            const auto countIt = node.params.find("mappings.count");
            const auto count = countIt == node.params.end() ? 0U : static_cast<std::size_t>(std::stoul(countIt->second));
            for (std::size_t i = 0; i < count; ++i) {
                add(paramIndex(node, "mappings." + std::to_string(i) + ".output"));
            }
        } else if (node.type == "derivedLoad") {
            add(paramIndex(node, "fhPaOutput", 309));
            add(paramIndex(node, "fhPbOutput", 310));
            add(paramIndex(node, "fhPcOutput", 311));
            add(paramIndex(node, "fhP3Output", 312));
            add(paramIndex(node, "fhQaOutput", 313));
            add(paramIndex(node, "fhQbOutput", 314));
            add(paramIndex(node, "fhQcOutput", 315));
            add(paramIndex(node, "fhQ3Output", 316));
            add(paramIndex(node, "fhSaOutput", 317));
            add(paramIndex(node, "fhSbOutput", 318));
            add(paramIndex(node, "fhScOutput", 319));
            add(paramIndex(node, "fhS3Output", 320));
            add(paramIndex(node, "fhCosAOutput", 321));
            add(paramIndex(node, "fhCosBOutput", 322));
            add(paramIndex(node, "fhCosCOutput", 323));
            add(paramIndex(node, "fhCos3Output", 324));
            add(paramIndex(node, "fhBalanceOutput", 325));
        } else if (node.type == "bmsDerived") {
            add(paramIndex(node, "chargeKwAllowOutput", 1552));
            add(paramIndex(node, "dischargeKwAllowOutput", 1553));
            add(paramIndex(node, "chargeKwhTodayOutput", 1615));
            add(paramIndex(node, "dischargeKwhTodayOutput", 1616));
        } else if (node.type == "cosCompensation") {
            add(paramIndex(node, "targetQaOutput", 505));
            add(paramIndex(node, "targetQbOutput", 506));
            add(paramIndex(node, "targetQcOutput", 507));
            add(paramIndex(node, "targetQ3Output", 508));
            add(paramIndex(node, "qaOutput", 601));
            add(paramIndex(node, "qbOutput", 602));
            add(paramIndex(node, "qcOutput", 603));
            add(paramIndex(node, "q3Output", 604));
            add(paramIndex(node, "runOutput", 8));
        } else if (node.type == "voltageCompensation") {
            add(paramIndex(node, "lvPaOutput", 605));
            add(paramIndex(node, "lvPbOutput", 606));
            add(paramIndex(node, "lvPcOutput", 607));
            add(paramIndex(node, "lvP3Output", 608));
            add(paramIndex(node, "hvPaOutput", 609));
            add(paramIndex(node, "hvPbOutput", 610));
            add(paramIndex(node, "hvPcOutput", 611));
            add(paramIndex(node, "hvP3Output", 612));
            add(paramIndex(node, "lvRunOutput", 10));
            add(paramIndex(node, "hvRunOutput", 12));
        } else if (node.type == "chargeDischarge") {
            add(paramIndex(node, "cdRunOutput", 14));
            add(paramIndex(node, "fdRunOutput", 16));
            add(paramIndex(node, "cdP3Output", 613));
            add(paramIndex(node, "fdP3Output", 614));
            add(paramIndex(node, "phaseStateOutput", 0));
        } else if (node.type == "chargeDischargeCycleTest") {
            add(paramIndex(node, "paOutput", 615));
            add(paramIndex(node, "pbOutput", 616));
            add(paramIndex(node, "pcOutput", 617));
            add(paramIndex(node, "p3Output", 618));
            add(paramIndex(node, "runOutput", 18));
            add(paramIndex(node, "phaseStateOutput", 0));
        } else if (node.type == "timedChargeDischarge") {
            add(paramIndex(node, "powerNowOutput", 461));
            add(paramIndex(node, "socNowOutput", 462));
            add(paramIndex(node, "paOutput", 615));
            add(paramIndex(node, "pbOutput", 616));
            add(paramIndex(node, "pcOutput", 617));
            add(paramIndex(node, "p3Output", 618));
            add(paramIndex(node, "runOutput", 18));
        } else if (node.type == "photovoltaicCharge") {
            add(paramIndex(node, "paOutput", 619));
            add(paramIndex(node, "pbOutput", 620));
            add(paramIndex(node, "pcOutput", 621));
            add(paramIndex(node, "p3Output", 622));
            add(paramIndex(node, "runOutput", 22));
        } else if (node.type == "phaseBalance") {
            add(paramIndex(node, "balanceOutput", 564));
            add(paramIndex(node, "tqCnPaOutput", 565));
            add(paramIndex(node, "tqCnPbOutput", 566));
            add(paramIndex(node, "tqCnPcOutput", 567));
            add(paramIndex(node, "paOutput", 623));
            add(paramIndex(node, "pbOutput", 624));
            add(paramIndex(node, "pcOutput", 625));
            add(paramIndex(node, "runOutput", 20));
        } else if (node.type == "skOverride") {
            add(paramIndex(node, "runOutput", 26));
        } else if (node.type == "reserveCapacity") {
            add(paramIndex(node, "runOutput", 24));
        } else if (node.type == "scheduleSelect") {
            add(paramIndex(node, "powerOutputIndex", 0));
            add(paramIndex(node, "socOutputIndex", 0));
            add(paramIndex(node, "modeOutputIndex", 0));
        } else if (node.type == "phaseArbiter" || node.type == "powerConstraint") {
            for (const auto index : paramIndexes(node, "activeOutputIndexes")) {
                add(index);
            }
            for (const auto index : paramIndexes(node, "reactiveOutputIndexes")) {
                add(index);
            }
            if (node.type == "powerConstraint") {
                add(paramIndex(node, "reserveRunOutputIndex", 0));
                for (const auto index : paramIndexes(node, "lowStateClearIndexes")) {
                    add(index);
                }
                for (const auto index : paramIndexes(node, "highStateClearIndexes")) {
                    add(index);
                }
            }
        } else if (node.type == "formula" || node.type == "timeSource" || node.type == "windowAggregate" ||
                   node.type == "switch" || node.type == "controlGate" ||
                   node.type == "feedbackVerify" || node.type == "rateLimit" ||
                   node.type == "hysteresis" || node.type == "debounce" || node.type == "sequence") {
            add(paramIndex(node, node.type == "sequence" ? "stateOutputIndex" : "outputIndex", 0));
        } else if (node.type == "pcsPowerSolve") {
            add(paramIndex(node, "zrRunOutput", 24));
            add(paramIndex(node, "cosRunOutput", 8));
            add(paramIndex(node, "lvRunOutput", 10));
            add(paramIndex(node, "hvRunOutput", 12));
            add(paramIndex(node, "gfRunOutput", 22));
            add(paramIndex(node, "paOutput", 627));
            add(paramIndex(node, "pbOutput", 628));
            add(paramIndex(node, "pcOutput", 629));
            add(paramIndex(node, "qaOutput", 630));
            add(paramIndex(node, "qbOutput", 631));
            add(paramIndex(node, "qcOutput", 632));
        }
    }
    return std::vector<std::uint32_t>(indexes.begin(), indexes.end());
}

bool GraphEmsEngine::runMeterAverage(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    if (!shouldRunNode(node)) {
        return false;
    }
    const auto windowIt = node.params.find("windowSizeIndex");
    const auto windowIndex = windowIt == node.params.end()
        ? 0U
        : static_cast<std::uint32_t>(std::stoul(windowIt->second));
    const auto windowRaw = windowIndex == 0 ? 10.0 : latestValue(windowIndex, nowMs).value_or(10.0);
    const auto windowSize = static_cast<std::size_t>(std::max(1.0, windowRaw > 0.0 ? windowRaw : 10.0));

    const auto countIt = node.params.find("mappings.count");
    const auto count = countIt == node.params.end() ? 0U : static_cast<std::size_t>(std::stoul(countIt->second));
    bool updated = false;
    for (std::size_t i = 0; i < count; ++i) {
        const auto inputIt = node.params.find("mappings." + std::to_string(i) + ".input");
        const auto outputIt = node.params.find("mappings." + std::to_string(i) + ".output");
        if (inputIt == node.params.end() || outputIt == node.params.end()) {
            continue;
        }

        const auto inputIndex = static_cast<std::uint32_t>(std::stoul(inputIt->second));
        const auto outputIndex = static_cast<std::uint32_t>(std::stoul(outputIt->second));
        const auto input = latestValue(inputIndex, nowMs);
        if (!input) {
            continue;
        }

        auto& values = averageWindows_[outputIndex];
        while (values.size() >= windowSize) {
            values.erase(values.begin());
        }
        values.push_back(*input);
        const auto average = std::accumulate(values.begin(), values.end(), 0.0) /
            static_cast<double>(values.size());
        const auto routed = set(outputIndex, average, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    if (paramBool(node, "deriveTqMetrics", false)) {
        const auto pa = latestValue(paramIndex(node, "tqPaIndex", 209), nowMs);
        const auto pb = latestValue(paramIndex(node, "tqPbIndex", 210), nowMs);
        const auto pc = latestValue(paramIndex(node, "tqPcIndex", 211), nowMs);
        const auto p3 = latestValue(paramIndex(node, "tqP3Index", 212), nowMs);
        const auto qa = latestValue(paramIndex(node, "tqQaIndex", 213), nowMs);
        const auto qb = latestValue(paramIndex(node, "tqQbIndex", 214), nowMs);
        const auto qc = latestValue(paramIndex(node, "tqQcIndex", 215), nowMs);
        const auto q3 = latestValue(paramIndex(node, "tqQ3Index", 216), nowMs);

        const auto writeMetric = [&](std::uint32_t index, double value) {
            const auto routed = set(index, value, nowMs);
            if (routed.accepted) {
                ++result.latestWrites;
                updated = true;
            }
        };
        if (pa && qa) {
            writeMetric(paramIndex(node, "tqSaOutput", 217), apparentPower(*pa, *qa));
        }
        if (pb && qb) {
            writeMetric(paramIndex(node, "tqSbOutput", 218), apparentPower(*pb, *qb));
        }
        if (pc && qc) {
            writeMetric(paramIndex(node, "tqScOutput", 219), apparentPower(*pc, *qc));
        }
        if (p3 && q3) {
            writeMetric(paramIndex(node, "tqS3Output", 220), apparentPower(*p3, *q3));
        }

        const auto sa = latestValue(paramIndex(node, "tqSaOutput", 217), nowMs);
        const auto sb = latestValue(paramIndex(node, "tqSbOutput", 218), nowMs);
        const auto sc = latestValue(paramIndex(node, "tqScOutput", 219), nowMs);
        const auto s3 = latestValue(paramIndex(node, "tqS3Output", 220), nowMs);
        if (pa && sa) {
            writeMetric(paramIndex(node, "tqCosAOutput", 221), powerFactor(*pa, *sa));
        }
        if (pb && sb) {
            writeMetric(paramIndex(node, "tqCosBOutput", 222), powerFactor(*pb, *sb));
        }
        if (pc && sc) {
            writeMetric(paramIndex(node, "tqCosCOutput", 223), powerFactor(*pc, *sc));
        }
        if (p3 && s3) {
            writeMetric(paramIndex(node, "tqCos3Output", 224), powerFactor(*p3, *s3));
        }

        if (pa && pb && pc && p3 && isFiniteNonZero(*p3)) {
            const auto maxPhase = std::max(*pa, std::max(*pb, *pc));
            const auto minPhase = std::min(*pa, std::min(*pb, *pc));
            writeMetric(paramIndex(node, "tqBalanceOutput", 225), std::abs((maxPhase - minPhase) / *p3 * 300.0));
        }
    }
    return updated;
}

bool GraphEmsEngine::runDerivedLoad(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto sourceIt = node.params.find("source");
    const auto source = sourceIt == node.params.end() ? std::string("tqCn") : sourceIt->second;
    const bool useBw = source == "tqBw" || source == "bw";
    const bool useDirectFh = source == "fh" || source == "direct" || source == "directFh";

    double fhPa = 0.0;
    double fhPb = 0.0;
    double fhPc = 0.0;
    double fhP3 = 0.0;
    double fhQa = 0.0;
    double fhQb = 0.0;
    double fhQc = 0.0;
    double fhQ3 = 0.0;

    if (useDirectFh) {
        const auto directPa = latestValue(paramIndex(node, "fhPaIndex", 309), nowMs);
        const auto directPb = latestValue(paramIndex(node, "fhPbIndex", 310), nowMs);
        const auto directPc = latestValue(paramIndex(node, "fhPcIndex", 311), nowMs);
        const auto directP3 = latestValue(paramIndex(node, "fhP3Index", 312), nowMs);
        const auto directQa = latestValue(paramIndex(node, "fhQaIndex", 313), nowMs);
        const auto directQb = latestValue(paramIndex(node, "fhQbIndex", 314), nowMs);
        const auto directQc = latestValue(paramIndex(node, "fhQcIndex", 315), nowMs);
        const auto directQ3 = latestValue(paramIndex(node, "fhQ3Index", 316), nowMs);
        if (!directPa || !directPb || !directPc || !directP3 ||
            !directQa || !directQb || !directQc || !directQ3) {
            return false;
        }
        fhPa = *directPa;
        fhPb = *directPb;
        fhPc = *directPc;
        fhP3 = *directP3;
        fhQa = *directQa;
        fhQb = *directQb;
        fhQc = *directQc;
        fhQ3 = *directQ3;
    } else {
        const auto tqPa = latestValue(paramIndex(node, "tqPaIndex", 209), nowMs);
        const auto tqPb = latestValue(paramIndex(node, "tqPbIndex", 210), nowMs);
        const auto tqPc = latestValue(paramIndex(node, "tqPcIndex", 211), nowMs);
        const auto tqP3 = latestValue(paramIndex(node, "tqP3Index", 212), nowMs);
        const auto tqQa = latestValue(paramIndex(node, "tqQaIndex", 213), nowMs);
        const auto tqQb = latestValue(paramIndex(node, "tqQbIndex", 214), nowMs);
        const auto tqQc = latestValue(paramIndex(node, "tqQcIndex", 215), nowMs);
        const auto tqQ3 = latestValue(paramIndex(node, "tqQ3Index", 216), nowMs);

        const auto otherPa = latestValue(paramIndex(node, useBw ? "bwPaIndex" : "cnPaIndex", useBw ? 401 : 259), nowMs);
        const auto otherPb = latestValue(paramIndex(node, useBw ? "bwPbIndex" : "cnPbIndex", useBw ? 402 : 260), nowMs);
        const auto otherPc = latestValue(paramIndex(node, useBw ? "bwPcIndex" : "cnPcIndex", useBw ? 403 : 261), nowMs);
        const auto otherP3 = latestValue(paramIndex(node, useBw ? "bwP3Index" : "cnP3Index", useBw ? 404 : 262), nowMs);
        const auto otherQa = latestValue(paramIndex(node, useBw ? "bwQaIndex" : "cnQaIndex", useBw ? 405 : 263), nowMs);
        const auto otherQb = latestValue(paramIndex(node, useBw ? "bwQbIndex" : "cnQbIndex", useBw ? 406 : 264), nowMs);
        const auto otherQc = latestValue(paramIndex(node, useBw ? "bwQcIndex" : "cnQcIndex", useBw ? 407 : 265), nowMs);
        const auto otherQ3 = latestValue(paramIndex(node, useBw ? "bwQ3Index" : "cnQ3Index", useBw ? 408 : 266), nowMs);

        if (!tqPa || !tqPb || !tqPc || !tqP3 || !tqQa || !tqQb || !tqQc || !tqQ3 ||
            !otherPa || !otherPb || !otherPc || !otherP3 || !otherQa || !otherQb || !otherQc || !otherQ3) {
            return false;
        }

        fhPa = *tqPa - *otherPa;
        fhPb = *tqPb - *otherPb;
        fhPc = *tqPc - *otherPc;
        fhP3 = *tqP3 - *otherP3;
        fhQa = *tqQa - *otherQa;
        fhQb = *tqQb - *otherQb;
        fhQc = *tqQc - *otherQc;
        fhQ3 = *tqQ3 - *otherQ3;
    }

    const double fhSa = apparentPower(fhPa, fhQa);
    const double fhSb = apparentPower(fhPb, fhQb);
    const double fhSc = apparentPower(fhPc, fhQc);
    const double fhS3 = apparentPower(fhP3, fhQ3);
    const double fhCosA = powerFactor(fhPa, fhSa);
    const double fhCosB = powerFactor(fhPb, fhSb);
    const double fhCosC = powerFactor(fhPc, fhSc);
    const double fhCos3 = powerFactor(fhP3, fhS3);

    bool updated = false;
    const std::pair<std::uint32_t, double> outputs[] = {
        {paramIndex(node, "fhPaOutput", 309), fhPa},
        {paramIndex(node, "fhPbOutput", 310), fhPb},
        {paramIndex(node, "fhPcOutput", 311), fhPc},
        {paramIndex(node, "fhP3Output", 312), fhP3},
        {paramIndex(node, "fhQaOutput", 313), fhQa},
        {paramIndex(node, "fhQbOutput", 314), fhQb},
        {paramIndex(node, "fhQcOutput", 315), fhQc},
        {paramIndex(node, "fhQ3Output", 316), fhQ3},
        {paramIndex(node, "fhSaOutput", 317), fhSa},
        {paramIndex(node, "fhSbOutput", 318), fhSb},
        {paramIndex(node, "fhScOutput", 319), fhSc},
        {paramIndex(node, "fhS3Output", 320), fhS3},
        {paramIndex(node, "fhCosAOutput", 321), fhCosA},
        {paramIndex(node, "fhCosBOutput", 322), fhCosB},
        {paramIndex(node, "fhCosCOutput", 323), fhCosC},
        {paramIndex(node, "fhCos3Output", 324), fhCos3}
    };
    for (const auto& output : outputs) {
        const auto routed = set(output.first, output.second, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }

    if (isFiniteNonZero(fhP3)) {
        const auto maxPhase = std::max(fhPa, std::max(fhPb, fhPc));
        const auto minPhase = std::min(fhPa, std::min(fhPb, fhPc));
        const auto routed = set(
            paramIndex(node, "fhBalanceOutput", 325),
            std::abs((maxPhase - minPhase) / fhP3 * 300.0),
            nowMs
        );
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    return updated;
}

bool GraphEmsEngine::runBmsDerived(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    bool updated = false;
    const auto bmsModelParam = paramDouble(node, "bmsModel");
    const auto bmsModel = bmsModelParam
        ? static_cast<int>(*bmsModelParam)
        : profileInt("BMS_MODEL", 2);
    const auto voltage = latestValue(paramIndex(node, "voltageIndex", 1566), nowMs);
    const auto chargeCurrent = latestValue(paramIndex(node, "chargeCurrentAllowIndex", 1556), nowMs);
    const auto dischargeCurrent = latestValue(paramIndex(node, "dischargeCurrentAllowIndex", 1557), nowMs);
    if (voltage && chargeCurrent) {
        const auto routed = set(paramIndex(node, "chargeKwAllowOutput", 1552), *voltage * *chargeCurrent * 0.001, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    if (voltage && dischargeCurrent) {
        const auto routed = set(paramIndex(node, "dischargeKwAllowOutput", 1553), *voltage * *dischargeCurrent * 0.001, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }

    if (bmsModel == 1 || bmsModel == 3) {
        const auto chargeSum = latestValue(paramIndex(node, "chargeKwhSumIndex", 1586), nowMs);
        const auto chargeZero = latestValue(paramIndex(node, "chargeKwhZeroIndex", 398), nowMs);
        if (chargeSum && chargeZero) {
            const auto routed = set(paramIndex(node, "chargeKwhTodayOutput", 1615), *chargeSum - *chargeZero, nowMs);
            if (routed.accepted) {
                ++result.latestWrites;
                updated = true;
            }
        }
        const auto dischargeSum = latestValue(paramIndex(node, "dischargeKwhSumIndex", 1587), nowMs);
        const auto dischargeZero = latestValue(paramIndex(node, "dischargeKwhZeroIndex", 399), nowMs);
        if (dischargeSum && dischargeZero) {
            const auto routed = set(paramIndex(node, "dischargeKwhTodayOutput", 1616), *dischargeSum - *dischargeZero, nowMs);
            if (routed.accepted) {
                ++result.latestWrites;
                updated = true;
            }
        }
    }
    return updated;
}

bool GraphEmsEngine::runCosCompensation(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto cosTarget = latestValue(paramIndex(node, "targetCosIndex", 514), nowMs);
    const auto pa = latestValue(paramIndex(node, "tqPaIndex", 209), nowMs);
    const auto pb = latestValue(paramIndex(node, "tqPbIndex", 210), nowMs);
    const auto pc = latestValue(paramIndex(node, "tqPcIndex", 211), nowMs);
    const auto qa = latestValue(paramIndex(node, "tqQaIndex", 213), nowMs);
    const auto qb = latestValue(paramIndex(node, "tqQbIndex", 214), nowMs);
    const auto qc = latestValue(paramIndex(node, "tqQcIndex", 215), nowMs);
    if (!cosTarget || !pa || !pb || !pc || !qa || !qb || !qc) {
        return false;
    }
    if (*cosTarget <= -1.0 || *cosTarget >= 1.0) {
        return false;
    }

    const double targetTan = std::tan(std::acos(*cosTarget));
    const double targetQa = std::abs(targetTan * *pa);
    const double targetQb = std::abs(targetTan * *pb);
    const double targetQc = std::abs(targetTan * *pc);
    const double targetQ3 = targetQa + targetQb + targetQc;

    const auto outputQ = [](double current, double target) {
        if ((current > 0.0 && current > target) || (current < 0.0 && current < -target)) {
            return current > 0.0 ? (current - target) : (current + target);
        }
        return 0.0;
    };
    const double outQa = outputQ(*qa, targetQa);
    const double outQb = outputQ(*qb, targetQb);
    const double outQc = outputQ(*qc, targetQc);
    const double outQ3 = std::abs(outQa) + std::abs(outQb) + std::abs(outQc);

    bool updated = false;
    const std::pair<std::uint32_t, double> outputs[] = {
        {paramIndex(node, "targetQaOutput", 505), targetQa},
        {paramIndex(node, "targetQbOutput", 506), targetQb},
        {paramIndex(node, "targetQcOutput", 507), targetQc},
        {paramIndex(node, "targetQ3Output", 508), targetQ3},
        {paramIndex(node, "qaOutput", 601), outQa},
        {paramIndex(node, "qbOutput", 602), outQb},
        {paramIndex(node, "qcOutput", 603), outQc},
        {paramIndex(node, "q3Output", 604), outQ3},
        {paramIndex(node, "runOutput", 8), outQ3 != 0.0 ? 1.0 : 0.0}
    };
    for (const auto& output : outputs) {
        const auto routed = set(output.first, output.second, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    return updated;
}

bool GraphEmsEngine::runVoltageCompensation(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto cnUa = latestValue(paramIndex(node, "cnUaIndex", 251), nowMs);
    const auto cnUb = latestValue(paramIndex(node, "cnUbIndex", 252), nowMs);
    const auto cnUc = latestValue(paramIndex(node, "cnUcIndex", 253), nowMs);
    const auto lvLow = latestValue(paramIndex(node, "lvLowIndex", 544), nowMs);
    const auto lvUp = latestValue(paramIndex(node, "lvUpIndex", 545), nowMs);
    const auto hvLow = latestValue(paramIndex(node, "hvLowIndex", 546), nowMs);
    const auto hvUp = latestValue(paramIndex(node, "hvUpIndex", 547), nowMs);
    const auto gradP = latestValue(paramIndex(node, "gradPIndex", 533), nowMs);
    const auto pMax = latestValue(paramIndex(node, "pMaxIndex", 535), nowMs);
    if (!cnUa || !cnUb || !cnUc || !lvLow || !lvUp || !hvLow || !hvUp || !gradP || !pMax) {
        return false;
    }

    const auto clamp = [](double value, double lower, double upper) {
        return std::max(lower, std::min(value, upper));
    };
    const auto lvOutput = [&](double voltage) {
        double value = 0.0;
        if (voltage < *lvLow) {
            value -= *gradP;
        } else if (voltage > *lvUp) {
            value += *gradP;
        }
        return clamp(value, -*pMax, 0.0);
    };
    const auto hvOutput = [&](double voltage) {
        double value = 0.0;
        if (voltage > *hvUp) {
            value += *gradP;
        } else if (voltage < *hvLow) {
            value -= *gradP;
        }
        return clamp(value, 0.0, *pMax);
    };

    const double outPaLv = lvOutput(*cnUa);
    const double outPbLv = lvOutput(*cnUb);
    const double outPcLv = lvOutput(*cnUc);
    const double outP3Lv = std::abs(outPaLv) + std::abs(outPbLv) + std::abs(outPcLv);
    const double outPaHv = hvOutput(*cnUa);
    const double outPbHv = hvOutput(*cnUb);
    const double outPcHv = hvOutput(*cnUc);
    const double outP3Hv = std::abs(outPaHv) + std::abs(outPbHv) + std::abs(outPcHv);

    bool updated = false;
    const std::pair<std::uint32_t, double> outputs[] = {
        {paramIndex(node, "lvPaOutput", 605), outPaLv},
        {paramIndex(node, "lvPbOutput", 606), outPbLv},
        {paramIndex(node, "lvPcOutput", 607), outPcLv},
        {paramIndex(node, "lvP3Output", 608), outP3Lv},
        {paramIndex(node, "hvPaOutput", 609), outPaHv},
        {paramIndex(node, "hvPbOutput", 610), outPbHv},
        {paramIndex(node, "hvPcOutput", 611), outPcHv},
        {paramIndex(node, "hvP3Output", 612), outP3Hv},
        {paramIndex(node, "lvRunOutput", 10), outP3Lv != 0.0 ? 1.0 : 0.0},
        {paramIndex(node, "hvRunOutput", 12), outP3Hv != 0.0 ? 1.0 : 0.0}
    };
    for (const auto& output : outputs) {
        const auto routed = set(output.first, output.second, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    return updated;
}

bool GraphEmsEngine::runChargeDischarge(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto modeIt = node.params.find("mode");
    if (modeIt != node.params.end() && modeIt->second == "dischargeThenCharge") {
        return runSequentialChargeDischarge(node, nowMs, result);
    }

    const auto bmsSoc = latestValue(paramIndex(node, "bmsSocIndex", 1570), nowMs);
    const auto cdTargetP = latestValue(paramIndex(node, "cdTargetPowerIndex", 451), nowMs);
    const auto cdTargetSoc = latestValue(paramIndex(node, "cdTargetSocIndex", 452), nowMs);
    const auto tqPxzPosValue = latestValue(paramIndex(node, "positiveLimitIndex", 453), nowMs);
    const auto tqPxzPosEn = latestValue(paramIndex(node, "positiveLimitEnableIndex", 454), nowMs);
    const auto fdTargetP = latestValue(paramIndex(node, "fdTargetPowerIndex", 455), nowMs);
    const auto fdTargetSoc = latestValue(paramIndex(node, "fdTargetSocIndex", 456), nowMs);
    const auto tqPxzNegValue = latestValue(paramIndex(node, "negativeLimitIndex", 457), nowMs);
    const auto tqPxzNegEn = latestValue(paramIndex(node, "negativeLimitEnableIndex", 458), nowMs);
    const auto fhP3 = latestValue(paramIndex(node, "fhP3Index", 312), nowMs);
    if (!bmsSoc || !cdTargetP || !cdTargetSoc || !fdTargetP || !fdTargetSoc || !fhP3) {
        return false;
    }

    double outP3Cd = 0.0;
    double cdRun = 0.0;
    if (*bmsSoc < *cdTargetSoc && *cdTargetP != 0.0) {
        cdRun = 1.0;
        if (tqPxzPosEn && *tqPxzPosEn == 1.0 && tqPxzPosValue) {
            const double pYx = *tqPxzPosValue - *fhP3;
            outP3Cd = pYx > 0.0 ? std::min(pYx, *cdTargetP) : 0.0;
        } else {
            outP3Cd = *cdTargetP;
        }
    }

    double outP3Fd = 0.0;
    double fdRun = 0.0;
    if (*bmsSoc > *fdTargetSoc && *fdTargetP != 0.0) {
        fdRun = 1.0;
        if (tqPxzNegEn && *tqPxzNegEn == 1.0 && tqPxzNegValue) {
            outP3Fd = -1.0 * std::min(std::max(*tqPxzNegValue - *fhP3, 0.0), *fdTargetP);
        } else {
            outP3Fd = -1.0 * (*fdTargetP);
        }
    }

    bool updated = false;
    const std::pair<std::uint32_t, double> outputs[] = {
        {paramIndex(node, "cdRunOutput", 14), cdRun},
        {paramIndex(node, "fdRunOutput", 16), fdRun},
        {paramIndex(node, "cdP3Output", 613), outP3Cd},
        {paramIndex(node, "fdP3Output", 614), outP3Fd}
    };
    for (const auto& output : outputs) {
        const auto routed = set(output.first, output.second, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    return updated;
}

bool GraphEmsEngine::runSequentialChargeDischarge(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto bmsSoc = latestValue(paramIndex(node, "bmsSocIndex", 1570), nowMs);
    const auto cdTargetP = latestValue(paramIndex(node, "cdTargetPowerIndex", 451), nowMs);
    const auto cdTargetSoc = latestValue(paramIndex(node, "cdTargetSocIndex", 452), nowMs);
    const auto fdTargetP = latestValue(paramIndex(node, "fdTargetPowerIndex", 455), nowMs);
    const auto fdTargetSoc = latestValue(paramIndex(node, "fdTargetSocIndex", 456), nowMs);
    if (!bmsSoc || !cdTargetP || !cdTargetSoc || !fdTargetP || !fdTargetSoc) {
        return false;
    }

    const auto phaseStateOutput = paramIndex(node, "phaseStateOutput", 0);
    int phase = 1;
    if (phaseStateOutput != 0) {
        if (const auto savedPhase = latestValue(phaseStateOutput, nowMs)) {
            phase = static_cast<int>(*savedPhase);
        }
    }
    if (phase < 1 || phase > 3) {
        phase = 1;
    }

    double outP3Cd = 0.0;
    double outP3Fd = 0.0;
    double cdRun = 0.0;
    double fdRun = 0.0;

    if (phase == 1) {
        if (*bmsSoc <= *fdTargetSoc) {
            phase = 2;
        } else if (*fdTargetP != 0.0) {
            fdRun = 1.0;
            outP3Fd = -1.0 * std::abs(*fdTargetP);
        }
    }

    if (phase == 2) {
        if (*bmsSoc >= *cdTargetSoc) {
            phase = 3;
        } else if (*cdTargetP != 0.0) {
            cdRun = 1.0;
            outP3Cd = std::abs(*cdTargetP);
        }
    }

    bool updated = false;
    const std::pair<std::uint32_t, double> outputs[] = {
        {paramIndex(node, "cdRunOutput", 14), cdRun},
        {paramIndex(node, "fdRunOutput", 16), fdRun},
        {paramIndex(node, "cdP3Output", 613), outP3Cd},
        {paramIndex(node, "fdP3Output", 614), outP3Fd},
        {phaseStateOutput, static_cast<double>(phase)}
    };
    for (const auto& output : outputs) {
        if (output.first == 0) {
            continue;
        }
        const auto routed = set(output.first, output.second, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    return updated;
}

bool GraphEmsEngine::runChargeDischargeCycleTest(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto bmsSoc = latestValue(paramIndex(node, "bmsSocIndex", 1570), nowMs);
    if (!bmsSoc) {
        return false;
    }

    const auto latestParam = [&](std::uint32_t index) {
        return latestValue(index, nowMs);
    };
    const auto dischargeDepth = paramOrLatestValue(
        node,
        "dischargeDepth",
        "dischargeDepthIndex",
        latestParam
    );
    const auto chargeDepth = paramOrLatestValue(
        node,
        "chargeDepth",
        "chargeDepthIndex",
        latestParam
    );
    if (!dischargeDepth || !chargeDepth) {
        return false;
    }

    Optional<double> phasePowerA = paramOrLatestValue(node, "phasePowerA", "phasePowerAIndex", latestParam);
    Optional<double> phasePowerB = paramOrLatestValue(node, "phasePowerB", "phasePowerBIndex", latestParam);
    Optional<double> phasePowerC = paramOrLatestValue(node, "phasePowerC", "phasePowerCIndex", latestParam);
    const auto totalPower = paramOrLatestValue(node, "totalPower", "totalPowerIndex", latestParam);
    if (!phasePowerA || !phasePowerB || !phasePowerC) {
        if (!totalPower) {
            return false;
        }
        const double perPhase = std::abs(*totalPower) / 3.0;
        phasePowerA = perPhase;
        phasePowerB = perPhase;
        phasePowerC = perPhase;
    }

    if (*chargeDepth <= *dischargeDepth) {
        return false;
    }

    const auto phaseStateOutput = paramIndex(node, "phaseStateOutput", 0);
    int phase = 1;
    bool hasSavedPhase = false;
    if (phaseStateOutput != 0) {
        if (const auto savedPhase = latestValue(phaseStateOutput, nowMs)) {
            phase = static_cast<int>(*savedPhase);
            hasSavedPhase = true;
        }
    }
    if (phase != 1 && phase != 2) {
        hasSavedPhase = false;
        phase = 1;
    }
    if (!hasSavedPhase && *bmsSoc <= *dischargeDepth) {
        phase = 2;
    }

    if (phase == 1 && *bmsSoc <= *dischargeDepth) {
        phase = 2;
    } else if (phase == 2 && *bmsSoc >= *chargeDepth) {
        phase = 1;
    }

    const bool charging = phase == 2;
    const double sign = charging ? 1.0 : -1.0;
    const double outPa = sign * std::abs(*phasePowerA);
    const double outPb = sign * std::abs(*phasePowerB);
    const double outPc = sign * std::abs(*phasePowerC);
    const double outP3 = std::abs(outPa) + std::abs(outPb) + std::abs(outPc);

    bool updated = false;
    const std::pair<std::uint32_t, double> outputs[] = {
        {paramIndex(node, "paOutput", 615), outPa},
        {paramIndex(node, "pbOutput", 616), outPb},
        {paramIndex(node, "pcOutput", 617), outPc},
        {paramIndex(node, "p3Output", 618), outP3},
        {paramIndex(node, "runOutput", 18), outP3 != 0.0 ? 1.0 : 0.0},
        {phaseStateOutput, static_cast<double>(phase)}
    };
    for (const auto& output : outputs) {
        if (output.first == 0) {
            continue;
        }
        const auto routed = set(output.first, output.second, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    return updated;
}

bool GraphEmsEngine::runTimedChargeDischarge(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto hour = static_cast<std::uint32_t>(localHourFromEpochMs(nowMs));
    Optional<double> power = NullOpt;
    Optional<double> soc = NullOpt;
    Optional<double> modeValue = NullOpt;
    Optional<double> chargePower = NullOpt;
    Optional<double> dischargePower = NullOpt;
    Optional<double> targetSoc = NullOpt;
    const auto curveCountIt = node.params.find("scheduleCurve.count");
    const auto curveCount = curveCountIt == node.params.end()
        ? 0U
        : static_cast<std::size_t>(std::stoul(curveCountIt->second));
    for (std::size_t i = 0; i < curveCount; ++i) {
        const auto prefix = "scheduleCurve." + std::to_string(i) + ".";
        const auto hourIt = node.params.find(prefix + "hour");
        const auto curveHour = hourIt == node.params.end()
            ? static_cast<std::uint32_t>(i)
            : static_cast<std::uint32_t>(std::stoul(hourIt->second));
        if (curveHour != hour) {
            continue;
        }
        power = paramDouble(node, prefix + "power");
        soc = paramDouble(node, prefix + "soc");
        modeValue = paramDouble(node, prefix + "mode");
        chargePower = paramDouble(node, prefix + "chargePower");
        dischargePower = paramDouble(node, prefix + "dischargePower");
        targetSoc = paramDouble(node, prefix + "targetSoc");
        break;
    }
    const bool hasSignedCurvePlan = power && targetSoc;
    const bool hasSplitCurvePlan = !hasSignedCurvePlan && chargePower && dischargePower && targetSoc;
    if (hasSignedCurvePlan) {
        soc = targetSoc;
        modeValue = 0.0;
    } else if (hasSplitCurvePlan) {
        power = chargePower;
        soc = targetSoc;
        modeValue = 0.0;
    } else if (!power || !soc || !modeValue) {
        power = latestValue(paramIndex(node, "powerScheduleStartIndex", 400) + hour, nowMs);
        soc = latestValue(paramIndex(node, "socScheduleStartIndex", 424) + hour, nowMs);
        modeValue = latestValue(paramIndex(node, "modeScheduleStartIndex", 760) + hour, nowMs);
    }
    const auto bmsSoc = latestValue(paramIndex(node, "bmsSocIndex", 1570), nowMs);
    const auto cnUa = latestValue(paramIndex(node, "cnUaIndex", 251), nowMs);
    const auto cnUb = latestValue(paramIndex(node, "cnUbIndex", 252), nowMs);
    const auto cnUc = latestValue(paramIndex(node, "cnUcIndex", 253), nowMs);
    const auto gradP = latestValue(paramIndex(node, "gradPIndex", 533), nowMs);
    const auto dsEnVmax = latestValue(paramIndex(node, "vMaxIndex", 463), nowMs);
    const auto dsEnVmin = latestValue(paramIndex(node, "vMinIndex", 464), nowMs);
    if (!power || !soc || !modeValue || !bmsSoc || !cnUa || !cnUb || !cnUc || !gradP || !dsEnVmax || !dsEnVmin) {
        return false;
    }

    const auto paOutput = paramIndex(node, "paOutput", 615);
    const auto pbOutput = paramIndex(node, "pbOutput", 616);
    const auto pcOutput = paramIndex(node, "pcOutput", 617);
    const auto p3Output = paramIndex(node, "p3Output", 618);
    const int mode = static_cast<int>(*modeValue);
    double outPaDs = latestValue(paOutput, nowMs).value_or(0.0);
    double outPbDs = latestValue(pbOutput, nowMs).value_or(0.0);
    double outPcDs = latestValue(pcOutput, nowMs).value_or(0.0);
    double outP3Ds = latestValue(p3Output, nowMs).value_or(0.0);
    const auto activePowerLimit = [&](bool charging) {
        if (hasSignedCurvePlan) {
            return std::abs(*power);
        }
        if (hasSplitCurvePlan) {
            return std::max(0.0, charging ? *chargePower : *dischargePower);
        }
        return std::max(0.0, *power);
    };
    const bool shouldCharge = hasSignedCurvePlan
        ? (*power > 0.0 && *bmsSoc < *soc)
        : (*bmsSoc < *soc && mode != 2);
    const bool shouldDischarge = hasSignedCurvePlan
        ? (*power < 0.0 && *bmsSoc > *soc)
        : (*bmsSoc > *soc && mode != 1);

    if (shouldCharge) {
        const auto limit = activePowerLimit(true);
        if (outP3Ds < limit - 1.0) {
            if (*cnUa > *dsEnVmin) {
                outPaDs += *gradP;
            }
            if (*cnUb > *dsEnVmin) {
                outPbDs += *gradP;
            }
            if (*cnUc > *dsEnVmin) {
                outPcDs += *gradP;
            }
        }
        if ((std::abs(outPaDs) + std::abs(outPbDs) + std::abs(outPcDs)) > limit) {
            outPaDs = 0.3333 * limit;
            outPbDs = 0.3333 * limit;
            outPcDs = 0.3333 * limit;
        }
        if (*cnUa < *dsEnVmin - 1.0) {
            outPaDs -= *gradP;
        }
        if (*cnUb < *dsEnVmin - 1.0) {
            outPbDs -= *gradP;
        }
        if (*cnUc < *dsEnVmin - 1.0) {
            outPcDs -= *gradP;
        }
        outPaDs = std::max(0.0, outPaDs);
        outPbDs = std::max(0.0, outPbDs);
        outPcDs = std::max(0.0, outPcDs);
    } else if (shouldDischarge) {
        const auto limit = activePowerLimit(false);
        if (outP3Ds < limit - 1.0) {
            if (*cnUa < *dsEnVmax) {
                outPaDs -= *gradP;
            }
            if (*cnUb < *dsEnVmax) {
                outPbDs -= *gradP;
            }
            if (*cnUc < *dsEnVmax) {
                outPcDs -= *gradP;
            }
        }
        if ((std::abs(outPaDs) + std::abs(outPbDs) + std::abs(outPcDs)) > limit) {
            outPaDs = -0.3333 * limit;
            outPbDs = -0.3333 * limit;
            outPcDs = -0.3333 * limit;
        }
        if (*cnUa > *dsEnVmax + 1.0) {
            outPaDs += *gradP;
        }
        if (*cnUb > *dsEnVmax + 1.0) {
            outPbDs += *gradP;
        }
        if (*cnUc > *dsEnVmax + 1.0) {
            outPcDs += *gradP;
        }
        outPaDs = std::min(0.0, outPaDs);
        outPbDs = std::min(0.0, outPbDs);
        outPcDs = std::min(0.0, outPcDs);
    } else {
        auto decayToZero = [&](double value) {
            if (value > 0.0) {
                value -= 2.0 * *gradP;
                return std::max(0.0, value);
            }
            if (value < 0.0) {
                value += 2.0 * *gradP;
                return std::min(0.0, value);
            }
            return value;
        };
        outPaDs = decayToZero(outPaDs);
        outPbDs = decayToZero(outPbDs);
        outPcDs = decayToZero(outPcDs);
    }

    if (mode == 1) {
        outPaDs = std::max(0.0, outPaDs);
        outPbDs = std::max(0.0, outPbDs);
        outPcDs = std::max(0.0, outPcDs);
    } else if (mode == 2) {
        outPaDs = std::min(0.0, outPaDs);
        outPbDs = std::min(0.0, outPbDs);
        outPcDs = std::min(0.0, outPcDs);
    }

    outP3Ds = std::abs(outPaDs) + std::abs(outPbDs) + std::abs(outPcDs);
    double currentScheduledPower = *power;
    if (hasSignedCurvePlan) {
        currentScheduledPower = *power;
    } else if (hasSplitCurvePlan) {
        if (*bmsSoc > *soc) {
            currentScheduledPower = *dischargePower;
        } else if (*bmsSoc < *soc) {
            currentScheduledPower = *chargePower;
        } else {
            currentScheduledPower = 0.0;
        }
    }

    bool updated = false;
    const std::pair<std::uint32_t, double> outputs[] = {
        {paramIndex(node, "powerNowOutput", 461), currentScheduledPower},
        {paramIndex(node, "socNowOutput", 462), *soc},
        {paOutput, outPaDs},
        {pbOutput, outPbDs},
        {pcOutput, outPcDs},
        {p3Output, outP3Ds},
        {paramIndex(node, "runOutput", 18), outP3Ds != 0.0 ? 1.0 : 0.0}
    };
    for (const auto& output : outputs) {
        const auto routed = set(output.first, output.second, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    return updated;
}

bool GraphEmsEngine::runPhotovoltaicCharge(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto fhPa = latestValue(paramIndex(node, "fhPaIndex", 309), nowMs);
    const auto fhPb = latestValue(paramIndex(node, "fhPbIndex", 310), nowMs);
    const auto fhPc = latestValue(paramIndex(node, "fhPcIndex", 311), nowMs);
    const auto tqPxzNegValue = latestValue(paramIndex(node, "negativeLimitIndex", 457), nowMs);
    const auto startHour = latestValue(paramIndex(node, "startHourIndex", 581), nowMs);
    const auto endHour = latestValue(paramIndex(node, "endHourIndex", 583), nowMs);
    if (!fhPa || !fhPb || !fhPc || !tqPxzNegValue || !startHour || !endHour) {
        return false;
    }

    const int currentHour = localHourFromEpochMs(nowMs);
    double outPaGf = 0.0;
    double outPbGf = 0.0;
    double outPcGf = 0.0;
    if (currentHour >= static_cast<int>(*startHour) && currentHour <= static_cast<int>(*endHour)) {
        outPaGf = *fhPa <= *tqPxzNegValue ? (*tqPxzNegValue - *fhPa) : 0.0;
        outPbGf = *fhPb <= *tqPxzNegValue ? (*tqPxzNegValue - *fhPb) : 0.0;
        outPcGf = *fhPc <= *tqPxzNegValue ? (*tqPxzNegValue - *fhPc) : 0.0;
    }
    const double outP3Gf = std::abs(outPaGf) + std::abs(outPbGf) + std::abs(outPcGf);

    bool updated = false;
    const std::pair<std::uint32_t, double> outputs[] = {
        {paramIndex(node, "paOutput", 619), outPaGf},
        {paramIndex(node, "pbOutput", 620), outPbGf},
        {paramIndex(node, "pcOutput", 621), outPcGf},
        {paramIndex(node, "p3Output", 622), outP3Gf},
        {paramIndex(node, "runOutput", 22), outP3Gf != 0.0 ? 1.0 : 0.0}
    };
    for (const auto& output : outputs) {
        const auto routed = set(output.first, output.second, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    return updated;
}

bool GraphEmsEngine::runPhaseBalance(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto tqPa = latestValue(paramIndex(node, "tqPaIndex", 209), nowMs);
    const auto tqPb = latestValue(paramIndex(node, "tqPbIndex", 210), nowMs);
    const auto tqPc = latestValue(paramIndex(node, "tqPcIndex", 211), nowMs);
    const auto tqP3 = latestValue(paramIndex(node, "tqP3Index", 212), nowMs);
    const auto cnPa = latestValue(paramIndex(node, "cnPaIndex", 259), nowMs);
    const auto cnPb = latestValue(paramIndex(node, "cnPbIndex", 260), nowMs);
    const auto cnPc = latestValue(paramIndex(node, "cnPcIndex", 261), nowMs);
    const auto bphPer = latestValue(paramIndex(node, "balancePercentIndex", 562), nowMs);
    if (!tqPa || !tqPb || !tqPc || !tqP3 || !cnPa || !cnPb || !cnPc || !bphPer ||
        !isFiniteNonZero(*tqP3)) {
        return false;
    }

    const double tqCnPa = *tqPa + *cnPa;
    const double tqCnPb = *tqPb + *cnPb;
    const double tqCnPc = *tqPc + *cnPc;
    const double allowBph = *bphPer * *tqP3 / 300.0;
    const double maxVal = std::max(tqCnPa, std::max(tqCnPb, tqCnPc));
    const double minVal = std::min(tqCnPa, std::min(tqCnPb, tqCnPc));
    const double dVal = maxVal - minVal;
    const double tqCnBph = std::abs(dVal / *tqP3 * 300.0);

    double outPaPh = 0.0;
    double outPbPh = 0.0;
    double outPcPh = 0.0;
    if (dVal > allowBph) {
        const double setVal = (dVal - allowBph) / 2.0;
        if (tqCnPa == maxVal) {
            outPaPh = setVal;
        } else if (tqCnPb == maxVal) {
            outPbPh = setVal;
        } else if (tqCnPc == maxVal) {
            outPcPh = setVal;
        }

        if (tqCnPa == minVal) {
            outPaPh = -setVal;
        } else if (tqCnPb == minVal) {
            outPbPh = -setVal;
        } else if (tqCnPc == minVal) {
            outPcPh = -setVal;
        }
    }
    const double outP3Ph = std::abs(outPaPh) + std::abs(outPbPh) + std::abs(outPcPh);

    bool updated = false;
    const std::pair<std::uint32_t, double> outputs[] = {
        {paramIndex(node, "balanceOutput", 564), tqCnBph},
        {paramIndex(node, "tqCnPaOutput", 565), tqCnPa},
        {paramIndex(node, "tqCnPbOutput", 566), tqCnPb},
        {paramIndex(node, "tqCnPcOutput", 567), tqCnPc},
        {paramIndex(node, "paOutput", 623), outPaPh},
        {paramIndex(node, "pbOutput", 624), outPbPh},
        {paramIndex(node, "pcOutput", 625), outPcPh},
        {paramIndex(node, "runOutput", 20), outP3Ph != 0.0 ? 1.0 : 0.0}
    };
    for (const auto& output : outputs) {
        const auto routed = set(output.first, output.second, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    return updated;
}

bool GraphEmsEngine::runSkOverride(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto skP3 = latestValue(paramIndex(node, "skP3Index", 590), nowMs);
    const auto skQ3 = latestValue(paramIndex(node, "skQ3Index", 591), nowMs);
    if (!skP3 || !skQ3) {
        return false;
    }
    const auto routed = set(
        paramIndex(node, "runOutput", 26),
        (*skP3 != 0.0 || *skQ3 != 0.0) ? 1.0 : 0.0,
        nowMs
    );
    if (routed.accepted) {
        ++result.latestWrites;
    }
    return routed.accepted;
}

bool GraphEmsEngine::runReserveCapacity(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto zrEn = latestValue(paramIndex(node, "enableIndex", 23), nowMs);
    const auto zrP1 = latestValue(paramIndex(node, "zrP1Index", 588), nowMs);
    const auto tqPxzNegValue = latestValue(paramIndex(node, "negativeLimitIndex", 457), nowMs);
    const auto fhPa = latestValue(paramIndex(node, "fhPaIndex", 309), nowMs);
    const auto fhPb = latestValue(paramIndex(node, "fhPbIndex", 310), nowMs);
    const auto fhPc = latestValue(paramIndex(node, "fhPcIndex", 311), nowMs);
    double zrRunValue = 0.0;
    if (zrEn && *zrEn == 1.0 && zrP1 && tqPxzNegValue && fhPa && fhPb && fhPc) {
        if (*tqPxzNegValue - *fhPa - *zrP1 < 0.0 ||
            *tqPxzNegValue - *fhPb - *zrP1 < 0.0 ||
            *tqPxzNegValue - *fhPc - *zrP1 < 0.0) {
            zrRunValue = 1.0;
        }
    }
    const auto routed = set(paramIndex(node, "runOutput", 24), zrRunValue, nowMs);
    if (routed.accepted) {
        ++result.latestWrites;
    }
    return routed.accepted;
}

bool GraphEmsEngine::runFormula(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    std::vector<double> inputs;
    const auto count = paramCount(node, "inputs");
    inputs.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto prefix = "inputs." + std::to_string(i) + ".";
        const auto index = paramIndex(node, prefix + "index", 0);
        if (index != 0) {
            const auto value = latestValue(index, nowMs);
            if (!value) {
                const auto defaultValue = paramDouble(node, prefix + "defaultValue");
                if (!defaultValue || !std::isfinite(*defaultValue)) {
                    return false;
                }
                inputs.push_back(*defaultValue);
                continue;
            }
            inputs.push_back(*value);
            continue;
        }
        const auto value = paramDouble(node, prefix + "value");
        if (!value) {
            return false;
        }
        inputs.push_back(*value);
    }

    const auto operation = normalizedToken(node.params.at("operation"));
    double output = 0.0;
    if (operation == "add" || operation == "sum") {
        output = std::accumulate(inputs.begin(), inputs.end(), 0.0);
    } else if (operation == "subtract") {
        output = inputs.front();
        for (std::size_t i = 1; i < inputs.size(); ++i) {
            output -= inputs[i];
        }
    } else if (operation == "multiply") {
        output = std::accumulate(inputs.begin(), inputs.end(), 1.0, std::multiplies<double>());
    } else if (operation == "divide" || operation == "safedivide") {
        output = inputs.front();
        for (std::size_t i = 1; i < inputs.size(); ++i) {
            if (std::abs(inputs[i]) <= 1e-12) {
                if (operation == "safedivide") {
                    output = paramDouble(node, "zeroDivisorValue").value_or(0.0);
                    break;
                }
                throw std::runtime_error("formula division by zero");
            }
            output /= inputs[i];
        }
    } else if (operation == "min") {
        output = *std::min_element(inputs.begin(), inputs.end());
    } else if (operation == "max") {
        output = *std::max_element(inputs.begin(), inputs.end());
    } else if (operation == "average") {
        output = std::accumulate(inputs.begin(), inputs.end(), 0.0) / static_cast<double>(inputs.size());
    } else if (operation == "abs") {
        output = std::abs(inputs.front());
    } else if (operation == "negate") {
        output = -inputs.front();
    } else if (operation == "square") {
        output = inputs.front() * inputs.front();
    } else if (operation == "sqrt") {
        if (inputs.front() < 0.0) {
            if (normalizedToken(node.params.count("invalidPolicy") ? node.params.at("invalidPolicy") : "error") == "skip") {
                return false;
            }
            throw std::runtime_error("formula square root of negative value");
        }
        output = std::sqrt(inputs.front());
    } else if (operation == "acos") {
        if (inputs.front() < -1.0 || inputs.front() > 1.0) {
            if (normalizedToken(node.params.count("invalidPolicy") ? node.params.at("invalidPolicy") : "error") == "skip") {
                return false;
            }
            throw std::runtime_error("formula acos input outside [-1, 1]");
        }
        output = std::acos(inputs.front());
    } else if (operation == "tan") {
        output = std::tan(inputs.front());
    } else if (operation == "sin") {
        output = std::sin(inputs.front());
    } else if (operation == "cos") {
        output = std::cos(inputs.front());
    } else if (operation == "clamp") {
        const auto latestParam = [&](std::uint32_t index) {
            return latestValue(index, nowMs);
        };
        const auto lower = paramOrLatestValue(node, "lower", "lowerIndex", latestParam);
        const auto upper = paramOrLatestValue(node, "upper", "upperIndex", latestParam);
        if (!lower || !upper) {
            return false;
        }
        if (*lower > *upper) {
            throw std::runtime_error("formula clamp lower bound exceeds upper bound");
        }
        output = std::max(*lower, std::min(inputs.front(), *upper));
    }

    if (!std::isfinite(output)) {
        throw std::runtime_error("formula result is not finite");
    }
    const auto routed = set(paramIndex(node, "outputIndex"), output, nowMs);
    if (!routed.accepted) {
        throw std::runtime_error("formula output rejected: " + routed.message);
    }
    ++result.latestWrites;
    return true;
}

bool GraphEmsEngine::runTimeSource(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto component = normalizedToken(
        node.params.count("component") ? node.params.at("component") : "hour"
    );
    const auto routed = set(
        paramIndex(node, "outputIndex"),
        static_cast<double>(localTimeComponentFromEpochMs(nowMs, component)),
        nowMs
    );
    if (!routed.accepted) {
        throw std::runtime_error("timeSource output rejected: " + routed.message);
    }
    ++result.latestWrites;
    return true;
}

bool GraphEmsEngine::runWindowAggregate(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto input = latestValue(paramIndex(node, "inputIndex"), nowMs);
    if (!input || !std::isfinite(*input)) {
        return false;
    }

    double configuredWindowSize = paramDouble(node, "windowSize").value_or(10.0);
    const auto windowSizeIndex = paramIndex(node, "windowSizeIndex", 0);
    if (windowSizeIndex != 0) {
        const auto dynamicWindowSize = latestValue(windowSizeIndex, nowMs);
        if (!dynamicWindowSize || !std::isfinite(*dynamicWindowSize)) {
            return false;
        }
        configuredWindowSize = *dynamicWindowSize;
    }
    const auto windowSize = static_cast<std::size_t>(
        std::max(1.0, std::min(std::floor(configuredWindowSize), 4096.0))
    );
    const auto outputIndex = paramIndex(node, "outputIndex");
    auto& values = averageWindows_[outputIndex];
    while (values.size() >= windowSize) {
        values.erase(values.begin());
    }
    values.push_back(*input);

    const auto operationIt = node.params.find("operation");
    const auto operation = operationIt == node.params.end() ? std::string("average") : normalizedToken(operationIt->second);
    double output = 0.0;
    if (operation == "average") {
        output = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    } else if (operation == "sum") {
        output = std::accumulate(values.begin(), values.end(), 0.0);
    } else if (operation == "min") {
        output = *std::min_element(values.begin(), values.end());
    } else {
        output = *std::max_element(values.begin(), values.end());
    }

    const auto routed = set(outputIndex, output, nowMs);
    if (!routed.accepted) {
        throw std::runtime_error("windowAggregate output rejected: " + routed.message);
    }
    ++result.latestWrites;
    return true;
}

bool GraphEmsEngine::runScheduleSelect(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto currentHour = localHourFromEpochMs(nowMs);
    double power = paramDouble(node, "defaultPower").value_or(0.0);
    double targetSoc = paramDouble(node, "defaultTargetSoc").value_or(0.0);
    double mode = paramDouble(node, "defaultMode").value_or(0.0);
    const auto count = paramCount(node, "scheduleCurve");
    for (std::size_t i = 0; i < count; ++i) {
        const auto prefix = "scheduleCurve." + std::to_string(i) + ".";
        const auto hour = static_cast<int>(paramDouble(node, prefix + "hour").value());
        if (hour != currentHour) {
            continue;
        }
        power = paramDouble(node, prefix + "power").value_or(power);
        targetSoc = paramDouble(node, prefix + "targetSoc").value_or(
            paramDouble(node, prefix + "soc").value_or(targetSoc)
        );
        mode = paramDouble(node, prefix + "mode").value_or(mode);
        break;
    }

    bool updated = false;
    const std::pair<std::uint32_t, double> outputs[] = {
        {paramIndex(node, "powerOutputIndex", 0), power},
        {paramIndex(node, "socOutputIndex", 0), targetSoc},
        {paramIndex(node, "modeOutputIndex", 0), mode}
    };
    for (const auto& output : outputs) {
        if (output.first == 0) {
            continue;
        }
        const auto routed = set(output.first, output.second, nowMs);
        if (!routed.accepted) {
            throw std::runtime_error("scheduleSelect output rejected: " + routed.message);
        }
        ++result.latestWrites;
        updated = true;
    }
    return updated;
}

bool GraphEmsEngine::runPhaseArbiter(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto activeOutputs = paramIndexes(node, "activeOutputIndexes");
    const auto reactiveOutputs = paramIndexes(node, "reactiveOutputIndexes");
    const auto phaseCount = activeOutputs.size();
    std::vector<double> active(phaseCount, 0.0);
    std::vector<double> reactive(phaseCount, 0.0);

    const auto loadBase = [&](const std::string& key, std::vector<double>& target) {
        const auto indexes = paramIndexes(node, key);
        for (std::size_t i = 0; i < indexes.size(); ++i) {
            const auto value = latestValue(indexes[i], nowMs);
            target[i] = value && std::isfinite(*value) ? *value : 0.0;
        }
    };
    loadBase("activeBaseIndexes", active);
    loadBase("reactiveBaseIndexes", reactive);

    const auto candidateCount = paramCount(node, "candidates");
    for (std::size_t i = 0; i < candidateCount; ++i) {
        const auto prefix = "candidates." + std::to_string(i) + ".";
        const auto enableIndex = paramIndex(node, prefix + "enableIndex", 0);
        if (enableIndex != 0) {
            const auto enabled = latestValue(enableIndex, nowMs);
            const auto expected = paramDouble(node, prefix + "enableValue").value_or(1.0);
            if (!enabled || !std::isfinite(*enabled) || std::abs(*enabled - expected) > 1e-9) {
                continue;
            }
        }

        std::vector<double> values(phaseCount, paramDouble(node, prefix + "defaultValue").value_or(0.0));
        const auto totalIndex = paramIndex(node, prefix + "totalIndex", 0);
        if (totalIndex != 0) {
            const auto total = latestValue(totalIndex, nowMs);
            if (!total || !std::isfinite(*total)) {
                continue;
            }
            std::fill(values.begin(), values.end(), *total / static_cast<double>(phaseCount));
        } else {
            const auto indexes = paramIndexes(node, prefix + "indexes");
            for (std::size_t phase = 0; phase < indexes.size(); ++phase) {
                const auto value = latestValue(indexes[phase], nowMs);
                if (value && std::isfinite(*value)) {
                    values[phase] = *value;
                }
            }
        }

        const auto targetName = normalizedToken(node.params.count(prefix + "target")
            ? node.params.at(prefix + "target") : "active");
        const auto merge = normalizedToken(node.params.count(prefix + "merge")
            ? node.params.at(prefix + "merge") : "stronger");
        const auto direction = normalizedToken(node.params.count(prefix + "direction")
            ? node.params.at(prefix + "direction") : "any");
        auto& target = targetName == "reactive" ? reactive : active;
        for (std::size_t phase = 0; phase < phaseCount; ++phase) {
            const auto value = values[phase];
            if ((direction == "positive" && value <= 0.0) ||
                (direction == "negative" && value >= 0.0)) {
                continue;
            }
            if (merge == "max") {
                target[phase] = std::max(target[phase], value);
            } else if (merge == "min") {
                target[phase] = std::min(target[phase], value);
            } else if (merge == "add") {
                target[phase] += value;
            } else if (merge == "override") {
                target[phase] = value;
            } else if (value > 0.0) {
                target[phase] = std::max(target[phase], value);
            } else if (value < 0.0) {
                target[phase] = std::min(target[phase], value);
            }
        }
    }

    const auto overrideEnableIndex = paramIndex(node, "override.enableIndex", 0);
    if (overrideEnableIndex != 0) {
        const auto enabled = latestValue(overrideEnableIndex, nowMs);
        const auto expected = paramDouble(node, "override.enableValue").value_or(1.0);
        if (enabled && std::isfinite(*enabled) && std::abs(*enabled - expected) <= 1e-9) {
            const auto activeTotal = latestValue(paramIndex(node, "override.activeTotalIndex", 0), nowMs);
            const auto reactiveTotal = latestValue(paramIndex(node, "override.reactiveTotalIndex", 0), nowMs);
            if (activeTotal && reactiveTotal && std::isfinite(*activeTotal) && std::isfinite(*reactiveTotal)) {
                std::fill(active.begin(), active.end(), *activeTotal / static_cast<double>(phaseCount));
                std::fill(reactive.begin(), reactive.end(), *reactiveTotal / static_cast<double>(phaseCount));
            }
        }
    }

    bool updated = false;
    for (std::size_t phase = 0; phase < phaseCount; ++phase) {
        const std::pair<std::uint32_t, double> outputs[] = {
            {activeOutputs[phase], active[phase]},
            {reactiveOutputs[phase], reactive[phase]}
        };
        for (const auto& output : outputs) {
            const auto routed = set(output.first, output.second, nowMs);
            if (!routed.accepted) {
                throw std::runtime_error("phaseArbiter output rejected: " + routed.message);
            }
            ++result.latestWrites;
            updated = true;
        }
    }
    return updated;
}

bool GraphEmsEngine::runPowerConstraint(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto activeInputs = paramIndexes(node, "activeInputIndexes");
    const auto reactiveInputs = paramIndexes(node, "reactiveInputIndexes");
    const auto activeOutputs = paramIndexes(node, "activeOutputIndexes");
    const auto reactiveOutputs = paramIndexes(node, "reactiveOutputIndexes");
    const auto phaseCount = activeInputs.size();
    std::vector<double> active(phaseCount, 0.0);
    std::vector<double> reactive(phaseCount, 0.0);
    for (std::size_t phase = 0; phase < phaseCount; ++phase) {
        const auto p = latestValue(activeInputs[phase], nowMs);
        const auto q = latestValue(reactiveInputs[phase], nowMs);
        if (!p || !q || !std::isfinite(*p) || !std::isfinite(*q)) {
            return false;
        }
        active[phase] = *p;
        reactive[phase] = *q;
    }

    std::vector<Optional<double>> loads(phaseCount);
    const auto loadIndexes = paramIndexes(node, "loadIndexes");
    for (std::size_t phase = 0; phase < loadIndexes.size(); ++phase) {
        loads[phase] = latestValue(loadIndexes[phase], nowMs);
    }
    const auto allLoadsValid = [&]() {
        return loads.size() == phaseCount && std::all_of(loads.begin(), loads.end(), [](const Optional<double>& value) {
            return value && std::isfinite(*value);
        });
    };

    const auto positiveEnabled = latestValue(paramIndex(node, "positiveLimitEnableIndex", 0), nowMs);
    const auto positiveLimit = latestValue(paramIndex(node, "positiveLimitIndex", 0), nowMs);
    if (positiveEnabled && *positiveEnabled == 1.0 && positiveLimit && allLoadsValid()) {
        for (std::size_t phase = 0; phase < phaseCount; ++phase) {
            if (active[phase] > 0.0) {
                active[phase] = std::max(0.0, std::min(active[phase], *positiveLimit - *loads[phase]));
            }
        }
    }

    const auto negativeEnabled = latestValue(paramIndex(node, "negativeLimitEnableIndex", 0), nowMs);
    const auto negativeLimit = latestValue(paramIndex(node, "negativeLimitIndex", 0), nowMs);
    if (negativeEnabled && *negativeEnabled == 1.0 && negativeLimit && allLoadsValid()) {
        for (std::size_t phase = 0; phase < phaseCount; ++phase) {
            if (active[phase] < 0.0) {
                active[phase] = std::min(0.0, std::max(active[phase], *negativeLimit - *loads[phase]));
            }
        }
    }

    double reserveRun = 0.0;
    const auto reserveEnabled = latestValue(paramIndex(node, "reserveEnableIndex", 0), nowMs);
    const auto reserveMargin = latestValue(paramIndex(node, "reserveMarginIndex", 0), nowMs);
    if (reserveEnabled && *reserveEnabled == 1.0 && reserveMargin && negativeLimit && allLoadsValid()) {
        for (std::size_t phase = 0; phase < phaseCount; ++phase) {
            const auto reserveTarget = *negativeLimit - *loads[phase] - *reserveMargin;
            if (reserveTarget < 0.0) {
                active[phase] = std::min(active[phase], reserveTarget);
                reserveRun = 1.0;
            }
        }
    }

    const auto clampAbs = [](double value, double limit) {
        if (!(limit > 0.0) || std::abs(value) <= limit) {
            return value;
        }
        return value > 0.0 ? limit : -limit;
    };
    const auto activeLimit = latestValue(paramIndex(node, "activeAbsLimitIndex", 0), nowMs);
    const auto reactiveLimit = latestValue(paramIndex(node, "reactiveAbsLimitIndex", 0), nowMs);
    for (std::size_t phase = 0; phase < phaseCount; ++phase) {
        if (activeLimit) {
            active[phase] = clampAbs(active[phase], *activeLimit);
        }
        if (reactiveLimit) {
            reactive[phase] = clampAbs(reactive[phase], *reactiveLimit);
        }
    }

    const auto apparentTotalLimit = latestValue(paramIndex(node, "apparentTotalLimitIndex", 0), nowMs);
    if (apparentTotalLimit && *apparentTotalLimit > 0.0) {
        const auto phaseLimit = *apparentTotalLimit / static_cast<double>(phaseCount);
        for (std::size_t phase = 0; phase < phaseCount; ++phase) {
            const auto apparent = apparentPower(active[phase], reactive[phase]);
            if (apparent <= phaseLimit) {
                continue;
            }
            const auto remaining = std::max(0.0, phaseLimit * phaseLimit - reactive[phase] * reactive[phase]);
            active[phase] = active[phase] > 0.0 ? std::sqrt(remaining)
                : active[phase] < 0.0 ? -std::sqrt(remaining) : 0.0;
        }
    }

    const auto activeTotal = std::accumulate(active.begin(), active.end(), 0.0);
    if (activeTotal > 0.0) {
        const auto limit = latestValue(paramIndex(node, "positiveTotalLimitIndex", 0), nowMs);
        if (limit && activeTotal > *limit) {
            const auto positive = std::accumulate(active.begin(), active.end(), 0.0, [](double total, double value) {
                return total + (value > 0.0 ? value : 0.0);
            });
            const auto negative = activeTotal - positive;
            if (positive > 0.0) {
                const auto ratio = (*limit - negative) / positive;
                for (auto& value : active) {
                    if (value > 0.0) value *= ratio;
                }
            }
        }
    } else if (activeTotal < 0.0) {
        const auto limit = latestValue(paramIndex(node, "negativeTotalLimitIndex", 0), nowMs);
        if (limit && activeTotal < -*limit) {
            const auto positive = std::accumulate(active.begin(), active.end(), 0.0, [](double total, double value) {
                return total + (value > 0.0 ? value : 0.0);
            });
            const auto negative = activeTotal - positive;
            if (negative < 0.0) {
                const auto ratio = (-*limit - positive) / negative;
                for (auto& value : active) {
                    if (value < 0.0) value *= ratio;
                }
            }
        }
    }

    bool lowState = false;
    bool highState = false;
    const auto state = latestValue(paramIndex(node, "stateIndex", 0), nowMs);
    const auto upper = latestValue(paramIndex(node, "stateUpperIndex", 0), nowMs);
    const auto lower = latestValue(paramIndex(node, "stateLowerIndex", 0), nowMs);
    if (state && upper && lower) {
        lowState = *state < *lower;
        highState = *state > *upper;
        for (std::size_t phase = 0; phase < phaseCount; ++phase) {
            if (lowState && active[phase] < 0.0) active[phase] = 0.0;
            if (highState && active[phase] > 0.0) active[phase] = 0.0;
            if (lowState && paramBool(node, "lowStateClearReactive", true)) reactive[phase] = 0.0;
        }
    }

    bool updated = false;
    const auto write = [&](std::uint32_t index, double value) {
        if (index == 0) return;
        const auto routed = set(index, value, nowMs);
        if (!routed.accepted) {
            throw std::runtime_error("powerConstraint output rejected: " + routed.message);
        }
        ++result.latestWrites;
        updated = true;
    };
    for (std::size_t phase = 0; phase < phaseCount; ++phase) {
        write(activeOutputs[phase], active[phase]);
        write(reactiveOutputs[phase], reactive[phase]);
    }
    write(paramIndex(node, "reserveRunOutputIndex", 0), reserveRun);
    if (lowState) {
        for (const auto index : paramIndexes(node, "lowStateClearIndexes")) write(index, 0.0);
    }
    if (highState) {
        for (const auto index : paramIndexes(node, "highStateClearIndexes")) write(index, 0.0);
    }
    return updated;
}

bool GraphEmsEngine::runSwitch(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto latestParam = [&](std::uint32_t index) {
        return latestValue(index, nowMs);
    };
    bool condition = false;
    const auto conditionIndex = paramIndex(node, "conditionIndex", 0);
    if (conditionIndex != 0) {
        const auto value = latestValue(conditionIndex, nowMs);
        if (!value) {
            return false;
        }
        condition = *value != 0.0;
    } else {
        const auto left = paramOrLatestValue(node, "leftValue", "leftIndex", latestParam);
        const auto right = paramOrLatestValue(node, "rightValue", "rightIndex", latestParam);
        if (!left || !right) {
            return false;
        }
        const auto tolerance = std::abs(paramDouble(node, "tolerance").value_or(1e-9));
        condition = evaluateComparison(*left, *right, node.params.at("operator"), tolerance);
    }

    const auto selected = condition
        ? paramOrLatestValue(node, "trueValue", "trueIndex", latestParam)
        : paramOrLatestValue(node, "falseValue", "falseIndex", latestParam);
    if (!selected || !std::isfinite(*selected)) {
        return false;
    }
    const auto routed = set(paramIndex(node, "outputIndex"), *selected, nowMs);
    if (!routed.accepted) {
        throw std::runtime_error("switch output rejected: " + routed.message);
    }
    ++result.latestWrites;
    return true;
}

bool GraphEmsEngine::runControlGate(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto combineIt = node.params.find("combine");
    const auto combine = combineIt == node.params.end() ? std::string("all") : normalizedToken(combineIt->second);
    const bool requireAll = combine == "all";
    bool permitted = requireAll;
    bool missingInput = false;
    const auto count = paramCount(node, "conditions");
    for (std::size_t i = 0; i < count; ++i) {
        const auto prefix = "conditions." + std::to_string(i) + ".";
        const auto left = latestValue(paramIndex(node, prefix + "index"), nowMs);
        Optional<double> right = paramDouble(node, prefix + "value");
        if (!right) {
            right = latestValue(paramIndex(node, prefix + "valueIndex"), nowMs);
        }
        if (!left || !right || !std::isfinite(*left) || !std::isfinite(*right)) {
            missingInput = true;
            permitted = false;
            break;
        }
        const auto tolerance = std::abs(paramDouble(node, prefix + "tolerance").value_or(1e-9));
        bool passed = evaluateComparison(*left, *right, node.params.at(prefix + "operator"), tolerance);
        if (paramBool(node, prefix + "invert", false)) {
            passed = !passed;
        }
        if (requireAll) {
            permitted = permitted && passed;
            if (!permitted) {
                break;
            }
        } else {
            permitted = permitted || passed;
        }
    }
    if (missingInput) {
        permitted = false;
    }

    const auto routed = set(paramIndex(node, "outputIndex"), permitted ? 1.0 : 0.0, nowMs);
    if (!routed.accepted) {
        throw std::runtime_error("controlGate output rejected: " + routed.message);
    }
    ++result.latestWrites;
    return true;
}

bool GraphEmsEngine::runFeedbackVerify(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto target = latestValue(paramIndex(node, "targetIndex"), nowMs);
    const auto feedback = latestValue(paramIndex(node, "feedbackIndex"), nowMs);
    auto& state = feedbackVerifyStates_[node.id];
    double status = -2.0;

    if (!target || !std::isfinite(*target)) {
        state = FeedbackVerifyState{};
    } else {
        const auto targetChangeTolerance = paramDouble(node, "targetChangeTolerance").value_or(1e-9);
        if (!state.initialized || nowMs < state.targetChangedAt ||
            std::abs(*target - state.target) > targetChangeTolerance) {
            state.initialized = true;
            state.target = *target;
            state.targetChangedAt = nowMs;
        }

        if (feedback && std::isfinite(*feedback)) {
            const auto tolerance = paramDouble(node, "tolerance").value_or(0.0);
            if (std::abs(*feedback - state.target) <= tolerance) {
                status = 1.0;
            } else {
                const auto timeoutMs = static_cast<std::int64_t>(paramDouble(node, "timeoutMs").value_or(5000.0));
                status = nowMs - state.targetChangedAt >= timeoutMs ? -1.0 : 0.0;
            }
        }
    }

    const auto routed = set(paramIndex(node, "outputIndex"), status, nowMs);
    if (!routed.accepted) {
        throw std::runtime_error("feedbackVerify output rejected: " + routed.message);
    }
    ++result.latestWrites;
    return true;
}

bool GraphEmsEngine::runControlWrite(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    if (!paramBool(node, "submitWrites", false)) {
        return false;
    }

    const auto permitIndex = paramIndex(node, "permitIndex", 0);
    if (permitIndex != 0) {
        const auto permit = latestValue(permitIndex, nowMs);
        const auto expected = paramDouble(node, "permitValue").value_or(1.0);
        const auto tolerance = paramDouble(node, "permitTolerance").value_or(1e-9);
        if (!permit || !std::isfinite(*permit) || std::abs(*permit - expected) > tolerance) {
            return false;
        }
    }

    const auto input = latestValue(paramIndex(node, "inputIndex"), nowMs);
    if (!input || !std::isfinite(*input)) {
        return false;
    }
    const auto minValue = paramDouble(node, "minValue").value();
    const auto maxValue = paramDouble(node, "maxValue").value();
    if (*input < minValue || *input > maxValue) {
        throw std::runtime_error("controlWrite input outside configured bounds");
    }

    double commandValue = *input;
    const auto valueModeIt = node.params.find("valueMode");
    const auto valueMode = valueModeIt == node.params.end() ? std::string("none") : normalizedToken(valueModeIt->second);
    if (valueMode == "truncate") {
        commandValue = std::trunc(commandValue);
    } else if (valueMode == "round") {
        commandValue = std::round(commandValue);
    }

    const auto targetIndex = paramIndex(node, "targetIndex");
    const auto deadband = paramDouble(node, "deadband").value_or(0.0);
    const auto current = latestValue(targetIndex, nowMs);
    if (current && std::abs(*current - commandValue) <= deadband) {
        return false;
    }
    const auto duplicateTolerance = std::max(deadband, 1e-9);
    for (const auto& pending : router_.peekPendingWrites()) {
        if (pending.index == targetIndex && std::abs(pending.value - commandValue) <= duplicateTolerance) {
            return false;
        }
    }

    PendingWriteCommand pending;
    pending.cmdId = graphCmdId(targetIndex, nowMs);
    pending.index = targetIndex;
    pending.value = commandValue;
    pending.source = "graph-ems";
    pending.ts = nowMs;
    pending.highPriority = paramBool(node, "highPriority", false);
    const auto routed = router_.submitWriteCommand(pending);
    if (!routed.accepted) {
        throw std::runtime_error("controlWrite rejected: " + routed.message);
    }
    ++result.deviceWrites;
    return true;
}

bool GraphEmsEngine::runRateLimit(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto input = latestValue(paramIndex(node, "inputIndex"), nowMs);
    if (!input || !std::isfinite(*input)) {
        return false;
    }
    const auto minValue = paramDouble(node, "minValue").value();
    const auto maxValue = paramDouble(node, "maxValue").value();
    if (*input < minValue || *input > maxValue) {
        throw std::runtime_error("rateLimit input outside configured bounds");
    }

    const auto outputIndex = paramIndex(node, "outputIndex");
    auto& state = rateLimitStates_[node.id];
    double next = state.output;
    if (!state.initialized) {
        const auto existing = latestValue(outputIndex, nowMs);
        next = existing && std::isfinite(*existing)
            ? *existing
            : paramDouble(node, "initialValue").value_or(0.0);
    } else if (nowMs >= state.lastRunAt) {
        const auto elapsedSeconds = static_cast<double>(nowMs - state.lastRunAt) / 1000.0;
        const auto delta = *input - state.output;
        if (delta > 0.0) {
            next = state.output + std::min(delta, paramDouble(node, "risePerSecond").value() * elapsedSeconds);
        } else if (delta < 0.0) {
            next = state.output + std::max(delta, -paramDouble(node, "fallPerSecond").value() * elapsedSeconds);
        }
    }

    next = std::max(minValue, std::min(next, maxValue));
    const auto routed = set(outputIndex, next, nowMs);
    if (!routed.accepted) {
        throw std::runtime_error("rateLimit output rejected: " + routed.message);
    }
    state.initialized = true;
    state.output = next;
    state.lastRunAt = nowMs;
    ++result.latestWrites;
    return true;
}

bool GraphEmsEngine::runHysteresis(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto input = latestValue(paramIndex(node, "inputIndex"), nowMs);
    const auto outputIndex = paramIndex(node, "outputIndex");
    auto& state = hysteresisStates_[node.id];
    if (!state.initialized) {
        const auto existing = latestValue(outputIndex, nowMs);
        const auto invert = paramBool(node, "invert", false);
        state.output = existing ? ((*existing != 0.0) != invert) : paramBool(node, "initialState", false);
        state.initialized = true;
    }

    if (!input || !std::isfinite(*input)) {
        state.output = false;
    } else if (*input >= paramDouble(node, "highThreshold").value()) {
        state.output = true;
    } else if (*input <= paramDouble(node, "lowThreshold").value()) {
        state.output = false;
    }
    const auto output = input && std::isfinite(*input) &&
        (paramBool(node, "invert", false) ? !state.output : state.output);
    const auto routed = set(outputIndex, output ? 1.0 : 0.0, nowMs);
    if (!routed.accepted) {
        throw std::runtime_error("hysteresis output rejected: " + routed.message);
    }
    ++result.latestWrites;
    return true;
}

bool GraphEmsEngine::runDebounce(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto input = latestValue(paramIndex(node, "inputIndex"), nowMs);
    const auto outputIndex = paramIndex(node, "outputIndex");
    auto& state = debounceStates_[node.id];
    const bool inputValid = input && std::isfinite(*input);
    const bool desired = inputValid && (*input != 0.0);
    if (!state.initialized) {
        const auto existing = latestValue(outputIndex, nowMs);
        const auto invert = paramBool(node, "invert", false);
        state.output = existing ? ((*existing != 0.0) != invert) : paramBool(node, "initialState", false);
        state.candidate = desired;
        state.candidateSince = nowMs;
        state.initialized = true;
    }

    if (!inputValid) {
        state.output = false;
        state.candidate = false;
        state.candidateSince = nowMs;
    } else {
        if (desired != state.candidate || nowMs < state.candidateSince) {
            state.candidate = desired;
            state.candidateSince = nowMs;
        }
        if (state.candidate != state.output) {
            const auto delay = static_cast<std::int64_t>(paramDouble(
                node,
                state.candidate ? "onDelayMs" : "offDelayMs"
            ).value_or(0.0));
            if (nowMs - state.candidateSince >= delay) {
                state.output = state.candidate;
            }
        }
    }

    const auto output = inputValid &&
        (paramBool(node, "invert", false) ? !state.output : state.output);
    const auto routed = set(outputIndex, output ? 1.0 : 0.0, nowMs);
    if (!routed.accepted) {
        throw std::runtime_error("debounce output rejected: " + routed.message);
    }
    ++result.latestWrites;
    return true;
}

bool GraphEmsEngine::runSequence(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    std::set<int> configuredStates;
    const auto stateCount = paramCount(node, "states");
    for (std::size_t i = 0; i < stateCount; ++i) {
        configuredStates.insert(static_cast<int>(paramDouble(
            node,
            "states." + std::to_string(i) + ".id"
        ).value()));
    }

    auto& state = sequenceStates_[node.id];
    bool skipTransitions = false;
    if (!state.initialized) {
        const auto existing = latestValue(paramIndex(node, "stateOutputIndex"), nowMs);
        const auto existingState = existing && std::isfinite(*existing) && std::floor(*existing) == *existing
            ? static_cast<int>(*existing)
            : -1;
        state.current = configuredStates.find(existingState) != configuredStates.end()
            ? existingState
            : static_cast<int>(paramDouble(node, "initialState").value_or(
                static_cast<double>(*configuredStates.begin())
            ));
        state.enteredAt = nowMs;
        state.initialized = true;
        skipTransitions = !paramBool(node, "evaluateOnInitialize", false);
    } else if (nowMs < state.enteredAt) {
        state.enteredAt = nowMs;
        skipTransitions = true;
    }

    if (!skipTransitions) {
        const auto transitionCount = paramCount(node, "transitions");
        for (std::size_t i = 0; i < transitionCount; ++i) {
            const auto prefix = "transitions." + std::to_string(i) + ".";
            const auto from = static_cast<int>(paramDouble(node, prefix + "from").value());
            if (from != state.current) {
                continue;
            }
            const auto minDurationMs = static_cast<std::int64_t>(
                paramDouble(node, prefix + "minDurationMs").value_or(0.0)
            );
            if (nowMs - state.enteredAt < minDurationMs) {
                continue;
            }

            const auto combineIt = node.params.find(prefix + "combine");
            const auto combine = combineIt == node.params.end() ? std::string("all") : normalizedToken(combineIt->second);
            const bool requireAll = combine == "all";
            bool matched = requireAll;
            bool allInputsValid = true;
            const auto conditionCount = paramCount(node, prefix + "conditions");
            for (std::size_t j = 0; j < conditionCount; ++j) {
                const auto conditionPrefix = prefix + "conditions." + std::to_string(j) + ".";
                const auto left = latestValue(paramIndex(node, conditionPrefix + "index"), nowMs);
                Optional<double> right = paramDouble(node, conditionPrefix + "value");
                if (!right) {
                    right = latestValue(paramIndex(node, conditionPrefix + "valueIndex"), nowMs);
                }
                if (!left || !right || !std::isfinite(*left) || !std::isfinite(*right)) {
                    allInputsValid = false;
                    continue;
                }
                bool passed = evaluateComparison(
                    *left,
                    *right,
                    node.params.at(conditionPrefix + "operator"),
                    paramDouble(node, conditionPrefix + "tolerance").value_or(1e-9)
                );
                if (paramBool(node, conditionPrefix + "invert", false)) {
                    passed = !passed;
                }
                matched = requireAll ? (matched && passed) : (matched || passed);
            }
            if (!allInputsValid || !matched) {
                continue;
            }

            state.current = static_cast<int>(paramDouble(node, prefix + "to").value());
            state.enteredAt = nowMs;
            break;
        }
    }

    const auto routed = set(paramIndex(node, "stateOutputIndex"), static_cast<double>(state.current), nowMs);
    if (!routed.accepted) {
        throw std::runtime_error("sequence state output rejected: " + routed.message);
    }
    ++result.latestWrites;
    return true;
}

bool GraphEmsEngine::runPcsPowerSolve(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    const auto outQaCos = latestValue(paramIndex(node, "outQaCosIndex", 601), nowMs);
    const auto outQbCos = latestValue(paramIndex(node, "outQbCosIndex", 602), nowMs);
    const auto outQcCos = latestValue(paramIndex(node, "outQcCosIndex", 603), nowMs);
    const auto skRun = latestValue(paramIndex(node, "skRunIndex", 26), nowMs);
    const auto skP3 = latestValue(paramIndex(node, "skP3Index", 590), nowMs);
    const auto skQ3 = latestValue(paramIndex(node, "skQ3Index", 591), nowMs);
    const auto zrEn = latestValue(paramIndex(node, "zrEnableIndex", 23), nowMs);
    const auto zrP1 = latestValue(paramIndex(node, "zrP1Index", 588), nowMs);
    const auto tqPxzPosEn = latestValue(paramIndex(node, "positiveLimitEnableIndex", 454), nowMs);
    const auto tqPxzPosValue = latestValue(paramIndex(node, "positiveLimitIndex", 453), nowMs);
    const auto tqPxzNegEn = latestValue(paramIndex(node, "negativeLimitEnableIndex", 458), nowMs);
    const auto tqPxzNegValue = latestValue(paramIndex(node, "negativeLimitIndex", 457), nowMs);
    const auto fhPa = latestValue(paramIndex(node, "fhPaIndex", 309), nowMs);
    const auto fhPb = latestValue(paramIndex(node, "fhPbIndex", 310), nowMs);
    const auto fhPc = latestValue(paramIndex(node, "fhPcIndex", 311), nowMs);
    const auto outP3Cd = latestValue(paramIndex(node, "outP3CdIndex", 613), nowMs);
    const auto outP3Fd = latestValue(paramIndex(node, "outP3FdIndex", 614), nowMs);
    const auto outPaDs = latestValue(paramIndex(node, "outPaDsIndex", 615), nowMs);
    const auto outPbDs = latestValue(paramIndex(node, "outPbDsIndex", 616), nowMs);
    const auto outPcDs = latestValue(paramIndex(node, "outPcDsIndex", 617), nowMs);
    const auto outPaLv = latestValue(paramIndex(node, "outPaLvIndex", 605), nowMs);
    const auto outPbLv = latestValue(paramIndex(node, "outPbLvIndex", 606), nowMs);
    const auto outPcLv = latestValue(paramIndex(node, "outPcLvIndex", 607), nowMs);
    const auto outPaHv = latestValue(paramIndex(node, "outPaHvIndex", 609), nowMs);
    const auto outPbHv = latestValue(paramIndex(node, "outPbHvIndex", 610), nowMs);
    const auto outPcHv = latestValue(paramIndex(node, "outPcHvIndex", 611), nowMs);
    const auto outPaGf = latestValue(paramIndex(node, "outPaGfIndex", 619), nowMs);
    const auto outPbGf = latestValue(paramIndex(node, "outPbGfIndex", 620), nowMs);
    const auto outPcGf = latestValue(paramIndex(node, "outPcGfIndex", 621), nowMs);
    const auto outPaPh = latestValue(paramIndex(node, "outPaPhIndex", 623), nowMs);
    const auto outPbPh = latestValue(paramIndex(node, "outPbPhIndex", 624), nowMs);
    const auto outPcPh = latestValue(paramIndex(node, "outPcPhIndex", 625), nowMs);
    const auto pMax = latestValue(paramIndex(node, "pMaxIndex", 535), nowMs);
    const auto qMax = latestValue(paramIndex(node, "qMaxIndex", 504), nowMs);
    const auto s3Max = latestValue(paramIndex(node, "s3MaxIndex", 151), nowMs);

    double pcsPaOut = outPaDs ? *outPaDs : 0.0;
    double pcsPbOut = outPbDs ? *outPbDs : 0.0;
    double pcsPcOut = outPcDs ? *outPcDs : 0.0;
    double pcsQaOut = outQaCos ? *outQaCos : 0.0;
    double pcsQbOut = outQbCos ? *outQbCos : 0.0;
    double pcsQcOut = outQcCos ? *outQcCos : 0.0;

    const double cdFdP3 = (outP3Cd ? *outP3Cd : 0.0) + (outP3Fd ? *outP3Fd : 0.0);
    if (cdFdP3 != 0.0) {
        const double cdFdPhaseP = cdFdP3 / 3.0;
        if (cdFdPhaseP > 0.0) {
            pcsPaOut = std::max(pcsPaOut, cdFdPhaseP);
            pcsPbOut = std::max(pcsPbOut, cdFdPhaseP);
            pcsPcOut = std::max(pcsPcOut, cdFdPhaseP);
        } else {
            pcsPaOut = std::min(pcsPaOut, cdFdPhaseP);
            pcsPbOut = std::min(pcsPbOut, cdFdPhaseP);
            pcsPcOut = std::min(pcsPcOut, cdFdPhaseP);
        }
    }

    if (outPaLv && *outPaLv < 0.0) {
        pcsPaOut = std::min(pcsPaOut, *outPaLv);
    }
    if (outPbLv && *outPbLv < 0.0) {
        pcsPbOut = std::min(pcsPbOut, *outPbLv);
    }
    if (outPcLv && *outPcLv < 0.0) {
        pcsPcOut = std::min(pcsPcOut, *outPcLv);
    }
    if (outPaHv && *outPaHv > 0.0) {
        pcsPaOut = std::max(pcsPaOut, *outPaHv);
    }
    if (outPbHv && *outPbHv > 0.0) {
        pcsPbOut = std::max(pcsPbOut, *outPbHv);
    }
    if (outPcHv && *outPcHv > 0.0) {
        pcsPcOut = std::max(pcsPcOut, *outPcHv);
    }
    if (outPaGf && *outPaGf > 0.0) {
        pcsPaOut = std::max(pcsPaOut, *outPaGf);
    }
    if (outPbGf && *outPbGf > 0.0) {
        pcsPbOut = std::max(pcsPbOut, *outPbGf);
    }
    if (outPcGf && *outPcGf > 0.0) {
        pcsPcOut = std::max(pcsPcOut, *outPcGf);
    }

    pcsPaOut += outPaPh ? *outPaPh : 0.0;
    pcsPbOut += outPbPh ? *outPbPh : 0.0;
    pcsPcOut += outPcPh ? *outPcPh : 0.0;

    if (skRun && *skRun == 1.0 && skP3 && skQ3) {
        pcsPaOut = pcsPbOut = pcsPcOut = *skP3 / 3.0;
        pcsQaOut = pcsQbOut = pcsQcOut = *skQ3 / 3.0;
    }

    if (tqPxzPosEn && *tqPxzPosEn == 1.0 && tqPxzPosValue && fhPa && fhPb && fhPc) {
        const double powerACdSy = *tqPxzPosValue - *fhPa;
        const double powerBCdSy = *tqPxzPosValue - *fhPb;
        const double powerCCdSy = *tqPxzPosValue - *fhPc;
        if (pcsPaOut > 0.0) {
            pcsPaOut = std::min(pcsPaOut, powerACdSy);
            if (pcsPaOut < 0.0) {
                pcsPaOut = 0.0;
            }
        }
        if (pcsPbOut > 0.0) {
            pcsPbOut = std::min(pcsPbOut, powerBCdSy);
            if (pcsPbOut < 0.0) {
                pcsPbOut = 0.0;
            }
        }
        if (pcsPcOut > 0.0) {
            pcsPcOut = std::min(pcsPcOut, powerCCdSy);
            if (pcsPcOut < 0.0) {
                pcsPcOut = 0.0;
            }
        }
    }

    if (tqPxzNegEn && *tqPxzNegEn == 1.0 && tqPxzNegValue && fhPa && fhPb && fhPc) {
        const double powerAFdSy = *tqPxzNegValue - *fhPa;
        const double powerBFdSy = *tqPxzNegValue - *fhPb;
        const double powerCFdSy = *tqPxzNegValue - *fhPc;
        if (pcsPaOut < 0.0) {
            pcsPaOut = std::max(pcsPaOut, powerAFdSy);
            if (pcsPaOut > 0.0) {
                pcsPaOut = 0.0;
            }
        }
        if (pcsPbOut < 0.0) {
            pcsPbOut = std::max(pcsPbOut, powerBFdSy);
            if (pcsPbOut > 0.0) {
                pcsPbOut = 0.0;
            }
        }
        if (pcsPcOut < 0.0) {
            pcsPcOut = std::max(pcsPcOut, powerCFdSy);
            if (pcsPcOut > 0.0) {
                pcsPcOut = 0.0;
            }
        }
    }

    double zrRunValue = 0.0;
    if (zrEn && *zrEn == 1.0 && tqPxzNegValue && zrP1 && fhPa && fhPb && fhPc) {
        const double powerAZxSy = *tqPxzNegValue - *fhPa - *zrP1;
        const double powerBZxSy = *tqPxzNegValue - *fhPb - *zrP1;
        const double powerCZxSy = *tqPxzNegValue - *fhPc - *zrP1;
        if (powerAZxSy < 0.0) {
            pcsPaOut = std::min(pcsPaOut, powerAZxSy);
            zrRunValue = 1.0;
        }
        if (powerBZxSy < 0.0) {
            pcsPbOut = std::min(pcsPbOut, powerBZxSy);
            zrRunValue = 1.0;
        }
        if (powerCZxSy < 0.0) {
            pcsPcOut = std::min(pcsPcOut, powerCZxSy);
            zrRunValue = 1.0;
        }
    }

    auto clampAbs = [](double value, double limit) {
        if (!(limit > 0.0) || std::abs(value) <= limit) {
            return value;
        }
        return value > 0.0 ? limit : -limit;
    };

    if (pMax) {
        pcsPaOut = clampAbs(pcsPaOut, *pMax);
        pcsPbOut = clampAbs(pcsPbOut, *pMax);
        pcsPcOut = clampAbs(pcsPcOut, *pMax);
    }
    if (qMax) {
        pcsQaOut = clampAbs(pcsQaOut, *qMax);
        pcsQbOut = clampAbs(pcsQbOut, *qMax);
        pcsQcOut = clampAbs(pcsQcOut, *qMax);
    }

    if (s3Max && *s3Max > 0.0) {
        const double s1Max = *s3Max / 3.0;
        auto clampByApparentPower = [&](double& p, double q) {
            const double s = apparentPower(p, q);
            if (!(s1Max > 0.0) || s <= s1Max) {
                return;
            }
            const double remain = std::max(0.0, s1Max * s1Max - q * q);
            if (p > 0.0) {
                p = std::sqrt(remain);
            } else if (p < 0.0) {
                p = -std::sqrt(remain);
            } else {
                p = 0.0;
            }
        };
        clampByApparentPower(pcsPaOut, pcsQaOut);
        clampByApparentPower(pcsPbOut, pcsQbOut);
        clampByApparentPower(pcsPcOut, pcsQcOut);
    }

    const double pcsP3Out = pcsPaOut + pcsPbOut + pcsPcOut;
    if (pcsP3Out > 0.0) {
        const auto chargeKwAllow = latestValue(paramIndex(node, "chargeKwAllowIndex", 1552), nowMs);
        if (chargeKwAllow && pcsP3Out > *chargeKwAllow) {
            double pcsPos = 0.0;
            double pcsNeg = 0.0;
            const double values[] = {pcsPaOut, pcsPbOut, pcsPcOut};
            for (const auto value : values) {
                if (value > 0.0) {
                    pcsPos += value;
                } else {
                    pcsNeg += value;
                }
            }
            if (pcsPos > 0.0) {
                const double bmsPer = (*chargeKwAllow - pcsNeg) / pcsPos;
                if (pcsPaOut > 0.0) {
                    pcsPaOut *= bmsPer;
                }
                if (pcsPbOut > 0.0) {
                    pcsPbOut *= bmsPer;
                }
                if (pcsPcOut > 0.0) {
                    pcsPcOut *= bmsPer;
                }
            }
        }
    } else if (pcsP3Out < 0.0) {
        const auto dischargeKwAllow = latestValue(paramIndex(node, "dischargeKwAllowIndex", 1553), nowMs);
        if (dischargeKwAllow) {
            const double bmsDischargeKwAllow = -*dischargeKwAllow;
            if (pcsP3Out < bmsDischargeKwAllow) {
                double pcsPos = 0.0;
                double pcsNeg = 0.0;
                const double values[] = {pcsPaOut, pcsPbOut, pcsPcOut};
                for (const auto value : values) {
                    if (value > 0.0) {
                        pcsPos += value;
                    } else {
                        pcsNeg += value;
                    }
                }
                if (pcsNeg < 0.0) {
                    const double bmsPer = (bmsDischargeKwAllow - pcsPos) / pcsNeg;
                    if (pcsPaOut < 0.0) {
                        pcsPaOut *= bmsPer;
                    }
                    if (pcsPbOut < 0.0) {
                        pcsPbOut *= bmsPer;
                    }
                    if (pcsPcOut < 0.0) {
                        pcsPcOut *= bmsPer;
                    }
                }
            }
        }
    }

    const auto bmsSoc = latestValue(paramIndex(node, "bmsSocIndex", 1570), nowMs);
    const auto bmsSocMax = latestValue(paramIndex(node, "bmsSocMaxIndex", 161), nowMs);
    const auto bmsSocMin = latestValue(paramIndex(node, "bmsSocMinIndex", 162), nowMs);
    bool bmsLowLimit = false;
    bool bmsUpLimit = false;
    if (bmsSoc && bmsSocMax && bmsSocMin) {
        if (*bmsSoc < *bmsSocMin) {
            bmsLowLimit = true;
            if (pcsPaOut < 0.0) {
                pcsPaOut = 0.0;
            }
            if (pcsPbOut < 0.0) {
                pcsPbOut = 0.0;
            }
            if (pcsPcOut < 0.0) {
                pcsPcOut = 0.0;
            }
            pcsQaOut = 0.0;
            pcsQbOut = 0.0;
            pcsQcOut = 0.0;
        }
        if (*bmsSoc > *bmsSocMax) {
            bmsUpLimit = true;
            if (pcsPaOut > 0.0) {
                pcsPaOut = 0.0;
            }
            if (pcsPbOut > 0.0) {
                pcsPbOut = 0.0;
            }
            if (pcsPcOut > 0.0) {
                pcsPcOut = 0.0;
            }
        }
    }

    bool updated = false;
    std::vector<std::pair<std::uint32_t, double>> outputs;
    if (bmsLowLimit) {
        outputs.push_back({paramIndex(node, "cosRunOutput", 8), 0.0});
        outputs.push_back({paramIndex(node, "lvRunOutput", 10), 0.0});
    }
    if (bmsUpLimit) {
        outputs.push_back({paramIndex(node, "hvRunOutput", 12), 0.0});
        outputs.push_back({paramIndex(node, "gfRunOutput", 22), 0.0});
    }
    outputs.push_back({paramIndex(node, "zrRunOutput", 24), zrRunValue});
    outputs.push_back({paramIndex(node, "paOutput", 627), pcsPaOut});
    outputs.push_back({paramIndex(node, "pbOutput", 628), pcsPbOut});
    outputs.push_back({paramIndex(node, "pcOutput", 629), pcsPcOut});
    outputs.push_back({paramIndex(node, "qaOutput", 630), pcsQaOut});
    outputs.push_back({paramIndex(node, "qbOutput", 631), pcsQbOut});
    outputs.push_back({paramIndex(node, "qcOutput", 632), pcsQcOut});
    for (const auto& output : outputs) {
        const auto routed = set(output.first, output.second, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    if (paramBool(node, "submitWrites", false)) {
        updated = submitPcsWritebackCommands(node, nowMs, result, true) || updated;
    }
    return updated;
}

bool GraphEmsEngine::runPcsWriteback(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result
) {
    if (!paramBool(node, "submitWrites", false)) {
        return false;
    }
    return submitPcsWritebackCommands(node, nowMs, result, false);
}

bool GraphEmsEngine::submitPcsWritebackCommands(
    const GraphEmsNodeConfig& node,
    std::int64_t nowMs,
    GraphEmsRunResult& result,
    bool submitMissingZeroTargets
) {
    const auto pcsComStatus = latestValue(paramIndex(node, "comStatusIndex", 1399), nowMs);
    if (!pcsComStatus || *pcsComStatus != 1.0) {
        return false;
    }
    const auto permitIndex = paramIndex(node, "permitIndex", 0);
    if (permitIndex != 0) {
        const auto permit = latestValue(permitIndex, nowMs);
        const auto expected = paramDouble(node, "permitValue").value_or(1.0);
        const auto tolerance = std::abs(paramDouble(node, "permitTolerance").value_or(1e-9));
        if (!permit || std::abs(*permit - expected) > tolerance) {
            return false;
        }
    }

    struct PcsCommandTarget {
        std::uint32_t inputIndex = 0;
        std::uint32_t outputIndex = 0;
    };

    const PcsCommandTarget commands[] = {
        {paramIndex(node, "paInput", 627), paramIndex(node, "pControlAIndex", 1318)},
        {paramIndex(node, "pbInput", 628), paramIndex(node, "pControlBIndex", 1319)},
        {paramIndex(node, "pcInput", 629), paramIndex(node, "pControlCIndex", 1320)},
        {paramIndex(node, "qaInput", 630), paramIndex(node, "qControlAIndex", 1321)},
        {paramIndex(node, "qbInput", 631), paramIndex(node, "qControlBIndex", 1322)},
        {paramIndex(node, "qcInput", 632), paramIndex(node, "qControlCIndex", 1323)}
    };

    bool submitted = false;
    const auto pendingWrites = router_.peekPendingWrites();
    for (const auto& command : commands) {
        const auto target = latestValue(command.inputIndex, nowMs);
        if (!target) {
            continue;
        }
        const int targetValue = static_cast<int>(*target);
        const double targetDouble = static_cast<double>(targetValue);
        const auto currentValue = latestValue(command.outputIndex, nowMs);
        const double current = currentValue.value_or(0.0);
        if (targetValue == 0) {
            if (currentValue || !submitMissingZeroTargets) {
                if (current == 0.0) {
                    continue;
                }
            }
        } else if (valueMatches(current, targetDouble)) {
            if (currentValue) {
                continue;
            }
        }
        bool pendingDuplicate = false;
        for (const auto& pending : pendingWrites) {
            if (pending.index == command.outputIndex && valueMatches(pending.value, targetDouble)) {
                pendingDuplicate = true;
                break;
            }
        }
        if (pendingDuplicate) {
            continue;
        }

        PendingWriteCommand pending;
        pending.cmdId = graphCmdId(command.outputIndex, nowMs);
        pending.index = command.outputIndex;
        pending.value = targetDouble;
        pending.source = "graph-ems";
        pending.ts = nowMs;
        const auto routed = router_.submitWriteCommand(pending);
        if (routed.accepted) {
            ++result.deviceWrites;
            submitted = true;
        }
    }
    return submitted;
}

}  // namespace edge_gateway
