#pragma once

#include <string>

namespace edge_gateway {

inline std::string escapeDiagnosticOutputJson(const std::string& value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(value.size() + 8);
    for (std::size_t i = 0; i < value.size();) {
        const auto ch = static_cast<unsigned char>(value[i]);
        if (ch < 0x80) {
            switch (ch) {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (ch < 0x20) {
                        out += "\\u00";
                        out += hex[ch >> 4];
                        out += hex[ch & 0x0f];
                    } else out += static_cast<char>(ch);
                    break;
            }
            ++i;
            continue;
        }
        const auto continuation = [&](std::size_t offset) {
            return i + offset < value.size() &&
                (static_cast<unsigned char>(value[i + offset]) & 0xc0) == 0x80;
        };
        std::size_t length = 0;
        if (ch >= 0xc2 && ch <= 0xdf && continuation(1)) length = 2;
        else if (ch >= 0xe0 && ch <= 0xef && continuation(1) && continuation(2)) {
            const auto second = static_cast<unsigned char>(value[i + 1]);
            if ((ch != 0xe0 || second >= 0xa0) && (ch != 0xed || second < 0xa0)) length = 3;
        } else if (ch >= 0xf0 && ch <= 0xf4 && continuation(1) && continuation(2) && continuation(3)) {
            const auto second = static_cast<unsigned char>(value[i + 1]);
            if ((ch != 0xf0 || second >= 0x90) && (ch != 0xf4 || second < 0x90)) length = 4;
        }
        if (length) {
            out.append(value, i, length);
            i += length;
        } else {
            out += "\\ufffd";
            ++i;
        }
    }
    return out;
}

}  // namespace edge_gateway
