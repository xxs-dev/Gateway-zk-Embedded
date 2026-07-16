#include "edge_gateway/power_control_ownership.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace edge_gateway {

namespace {

std::string escapeJson(const std::string& value) {
    std::string result;
    for (const auto ch : value) {
        if (ch == '\\' || ch == '"') {
            result.push_back('\\');
        }
        result.push_back(ch);
    }
    return result;
}

std::size_t skipWhitespace(const std::string& text, std::size_t pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
    return pos;
}

std::string readText(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) {
        return std::string();
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const auto text = buffer.str();
    return text.size() <= 64 * 1024 ? text : std::string();
}

std::string stringField(const std::string& text, const char* key) {
    const auto needle = std::string("\"") + key + "\"";
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
    std::string result;
    while (pos < text.size()) {
        const auto ch = text[pos++];
        if (ch == '"') {
            return result;
        }
        result.push_back(ch == '\\' && pos < text.size() ? text[pos++] : ch);
    }
    return std::string();
}

std::int64_t intField(const std::string& text, const char* key) {
    const auto needle = std::string("\"") + key + "\"";
    auto pos = text.find(needle);
    if (pos == std::string::npos) {
        return 0;
    }
    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        return 0;
    }
    pos = skipWhitespace(text, pos + 1);
    const auto begin = pos;
    if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) {
        ++pos;
    }
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
    try {
        return std::stoll(text.substr(begin, pos - begin));
    } catch (...) {
        return 0;
    }
}

std::vector<std::uint32_t> indexArray(const std::string& text) {
    std::vector<std::uint32_t> result;
    auto pos = text.find("\"targetIndexes\"");
    pos = pos == std::string::npos ? pos : text.find('[', pos);
    const auto end = pos == std::string::npos ? pos : text.find(']', pos);
    if (pos == std::string::npos || end == std::string::npos) {
        return result;
    }
    ++pos;
    while (pos < end) {
        pos = skipWhitespace(text, pos);
        if (pos < end && text[pos] == ',') {
            ++pos;
            continue;
        }
        const auto begin = pos;
        while (pos < end && std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
            ++pos;
        }
        if (begin == pos) {
            break;
        }
        result.push_back(static_cast<std::uint32_t>(std::stoul(text.substr(begin, pos - begin))));
    }
    return result;
}

Optional<PowerControlOwnershipState> parseState(const std::string& text, std::int64_t nowMs) {
    if (text.empty()) {
        return NullOpt;
    }
    PowerControlOwnershipState state;
    state.scope = stringField(text, "scope");
    state.owner = stringField(text, "owner");
    state.sessionId = stringField(text, "sessionId");
    state.heartbeatAtMs = intField(text, "heartbeatAtMs");
    state.expireAtMs = intField(text, "expireAtMs");
    state.targetIndexes = indexArray(text);
    return state.owner.empty() || state.expireAtMs <= nowMs
        ? Optional<PowerControlOwnershipState>()
        : Optional<PowerControlOwnershipState>(state);
}

std::string directoryOf(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

void ensureDirectory(const std::string& dir) {
#ifndef _WIN32
    std::string partial;
    for (const auto ch : dir) {
        partial.push_back(ch);
        if (ch == '/' && partial.size() > 1) {
            mkdir(partial.c_str(), 0775);
        }
    }
    if (!dir.empty()) {
        mkdir(dir.c_str(), 0775);
    }
#else
    (void)dir;
#endif
}

class ScopedOwnershipLock {
public:
    ScopedOwnershipLock(const std::string& statePath, bool exclusive) {
#ifndef _WIN32
        const auto lockPath = statePath + ".lock";
        ensureDirectory(directoryOf(lockPath));
        fd_ = ::open(lockPath.c_str(), O_CREAT | O_RDWR, 0664);
        if (fd_ >= 0 && ::flock(fd_, exclusive ? LOCK_EX : LOCK_SH) == 0) {
            locked_ = true;
        }
#else
        (void)statePath;
        (void)exclusive;
        locked_ = true;
#endif
    }

    ~ScopedOwnershipLock() {
#ifndef _WIN32
        if (locked_) {
            ::flock(fd_, LOCK_UN);
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }
#endif
    }

    bool locked() const {
        return locked_;
    }

private:
    int fd_ = -1;
    bool locked_ = false;
};

bool writeState(
    const std::string& path,
    const std::string& scope,
    const std::string& owner,
    const std::string& sessionId,
    const std::vector<std::uint32_t>& indexes,
    std::int64_t nowMs,
    int ttlMs
) {
    ensureDirectory(directoryOf(path));
    const auto temp = path + ".tmp";
    std::ofstream output(temp.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
    if (!output) {
        return false;
    }
    output << "{\"scope\":\"" << escapeJson(scope)
           << "\",\"owner\":\"" << escapeJson(owner)
           << "\",\"sessionId\":\"" << escapeJson(sessionId)
           << "\",\"heartbeatAtMs\":" << nowMs
           << ",\"expireAtMs\":" << (nowMs + std::max(1, ttlMs))
           << ",\"targetIndexes\":[";
    for (std::size_t i = 0; i < indexes.size(); ++i) {
        output << (i == 0 ? "" : ",") << indexes[i];
    }
    output << "]}";
    output.close();
    if (!output || std::rename(temp.c_str(), path.c_str()) != 0) {
        std::remove(temp.c_str());
        return false;
    }
    return true;
}

}  // namespace

PowerControlOwnership::PowerControlOwnership(std::string path, std::string owner)
    : path_(std::move(path)), owner_(std::move(owner)) {
}

bool PowerControlOwnership::enabled() const {
    return !path_.empty();
}

Optional<PowerControlOwnershipState> PowerControlOwnership::active(std::int64_t nowMs) const {
    if (!enabled()) {
        return NullOpt;
    }
    ScopedOwnershipLock lock(path_, false);
    return lock.locked() ? parseState(readText(path_), nowMs) : Optional<PowerControlOwnershipState>();
}

bool PowerControlOwnership::isBlocked(std::uint32_t index, const std::string& source, std::int64_t nowMs) const {
    const auto state = active(nowMs);
    return state && state->owner != source &&
        std::find(state->targetIndexes.begin(), state->targetIndexes.end(), index) != state->targetIndexes.end();
}

bool PowerControlOwnership::acquire(
    const std::string& scope,
    const std::string& sessionId,
    const std::vector<std::uint32_t>& targetIndexes,
    std::int64_t nowMs,
    int ttlMs
) const {
    if (!enabled()) {
        return true;
    }
    ScopedOwnershipLock lock(path_, true);
    if (!lock.locked()) {
        return false;
    }
    const auto current = parseState(readText(path_), nowMs);
    if (current && (current->owner != owner_ || current->sessionId != sessionId)) {
        return false;
    }
    return writeState(path_, scope, owner_, sessionId, targetIndexes, nowMs, ttlMs);
}

bool PowerControlOwnership::renew(const std::string& sessionId, std::int64_t nowMs, int ttlMs) const {
    if (!enabled()) {
        return true;
    }
    ScopedOwnershipLock lock(path_, true);
    if (!lock.locked()) {
        return false;
    }
    const auto current = parseState(readText(path_), nowMs);
    return current && current->owner == owner_ && current->sessionId == sessionId &&
        writeState(path_, current->scope, owner_, sessionId, current->targetIndexes, nowMs, ttlMs);
}

void PowerControlOwnership::release(const std::string& sessionId) const {
    if (!enabled()) {
        return;
    }
    ScopedOwnershipLock lock(path_, true);
    if (!lock.locked()) {
        return;
    }
    const auto current = parseState(readText(path_), 0);
    if (current && current->owner == owner_ && (sessionId.empty() || current->sessionId == sessionId)) {
        std::remove(path_.c_str());
    }
}

}  // namespace edge_gateway
