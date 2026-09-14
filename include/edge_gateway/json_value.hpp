#pragma once

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <locale>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace edge_gateway {
namespace json {

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
    explicit JsonParser(const std::string& text, std::size_t maxDepth = 0, std::size_t maxNodes = 0)
        : text_(text), maxDepth_(maxDepth), maxNodes_(maxNodes) {
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
        if ((maxDepth_ && depth_ >= maxDepth_) || (maxNodes_ && ++nodes_ > maxNodes_)) {
            fail("json nesting/node limit exceeded");
        }
        struct DepthScope {
            std::size_t& depth;
            explicit DepthScope(std::size_t& value) : depth(value) { ++depth; }
            ~DepthScope() { --depth; }
        } depthScope(depth_);
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
                if (maxDepth_) validateUtf8(result);
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
                    case 'u': {
                        auto value = unicodeUnit();
                        if (value >= 0xd800 && value <= 0xdbff) {
                            expect('\\');
                            expect('u');
                            const auto low = unicodeUnit();
                            if (low < 0xdc00 || low > 0xdfff) fail("invalid json surrogate pair");
                            value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
                        } else if (value >= 0xdc00 && value <= 0xdfff) {
                            fail("unpaired json surrogate");
                        }
                        if (value <= 0x7f) result.push_back(static_cast<char>(value));
                        else if (value <= 0x7ff) {
                            result.push_back(static_cast<char>(0xc0 | (value >> 6)));
                            result.push_back(static_cast<char>(0x80 | (value & 63)));
                        } else if (value <= 0xffff) {
                            result.push_back(static_cast<char>(0xe0 | (value >> 12)));
                            result.push_back(static_cast<char>(0x80 | ((value >> 6) & 63)));
                            result.push_back(static_cast<char>(0x80 | (value & 63)));
                        } else {
                            result.push_back(static_cast<char>(0xf0 | (value >> 18)));
                            result.push_back(static_cast<char>(0x80 | ((value >> 12) & 63)));
                            result.push_back(static_cast<char>(0x80 | ((value >> 6) & 63)));
                            result.push_back(static_cast<char>(0x80 | (value & 63)));
                        }
                        break;
                    }
                    default:
                        fail("unsupported json escape");
                }
            } else {
                if (maxDepth_ && static_cast<unsigned char>(ch) < 0x20) fail("raw control character in json string");
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
        if (maxDepth_) {
            // The protocol uses strict JSON; preserve the legacy configuration parser's default mode.
            std::size_t i = start;
            if (text_[i] == '-') ++i;
            if (i == pos_) fail("invalid json number");
            if (text_[i] == '0') ++i;
            else {
                if (text_[i] < '1' || text_[i] > '9') fail("invalid json number");
                while (i < pos_ && std::isdigit(static_cast<unsigned char>(text_[i]))) ++i;
            }
            if (i < pos_ && text_[i] == '.') {
                const auto begin = ++i;
                while (i < pos_ && std::isdigit(static_cast<unsigned char>(text_[i]))) ++i;
                if (i == begin) fail("missing fractional digits");
            }
            if (i < pos_ && (text_[i] == 'e' || text_[i] == 'E')) {
                ++i;
                if (i < pos_ && (text_[i] == '+' || text_[i] == '-')) ++i;
                const auto begin = i;
                while (i < pos_ && std::isdigit(static_cast<unsigned char>(text_[i]))) ++i;
                if (i == begin) fail("missing exponent digits");
            }
            if (i != pos_) fail("invalid json number syntax");
            std::istringstream number(text_.substr(start, pos_ - start));
            number.imbue(std::locale::classic());
            double value = 0;
            number >> std::noskipws >> value;
            if (!number || !number.eof() || !std::isfinite(value)) fail("json number out of range");
            return value;
        }
        return std::strtod(text_.c_str() + start, nullptr);
    }

    void validateUtf8(const std::string& value) const {
        for (std::size_t i = 0; i < value.size();) {
            const auto first = static_cast<unsigned char>(value[i++]);
            if (first < 0x80) continue;
            unsigned int code = 0;
            unsigned int minimum = 0;
            std::size_t remaining = 0;
            if (first >= 0xc2 && first <= 0xdf) { code = first & 31; minimum = 0x80; remaining = 1; }
            else if (first >= 0xe0 && first <= 0xef) { code = first & 15; minimum = 0x800; remaining = 2; }
            else if (first >= 0xf0 && first <= 0xf4) { code = first & 7; minimum = 0x10000; remaining = 3; }
            else fail("invalid UTF-8 leading byte");
            if (remaining > value.size() - i) fail("truncated UTF-8 sequence");
            while (remaining--) {
                const auto next = static_cast<unsigned char>(value[i++]);
                if ((next & 0xc0) != 0x80) fail("invalid UTF-8 continuation byte");
                code = (code << 6) | (next & 63);
            }
            if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
                fail("invalid UTF-8 code point");
            }
        }
    }

    unsigned int unicodeUnit() {
        unsigned int value = 0;
        for (int i = 0; i < 4; ++i) {
            if (isEnd()) fail("truncated json unicode escape");
            const char ch = get();
            unsigned int digit = 0;
            if (ch >= '0' && ch <= '9') digit = ch - '0';
            else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
            else fail("invalid json unicode escape");
            value = value * 16 + digit;
        }
        return value;
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
        while (!isEnd() && (maxDepth_ ? (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                text_[pos_] == '\r' || text_[pos_] == '\n') :
                std::isspace(static_cast<unsigned char>(text_[pos_])) != 0)) {
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
    std::size_t depth_ = 0;
    std::size_t nodes_ = 0;
    std::size_t maxDepth_;
    std::size_t maxNodes_;
};


}  // namespace json
}  // namespace edge_gateway
