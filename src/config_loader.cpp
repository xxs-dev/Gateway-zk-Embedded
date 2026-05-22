#include "edge_gateway/config_loader.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace edge_gateway {

namespace {

bool isAbsolutePath(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    if (path.front() == '/' || path.front() == '\\') {
        return true;
    }
    return path.size() > 1 && path[1] == ':';
}

std::string directoryOf(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

std::string joinPath(const std::string& base, const std::string& child) {
    if (base.empty()) {
        return child;
    }
    const char last = base.back();
    if (last == '/' || last == '\\') {
        return base + child;
    }
    return base + "/" + child;
}

bool fileExists(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    return input.good();
}

std::string resolveRelativeToConfig(const std::string& configPath, const std::string& value) {
    if (value.empty() || isAbsolutePath(value) || fileExists(value)) {
        return value;
    }

    const auto configDir = directoryOf(configPath);
    const auto configParent = directoryOf(configDir);
    const auto configGrandParent = directoryOf(configParent);

    const std::vector<std::string> candidates = {
        joinPath(configDir, value),
        joinPath(configParent, value),
        joinPath(configGrandParent, value)
    };
    for (const auto& candidate : candidates) {
        if (fileExists(candidate)) {
            return candidate;
        }
    }
    return value;
}

void resolveConfigRelativePaths(DeviceConfig& config, const std::string& configPath) {
    if (!config.protocol.standardPointsFile.empty()) {
        config.protocol.standardPointsFile = resolveRelativeToConfig(configPath, config.protocol.standardPointsFile);
    }
}

std::string discoverIdentityConfigFile(const std::string& configPath) {
    const char* envPath = std::getenv("GATEWAY_IDENTITY_CONFIG");
    if (envPath != nullptr && fileExists(envPath)) {
        return envPath;
    }

    const auto configDir = directoryOf(configPath);
    const auto configParent = directoryOf(configDir);
    const auto configGrandParent = directoryOf(configParent);
    const std::vector<std::string> candidates = {
        joinPath(configDir, "device_identity.json"),
        joinPath(configParent, "device_identity.json"),
        joinPath(configGrandParent, "runtime/device_identity.json"),
        "config/runtime/device_identity.json",
        "/opt/modbus-gateway/config/runtime/device_identity.json"
    };
    for (const auto& candidate : candidates) {
        if (fileExists(candidate)) {
            return candidate;
        }
    }
    return std::string();
}

void resolveAppConfigRelativePaths(AppConfig& config, const std::string& configPath) {
    if (!config.identityConfigFile.empty()) {
        config.identityConfigFile = resolveRelativeToConfig(configPath, config.identityConfigFile);
    } else {
        config.identityConfigFile = discoverIdentityConfigFile(configPath);
    }

    for (auto& file : config.deviceConfigFiles) {
        file = resolveRelativeToConfig(configPath, file);
    }

    for (auto& rule : config.computeEngine.rules) {
        if (!rule.script.legacyGlListFile.empty()) {
            rule.script.legacyGlListFile = resolveRelativeToConfig(configPath, rule.script.legacyGlListFile);
        }
        if (!rule.script.legacyVarListFile.empty()) {
            rule.script.legacyVarListFile = resolveRelativeToConfig(configPath, rule.script.legacyVarListFile);
        }
        if (!rule.script.graphFile.empty()) {
            rule.script.graphFile = resolveRelativeToConfig(configPath, rule.script.graphFile);
        }
        if (!rule.script.graphStateFile.empty()) {
            rule.script.graphStateFile = resolveRelativeToConfig(configPath, rule.script.graphStateFile);
        }
    }
}

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

    using Object = JsonObject;
    using Array = JsonArray;

    JsonValue() = default;

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

    static JsonValue makeObject(Object value) {
        JsonValue result;
        result.type_ = Type::Object;
        result.objectValue_.reset(new Object(std::move(value)));
        return result;
    }

    static JsonValue makeArray(Array value) {
        JsonValue result;
        result.type_ = Type::Array;
        result.arrayValue_.reset(new Array(std::move(value)));
        return result;
    }

    Type type() const {
        return type_;
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

    const Object& asObject() const {
        if (!isObject()) {
            throw std::runtime_error("json value is not object");
        }
        return *objectValue_;
    }

    const Array& asArray() const {
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
    std::shared_ptr<Object> objectValue_;
    std::shared_ptr<Array> arrayValue_;
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
        throw std::runtime_error(message + " at " + locationString());
    }

    std::string locationString() const {
        std::size_t line = 1;
        std::size_t column = 1;
        for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        return "line " + std::to_string(line) + ", column " + std::to_string(column);
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

        fail("invalid json value");
    }

    JsonValue parseObject() {
        expect('{');
        JsonValue::Object object;
        skipWhitespace();
        if (match('}')) {
            return JsonValue::makeObject(std::move(object));
        }

        while (true) {
            skipWhitespace();
            const auto key = parseString();
            skipWhitespace();
            expect(':');
            skipWhitespace();
            JsonMember member;
            member.key = key;
            member.value.reset(new JsonValue(parseValue()));
            object.values.push_back(std::move(member));
            skipWhitespace();
            if (match('}')) {
                break;
            }
            expect(',');
        }
        return JsonValue::makeObject(std::move(object));
    }

    JsonValue parseArray() {
        expect('[');
        JsonValue::Array array;
        skipWhitespace();
        if (match(']')) {
            return JsonValue::makeArray(std::move(array));
        }

        while (true) {
            skipWhitespace();
            array.values.push_back(std::make_shared<JsonValue>(parseValue()));
            skipWhitespace();
            if (match(']')) {
                break;
            }
            expect(',');
        }
        return JsonValue::makeArray(std::move(array));
    }

    std::string parseString() {
        expect('"');
        std::string result;
        while (!isEnd()) {
            const char ch = get();
            if (ch == '"') {
                return result;
            }
            if (ch == '\\') {
                if (isEnd()) {
                    fail("invalid json escape");
                }
                const char esc = get();
                switch (esc) {
                    case '"': result.push_back('"'); break;
                    case '\\': result.push_back('\\'); break;
                    case '/': result.push_back('/'); break;
                    case 'b': result.push_back('\b'); break;
                    case 'f': result.push_back('\f'); break;
                    case 'n': result.push_back('\n'); break;
                    case 'r': result.push_back('\r'); break;
                    case 't': result.push_back('\t'); break;
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
            if (!isEnd() && (peek() == '+' || peek() == '-')) {
                ++pos_;
            }
            while (!isEnd() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                ++pos_;
            }
        }
        return std::strtod(text_.c_str() + start, nullptr);
    }

    void consumeLiteral(const char* literal) {
        while (*literal != '\0') {
            if (isEnd() || get() != *literal) {
                fail("invalid json literal");
            }
            ++literal;
        }
    }

    void skipWhitespace() {
        while (!isEnd() && std::isspace(static_cast<unsigned char>(text_[pos_])) != 0) {
            ++pos_;
        }
    }

    bool match(char expected) {
        if (!isEnd() && peek() == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    void expect(char expected) {
        if (isEnd() || get() != expected) {
            fail(std::string("unexpected json token, expected '") + expected + "'");
        }
    }

    char peek() const {
        return text_[pos_];
    }

    char get() {
        return text_[pos_++];
    }

    bool isEnd() const {
        return pos_ >= text_.size();
    }

    const std::string& text_;
    std::size_t pos_ = 0;
};

const JsonValue* findValue(const JsonValue::Object& object, const char* key) {
    for (const auto& entry : object.values) {
        if (entry.key == key) {
            return entry.value.get();
        }
    }
    return nullptr;
}

std::string requireString(const JsonValue::Object& object, const char* key, const std::string& defaultValue = "") {
    const auto* value = findValue(object, key);
    if (value == nullptr || value->isNull()) {
        return defaultValue;
    }
    return value->asString();
}

bool requireBool(const JsonValue::Object& object, const char* key, bool defaultValue = false) {
    const auto* value = findValue(object, key);
    if (value == nullptr || value->isNull()) {
        return defaultValue;
    }
    return value->asBool();
}

int requireInt(const JsonValue::Object& object, const char* key, int defaultValue = 0) {
    const auto* value = findValue(object, key);
    if (value == nullptr || value->isNull()) {
        return defaultValue;
    }
    return static_cast<int>(value->asNumber());
}

std::size_t requireSize(const JsonValue::Object& object, const char* key, std::size_t defaultValue = 0) {
    const auto* value = findValue(object, key);
    if (value == nullptr || value->isNull()) {
        return defaultValue;
    }
    return static_cast<std::size_t>(value->asNumber());
}

std::int64_t requireInt64(const JsonValue::Object& object, const char* key, std::int64_t defaultValue = 0) {
    const auto* value = findValue(object, key);
    if (value == nullptr || value->isNull()) {
        return defaultValue;
    }
    return static_cast<std::int64_t>(value->asNumber());
}

double requireDouble(const JsonValue::Object& object, const char* key, double defaultValue = 0.0) {
    const auto* value = findValue(object, key);
    if (value == nullptr || value->isNull()) {
        return defaultValue;
    }
    return value->asNumber();
}

std::vector<std::string> parseStringArray(const JsonValue* value) {
    std::vector<std::string> result;
    if (value == nullptr || value->isNull()) {
        return result;
    }
    for (const auto& item : value->asArray().values) {
        result.push_back(item->asString());
    }
    return result;
}

std::vector<double> parseDoubleArray(const JsonValue* value) {
    std::vector<double> result;
    if (value == nullptr || value->isNull()) {
        return result;
    }
    for (const auto& item : value->asArray().values) {
        result.push_back(item->asNumber());
    }
    return result;
}

std::unordered_map<std::string, std::string> parseStringMap(const JsonValue* value) {
    std::unordered_map<std::string, std::string> result;
    if (value == nullptr || value->isNull()) {
        return result;
    }
    for (const auto& entry : value->asObject().values) {
        result[entry.key] = entry.value->asString();
    }
    return result;
}

CachePolicy parseCachePolicy(const JsonValue* value) {
    CachePolicy policy;
    if (value == nullptr || value->isNull()) {
        return policy;
    }
    const auto& object = value->asObject();
    policy.storeLatest = requireBool(object, "storeLatest", policy.storeLatest);
    policy.storeHistory = requireBool(object, "storeHistory", policy.storeHistory);
    policy.historySize = requireSize(object, "historySize", policy.historySize);
    policy.ttlMs = requireInt64(object, "ttlMs", policy.ttlMs);
    return policy;
}

CanSignalSpec parseCanSignalSpec(const JsonValue* value) {
    CanSignalSpec spec;
    if (value == nullptr || value->isNull()) {
        return spec;
    }
    const auto& object = value->asObject();
    spec.frameId = requireString(object, "frameId", spec.frameId);
    spec.extended = requireBool(object, "extended", spec.extended);
    spec.dlc = requireInt(object, "dlc", spec.dlc);
    spec.byteOffset = requireInt(object, "byteOffset", spec.byteOffset);
    spec.bitOffset = requireInt(object, "bitOffset", spec.bitOffset);
    spec.bitLength = requireInt(object, "bitLength", spec.bitLength);
    spec.bitOrder = requireString(object, "bitOrder", spec.bitOrder);
    spec.endian = requireString(object, "endian", spec.endian);
    spec.receiveTimeoutMs = requireInt(object, "receiveTimeoutMs", spec.receiveTimeoutMs);
    spec.remoteRequest = requireBool(object, "remoteRequest", spec.remoteRequest);
    return spec;
}

ReadSpec parseReadSpec(const JsonValue* value) {
    ReadSpec spec;
    if (value == nullptr || value->isNull()) {
        return spec;
    }
    const auto& object = value->asObject();
    spec.enable = requireBool(object, "enable", spec.enable);
    spec.function = requireInt(object, "function", spec.function);
    spec.length = requireInt(object, "length", spec.length);
    spec.dataType = requireString(object, "dataType", spec.dataType);
    spec.scale = requireDouble(object, "scale", spec.scale);
    spec.offset = requireDouble(object, "offset", spec.offset);
    spec.byteOrder = requireString(object, "byteOrder", spec.byteOrder);
    spec.signedFlag = requireBool(object, "signed", spec.signedFlag);
    spec.unit = requireString(object, "unit", spec.unit);
    spec.intervalMs = requireInt(object, "intervalMs", spec.intervalMs);
    spec.bit = requireInt(object, "bit", spec.bit);
    spec.gpio = requireInt(object, "gpio", spec.gpio);
    spec.activeHigh = requireBool(object, "activeHigh", spec.activeHigh);
    spec.debounceMs = requireInt(object, "debounceMs", spec.debounceMs);
    if (const auto* dlt645 = value->find("dlt645")) {
        const auto& dlt645Object = dlt645->asObject();
        spec.dlt645Di = requireString(dlt645Object, "di", spec.dlt645Di);
        spec.dlt645ByteCount = requireInt(dlt645Object, "byteCount", spec.dlt645ByteCount);
        spec.dlt645Decoder = requireString(dlt645Object, "decoder", spec.dlt645Decoder);
        if (spec.dlt645ByteCount > 0 && spec.length <= 1) {
            spec.length = spec.dlt645ByteCount;
        }
    }
    spec.can = parseCanSignalSpec(value->find("can"));
    spec.cachePolicy = parseCachePolicy(value->find("cachePolicy"));
    return spec;
}

WriteSpec parseWriteSpec(const JsonValue* value) {
    WriteSpec spec;
    if (value == nullptr || value->isNull()) {
        return spec;
    }
    const auto& object = value->asObject();
    spec.enable = requireBool(object, "enable", spec.enable);
    spec.address = requireInt(object, "address", spec.address);
    spec.function = requireInt(object, "function", spec.function);
    spec.length = requireInt(object, "length", spec.length);
    spec.dataType = requireString(object, "dataType", spec.dataType);
    spec.scale = requireDouble(object, "scale", spec.scale);
    spec.offset = requireDouble(object, "offset", spec.offset);
    spec.byteOrder = requireString(object, "byteOrder", spec.byteOrder);
    if (const auto* minValue = value->find("min")) {
        if (!minValue->isNull()) {
            spec.minValue = minValue->asNumber();
        }
    }
    if (const auto* maxValue = value->find("max")) {
        if (!maxValue->isNull()) {
            spec.maxValue = maxValue->asNumber();
        }
    }
    spec.step = requireDouble(object, "step", spec.step);
    spec.allowedValues = parseDoubleArray(value->find("allowedValues"));
    spec.verifyAfterWrite = requireBool(object, "verifyAfterWrite", spec.verifyAfterWrite);
    spec.verifyDelayMs = requireInt(object, "verifyDelayMs", spec.verifyDelayMs);
    spec.verifyByRead = requireBool(object, "verifyByRead", spec.verifyByRead);
    spec.can = parseCanSignalSpec(value->find("can"));
    return spec;
}

AlarmRuleConfig parseAlarmRule(const JsonValue& value) {
    AlarmRuleConfig rule;
    const auto& object = value.asObject();
    rule.type = requireString(object, "type");
    rule.threshold = requireDouble(object, "threshold", rule.threshold);
    rule.reportRecovery = requireBool(object, "reportRecovery", rule.reportRecovery);
    rule.persistValue = requireString(object, "persistValue", rule.persistValue);
    return rule;
}

PointDefinition parsePointDefinition(const JsonValue& value) {
    PointDefinition point;
    const auto& object = value.asObject();
    point.index = static_cast<std::uint32_t>(requireInt(object, "index", 0));
    point.pointCode = requireString(object, "pointCode");
    point.name = requireString(object, "name");
    point.desc = requireString(object, "desc");
    point.category = requireString(object, "category");
    point.address = requireInt(object, "address", point.address);
    point.enabled = requireBool(object, "enabled", point.enabled);
    point.isStore = requireBool(object, "isStore", point.isStore);
    point.fullUpload = requireBool(object, "fullUpload", point.fullUpload);
    point.reportOnChange = requireBool(object, "reportOnChange", point.reportOnChange);
    point.persistIntervalSec = requireInt(object, "persistIntervalSec", point.persistIntervalSec);
    point.tags = parseStringArray(value.find("tags"));
    point.read = parseReadSpec(value.find("read"));
    point.write = parseWriteSpec(value.find("write"));
    if (const auto* alarms = value.find("alarms")) {
        for (const auto& item : alarms->asArray().values) {
            point.alarms.push_back(parseAlarmRule(*item));
        }
    }
    point.valueMap = parseStringMap(value.find("valueMap"));
    return point;
}

LogicalDeviceConfig parseLogicalDevice(const JsonValue& value, int defaultSlave) {
    LogicalDeviceConfig device;
    const auto& object = value.asObject();
    device.meterCode = requireString(object, "meterCode");
    device.deviceName = requireString(object, "deviceName");
    device.enabled = requireBool(object, "enabled", device.enabled);
    device.slave = requireInt(object, "slave", defaultSlave);
    device.address = requireString(object, "address", device.address);
    device.onlineTimeoutMs = requireInt(object, "onlineTimeoutMs", device.onlineTimeoutMs);
    device.onlineFrameIds = parseStringArray(value.find("onlineFrameIds"));
    if (const auto* points = value.find("points")) {
        for (const auto& item : points->asArray().values) {
            device.points.push_back(parsePointDefinition(*item));
        }
    }
    return device;
}

SerialTransportConfig parseTransport(const JsonValue* value) {
    SerialTransportConfig transport;
    if (value == nullptr || value->isNull()) {
        return transport;
    }
    const auto& object = value->asObject();
    transport.serialPort = requireString(object, "serialPort", transport.serialPort);
    transport.baudRate = requireInt(object, "baudRate", transport.baudRate);
    transport.dataBits = requireInt(object, "dataBits", transport.dataBits);
    transport.stopBits = requireInt(object, "stopBits", transport.stopBits);
    transport.parity = requireString(object, "parity", transport.parity);
    transport.timeoutMs = requireInt(object, "timeoutMs", transport.timeoutMs);
    return transport;
}

TcpTransportConfig parseTcpTransport(const JsonValue* value) {
    TcpTransportConfig transport;
    if (value == nullptr || value->isNull()) {
        return transport;
    }
    const auto& object = value->asObject();
    transport.host = requireString(object, "host", transport.host);
    transport.port = requireInt(object, "port", transport.port);
    transport.connectTimeoutMs = requireInt(object, "connectTimeoutMs", transport.connectTimeoutMs);
    transport.timeoutMs = requireInt(object, "timeoutMs", transport.timeoutMs);
    return transport;
}

CanProtocolConfig parseCanProtocol(const JsonValue* value) {
    CanProtocolConfig can;
    if (value == nullptr || value->isNull()) {
        return can;
    }
    const auto& object = value->asObject();
    can.interfaceName = requireString(object, "interfaceName", can.interfaceName);
    can.interfaceCode = requireString(object, "interfaceCode", can.interfaceCode);
    can.bitrate = requireInt(object, "bitrate", can.bitrate);
    can.samplePoint = requireDouble(object, "samplePoint", can.samplePoint);
    can.restartMs = requireInt(object, "restartMs", can.restartMs);
    can.listenOnly = requireBool(object, "listenOnly", can.listenOnly);
    can.loopback = requireBool(object, "loopback", can.loopback);
    can.fdEnabled = requireBool(object, "fdEnabled", can.fdEnabled);
    can.dataBitrate = requireInt(object, "dataBitrate", can.dataBitrate);
    can.manageInterface = requireBool(object, "manageInterface", can.manageInterface);
    can.rxQueueSize = requireSize(object, "rxQueueSize", can.rxQueueSize);
    can.txQueueSize = requireSize(object, "txQueueSize", can.txQueueSize);
    return can;
}

ProtocolConfig parseProtocol(const JsonValue* value) {
    ProtocolConfig protocol;
    if (value == nullptr || value->isNull()) {
        return protocol;
    }
    const auto& object = value->asObject();
    protocol.type = requireString(object, "type", protocol.type);
    protocol.slave = requireInt(object, "slave", protocol.slave);
    protocol.backend = requireString(object, "backend", protocol.backend);
    protocol.gpioBasePath = requireString(object, "gpioBasePath", protocol.gpioBasePath);
    protocol.transport = parseTransport(value->find("transport"));
    protocol.tcp = parseTcpTransport(value->find("tcp"));
    protocol.can = parseCanProtocol(value->find("can"));
    return protocol;
}

void parseDlt645Config(const JsonValue* value, ProtocolConfig& protocol) {
    if (value == nullptr || value->isNull()) {
        return;
    }
    const auto& object = value->asObject();
    protocol.standardPointsFile = requireString(object, "standardPointsFile", protocol.standardPointsFile);
    protocol.standardPointsVersion = requireString(object, "standardPointsVersion", protocol.standardPointsVersion);
}

CollectConfig parseCollect(const JsonValue* value) {
    CollectConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.defaultIntervalMs = requireInt(object, "defaultIntervalMs", config.defaultIntervalMs);
    config.batchOptimize = requireBool(object, "batchOptimize", config.batchOptimize);
    config.maxBatchRegisters = requireInt(object, "maxBatchRegisters", config.maxBatchRegisters);
    config.writebackIntervalMs = requireInt(object, "writebackIntervalMs", config.writebackIntervalMs);
    config.interfaceCheckIntervalMs = requireInt(object, "interfaceCheckIntervalMs", config.interfaceCheckIntervalMs);
    return config;
}

MemoryStoreConfig parseMemoryStore(const JsonValue* value) {
    MemoryStoreConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.enabled = requireBool(object, "enabled", config.enabled);
    config.backend = requireString(object, "backend", config.backend);
    config.keepHistory = requireSize(object, "keepHistory", config.keepHistory);
    config.defaultTtlMs = requireInt64(object, "defaultTtlMs", config.defaultTtlMs);
    config.indexBy = parseStringArray(value->find("indexBy"));
    config.sharedMemoryName = requireString(object, "sharedMemoryName", config.sharedMemoryName);
    config.maxLatestPoints = requireSize(object, "maxLatestPoints", config.maxLatestPoints);
    config.maxPendingWrites = requireSize(object, "maxPendingWrites", config.maxPendingWrites);
    config.maxPersistentSamples = requireSize(object, "maxPersistentSamples", config.maxPersistentSamples);
    config.sqlitePath = requireString(object, "sqlitePath", config.sqlitePath);
    config.sqliteLibraryPath = requireString(object, "sqliteLibraryPath", config.sqliteLibraryPath);
    config.persistFlushIntervalMs = requireInt(object, "persistFlushIntervalMs", config.persistFlushIntervalMs);
    config.writebackIntervalMs = requireInt(object, "writebackIntervalMs", config.writebackIntervalMs);
    config.writebackBatchSize = requireSize(object, "writebackBatchSize", config.writebackBatchSize);
    return config;
}

MqttConfig parseMqttConfig(const JsonValue* value) {
    MqttConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.enabled = requireBool(object, "enabled", config.enabled);
    config.protocolVersion = requireString(object, "protocolVersion", config.protocolVersion);
    config.broker = requireString(object, "broker", config.broker);
    config.clientId = requireString(object, "clientId", config.clientId);
    config.username = requireString(object, "username", config.username);
    config.password = requireString(object, "password", config.password);
    config.telemetryTopic = requireString(object, "telemetryTopic", config.telemetryTopic);
    config.changeEventTopic = requireString(object, "changeEventTopic", config.changeEventTopic);
    config.alarmTopic = requireString(object, "alarmTopic", config.alarmTopic);
    config.statusTopic = requireString(object, "statusTopic", config.statusTopic);
    config.commandRequestTopic = requireString(object, "commandRequestTopic", config.commandRequestTopic);
    config.commandReplyTopic = requireString(object, "commandReplyTopic", config.commandReplyTopic);
    config.otaRequestTopic = requireString(object, "otaRequestTopic", config.otaRequestTopic);
    config.otaReplyTopic = requireString(object, "otaReplyTopic", config.otaReplyTopic);
    config.otaStatusTopic = requireString(object, "otaStatusTopic", config.otaStatusTopic);
    config.systemMonitorRequestTopic = requireString(object, "systemMonitorRequestTopic", config.systemMonitorRequestTopic);
    config.systemMonitorReplyTopic = requireString(object, "systemMonitorReplyTopic", config.systemMonitorReplyTopic);
    config.systemMonitorTelemetryTopic = requireString(object, "systemMonitorTelemetryTopic", config.systemMonitorTelemetryTopic);
    config.systemMonitorAlertTopic = requireString(object, "systemMonitorAlertTopic", config.systemMonitorAlertTopic);
    config.systemMonitorPointTopic = requireString(object, "systemMonitorPointTopic", config.systemMonitorPointTopic);
    config.diagRequestTopic = requireString(object, "diagRequestTopic", config.diagRequestTopic);
    config.diagReplyTopic = requireString(object, "diagReplyTopic", config.diagReplyTopic);
    config.configPullRequestTopic = requireString(object, "configPullRequestTopic", config.configPullRequestTopic);
    config.configPullReplyTopic = requireString(object, "configPullReplyTopic", config.configPullReplyTopic);
    config.qos = requireInt(object, "qos", config.qos);
    config.cleanSession = requireBool(object, "cleanSession", config.cleanSession);
    config.keepAliveSec = requireInt(object, "keepAliveSec", config.keepAliveSec);
    config.sessionExpirySec = requireInt(object, "sessionExpirySec", config.sessionExpirySec);
    if (const auto* tls = value->find("tls")) {
        const auto& tlsObject = tls->asObject();
        config.tls.enabled = requireBool(tlsObject, "enabled", config.tls.enabled);
        config.tls.caFile = requireString(tlsObject, "caFile", config.tls.caFile);
        config.tls.certFile = requireString(tlsObject, "certFile", config.tls.certFile);
        config.tls.keyFile = requireString(tlsObject, "keyFile", config.tls.keyFile);
        config.tls.insecureSkipVerify = requireBool(tlsObject, "insecureSkipVerify", config.tls.insecureSkipVerify);
    }
    config.maxPayloadBytes = requireSize(object, "maxPayloadBytes", config.maxPayloadBytes);
    if (const auto* offline = value->find("offlineBuffer")) {
        const auto& offlineObject = offline->asObject();
        config.offlineBufferEnabled = requireBool(offlineObject, "enabled", config.offlineBufferEnabled);
        config.offlineBufferMode = requireString(offlineObject, "mode", config.offlineBufferMode);
        config.offlineBufferDir = requireString(offlineObject, "dir", config.offlineBufferDir);
        config.offlineRealtimeFile = requireString(offlineObject, "realtimeFile", config.offlineRealtimeFile);
        config.offlineRealtimeFileSizeBytes = static_cast<std::uint64_t>(requireSize(offlineObject, "realtimeFileSizeBytes", static_cast<std::size_t>(config.offlineRealtimeFileSizeBytes)));
        config.offlineMaxRealtimeMessageBytes = static_cast<std::uint32_t>(requireSize(offlineObject, "maxRealtimeMessageBytes", config.offlineMaxRealtimeMessageBytes));
        config.offlineBufferMaxMemoryMessages = requireSize(offlineObject, "maxMemoryMessages", config.offlineBufferMaxMemoryMessages);
        config.offlineBufferFlushBatchSize = requireSize(offlineObject, "flushBatchSize", config.offlineBufferFlushBatchSize);
        config.offlineBufferFlushIntervalMs = requireInt(offlineObject, "flushIntervalMs", config.offlineBufferFlushIntervalMs);
        config.offlineBufferReplayBatchSize = requireSize(offlineObject, "replayBatchSize", config.offlineBufferReplayBatchSize);
        config.offlineBufferMaxDiskBytes = requireSize(offlineObject, "maxDiskBytes", config.offlineBufferMaxDiskBytes);
        if (const auto* outbox = offline->find("eventOutbox")) {
            const auto& outboxObject = outbox->asObject();
            config.eventOutboxSqlitePath = requireString(outboxObject, "sqlitePath", config.eventOutboxSqlitePath);
            config.eventOutboxSqliteLibraryPath = requireString(outboxObject, "sqliteLibraryPath", config.eventOutboxSqliteLibraryPath);
            config.eventOutboxRetentionMonths = requireInt(outboxObject, "retentionMonths", config.eventOutboxRetentionMonths);
            config.eventOutboxCleanupIntervalHours = requireInt(outboxObject, "cleanupIntervalHours", config.eventOutboxCleanupIntervalHours);
            config.eventOutboxReplayBatchSize = requireSize(outboxObject, "replayBatchSize", config.eventOutboxReplayBatchSize);
            config.eventOutboxMaxDiskBytes = requireSize(outboxObject, "maxDiskBytes", config.eventOutboxMaxDiskBytes);
        } else {
            config.eventOutboxMaxDiskBytes = config.offlineBufferMaxDiskBytes;
        }
    }
    return config;
}

MqttAlarmRule parseMqttAlarmRule(const JsonValue& value) {
    MqttAlarmRule rule;
    const auto& object = value.asObject();
    rule.index = static_cast<std::uint32_t>(requireInt(object, "index", 0));
    if (const auto* high = value.find("high")) {
        if (!high->isNull()) {
            rule.high = high->asNumber();
        }
    }
    if (const auto* low = value.find("low")) {
        if (!low->isNull()) {
            rule.low = low->asNumber();
        }
    }
    rule.reportRecovery = requireBool(object, "reportRecovery", rule.reportRecovery);
    return rule;
}

MqttDriverConfig parseMqttDriverConfig(const JsonValue* value) {
    MqttDriverConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.enabled = requireBool(object, "enabled", config.enabled);
    config.sharedMemoryName = requireString(object, "sharedMemoryName", config.sharedMemoryName);
    if (const auto* names = value->find("sharedMemoryNames")) {
        for (const auto& item : names->asArray().values) {
            const auto name = item->asString();
            if (!name.empty()) {
                config.sharedMemoryNames.push_back(name);
            }
        }
    }
    if (config.sharedMemoryNames.empty() && !config.sharedMemoryName.empty()) {
        config.sharedMemoryNames.push_back(config.sharedMemoryName);
    }
    config.scanIntervalMs = requireInt(object, "scanIntervalMs", config.scanIntervalMs);
    config.fullUploadIntervalMs = requireInt(object, "fullUploadIntervalMs", config.fullUploadIntervalMs);
    config.snapshotBacklogThreshold = requireSize(object, "snapshotBacklogThreshold", config.snapshotBacklogThreshold);
    config.snapshotBackoffIntervalMs = requireInt(object, "snapshotBackoffIntervalMs", config.snapshotBackoffIntervalMs);
    config.eventReplayMaxBytes = requireSize(object, "eventReplayMaxBytes", config.eventReplayMaxBytes);
    config.publishFullOnStart = requireBool(object, "publishFullOnStart", config.publishFullOnStart);
    config.publishAllOnFull = requireBool(object, "publishAllOnFull", config.publishAllOnFull);
    if (const auto* indexes = value->find("fullUploadIndexes")) {
        for (const auto& item : indexes->asArray().values) {
            config.fullUploadIndexes.push_back(static_cast<std::uint32_t>(item->asNumber()));
        }
    }
    if (const auto* rules = value->find("alarmRules")) {
        for (const auto& item : rules->asArray().values) {
            config.alarmRules.push_back(parseMqttAlarmRule(*item));
        }
    }
    return config;
}

AlarmStoreConfig parseAlarmStoreConfig(const JsonValue* value) {
    AlarmStoreConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.enabled = requireBool(object, "enabled", config.enabled);
    config.sqlitePath = requireString(object, "sqlitePath", config.sqlitePath);
    config.sqliteLibraryPath = requireString(object, "sqliteLibraryPath", config.sqliteLibraryPath);
    return config;
}

EventEngineConfig parseEventEngineConfig(const JsonValue* value) {
    EventEngineConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.enabled = requireBool(object, "enabled", config.enabled);
    config.scanIntervalMs = requireInt(object, "scanIntervalMs", config.scanIntervalMs);
    config.scanFallbackIntervalMs = requireInt(object, "scanFallbackIntervalMs", config.scanFallbackIntervalMs);
    config.updateDrainBatchSize = requireSize(object, "updateDrainBatchSize", config.updateDrainBatchSize);
    config.publishMode = requireString(object, "publishMode", config.publishMode);
    config.mqttClientIdSuffix = requireString(object, "mqttClientIdSuffix", config.mqttClientIdSuffix);
    return config;
}

ComputeInputConfig parseComputeInputConfig(const JsonValue& value) {
    ComputeInputConfig config;
    const auto& object = value.asObject();
    config.name = requireString(object, "name", config.name);
    config.index = static_cast<std::uint32_t>(requireInt(object, "index", static_cast<int>(config.index)));
    config.required = requireBool(object, "required", config.required);
    return config;
}

ComputeOutputConfig parseComputeOutputConfig(const JsonValue& value, const ComputeEngineConfig& engineConfig) {
    ComputeOutputConfig config;
    const auto& object = value.asObject();
    config.name = requireString(object, "name", config.name);
    config.index = static_cast<std::uint32_t>(requireInt(object, "index", static_cast<int>(config.index)));
    config.mode = requireString(object, "mode", config.mode);
    config.sharedMemoryName = requireString(object, "sharedMemoryName", config.sharedMemoryName);
    config.ttlMs = requireInt64(object, "ttlMs", engineConfig.defaultOutputTtlMs);
    config.qualityPolicy = requireString(object, "qualityPolicy", config.qualityPolicy);
    config.minIntervalMs = requireInt(object, "minIntervalMs", config.minIntervalMs);
    config.deadband = requireDouble(object, "deadband", config.deadband);
    return config;
}

ComputeTriggerConfig parseComputeTriggerConfig(const JsonValue* value) {
    ComputeTriggerConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.type = requireString(object, "type", config.type);
    config.intervalMs = requireInt(object, "intervalMs", config.intervalMs);
    config.minIntervalMs = requireInt(object, "minIntervalMs", config.minIntervalMs);
    config.deadband = requireDouble(object, "deadband", config.deadband);
    return config;
}

ComputeScriptConfig parseComputeScriptConfig(const JsonValue* value) {
    ComputeScriptConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.type = requireString(object, "type", config.type);
    config.expression = requireString(object, "expression", config.expression);
    config.legacyGlListFile = requireString(object, "legacyGlListFile", config.legacyGlListFile);
    config.legacyVarListFile = requireString(object, "legacyVarListFile", config.legacyVarListFile);
    config.legacyEncoding = requireString(object, "legacyEncoding", config.legacyEncoding);
    config.legacyProfile = parseStringMap(value->find("legacyProfile"));
    config.graphFile = requireString(object, "graphFile", config.graphFile);
    config.graphStateFile = requireString(object, "graphStateFile", config.graphStateFile);
    config.graphProfile = parseStringMap(value->find("graphProfile"));
    return config;
}

ComputeRuleConfig parseComputeRuleConfig(const JsonValue& value, const ComputeEngineConfig& engineConfig) {
    ComputeRuleConfig config;
    const auto& object = value.asObject();
    config.ruleCode = requireString(object, "ruleCode", config.ruleCode);
    config.name = requireString(object, "name", config.name);
    config.enabled = requireBool(object, "enabled", config.enabled);
    config.trigger = parseComputeTriggerConfig(value.find("trigger"));
    if (const auto* inputs = value.find("inputs")) {
        for (const auto& item : inputs->asArray().values) {
            config.inputs.push_back(parseComputeInputConfig(*item));
        }
    }
    if (const auto* outputs = value.find("outputs")) {
        for (const auto& item : outputs->asArray().values) {
            config.outputs.push_back(parseComputeOutputConfig(*item, engineConfig));
        }
    }
    config.script = parseComputeScriptConfig(value.find("script"));
    return config;
}

ComputeEngineConfig parseComputeEngineConfig(const JsonValue* value) {
    ComputeEngineConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.enabled = requireBool(object, "enabled", config.enabled);
    config.sharedMemoryNames = parseStringArray(value->find("sharedMemoryNames"));
    config.outputDefaultSharedMemoryName = requireString(
        object,
        "outputDefaultSharedMemoryName",
        config.outputDefaultSharedMemoryName
    );
    config.scanIntervalMs = requireInt(object, "scanIntervalMs", config.scanIntervalMs);
    config.maxRuleEvalPerScan = requireSize(object, "maxRuleEvalPerScan", config.maxRuleEvalPerScan);
    config.badQuality = requireInt(object, "badQuality", config.badQuality);
    config.defaultOutputTtlMs = requireInt64(object, "defaultOutputTtlMs", config.defaultOutputTtlMs);
    config.maxWritesPerScan = requireSize(object, "maxWritesPerScan", config.maxWritesPerScan);
    if (const auto* rules = value->find("rules")) {
        for (const auto& item : rules->asArray().values) {
            config.rules.push_back(parseComputeRuleConfig(*item, config));
        }
    }
    return config;
}

OtaStorageMinioConfig parseOtaStorageMinioConfig(const JsonValue* value) {
    OtaStorageMinioConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.endpoint = requireString(object, "endpoint", config.endpoint);
    config.accessKey = requireString(object, "accessKey", config.accessKey);
    config.secretKey = requireString(object, "secretKey", config.secretKey);
    config.bucket = requireString(object, "bucket", config.bucket);
    config.basePath = requireString(object, "basePath", config.basePath);
    config.publicBaseUrl = requireString(object, "publicBaseUrl", config.publicBaseUrl);
    return config;
}

OtaStorageConfig parseOtaStorageConfig(const JsonValue* value) {
    OtaStorageConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.provider = requireString(object, "provider", config.provider);
    config.presignExpireMinutes = requireInt(object, "presignExpireMinutes", config.presignExpireMinutes);
    config.minio = parseOtaStorageMinioConfig(value->find("minio"));
    return config;
}

OtaConfig parseOtaConfig(const JsonValue* value) {
    OtaConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.enabled = requireBool(object, "enabled", config.enabled);
    config.currentVersion = requireString(object, "currentVersion", config.currentVersion);
    config.artifactBaseUrl = requireString(object, "artifactBaseUrl", config.artifactBaseUrl);
    config.downloadDir = requireString(object, "downloadDir", config.downloadDir);
    config.stagingDir = requireString(object, "stagingDir", config.stagingDir);
    config.backupDir = requireString(object, "backupDir", config.backupDir);
    config.packageType = requireString(object, "packageType", config.packageType);
    config.applyScript = requireString(object, "applyScript", config.applyScript);
    config.rollbackScript = requireString(object, "rollbackScript", config.rollbackScript);
    config.checksumRequired = requireBool(object, "checksumRequired", config.checksumRequired);
    config.autoReboot = requireBool(object, "autoReboot", config.autoReboot);
    config.retentionCount = requireInt(object, "retentionCount", config.retentionCount);
    config.statusReportIntervalSec = requireInt(object, "statusReportIntervalSec", config.statusReportIntervalSec);
    config.upgradeTimeoutSec = requireInt(object, "upgradeTimeoutSec", config.upgradeTimeoutSec);
    config.downloadRetryCount = requireInt(object, "downloadRetryCount", config.downloadRetryCount);
    config.downloadRetryBackoffMs = requireInt(object, "downloadRetryBackoffMs", config.downloadRetryBackoffMs);
    config.maxPendingStatusBytes = requireSize(object, "maxPendingStatusBytes", config.maxPendingStatusBytes);
    config.maxArtifactBytes = static_cast<std::uint64_t>(requireSize(object, "maxArtifactBytes", static_cast<std::size_t>(config.maxArtifactBytes)));
    config.minFreeBytes = static_cast<std::uint64_t>(requireSize(object, "minFreeBytes", static_cast<std::size_t>(config.minFreeBytes)));
    config.storage = parseOtaStorageConfig(value->find("storage"));
    return config;
}

RealtimeConfig parseRealtimeConfig(const JsonValue* value) {
    RealtimeConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.enabled = requireBool(object, "enabled", config.enabled);
    config.telemetryTopic = requireString(object, "telemetryTopic", config.telemetryTopic);
    config.alarmTopic = requireString(object, "alarmTopic", config.alarmTopic);
    config.statusTopic = requireString(object, "statusTopic", config.statusTopic);
    config.maxLatestPoints = requireSize(object, "maxLatestPoints", config.maxLatestPoints);
    config.trendBufferSize = requireSize(object, "trendBufferSize", config.trendBufferSize);
    config.pushThrottleMs = requireInt(object, "pushThrottleMs", config.pushThrottleMs);
    return config;
}

SystemMonitorConfig parseSystemMonitorConfig(const JsonValue* value) {
    SystemMonitorConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.enabled = requireBool(object, "enabled", config.enabled);
    config.defaultIntervalMs = requireInt(object, "defaultIntervalMs", config.defaultIntervalMs);
    config.minIntervalMs = requireInt(object, "minIntervalMs", config.minIntervalMs);
    config.subscriptionTtlSec = requireInt(object, "subscriptionTtlSec", config.subscriptionTtlSec);
    config.cpuAlertThreshold = requireDouble(object, "cpuAlertThreshold", config.cpuAlertThreshold);
    config.memAlertThreshold = requireDouble(object, "memAlertThreshold", config.memAlertThreshold);
    config.diskAlertThreshold = requireDouble(object, "diskAlertThreshold", config.diskAlertThreshold);
    config.alertRepeatIntervalSec = requireInt(object, "alertRepeatIntervalSec", config.alertRepeatIntervalSec);
    config.diagEnabled = requireBool(object, "diagEnabled", config.diagEnabled);
    config.maxDiagOutputBytes = requireSize(object, "maxDiagOutputBytes", config.maxDiagOutputBytes);
    config.allowedCommands = parseStringArray(value->find("allowedCommands"));
    if (config.allowedCommands.empty()) {
        config.allowedCommands = SystemMonitorConfig().allowedCommands;
    }
    if (const auto* cellular = value->find("cellular")) {
        const auto& cellularObject = cellular->asObject();
        config.cellular.enabled = requireBool(cellularObject, "enabled", config.cellular.enabled);
        config.cellular.probeIntervalMs = requireInt(cellularObject, "probeIntervalMs", config.cellular.probeIntervalMs);
        config.cellular.commandTimeoutMs = requireInt(cellularObject, "commandTimeoutMs", config.cellular.commandTimeoutMs);
        config.cellular.atBaudRate = requireInt(cellularObject, "atBaudRate", config.cellular.atBaudRate);
        config.cellular.signalAlertThresholdPercent = requireDouble(
            cellularObject,
            "signalAlertThresholdPercent",
            config.cellular.signalAlertThresholdPercent
        );
        config.cellular.maskSensitiveFields = requireBool(cellularObject, "maskSensitiveFields", config.cellular.maskSensitiveFields);
        const auto interfacePatterns = parseStringArray(cellular->find("interfacePatterns"));
        if (!interfacePatterns.empty()) {
            config.cellular.interfacePatterns = interfacePatterns;
        }
        const auto modemDevicePatterns = parseStringArray(cellular->find("modemDevicePatterns"));
        if (!modemDevicePatterns.empty()) {
            config.cellular.modemDevicePatterns = modemDevicePatterns;
        }
    }
    return config;
}

LocalDisplayGroupConfig parseLocalDisplayGroupConfig(const JsonValue& value) {
    LocalDisplayGroupConfig config;
    const auto& object = value.asObject();
    config.title = requireString(object, "title", config.title);
    if (const auto* indexes = value.find("pointIndexes")) {
        for (const auto& item : indexes->asArray().values) {
            config.pointIndexes.push_back(static_cast<std::uint32_t>(item->asNumber()));
        }
    }
    return config;
}

LocalDisplayPageConfig parseLocalDisplayPageConfig(const JsonValue& value) {
    LocalDisplayPageConfig config;
    const auto& object = value.asObject();
    config.pageCode = requireString(object, "pageCode", config.pageCode);
    config.title = requireString(object, "title", config.title);
    if (const auto* groups = value.find("groups")) {
        for (const auto& item : groups->asArray().values) {
            config.groups.push_back(parseLocalDisplayGroupConfig(*item));
        }
    }
    return config;
}

LocalDisplayWidgetGridConfig parseLocalDisplayWidgetGridConfig(const JsonValue* value) {
    LocalDisplayWidgetGridConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.row = requireInt(object, "row", config.row);
    config.col = requireInt(object, "col", config.col);
    config.rowSpan = requireInt(object, "rowSpan", config.rowSpan);
    config.colSpan = requireInt(object, "colSpan", config.colSpan);
    if (config.row < 0) {
        config.row = 0;
    }
    if (config.col < 0) {
        config.col = 0;
    }
    if (config.rowSpan < 1) {
        config.rowSpan = 1;
    }
    if (config.colSpan < 1) {
        config.colSpan = 1;
    }
    return config;
}

LocalDisplayWidgetGridConfig parseLocalDisplayWidgetPositionConfig(const JsonValue* value) {
    LocalDisplayWidgetGridConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.col = requireInt(object, "x", config.col);
    config.row = requireInt(object, "y", config.row);
    config.colSpan = requireInt(object, "w", config.colSpan);
    config.rowSpan = requireInt(object, "h", config.rowSpan);
    if (config.row < 0) {
        config.row = 0;
    }
    if (config.col < 0) {
        config.col = 0;
    }
    if (config.rowSpan < 1) {
        config.rowSpan = 1;
    }
    if (config.colSpan < 1) {
        config.colSpan = 1;
    }
    return config;
}

std::vector<std::uint32_t> parseUInt32Array(const JsonValue* value) {
    std::vector<std::uint32_t> result;
    if (value == nullptr || value->isNull()) {
        return result;
    }
    for (const auto& item : value->asArray().values) {
        const auto numeric = item->asNumber();
        if (numeric > 0) {
            result.push_back(static_cast<std::uint32_t>(numeric));
        }
    }
    return result;
}

LocalDisplayWidgetConfig parseLocalDisplayWidgetConfig(const JsonValue& value) {
    LocalDisplayWidgetConfig config;
    const auto& object = value.asObject();
    config.id = requireString(object, "id", config.id);
    config.type = requireString(object, "type", config.type);
    config.title = requireString(object, "title", config.title);
    config.text = requireString(object, "text", config.text);
    config.pointIndex = static_cast<std::uint32_t>(requireSize(object, "pointIndex", config.pointIndex));
    config.pointIndexes = parseUInt32Array(value.find("pointIndexes"));
    if (const auto* bind = value.find("bind")) {
        const auto& bindObject = bind->asObject();
        config.pointIndex = static_cast<std::uint32_t>(requireSize(bindObject, "index", config.pointIndex));
        const auto bindIndexes = parseUInt32Array(bind->find("indexes"));
        if (!bindIndexes.empty()) {
            config.pointIndexes = bindIndexes;
        }
    }
    config.columns = parseStringArray(value.find("columns"));
    config.valueFormat = requireString(object, "valueFormat", config.valueFormat);
    if (const auto* position = value.find("position")) {
        config.grid = parseLocalDisplayWidgetPositionConfig(position);
    } else {
        config.grid = parseLocalDisplayWidgetGridConfig(value.find("grid"));
    }
    return config;
}

LocalDisplayScreenLayoutConfig parseLocalDisplayScreenLayoutConfig(const JsonValue* value) {
    LocalDisplayScreenLayoutConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.type = requireString(object, "type", config.type);
    config.columns = requireInt(object, "columns", config.columns);
    config.rowHeight = requireInt(object, "rowHeight", config.rowHeight);
    config.gap = requireInt(object, "gap", config.gap);
    if (config.columns < 1) {
        config.columns = 12;
    }
    if (config.rowHeight < 24) {
        config.rowHeight = 64;
    }
    if (config.gap < 0) {
        config.gap = 12;
    }
    return config;
}

LocalDisplayScreenConfig parseLocalDisplayScreenConfig(const JsonValue& value) {
    LocalDisplayScreenConfig config;
    const auto& object = value.asObject();
    config.screenCode = requireString(object, "screenCode", config.screenCode);
    config.title = requireString(object, "title", config.title);
    config.layout = parseLocalDisplayScreenLayoutConfig(value.find("layout"));
    if (const auto* widgets = value.find("widgets")) {
        for (const auto& item : widgets->asArray().values) {
            config.widgets.push_back(parseLocalDisplayWidgetConfig(*item));
        }
    }
    return config;
}

LocalDisplayLayoutPageConfig parseLocalDisplayLayoutPageConfig(const JsonValue& value) {
    LocalDisplayLayoutPageConfig config;
    const auto& object = value.asObject();
    config.pageCode = requireString(object, "pageCode", config.pageCode);
    config.title = requireString(object, "title", config.title);
    if (const auto* widgets = value.find("widgets")) {
        for (const auto& item : widgets->asArray().values) {
            config.widgets.push_back(parseLocalDisplayWidgetConfig(*item));
        }
    }
    return config;
}

LocalDisplayLayoutConfig parseLocalDisplayLayoutConfig(const JsonValue* value) {
    LocalDisplayLayoutConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.version = requireInt(object, "version", config.version);
    config.theme = requireString(object, "theme", config.theme);
    config.columns = requireInt(object, "columns", config.columns);
    if (config.columns < 1) {
        config.columns = 12;
    }
    if (const auto* pages = value->find("pages")) {
        for (const auto& item : pages->asArray().values) {
            config.pages.push_back(parseLocalDisplayLayoutPageConfig(*item));
        }
    }
    return config;
}

LocalDisplayViewTemplateConfig parseLocalDisplayViewTemplateConfig(const JsonValue* value) {
    LocalDisplayViewTemplateConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.enabled = requireBool(object, "enabled", config.enabled);
    config.html = requireString(object, "html", config.html);
    config.css = requireString(object, "css", config.css);
    config.refreshIntervalMs = requireInt(object, "refreshIntervalMs", config.refreshIntervalMs);
    return config;
}

LocalDisplayConfig parseLocalDisplayConfig(const JsonValue* value) {
    LocalDisplayConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.enabled = requireBool(object, "enabled", config.enabled);
    config.bindHost = requireString(object, "bindHost", config.bindHost);
    config.port = requireInt(object, "port", config.port);
    config.refreshIntervalMs = requireInt(object, "refreshIntervalMs", config.refreshIntervalMs);
    config.maxPointsPerFrame = requireSize(object, "maxPointsPerFrame", config.maxPointsPerFrame);
    config.showOnlyConfiguredPoints = requireBool(
        object,
        "showOnlyConfiguredPoints",
        config.showOnlyConfiguredPoints
    );
    config.sharedMemoryNames = parseStringArray(value->find("sharedMemoryNames"));
    if (const auto* pages = value->find("pages")) {
        for (const auto& item : pages->asArray().values) {
            config.pages.push_back(parseLocalDisplayPageConfig(*item));
        }
    }
    if (const auto* screens = value->find("screens")) {
        for (const auto& item : screens->asArray().values) {
            config.screens.push_back(parseLocalDisplayScreenConfig(*item));
        }
    }
    config.layout = parseLocalDisplayLayoutConfig(value->find("layout"));
    config.viewTemplate = parseLocalDisplayViewTemplateConfig(value->find("viewTemplate"));
    return config;
}

CameraAuthConfig parseCameraAuthConfig(const JsonValue* value) {
    CameraAuthConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.enabled = requireBool(object, "enabled", config.enabled);
    config.mode = requireString(object, "mode", config.mode);
    config.username = requireString(object, "username", config.username);
    config.password = requireString(object, "password", config.password);
    config.token = requireString(object, "token", config.token);
    config.tokenParam = requireString(object, "tokenParam", config.tokenParam);
    config.hideCredentialsInStatus = requireBool(
        object,
        "hideCredentialsInStatus",
        config.hideCredentialsInStatus
    );
    if (config.tokenParam.empty()) {
        config.tokenParam = "token";
    }
    return config;
}

CameraMediaConfig parseCameraMediaConfig(const JsonValue* value) {
    CameraMediaConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.type = requireString(object, "type", config.type);
    config.serverUrl = requireString(object, "serverUrl", config.serverUrl);
    config.transport = requireString(object, "transport", config.transport);
    config.reconnectIntervalMs = requireInt(object, "reconnectIntervalMs", config.reconnectIntervalMs);
    config.auth = parseCameraAuthConfig(value->find("auth"));
    if (config.reconnectIntervalMs < 1000) {
        config.reconnectIntervalMs = 1000;
    }
    return config;
}

CameraVideoConfig parseCameraVideoConfig(const JsonValue* value) {
    CameraVideoConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.width = requireInt(object, "width", config.width);
    config.height = requireInt(object, "height", config.height);
    config.fps = requireInt(object, "fps", config.fps);
    config.codec = requireString(object, "codec", config.codec);
    config.bitrateKbps = requireInt(object, "bitrateKbps", config.bitrateKbps);
    if (config.width <= 0) {
        config.width = 1280;
    }
    if (config.height <= 0) {
        config.height = 720;
    }
    if (config.fps <= 0) {
        config.fps = 15;
    }
    if (config.bitrateKbps <= 0) {
        config.bitrateKbps = 1500;
    }
    return config;
}

CameraStreamConfig parseCameraStreamConfig(const JsonValue* value) {
    CameraStreamConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.path = requireString(object, "path", config.path);
    config.publishUrl = requireString(object, "publishUrl", config.publishUrl);
    return config;
}

CameraStatusPointIndexes parseCameraStatusPointIndexes(const JsonValue* value) {
    CameraStatusPointIndexes config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.online = static_cast<std::uint32_t>(requireSize(object, "online", config.online));
    config.fps = static_cast<std::uint32_t>(requireSize(object, "fps", config.fps));
    config.bitrateKbps = static_cast<std::uint32_t>(requireSize(object, "bitrateKbps", config.bitrateKbps));
    config.errorCode = static_cast<std::uint32_t>(requireSize(object, "errorCode", config.errorCode));
    return config;
}

CameraConfig parseCameraConfig(const JsonValue& value) {
    CameraConfig config;
    const auto& object = value.asObject();
    config.cameraCode = requireString(object, "cameraCode", config.cameraCode);
    config.name = requireString(object, "name", config.name);
    config.enabled = requireBool(object, "enabled", config.enabled);
    config.sourceType = requireString(object, "sourceType", config.sourceType);
    config.source = requireString(object, "source", config.source);
    config.sourceAuth = parseCameraAuthConfig(value.find("sourceAuth"));
    config.command = requireString(object, "command", config.command);
    config.video = parseCameraVideoConfig(value.find("video"));
    config.stream = parseCameraStreamConfig(value.find("stream"));
    config.statusPointIndexes = parseCameraStatusPointIndexes(value.find("statusPointIndexes"));
    return config;
}

CameraServiceConfig parseCameraServiceConfig(const JsonValue* value) {
    CameraServiceConfig config;
    if (value == nullptr || value->isNull()) {
        return config;
    }
    const auto& object = value->asObject();
    config.enabled = requireBool(object, "enabled", config.enabled);
    config.statusIntervalMs = requireInt(object, "statusIntervalMs", config.statusIntervalMs);
    config.sharedMemoryName = requireString(object, "sharedMemoryName", config.sharedMemoryName);
    config.statusTopic = requireString(object, "statusTopic", config.statusTopic);
    config.eventTopic = requireString(object, "eventTopic", config.eventTopic);
    config.media = parseCameraMediaConfig(value->find("media"));
    if (const auto* cameras = value->find("cameras")) {
        for (const auto& item : cameras->asArray().values) {
            config.cameras.push_back(parseCameraConfig(*item));
        }
    }
    if (config.statusIntervalMs < 1000) {
        config.statusIntervalMs = 1000;
    }
    return config;
}

AppConfig buildBuiltinExampleAppConfig() {
    AppConfig config;
    config.runtimeMode = "gateway";
    config.identityConfigFile = "config/runtime/device_identity.json";
    config.deviceConfigFiles = {"config/runtime/devices/device_slave_ttySP1.json"};
    config.mqtt.enabled = false;
    config.mqtt.protocolVersion = "mqtt3";
    config.mqtt.broker = "tcp://127.0.0.1:1883";
    config.mqtt.clientId = "GW0001";
    config.mqtt.telemetryTopic = "edge/telemetry";
    config.mqtt.changeEventTopic = "edge/event/change";
    config.mqtt.alarmTopic = "edge/alarm";
    config.mqtt.statusTopic = "edge/status";
    config.mqtt.commandRequestTopic = "edge/command/request";
    config.mqtt.commandReplyTopic = "edge/command/reply";
    config.mqtt.otaRequestTopic = "edge/ota/request";
    config.mqtt.otaReplyTopic = "edge/ota/reply";
    config.mqtt.otaStatusTopic = "edge/ota/status";
    config.mqtt.systemMonitorRequestTopic = "edge/system/monitor/request";
    config.mqtt.systemMonitorReplyTopic = "edge/system/monitor/reply";
    config.mqtt.systemMonitorTelemetryTopic = "edge/system/monitor/telemetry";
    config.mqtt.systemMonitorAlertTopic = "edge/system/monitor/alert";
    config.mqtt.systemMonitorPointTopic = "edge/system/monitor/points";
    config.mqtt.diagRequestTopic = "edge/system/diag/request";
    config.mqtt.diagReplyTopic = "edge/system/diag/reply";
    config.mqtt.configPullRequestTopic = "edge/config/pull/request";
    config.mqtt.configPullReplyTopic = "edge/config/pull/reply";
    config.mqtt.qos = 1;
    config.mqtt.cleanSession = true;
    config.mqtt.keepAliveSec = 60;
    config.mqtt.sessionExpirySec = 0;
    config.mqttDriver.enabled = false;
    config.mqttDriver.sharedMemoryName = "gateway_point_store";
    config.mqttDriver.scanIntervalMs = 1000;
    config.mqttDriver.fullUploadIntervalMs = 60000;
    config.mqttDriver.snapshotBacklogThreshold = 0;
    config.mqttDriver.snapshotBackoffIntervalMs = 0;
    config.mqttDriver.eventReplayMaxBytes = 256 * 1024;
    config.mqttDriver.publishFullOnStart = true;
    config.mqttDriver.publishAllOnFull = true;
    config.alarmStore.enabled = false;
    config.alarmStore.sqlitePath = "alarm_events.db";
    config.eventEngine.enabled = true;
    config.eventEngine.scanIntervalMs = 100;
    config.eventEngine.scanFallbackIntervalMs = 5000;
    config.eventEngine.updateDrainBatchSize = 4096;
    config.eventEngine.publishMode = "mqtt_driver_outbox";
    config.eventEngine.mqttClientIdSuffix = "event_engine";
    config.computeEngine.enabled = false;
    config.computeEngine.sharedMemoryNames = {"gateway_point_store"};
    config.computeEngine.outputDefaultSharedMemoryName = "gateway_point_store";
    config.computeEngine.scanIntervalMs = 200;
    config.computeEngine.maxRuleEvalPerScan = 1000;
    config.computeEngine.defaultOutputTtlMs = 600000;
    config.computeEngine.maxWritesPerScan = 100;
    config.ota.enabled = true;
    config.ota.currentVersion = "1.0.0";
    config.ota.artifactBaseUrl = "https://example.com/releases";
    config.ota.downloadDir = "/opt/modbus-gateway/ota/downloads";
    config.ota.stagingDir = "/opt/modbus-gateway/ota/staging";
    config.ota.backupDir = "/opt/modbus-gateway/ota/backup";
    config.ota.packageType = "tar.gz";
    config.ota.applyScript = "/opt/modbus-gateway/bin/ota-apply.sh";
    config.ota.rollbackScript = "/opt/modbus-gateway/bin/ota-rollback.sh";
    config.ota.checksumRequired = true;
    config.ota.autoReboot = true;
    config.ota.retentionCount = 3;
    config.ota.statusReportIntervalSec = 5;
    config.ota.upgradeTimeoutSec = 900;
    config.ota.maxArtifactBytes = 512ULL * 1024ULL * 1024ULL;
    config.ota.minFreeBytes = 256ULL * 1024ULL * 1024ULL;
    config.ota.storage.provider = "local";
    config.ota.storage.presignExpireMinutes = 60;
    config.ota.storage.minio.endpoint = "http://127.0.0.1:9000";
    config.ota.storage.minio.accessKey = "minioadmin";
    config.ota.storage.minio.secretKey = "minioadmin";
    config.ota.storage.minio.bucket = "edge-ota";
    config.ota.storage.minio.basePath = "packages";
    config.realtime.enabled = true;
    config.realtime.telemetryTopic = "edge/telemetry";
    config.realtime.alarmTopic = "edge/alarm";
    config.realtime.statusTopic = "edge/status";
    config.realtime.maxLatestPoints = 100000;
    config.realtime.trendBufferSize = 300;
    config.realtime.pushThrottleMs = 200;
    config.systemMonitor.enabled = false;
    config.systemMonitor.defaultIntervalMs = 5000;
    config.systemMonitor.minIntervalMs = 1000;
    config.systemMonitor.subscriptionTtlSec = 30;
    config.systemMonitor.cpuAlertThreshold = 90.0;
    config.systemMonitor.memAlertThreshold = 90.0;
    config.systemMonitor.diskAlertThreshold = 90.0;
    config.systemMonitor.alertRepeatIntervalSec = 60;
    config.systemMonitor.diagEnabled = true;
    config.localDisplay.enabled = false;
    config.localDisplay.bindHost = "127.0.0.1";
    config.localDisplay.port = 18080;
    config.localDisplay.refreshIntervalMs = 500;
    config.localDisplay.maxPointsPerFrame = 500;
    config.localDisplay.showOnlyConfiguredPoints = true;
    config.localDisplay.sharedMemoryNames = {"gateway_point_store"};
    LocalDisplayScreenConfig screen;
    screen.screenCode = "overview";
    screen.title = "设备总览";
    LocalDisplayWidgetConfig titleWidget;
    titleWidget.id = "title_overview";
    titleWidget.type = "groupTitle";
    titleWidget.title = "网关总览";
    titleWidget.text = "Gateway Local Display";
    titleWidget.grid.colSpan = 12;
    LocalDisplayWidgetConfig tableWidget;
    tableWidget.id = "key_points";
    tableWidget.type = "pointTable";
    tableWidget.title = "关键点表";
    tableWidget.pointIndexes = {11000, 11001, 11002, 11003, 11004, 11005, 11006, 11007, 11008, 11009};
    tableWidget.columns = {"index", "meterCode", "pointCode", "name", "value", "unit", "quality", "time"};
    tableWidget.grid.row = 1;
    tableWidget.grid.colSpan = 12;
    tableWidget.grid.rowSpan = 6;
    screen.widgets = {titleWidget, tableWidget};
    config.localDisplay.screens = {screen};
    return config;
}

DeviceConfig buildBuiltinExampleDeviceConfig() {
    DeviceConfig config;
    config.schemaVersion = "1.0.0";
    config.machineCode = "";
    config.meterCode = "METER0001";
    config.deviceName = "1号储能电表";
    config.protocol.type = "modbus_rtu";
    config.protocol.slave = 1;
    config.protocol.transport.serialPort = "/dev/ttyS1";
    config.protocol.transport.baudRate = 9600;
    config.protocol.transport.dataBits = 8;
    config.protocol.transport.stopBits = 1;
    config.protocol.transport.parity = "N";
    config.protocol.transport.timeoutMs = 1000;
    config.protocol.tcp.host = "127.0.0.1";
    config.protocol.tcp.port = 502;
    config.protocol.tcp.connectTimeoutMs = 1000;
    config.protocol.tcp.timeoutMs = 1000;
    config.collect.defaultIntervalMs = 5000;
    config.collect.batchOptimize = true;
    config.collect.maxBatchRegisters = 120;
    config.memoryStore.enabled = true;
    config.memoryStore.backend = "memory";
    config.memoryStore.keepHistory = 100;
    config.memoryStore.defaultTtlMs = 600000;
    config.memoryStore.indexBy = {"machineCode", "meterCode", "pointCode"};
    config.memoryStore.sharedMemoryName = "gateway_point_store";
    config.memoryStore.maxLatestPoints = 100000;
    config.memoryStore.maxPendingWrites = 4096;
    config.memoryStore.maxPersistentSamples = 20000;
    config.memoryStore.sqlitePath = "point_samples.db";
    config.memoryStore.sqliteLibraryPath = "";
    config.memoryStore.persistFlushIntervalMs = 60000;
    config.memoryStore.writebackIntervalMs = 500;
    config.memoryStore.writebackBatchSize = 100;

    PointDefinition voltage;
    voltage.index = 10001;
    voltage.pointCode = "voltage_a";
    voltage.name = "A相电压";
    voltage.desc = "电表A相电压";
    voltage.category = "telemetry";
    voltage.address = 0;
    voltage.enabled = true;
    voltage.isStore = false;
    voltage.persistIntervalSec = 60;
    voltage.read.enable = true;
    voltage.read.function = 3;
    voltage.read.length = 1;
    voltage.read.dataType = "uint16";
    voltage.read.scale = 0.1;
    voltage.read.offset = 0.0;
    voltage.read.byteOrder = "AB";
    voltage.read.signedFlag = false;
    voltage.read.unit = "V";
    voltage.read.intervalMs = 5000;
    voltage.read.cachePolicy.storeLatest = true;
    voltage.read.cachePolicy.storeHistory = true;
    voltage.read.cachePolicy.historySize = 100;
    voltage.read.cachePolicy.ttlMs = 600000;
    voltage.write.enable = false;

    PointDefinition runMode;
    runMode.index = 10002;
    runMode.pointCode = "run_mode";
    runMode.name = "运行模式";
    runMode.desc = "设备运行模式";
    runMode.category = "setting";
    runMode.address = 120;
    runMode.enabled = true;
    runMode.isStore = true;
    runMode.persistIntervalSec = 60;
    runMode.read.enable = true;
    runMode.read.function = 3;
    runMode.read.length = 1;
    runMode.read.dataType = "uint16";
    runMode.read.scale = 1.0;
    runMode.read.offset = 0.0;
    runMode.read.byteOrder = "AB";
    runMode.read.signedFlag = false;
    runMode.read.unit = "";
    runMode.read.intervalMs = 2000;
    runMode.read.cachePolicy.storeLatest = true;
    runMode.read.cachePolicy.storeHistory = true;
    runMode.read.cachePolicy.historySize = 50;
    runMode.read.cachePolicy.ttlMs = 600000;
    runMode.write.enable = true;
    runMode.write.function = 6;
    runMode.write.length = 1;
    runMode.write.dataType = "uint16";
    runMode.write.scale = 1.0;
    runMode.write.offset = 0.0;
    runMode.write.byteOrder = "AB";
    runMode.write.minValue = 0.0;
    runMode.write.maxValue = 2.0;
    runMode.write.step = 1.0;
    runMode.write.allowedValues = {0.0, 1.0, 2.0};
    runMode.write.verifyAfterWrite = true;
    runMode.write.verifyDelayMs = 200;
    runMode.write.verifyByRead = true;
    runMode.valueMap = {
        {"0", "停止"},
        {"1", "自动"},
        {"2", "手动"}
    };

    config.points = {voltage, runMode};
    return config;
}

DeviceConfig parseDeviceConfig(const std::string& text) {
    const auto root = JsonParser(text).parse();
    const auto& object = root.asObject();

    DeviceConfig config;
    config.schemaVersion = requireString(object, "schemaVersion", config.schemaVersion);
    config.machineCode = requireString(object, "machineCode", config.machineCode);
    config.meterCode = requireString(object, "meterCode", config.meterCode);
    config.deviceName = requireString(object, "deviceName", config.deviceName);
    config.protocol = parseProtocol(root.find("protocol"));
    parseDlt645Config(root.find("dlt645"), config.protocol);
    config.collect = parseCollect(root.find("collect"));
    config.memoryStore = parseMemoryStore(root.find("memoryStore"));

    if (const auto* meters = root.find("meters")) {
        for (const auto& item : meters->asArray().values) {
            config.meters.push_back(parseLogicalDevice(*item, config.protocol.slave));
        }
    } else {
        throw std::invalid_argument("device config missing required top-level field: meters");
    }

    if (const auto* points = root.find("points")) {
        for (const auto& item : points->asArray().values) {
            config.points.push_back(parsePointDefinition(*item));
        }
    }
    return config;
}

std::vector<PointDefinition> parseDlt645StandardPoints(const std::string& text) {
    const auto root = JsonParser(text).parse();
    if (!root.isObject()) {
        throw std::invalid_argument("DLT645 standard points root must be object");
    }
    const auto* points = root.find("points");
    if (points == nullptr || !points->isArray()) {
        throw std::invalid_argument("DLT645 standard points missing required points array");
    }

    std::vector<PointDefinition> result;
    result.reserve(points->asArray().values.size());
    std::uint32_t generatedIndex = 1;
    for (const auto& item : points->asArray().values) {
        const auto& object = item->asObject();
        PointDefinition point;
        point.index = generatedIndex++;
        point.pointCode = requireString(object, "pointCode");
        point.name = requireString(object, "name");
        point.desc = requireString(object, "desc");
        point.category = requireString(object, "category", "telemetry");
        point.enabled = requireBool(object, "enabledByDefault", true);
        point.isStore = requireBool(object, "storeHistory", true);
        point.fullUpload = requireBool(object, "fullUpload", true);
        point.reportOnChange = requireBool(object, "reportOnChange", false);
        point.persistIntervalSec = 60;

        point.read.enable = point.enabled;
        point.read.dataType = requireString(object, "dataType");
        point.read.scale = requireDouble(object, "scale", 1.0);
        point.read.offset = 0.0;
        point.read.unit = requireString(object, "unit");
        point.read.intervalMs = requireInt(object, "defaultIntervalMs", 1000);
        point.read.dlt645Di = requireString(object, "di");
        point.read.dlt645ByteCount = requireInt(object, "byteCount", 0);
        point.read.dlt645Decoder = requireString(object, "decoder", point.read.dlt645Decoder);
        point.read.length = point.read.dlt645ByteCount > 0 ? point.read.dlt645ByteCount : point.read.length;
        point.read.cachePolicy.storeLatest = requireBool(object, "storeLatest", true);
        point.read.cachePolicy.storeHistory = requireBool(object, "storeHistory", true);
        point.read.cachePolicy.historySize = 100;
        point.read.cachePolicy.ttlMs = 600000;

        result.push_back(std::move(point));
    }
    return result;
}

AppConfig parseAppConfig(const std::string& text) {
    const auto root = JsonParser(text).parse();
    AppConfig config;
    config.runtimeMode = requireString(root.asObject(), "runtimeMode", config.runtimeMode);
    if (config.runtimeMode != "gateway" && config.runtimeMode != "ems") {
        throw std::invalid_argument("app config runtimeMode must be gateway or ems");
    }
    config.identityConfigFile = requireString(root.asObject(), "identityConfigFile", config.identityConfigFile);
    config.deviceConfigFiles = parseStringArray(root.find("deviceConfigFiles"));
    config.mqtt = parseMqttConfig(root.find("mqtt"));
    config.mqttDriver = parseMqttDriverConfig(root.find("mqttDriver"));
    config.alarmStore = parseAlarmStoreConfig(root.find("alarmStore"));
    config.eventEngine = parseEventEngineConfig(root.find("eventEngine"));
    config.computeEngine = parseComputeEngineConfig(root.find("computeEngine"));
    config.ota = parseOtaConfig(root.find("ota"));
    config.realtime = parseRealtimeConfig(root.find("realtime"));
    config.systemMonitor = parseSystemMonitorConfig(root.find("systemMonitor"));
    config.localDisplay = parseLocalDisplayConfig(root.find("localDisplay"));
    config.cameraService = parseCameraServiceConfig(root.find("cameraService"));
    return config;
}

DeviceIdentity parseDeviceIdentity(const std::string& text) {
    const auto root = JsonParser(text).parse();
    const auto& object = root.asObject();
    DeviceIdentity identity;
    identity.schemaVersion = requireString(object, "schemaVersion", identity.schemaVersion);
    identity.machineCode = requireString(object, "machineCode", identity.machineCode);
    identity.imei = requireString(object, "imei", identity.imei);
    identity.imei = requireString(object, "immei", identity.imei);
    identity.serialNumber = requireString(object, "serialNumber", identity.serialNumber);
    identity.model = requireString(object, "model", identity.model);
    identity.hardwareVersion = requireString(object, "hardwareVersion", identity.hardwareVersion);
    identity.firmwareVersion = requireString(object, "firmwareVersion", identity.firmwareVersion);
    if (identity.machineCode.empty()) {
        throw std::invalid_argument("device identity missing required field: machineCode");
    }
    return identity;
}

}  // namespace

DeviceConfig ConfigLoader::loadFromText(const std::string& text) {
    auto config = parseDeviceConfig(text);
    return config;
}

DeviceConfig ConfigLoader::loadFromFile(const std::string& filePath) {
    std::ifstream input(filePath);
    if (input.is_open()) {
        std::stringstream buffer;
        buffer << input.rdbuf();
        try {
            auto config = parseDeviceConfig(buffer.str());
            resolveConfigRelativePaths(config, filePath);
            const auto identityPath = discoverIdentityConfigFile(filePath);
            if (!identityPath.empty()) {
                applyIdentity(config, loadDeviceIdentityFromFile(identityPath));
            }
            return config;
        } catch (const std::exception& ex) {
            throw std::runtime_error("failed to parse config file: " + filePath + ": " + ex.what());
        }
    }

    if (filePath == "config/runtime/devices/device_slave_ttySP1.json" ||
        filePath == ".\\config\\runtime\\devices\\device_slave_ttySP1.json") {
        auto config = buildBuiltinExampleDeviceConfig();
        const auto identityPath = discoverIdentityConfigFile(filePath);
        if (!identityPath.empty()) {
            applyIdentity(config, loadDeviceIdentityFromFile(identityPath));
        }
        return config;
    }

    throw std::runtime_error("failed to open config file: " + filePath);
}

DeviceConfig ConfigLoader::loadFromFile(const std::string& filePath, const DeviceIdentity& identity) {
    auto config = loadFromFile(filePath);
    applyIdentity(config, identity);
    return config;
}

std::vector<DeviceConfig> ConfigLoader::loadMany(const std::vector<std::string>& filePaths) {
    std::vector<DeviceConfig> configs;
    configs.reserve(filePaths.size());
    for (const auto& filePath : filePaths) {
        configs.push_back(loadFromFile(filePath));
    }
    return configs;
}

std::vector<DeviceConfig> ConfigLoader::loadMany(
    const std::vector<std::string>& filePaths,
    const DeviceIdentity& identity
) {
    std::vector<DeviceConfig> configs;
    configs.reserve(filePaths.size());
    for (const auto& filePath : filePaths) {
        configs.push_back(loadFromFile(filePath, identity));
    }
    return configs;
}

AppConfig ConfigLoader::loadAppConfigFromFile(const std::string& filePath) {
    std::ifstream input(filePath);
    if (input.is_open()) {
        std::stringstream buffer;
        buffer << input.rdbuf();
        try {
            auto config = parseAppConfig(buffer.str());
            resolveAppConfigRelativePaths(config, filePath);
            return config;
        } catch (const std::exception& ex) {
            throw std::runtime_error("failed to parse app config file: " + filePath + ": " + ex.what());
        }
    }

    if (filePath == "config/runtime/apps/mqtt-service.json" ||
        filePath == ".\\config\\runtime\\apps\\mqtt-service.json") {
        return buildBuiltinExampleAppConfig();
    }

    throw std::runtime_error("failed to open app config file: " + filePath);
}

DeviceIdentity ConfigLoader::loadDeviceIdentityFromFile(const std::string& filePath) {
    if (filePath.empty()) {
        return DeviceIdentity();
    }
    std::ifstream input(filePath);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open device identity file: " + filePath);
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    try {
        return parseDeviceIdentity(buffer.str());
    } catch (const std::exception& ex) {
        throw std::runtime_error("failed to parse device identity file: " + filePath + ": " + ex.what());
    }
}

void ConfigLoader::applyIdentity(DeviceConfig& config, const DeviceIdentity& identity) {
    if (identity.machineCode.empty()) {
        return;
    }
    if (!config.machineCode.empty() && config.machineCode != identity.machineCode) {
        throw std::invalid_argument(
            "device config machineCode mismatch identity file: config=" + config.machineCode +
            " identity=" + identity.machineCode
        );
    }
    config.machineCode = identity.machineCode;
}

MqttDriverConfig ConfigLoader::loadMqttDriverConfigFromFile(const std::string& filePath) {
    return loadAppConfigFromFile(filePath).mqttDriver;
}

std::vector<PointDefinition> ConfigLoader::loadDlt645StandardPointsFromFile(const std::string& filePath) {
    std::ifstream input(filePath);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open DLT645 standard points file: " + filePath);
    }

    std::stringstream buffer;
    buffer << input.rdbuf();
    try {
        return parseDlt645StandardPoints(buffer.str());
    } catch (const std::exception& ex) {
        throw std::runtime_error("failed to parse DLT645 standard points file: " + filePath + ": " + ex.what());
    }
}

}  // namespace edge_gateway
