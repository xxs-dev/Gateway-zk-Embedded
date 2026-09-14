#include "edge_gateway/power_control_ownership.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace edge_gateway {

namespace {

constexpr std::size_t kMaxOwnershipStateBytes = 64 * 1024;
constexpr std::size_t kMaxCommandReceipts = 64;

bool validCommandFingerprint(const std::string& fingerprint, bool allowEmpty) {
    return (allowEmpty && fingerprint.empty()) ||
        (fingerprint.size() == 64 &&
         std::all_of(fingerprint.begin(), fingerprint.end(), [](char ch) {
             return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
         }));
}

std::string escapeJson(const std::string& value) {
    std::string result;
    static const char hex[] = "0123456789abcdef";
    for (const auto raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        switch (ch) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (ch < 0x20U) {
                    result += "\\u00";
                    result.push_back(hex[(ch >> 4) & 0x0FU]);
                    result.push_back(hex[ch & 0x0FU]);
                } else {
                    result.push_back(static_cast<char>(ch));
                }
                break;
        }
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
    return text.size() <= kMaxOwnershipStateBytes ? text : std::string();
}

bool stateFileExists(const std::string& path) {
#ifdef _WIN32
    const auto attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    return static_cast<bool>(input);
#endif
}

unsigned hexDigit(char ch) {
    if (ch >= '0' && ch <= '9') return static_cast<unsigned>(ch - '0');
    if (ch >= 'a' && ch <= 'f') return static_cast<unsigned>(ch - 'a' + 10);
    if (ch >= 'A' && ch <= 'F') return static_cast<unsigned>(ch - 'A' + 10);
    return 0xFFFFFFFFU;
}

void appendUtf8(std::string& output, unsigned codepoint) {
    if (codepoint <= 0x7FU) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint < 0xD800U || codepoint > 0xDFFFU) {
        output.push_back(static_cast<char>(0xE0U | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    }
}

bool parseJsonString(const std::string& text, std::size_t* pos, std::string* value) {
    if (*pos >= text.size() || text[*pos] != '"') {
        return false;
    }
    ++*pos;
    value->clear();
    while (*pos < text.size()) {
        const auto ch = static_cast<unsigned char>(text[(*pos)++]);
        if (ch == '"') {
            return true;
        }
        if (ch < 0x20U) {
            return false;
        }
        if (ch != '\\') {
            value->push_back(static_cast<char>(ch));
            continue;
        }
        if (*pos >= text.size()) {
            return false;
        }
        const auto escaped = text[(*pos)++];
        switch (escaped) {
            case '"': value->push_back('"'); break;
            case '\\': value->push_back('\\'); break;
            case '/': value->push_back('/'); break;
            case 'b': value->push_back('\b'); break;
            case 'f': value->push_back('\f'); break;
            case 'n': value->push_back('\n'); break;
            case 'r': value->push_back('\r'); break;
            case 't': value->push_back('\t'); break;
            case 'u': {
                if (*pos + 4 > text.size()) return false;
                unsigned codepoint = 0;
                for (int i = 0; i < 4; ++i) {
                    const auto digit = hexDigit(text[(*pos)++]);
                    if (digit > 0x0FU) return false;
                    codepoint = (codepoint << 4) | digit;
                }
                if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) return false;
                appendUtf8(*value, codepoint);
                break;
            }
            default: return false;
        }
    }
    return false;
}

std::size_t fieldValuePosition(const std::string& text, const char* key) {
    const auto needle = std::string("\"") + key + "\"";
    auto pos = text.find(needle);
    if (pos == std::string::npos) return pos;
    pos = skipWhitespace(text, pos + needle.size());
    if (pos >= text.size() || text[pos] != ':') return std::string::npos;
    return skipWhitespace(text, pos + 1);
}

std::string stringField(const std::string& text, const char* key) {
    auto pos = fieldValuePosition(text, key);
    std::string result;
    if (pos != std::string::npos && parseJsonString(text, &pos, &result)) return result;
    return std::string();
}

bool parseStringArrayField(
    const std::string& text,
    const char* key,
    std::vector<std::string>* result
) {
    result->clear();
    auto pos = fieldValuePosition(text, key);
    if (pos == std::string::npos || pos >= text.size() || text[pos++] != '[') return false;
    while (pos < text.size()) {
        pos = skipWhitespace(text, pos);
        if (pos < text.size() && text[pos] == ']') return true;
        std::string value;
        if (!parseJsonString(text, &pos, &value)) return false;
        result->push_back(std::move(value));
        pos = skipWhitespace(text, pos);
        if (pos < text.size() && text[pos] == ']') return true;
        if (pos >= text.size() || text[pos++] != ',') return false;
        pos = skipWhitespace(text, pos);
        if (pos >= text.size() || text[pos] == ']') return false;
    }
    return false;
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

bool validIntegerField(const std::string& text, const char* key, bool required) {
    auto pos = fieldValuePosition(text, key);
    if (pos == std::string::npos) return !required;
    if (pos < text.size() && text[pos] == '-') ++pos;
    const auto begin = pos;
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
    return begin != pos;
}

bool validStringField(const std::string& text, const char* key) {
    auto pos = fieldValuePosition(text, key);
    std::string ignored;
    return pos != std::string::npos && parseJsonString(text, &pos, &ignored);
}

bool validArrayField(const std::string& text, const char* key, bool required) {
    const auto pos = fieldValuePosition(text, key);
    if (pos == std::string::npos) return !required;
    if (pos >= text.size() || text[pos] != '[') return false;
    return text.find(']', pos + 1) != std::string::npos;
}

bool validStateEnvelope(const std::string& text) {
    const auto begin = skipWhitespace(text, 0);
    if (begin >= text.size() || text[begin] != '{') return false;
    auto end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
    if (end <= begin || text[end - 1] != '}') return false;
    return validStringField(text, "scope") &&
        validStringField(text, "owner") &&
        validStringField(text, "sessionId") &&
        validIntegerField(text, "heartbeatAtMs", true) &&
        validIntegerField(text, "expireAtMs", true) &&
        validArrayField(text, "targetIndexes", true) &&
        validIntegerField(text, "generation", false) &&
        validArrayField(text, "receipts", false);
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

Optional<PowerControlOwnershipState> parseState(const std::string& text) {
    if (text.empty() || !validStateEnvelope(text)) {
        return NullOpt;
    }
    try {
        PowerControlOwnershipState state;
        state.scope = stringField(text, "scope");
        state.owner = stringField(text, "owner");
        state.sessionId = stringField(text, "sessionId");
        state.heartbeatAtMs = intField(text, "heartbeatAtMs");
        state.expireAtMs = intField(text, "expireAtMs");
        state.targetIndexes = indexArray(text);
        const auto generation = intField(text, "generation");
        if (generation < 0 ||
            static_cast<std::uint64_t>(generation) >
                static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
            return NullOpt;
        }
        state.generation = static_cast<std::uint32_t>(generation);
        state.lastCommandId = stringField(text, "lastCommandId");
        std::vector<std::string> encodedReceipts;
        const bool hasReceiptField = fieldValuePosition(text, "receipts") != std::string::npos;
        if (hasReceiptField && !parseStringArrayField(text, "receipts", &encodedReceipts)) {
            return NullOpt;
        }
        std::vector<std::string> receiptFingerprints;
        const bool hasFingerprintField =
            fieldValuePosition(text, "receiptFingerprints") != std::string::npos;
        if (hasFingerprintField &&
            (!parseStringArrayField(text, "receiptFingerprints", &receiptFingerprints) ||
             receiptFingerprints.size() != encodedReceipts.size())) {
            return NullOpt;
        }
        for (std::size_t receiptIndex = 0; receiptIndex < encodedReceipts.size(); ++receiptIndex) {
            const auto& encoded = encodedReceipts[receiptIndex];
            if (encoded.size() < 2 || encoded[1] != ':' ||
                (encoded[0] != '1' && encoded[0] != '0') ||
                encoded.size() - 2 > 63) {
                return NullOpt;
            }
            const auto id = encoded.substr(2);
            const auto fingerprint = hasFingerprintField
                ? receiptFingerprints[receiptIndex]
                : std::string();
            if (!validCommandFingerprint(fingerprint, true)) {
                return NullOpt;
            }
            if (id.empty() || std::find_if(
                    state.receipts.begin(),
                    state.receipts.end(),
                    [&id](const PowerControlCommandReceipt& receipt) {
                        return receipt.id == id;
                    }
                ) != state.receipts.end()) {
                return NullOpt;
            }
            state.receipts.push_back({id, encoded[0] == '1', fingerprint});
        }
        if (!hasReceiptField && state.receipts.empty() && !state.lastCommandId.empty()) {
            state.receipts.push_back({state.lastCommandId, true, std::string()});
        }
        if (state.receipts.size() > kMaxCommandReceipts) {
            state.receipts.erase(
                state.receipts.begin(),
                state.receipts.end() - static_cast<std::ptrdiff_t>(kMaxCommandReceipts)
            );
        }
        return Optional<PowerControlOwnershipState>(state);
    } catch (...) {
        return NullOpt;
    }
}

Optional<PowerControlCommandReceipt> findReceipt(
    const PowerControlOwnershipState& state,
    const std::string& commandId
) {
    const auto it = std::find_if(
        state.receipts.rbegin(),
        state.receipts.rend(),
        [&commandId](const PowerControlCommandReceipt& receipt) {
            return receipt.id == commandId;
        }
    );
    return it == state.receipts.rend()
        ? Optional<PowerControlCommandReceipt>()
        : Optional<PowerControlCommandReceipt>(*it);
}

void upsertReceipt(
    PowerControlOwnershipState& state,
    const std::string& commandId,
    bool accepted,
    const std::string& fingerprint
) {
    state.receipts.erase(
        std::remove_if(
            state.receipts.begin(),
            state.receipts.end(),
            [&commandId](const PowerControlCommandReceipt& receipt) {
                return receipt.id == commandId;
            }
        ),
        state.receipts.end()
    );
    state.receipts.push_back({commandId, accepted, fingerprint});
    if (state.receipts.size() > kMaxCommandReceipts) {
        state.receipts.erase(
            state.receipts.begin(),
            state.receipts.begin() +
                static_cast<std::ptrdiff_t>(state.receipts.size() - kMaxCommandReceipts)
        );
    }
}

bool isActiveState(const PowerControlOwnershipState& state, std::int64_t nowMs) {
    return !state.owner.empty() && state.expireAtMs > nowMs;
}

std::uint32_t nextGeneration(std::uint32_t generation) {
    ++generation;
    return generation == 0 ? 1U : generation;
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
        const auto lockPath = statePath + ".lock";
        handle_ = CreateFileA(
            lockPath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        // The state document is small and every operation is short-lived. An
        // exclusive file handle gives Windows the same whole-file semantics as
        // flock without relying on byte-range locks on an empty lock file.
        (void)exclusive;
        locked_ = handle_ != INVALID_HANDLE_VALUE;
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
#else
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
#endif
    }

    bool locked() const {
        return locked_;
    }

private:
#ifndef _WIN32
    int fd_ = -1;
#else
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#endif
    bool locked_ = false;
};

bool writeState(
    const std::string& path,
    const PowerControlOwnershipState& state
) {
    ensureDirectory(directoryOf(path));
    std::ostringstream payload;
    payload << "{\"scope\":\"" << escapeJson(state.scope)
            << "\",\"owner\":\"" << escapeJson(state.owner)
            << "\",\"sessionId\":\"" << escapeJson(state.sessionId)
            << "\",\"heartbeatAtMs\":" << state.heartbeatAtMs
            << ",\"expireAtMs\":" << state.expireAtMs
            << ",\"targetIndexes\":[";
    for (std::size_t i = 0; i < state.targetIndexes.size(); ++i) {
        payload << (i == 0 ? "" : ",") << state.targetIndexes[i];
    }
    payload << "],\"generation\":" << state.generation
            << ",\"lastCommandId\":\"" << escapeJson(state.lastCommandId)
            << "\",\"receipts\":[";
    const auto receiptBegin = state.receipts.size() > kMaxCommandReceipts
        ? state.receipts.size() - kMaxCommandReceipts
        : 0;
    for (std::size_t i = receiptBegin; i < state.receipts.size(); ++i) {
        payload << (i == receiptBegin ? "" : ",")
                << "\"" << (state.receipts[i].accepted ? "1:" : "0:")
                << escapeJson(state.receipts[i].id) << "\"";
    }
    payload << "],\"receiptFingerprints\":[";
    for (std::size_t i = receiptBegin; i < state.receipts.size(); ++i) {
        payload << (i == receiptBegin ? "" : ",")
                << "\"" << escapeJson(state.receipts[i].fingerprint) << "\"";
    }
    payload << "]}";
    const auto text = payload.str();
    if (text.size() > kMaxOwnershipStateBytes) {
        return false;
    }

    const auto temp = path + ".tmp";
    std::ofstream output(temp.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
    if (!output) {
        return false;
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    if (!output) {
        output.close();
        std::remove(temp.c_str());
        return false;
    }
    output.close();
#ifndef _WIN32
    const int tempFd = ::open(temp.c_str(), O_RDONLY);
    if (tempFd < 0 || ::fsync(tempFd) != 0) {
        if (tempFd >= 0) ::close(tempFd);
        std::remove(temp.c_str());
        return false;
    }
    ::close(tempFd);
    if (std::rename(temp.c_str(), path.c_str()) != 0) {
        std::remove(temp.c_str());
        return false;
    }
#else
    if (!MoveFileExA(
            temp.c_str(),
            path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::remove(temp.c_str());
        return false;
    }
#endif
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
    if (!lock.locked()) {
        return Optional<PowerControlOwnershipState>();
    }
    const auto state = parseState(readText(path_));
    return state && isActiveState(*state, nowMs)
        ? state
        : Optional<PowerControlOwnershipState>();
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
    const auto text = readText(path_);
    const auto current = parseState(text);
    if (!current && stateFileExists(path_)) {
        return false;
    }
    if (current && isActiveState(*current, nowMs) &&
        (current->owner != owner_ || current->sessionId != sessionId)) {
        return false;
    }
    PowerControlOwnershipState next;
    if (current) {
        next = *current;
    }
    next.scope = scope;
    next.owner = owner_;
    next.sessionId = sessionId;
    next.targetIndexes = targetIndexes;
    next.heartbeatAtMs = nowMs;
    next.expireAtMs = nowMs + std::max(1, ttlMs);
    return writeState(path_, next);
}

bool PowerControlOwnership::renew(const std::string& sessionId, std::int64_t nowMs, int ttlMs) const {
    if (!enabled()) {
        return true;
    }
    ScopedOwnershipLock lock(path_, true);
    if (!lock.locked()) {
        return false;
    }
    const auto current = parseState(readText(path_));
    if (!current || !isActiveState(*current, nowMs) ||
        current->owner != owner_ || current->sessionId != sessionId) {
        return false;
    }
    auto next = *current;
    next.heartbeatAtMs = nowMs;
    next.expireAtMs = nowMs + std::max(1, ttlMs);
    return writeState(path_, next);
}

void PowerControlOwnership::release(const std::string& sessionId) const {
    if (!enabled()) {
        return;
    }
    ScopedOwnershipLock lock(path_, true);
    if (!lock.locked()) {
        return;
    }
    const auto text = readText(path_);
    const auto current = parseState(text);
    if (!current && stateFileExists(path_)) {
        return;
    }
    if (current && current->owner == owner_ && (sessionId.empty() || current->sessionId == sessionId)) {
        auto next = *current;
        next.owner.clear();
        next.sessionId.clear();
        next.heartbeatAtMs = 0;
        next.expireAtMs = 0;
        next.generation = nextGeneration(next.generation);
        writeState(path_, next);
    }
}

PowerControlTakeoverResult PowerControlOwnership::acquireOrRenew(
    const std::string& scope,
    const std::string& sessionId,
    const std::vector<std::uint32_t>& targetIndexes,
    const std::string& commandId,
    std::int64_t nowMs,
    int ttlMs,
    const std::string& commandFingerprint
) const {
    PowerControlTakeoverResult result;
    if (!validCommandFingerprint(commandFingerprint, true)) {
        result.message = "invalid command fingerprint";
        return result;
    }
    if (!enabled()) {
        result.accepted = true;
        result.message = "ownership disabled";
        return result;
    }
    ScopedOwnershipLock lock(path_, true);
    if (!lock.locked()) {
        result.message = "failed to lock ownership state";
        return result;
    }

    const auto text = readText(path_);
    const auto current = parseState(text);
    if (!current && stateFileExists(path_)) {
        result.message = "invalid ownership state";
        return result;
    }
    if (current && !commandId.empty()) {
        const auto receipt = findReceipt(*current, commandId);
        if (receipt) {
            result.duplicate = true;
            result.generation = current->generation;
            if (receipt->fingerprint != commandFingerprint) {
                result.accepted = false;
                result.message = "command id payload mismatch";
                return result;
            }
            result.accepted = receipt->accepted;
            result.message = receipt->accepted
                ? "duplicate accepted command"
                : "duplicate incomplete or rejected command";
            return result;
        }
    }
    if (current && isActiveState(*current, nowMs)) {
        result.generation = current->generation;
        if (current->owner != owner_ || current->sessionId != sessionId) {
            result.message = "power control is owned by another controller";
            return result;
        }
        auto next = *current;
        next.scope = scope;
        next.targetIndexes = targetIndexes;
        next.heartbeatAtMs = nowMs;
        next.expireAtMs = nowMs + std::max(1, ttlMs);
        next.lastCommandId = commandId;
        if (!commandId.empty()) {
            upsertReceipt(next, commandId, false, commandFingerprint);
        }
        if (next.generation == 0) {
            next.generation = 1;
        }
        if (!writeState(path_, next)) {
            result.message = "failed to persist ownership state";
            return result;
        }
        result.accepted = true;
        result.generation = next.generation;
        result.message = "renewed";
        return result;
    }

    PowerControlOwnershipState next;
    if (current) {
        next = *current;
    }
    next.scope = scope;
    next.owner = owner_;
    next.sessionId = sessionId;
    next.targetIndexes = targetIndexes;
    next.heartbeatAtMs = nowMs;
    next.expireAtMs = nowMs + std::max(1, ttlMs);
    next.generation = nextGeneration(current ? current->generation : 0);
    next.lastCommandId = commandId;
    if (!commandId.empty()) {
        upsertReceipt(next, commandId, false, commandFingerprint);
    }
    if (!writeState(path_, next)) {
        result.message = "failed to persist ownership state";
        return result;
    }
    result.accepted = true;
    result.generation = next.generation;
    result.message = "acquired";
    return result;
}

PowerControlReceiptLookup PowerControlOwnership::lookupReceipt(
    const std::string& commandId
) const {
    PowerControlReceiptLookup result;
    if (!enabled() || commandId.empty()) {
        return result;
    }
    ScopedOwnershipLock lock(path_, false);
    if (!lock.locked()) {
        return result;
    }
    const auto text = readText(path_);
    const auto current = parseState(text);
    if (!current && stateFileExists(path_)) {
        return result;
    }
    if (!current) {
        return result;
    }
    const auto receipt = findReceipt(*current, commandId);
    if (receipt) {
        result.found = true;
        result.accepted = receipt->accepted;
        result.fingerprint = receipt->fingerprint;
    }
    return result;
}

bool PowerControlOwnership::recordReceipt(
    const std::string& sessionId,
    const std::string& commandId,
    std::uint32_t generation,
    bool accepted,
    const std::string& commandFingerprint
) const {
    if (!enabled()) {
        return true;
    }
    if (commandId.empty() || generation == 0 ||
        !validCommandFingerprint(commandFingerprint, true)) {
        return false;
    }
    ScopedOwnershipLock lock(path_, true);
    if (!lock.locked()) {
        return false;
    }
    const auto text = readText(path_);
    const auto current = parseState(text);
    if (!current && stateFileExists(path_)) {
        return false;
    }
    if (!current || current->owner != owner_ || current->sessionId != sessionId ||
        current->generation != generation) {
        return false;
    }
    auto next = *current;
    const auto receipt = findReceipt(next, commandId);
    if (!receipt) {
        return false;
    }
    if (receipt->fingerprint != commandFingerprint) {
        return false;
    }
    upsertReceipt(next, commandId, accepted, receipt->fingerprint);
    return writeState(path_, next);
}

bool PowerControlOwnership::recordDetachedReceipt(
    const std::string& commandId,
    bool accepted,
    const std::string& commandFingerprint
) const {
    if (!enabled()) {
        return true;
    }
    if (commandId.empty() || !validCommandFingerprint(commandFingerprint, false)) {
        return false;
    }
    ScopedOwnershipLock lock(path_, true);
    if (!lock.locked()) {
        return false;
    }
    const auto text = readText(path_);
    const auto current = parseState(text);
    if (!current && stateFileExists(path_)) {
        return false;
    }
    PowerControlOwnershipState next;
    if (current) {
        next = *current;
        const auto receipt = findReceipt(next, commandId);
        if (receipt && receipt->fingerprint != commandFingerprint) {
            return false;
        }
    }
    next.lastCommandId = commandId;
    upsertReceipt(next, commandId, accepted, commandFingerprint);
    return writeState(path_, next);
}

std::uint32_t PowerControlOwnership::releaseAndAdvance(const std::string& sessionId) const {
    if (!enabled()) {
        return 0;
    }
    ScopedOwnershipLock lock(path_, true);
    if (!lock.locked()) {
        return 0;
    }
    const auto text = readText(path_);
    const auto current = parseState(text);
    if (!current && stateFileExists(path_)) {
        return 0;
    }
    if (!current || current->owner != owner_ || current->sessionId != sessionId) {
        return 0;
    }
    auto next = *current;
    next.owner.clear();
    next.sessionId.clear();
    next.heartbeatAtMs = 0;
    next.expireAtMs = 0;
    next.generation = nextGeneration(next.generation);
    return writeState(path_, next) ? next.generation : 0;
}

PowerControlAuthorizationResult PowerControlOwnership::authorize(
    std::uint32_t index,
    const std::string& source,
    std::uint32_t commandGeneration,
    bool highPriority,
    std::int64_t nowMs
) const {
    PowerControlAuthorizationResult result;
    if (highPriority) {
        result.allowed = true;
        result.message = "high priority";
        return result;
    }
    if (!enabled()) {
        result.allowed = true;
        result.message = "ownership disabled";
        return result;
    }
    ScopedOwnershipLock lock(path_, false);
    if (!lock.locked()) {
        result.message = "failed to lock ownership state";
        return result;
    }
    const auto text = readText(path_);
    const auto current = parseState(text);
    result.generation = current ? current->generation : 0;
    if (!current) {
        if (stateFileExists(path_)) {
            result.message = "invalid ownership state";
            return result;
        }
        if (source == owner_) {
            result.message = "third-party control requires an active generation";
            return result;
        }
        result.allowed = true;
        result.message = "no ownership state";
        return result;
    }

    const bool targeted = std::find(
        current->targetIndexes.begin(),
        current->targetIndexes.end(),
        index
    ) != current->targetIndexes.end();
    if (isActiveState(*current, nowMs)) {
        if (source == owner_ && !targeted) {
            result.message = "third-party index is outside the active scope";
        } else if (!targeted) {
            result.allowed = true;
            result.generation = 0;
            result.message = "index is outside the active scope";
        } else if (source != current->owner) {
            result.message = "active target requires the current owner";
        } else if (commandGeneration == current->generation) {
            result.allowed = true;
            result.message = "owner and generation match";
        } else {
            result.message = "active target requires the current generation";
        }
        return result;
    }
    if (source == owner_) {
        result.message = commandGeneration == 0
            ? "third-party control requires an active generation"
            : "third-party control generation is inactive";
        return result;
    }
    if (commandGeneration == 0) {
        result.allowed = true;
        result.generation = 0;
        result.message = "legacy generation";
        return result;
    }
    if (targeted) {
        result.message = commandGeneration == current->generation
            ? "control lease has expired"
            : "stale control generation";
        return result;
    }
    result.allowed = true;
    result.generation = 0;
    result.message = "inactive ownership";
    return result;
}

}  // namespace edge_gateway
