#include "edge_gateway/scada_project_loader.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace edge_gateway {

namespace {

constexpr std::uintmax_t kMaxJsonBytes = 16U * 1024U * 1024U;

std::string joinPath(const std::string& base, const std::string& child) {
    if (base.empty()) return child;
    const auto last = base.back();
    return last == '/' || last == '\\' ? base + child : base + "/" + child;
}

bool isDirectory(const std::string& path) {
#ifdef _WIN32
    struct _stat info {};
    return _stat(path.c_str(), &info) == 0 && (info.st_mode & _S_IFDIR) != 0;
#else
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
#endif
}

bool isRegularFile(const std::string& path) {
#ifdef _WIN32
    struct _stat info {};
    return _stat(path.c_str(), &info) == 0 && (info.st_mode & _S_IFREG) != 0;
#else
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
#endif
}

std::string absolutePath(const std::string& path) {
#ifdef _WIN32
    char buffer[_MAX_PATH] = {};
    return _fullpath(buffer, path.c_str(), sizeof(buffer)) == nullptr ? path : std::string(buffer);
#else
    char buffer[PATH_MAX] = {};
    return realpath(path.c_str(), buffer) == nullptr ? path : std::string(buffer);
#endif
}

bool endsWithJson(const std::string& value) {
    return value.size() >= 5 && value.compare(value.size() - 5, 5, ".json") == 0;
}

std::vector<std::string> listJsonFiles(const std::string& directory) {
    std::vector<std::string> result;
#ifdef _WIN32
    struct _finddata_t entry {};
    const auto handle = _findfirst(joinPath(directory, "*").c_str(), &entry);
    if (handle == -1) return result;
    do {
        const std::string name(entry.name);
        if ((entry.attrib & _A_SUBDIR) == 0 && endsWithJson(name)) {
            result.push_back(joinPath(directory, name));
        }
    } while (_findnext(handle, &entry) == 0);
    _findclose(handle);
#else
    auto* handle = opendir(directory.c_str());
    if (handle == nullptr) return result;
    while (auto* entry = readdir(handle)) {
        const std::string name(entry->d_name);
        if (!endsWithJson(name)) continue;
        const auto path = joinPath(directory, name);
        struct stat info {};
        if (stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode)) {
            result.push_back(path);
        }
    }
    closedir(handle);
#endif
    std::sort(result.begin(), result.end());
    return result;
}

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Object, Array };

    Type type = Type::Null;
    bool boolValue = false;
    double numberValue = 0.0;
    std::string stringValue;
    std::vector<std::pair<std::string, JsonValue>> objectValue;
    std::vector<JsonValue> arrayValue;

    const JsonValue* find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& item : objectValue) {
            if (item.first == key) return &item.second;
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : text_(text) {
    }

    JsonValue parse() {
        skipWhitespace();
        auto value = parseValue();
        skipWhitespace();
        if (position_ != text_.size()) fail("unexpected trailing characters");
        return value;
    }

private:
    [[noreturn]] void fail(const std::string& message) const {
        throw std::runtime_error("SCADA json parse failed at byte " +
            std::to_string(position_) + ": " + message);
    }

    JsonValue parseValue() {
        skipWhitespace();
        if (position_ >= text_.size()) fail("unexpected end of input");
        const auto ch = text_[position_];
        if (ch == '{') return parseObject();
        if (ch == '[') return parseArray();
        if (ch == '"') {
            JsonValue value;
            value.type = JsonValue::Type::String;
            value.stringValue = parseString();
            return value;
        }
        if (ch == 't') return parseLiteral("true", JsonValue::Type::Bool, true);
        if (ch == 'f') return parseLiteral("false", JsonValue::Type::Bool, false);
        if (ch == 'n') return parseLiteral("null", JsonValue::Type::Null, false);
        if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch)) != 0) return parseNumber();
        fail("invalid value");
    }

    JsonValue parseObject() {
        expect('{');
        JsonValue value;
        value.type = JsonValue::Type::Object;
        skipWhitespace();
        if (consume('}')) return value;
        while (true) {
            skipWhitespace();
            if (position_ >= text_.size() || text_[position_] != '"') fail("object key expected");
            auto key = parseString();
            skipWhitespace();
            expect(':');
            value.objectValue.emplace_back(std::move(key), parseValue());
            skipWhitespace();
            if (consume('}')) break;
            expect(',');
        }
        return value;
    }

    JsonValue parseArray() {
        expect('[');
        JsonValue value;
        value.type = JsonValue::Type::Array;
        skipWhitespace();
        if (consume(']')) return value;
        while (true) {
            value.arrayValue.push_back(parseValue());
            skipWhitespace();
            if (consume(']')) break;
            expect(',');
        }
        return value;
    }

    JsonValue parseLiteral(const char* literal, JsonValue::Type type, bool boolValue) {
        const std::string token(literal);
        if (text_.compare(position_, token.size(), token) != 0) fail("invalid literal");
        position_ += token.size();
        JsonValue value;
        value.type = type;
        value.boolValue = boolValue;
        return value;
    }

    JsonValue parseNumber() {
        const char* start = text_.c_str() + position_;
        char* end = nullptr;
        errno = 0;
        const auto number = std::strtod(start, &end);
        if (end == start || errno == ERANGE) fail("invalid number");
        position_ += static_cast<std::size_t>(end - start);
        JsonValue value;
        value.type = JsonValue::Type::Number;
        value.numberValue = number;
        return value;
    }

    std::string parseString() {
        expect('"');
        std::string result;
        while (position_ < text_.size()) {
            const auto ch = text_[position_++];
            if (ch == '"') return result;
            if (ch != '\\') {
                if (static_cast<unsigned char>(ch) < 0x20) fail("control character in string");
                result.push_back(ch);
                continue;
            }
            if (position_ >= text_.size()) fail("unterminated escape");
            const auto escaped = text_[position_++];
            switch (escaped) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': appendUnicode(result, parseHex4()); break;
                default: fail("unsupported escape");
            }
        }
        fail("unterminated string");
    }

    std::uint32_t parseHex4() {
        if (position_ + 4 > text_.size()) fail("short unicode escape");
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            value <<= 4U;
            const auto ch = text_[position_++];
            if (ch >= '0' && ch <= '9') value += static_cast<std::uint32_t>(ch - '0');
            else if (ch >= 'a' && ch <= 'f') value += static_cast<std::uint32_t>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') value += static_cast<std::uint32_t>(ch - 'A' + 10);
            else fail("invalid unicode escape");
        }
        if (value >= 0xD800U && value <= 0xDBFFU) {
            if (position_ + 6 > text_.size() || text_[position_] != '\\' || text_[position_ + 1] != 'u') {
                fail("missing low surrogate");
            }
            position_ += 2;
            const auto low = parseHex4();
            if (low < 0xDC00U || low > 0xDFFFU) fail("invalid low surrogate");
            return 0x10000U + ((value - 0xD800U) << 10U) + (low - 0xDC00U);
        }
        if (value >= 0xDC00U && value <= 0xDFFFU) fail("unexpected low surrogate");
        return value;
    }

    static void appendUnicode(std::string& output, std::uint32_t codepoint) {
        if (codepoint <= 0x7FU) output.push_back(static_cast<char>(codepoint));
        else if (codepoint <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else if (codepoint <= 0xFFFFU) {
            output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        }
    }

    void skipWhitespace() {
        while (position_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[position_])) != 0) {
            ++position_;
        }
    }

    bool consume(char expected) {
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect(char expected) {
        if (!consume(expected)) fail(std::string("expected '") + expected + "'");
    }

    const std::string& text_;
    std::size_t position_ = 0;
};

std::string readTextFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open SCADA file: " + path);
    const auto end = input.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end) > kMaxJsonBytes) {
        throw std::runtime_error("SCADA json exceeds size limit: " + path);
    }
    input.seekg(0, std::ios::beg);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

JsonValue readJson(const std::string& path) {
    return JsonParser(readTextFile(path)).parse();
}

const JsonValue& requireType(const JsonValue* value, JsonValue::Type type, const std::string& name) {
    if (value == nullptr || value->type != type) throw std::runtime_error("SCADA field has invalid type: " + name);
    return *value;
}

std::string stringValue(const JsonValue& object, const char* key, const std::string& fallback = {}) {
    const auto* value = object.find(key);
    if (value == nullptr || value->type == JsonValue::Type::Null) return fallback;
    return requireType(value, JsonValue::Type::String, key).stringValue;
}

bool boolValue(const JsonValue& object, const char* key, bool fallback = false) {
    const auto* value = object.find(key);
    if (value == nullptr || value->type == JsonValue::Type::Null) return fallback;
    return requireType(value, JsonValue::Type::Bool, key).boolValue;
}

double numberValue(const JsonValue& object, const char* key, double fallback = 0.0) {
    const auto* value = object.find(key);
    if (value == nullptr || value->type == JsonValue::Type::Null) return fallback;
    return requireType(value, JsonValue::Type::Number, key).numberValue;
}

const std::vector<JsonValue>& arrayValue(const JsonValue& object, const char* key) {
    return requireType(object.find(key), JsonValue::Type::Array, key).arrayValue;
}

std::vector<std::string> stringArray(const JsonValue& object, const char* key) {
    std::vector<std::string> result;
    const auto* value = object.find(key);
    if (value == nullptr || value->type == JsonValue::Type::Null) return result;
    for (const auto& item : requireType(value, JsonValue::Type::Array, key).arrayValue) {
        result.push_back(requireType(&item, JsonValue::Type::String, key).stringValue);
    }
    return result;
}

std::string scalarText(const JsonValue& value) {
    switch (value.type) {
        case JsonValue::Type::String:
            return value.stringValue;
        case JsonValue::Type::Bool:
            return value.boolValue ? "true" : "false";
        case JsonValue::Type::Number: {
            std::ostringstream output;
            output.precision(15);
            output << value.numberValue;
            return output.str();
        }
        default:
            return {};
    }
}

ScadaManifest parseManifest(const JsonValue& root) {
    requireType(&root, JsonValue::Type::Object, "manifest");
    ScadaManifest result;
    result.schemaVersion = stringValue(root, "schemaVersion", result.schemaVersion);
    result.projectId = stringValue(root, "projectId");
    result.projectName = stringValue(root, "projectName");
    result.packageVersion = stringValue(root, "packageVersion", result.packageVersion);
    result.entryScreen = stringValue(root, "entryScreen");
    result.packageRole = stringValue(root, "packageRole", result.packageRole);
    return result;
}

ScadaTopology parseTopology(const JsonValue& root) {
    requireType(&root, JsonValue::Type::Object, "topology");
    ScadaTopology result;
    const auto mode = stringValue(root, "mode", "integrated");
    result.mode = mode == "upperComputer" ? ScadaDeploymentMode::UpperComputer : ScadaDeploymentMode::Integrated;
    result.scadaHost = stringValue(root, "scadaHost", result.scadaHost);
    result.emsHost = stringValue(root, "emsHost", result.emsHost);
    result.dataTransport = stringValue(root, "dataTransport", result.dataTransport);
    result.offlinePolicy = stringValue(root, "offlinePolicy", result.offlinePolicy);
    if (const auto* policy = root.find("upperComputerOfflinePolicy")) {
        requireType(policy, JsonValue::Type::Object, "upperComputerOfflinePolicy");
        result.retainLocalSafetyRules = boolValue(*policy, "retainLocalSafetyRules", true);
        result.requireFreshLeaseForControl = boolValue(*policy, "requireFreshLeaseForControl", true);
        result.offlineTimeoutMs = static_cast<int>(numberValue(*policy, "timeoutMs", result.offlineTimeoutMs));
        result.offlineAction = stringValue(*policy, "action", result.offlineAction);
        if (const auto* actions = policy->find("safetyActions")) {
            for (const auto& item : requireType(actions, JsonValue::Type::Array, "safetyActions").arrayValue) {
                requireType(&item, JsonValue::Type::Object, "safety action");
                ScadaOfflineSafetyAction action;
                action.actionId = stringValue(item, "actionId");
                action.nodeId = stringValue(item, "nodeId");
                action.tagId = stringValue(item, "tagId");
                action.value = numberValue(item, "value", 0.0);
                action.highPriority = boolValue(item, "highPriority", true);
                result.safetyActions.push_back(std::move(action));
            }
        }
    }
    return result;
}

std::vector<ScadaNode> parseNodes(const JsonValue& root) {
    requireType(&root, JsonValue::Type::Array, "nodes");
    std::vector<ScadaNode> result;
    for (const auto& item : root.arrayValue) {
        requireType(&item, JsonValue::Type::Object, "node");
        ScadaNode node;
        node.nodeId = stringValue(item, "nodeId");
        node.machineCode = stringValue(item, "machineCode");
        node.displayName = stringValue(item, "displayName");
        node.connectionProfileId = stringValue(item, "connectionProfileId");
        node.roles = stringArray(item, "roles");
        result.push_back(std::move(node));
    }
    return result;
}

ScadaTagAccess parseAccess(const std::string& value) {
    if (value == "write") return ScadaTagAccess::Write;
    if (value == "readWrite") return ScadaTagAccess::ReadWrite;
    return ScadaTagAccess::Read;
}

std::vector<ScadaTag> parseTags(const JsonValue& root) {
    requireType(&root, JsonValue::Type::Array, "tags");
    std::vector<ScadaTag> result;
    for (const auto& item : root.arrayValue) {
        requireType(&item, JsonValue::Type::Object, "tag");
        ScadaTag tag;
        tag.tagId = stringValue(item, "tagId");
        tag.nodeId = stringValue(item, "nodeId");
        tag.deviceId = stringValue(item, "deviceId");
        tag.meterCode = stringValue(item, "meterCode");
        tag.pointCode = stringValue(item, "pointCode");
        tag.semanticRole = stringValue(item, "semanticRole");
        tag.displayName = stringValue(item, "displayName");
        tag.unit = stringValue(item, "unit");
        tag.dataType = stringValue(item, "dataType", tag.dataType);
        tag.access = parseAccess(stringValue(item, "access", "read"));
        tag.indexFallback = static_cast<std::uint32_t>(numberValue(item, "indexFallback", 0));
        result.push_back(std::move(tag));
    }
    return result;
}

std::vector<ScadaRuntimeMapping> parseMappings(const JsonValue& root) {
    requireType(&root, JsonValue::Type::Array, "runtime-map");
    std::vector<ScadaRuntimeMapping> result;
    for (const auto& item : root.arrayValue) {
        requireType(&item, JsonValue::Type::Object, "runtime mapping");
        ScadaRuntimeMapping mapping;
        mapping.nodeId = stringValue(item, "nodeId");
        mapping.tagId = stringValue(item, "tagId");
        mapping.sharedMemoryName = stringValue(item, "sharedMemoryName");
        mapping.index = static_cast<std::uint32_t>(numberValue(item, "index", 0));
        mapping.writable = boolValue(item, "writable", false);
        mapping.dataType = stringValue(item, "dataType", mapping.dataType);
        mapping.unit = stringValue(item, "unit");
        result.push_back(std::move(mapping));
    }
    return result;
}

ScadaTagReference parseReference(const JsonValue& root) {
    ScadaTagReference result;
    result.nodeId = stringValue(root, "nodeId");
    result.tagId = stringValue(root, "tagId");
    result.slot = stringValue(root, "slot", result.slot);
    return result;
}

ScadaStateCondition parseStateCondition(const JsonValue& root) {
    ScadaStateCondition result;
    result.nodeId = stringValue(root, "nodeId");
    result.tagId = stringValue(root, "tagId");
    result.comparison = stringValue(root, "comparison", result.comparison);
    result.value = stringValue(root, "value");
    return result;
}

ScadaStateRule parseStateRule(const JsonValue& root) {
    ScadaStateRule result;
    result.code = stringValue(root, "code");
    result.label = stringValue(root, "label");
    result.color = stringValue(root, "color", result.color);
    result.image = stringValue(root, "image");
    result.priority = static_cast<int>(numberValue(root, "priority", 0));
    result.match = stringValue(root, "match", result.match);
    if (const auto* conditions = root.find("conditions")) {
        for (const auto& item : requireType(conditions, JsonValue::Type::Array, "state rule conditions").arrayValue) {
            requireType(&item, JsonValue::Type::Object, "state rule condition");
            result.conditions.push_back(parseStateCondition(item));
        }
    }
    return result;
}

ScadaWidget parseWidget(const JsonValue& root) {
    ScadaWidget result;
    result.widgetId = stringValue(root, "widgetId");
    result.type = stringValue(root, "type", result.type);
    result.title = stringValue(root, "title");
    result.zIndex = static_cast<int>(numberValue(root, "zIndex", 0));
    result.visible = boolValue(root, "visible", true);
    if (const auto* geometry = root.find("geometry")) {
        requireType(geometry, JsonValue::Type::Object, "geometry");
        result.geometry.x = numberValue(*geometry, "x", 0);
        result.geometry.y = numberValue(*geometry, "y", 0);
        result.geometry.width = numberValue(*geometry, "width", result.geometry.width);
        result.geometry.height = numberValue(*geometry, "height", result.geometry.height);
    }
    if (const auto* bindings = root.find("bindings")) {
        for (const auto& item : requireType(bindings, JsonValue::Type::Array, "bindings").arrayValue) {
            requireType(&item, JsonValue::Type::Object, "binding");
            result.bindings.push_back(parseReference(item));
        }
    }
    if (const auto* stateRules = root.find("stateRules")) {
        for (const auto& item : requireType(stateRules, JsonValue::Type::Array, "state rules").arrayValue) {
            requireType(&item, JsonValue::Type::Object, "state rule");
            result.stateRules.push_back(parseStateRule(item));
        }
    }
    const auto* action = root.find("action");
    if (action != nullptr && action->type != JsonValue::Type::Null) {
        requireType(action, JsonValue::Type::Object, "action");
        result.action.type = stringValue(*action, "type", result.action.type);
        result.action.targetScreen = stringValue(*action, "targetScreen");
        result.action.nodeId = stringValue(*action, "nodeId");
        result.action.tagId = stringValue(*action, "tagId");
        result.action.value = stringValue(*action, "value");
        result.action.requiresConfirmation = boolValue(*action, "requiresConfirmation", true);
        result.action.highPriority = boolValue(*action, "highPriority", false);
    }
    const auto* properties = root.find("properties");
    if (properties != nullptr && properties->type != JsonValue::Type::Null) {
        requireType(properties, JsonValue::Type::Object, "properties");
        for (const auto& property : properties->objectValue) {
            const auto text = scalarText(property.second);
            if (!text.empty() || property.second.type == JsonValue::Type::String) {
                result.properties[property.first] = text;
            }
        }
    }
    return result;
}

std::vector<ScadaAlarm> parseAlarms(const JsonValue& root) {
    requireType(&root, JsonValue::Type::Array, "alarms");
    std::vector<ScadaAlarm> result;
    for (const auto& item : root.arrayValue) {
        requireType(&item, JsonValue::Type::Object, "alarm");
        ScadaAlarm alarm;
        alarm.alarmId = stringValue(item, "alarmId");
        alarm.nodeId = stringValue(item, "nodeId");
        alarm.tagId = stringValue(item, "tagId");
        alarm.severity = stringValue(item, "severity", alarm.severity);
        alarm.comparison = stringValue(item, "comparison", alarm.comparison);
        alarm.threshold = stringValue(item, "threshold");
        alarm.delayMs = static_cast<int>(numberValue(item, "delayMs", 0));
        alarm.deadband = numberValue(item, "deadband", 0.0);
        alarm.requiresAcknowledgement = boolValue(item, "requiresAcknowledgement", false);
        result.push_back(std::move(alarm));
    }
    return result;
}

std::vector<ScadaTrend> parseTrends(const JsonValue& root) {
    requireType(&root, JsonValue::Type::Array, "trends");
    std::vector<ScadaTrend> result;
    for (const auto& item : root.arrayValue) {
        requireType(&item, JsonValue::Type::Object, "trend");
        ScadaTrend trend;
        trend.trendId = stringValue(item, "trendId");
        trend.sampleIntervalMs = static_cast<int>(numberValue(item, "sampleIntervalMs", trend.sampleIntervalMs));
        trend.maxPoints = static_cast<int>(numberValue(item, "maxPoints", trend.maxPoints));
        if (const auto* series = item.find("series")) {
            for (const auto& reference : requireType(series, JsonValue::Type::Array, "trend series").arrayValue) {
                requireType(&reference, JsonValue::Type::Object, "trend series reference");
                trend.series.push_back(parseReference(reference));
            }
        }
        result.push_back(std::move(trend));
    }
    return result;
}

ScadaScreen parseScreen(const JsonValue& root) {
    requireType(&root, JsonValue::Type::Object, "screen");
    ScadaScreen result;
    result.screenId = stringValue(root, "screenId");
    result.title = stringValue(root, "title");
    result.width = static_cast<int>(numberValue(root, "width", result.width));
    result.height = static_cast<int>(numberValue(root, "height", result.height));
    result.background = stringValue(root, "background");
    if (const auto* widgets = root.find("widgets")) {
        for (const auto& item : requireType(widgets, JsonValue::Type::Array, "widgets").arrayValue) {
            requireType(&item, JsonValue::Type::Object, "widget");
            result.widgets.push_back(parseWidget(item));
        }
    }
    return result;
}

std::string tagKey(const std::string& nodeId, const std::string& tagId) {
    return nodeId + '\x1f' + tagId;
}

bool stableId(const std::string& value) {
    static const std::regex pattern("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$");
    return std::regex_match(value, pattern);
}

bool validComparison(const std::string& value) {
    return value == "eq" || value == "ne" || value == "gt" || value == "gte" ||
           value == "lt" || value == "lte" || value == "bitSet" || value == "bitClear" ||
           value == "maskAny" || value == "maskNone";
}

bool validComparisonValue(const std::string& value) {
    if (value == "true" || value == "false" || value == "on" || value == "off") return true;
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const auto parsed = std::strtod(value.c_str(), &end);
    return errno == 0 && end != value.c_str() && *end == '\0' && std::isfinite(parsed);
}

}  // namespace

ScadaProject ScadaProjectLoader::loadFromDirectory(const std::string& directory) {
    if (!isDirectory(directory)) {
        throw std::runtime_error("SCADA project directory does not exist: " + directory);
    }

    ScadaProject project;
    project.rootDirectory = absolutePath(directory);
    project.manifest = parseManifest(readJson(joinPath(directory, "manifest.json")));
    project.topology = parseTopology(readJson(joinPath(directory, "topology.json")));
    project.nodes = parseNodes(readJson(joinPath(directory, "nodes.json")));
    project.tags = parseTags(readJson(joinPath(directory, "tags.json")));
    project.runtimeMappings = parseMappings(readJson(joinPath(directory, "runtime-map.json")));

    const auto alarmsPath = joinPath(directory, "alarms.json");
    if (isRegularFile(alarmsPath)) {
        project.alarms = parseAlarms(readJson(alarmsPath));
    }
    const auto trendsPath = joinPath(directory, "trends.json");
    if (isRegularFile(trendsPath)) {
        project.trends = parseTrends(readJson(trendsPath));
    }

    const auto screensDirectory = joinPath(directory, "screens");
    if (isDirectory(screensDirectory)) {
        for (const auto& screenFile : listJsonFiles(screensDirectory)) {
            project.screens.push_back(parseScreen(readJson(screenFile)));
        }
    }

    validate(project);
    return project;
}

void ScadaProjectLoader::validate(const ScadaProject& project) {
    if (project.manifest.schemaVersion != "2.0") throw std::runtime_error("unsupported SCADA schema version");
    if (!stableId(project.manifest.projectId)) throw std::runtime_error("invalid SCADA projectId");
    if (!stableId(project.manifest.packageVersion)) throw std::runtime_error("invalid SCADA packageVersion");
    if (project.nodes.empty()) throw std::runtime_error("SCADA project has no edge node");
    if (project.topology.mode == ScadaDeploymentMode::Integrated &&
        project.manifest.packageRole != "edgeNode" && project.nodes.size() != 1) {
        throw std::runtime_error("integrated SCADA project must contain exactly one node");
    }
    if (project.topology.mode == ScadaDeploymentMode::UpperComputer &&
        !project.topology.retainLocalSafetyRules) {
        throw std::runtime_error("upper-computer SCADA must retain edge safety rules");
    }
    if (project.topology.mode == ScadaDeploymentMode::UpperComputer) {
        if (project.topology.offlineTimeoutMs < 1000 || project.topology.offlineTimeoutMs > 600000) {
            throw std::runtime_error("upper-computer SCADA offline timeout is out of range");
        }
        if (project.topology.offlineAction != "executeConfiguredActions") {
            throw std::runtime_error("upper-computer SCADA requires explicit offline safety actions");
        }
        if (project.topology.safetyActions.empty()) {
            throw std::runtime_error("upper-computer SCADA has no offline safety action");
        }
    }

    std::unordered_set<std::string> nodeIds;
    std::unordered_set<std::string> machineCodes;
    for (const auto& node : project.nodes) {
        if (!stableId(node.nodeId) || node.machineCode.empty()) throw std::runtime_error("invalid SCADA node identity");
        if (!nodeIds.insert(node.nodeId).second) throw std::runtime_error("duplicate SCADA nodeId: " + node.nodeId);
        if (!machineCodes.insert(node.machineCode).second) throw std::runtime_error("duplicate SCADA machineCode: " + node.machineCode);
    }

    std::unordered_map<std::string, const ScadaTag*> tags;
    std::unordered_set<std::string> semantics;
    for (const auto& tag : project.tags) {
        if (!stableId(tag.tagId) || nodeIds.count(tag.nodeId) == 0) throw std::runtime_error("invalid SCADA tag identity");
        const auto key = tagKey(tag.nodeId, tag.tagId);
        if (!tags.emplace(key, &tag).second) throw std::runtime_error("duplicate SCADA tag: " + key);
        if (!tag.semanticRole.empty()) {
            const auto semanticKey = tag.nodeId + '\x1f' + tag.deviceId + '\x1f' + tag.semanticRole;
            if (!semantics.insert(semanticKey).second) throw std::runtime_error("ambiguous SCADA semantic role: " + tag.semanticRole);
        }
    }

    std::unordered_set<std::string> mappedTags;
    std::unordered_set<std::string> routes;
    for (const auto& mapping : project.runtimeMappings) {
        const auto key = tagKey(mapping.nodeId, mapping.tagId);
        if (tags.count(key) == 0) throw std::runtime_error("SCADA mapping references unknown tag: " + key);
        if (!mappedTags.insert(key).second) throw std::runtime_error("duplicate SCADA tag mapping: " + key);
        if (mapping.index == 0 || mapping.sharedMemoryName.empty()) throw std::runtime_error("invalid SCADA runtime mapping");
        const auto route = mapping.nodeId + '\x1f' + mapping.sharedMemoryName + '\x1f' + std::to_string(mapping.index);
        if (!routes.insert(route).second) throw std::runtime_error("duplicate SCADA runtime route: " + route);
        const auto* tag = tags.at(key);
        if (mapping.writable && tag->access == ScadaTagAccess::Read) {
            throw std::runtime_error("read-only SCADA tag has writable runtime mapping: " + key);
        }
        if (!mapping.writable && tag->access != ScadaTagAccess::Read) {
            throw std::runtime_error("writable SCADA tag has read-only runtime mapping: " + key);
        }
    }
    for (const auto& item : tags) {
        if (mappedTags.count(item.first) == 0 && item.second->indexFallback == 0) {
            throw std::runtime_error("SCADA tag has no runtime mapping: " + item.first);
        }
    }

    if (project.topology.mode == ScadaDeploymentMode::UpperComputer) {
        std::unordered_set<std::string> actionIds;
        for (const auto& action : project.topology.safetyActions) {
            if (!stableId(action.actionId) || !actionIds.insert(action.actionId).second) {
                throw std::runtime_error("invalid or duplicate SCADA offline safety action: " + action.actionId);
            }
            const auto key = tagKey(action.nodeId, action.tagId);
            const auto tag = tags.find(key);
            if (tag == tags.end()) {
                throw std::runtime_error("SCADA offline safety action references unknown tag: " + key);
            }
            if (tag->second->access == ScadaTagAccess::Read) {
                throw std::runtime_error("SCADA offline safety action targets read-only tag: " + key);
            }
            const auto mapping = std::find_if(project.runtimeMappings.begin(), project.runtimeMappings.end(), [&](const ScadaRuntimeMapping& item) {
                return item.nodeId == action.nodeId && item.tagId == action.tagId;
            });
            if (mapping == project.runtimeMappings.end() || !mapping->writable) {
                throw std::runtime_error("SCADA offline safety action has no writable runtime mapping: " + key);
            }
            if (!std::isfinite(action.value)) {
                throw std::runtime_error("SCADA offline safety action value is not finite: " + action.actionId);
            }
        }
    }

    std::unordered_set<std::string> screenIds;
    for (const auto& screen : project.screens) {
        if (!stableId(screen.screenId) || screen.width <= 0 || screen.height <= 0) throw std::runtime_error("invalid SCADA screen");
        if (!screenIds.insert(screen.screenId).second) throw std::runtime_error("duplicate SCADA screen: " + screen.screenId);
        std::unordered_set<std::string> widgetIds;
        for (const auto& widget : screen.widgets) {
            if (!stableId(widget.widgetId) || widget.geometry.width <= 0 || widget.geometry.height <= 0) {
                throw std::runtime_error("invalid SCADA widget");
            }
            if (!widgetIds.insert(widget.widgetId).second) throw std::runtime_error("duplicate SCADA widget: " + widget.widgetId);
            for (const auto& binding : widget.bindings) {
                if (tags.count(tagKey(binding.nodeId, binding.tagId)) == 0) {
                    throw std::runtime_error("SCADA widget references unknown tag: " + binding.tagId);
                }
            }
            std::unordered_set<std::string> stateRuleCodes;
            for (const auto& rule : widget.stateRules) {
                if (!stableId(rule.code) || !stateRuleCodes.insert(rule.code).second) {
                    throw std::runtime_error("invalid or duplicate SCADA state rule code: " + rule.code);
                }
                if (rule.conditions.empty()) throw std::runtime_error("SCADA state rule has no condition");
                if (rule.match != "all" && rule.match != "any") throw std::runtime_error("invalid SCADA state rule match mode");
                for (const auto& condition : rule.conditions) {
                    if (tags.count(tagKey(condition.nodeId, condition.tagId)) == 0) {
                        throw std::runtime_error("SCADA state rule references unknown tag: " + condition.tagId);
                    }
                    if (!validComparison(condition.comparison) || !validComparisonValue(condition.value)) {
                        throw std::runtime_error("invalid SCADA state rule comparison: " + rule.code);
                    }
                }
            }
            const auto controlAction = widget.action.type == "writeSetpoint" ||
                widget.action.type == "pulse" || widget.action.type == "toggle";
            if (!widget.action.type.empty() && widget.action.type != "none" &&
                widget.action.type != "navigate" && !controlAction) {
                throw std::runtime_error("unsupported SCADA action type: " + widget.action.type);
            }
            if (controlAction) {
                const auto actionTag = tags.find(tagKey(widget.action.nodeId, widget.action.tagId));
                if (actionTag == tags.end()) throw std::runtime_error("SCADA action references unknown tag");
                if (actionTag->second->access == ScadaTagAccess::Read) throw std::runtime_error("SCADA action targets read-only tag");
                if (widget.action.type == "writeSetpoint" && !widget.action.value.empty() &&
                    !validComparisonValue(widget.action.value)) {
                    throw std::runtime_error("invalid SCADA setpoint value");
                }
            }
        }
    }
    if (!project.manifest.entryScreen.empty() && screenIds.count(project.manifest.entryScreen) == 0) {
        throw std::runtime_error("SCADA entry screen does not exist: " + project.manifest.entryScreen);
    }
    for (const auto& screen : project.screens) {
        for (const auto& widget : screen.widgets) {
            if (widget.action.type == "navigate" && screenIds.count(widget.action.targetScreen) == 0) {
                throw std::runtime_error("SCADA action target screen does not exist: " + widget.action.targetScreen);
            }
        }
    }

    std::unordered_set<std::string> alarmIds;
    for (const auto& alarm : project.alarms) {
        if (!stableId(alarm.alarmId) || !alarmIds.insert(alarm.alarmId).second) {
            throw std::runtime_error("invalid or duplicate SCADA alarmId: " + alarm.alarmId);
        }
        if (tags.count(tagKey(alarm.nodeId, alarm.tagId)) == 0) {
            throw std::runtime_error("SCADA alarm references unknown tag: " + alarm.tagId);
        }
        if (!validComparison(alarm.comparison) || !validComparisonValue(alarm.threshold)) {
            throw std::runtime_error("invalid SCADA alarm comparison: " + alarm.alarmId);
        }
        if (alarm.delayMs < 0 || alarm.deadband < 0.0) throw std::runtime_error("invalid SCADA alarm timing");
    }

    std::unordered_set<std::string> trendIds;
    for (const auto& trend : project.trends) {
        if (!stableId(trend.trendId) || !trendIds.insert(trend.trendId).second) {
            throw std::runtime_error("invalid or duplicate SCADA trendId: " + trend.trendId);
        }
        if (trend.sampleIntervalMs <= 0 || trend.maxPoints <= 1 || trend.series.empty()) {
            throw std::runtime_error("invalid SCADA trend limits");
        }
        for (const auto& series : trend.series) {
            if (tags.count(tagKey(series.nodeId, series.tagId)) == 0) {
                throw std::runtime_error("SCADA trend references unknown tag: " + series.tagId);
            }
        }
    }
}

}  // namespace edge_gateway
