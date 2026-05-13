#include "edge_gateway/legacy_ems_point_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <iconv.h>
#endif

namespace edge_gateway {

namespace {

std::string readFile(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open legacy EMS XML file: " + path);
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

#ifdef _WIN32
std::wstring toWide(const std::string& text, UINT codePage) {
    if (text.empty()) {
        return std::wstring();
    }
    const int wideLength = MultiByteToWideChar(
        codePage,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0
    );
    if (wideLength <= 0) {
        throw std::runtime_error("failed to decode legacy EMS XML text");
    }
    std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
    MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), &wide[0], wideLength);
    return wide;
}

std::string wideToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return std::string();
    }
    const int utf8Length = WideCharToMultiByte(
        CP_UTF8,
        0,
        wide.data(),
        static_cast<int>(wide.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (utf8Length <= 0) {
        throw std::runtime_error("failed to encode legacy EMS XML text as UTF-8");
    }
    std::string utf8(static_cast<std::size_t>(utf8Length), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        wide.data(),
        static_cast<int>(wide.size()),
        &utf8[0],
        utf8Length,
        nullptr,
        nullptr
    );
    return utf8;
}
#else
std::string iconvConvert(const std::string& text, const char* fromCode) {
    if (text.empty()) {
        return std::string();
    }
    iconv_t cd = iconv_open("UTF-8", fromCode);
    if (cd == reinterpret_cast<iconv_t>(-1)) {
        throw std::runtime_error(std::string("failed to open iconv for ") + fromCode);
    }

    std::string output(text.size() * 4 + 16, '\0');
    char* in = const_cast<char*>(text.data());
    std::size_t inBytes = text.size();
    char* out = &output[0];
    std::size_t outBytes = output.size();

    while (inBytes > 0) {
        const auto rc = iconv(cd, &in, &inBytes, &out, &outBytes);
        if (rc != static_cast<std::size_t>(-1)) {
            continue;
        }
        if (errno == E2BIG) {
            const auto used = static_cast<std::size_t>(out - output.data());
            output.resize(output.size() * 2);
            out = &output[0] + used;
            outBytes = output.size() - used;
            continue;
        }
        const std::string message = std::string("failed to convert legacy EMS XML text: ") + std::strerror(errno);
        iconv_close(cd);
        throw std::runtime_error(message);
    }

    output.resize(static_cast<std::size_t>(out - &output[0]));
    iconv_close(cd);
    return output;
}
#endif

std::string decodeText(const std::string& text, const std::string& encoding) {
    const auto normalized = lowerAscii(encoding.empty() ? std::string("gbk") : encoding);
    if (normalized == "utf-8" || normalized == "utf8") {
        return text;
    }
#ifdef _WIN32
    if (normalized == "gbk" || normalized == "gb2312" || normalized == "cp936") {
        return wideToUtf8(toWide(text, 936));
    }
    return wideToUtf8(toWide(text, CP_ACP));
#else
    if (normalized == "gbk" || normalized == "gb2312" || normalized == "cp936") {
        return iconvConvert(text, "GBK");
    }
    return iconvConvert(text, encoding.c_str());
#endif
}

std::string xmlUnescape(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '&') {
            result.push_back(value[i]);
            continue;
        }
        if (value.compare(i, 5, "&amp;") == 0) {
            result.push_back('&');
            i += 4;
        } else if (value.compare(i, 6, "&quot;") == 0) {
            result.push_back('"');
            i += 5;
        } else if (value.compare(i, 6, "&apos;") == 0) {
            result.push_back('\'');
            i += 5;
        } else if (value.compare(i, 4, "&lt;") == 0) {
            result.push_back('<');
            i += 3;
        } else if (value.compare(i, 4, "&gt;") == 0) {
            result.push_back('>');
            i += 3;
        } else {
            result.push_back(value[i]);
        }
    }
    return result;
}

std::unordered_map<std::string, std::string> parseAttributes(const std::string& tag) {
    std::unordered_map<std::string, std::string> attrs;
    std::size_t pos = 0;
    while (pos < tag.size()) {
        while (pos < tag.size() && std::isspace(static_cast<unsigned char>(tag[pos])) != 0) {
            ++pos;
        }
        const auto keyStart = pos;
        while (pos < tag.size() &&
               (std::isalnum(static_cast<unsigned char>(tag[pos])) != 0 || tag[pos] == '_' || tag[pos] == '-')) {
            ++pos;
        }
        if (keyStart == pos) {
            ++pos;
            continue;
        }
        const auto key = tag.substr(keyStart, pos - keyStart);
        while (pos < tag.size() && std::isspace(static_cast<unsigned char>(tag[pos])) != 0) {
            ++pos;
        }
        if (pos >= tag.size() || tag[pos] != '=') {
            continue;
        }
        ++pos;
        while (pos < tag.size() && std::isspace(static_cast<unsigned char>(tag[pos])) != 0) {
            ++pos;
        }
        if (pos >= tag.size() || (tag[pos] != '"' && tag[pos] != '\'')) {
            continue;
        }
        const char quote = tag[pos++];
        const auto valueStart = pos;
        while (pos < tag.size() && tag[pos] != quote) {
            ++pos;
        }
        attrs[key] = xmlUnescape(tag.substr(valueStart, pos - valueStart));
        if (pos < tag.size()) {
            ++pos;
        }
    }
    return attrs;
}

std::string attr(const std::unordered_map<std::string, std::string>& attrs, const char* key) {
    const auto it = attrs.find(key);
    return it == attrs.end() ? std::string() : it->second;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

bool isWritable(const std::string& iolink) {
    return contains(iolink, "读写") || contains(lowerAscii(iolink), "write");
}

bool isReadable(const std::string& iolink) {
    return !contains(iolink, "只写");
}

void loadXmlIntoCatalog(
    LegacyEmsPointCatalog& catalog,
    const std::string& filePath,
    LegacyEmsPointSource source,
    const std::string& encoding
) {
    const auto text = decodeText(readFile(filePath), encoding);
    std::size_t pos = 0;
    while (true) {
        const auto start = text.find("<Data", pos);
        if (start == std::string::npos) {
            break;
        }
        const auto end = text.find('>', start);
        if (end == std::string::npos) {
            throw std::runtime_error("malformed legacy EMS XML Data tag in: " + filePath);
        }
        const auto tag = text.substr(start + 5, end - (start + 5));
        const auto attrs = parseAttributes(tag);
        const auto indexText = attr(attrs, "index");
        if (!indexText.empty()) {
            LegacyEmsPoint point;
            point.source = source;
            point.index = static_cast<std::uint32_t>(std::stoul(indexText));
            point.name = attr(attrs, "name");
            point.desc = attr(attrs, "desc");
            point.note = attr(attrs, "note");
            point.func = attr(attrs, "func");
            point.unit = attr(attrs, "unit");
            point.type = attr(attrs, "type");
            point.flash = attr(attrs, "flash");
            point.zone = attr(attrs, "zone");
            point.iolink = attr(attrs, "iolink");
            point.almstr = attr(attrs, "almstr");
            point.readable = isReadable(point.iolink);
            point.writable = isWritable(point.iolink);
            catalog.addPoint(point);
        }
        pos = end + 1;
    }
}

}  // namespace

LegacyEmsPointCatalog LegacyEmsPointCatalog::loadFromFiles(
    const std::string& glListFile,
    const std::string& varListFile,
    const std::string& encoding
) {
    LegacyEmsPointCatalog catalog;
    if (!glListFile.empty()) {
        loadXmlIntoCatalog(catalog, glListFile, LegacyEmsPointSource::Global, encoding);
    }
    if (!varListFile.empty()) {
        loadXmlIntoCatalog(catalog, varListFile, LegacyEmsPointSource::Variable, encoding);
    }
    return catalog;
}

Optional<LegacyEmsPoint> LegacyEmsPointCatalog::findByIndex(std::uint32_t index) const {
    const auto it = pointsByIndex_.find(index);
    if (it == pointsByIndex_.end()) {
        return NullOpt;
    }
    return it->second;
}

std::size_t LegacyEmsPointCatalog::size() const {
    return pointsByIndex_.size();
}

std::vector<LegacyEmsPoint> LegacyEmsPointCatalog::points() const {
    std::vector<LegacyEmsPoint> result;
    result.reserve(pointsByIndex_.size());
    for (const auto& entry : pointsByIndex_) {
        result.push_back(entry.second);
    }
    std::sort(result.begin(), result.end(), [](const LegacyEmsPoint& lhs, const LegacyEmsPoint& rhs) {
        return lhs.index < rhs.index;
    });
    return result;
}

void LegacyEmsPointCatalog::addPoint(const LegacyEmsPoint& point) {
    if (point.index == 0) {
        return;
    }
    pointsByIndex_[point.index] = point;
}

}  // namespace edge_gateway
