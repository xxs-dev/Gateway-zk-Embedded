#pragma once

// Maintenance channel v2: strict JSON parsing, RFC 8785 (JCS) canonical form for
// field tickets / audit entries, and a small writer that never emits bare NaN.

#include <cstdint>
#include <string>
#include <vector>

#include "edge_gateway/json_value.hpp"

namespace edge_gateway {
namespace maintenance_v2 {

using json::JsonMember;
using json::JsonParser;
using json::JsonValue;

// Strict parse (raw control characters, invalid numbers and bad UTF-8 rejected).
JsonValue parseJson(const std::string& text, std::size_t maxNodes = 200000);

// RFC 8785 canonical JSON. Numbers must be integers within +-2^53 (tickets and
// audit entries only carry integers); anything else throws.
std::string canonicalJson(const JsonValue& value);

// Order-preserving compact serialisation (numbers via number()).
std::string toJson(const JsonValue& value);

// Field accessors (missing / wrong type -> fallback).
std::string str(const JsonValue& object, const char* key, const std::string& fallback = std::string());
bool boolean(const JsonValue& object, const char* key, bool fallback);
bool integer(const JsonValue& object, const char* key, std::int64_t* out);
std::vector<std::string> strings(const JsonValue& object, const char* key);
bool hasKey(const JsonValue& object, const char* key);

std::string escape(const std::string& value);
std::string quote(const std::string& value);
// Integers exactly; other finite values with round-trip precision; NaN/Inf -> null.
std::string number(double value);

// Tiny streaming writer: obj().key("a").value(1).key("b").str("x").end()
class Writer {
public:
    Writer& beginObject();
    Writer& endObject();
    Writer& beginArray();
    Writer& endArray();
    Writer& key(const std::string& name);
    Writer& str(const std::string& value);
    Writer& num(double value);
    Writer& integer(std::int64_t value);
    Writer& boolean(bool value);
    Writer& null();
    Writer& raw(const std::string& json);
    // Convenience: key + value
    Writer& field(const std::string& name, const std::string& value) { return key(name).str(value); }
    Writer& field(const std::string& name, const char* value) { return key(name).str(value); }
    Writer& fieldInt(const std::string& name, std::int64_t value) { return key(name).integer(value); }
    Writer& fieldNum(const std::string& name, double value) { return key(name).num(value); }
    Writer& fieldBool(const std::string& name, bool value) { return key(name).boolean(value); }
    Writer& fieldStrings(const std::string& name, const std::vector<std::string>& values);
    const std::string& text() const { return out_; }

private:
    void separator();
    std::string out_;
    std::vector<bool> first_;
    bool afterKey_ = false;
};

// {"success":false,"code":...,"message":...}
std::string errorBody(const std::string& code, const std::string& message);

std::string base64Encode(const std::string& bytes);
bool base64Decode(const std::string& text, std::string* out);
bool base64UrlDecode(const std::string& text, std::string* out);
std::string toHex(const std::string& bytes);

}  // namespace maintenance_v2
}  // namespace edge_gateway
