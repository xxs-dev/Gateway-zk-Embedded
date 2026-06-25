#include "edge_gateway/priority_control_lease.hpp"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

#ifndef _WIN32
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace edge_gateway {

namespace {

std::string escapeJson(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const auto ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

std::size_t skipWhitespace(const std::string& text, std::size_t pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
    return pos;
}

std::string readTextFile(const std::string& path, std::size_t maxBytes) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) {
        return std::string();
    }
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > maxBytes) {
        return std::string();
    }
    input.seekg(0, std::ios::beg);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::int64_t extractInt64Field(const std::string& text, const char* key, std::int64_t fallback) {
    const std::string needle = std::string("\"") + key + "\"";
    auto pos = text.find(needle);
    if (pos == std::string::npos) {
        return fallback;
    }
    pos = skipWhitespace(text, pos + needle.size());
    if (pos >= text.size() || text[pos] != ':') {
        return fallback;
    }
    pos = skipWhitespace(text, pos + 1);
    const auto begin = pos;
    if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) {
        ++pos;
    }
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
    if (begin == pos) {
        return fallback;
    }
    try {
        return std::stoll(text.substr(begin, pos - begin));
    } catch (...) {
        return fallback;
    }
}

std::uint32_t extractUInt32Field(const std::string& text, const char* key, std::uint32_t fallback) {
    const auto value = extractInt64Field(text, key, static_cast<std::int64_t>(fallback));
    if (value < 0) {
        return fallback;
    }
    return static_cast<std::uint32_t>(value);
}

std::string extractStringField(const std::string& text, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    auto pos = text.find(needle);
    if (pos == std::string::npos) {
        return std::string();
    }
    pos = skipWhitespace(text, pos + needle.size());
    if (pos >= text.size() || text[pos] != ':') {
        return std::string();
    }
    pos = skipWhitespace(text, pos + 1);
    if (pos >= text.size() || text[pos] != '"') {
        return std::string();
    }
    ++pos;
    std::string value;
    while (pos < text.size()) {
        const char ch = text[pos++];
        if (ch == '"') {
            return value;
        }
        if (ch == '\\' && pos < text.size()) {
            value.push_back(text[pos++]);
        } else {
            value.push_back(ch);
        }
    }
    return std::string();
}

std::string directoryOf(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return std::string();
    }
    return path.substr(0, pos);
}

void ensureDirectory(const std::string& dir) {
    if (dir.empty()) {
        return;
    }
#ifndef _WIN32
    std::string partial;
    for (const auto ch : dir) {
        partial.push_back(ch);
        if (ch == '/') {
            if (partial.size() > 1) {
                mkdir(partial.c_str(), 0775);
            }
        }
    }
    mkdir(dir.c_str(), 0775);
#endif
}

}  // namespace

PriorityControlLease::PriorityControlLease(std::string path, std::string owner)
    : path_(std::move(path)),
      owner_(std::move(owner)) {
}

bool PriorityControlLease::enabled() const {
    return !path_.empty();
}

Optional<PriorityControlLeaseState> PriorityControlLease::activeLease(std::int64_t nowMs) const {
    if (!enabled()) {
        return NullOpt;
    }
    const auto text = readTextFile(path_, 16 * 1024);
    if (text.empty()) {
        return NullOpt;
    }
    const auto expireAtMs = extractInt64Field(text, "expireAtMs", 0);
    if (expireAtMs <= nowMs) {
        return NullOpt;
    }

    PriorityControlLeaseState state;
    state.owner = extractStringField(text, "owner");
    state.cmdId = extractStringField(text, "cmdId");
    state.meterCode = extractStringField(text, "meterCode");
    state.index = extractUInt32Field(text, "index", 0);
    state.createdAtMs = extractInt64Field(text, "createdAtMs", 0);
    state.expireAtMs = expireAtMs;
    return state;
}

bool PriorityControlLease::isBlocked(std::int64_t nowMs, const std::string& owner) const {
    const auto lease = activeLease(nowMs);
    if (!lease) {
        return false;
    }
    const auto currentOwner = owner.empty() ? owner_ : owner;
    return lease->owner.empty() || lease->owner != currentOwner;
}

void PriorityControlLease::acquire(
    const std::string& cmdId,
    const std::string& meterCode,
    std::uint32_t index,
    std::int64_t nowMs,
    int ttlMs
) const {
    if (!enabled()) {
        return;
    }
    ensureDirectory(directoryOf(path_));
    const auto tempPath = path_ + ".tmp";
    std::ofstream output(tempPath.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
    if (!output) {
        throw std::runtime_error("failed to write priority control lease");
    }
    output << "{\"owner\":\"" << escapeJson(owner_)
           << "\",\"cmdId\":\"" << escapeJson(cmdId)
           << "\",\"meterCode\":\"" << escapeJson(meterCode)
           << "\",\"index\":" << index
           << ",\"createdAtMs\":" << nowMs
           << ",\"expireAtMs\":" << (nowMs + ttlMs)
           << "}";
    output.close();
    if (!output) {
        std::remove(tempPath.c_str());
        throw std::runtime_error("failed to flush priority control lease");
    }
    if (std::rename(tempPath.c_str(), path_.c_str()) != 0) {
        std::remove(tempPath.c_str());
        throw std::runtime_error("failed to install priority control lease");
    }
}

void PriorityControlLease::release(const std::string& cmdId) const {
    if (!enabled()) {
        return;
    }
#ifndef _WIN32
    const int fd = open(path_.c_str(), O_RDWR);
    if (fd < 0) {
        return;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return;
    }
    if (!cmdId.empty()) {
        char buf[16 * 1024];
        const auto n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            const std::string text(buf, static_cast<std::size_t>(n));
            if (extractStringField(text, "cmdId") != cmdId) {
                flock(fd, LOCK_UN);
                close(fd);
                return;
            }
        }
    }
    unlink(path_.c_str());
    flock(fd, LOCK_UN);
    close(fd);
#else
    if (!cmdId.empty()) {
        const auto text = readTextFile(path_, 16 * 1024);
        if (!text.empty() && extractStringField(text, "cmdId") != cmdId) {
            return;
        }
    }
    std::remove(path_.c_str());
#endif
}

}  // namespace edge_gateway
