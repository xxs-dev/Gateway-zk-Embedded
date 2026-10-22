#include "v2_json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace edge_gateway {
namespace maintenance_v2 {

namespace {

const double kMaxSafeInteger = 9007199254740992.0;  // 2^53

std::vector<std::uint16_t> utf16Units(const std::string& text) {
    std::vector<std::uint16_t> out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::uint32_t code = c;
        std::size_t len = 1;
        if (c >= 0xf0 && i + 3 < text.size()) {
            code = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(text[i + 1]) & 0x3fu) << 12) |
                   ((static_cast<unsigned char>(text[i + 2]) & 0x3fu) << 6) | (static_cast<unsigned char>(text[i + 3]) & 0x3fu);
            len = 4;
        } else if (c >= 0xe0 && i + 2 < text.size()) {
            code = ((c & 0x0fu) << 12) | ((static_cast<unsigned char>(text[i + 1]) & 0x3fu) << 6) |
                   (static_cast<unsigned char>(text[i + 2]) & 0x3fu);
            len = 3;
        } else if (c >= 0xc0 && i + 1 < text.size()) {
            code = ((c & 0x1fu) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3fu);
            len = 2;
        }
        if (code >= 0x10000) {
            code -= 0x10000;
            out.push_back(static_cast<std::uint16_t>(0xd800 + (code >> 10)));
            out.push_back(static_cast<std::uint16_t>(0xdc00 + (code & 0x3ff)));
        } else {
            out.push_back(static_cast<std::uint16_t>(code));
        }
        i += len;
    }
    return out;
}

void canonicalInto(const JsonValue& value, std::string& out) {
    switch (value.type()) {
    case JsonValue::Type::Null:
        out += "null";
        return;
    case JsonValue::Type::Bool:
        out += value.asBool() ? "true" : "false";
        return;
    case JsonValue::Type::Number: {
        const double v = value.asNumber();
        if (!std::isfinite(v) || std::floor(v) != v || std::fabs(v) > kMaxSafeInteger) {
            throw std::runtime_error("canonical json supports integers only");
        }
        out += std::to_string(static_cast<long long>(v == 0 ? 0.0 : v));
        return;
    }
    case JsonValue::Type::String:
        out += quote(value.asString());
        return;
    case JsonValue::Type::Array: {
        out += '[';
        bool first = true;
        for (const auto& item : value.asArray().values) {
            if (!first) out += ',';
            first = false;
            canonicalInto(*item, out);
        }
        out += ']';
        return;
    }
    case JsonValue::Type::Object: {
        const auto& members = value.asObject().values;
        std::vector<std::pair<std::vector<std::uint16_t>, const JsonMember*>> sorted;
        sorted.reserve(members.size());
        for (const auto& m : members) sorted.emplace_back(utf16Units(m.key), &m);
        std::sort(sorted.begin(), sorted.end(), [](const std::pair<std::vector<std::uint16_t>, const JsonMember*>& a,
                                                   const std::pair<std::vector<std::uint16_t>, const JsonMember*>& b) {
            return a.first < b.first;
        });
        for (std::size_t i = 1; i < sorted.size(); ++i) {
            if (sorted[i].first == sorted[i - 1].first) throw std::runtime_error("duplicate json key: " + sorted[i].second->key);
        }
        out += '{';
        bool first = true;
        for (const auto& entry : sorted) {
            if (!first) out += ',';
            first = false;
            out += quote(entry.second->key);
            out += ':';
            canonicalInto(*entry.second->value, out);
        }
        out += '}';
        return;
    }
    }
}

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64Value(char c, bool url) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (!url && c == '+') return 62;
    if (!url && c == '/') return 63;
    if (url && c == '-') return 62;
    if (url && c == '_') return 63;
    return -1;
}

bool decodeBase64(const std::string& text, bool url, std::string* out) {
    std::string result;
    result.reserve(text.size() * 3 / 4);
    std::uint32_t buffer = 0;
    int bits = 0;
    std::size_t i = 0;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '=') break;
        if (!url && (c == '\r' || c == '\n')) continue;
        const int v = b64Value(c, url);
        if (v < 0) return false;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<char>((buffer >> bits) & 0xff));
        }
    }
    if (url && i != text.size()) return false;  // base64url without padding
    for (; i < text.size(); ++i) {
        if (text[i] != '=' && text[i] != '\r' && text[i] != '\n') return false;
    }
    if (bits >= 6) return false;
    if (bits > 0 && (buffer & ((1u << bits) - 1u)) != 0) return false;  // non-canonical trailing bits
    *out = std::move(result);
    return true;
}

}  // namespace

JsonValue parseJson(const std::string& text, std::size_t maxNodes) {
    return JsonParser(text, 64, maxNodes).parse();
}

std::string canonicalJson(const JsonValue& value) {
    std::string out;
    canonicalInto(value, out);
    return out;
}

namespace {

void serializeInto(const JsonValue& value, std::string& out) {
    switch (value.type()) {
    case JsonValue::Type::Null:
        out += "null";
        return;
    case JsonValue::Type::Bool:
        out += value.asBool() ? "true" : "false";
        return;
    case JsonValue::Type::Number:
        out += number(value.asNumber());
        return;
    case JsonValue::Type::String:
        out += quote(value.asString());
        return;
    case JsonValue::Type::Array: {
        out += '[';
        bool first = true;
        for (const auto& item : value.asArray().values) {
            if (!first) out += ',';
            first = false;
            serializeInto(*item, out);
        }
        out += ']';
        return;
    }
    case JsonValue::Type::Object: {
        out += '{';
        bool first = true;
        for (const auto& m : value.asObject().values) {
            if (!first) out += ',';
            first = false;
            out += quote(m.key);
            out += ':';
            serializeInto(*m.value, out);
        }
        out += '}';
        return;
    }
    }
}

}  // namespace

std::string toJson(const JsonValue& value) {
    std::string out;
    serializeInto(value, out);
    return out;
}

std::string str(const JsonValue& object, const char* key, const std::string& fallback) {
    const auto* v = object.find(key);
    return v != nullptr && v->isString() ? v->asString() : fallback;
}

bool boolean(const JsonValue& object, const char* key, bool fallback) {
    const auto* v = object.find(key);
    return v != nullptr && v->isBool() ? v->asBool() : fallback;
}

bool integer(const JsonValue& object, const char* key, std::int64_t* out) {
    const auto* v = object.find(key);
    if (v == nullptr || !v->isNumber()) return false;
    const double d = v->asNumber();
    if (!std::isfinite(d) || std::floor(d) != d || std::fabs(d) > kMaxSafeInteger) return false;
    *out = static_cast<std::int64_t>(d);
    return true;
}

std::vector<std::string> strings(const JsonValue& object, const char* key) {
    std::vector<std::string> out;
    const auto* v = object.find(key);
    if (v == nullptr || !v->isArray()) return out;
    for (const auto& item : v->asArray().values) {
        if (item->isString()) out.push_back(item->asString());
    }
    return out;
}

bool hasKey(const JsonValue& object, const char* key) {
    return object.find(key) != nullptr;
}

std::string escape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(ch);
            }
        }
    }
    return out;
}

std::string quote(const std::string& value) {
    return "\"" + escape(value) + "\"";
}

std::string number(double value) {
    if (!std::isfinite(value)) return "null";
    if (std::floor(value) == value && std::fabs(value) <= kMaxSafeInteger) {
        return std::to_string(static_cast<long long>(value == 0 ? 0.0 : value));
    }
    char buf[40];
    for (int precision = 15; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*g", precision, value);
        if (std::strtod(buf, nullptr) == value) break;
    }
    return buf;
}

void Writer::separator() {
    if (afterKey_) {
        afterKey_ = false;
        return;
    }
    if (!first_.empty()) {
        if (!first_.back()) out_ += ',';
        first_.back() = false;
    }
}

Writer& Writer::beginObject() {
    separator();
    out_ += '{';
    first_.push_back(true);
    return *this;
}

Writer& Writer::endObject() {
    out_ += '}';
    first_.pop_back();
    return *this;
}

Writer& Writer::beginArray() {
    separator();
    out_ += '[';
    first_.push_back(true);
    return *this;
}

Writer& Writer::endArray() {
    out_ += ']';
    first_.pop_back();
    return *this;
}

Writer& Writer::key(const std::string& name) {
    separator();
    out_ += quote(name);
    out_ += ':';
    afterKey_ = true;
    return *this;
}

Writer& Writer::str(const std::string& value) {
    separator();
    out_ += quote(value);
    return *this;
}

Writer& Writer::num(double value) {
    separator();
    out_ += number(value);
    return *this;
}

Writer& Writer::integer(std::int64_t value) {
    separator();
    out_ += std::to_string(static_cast<long long>(value));
    return *this;
}

Writer& Writer::boolean(bool value) {
    separator();
    out_ += value ? "true" : "false";
    return *this;
}

Writer& Writer::null() {
    separator();
    out_ += "null";
    return *this;
}

Writer& Writer::raw(const std::string& json) {
    separator();
    out_ += json;
    return *this;
}

Writer& Writer::fieldStrings(const std::string& name, const std::vector<std::string>& values) {
    key(name).beginArray();
    for (const auto& v : values) str(v);
    return endArray();
}

std::string errorBody(const std::string& code, const std::string& message) {
    Writer w;
    w.beginObject().fieldBool("success", false).field("code", code).field("message", message.empty() ? code : message).endObject();
    return w.text();
}

std::string base64Encode(const std::string& bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const std::uint32_t n = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                                static_cast<unsigned char>(bytes[i + 2]);
        out += kB64[(n >> 18) & 63];
        out += kB64[(n >> 12) & 63];
        out += kB64[(n >> 6) & 63];
        out += kB64[n & 63];
    }
    if (i + 1 == bytes.size()) {
        const std::uint32_t n = static_cast<unsigned char>(bytes[i]) << 16;
        out += kB64[(n >> 18) & 63];
        out += kB64[(n >> 12) & 63];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const std::uint32_t n = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8);
        out += kB64[(n >> 18) & 63];
        out += kB64[(n >> 12) & 63];
        out += kB64[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

bool base64Decode(const std::string& text, std::string* out) {
    return decodeBase64(text, false, out);
}

bool base64UrlDecode(const std::string& text, std::string* out) {
    return decodeBase64(text, true, out);
}

std::string toHex(const std::string& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

}  // namespace maintenance_v2
}  // namespace edge_gateway
