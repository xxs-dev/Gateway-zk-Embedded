#include "edge_gateway/graph_ems_engine.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
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

const JsonValue* findValue(const JsonObject& object, const char* key) {
    for (const auto& entry : object.values) {
        if (entry.key == key) {
            return entry.value.get();
        }
    }
    return nullptr;
}

int localHourFromEpochMs(std::int64_t nowMs) {
    std::time_t seconds = static_cast<std::time_t>(nowMs / 1000LL);
    std::tm localTime{};
#if defined(_WIN32)
    localtime_s(&localTime, &seconds);
#else
    localtime_r(&seconds, &localTime);
#endif
    if (localTime.tm_hour < 0 || localTime.tm_hour > 23) {
        return 0;
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

    return params;
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
        "timedChargeDischarge",
        "photovoltaicCharge",
        "phaseBalance",
        "skOverride",
        "reserveCapacity",
        "pcsPowerSolve",
        "pcsWriteback",
        "formula",
        "switch"
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
    const auto orderedNodes = executionOrder(config_);
    for (const auto* nodePtr : orderedNodes) {
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
    return result;
}

Optional<double> GraphEmsEngine::latestValue(std::uint32_t index, std::int64_t nowMs) const {
    const auto latest = router_.getLatestByIndex(index, nowMs);
    if (!latest || latest->quality != 1 || latest->stale) {
        return NullOpt;
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
    return router_.putLatestByIndex(point);
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
    if (pointsValue == nullptr || pointsValue->isNull()) {
        return;
    }
    const auto allowedIndexes = stateOutputIndexes();
    const std::set<std::uint32_t> allowed(allowedIndexes.begin(), allowedIndexes.end());
    for (const auto& item : pointsValue->asArray().values) {
        const auto& pointObject = item->asObject();
        const auto index = uint32Value(pointObject, "index");
        const auto* value = findValue(pointObject, "value");
        if (index == 0 || allowed.find(index) == allowed.end() || value == nullptr || !value->isNumber()) {
            continue;
        }
        set(index, value->asNumber(), nowMs);
    }
}

void GraphEmsEngine::saveState(std::int64_t nowMs) const {
    if (stateFile_.empty()) {
        return;
    }
    const auto indexes = stateOutputIndexes();
    std::vector<StoredPointValue> points;
    points.reserve(indexes.size());
    for (const auto index : indexes) {
        const auto latest = router_.getLatestByIndex(index, nowMs);
        if (latest && latest->quality == 1 && !latest->stale) {
            points.push_back(*latest);
        }
    }
    makeDirectory(directoryOf(stateFile_));
    std::ofstream output(stateFile_.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("failed to open graph EMS state file: " + stateFile_);
    }
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
    output << "  ]\n";
    output << "}\n";
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
