#include "edge_gateway/ota_service.hpp"
#include "edge_gateway/json_value.hpp"
#include "edge_gateway/ota_query_crypto.hpp"
#include "ota_query_sha256.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace edge_gateway {

namespace {

std::string quoteArg(const std::string& value) {
#ifndef _WIN32
    std::string escaped = "'";
    for (const auto ch : value) {
        if (ch == '\'') {
            escaped += "'\\''";
        } else {
            escaped.push_back(ch);
        }
    }
    escaped += "'";
    return escaped;
#else
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const auto ch : value) {
        if (ch == '"') {
            escaped += "\\\"";
        } else if (ch == '\\') {
            escaped += "\\\\";
        } else {
            escaped.push_back(ch);
        }
    }
    return std::string("\"") + escaped + "\"";
#endif
}

int makeDir(const std::string& path) {
#ifdef _WIN32
    return _mkdir(path.c_str());
#else
    return mkdir(path.c_str(), 0755);
#endif
}

bool pathExists(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    return input.good();
}

std::uint64_t fileSizeBytes(const std::string& path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) {
        return 0;
    }
    return st.st_size > 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
}

// Lock a separate, never-renamed inode, including across OtaService instances.
class PendingJournalLock {
public:
    explicit PendingJournalLock(const std::string& path) {
#ifdef _WIN32
        handle_ = CreateFileA((path + ".lock").c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        OVERLAPPED offset{};
        if (handle_ == INVALID_HANDLE_VALUE || !LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &offset)) {
            if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
            throw std::runtime_error("cannot lock OTA pending journal");
        }
#else
        fd_ = open((path + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        int result;
        do { result = fd_ < 0 ? -1 : flock(fd_, LOCK_EX); } while (result < 0 && errno == EINTR);
        if (result < 0) {
            if (fd_ >= 0) close(fd_);
            throw std::runtime_error("cannot lock OTA pending journal");
        }
#endif
    }
    ~PendingJournalLock() {
#ifdef _WIN32
        CloseHandle(handle_);
#else
        close(fd_);
#endif
    }
    PendingJournalLock(const PendingJournalLock&) = delete;
    PendingJournalLock& operator=(const PendingJournalLock&) = delete;
private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

#ifndef _WIN32
// Never unlink this inode or LOCK_UN: child copies share the same lock.
class JobLifecycleLock {
public:
    JobLifecycleLock(const std::string& path, bool create) {
        fd_ = open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | (create ? O_CREAT : 0), 0600);
        if (fd_ < 0) throw std::runtime_error("cannot open OTA lifecycle lock");
        struct stat info{};
        if (fstat(fd_, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size != 0) {
            close(fd_); fd_ = -1;
            throw std::runtime_error("invalid OTA lifecycle lock");
        }
        int result;
        do { result = flock(fd_, LOCK_EX | LOCK_NB); } while (result < 0 && errno == EINTR);
        if (result < 0) {
            const auto error = errno;
            close(fd_); fd_ = -1;
            if (error != EWOULDBLOCK && error != EAGAIN)
                throw std::runtime_error("cannot acquire OTA lifecycle lock");
        }
    }
    ~JobLifecycleLock() { if (fd_ >= 0) close(fd_); }
    int fd() const { return fd_; }
    JobLifecycleLock(const JobLifecycleLock&) = delete;
    JobLifecycleLock& operator=(const JobLifecycleLock&) = delete;
private:
    int fd_ = -1;
};

#endif

bool strictlyAbsent(const std::string& path) {
#ifdef _WIN32
    if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const auto error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return true;
#else
    struct stat info{};
    if (lstat(path.c_str(), &info) == 0) return false;
    if (errno == ENOENT) return true;
#endif
    throw std::runtime_error("cannot inspect OTA recovery evidence");
}

std::string newStatusOccurrence() {
    unsigned char bytes[16];
#ifdef _WIN32
    std::random_device random;
    for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
#else
    std::ifstream random("/dev/urandom", std::ios::binary);
    if (!random.read(reinterpret_cast<char*>(bytes), sizeof(bytes)))
        throw std::runtime_error("cannot generate OTA status occurrence");
#endif
    bytes[6] = (bytes[6] & 15) | 64;
    bytes[8] = (bytes[8] & 63) | 128;
    static const char hex[] = "0123456789abcdef";
    std::string result;
    for (std::size_t i = 0; i < sizeof(bytes); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) result += '-';
        result += hex[bytes[i] >> 4]; result += hex[bytes[i] & 15];
    }
    return result;
}

std::string readPendingJournal(const std::string& path) {
    struct stat info{};
    if (stat(path.c_str(), &info) != 0) {
        if (errno == ENOENT) return {};
        throw std::runtime_error("cannot stat OTA pending journal");
    }
    if ((info.st_mode & S_IFMT) != S_IFREG) throw std::runtime_error("OTA pending journal is not a regular file");
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) throw std::runtime_error("cannot open OTA pending journal");
    std::string result((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad()) throw std::runtime_error("cannot read OTA pending journal");
    return result;
}

#ifndef _WIN32
void syncJournalAncestors(std::string directory) {
    if (directory.empty()) directory = "/";
    for (;;) {
        const int dir = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir < 0) throw std::runtime_error("cannot open OTA journal directory");
        const int synced = fsync(dir);
        close(dir);
        if (synced != 0) throw std::runtime_error("cannot fsync OTA journal directory");
        if (directory == "/" || directory == ".") break;
        const auto slash = directory.find_last_of('/');
        directory = slash == std::string::npos ? "." : slash == 0 ? "/" : directory.substr(0, slash);
    }
}
#endif

void replacePendingJournal(const std::string& path, const std::string& bytes) {
    const auto temporary = path + ".tmp." + newStatusOccurrence();
#ifdef _WIN32
    HANDLE file = CreateFileA(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create OTA pending temporary file");
    try {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            DWORD written = 0;
            const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1024 * 1024));
            if (!WriteFile(file, bytes.data() + offset, count, &written, nullptr) || !written)
                throw std::runtime_error("cannot write OTA pending journal");
            offset += written;
        }
        if (!FlushFileBuffers(file)) throw std::runtime_error("cannot flush OTA pending journal");
        CloseHandle(file); file = INVALID_HANDLE_VALUE;
        if (!MoveFileExA(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("cannot replace OTA pending journal");
    } catch (...) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        DeleteFileA(temporary.c_str());
        throw;
    }
#else
    int fd = open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) throw std::runtime_error("cannot create OTA pending temporary file");
    try {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto written = write(fd, bytes.data() + offset, bytes.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw std::runtime_error("cannot write OTA pending journal");
            offset += static_cast<std::size_t>(written);
        }
        if (fsync(fd) != 0) throw std::runtime_error("cannot fsync OTA pending journal");
        const int closed = close(fd); fd = -1;
        if (closed != 0) throw std::runtime_error("cannot close OTA pending journal");
        if (rename(temporary.c_str(), path.c_str()) != 0)
            throw std::runtime_error("cannot replace OTA pending journal");
        // Sync ancestors too: stagingDir may have just been created.
        const auto separator = path.find_last_of('/');
        syncJournalAncestors(separator == std::string::npos ? "." : path.substr(0, separator));
    } catch (...) {
        if (fd >= 0) close(fd);
        unlink(temporary.c_str());
        throw;
    }
#endif
}

std::string pendingJsonString(const std::string& value) {
    static const char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char ch : value) {
        if (ch == '"' || ch == '\\') { result += '\\'; result += static_cast<char>(ch); }
        else if (ch < 32) { result += "\\u00"; result += hex[ch >> 4]; result += hex[ch & 15]; }
        else result += static_cast<char>(ch);
    }
    return result + '"';
}

std::string encodePendingStatus(const OtaStatus& status) {
    // Decimal strings avoid IEEE754 rounding of timestamps and byte counters.
    return "{\"format\":\"ota-status-v2\",\"jobId\":" + pendingJsonString(status.jobId) +
        ",\"machineCode\":" + pendingJsonString(status.machineCode) +
        ",\"stage\":" + pendingJsonString(status.stage) +
        ",\"progress\":" + pendingJsonString(std::to_string(status.progress)) +
        ",\"downloadedBytes\":" + pendingJsonString(std::to_string(status.downloadedBytes)) +
        ",\"totalBytes\":" + pendingJsonString(std::to_string(status.totalBytes)) +
        ",\"message\":" + pendingJsonString(status.message) +
        ",\"ts\":" + pendingJsonString(std::to_string(status.ts)) +
        ",\"occurrenceId\":" + pendingJsonString(status.occurrenceId) +
        ",\"appliedVersion\":" + pendingJsonString(status.appliedVersion) +
        ",\"appliedSha256\":" + pendingJsonString(status.appliedSha256) +
        ",\"productionTransaction\":" + pendingJsonString(status.productionTransaction) +
        ",\"productionCandidateSha256\":" + pendingJsonString(status.productionCandidateSha256) +
        ",\"productionHealth\":" + pendingJsonString(status.productionHealth) + "}\n";
}

std::string pendingField(const json::JsonValue& value, const char* name) {
    const auto* field = value.find(name);
    if (!field || !field->isString()) throw std::runtime_error("invalid OTA pending JSON field");
    return field->asString();
}

template<class T> T pendingInteger(const std::string& value) {
    if (value.empty()) throw std::runtime_error("empty OTA pending integer");
    std::istringstream stream(value);
    stream.imbue(std::locale::classic());
    T result{};
    if (!(stream >> result) || !stream.eof() || std::to_string(result) != value)
        throw std::runtime_error("invalid OTA pending integer");
    return result;
}

OtaStatus decodePendingStatus(const std::string& line) {
    const auto value = json::JsonParser(line, 4, 32).parse();
    if (pendingField(value, "format") != "ota-status-v2")
        throw std::runtime_error("unsupported OTA pending format");
    OtaStatus status;
    status.jobId = pendingField(value, "jobId");
    status.machineCode = pendingField(value, "machineCode");
    status.stage = pendingField(value, "stage");
    status.progress = pendingInteger<int>(pendingField(value, "progress"));
    status.downloadedBytes = pendingInteger<std::uint64_t>(pendingField(value, "downloadedBytes"));
    status.totalBytes = pendingInteger<std::uint64_t>(pendingField(value, "totalBytes"));
    status.message = pendingField(value, "message");
    status.ts = pendingInteger<std::int64_t>(pendingField(value, "ts"));
    status.occurrenceId = pendingField(value, "occurrenceId");
    for (const auto& field : {"appliedVersion", "appliedSha256", "productionTransaction",
                              "productionCandidateSha256", "productionHealth"}) {
        if (const auto* item = value.find(field)) {
            if (!item->isString()) throw std::runtime_error("invalid OTA production receipt field");
            const auto& text = item->asString();
            if (std::string(field) == "appliedVersion") status.appliedVersion = text;
            else if (std::string(field) == "appliedSha256") status.appliedSha256 = text;
            else if (std::string(field) == "productionTransaction") status.productionTransaction = text;
            else if (std::string(field) == "productionCandidateSha256") status.productionCandidateSha256 = text;
            else status.productionHealth = text;
        }
    }
    if (status.occurrenceId.size() != 36) throw std::runtime_error("invalid OTA occurrence ID");
    for (std::size_t i = 0; i < status.occurrenceId.size(); ++i) {
        const char ch = status.occurrenceId[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (ch != '-') throw std::runtime_error("invalid OTA occurrence ID");
        } else if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            throw std::runtime_error("invalid OTA occurrence ID");
        }
    }
    return status;
}

std::string sanitizeJournalField(std::string value) {
    for (auto& ch : value) {
        if (ch == '\t' || ch == '\r' || ch == '\n' || ch == ',') {
            ch = ' ';
        }
    }
    return value;
}

std::vector<std::string> splitTabLine(const std::string& line) {
    std::vector<std::string> parts;
    std::string current;
    for (const auto ch : line) {
        if (ch == '\t') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    parts.push_back(current);
    return parts;
}

std::string fileNameOf(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

std::string joinPath(const std::string& left, const std::string& right) {
    if (left.empty()) {
        return right;
    }
    const char tail = left[left.size() - 1];
    if (tail == '/' || tail == '\\') {
        return left + right;
    }
#ifdef _WIN32
    return left + "\\" + right;
#else
    return left + "/" + right;
#endif
}

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool isSafeOtaId(const std::string& value) {
    if (value.empty() || value.size() > 128) {
        return false;
    }
    for (const auto ch : value) {
        const bool ok = std::isalnum(static_cast<unsigned char>(ch)) != 0 ||
            ch == '_' || ch == '-' || ch == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool isSafeSha256(const std::string& value) {
    if (value.empty()) {
        return true;
    }
    if (value.size() != 64) {
        return false;
    }
    for (const auto ch : value) {
        if (std::isxdigit(static_cast<unsigned char>(ch)) == 0) {
            return false;
        }
    }
    return true;
}

bool isSafeRelativeArtifactName(const std::string& value) {
    if (value.empty() || value.size() > 255) {
        return false;
    }
    if (value.find("..") != std::string::npos || value.find('/') != std::string::npos || value.find('\\') != std::string::npos) {
        return false;
    }
    for (const auto ch : value) {
        const bool ok = std::isalnum(static_cast<unsigned char>(ch)) != 0 ||
            ch == '_' || ch == '-' || ch == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool isHttpUrlText(const std::string& value) {
    return value.find("http://") == 0 || value.find("https://") == 0;
}

bool hasControlCharacter(const std::string& value);

// Validate an artifact URL before handing it to curl/wget. Even though the URL
// is passed as a positional shell argument (so classic shell-metacharacter
// injection is not possible), a value beginning with '-' would be parsed as a
// command-line option by curl/wget (argument injection, e.g. -K/--config to
// read an arbitrary file, or -o to write one). Reject anything that is not a
// plain http(s) URL with a conservative character set.
bool isSafeDownloadUrl(const std::string& value) {
    if (value.empty() || value.size() > 2048) {
        return false;
    }
    if (!isHttpUrlText(value)) {
        return false;
    }
    if (hasControlCharacter(value)) {
        return false;
    }
    // No whitespace anywhere in the URL.
    for (const auto ch : value) {
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            return false;
        }
    }
    // The scheme guarantees the first character is 'h', so the value can never
    // be mistaken for an option flag; this is an explicit defensive check in
    // case the scheme prefix is ever relaxed.
    if (value.front() == '-') {
        return false;
    }
    const auto schemeEnd = value.find("://");
    if (schemeEnd == std::string::npos) {
        return false;
    }
    const auto hostStart = schemeEnd + 3;
    if (hostStart >= value.size()) {
        return false;
    }
    // The host portion must not be empty and must not start with '/'.
    if (value[hostStart] == '/') {
        return false;
    }
    // Restrict to an allow-list of characters that legitimately appear in a
    // URL. Excludes shell metacharacters, quotes, backticks, and parentheses
    // as defence-in-depth even though the value is not passed through a shell
    // string.
    for (const auto ch : value) {
        const bool ok = std::isalnum(static_cast<unsigned char>(ch)) != 0 ||
            ch == ':' || ch == '/' || ch == '.' || ch == '-' || ch == '_' ||
            ch == '~' || ch == '%' || ch == '?' || ch == '#' || ch == '[' ||
            ch == ']' || ch == '@' || ch == '!' || ch == '$' || ch == '&' ||
            ch == '\'' || ch == '(' || ch == ')' || ch == '*' || ch == '+' ||
            ch == ',' || ch == ';' || ch == '=';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool hasControlCharacter(const std::string& value) {
    for (const auto ch : value) {
        if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f) {
            return true;
        }
    }
    return false;
}

int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

std::string percentDecode(const std::string& value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int high = hexValue(value[i + 1]);
            const int low = hexValue(value[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        decoded.push_back(value[i]);
    }
    return decoded;
}

bool hasPathTraversalSegment(const std::string& value) {
    std::string normalized = value;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    return normalized == ".." ||
        normalized.find("../") == 0 ||
        normalized.find("/../") != std::string::npos ||
        (normalized.size() >= 3 && normalized.compare(normalized.size() - 3, 3, "/..") == 0);
}

std::string pathPartForTraversalCheck(const std::string& value) {
    auto pathText = value;
    const auto scheme = pathText.find("://");
    if (scheme != std::string::npos) {
        const auto pathStart = pathText.find('/', scheme + 3);
        pathText = pathStart == std::string::npos ? "/" : pathText.substr(pathStart);
    }
    const auto suffix = pathText.find_first_of("?#");
    if (suffix != std::string::npos) {
        pathText = pathText.substr(0, suffix);
    }
    return pathText;
}

std::string artifactFileName(const std::string& value) {
    if (hasPathTraversalSegment(pathPartForTraversalCheck(value))) {
        return std::string();
    }
    auto fileName = fileNameOf(value);
    const auto query = fileName.find_first_of("?#");
    if (query != std::string::npos) {
        fileName = fileName.substr(0, query);
    }
    return fileName;
}

bool isBase64Text(const std::string& value) {
    if (value.empty() || value.size() > 4096 || value.size() % 4 != 0) {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        const bool ok = std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '+' || ch == '/' ||
            (ch == '=' && i + 2 >= value.size());
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::string decodeBase64(const std::string& value) {
    if (!isBase64Text(value)) {
        throw std::runtime_error("invalid ota signature encoding");
    }
    std::string decoded;
    unsigned int buffer = 0;
    int bits = 0;
    for (const auto ch : value) {
        int sextet = -1;
        if (ch >= 'A' && ch <= 'Z') sextet = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') sextet = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') sextet = ch - '0' + 52;
        else if (ch == '+') sextet = 62;
        else if (ch == '/') sextet = 63;
        else break;  // '=' padding
        buffer = (buffer << 6) | static_cast<unsigned int>(sextet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<char>((buffer >> bits) & 0xff));
        }
    }
    if (decoded.empty()) {
        throw std::runtime_error("invalid ota signature encoding");
    }
    return decoded;
}

void validateOtaRequestFields(const OtaRequest& request, const OtaConfig& config) {
    if (!isSafeOtaId(request.jobId)) {
        throw std::runtime_error("invalid ota jobId");
    }
    if (!request.version.empty() && !isSafeOtaId(request.version)) {
        throw std::runtime_error("invalid ota version");
    }
    if (config.checksumRequired && request.sha256.empty()) {
        throw std::runtime_error("ota sha256 is required");
    }
    if (!isSafeSha256(request.sha256)) {
        throw std::runtime_error("invalid ota sha256");
    }
    if (config.requireSignature) {
        if (request.signature.empty()) {
            throw std::runtime_error("ota signature is required");
        }
        if (request.sha256.empty()) {
            throw std::runtime_error("ota sha256 is required for signed ota");
        }
    }
    if (!request.signature.empty() && !isBase64Text(request.signature)) {
        throw std::runtime_error("invalid ota signature encoding");
    }
    if (!request.packageType.empty() && request.packageType != "full" &&
        request.packageType != "config" && request.packageType != "scada") {
        throw std::runtime_error("unsupported ota packageType");
    }
    if (request.artifactUrl.empty() || hasControlCharacter(request.artifactUrl)) {
        throw std::runtime_error("invalid ota artifactUrl");
    }
    if (hasControlCharacter(percentDecode(request.artifactUrl))) {
        throw std::runtime_error("invalid ota artifactUrl");
    }
    const auto pathPart = pathPartForTraversalCheck(request.artifactUrl);
    if (hasPathTraversalSegment(pathPart) || hasPathTraversalSegment(percentDecode(pathPart))) {
        throw std::runtime_error("invalid ota artifactUrl");
    }
}

void validateOtaSize(const OtaConfig& config, std::uint64_t size) {
    if (config.maxArtifactBytes == 0 || size == 0) {
        return;
    }
    if (size > config.maxArtifactBytes) {
        throw std::runtime_error("ota artifact size exceeds maxArtifactBytes");
    }
}

struct HttpUrl {
    std::string host;
    std::string path = "/";
    int port = 80;
};

HttpUrl parseHttpUrl(const std::string& value) {
    const std::string prefix = "http://";
    if (value.find(prefix) != 0) {
        throw std::runtime_error("byte-accurate ota download only supports http artifactUrl");
    }
    std::string rest = value.substr(prefix.size());
    const auto slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    HttpUrl parsed;
    parsed.path = slash == std::string::npos ? "/" : rest.substr(slash);
    const auto colon = authority.rfind(':');
    if (colon != std::string::npos) {
        parsed.host = authority.substr(0, colon);
        parsed.port = std::stoi(authority.substr(colon + 1));
    } else {
        parsed.host = authority;
    }
    if (parsed.host.empty()) {
        throw std::runtime_error("http artifactUrl host is empty");
    }
    return parsed;
}

int downloadProgress(std::uint64_t downloadedBytes, std::uint64_t totalBytes) {
    if (totalBytes == 0) {
        return 0;
    }
    const auto percent = (downloadedBytes * 100) / totalBytes;
    return static_cast<int>(std::min<std::uint64_t>(100, percent));
}

#ifndef _WIN32
class SocketGuard {
public:
    explicit SocketGuard(int sock) : sock_(sock) {}
    ~SocketGuard() {
        if (sock_ >= 0) {
            close(sock_);
        }
    }
    int get() const { return sock_; }
private:
    int sock_;
};

int connectTcp(const std::string& host, int port, int timeoutSec) {
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const auto portText = std::to_string(port);
    if (getaddrinfo(host.c_str(), portText.c_str(), &hints, &result) != 0) {
        throw std::runtime_error("ota http getaddrinfo failed: " + host);
    }
    int sock = -1;
    for (auto* entry = result; entry != nullptr; entry = entry->ai_next) {
        sock = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (sock < 0) {
            continue;
        }
        const int flags = fcntl(sock, F_GETFL, 0);
        if (flags >= 0 && timeoutSec > 0) {
            fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        }
        int rc = connect(sock, entry->ai_addr, entry->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS && timeoutSec > 0) {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(sock, &writable);
            timeval tv {};
            tv.tv_sec = timeoutSec;
            tv.tv_usec = 0;
            rc = select(sock + 1, nullptr, &writable, nullptr, &tv);
            if (rc > 0) {
                int error = 0;
                socklen_t errorLen = sizeof(error);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &errorLen) != 0 || error != 0) {
                    rc = -1;
                } else {
                    rc = 0;
                }
            } else {
                rc = -1;
            }
        }
        if (flags >= 0 && timeoutSec > 0) {
            fcntl(sock, F_SETFL, flags);
        }
        if (rc == 0) {
            break;
        }
        close(sock);
        sock = -1;
    }
    freeaddrinfo(result);
    if (sock < 0) {
        throw std::runtime_error("ota http connect failed: " + host + ":" + portText);
    }
    return sock;
}

void sendAll(int sock, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto rc = send(sock, data.data() + sent, data.size() - sent, 0);
        if (rc <= 0) {
            throw std::runtime_error("ota http send failed");
        }
        sent += static_cast<std::size_t>(rc);
    }
}
#endif

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

#ifndef _WIN32
void setSocketTimeouts(int sock, int timeoutSec) {
    if (timeoutSec <= 0) {
        return;
    }
    timeval tv {};
    tv.tv_sec = timeoutSec;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

bool processStillRunning(pid_t pid) {
    if (pid <= 0) {
        return false;
    }
    return kill(pid, 0) == 0;
}

void terminateProcess(pid_t pid) {
    if (pid <= 0 || !processStillRunning(pid)) {
        return;
    }
    kill(-pid, SIGTERM);
    kill(pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        int status = 0;
        const pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid || waited < 0) {
            return;
        }
        usleep(100 * 1000);
    }
    if (processStillRunning(pid)) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

int runShellCommandWithTimeout(const std::string& command, int timeoutSec, int lifecycleFd = -1) {
    const pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        setsid();
        if (lifecycleFd >= 0 &&
            (dup2(lifecycleFd, 9) < 0 || fcntl(9, F_SETFD, 0) < 0)) _exit(127);
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    int status = 0;
    const auto startedAt = currentTimeMs();
    while (true) {
        const pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            return status;
        }
        if (waited < 0) {
            return -1;
        }
        if (timeoutSec > 0 && currentTimeMs() - startedAt > static_cast<std::int64_t>(timeoutSec) * 1000) {
            terminateProcess(pid);
            return -2;
        }
        usleep(100 * 1000);
    }
}

// Runs a program with an explicit argv (never through a shell).
int runArgvWithTimeout(const std::vector<std::string>& argv, int timeoutSec) {
    if (argv.empty()) {
        return -1;
    }
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& arg : argv) {
        args.push_back(const_cast<char*>(arg.c_str()));
    }
    args.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        setsid();
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        execvp(args[0], args.data());
        _exit(127);
    }
    int status = 0;
    const auto startedAt = currentTimeMs();
    while (true) {
        const pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            return status;
        }
        if (waited < 0) {
            return -1;
        }
        if (timeoutSec > 0 && currentTimeMs() - startedAt > static_cast<std::int64_t>(timeoutSec) * 1000) {
            terminateProcess(pid);
            return -2;
        }
        usleep(50 * 1000);
    }
}

bool commandStatusSucceeded(int status) {
    if (status < 0) {
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void writePrivateTempFile(const std::string& directory, const std::string& prefix,
                          const std::string& bytes, std::string* createdPath) {
    std::string pattern = joinPath(directory, prefix + ".XXXXXX");
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const int fd = mkstemp(buffer.data());
    if (fd < 0) {
        throw std::runtime_error("cannot create ota signature temporary file");
    }
    *createdPath = buffer.data();
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto written = write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            close(fd);
            unlink(createdPath->c_str());
            throw std::runtime_error("cannot write ota signature temporary file");
        }
        offset += static_cast<std::size_t>(written);
    }
    close(fd);
}

void ensureFreeDiskSpace(const std::string& path, std::uint64_t minFreeBytes, std::uint64_t expectedWriteBytes) {
    if (minFreeBytes == 0 && expectedWriteBytes == 0) {
        return;
    }
    struct statvfs fs {};
    if (statvfs(path.c_str(), &fs) != 0) {
        return;
    }
    const auto freeBytes = static_cast<std::uint64_t>(fs.f_bavail) * static_cast<std::uint64_t>(fs.f_frsize);
    const auto required = minFreeBytes > std::numeric_limits<std::uint64_t>::max() - expectedWriteBytes
        ? std::numeric_limits<std::uint64_t>::max()
        : minFreeBytes + expectedWriteBytes;
    if (freeBytes < required) {
        throw std::runtime_error("ota free disk space is below required threshold");
    }
}
#else
int runShellCommandWithTimeout(const std::string& command, int, int = -1) {
    return std::system(command.c_str());
}

bool commandStatusSucceeded(int status) {
    return status == 0;
}

void ensureFreeDiskSpace(const std::string&, std::uint64_t, std::uint64_t) {
}
#endif


std::vector<std::string> listFilesInDirectory(const std::string& path) {
    std::vector<std::string> files;
#ifdef _WIN32
    const auto pattern = joinPath(path, "*");
    struct _finddata_t entry;
    const intptr_t handle = _findfirst(pattern.c_str(), &entry);
    if (handle == -1) {
        return files;
    }
    intptr_t cursor = handle;
    do {
        const std::string name = entry.name;
        if (name != "." && name != ".." && (entry.attrib & _A_SUBDIR) == 0) {
            files.push_back(joinPath(path, name));
        }
    } while (_findnext(cursor, &entry) == 0);
    _findclose(cursor);
#else
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        return files;
    }
    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") {
            files.push_back(joinPath(path, name));
        }
    }
    closedir(dir);
#endif
    std::sort(files.begin(), files.end());
    return files;
}

void removeFilePath(const std::string& path) {
#ifdef _WIN32
    _unlink(path.c_str());
#else
    unlink(path.c_str());
#endif
}

}  // namespace

OtaService::OtaService(OtaConfig config) : config_(std::move(config)) {
}

bool OtaService::enabled() const {
    return config_.enabled;
}

void OtaService::rejectDuplicateRequest(const std::string& jobId) const {
    if (!isSafeOtaId(jobId))
        throw OtaRequestRejected(OtaRejectionReason::NotAdmitted, "invalid ota jobId");
    OtaRequest identity;
    identity.jobId = jobId;
    ensureJobNotProcessed(identity);
#ifndef _WIN32
    const auto path = joinPath(config_.stagingDir, "ota_job_" + jobId + ".lock");
    if (!strictlyAbsent(path)) {
        JobLifecycleLock probe(path, false);
        if (probe.fd() < 0)
            throw OtaRequestRejected(OtaRejectionReason::Duplicate, "OTA job execution already owned");
    }
#endif
}

bool OtaService::validateRequest(const OtaRequest& request, std::string* errorMessage,
                                 OtaRejectionReason* reason) const {
    if (reason) *reason = OtaRejectionReason::NotAdmitted;
    try {
        rejectDuplicateRequest(request.jobId);
        validateOtaRequestFields(request, config_);
        validateOtaSize(config_, request.size);
        (void)resolveArtifactPath(request);
        ensureTransportAllowed(resolveArtifactSource(request));
        ensureJobNotProcessed(request);
        ensureNoUnresolvedJob();
        return true;
    } catch (const OtaRequestRejected& ex) {
        if (reason) *reason = ex.reason();
        if (errorMessage) *errorMessage = ex.what();
        return false;
    } catch (const std::exception& ex) {
        if (errorMessage != nullptr) {
            *errorMessage = ex.what();
        }
        return false;
    }
}

std::string OtaService::statusJournalPath() const {
    return joinPath(config_.stagingDir, "ota_status_pending.log");
}

class OtaTerminalBacklog : public std::runtime_error {
public:
    OtaTerminalBacklog() : std::runtime_error("OTA terminal backlog requires delivery resolution") {}
};

void OtaService::appendPendingStatus(OtaStatus& status) const {
    std::lock_guard<std::mutex> guard(pendingMutex_);
    ensureDirectory(config_.stagingDir);
    PendingJournalLock lock(statusJournalPath());
    // reportStage/reportDownloadProgress clear this for each new occurrence.
    // A retry after an uncertain fsync keeps the ID assigned before any I/O.
    if (status.occurrenceId.empty()) status.occurrenceId = newStatusOccurrence();
    auto bytes = readPendingJournal(statusJournalPath());
    if (!bytes.empty() && bytes.back() != '\n')
        throw std::runtime_error("truncated OTA pending journal; refusing append");
    const auto record = encodePendingStatus(status);
    if (config_.maxPendingStatusBytes && record.size() > config_.maxPendingStatusBytes)
        throw std::runtime_error("OTA status exceeds pending journal limit");
    const auto previousSize = bytes.size();
    const auto existing = bytes.find(record);
    const bool appended = existing == std::string::npos || (existing != 0 && bytes[existing - 1] != '\n');
    if (appended) bytes += record;
    const bool terminal = status.stage == "completed" || status.stage == "failed";
    const auto limit = config_.maxPendingStatusBytes;
    if (limit && bytes.size() > limit) {
        struct Row { std::string bytes; bool disposable; bool appended; };
        std::vector<Row> rows;
        for (std::size_t pos = 0; pos < bytes.size();) {
            const auto end = bytes.find('\n', pos);
            if (end == std::string::npos) throw std::runtime_error("truncated OTA pending journal");
            const auto line = bytes.substr(pos, end - pos);
            bool disposable = false;
            if (!line.empty() && line.front() == '{') {
                const auto old = decodePendingStatus(line);
                disposable = old.stage != "completed" && old.stage != "failed";
            } else {
                const auto fields = splitTabLine(line);
                disposable = fields.size() >= 3 && fields[2] != "completed" && fields[2] != "failed";
            }
            rows.push_back({bytes.substr(pos, end - pos + 1), disposable, appended && pos == previousSize});
            pos = end + 1;
        }
        auto size = bytes.size();
        while (size > limit) {
            const auto old = std::find_if(rows.begin(), rows.end(), [](const Row& row) {
                return row.disposable && !row.appended;
            });
            if (old == rows.end()) {
                if (terminal && size - limit <= record.size()) break;
                throw OtaTerminalBacklog();
            }
            size -= old->bytes.size();
            rows.erase(old);
        }
        bytes.clear();
        bytes.reserve(size);
        for (const auto& row : rows) bytes += row.bytes;
    }
    replacePendingJournal(statusJournalPath(), bytes);
}

std::vector<OtaStatus> OtaService::loadPendingStatuses() const {
    std::lock_guard<std::mutex> guard(pendingMutex_);
    pendingSnapshot_.clear();
    struct stat info{};
    if (stat(statusJournalPath().c_str(), &info) != 0) {
        if (errno == ENOENT) return {};
        throw std::runtime_error("cannot stat OTA pending journal");
    }
    PendingJournalLock lock(statusJournalPath());
    const auto bytes = readPendingJournal(statusJournalPath());
    std::vector<OtaStatus> statuses;
    std::istringstream input(bytes);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.front() == '{') {
            if (input.eof()) throw std::runtime_error("truncated OTA pending JSON record");
            statuses.push_back(decodePendingStatus(line));
            continue;
        }
        const auto parts = splitTabLine(line);
        if (parts.size() < 6) {
            continue;
        }
        OtaStatus status;
        status.jobId = parts[0];
        status.machineCode = parts[1];
        status.stage = parts[2];
        try {
            status.progress = std::stoi(parts[3]);
            if (parts.size() >= 8) {
                status.downloadedBytes = static_cast<std::uint64_t>(std::stoull(parts[4]));
                status.totalBytes = static_cast<std::uint64_t>(std::stoull(parts[5]));
                status.message = parts[6];
                status.ts = static_cast<std::int64_t>(std::stoll(parts[7]));
            } else {
                status.message = parts[4];
                status.ts = static_cast<std::int64_t>(std::stoll(parts[5]));
            }
        } catch (...) {
            continue;
        }
        if (!status.jobId.empty() && !status.stage.empty()) {
            statuses.push_back(status);
        }
    }
    pendingSnapshot_ = bytes;
    return statuses;
}

struct PendingOtaJob {
    std::string jobId;
    std::string machineCode;
    std::string version;
    std::string artifactPath;
    std::string previousVersion;
    std::string token;
    std::string lifecycle;
    std::string packageType;
    std::string artifactSha256;
    std::uint64_t artifactSize = 0;
    std::uint64_t leaseDev = 0, leaseInode = 0;
};

std::string pendingJobPath(const std::string& stagingDir, const std::string& jobId) {
    return joinPath(stagingDir, "ota_job_" + jobId + ".json");
}

std::string encodePendingJob(const PendingOtaJob& job) {
    return "{\"format\":\"ota-job-v1\",\"jobId\":" + pendingJsonString(job.jobId) +
        ",\"machineCode\":" + pendingJsonString(job.machineCode) +
        ",\"version\":" + pendingJsonString(job.version) +
        ",\"artifactPath\":" + pendingJsonString(job.artifactPath) +
        ",\"previousVersion\":" + pendingJsonString(job.previousVersion) +
        ",\"token\":" + pendingJsonString(job.token) +
        (job.lifecycle.empty() ? "" : ",\"lifecycle\":" + pendingJsonString(job.lifecycle)) +
        (job.lifecycle != "systemd-handoff-v1" ? "" :
            ",\"artifactSha256\":" + pendingJsonString(job.artifactSha256) +
            ",\"packageType\":" + pendingJsonString(job.packageType) +
            ",\"artifactSize\":" + pendingJsonString(std::to_string(job.artifactSize)) +
            ",\"leaseDev\":" + pendingJsonString(std::to_string(job.leaseDev)) +
            ",\"leaseInode\":" + pendingJsonString(std::to_string(job.leaseInode))) + "}\n";
}

PendingOtaJob decodePendingJob(const std::string& bytes) {
    if (bytes.empty() || bytes.size() > 4096 || bytes.back() != '\n')
        throw std::runtime_error("incomplete OTA job intent");
    const auto document = json::JsonParser(bytes, 4, 32).parse();
    if (pendingField(document, "format") != "ota-job-v1")
        throw std::runtime_error("unsupported OTA job intent");
    PendingOtaJob job;
    job.jobId = pendingField(document, "jobId");
    job.machineCode = pendingField(document, "machineCode");
    job.version = pendingField(document, "version");
    job.artifactPath = pendingField(document, "artifactPath");
    job.previousVersion = pendingField(document, "previousVersion");
    job.token = pendingField(document, "token");
    if (document.find("lifecycle")) {
        job.lifecycle = pendingField(document, "lifecycle");
        if (job.lifecycle != "inherited-flock-v1" && job.lifecycle != "systemd-handoff-v1")
            throw std::runtime_error("unsupported OTA lifecycle protocol");
        if (job.lifecycle == "systemd-handoff-v1") {
            job.artifactSha256 = pendingField(document, "artifactSha256");
            job.packageType = document.find("packageType") ? pendingField(document, "packageType") : "full";
            if (job.packageType != "full" && job.packageType != "config")
                throw std::runtime_error("unsupported runtime handoff package type");
            job.artifactSize = pendingInteger<std::uint64_t>(pendingField(document, "artifactSize"));
            job.leaseDev = pendingInteger<std::uint64_t>(pendingField(document, "leaseDev"));
            job.leaseInode = pendingInteger<std::uint64_t>(pendingField(document, "leaseInode"));
            if (!isSafeSha256(job.artifactSha256) || job.artifactSha256.empty() || !job.artifactSize)
                throw std::runtime_error("invalid runtime handoff artifact binding");
        }
    }
    if (!isSafeOtaId(job.jobId) || !isSafeOtaId(job.version) || job.machineCode.empty() ||
        job.token.size() != 36 || job.artifactPath.empty())
        throw std::runtime_error("invalid OTA job intent");
    return job;
}

struct OtaHealthResult {
    std::string status;
    std::string rollback;
    std::int64_t ts = 0;
    std::string productionTransaction;
    std::string productionCandidateSha256;
};

OtaHealthResult decodeHealthResult(const std::string& bytes, const PendingOtaJob& job) {
    if (bytes.empty() || bytes.size() > 4096 || bytes.back() != '\n')
        throw std::runtime_error("incomplete OTA health result");
    const auto document = json::JsonParser(bytes, 4, 32).parse();
    if (pendingField(document, "format") != "ota-health-v1" ||
        pendingField(document, "jobId") != job.jobId ||
        pendingField(document, "version") != job.version ||
        pendingField(document, "token") != job.token)
        throw std::runtime_error("OTA health result does not match job intent");
    OtaHealthResult result;
    result.status = pendingField(document, "status");
    result.rollback = pendingField(document, "rollback");
    result.ts = pendingInteger<std::int64_t>(pendingField(document, "ts"));
    if (job.lifecycle == "systemd-handoff-v1" && job.packageType == "config" &&
        result.status == "healthy") {
        result.productionTransaction = pendingField(document, "productionTransaction");
        result.productionCandidateSha256 = pendingField(document, "productionCandidateSha256");
        if (result.productionTransaction.size() != 36 ||
            result.productionTransaction.compare(0, 4, "OTA_") != 0 ||
            !isSafeSha256(result.productionCandidateSha256) ||
            result.productionCandidateSha256.size() != 64)
            throw std::runtime_error("invalid production config completion binding");
    }
    if (result.ts <= 0 ||
        !((result.status == "healthy" && result.rollback == "none") ||
          (result.status == "failed" &&
           (result.rollback == "succeeded" || result.rollback == "failed" ||
            result.rollback == "skipped" || result.rollback == "unchanged"))))
        throw std::runtime_error("invalid OTA health terminal result");
    return result;
}

void bindProductionCompletion(OtaStatus& status, const PendingOtaJob& job,
                              const OtaHealthResult& health) {
    if (job.lifecycle != "systemd-handoff-v1" || job.packageType != "config" ||
        health.status != "healthy") return;
    status.appliedVersion = job.version;
    status.appliedSha256 = job.artifactSha256;
    status.productionTransaction = health.productionTransaction;
    status.productionCandidateSha256 = health.productionCandidateSha256;
    status.productionHealth = health.status;
}

class SuccessfulFinalizationError : public std::runtime_error {
public:
    explicit SuccessfulFinalizationError(const PendingOtaJob& job)
        : std::runtime_error("OTA successful apply requires finalization reconciliation"),
          jobId(job.jobId), token(job.token) {}
    std::string jobId;
    std::string token;
};

void persistFinalizationAudit(const std::string& stagingDir, const PendingOtaJob& job,
                             bool finalCommitted, const char* phase, const char* evidence,
                             const char* reason = "successful-apply-finalization-error",
                             bool deliveryPending = false, std::int64_t recordedTs = 0) {
    replacePendingJournal(joinPath(stagingDir, "ota_reconcile_" + job.jobId + ".json"),
            "{\"format\":\"ota-reconcile-v1\",\"reason\":" + pendingJsonString(reason) + ",\"jobId\":" +
            pendingJsonString(job.jobId) + ",\"token\":" + pendingJsonString(job.token) +
            ",\"version\":" + pendingJsonString(job.version) +
            ",\"finalState\":" + pendingJsonString(deliveryPending ? "delivery-pending" :
                finalCommitted ? "committed" : "unknown") +
            ",\"phase\":" + pendingJsonString(phase) + ",\"evidence\":" + pendingJsonString(evidence) +
            ",\"ts\":\"" + std::to_string(recordedTs > 0 ? recordedTs : currentTimeMs()) + "\"}\n");
}

void auditSuccessfulFinalization(const std::string& stagingDir, const PendingOtaJob& job,
                                bool finalCommitted, const char* phase, const char* evidence) noexcept {
    try {
        persistFinalizationAudit(stagingDir, job, finalCommitted, phase, evidence);
    } catch (...) {
        // No exception text or artifact location: either may contain credentials.
        try {
            std::cerr << "OTA reconciliation audit NOT durable; jobId=" << job.jobId
                      << " token=" << job.token << " finalState=" << (finalCommitted ? "committed" : "unknown")
                      << " phase=" << phase << "; stderr is best effort only\n";
        } catch (...) {}
    }
}

bool needsFinalizationReconciliation(const std::string& stagingDir, const PendingOtaJob& job,
                                    bool* synchronousTail = nullptr, bool* terminalIntent = nullptr,
                                    bool* deliveryPending = nullptr) {
    if (synchronousTail) *synchronousTail = false;
    if (terminalIntent) *terminalIntent = false;
    if (deliveryPending) *deliveryPending = false;
    const auto path = joinPath(stagingDir, "ota_reconcile_" + job.jobId + ".json");
    if (strictlyAbsent(path)) return false;
    if (fileSizeBytes(path) > 4096) throw std::runtime_error("invalid OTA reconciliation audit");
    const auto bytes = readPendingJournal(path);
    if (bytes.empty() || bytes.back() != '\n') throw std::runtime_error("incomplete OTA reconciliation audit");
    const auto audit = json::JsonParser(bytes, 4, 32).parse();
    const std::vector<std::string> fields{
        "format", "reason", "jobId", "token", "version", "finalState", "phase", "evidence", "ts"};
    if (!audit.isObject() || audit.asObject().values.size() != fields.size())
        throw std::runtime_error("invalid OTA reconciliation audit fields");
    for (const auto& name : fields) {
        std::size_t count = 0;
        for (const auto& member : audit.asObject().values) {
            if (member.key == name) {
                if (!member.value || !member.value->isString())
                    throw std::runtime_error("invalid OTA reconciliation audit field type");
                ++count;
            }
        }
        if (count != 1) throw std::runtime_error("ambiguous OTA reconciliation audit");
    }
    const auto reason = pendingField(audit, "reason");
    if (pendingField(audit, "format") != "ota-reconcile-v1" ||
        (reason != "successful-apply-finalization-error" && reason != "ota-terminal-outbox-v1") ||
        pendingField(audit, "jobId") != job.jobId || pendingField(audit, "token") != job.token ||
        pendingField(audit, "version") != job.version ||
        pendingInteger<std::int64_t>(pendingField(audit, "ts")) <= 0)
        throw std::runtime_error("OTA reconciliation audit does not match intent");
    const auto state = pendingField(audit, "finalState");
    if (state != "committed" && state != "unknown" && state != "delivery-pending")
        throw std::runtime_error("invalid OTA reconciliation audit state");
    if (state == "delivery-pending" && reason != "ota-terminal-outbox-v1")
        throw std::runtime_error("invalid OTA delivery audit state");
    const auto phase = pendingField(audit, "phase");
    if (terminalIntent) *terminalIntent = reason == "ota-terminal-outbox-v1";
    if (deliveryPending) *deliveryPending = state == "delivery-pending";
    if (synchronousTail)
        *synchronousTail = pendingField(audit, "evidence") == "apply-returned-success" &&
            ((state == "unknown" && (phase == "version" || phase == "history" ||
                phase == "artifact-retention" || phase == "final-marker")) ||
             (state == "delivery-pending" && phase == "terminal-journal"));
    // A visible marker after rename does not undo a directory-fsync failure.
    return state != "committed";
}

bool hasUnresolvedExecutionAudit(const std::string& stagingDir, const PendingOtaJob& job) {
    bool deliveryPending = false;
    const bool unresolved = needsFinalizationReconciliation(stagingDir, job, nullptr, nullptr,
        &deliveryPending);
#ifndef _WIN32
    if (deliveryPending) syncJournalAncestors(stagingDir);
#endif
    return unresolved && !deliveryPending;
}

std::int64_t finalizationAuditTimestamp(const std::string& stagingDir, const PendingOtaJob& job) {
    const auto audit = json::JsonParser(readPendingJournal(
        joinPath(stagingDir, "ota_reconcile_" + job.jobId + ".json")), 4, 32).parse();
    return pendingInteger<std::int64_t>(pendingField(audit, "ts"));
}

bool hasHandoffResolution(const std::string& stagingDir, const PendingOtaJob& job);

bool hasValidFinal(const std::string& stagingDir, const PendingOtaJob& job) {
    const auto path = joinPath(stagingDir, "ota_final_" + job.jobId + ".txt");
    struct stat info{};
    if (stat(path.c_str(), &info) != 0) {
        if (errno == ENOENT) return false;
        throw std::runtime_error("cannot stat OTA final marker");
    }
    if (info.st_size < 1 || info.st_size > 256) throw std::runtime_error("invalid OTA final marker");
    const auto bytes = readPendingJournal(path);
    const auto prefix = "jobId=" + job.jobId + "\ntoken=" + job.token + "\nstatus=";
    if (bytes != prefix + "completed\n" && bytes != prefix + "failed\n")
        throw std::runtime_error("OTA final marker does not match job intent");
    return true;
}

std::string statusSupersessionPath(const std::string& stagingDir, const PendingOtaJob& job) {
    return joinPath(stagingDir, "ota_status_supersession_" + job.jobId + ".json");
}

void persistStatusSupersession(const std::string& stagingDir, const PendingOtaJob& job,
                              const OtaStatus& status, const std::string& healthBytes) {
    replacePendingJournal(statusSupersessionPath(stagingDir, job),
        "{\"format\":\"ota-status-supersession-v1\",\"jobId\":" + pendingJsonString(job.jobId) +
        ",\"token\":" + pendingJsonString(job.token) +
        ",\"sourceOccurrence\":" + pendingJsonString(status.occurrenceId) +
        ",\"machineCode\":" + pendingJsonString(status.machineCode) +
        ",\"healthRecord\":" + pendingJsonString(healthBytes) + "}\n");
}

bool hasBoundSupersedingTerminal(const std::string& snapshot, const PendingOtaJob& job) {
    std::istringstream input(snapshot);
    std::string line;
    while (std::getline(input, line)) {
        try {
            if (line.empty() || line.front() != '{') continue;
            const auto replacement = decodePendingStatus(line);
            if (replacement.jobId == job.jobId && replacement.machineCode == job.machineCode &&
                replacement.occurrenceId == job.token && replacement.stage == "failed") return true;
        } catch (const std::exception&) {}
    }
    return false;
}

bool hasValidatedStatusSupersession(const std::string& stagingDir, const std::string& snapshot,
                                    const OtaStatus& status, const PendingOtaJob& job,
                                    bool requireBoundReplacement = true) {
    if (status.jobId != job.jobId || status.machineCode != job.machineCode ||
        status.stage != "failed" || status.occurrenceId == job.token ||
        (requireBoundReplacement && !hasBoundSupersedingTerminal(snapshot, job))) return false;
    const auto path = statusSupersessionPath(stagingDir, job);
    if (strictlyAbsent(path) || fileSizeBytes(path) > 8192) return false;
#ifndef _WIN32
    struct stat info{};
    if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != 0 ||
        info.st_nlink != 1 || (info.st_mode & 0022)) return false;
#endif
    const auto bytes = readPendingJournal(path);
    if (bytes.empty() || bytes.back() != '\n') return false;
    const auto proof = json::JsonParser(bytes, 4, 32).parse();
    if (!proof.isObject() || proof.asObject().values.size() != 6 ||
        pendingField(proof, "format") != "ota-status-supersession-v1" ||
        pendingField(proof, "jobId") != job.jobId || pendingField(proof, "token") != job.token ||
        pendingField(proof, "sourceOccurrence") != status.occurrenceId ||
        pendingField(proof, "machineCode") != status.machineCode) return false;
    const auto healthBytes = pendingField(proof, "healthRecord");
    if (healthBytes.empty() || healthBytes.size() > 4096 ||
        healthBytes != readPendingJournal(joinPath(stagingDir, "health_" + job.jobId + ".txt"))) return false;
    const auto health = decodeHealthResult(healthBytes, job);
    if (health.status != "failed" ||
        (health.rollback != "unchanged" && health.rollback != "succeeded")) return false;
    if (!hasValidFinal(stagingDir, job) ||
        readPendingJournal(joinPath(stagingDir, "ota_final_" + job.jobId + ".txt")) !=
            "jobId=" + job.jobId + "\ntoken=" + job.token + "\nstatus=failed\n") return false;
    bool deliveryPending = false;
    if (needsFinalizationReconciliation(stagingDir, job, nullptr, nullptr, &deliveryPending) &&
        !deliveryPending) return false;
    const auto audit = json::JsonParser(readPendingJournal(
        joinPath(stagingDir, "ota_reconcile_" + job.jobId + ".json")), 4, 32).parse();
    return pendingField(audit, "reason") == "ota-terminal-outbox-v1" &&
        pendingField(audit, "phase") == "terminal-journal" &&
        pendingField(audit, "evidence") == "verified-health";
}

std::string acknowledgeTerminalIntents(const std::string& stagingDir, const std::string& snapshot) {
    std::istringstream input(snapshot);
    std::string retained;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() != '{') continue;
        bool keep = false;
        try {
            const auto status = decodePendingStatus(line);
            if (status.stage != "completed" && status.stage != "failed") continue;
            const auto auditPath = joinPath(stagingDir, "ota_reconcile_" + status.jobId + ".json");
            if (strictlyAbsent(auditPath)) continue;
            const auto intentPath = pendingJobPath(stagingDir, status.jobId);
            if (strictlyAbsent(intentPath)) throw std::runtime_error("missing OTA terminal intent");
            const auto job = decodePendingJob(readPendingJournal(intentPath));
            if (job.token != status.occurrenceId) {
                if (hasValidatedStatusSupersession(stagingDir, snapshot, status, job)) continue;
                throw std::runtime_error("OTA terminal identity changed");
            }
            if (job.machineCode != status.machineCode)
                throw std::runtime_error("OTA terminal machine changed");
            const bool pending = needsFinalizationReconciliation(stagingDir, job);
            if (!hasValidFinal(stagingDir, job) ||
                (job.lifecycle == "systemd-handoff-v1" && !hasHandoffResolution(stagingDir, job)) ||
                readPendingJournal(joinPath(stagingDir, "ota_final_" + job.jobId + ".txt")) !=
                    "jobId=" + job.jobId + "\ntoken=" + job.token + "\nstatus=" + status.stage + "\n")
                throw std::runtime_error("OTA terminal final evidence changed");
            const auto audit = json::JsonParser(readPendingJournal(auditPath), 4, 32).parse();
            const auto healthPath = joinPath(stagingDir, "health_" + job.jobId + ".txt");
            if (!strictlyAbsent(healthPath)) {
                const auto health = decodeHealthResult(readPendingJournal(healthPath), job);
                if ((status.stage == "completed") != (health.status == "healthy"))
                    throw std::runtime_error("OTA terminal health changed");
            } else if (job.lifecycle == "systemd-handoff-v1" || status.stage != "completed" ||
                       pendingField(audit, "evidence") != "apply-returned-success") {
                throw std::runtime_error("OTA terminal health is missing");
            }
            if (pending) {
                const auto reason = pendingField(audit, "reason");
                const auto phase = pendingField(audit, "phase");
                const auto evidence = pendingField(audit, "evidence");
                const auto timestamp = pendingInteger<std::int64_t>(pendingField(audit, "ts"));
                persistFinalizationAudit(stagingDir, job, true, phase.c_str(), evidence.c_str(),
                    reason.c_str(), false, timestamp);
            }
        } catch (const std::exception&) {
            keep = true;
        }
        if (keep) retained += line + '\n';
    }
    return retained;
}

bool isActiveJob(const std::string& stagingDir, const PendingOtaJob& job) {
    const auto path = joinPath(stagingDir, "ota_active_job.txt");
    if (fileSizeBytes(path) > 256) return false;
    return readPendingJournal(path) == "jobId=" + job.jobId + "\ntoken=" + job.token + "\n";
}

bool hasHandoffResolution(const std::string& stagingDir, const PendingOtaJob& job) {
    const auto path = joinPath(stagingDir, "ota_handoff_" + job.jobId + ".json");
    if (strictlyAbsent(path) || fileSizeBytes(path) > 16384) return false;
#ifndef _WIN32
    struct stat info{};
    if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != 0 ||
        info.st_nlink != 1 || (info.st_mode & 0022)) return false;
#endif
    const auto bytes = readPendingJournal(path);
    if (bytes.empty() || bytes.back() != '\n') return false;
    const auto record = json::JsonParser(bytes, 6, 96).parse();
    const auto* mutation = record.find("runtimeMayChange");
    const bool installed = pendingField(record, "phase") == "OWNER_FINISHED";
    const auto* proof = record.find(installed ? "completionProof" : "cancelProof");
    if (pendingField(record, "format") != "ota-handoff-v1" || pendingField(record, "jobId") != job.jobId ||
        pendingField(record, "token") != job.token || pendingField(record, "version") != job.version ||
        (!installed && pendingField(record, "phase") != "CANCELLED") || !mutation || !mutation->isBool() ||
        mutation->asBool() != installed ||
        !proof || !proof->isObject()) return false;
    for (const auto* object : {&record, proof}) {
        std::set<std::string> names;
        for (const auto& member : object->asObject().values)
            if (!names.insert(member.key).second) return false;
    }
    const auto* noWriter = proof->find("noWriter");
    if (!installed && !(noWriter && noWriter->isBool() && noWriter->asBool() &&
        pendingField(*proof, "unit") == "ota-runtime-" + job.jobId + "-" + job.token + ".service" &&
        pendingField(*proof, "mainPid") == "0" && pendingField(*proof, "job") == "")) return false;
    const auto healthPath = joinPath(stagingDir, "health_" + job.jobId + ".txt");
    if (strictlyAbsent(healthPath) || fileSizeBytes(healthPath) == 0 || fileSizeBytes(healthPath) > 4096)
        return false;
    const auto healthBytes = readPendingJournal(healthPath);
    const auto health = decodeHealthResult(healthBytes, job);
    if (installed) {
        const auto tx = pendingField(*proof, "transaction");
        if (proof->asObject().values.size() != 5 || tx.size() != 36 || tx.compare(0, 4, "OTA_") != 0 ||
            !std::all_of(tx.begin() + 4, tx.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
            tx != pendingField(record, "productionTransaction") ||
            (job.packageType == "config" && health.status == "healthy" &&
                health.productionTransaction != tx) ||
            !isSafeSha256(pendingField(*proof, "healthSha256")) ||
            pendingField(*proof, "healthRecord") != healthBytes ||
            pendingField(*proof, "status") != health.status || pendingField(*proof, "rollback") != health.rollback ||
            !((health.status == "healthy" && health.rollback == "none") ||
              (health.status == "failed" && health.rollback == "succeeded"))) return false;
    } else if (health.status != "failed" || health.rollback != "unchanged") return false;
    const auto finalPath = joinPath(stagingDir, "ota_final_" + job.jobId + ".txt");
    const auto terminal = health.status == "healthy" ? "completed" : "failed";
    return strictlyAbsent(finalPath) || (fileSizeBytes(finalPath) <= 256 &&
        readPendingJournal(finalPath) == "jobId=" + job.jobId + "\ntoken=" + job.token + "\nstatus=" + terminal + "\n");
}

std::vector<std::string> pendingJobFiles(const std::string& stagingDir) {
    std::vector<std::string> files;
    for (const auto& path : listFilesInDirectory(stagingDir)) {
        const auto name = fileNameOf(path);
        if (name.compare(0, 8, "ota_job_") == 0 && name.size() > 13 &&
            name.compare(name.size() - 5, 5, ".json") == 0) files.push_back(path);
    }
    return files;
}

#ifndef _WIN32
std::string jobLifecyclePath(const std::string& stagingDir, const PendingOtaJob& job) {
    return joinPath(stagingDir, "ota_job_" + job.jobId + ".lock");
}

bool matchesLifecycleLease(const PendingOtaJob& job, int fd) {
    if (job.lifecycle != "systemd-handoff-v1") return true;
    struct stat info{};
    return fd >= 0 && job.leaseDev != 0 && job.leaseInode != 0 &&
        fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
        static_cast<std::uint64_t>(info.st_dev) == job.leaseDev &&
        static_cast<std::uint64_t>(info.st_ino) == job.leaseInode;
}

std::string readRecoveryEvidence(const std::string& path, std::size_t maximum) {
    struct stat info{};
    if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 1 || static_cast<std::uint64_t>(info.st_size) > maximum)
        throw std::runtime_error("invalid OTA recovery evidence");
    return readPendingJournal(path);
}

bool recordPreparedFailure(const std::string& stagingDir, const PendingOtaJob& job) {
    if (job.lifecycle != "inherited-flock-v1") return false;
    const auto barrierPath = joinPath(stagingDir, "ota_barrier_" + job.jobId + ".json");
    if (strictlyAbsent(barrierPath)) return false;
    const auto bytes = readRecoveryEvidence(barrierPath, 4096);
    if (bytes.empty() || bytes.back() != '\n') return false;
    const auto barrier = json::JsonParser(bytes, 4, 32).parse();
    if (pendingField(barrier, "format") != "official-ota-apply-barrier-v1" ||
        pendingField(barrier, "jobId") != job.jobId ||
        pendingField(barrier, "version") != job.version ||
        pendingField(barrier, "token") != job.token ||
        pendingField(barrier, "state") != "prepared") return false;
    // Absence only supplements a positive, job-bound protocol/barrier proof.
    const std::vector<std::string> exclusions{"ota-in-progress", "health_" + job.jobId + ".txt",
        "rollback_" + job.jobId + ".txt", "rollback_" + job.jobId + "_proof.txt",
        "ota_final_" + job.jobId + ".txt", job.jobId + "/health_pending.txt"};
    for (const auto& name : exclusions) {
        if (!strictlyAbsent(joinPath(stagingDir, name))) return false;
    }
    const auto appliedPath = joinPath(stagingDir, "applied_jobs.txt");
    if (!strictlyAbsent(appliedPath)) {
        const auto applied = readRecoveryEvidence(appliedPath, 16 * 1024 * 1024);
        if (applied.empty() || applied.back() != '\n') return false;
        std::istringstream lines(applied);
        std::string line;
        while (std::getline(lines, line)) {
            if (!isSafeOtaId(line) || line == job.jobId) return false;
        }
    }
    replacePendingJournal(joinPath(stagingDir, "health_" + job.jobId + ".txt"),
        "{\"format\":\"ota-health-v1\",\"jobId\":" + pendingJsonString(job.jobId) +
        ",\"version\":" + pendingJsonString(job.version) +
        ",\"token\":" + pendingJsonString(job.token) +
        ",\"status\":\"failed\",\"rollback\":\"unchanged\",\"ts\":\"" +
        std::to_string(currentTimeMs()) + "\"}\n");
    return true;
}
#endif

void OtaService::ensureNoUnresolvedJob() const {
    for (const auto& path : pendingJobFiles(config_.stagingDir)) {
        if (fileSizeBytes(path) > 4096) throw std::runtime_error("invalid OTA job intent");
        const auto job = decodePendingJob(readPendingJournal(path));
        if (path != pendingJobPath(config_.stagingDir, job.jobId))
            throw std::runtime_error("invalid OTA job intent path");
#ifndef _WIN32
        std::unique_ptr<JobLifecycleLock> lease;
        if (!job.lifecycle.empty()) {
            lease.reset(new JobLifecycleLock(jobLifecyclePath(config_.stagingDir, job), false));
            if (lease->fd() < 0) throw std::runtime_error("previous OTA writer is still running");
            if (!matchesLifecycleLease(job, lease->fd()))
                throw std::runtime_error("previous OTA lifecycle lease identity changed");
        }
#endif
        if (hasUnresolvedExecutionAudit(config_.stagingDir, job))
            throw std::runtime_error("previous successful OTA requires finalization reconciliation");
        if (!hasValidFinal(config_.stagingDir, job))
            throw std::runtime_error("previous OTA job has no verified terminal result");
        if (job.lifecycle == "systemd-handoff-v1" && !hasHandoffResolution(config_.stagingDir, job))
            throw std::runtime_error("runtime handoff is unresolved");
        const auto healthPath = joinPath(config_.stagingDir, "health_" + job.jobId + ".txt");
        const auto final = readPendingJournal(joinPath(config_.stagingDir, "ota_final_" + job.jobId + ".txt"));
        if (final == "jobId=" + job.jobId + "\ntoken=" + job.token + "\nstatus=completed\n" &&
            !pathExists(healthPath)) continue;
        if (fileSizeBytes(healthPath) == 0 || fileSizeBytes(healthPath) > 4096)
            throw std::runtime_error("previous OTA job has no verified safe result");
        const auto health = decodeHealthResult(readPendingJournal(healthPath), job);
        if ((final.find("status=completed\n") != std::string::npos) != (health.status == "healthy"))
            throw std::runtime_error("OTA terminal status conflicts with health result");
        if (health.status == "failed" && health.rollback != "succeeded" && health.rollback != "unchanged")
            throw std::runtime_error("previous OTA rollback is unresolved");
    }
}

void OtaService::reconcilePendingJobs() const {
    ensureDirectory(config_.stagingDir);
    // Scan/Direct query call here: only bounded records, never systemd/process waits.
    PendingJournalLock stateLock(joinPath(config_.stagingDir, "ota_state"));
    for (const auto& path : pendingJobFiles(config_.stagingDir)) {
        PendingOtaJob job;
        OtaHealthResult health;
        bool reconciliationNeeded = false;
        bool synchronousTail = false;
        bool deliveryPending = false;
#ifndef _WIN32
        std::unique_ptr<JobLifecycleLock> lease;
#endif
        try {
            if (fileSizeBytes(path) > 4096) continue;
            job = decodePendingJob(readPendingJournal(path));
            if (job.lifecycle == "systemd-handoff-v1" &&
                !hasHandoffResolution(config_.stagingDir, job)) continue;
            if (path != pendingJobPath(config_.stagingDir, job.jobId)) continue;
            bool synchronousEvidence = false;
            bool terminalIntent = false;
            reconciliationNeeded = needsFinalizationReconciliation(config_.stagingDir, job,
                &synchronousEvidence, &terminalIntent, &deliveryPending);
            if (hasValidFinal(config_.stagingDir, job) && !reconciliationNeeded) {
                if (!strictlyAbsent(statusSupersessionPath(config_.stagingDir, job))) {
                    const auto snapshot = readPendingJournal(statusJournalPath());
                    if (!hasBoundSupersedingTerminal(snapshot, job)) {
                        std::istringstream rows(snapshot);
                        std::string row;
                        while (std::getline(rows, row)) {
                            try {
                                if (row.empty() || row.front() != '{') continue;
                                const auto old = decodePendingStatus(row);
                                if (!hasValidatedStatusSupersession(config_.stagingDir, snapshot,
                                        old, job, false)) continue;
                                const auto health = decodeHealthResult(readPendingJournal(joinPath(
                                    config_.stagingDir, "health_" + job.jobId + ".txt")), job);
                                OtaStatus replacement;
                                replacement.jobId = job.jobId;
                                replacement.machineCode = job.machineCode;
                                replacement.stage = "failed";
                                replacement.message = health.rollback == "unchanged"
                                    ? "OTA rejected before runtime change" : "OTA failed; rollback " + health.rollback;
                                replacement.ts = health.ts;
                                replacement.occurrenceId = job.token;
                                appendPendingStatus(replacement);
                                break;
                            } catch (const std::exception&) {
                                // Leave an unknown old terminal intact for later verified replay.
                            }
                        }
                    }
                }
                continue;
            }
            if (deliveryPending) {
                if (!hasValidFinal(config_.stagingDir, job)) continue;
#ifndef _WIN32
                syncJournalAncestors(config_.stagingDir);
#endif
            } else if (!isActiveJob(config_.stagingDir, job)) continue;
#ifndef _WIN32
            if (!deliveryPending && !job.lifecycle.empty()) {
                lease.reset(new JobLifecycleLock(jobLifecyclePath(config_.stagingDir, job), false));
                if (lease->fd() < 0) continue;
                if (!matchesLifecycleLease(job, lease->fd())) continue;
            }
#endif
            const auto healthPath = joinPath(config_.stagingDir, "health_" + job.jobId + ".txt");
#ifndef _WIN32
            if (synchronousEvidence) {
                const auto finalPath = joinPath(config_.stagingDir, "ota_final_" + job.jobId + ".txt");
                if (!strictlyAbsent(finalPath) &&
                    (!hasValidFinal(config_.stagingDir, job) ||
                     readPendingJournal(finalPath) != "jobId=" + job.jobId + "\ntoken=" + job.token +
                         "\nstatus=completed\n")) continue;
            }
            if (synchronousEvidence && lease && job.lifecycle == "inherited-flock-v1" &&
                strictlyAbsent(healthPath) &&
                strictlyAbsent(joinPath(config_.stagingDir, job.jobId + "/health_pending.txt")) &&
                strictlyAbsent(joinPath(config_.stagingDir, "ota-in-progress"))) {
                synchronousTail = true;
            }
            if (synchronousEvidence && deliveryPending && strictlyAbsent(healthPath))
                synchronousTail = true;
            if (lease && !reconciliationNeeded && strictlyAbsent(healthPath)) recordPreparedFailure(config_.stagingDir, job);
#endif
            if (!synchronousTail) {
                if (fileSizeBytes(healthPath) == 0 || fileSizeBytes(healthPath) > 4096) continue;
                health = decodeHealthResult(readPendingJournal(healthPath), job);
                if (reconciliationNeeded && health.status != "healthy" && !terminalIntent) continue;
            }
        } catch (const std::exception&) {
            // A missing, stale or partial result cannot authorize success.
            continue;
        }
        OtaRequest request;
        request.jobId = job.jobId;
        request.version = job.version;
        OtaStatus terminal;
        terminal.jobId = job.jobId;
        terminal.machineCode = job.machineCode;
        const bool successful = synchronousTail || health.status == "healthy";
        terminal.stage = successful ? "completed" : "failed";
        terminal.progress = successful ? 100 : 0;
        terminal.message = synchronousTail ? "upgrade completed after synchronous finalization reconciliation" :
            health.status == "healthy" ? "upgrade completed after health verification" :
            health.rollback == "unchanged" ? "OTA rejected before runtime change" :
            "OTA failed; rollback " + health.rollback;
        terminal.ts = synchronousTail ? finalizationAuditTimestamp(config_.stagingDir, job) : health.ts;
        terminal.occurrenceId = job.token;
        bindProductionCompletion(terminal, job, health);
        if (deliveryPending) {
            const auto final = readPendingJournal(joinPath(config_.stagingDir,
                "ota_final_" + job.jobId + ".txt"));
            if (final != "jobId=" + job.jobId + "\ntoken=" + job.token +
                "\nstatus=" + terminal.stage + "\n") continue;
            try {
                appendPendingStatus(terminal);
            } catch (const OtaTerminalBacklog&) {
                // The older terminal must be replayed before this durable intent can fit.
            } catch (const std::exception&) {
                // A historical delivery fault must not invalidate the active job's
                // independent finalization. Its durable outbox remains for replay.
                if (isActiveJob(config_.stagingDir, job)) throw;
            }
            continue;
        }
        const auto historyPath = joinPath(config_.stagingDir, "upgrade_history.log");
        const auto recorded = [&](const std::string& result) {
            std::ifstream history(historyPath.c_str());
            std::string line;
            const auto needle = ",jobId=" + sanitizeJournalField(job.jobId) + ",";
            while (std::getline(history, line)) {
                if (line.find(",result=" + result + ",") != std::string::npos &&
                    line.find(needle) != std::string::npos) return true;
            }
            return false;
        };
        bool finalCommitted = false;
        bool outboxWritten = false;
        const char* phase = "version";
        try {
            if (successful) {
                writeVersionMarker(request, job.artifactPath, job.previousVersion);
                replacePendingJournal(joinPath(config_.stagingDir, "applied_version.txt"), job.version + "\n");
                phase = "history";
                if (!recorded("success"))
                    appendUpgradeRecord(request, job.artifactPath, job.previousVersion, terminal.ts);
            } else if (!recorded("failure")) {
                appendFailureRecord(request, "health", terminal.message,
                    health.rollback == "succeeded" || health.rollback == "failed",
                    health.rollback == "succeeded", health.ts);
            }
            if (!reconciliationNeeded) {
                phase = "outbox-intent";
                persistFinalizationAudit(config_.stagingDir, job, false, "final-marker",
                    "verified-health", "ota-terminal-outbox-v1");
                outboxWritten = true;
            }
            phase = "final-marker";
            replacePendingJournal(joinPath(config_.stagingDir, "ota_final_" + job.jobId + ".txt"),
                "jobId=" + job.jobId + "\ntoken=" + job.token + "\nstatus=" + terminal.stage + "\n");
            finalCommitted = true;
            phase = "delivery-intent";
            persistFinalizationAudit(config_.stagingDir, job, false, "terminal-journal",
                synchronousTail ? "apply-returned-success" : "verified-health",
                "ota-terminal-outbox-v1", true, synchronousTail ? terminal.ts : 0);
            outboxWritten = true;
            phase = "terminal-journal";
            appendPendingStatus(terminal);
        } catch (...) {
            if (!successful) throw;
            // The original bounded sync-tail audit remains the recovery authority.
            // Replacing it with an audit-update/terminal-journal phase would lose that proof.
            if (synchronousTail || reconciliationNeeded || outboxWritten)
                throw SuccessfulFinalizationError(job);
            // Keep ota_state held until the uncertainty is durably recorded.
            auditSuccessfulFinalization(config_.stagingDir, job, finalCommitted, phase, "verified-health");
            throw SuccessfulFinalizationError(job);
        }
    }
}

std::vector<OtaStatus> OtaService::loadFinalStatuses() const {
    std::vector<OtaStatus> statuses;
    ensureDirectory(config_.stagingDir);
    PendingJournalLock stateLock(joinPath(config_.stagingDir, "ota_state"));
    for (const auto& path : pendingJobFiles(config_.stagingDir)) {
        try {
            if (fileSizeBytes(path) > 4096) continue;
            const auto job = decodePendingJob(readPendingJournal(path));
            if (job.lifecycle == "systemd-handoff-v1" && !hasHandoffResolution(config_.stagingDir, job)) continue;
            if (path != pendingJobPath(config_.stagingDir, job.jobId) ||
                hasUnresolvedExecutionAudit(config_.stagingDir, job) ||
                !hasValidFinal(config_.stagingDir, job)) continue;
            const auto healthPath = joinPath(config_.stagingDir, "health_" + job.jobId + ".txt");
            OtaStatus status;
            status.jobId = job.jobId;
            status.machineCode = job.machineCode;
            status.occurrenceId = job.token;
            if (fileSizeBytes(healthPath) > 0 && fileSizeBytes(healthPath) <= 4096) {
                const auto health = decodeHealthResult(readPendingJournal(healthPath), job);
                status.stage = health.status == "healthy" ? "completed" : "failed";
                status.progress = health.status == "healthy" ? 100 : 0;
                status.message = health.status == "healthy" ? "upgrade completed after health verification" :
                    health.rollback == "unchanged" ? "OTA rejected before runtime change" :
                    "OTA failed; rollback " + health.rollback;
                status.ts = health.ts;
                bindProductionCompletion(status, job, health);
            } else {
                const auto final = readPendingJournal(joinPath(config_.stagingDir,
                    "ota_final_" + job.jobId + ".txt"));
                if (final.find("status=completed\n") == std::string::npos) continue;
                status.stage = "completed";
                status.progress = 100;
                status.message = "upgrade completed";
                const auto auditPath = joinPath(config_.stagingDir,
                    "ota_reconcile_" + job.jobId + ".json");
                if (!strictlyAbsent(auditPath))
                    status.ts = finalizationAuditTimestamp(config_.stagingDir, job);
            }
            statuses.push_back(std::move(status));
        } catch (const std::exception&) {
            // Incomplete or stale state cannot be shown as terminal.
        }
    }
    return statuses;
}

namespace {

struct ReadOnlyEvidenceFile {
    std::string bytes;
    bool present = false;
#ifndef _WIN32
    dev_t dev = 0;
    ino_t inode = 0;
#endif
};

ReadOnlyEvidenceFile readOriginalEvidenceFile(int directoryFd, const std::string& name, std::size_t limit,
                                             std::size_t& remaining, std::size_t* displayReadBudget = nullptr) {
    ReadOnlyEvidenceFile result;
#ifdef _WIN32
    (void)directoryFd; (void)name; (void)limit; (void)remaining; (void)displayReadBudget;
    return result;
#else
    const int fd = openat(directoryFd, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) return result;
        throw std::runtime_error("cannot open OTA query evidence");
    }
    try {
        struct stat before{}, after{}, named{};
        if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_uid != 0 ||
            before.st_nlink != 1 || (before.st_mode & 0022) || before.st_size < 1 ||
            static_cast<std::uint64_t>(before.st_size) > limit ||
            static_cast<std::uint64_t>(before.st_size) > remaining)
            throw std::runtime_error("invalid OTA query evidence metadata");
        if (displayReadBudget) {
            if (static_cast<std::uint64_t>(before.st_size) > *displayReadBudget) {
                *displayReadBudget = 0;
                throw std::runtime_error("OTA display read budget exhausted");
            }
            *displayReadBudget -= static_cast<std::size_t>(before.st_size);
        }
        result.bytes.resize(static_cast<std::size_t>(before.st_size));
        std::size_t offset = 0;
        while (offset < result.bytes.size()) {
            const auto count = read(fd, &result.bytes[offset], result.bytes.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error("short OTA query evidence");
            offset += static_cast<std::size_t>(count);
        }
        char extra;
        if (read(fd, &extra, 1) != 0 || fstat(fd, &after) != 0 ||
            fstatat(directoryFd, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(named.st_mode) ||
            before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
            before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
            before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
            before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
            before.st_ctim.tv_nsec != after.st_ctim.tv_nsec ||
            before.st_dev != named.st_dev || before.st_ino != named.st_ino)
            throw std::runtime_error("changing OTA query evidence");
        result.present = true;
        result.dev = before.st_dev;
        result.inode = before.st_ino;
        remaining -= result.bytes.size();
        close(fd);
        return result;
    } catch (...) { close(fd); throw; }
#endif
}

using OriginalEvidenceSnapshot = std::map<std::string, ReadOnlyEvidenceFile>;

#ifdef OTA_QUERY_FAULT_TEST
std::function<void()> queryAfterFirstSnapshot;
#endif

OriginalEvidenceSnapshot readOriginalEvidenceSnapshot(const std::string& stagingDir,
                                                      const std::string& jobId,
                                                      std::size_t* displayReadBudget = nullptr) {
#ifndef _WIN32
    struct stat root{};
    const int directoryFd = open(stagingDir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directoryFd < 0) throw std::runtime_error("cannot open OTA query directory");
    if (fstat(directoryFd, &root) != 0 || !S_ISDIR(root.st_mode) ||
        root.st_uid != 0 || (root.st_mode & 0022)) {
        close(directoryFd);
        throw std::runtime_error("untrusted OTA query directory");
    }
#else
    const int directoryFd = -1;
#endif
    const std::vector<std::pair<std::string, std::size_t>> names{
        {"ota_job_" + jobId + ".json", 4096},
        {"ota_handoff_" + jobId + ".json", 16384},
        {"health_" + jobId + ".txt", 4096},
        {"ota_final_" + jobId + ".txt", 256},
        {"ota_reconcile_" + jobId + ".json", 4096}
    };
    std::size_t remaining = 32768;
    OriginalEvidenceSnapshot snapshot;
    try {
        for (const auto& item : names)
            snapshot.emplace(item.first, readOriginalEvidenceFile(directoryFd, item.first,
                item.second, remaining, displayReadBudget));
#ifndef _WIN32
        struct stat after{};
        if (fstat(directoryFd, &after) != 0 || root.st_dev != after.st_dev ||
            root.st_ino != after.st_ino || root.st_mtim.tv_sec != after.st_mtim.tv_sec ||
            root.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
            root.st_ctim.tv_sec != after.st_ctim.tv_sec ||
            root.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
            throw std::runtime_error("changing OTA query directory");
        ReadOnlyEvidenceFile identity;
        identity.present = true;
        identity.dev = root.st_dev;
        identity.inode = root.st_ino;
        snapshot.emplace("", identity);
        close(directoryFd);
#endif
    } catch (...) {
#ifndef _WIN32
        close(directoryFd);
#endif
        throw;
    }
    return snapshot;
}

bool sameOriginalEvidence(const OriginalEvidenceSnapshot& first,
                          const OriginalEvidenceSnapshot& second) {
    if (first.size() != second.size()) return false;
    for (const auto& item : first) {
        const auto other = second.find(item.first);
        if (other == second.end() || item.second.present != other->second.present ||
            item.second.bytes != other->second.bytes) return false;
#ifndef _WIN32
        if (item.second.dev != other->second.dev || item.second.inode != other->second.inode)
            return false;
#endif
    }
    return true;
}

bool validOccurrence(const std::string& token) {
    if (token.size() != 36) return false;
    for (std::size_t i = 0; i < token.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (token[i] != '-') return false;
        } else if (!((token[i] >= '0' && token[i] <= '9') ||
                     (token[i] >= 'a' && token[i] <= 'f'))) return false;
    }
    return true;
}

void requireUniqueQueryJson(const json::JsonValue& value) {
    if (value.isObject()) {
        std::set<std::string> names;
        for (const auto& member : value.asObject().values) {
            if (!member.value || !names.insert(member.key).second)
                throw std::runtime_error("ambiguous OTA query JSON");
            requireUniqueQueryJson(*member.value);
        }
    } else if (value.isArray()) {
        for (const auto& child : value.asArray().values) {
            if (!child) throw std::runtime_error("invalid OTA query JSON array");
            requireUniqueQueryJson(*child);
        }
    }
}

std::int64_t validateQueryAudit(const json::JsonValue& audit, const PendingOtaJob& job) {
    requireUniqueQueryJson(audit);
    const std::vector<std::string> fields{
        "format", "reason", "jobId", "token", "version", "finalState", "phase", "evidence", "ts"};
    if (!audit.isObject() || audit.asObject().values.size() != fields.size())
        throw std::runtime_error("invalid OTA query audit fields");
    for (const auto& name : fields) (void)pendingField(audit, name.c_str());
    const auto reason = pendingField(audit, "reason");
    const auto state = pendingField(audit, "finalState");
    const auto phase = pendingField(audit, "phase");
    const auto evidence = pendingField(audit, "evidence");
    const auto timestamp = pendingInteger<std::int64_t>(pendingField(audit, "ts"));
    if (pendingField(audit, "format") != "ota-reconcile-v1" ||
        (reason != "successful-apply-finalization-error" && reason != "ota-terminal-outbox-v1") ||
        pendingField(audit, "jobId") != job.jobId ||
        pendingField(audit, "token") != job.token ||
        pendingField(audit, "version") != job.version || timestamp <= 0 ||
        (state != "committed" && state != "unknown" && state != "delivery-pending") ||
        (state == "delivery-pending" && (reason != "ota-terminal-outbox-v1" ||
            phase != "terminal-journal")) ||
        (evidence != "verified-health" && evidence != "apply-returned-success"))
        throw std::runtime_error("OTA query audit does not match intent");
    return timestamp;
}

bool sameQueryJson(const json::JsonValue& left, const json::JsonValue& right) {
    if (left.type() != right.type()) return false;
    if (left.isNull()) return true;
    if (left.isBool()) return left.asBool() == right.asBool();
    if (left.isString()) return left.asString() == right.asString();
    if (left.isNumber()) return left.asNumber() == right.asNumber();
    if (left.isArray()) {
        const auto& a = left.asArray().values;
        const auto& b = right.asArray().values;
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (!a[i] || !b[i] || !sameQueryJson(*a[i], *b[i])) return false;
        return true;
    }
    const auto& a = left.asObject().values;
    const auto& b = right.asObject().values;
    if (a.size() != b.size()) return false;
    for (const auto& item : a) {
        const auto* other = right.find(item.key);
        if (!item.value || !other || !sameQueryJson(*item.value, *other)) return false;
    }
    return true;
}

#ifndef _WIN32
int openTrustedQueryDirectory(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info{};
    if (fd < 0 || fstat(fd, &info) != 0 || !S_ISDIR(info.st_mode) ||
        info.st_uid != 0 || (info.st_mode & 0022)) {
        if (fd >= 0) close(fd);
        throw std::runtime_error("untrusted production query directory");
    }
    return fd;
}

int openTrustedQueryChild(int parent, const std::string& name) {
    if (!isSafeOtaId(name)) throw std::runtime_error("invalid production query directory name");
    const int fd = openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info{};
    if (fd < 0 || fstat(fd, &info) != 0 || !S_ISDIR(info.st_mode) ||
        info.st_uid != 0 || (info.st_mode & 0022)) {
        if (fd >= 0) close(fd);
        throw std::runtime_error("untrusted production query subdirectory");
    }
    return fd;
}

struct QueryDirectoryFd {
    explicit QueryDirectoryFd(int value) : value(value) {}
    ~QueryDirectoryFd() { if (value >= 0) close(value); }
    QueryDirectoryFd(const QueryDirectoryFd&) = delete;
    QueryDirectoryFd& operator=(const QueryDirectoryFd&) = delete;
    int value;
};

struct ProductionQueryEvidence {
    std::string approval, candidate, previous, state, health, priorPointer, lineage, baseline;
    dev_t rootDev = 0;
    ino_t rootInode = 0;
    dev_t transactionDev = 0;
    ino_t transactionInode = 0;
};

ProductionQueryEvidence readProductionQueryEvidence(const std::string& home,
    const std::string& transaction, const std::string& baselineId, bool healthy,
    std::size_t* displayReadBudget = nullptr) {
    const QueryDirectoryFd root(openTrustedQueryDirectory(home + "/data/runtime-production"));
    const QueryDirectoryFd transactions(openTrustedQueryChild(root.value, "transactions"));
    const QueryDirectoryFd tx(openTrustedQueryChild(transactions.value, transaction));
    struct stat rootInfo{}, txInfo{};
    if (fstat(root.value, &rootInfo) != 0 || fstat(tx.value, &txInfo) != 0)
        throw std::runtime_error("cannot inspect production query directories");
    ProductionQueryEvidence result;
    result.rootDev = rootInfo.st_dev; result.rootInode = rootInfo.st_ino;
    result.transactionDev = txInfo.st_dev; result.transactionInode = txInfo.st_ino;
    std::size_t remaining = 8 * 1024 * 1024;
    const auto exact = [&](int fd, const std::string& name) {
        const auto file = readOriginalEvidenceFile(fd, name, 1024 * 1024, remaining,
            displayReadBudget);
        if (!file.present) throw std::runtime_error("missing production query evidence");
        return file.bytes;
    };
    result.approval = exact(tx.value, "approval.json");
    result.candidate = exact(tx.value, "candidate.json");
    result.previous = exact(tx.value, "previous.json");
    result.state = exact(tx.value, "state.json");
    if (healthy) {
        result.health = exact(tx.value, "health.json");
        const QueryDirectoryFd lineage(openTrustedQueryChild(root.value, "boot-lineage"));
        const auto candidateSha = ota_query_detail::sha256Hex(result.candidate);
        result.lineage = exact(lineage.value, candidateSha + "-" + transaction + ".json");
    } else {
        result.priorPointer = exact(tx.value, "previous-pointer.json");
    }
    const QueryDirectoryFd baselines(openTrustedQueryChild(root.value, "baselines"));
    result.baseline = exact(baselines.value, baselineId + ".json");
    return result;
}

bool sameProductionQueryEvidence(const ProductionQueryEvidence& a, const ProductionQueryEvidence& b) {
    return a.approval == b.approval && a.candidate == b.candidate && a.previous == b.previous &&
        a.state == b.state && a.health == b.health && a.priorPointer == b.priorPointer &&
        a.lineage == b.lineage && a.baseline == b.baseline &&
        a.rootDev == b.rootDev && a.rootInode == b.rootInode &&
        a.transactionDev == b.transactionDev && a.transactionInode == b.transactionInode;
}

std::string productionCandidateForOriginalJob(const std::string& stagingDir,
    const PendingOtaJob& job, const json::JsonValue& handoff, const OtaHealthResult& otaHealth,
    std::size_t* displayReadBudget) {
    const std::string suffix = "/ota/staging";
    if (stagingDir.size() <= suffix.size() ||
        stagingDir.compare(stagingDir.size() - suffix.size(), suffix.size(), suffix) != 0)
        throw std::runtime_error("unbound production query home");
    const auto home = stagingDir.substr(0, stagingDir.size() - suffix.size());
    if (pendingField(handoff, "gatewayHome") != home)
        throw std::runtime_error("production query home mismatch");
    const auto transaction = "OTA_" + ota_query_detail::sha256Hex(job.jobId + job.token).substr(0, 32);
    const bool healthy = otaHealth.status == "healthy";
    // Read immutable identity first. The second pass below detects replacement or mixed reads.
    const QueryDirectoryFd root(openTrustedQueryDirectory(home + "/data/runtime-production"));
    const QueryDirectoryFd transactions(openTrustedQueryChild(root.value, "transactions"));
    const QueryDirectoryFd tx(openTrustedQueryChild(transactions.value, transaction));
    std::size_t remaining = 1024 * 1024;
    const auto candidateFile = readOriginalEvidenceFile(tx.value, healthy ? "candidate.json" : "previous.json",
        1024 * 1024, remaining, displayReadBudget);
    if (!candidateFile.present) throw std::runtime_error("missing production baseline");
    const auto selected = json::JsonParser(candidateFile.bytes, 12, 4096).parse();
    requireUniqueQueryJson(selected);
    const auto baselineId = pendingField(selected, "baselineId");
    if (!isSafeOtaId(baselineId)) throw std::runtime_error("invalid production baseline ID");
    const auto evidence = readProductionQueryEvidence(home, transaction, baselineId, healthy,
        displayReadBudget);
    const auto approval = json::JsonParser(evidence.approval, 12, 4096).parse();
    const auto candidate = json::JsonParser(evidence.candidate, 12, 4096).parse();
    const auto previous = json::JsonParser(evidence.previous, 12, 4096).parse();
    const auto state = json::JsonParser(evidence.state, 6, 128).parse();
    for (const auto* value : {&approval, &candidate, &previous, &state}) requireUniqueQueryJson(*value);
    const auto candidateSha = ota_query_detail::sha256Hex(evidence.candidate);
    const auto previousSha = ota_query_detail::sha256Hex(evidence.previous);
    const auto approvalSha = ota_query_detail::sha256Hex(evidence.approval);
    if (pendingField(handoff, "productionApprovalSha256") != approvalSha ||
        pendingField(state, "format") != "runtime-install-v1" ||
        pendingField(state, "transaction") != transaction ||
        pendingField(state, "approvalSha256") != approvalSha ||
        pendingField(state, "candidateSha256") != candidateSha ||
        pendingField(state, "previousSha256") != previousSha ||
        pendingField(approval, "transaction") != transaction ||
        pendingField(approval, "gatewayHome") != home ||
        pendingField(approval, "machineCode") != job.machineCode ||
        pendingField(candidate, "machineCode") != job.machineCode ||
        pendingField(candidate, "release") != job.version ||
        !approval.find("candidate") || !approval.find("previous") ||
        !sameQueryJson(*approval.find("candidate"), candidate) ||
        !sameQueryJson(*approval.find("previous"), previous) ||
        (job.packageType == "config" && healthy &&
            otaHealth.productionCandidateSha256 != candidateSha))
        throw std::runtime_error("production approval or candidate mismatch");
    if (healthy) {
        const auto health = json::JsonParser(evidence.health, 12, 4096).parse();
        const auto lineage = json::JsonParser(evidence.lineage, 6, 128).parse();
        requireUniqueQueryJson(health);
        requireUniqueQueryJson(lineage);
        if (pendingField(state, "phase") != "COMPLETED")
            throw std::runtime_error("production transaction not committed");
        if (pendingField(health, "status") != "healthy" ||
            pendingField(health, "transaction") != transaction ||
            pendingField(health, "baselineSha256") != candidateSha ||
            pendingField(lineage, "transaction") != transaction ||
            pendingField(lineage, "approvalSha256") != approvalSha ||
            pendingField(lineage, "candidateSha256") != candidateSha ||
            pendingField(lineage, "previousSha256") != previousSha ||
            evidence.baseline != evidence.candidate)
            throw std::runtime_error("production completion receipt mismatch");
    } else {
        const auto pointer = json::JsonParser(evidence.priorPointer, 4, 32).parse();
        requireUniqueQueryJson(pointer);
        const auto* adoption = state.find("adoption");
        if (pendingField(state, "phase") != "RECOVERED" ||
            !adoption || !adoption->isBool() || adoption->asBool() ||
            pendingField(pointer, "sha256") != previousSha ||
            pendingField(approval, "parentSha256") != previousSha ||
            evidence.baseline != evidence.previous)
            throw std::runtime_error("production rollback receipt mismatch");
    }
    if (!sameProductionQueryEvidence(evidence,
        readProductionQueryEvidence(home, transaction, baselineId, healthy, displayReadBudget)))
        throw std::runtime_error("mixed production query generation");
    return candidateSha;
}
#endif

bool legacyFinalForDisplay(const std::string& stagingDir, const std::string& jobId,
                           OtaStatus& status, std::size_t* displayReadBudget) {
    try {
        const auto snapshot = readOriginalEvidenceSnapshot(stagingDir, jobId, displayReadBudget);
        const auto intent = snapshot.at("ota_job_" + jobId + ".json");
        const auto healthFile = snapshot.at("health_" + jobId + ".txt");
        const auto finalFile = snapshot.at("ota_final_" + jobId + ".txt");
        if (!intent.present || !finalFile.present) return false;
        requireUniqueQueryJson(json::JsonParser(intent.bytes, 4, 32).parse());
        const auto job = decodePendingJob(intent.bytes);
        if (job.jobId != jobId || job.lifecycle == "systemd-handoff-v1" ||
            !isSafeOtaId(job.machineCode) || !validOccurrence(job.token)) return false;
        OtaHealthResult health;
        if (healthFile.present) {
            requireUniqueQueryJson(json::JsonParser(healthFile.bytes, 4, 32).parse());
            health = decodeHealthResult(healthFile.bytes, job);
        }
        const auto terminal = !healthFile.present || health.status == "healthy" ? "completed" : "failed";
        if (finalFile.bytes != "jobId=" + jobId + "\ntoken=" + job.token +
            "\nstatus=" + terminal + "\n") return false;
        const auto audit = snapshot.at("ota_reconcile_" + jobId + ".json");
        std::int64_t auditTs = 0;
        if (audit.present) {
            const auto record = json::JsonParser(audit.bytes, 4, 32).parse();
            auditTs = validateQueryAudit(record, job);
            if (pendingField(record, "finalState") == "unknown") return false;
        }
        if (!sameOriginalEvidence(snapshot,
            readOriginalEvidenceSnapshot(stagingDir, jobId, displayReadBudget))) return false;
        status.jobId = jobId;
        status.machineCode = job.machineCode;
        status.occurrenceId = job.token;
        status.stage = terminal;
        status.progress = terminal == "completed" ? 100 : 0;
        status.ts = healthFile.present ? health.ts : auditTs;
        status.message = !healthFile.present ? "upgrade completed" :
            terminal == "completed" ? "upgrade completed after health verification" :
            health.rollback == "unchanged" ? "OTA rejected before runtime change" :
            "OTA failed; rollback " + health.rollback;
        return true;
    } catch (const std::exception&) { return false; }
}

}  // namespace

OtaOriginalJobEvidence OtaService::readOriginalJob(const std::string& jobId) const {
    return readOriginalJobBounded(jobId, nullptr);
}

OtaOriginalJobEvidence OtaService::readOriginalJobBounded(const std::string& jobId,
    std::size_t* displayReadBudget) const {
    OtaOriginalJobEvidence result;
    result.jobId = jobId;
    if (!isSafeOtaId(jobId)) return result;
#ifdef _WIN32
    return result;
#else
    try {
        const auto snapshot = readOriginalEvidenceSnapshot(config_.stagingDir, jobId,
            displayReadBudget);
        const auto value = [&](const std::string& name) -> const std::string& {
            const auto& file = snapshot.at(name);
            if (!file.present || file.bytes.empty()) throw std::runtime_error("missing OTA query evidence");
            return file.bytes;
        };
        const auto intentBytes = value("ota_job_" + jobId + ".json");
        requireUniqueQueryJson(json::JsonParser(intentBytes, 4, 32).parse());
        const auto job = decodePendingJob(intentBytes);
        if (job.jobId != jobId || !isSafeOtaId(job.machineCode) || !validOccurrence(job.token) ||
            job.lifecycle != "systemd-handoff-v1")
            throw std::runtime_error("unsupported OTA query intent");
        const auto healthBytes = value("health_" + jobId + ".txt");
        requireUniqueQueryJson(json::JsonParser(healthBytes, 4, 32).parse());
        const auto health = decodeHealthResult(healthBytes, job);
        const auto handoffBytes = value("ota_handoff_" + jobId + ".json");
        if (handoffBytes.back() != '\n') throw std::runtime_error("incomplete OTA handoff");
        const auto handoff = json::JsonParser(handoffBytes, 6, 96).parse();
        requireUniqueQueryJson(handoff);
        if (pendingField(handoff, "format") != "ota-handoff-v1" ||
            pendingField(handoff, "jobId") != job.jobId ||
            pendingField(handoff, "token") != job.token ||
            pendingField(handoff, "version") != job.version ||
            pendingField(handoff, "intentSha256") != ota_query_detail::sha256Hex(intentBytes))
            throw std::runtime_error("OTA handoff does not bind intent");
        const auto final = value("ota_final_" + jobId + ".txt");
        const auto terminal = health.status == "healthy" ? "completed" : "failed";
        if (final != "jobId=" + job.jobId + "\ntoken=" + job.token + "\nstatus=" + terminal + "\n")
            throw std::runtime_error("OTA final marker does not bind health");
        const auto auditBytes = value("ota_reconcile_" + jobId + ".json");
        if (auditBytes.back() != '\n') throw std::runtime_error("incomplete OTA audit");
        const auto audit = json::JsonParser(auditBytes, 4, 32).parse();
        validateQueryAudit(audit, job);
        if (pendingField(audit, "finalState") == "unknown")
            throw std::runtime_error("unresolved OTA query audit");
        const auto state = pendingField(audit, "finalState");
        if (state != "committed" && state != "delivery-pending")
            throw std::runtime_error("invalid OTA query audit state");
        const auto phase = pendingField(handoff, "phase");
        const auto* mayChange = handoff.find("runtimeMayChange");
        if (!mayChange || !mayChange->isBool()) throw std::runtime_error("invalid OTA handoff phase");
        if (phase == "OWNER_FINISHED" && mayChange->asBool()) {
            const auto* proof = handoff.find("completionProof");
            if (!proof || !proof->isObject() || proof->asObject().values.size() != 5 ||
                pendingField(*proof, "status") != health.status ||
                pendingField(*proof, "rollback") != health.rollback ||
                pendingField(*proof, "healthRecord") != healthBytes ||
                pendingField(*proof, "healthSha256") != ota_query_detail::sha256Hex(healthBytes))
                throw std::runtime_error("OTA completion proof mismatch");
            const auto transaction = "OTA_" + ota_query_detail::sha256Hex(job.jobId + job.token).substr(0, 32);
            if (pendingField(*proof, "transaction") != transaction ||
                pendingField(handoff, "productionTransaction") != transaction ||
                (job.packageType == "config" && health.status == "healthy" &&
                    health.productionTransaction != transaction))
                throw std::runtime_error("OTA transaction mismatch");
            if (health.status == "healthy" && health.rollback == "none")
                result.outcome = "HEALTHY_APPLIED";
            else if (health.status == "failed" && health.rollback == "succeeded")
                result.outcome = "FAILED_ROLLED_BACK";
            if (result.outcome != "UNKNOWN") {
                result.productionTransaction = transaction;
                result.productionCandidateSha256 = productionCandidateForOriginalJob(
                    config_.stagingDir, job, handoff, health, displayReadBudget);
            }
        } else if (phase == "CANCELLED" && !mayChange->asBool() &&
                   health.status == "failed" && health.rollback == "unchanged") {
            const auto* proof = handoff.find("cancelProof");
            if (!proof || !proof->isObject() || !proof->find("noWriter") ||
                !proof->find("noWriter")->isBool() || !proof->find("noWriter")->asBool() ||
                pendingField(*proof, "unit") != "ota-runtime-" + job.jobId + "-" + job.token + ".service" ||
                pendingField(*proof, "mainPid") != "0" || pendingField(*proof, "job") != "")
                throw std::runtime_error("OTA unchanged proof mismatch");
            result.outcome = "REJECTED_UNCHANGED";
        }
        if (result.outcome == "UNKNOWN") return result;
#ifdef OTA_QUERY_FAULT_TEST
        if (queryAfterFirstSnapshot) queryAfterFirstSnapshot();
#endif
        if (!sameOriginalEvidence(snapshot, readOriginalEvidenceSnapshot(config_.stagingDir, jobId,
            displayReadBudget)))
            throw std::runtime_error("mixed OTA query generation");
        result.machineCode = job.machineCode;
        result.occurrenceId = job.token;
        result.version = job.version;
        result.artifactSha256 = job.artifactSha256;
        result.artifactSize = job.artifactSize;
        result.intentSha256 = ota_query_detail::sha256Hex(intentBytes);
        result.healthSha256 = ota_query_detail::sha256Hex(healthBytes);
        result.healthTs = health.ts;
    } catch (const std::exception&) {
        result = OtaOriginalJobEvidence{};
        result.jobId = jobId;
    }
    return result;
#endif
}

OtaOriginalJobEvidence OtaService::readOriginalJob(const std::string& jobId,
    const std::string& expectedMachineCode, const std::string& expectedOccurrenceId) const {
    OtaOriginalJobEvidence result;
    result.jobId = jobId;
    if (!isSafeOtaId(expectedMachineCode) || !validOccurrence(expectedOccurrenceId)) return result;
    const auto evidence = readOriginalJob(jobId);
    if (evidence.machineCode != expectedMachineCode ||
        evidence.occurrenceId != expectedOccurrenceId) return result;
    return evidence;
}

OtaQueryObservation OtaService::answerOriginalJobQuery(
    const OtaQueryChallenge& challenge, const std::string& expectedMachineCode) const {
    ota_query_protocol::validate(challenge);
    if (!config_.requireSignature || config_.querySigningPrivateKeyPath.empty() ||
        challenge.machineCode != expectedMachineCode ||
        !ota_query_crypto::verifyChallenge(challenge, config_.signaturePublicKeyPath))
        throw std::runtime_error("OTA query request not authenticated");
    const auto now = currentTimeMs();
    if (challenge.issuedAtMs > now + 30000 || challenge.expiresAtMs <= now)
        throw std::runtime_error("OTA query request expired");

    OtaQueryObservation result;
    result.jobId = challenge.jobId;
    result.machineCode = challenge.machineCode;
    result.nonce = challenge.nonce;
    const auto evidence = readOriginalJob(challenge.jobId);
    result.outcome = "UNKNOWN";
    const bool sameArtifact = evidence.machineCode == challenge.machineCode &&
        evidence.version == challenge.version &&
        evidence.artifactSha256 == challenge.artifactSha256 &&
        evidence.artifactSize == challenge.artifactSize;
    const bool committedProduction = evidence.outcome == "HEALTHY_APPLIED" ||
        evidence.outcome == "FAILED_ROLLED_BACK";
    const bool candidateBound = evidence.productionCandidateSha256 == challenge.candidateSha256 &&
        evidence.productionTransaction == "OTA_" +
            ota_query_detail::sha256Hex(challenge.jobId + evidence.occurrenceId).substr(0, 32);
    if (sameArtifact && (evidence.outcome == "REJECTED_UNCHANGED" ||
        (committedProduction && candidateBound))) {
        result.outcome = evidence.outcome;
        result.occurrenceId = evidence.occurrenceId;
        result.version = evidence.version;
        result.artifactSha256 = evidence.artifactSha256;
        result.artifactSize = evidence.artifactSize;
        result.intentSha256 = evidence.intentSha256;
        result.healthSha256 = evidence.healthSha256;
        result.healthTs = evidence.healthTs;
        if (committedProduction) {
            result.candidateSha256 = evidence.productionCandidateSha256;
            result.transaction = evidence.productionTransaction;
        }
    }
    result.signature = ota_query_crypto::signObservation(result, config_.querySigningPrivateKeyPath);
    return result;
}

OtaReadOnlyFinalStatuses OtaService::loadFinalStatusesReadOnly(
    std::size_t displayReadBudget) const {
    OtaReadOnlyFinalStatuses result;
#ifndef _WIN32
    std::size_t inspected = 0;
    DIR* directory = opendir(config_.stagingDir.c_str());
    if (!directory) {
        if (errno == ENOENT) return result;
        throw std::runtime_error("cannot open OTA query directory");
    }
    const auto addStatus = [&](OtaStatus status) {
        result.terminalJobs.push_back({status.jobId, status.machineCode, status.occurrenceId});
        result.items.push_back(std::move(status));
        if (result.items.size() <= 100) return;
        std::sort(result.items.begin(), result.items.end(), [](const auto& a, const auto& b) {
            return a.ts != b.ts ? a.ts < b.ts : a.jobId < b.jobId;
        });
        result.items.erase(result.items.begin());
        result.truncated = true;
    };
    try {
        for (;;) {
            errno = 0;
            dirent* item = readdir(directory);
            if (!item) {
                if (errno != 0) throw std::runtime_error("cannot enumerate OTA query directory");
                break;
            }
            if (++inspected > 4096) { result.truncated = true; break; }
            const std::string name(item->d_name);
            if (name.compare(0, 8, "ota_job_") != 0 || name.size() <= 13 ||
                name.compare(name.size() - 5, 5, ".json") != 0) continue;
            const auto jobId = name.substr(8, name.size() - 13);
            const auto proof = readOriginalJobBounded(jobId, &displayReadBudget);
            if (proof.outcome == "UNKNOWN") {
                OtaStatus legacy;
                if (legacyFinalForDisplay(config_.stagingDir, jobId, legacy, &displayReadBudget))
                    addStatus(std::move(legacy));
                if (displayReadBudget == 0) { result.truncated = true; break; }
                continue;
            }
            OtaStatus status;
            status.jobId = proof.jobId;
            status.machineCode = proof.machineCode;
            status.occurrenceId = proof.occurrenceId;
            status.stage = proof.outcome == "HEALTHY_APPLIED" ? "completed" : "failed";
            status.progress = status.stage == "completed" ? 100 : 0;
            status.ts = proof.healthTs;
            status.message = proof.outcome == "HEALTHY_APPLIED" ? "upgrade completed after health verification" :
                proof.outcome == "FAILED_ROLLED_BACK" ? "OTA failed; rollback succeeded" :
                "OTA rejected before runtime change";
            if (status.stage == "completed" && !proof.productionCandidateSha256.empty()) {
                status.appliedVersion = proof.version;
                status.appliedSha256 = proof.artifactSha256;
                status.productionTransaction = proof.productionTransaction;
                status.productionCandidateSha256 = proof.productionCandidateSha256;
                status.productionHealth = "healthy";
            }
            addStatus(std::move(status));
            if (displayReadBudget == 0) { result.truncated = true; break; }
        }
        closedir(directory);
    } catch (...) { closedir(directory); throw; }
#endif
    std::sort(result.items.begin(), result.items.end(), [](const auto& a, const auto& b) {
        return a.ts != b.ts ? a.ts < b.ts : a.jobId < b.jobId;
    });
    return result;
}

void OtaService::clearPendingStatuses() const {
    ensureDirectory(config_.stagingDir);
    PendingJournalLock stateLock(joinPath(config_.stagingDir, "ota_state"));
    std::lock_guard<std::mutex> guard(pendingMutex_);
    if (pendingSnapshot_.empty()) return;
    PendingJournalLock lock(statusJournalPath());
    const auto bytes = readPendingJournal(statusJournalPath());
    if (bytes.compare(0, pendingSnapshot_.size(), pendingSnapshot_) != 0)
        throw std::runtime_error("OTA pending snapshot changed; reload before clearing");
    const auto retained = acknowledgeTerminalIntents(config_.stagingDir, pendingSnapshot_);
    replacePendingJournal(statusJournalPath(), retained + bytes.substr(pendingSnapshot_.size()));
    pendingSnapshot_.clear();
}

OtaReply OtaService::createAcceptedReply(
    const OtaRequest& request,
    const std::string& machineCode,
    std::int64_t nowMs
) const {
    OtaReply reply;
    reply.jobId = request.jobId;
    reply.machineCode = machineCode;
    reply.accepted = true;
    reply.message = "accepted";
    reply.ts = nowMs;
    return reply;
}

void OtaService::execute(
    const OtaRequest& request,
    const std::string& machineCode,
    std::int64_t nowMs,
    OtaReply* reply,
    OtaStatus* status,
    const std::function<void(const OtaStatus&)>& publishStatus
) const {
    if (reply == nullptr || status == nullptr) {
        throw std::invalid_argument("ota reply/status is required");
    }
    const bool runtimeHandoff = fileNameOf(config_.applyScript) == "offline-runtime-upgrade.py";
#ifndef _WIN32
    std::unique_ptr<JobLifecycleLock> applyLease;
#endif
    try {
        rejectDuplicateRequest(request.jobId);
        if (!config_.enabled) throw std::runtime_error("ota is disabled");
        validateOtaRequestFields(request, config_);
        if (runtimeHandoff && ((request.packageType != "full" && request.packageType != "config") || !config_.requireSignature ||
                              !config_.checksumRequired || request.size == 0))
            throw std::runtime_error("runtime handoff requires a signed, sized full/config artifact");
#ifdef _WIN32
        if (runtimeHandoff) throw std::runtime_error("runtime handoff requires Linux systemd");
#endif
        validateOtaSize(config_, request.size);
        ensureDirectory(config_.downloadDir);
        ensureDirectory(config_.stagingDir);
        ensureDirectory(config_.backupDir);
#ifndef _WIN32
        applyLease.reset(new JobLifecycleLock(joinPath(config_.stagingDir, "ota_job_" + request.jobId + ".lock"), true));
        if (applyLease->fd() < 0)
            throw OtaRequestRejected(OtaRejectionReason::Duplicate, "OTA job execution already owned");
#endif
        PendingJournalLock stateLock(joinPath(config_.stagingDir, "ota_state"));
        ensureJobNotProcessed(request);
        ensureNoUnresolvedJob();
        if (!strictlyAbsent(joinPath(config_.stagingDir, "ota-in-progress")))
            throw std::runtime_error("previous OTA in-progress marker requires recovery");
    } catch (const OtaRequestRejected&) {
        throw;
    } catch (const std::exception& ex) {
        throw OtaRequestRejected(OtaRejectionReason::NotAdmitted, ex.what());
    }

    reply->jobId = request.jobId;
    reply->machineCode = machineCode;
    reply->accepted = true;
    reply->message = "accepted";
    reply->ts = nowMs;

    status->jobId = request.jobId;
    status->machineCode = machineCode;
    status->downloadedBytes = 0;
    status->totalBytes = request.size;
    std::string artifactPath;
    std::string previousVersion;
    const auto startedAt = currentTimeMs();
    bool intentPersisted = false;
    std::string ownToken;
    try {
        reportStage(status, "accepted", 0, "accepted", nowMs, publishStatus);
        artifactPath = resolveArtifactPath(request);
        previousVersion = readInstalledVersion();
        // The signed descriptor binds jobId/machineCode/version/sha256/size, so it
        // is checked before anything is downloaded; the sha256 check below then
        // binds the downloaded bytes to the signed descriptor.
        verifySignature(request, machineCode);
        // Download, backup copy and work-dir copy each need the artifact size.
        const auto expectedWrite = request.size > std::numeric_limits<std::uint64_t>::max() / 3
            ? std::numeric_limits<std::uint64_t>::max()
            : request.size * 3;
        ensureFreeDiskSpace(config_.downloadDir, config_.minFreeBytes, expectedWrite);

        downloadArtifact(request, artifactPath, status, publishStatus);
        if (config_.upgradeTimeoutSec > 0 && currentTimeMs() - startedAt > static_cast<std::int64_t>(config_.upgradeTimeoutSec) * 1000) {
            throw std::runtime_error("ota upgrade timeout after download");
        }

        reportStage(status, "verifying", 100, "verifying artifact", currentTimeMs(), publishStatus);
        verifyChecksum(request, artifactPath);
        if (config_.upgradeTimeoutSec > 0 && currentTimeMs() - startedAt > static_cast<std::int64_t>(config_.upgradeTimeoutSec) * 1000) {
            throw std::runtime_error("ota upgrade timeout after verify");
        }

        PendingOtaJob pending{request.jobId, machineCode, request.version, artifactPath,
                                    previousVersion, newStatusOccurrence()};
        ownToken = pending.token;
#ifndef _WIN32
        if (config_.applyScript == "/opt/modbus-gateway/bin/ota-apply.sh" &&
            !commandStatusSucceeded(runArgvWithTimeout({"python3", "-c", "import fcntl,json,os,sys"},
                                                       config_.upgradeTimeoutSec)))
            throw std::runtime_error("official OTA requires python3 before intent");
        pending.lifecycle = "inherited-flock-v1";
        if (runtimeHandoff) {
            pending.lifecycle = "systemd-handoff-v1";
            pending.packageType = request.packageType;
            pending.artifactSha256 = toLower(request.sha256);
            pending.artifactSize = request.size;
            struct stat leaseInfo{};
            if (fstat(applyLease->fd(), &leaseInfo) != 0) throw std::runtime_error("cannot bind donor lease identity");
            pending.leaseDev = leaseInfo.st_dev;
            pending.leaseInode = leaseInfo.st_ino;
        }
#endif
        {
            PendingJournalLock stateLock(joinPath(config_.stagingDir, "ota_state"));
            try {
                ensureJobNotProcessed(request);
                ensureNoUnresolvedJob();
                if (!strictlyAbsent(joinPath(config_.stagingDir, "ota-in-progress")))
                    throw std::runtime_error("previous OTA in-progress marker requires recovery");
            } catch (const OtaRequestRejected&) {
                throw;
            } catch (const std::exception& ex) {
                throw OtaRequestRejected(OtaRejectionReason::NotAdmitted, ex.what());
            }
            replacePendingJournal(pendingJobPath(config_.stagingDir, request.jobId), encodePendingJob(pending));
            intentPersisted = true;
            replacePendingJournal(joinPath(config_.stagingDir, "ota_active_job.txt"),
                "jobId=" + pending.jobId + "\ntoken=" + pending.token + "\n");
        }
        reportStage(status, "applying", 100, "running apply script", currentTimeMs(), publishStatus);
        runScript(config_.applyScript, request, artifactPath, pending.token,
#ifndef _WIN32
                  applyLease->fd()
#else
                  -1
#endif
        );
        if (runtimeHandoff) {
#ifndef _WIN32
            // READY is durable. Closing the donor lease enables only the independent owner.
            applyLease.reset();
            const auto helper = config_.applyScript.substr(0, config_.applyScript.find_last_of('/') + 1) + "ota-health-result.py";
            // execute already runs on the existing OTA worker, not the telemetry scan.
            if (commandStatusSucceeded(runArgvWithTimeout({"/usr/bin/python3", "-B", helper,
                    "--handoff-finish", config_.stagingDir, request.jobId, ownToken}, 40))) {
                reconcilePendingJobs();
                for (const auto& final : loadFinalStatuses()) {
                    if (final.jobId == request.jobId && final.occurrenceId == ownToken) { *status = final; break; }
                }
            }
#endif
            return; // Never enter the synchronous success tail during a handoff gap.
        }
    } catch (const std::exception& ex) {
        const auto* rejection = dynamic_cast<const OtaRequestRejected*>(&ex);
        if (rejection && rejection->reason() == OtaRejectionReason::Duplicate) throw;
#ifndef _WIN32
        if (intentPersisted) applyLease.reset();
#endif
        if (runtimeHandoff && intentPersisted) {
            // An uncertain dispatch is not failure/unchanged proof or permission for legacy rollback.
            status->stage = "applying";
            status->message = "runtime handoff unresolved; query original job";
#ifndef _WIN32
            const auto helper = config_.applyScript.substr(0, config_.applyScript.find_last_of('/') + 1) + "ota-health-result.py";
            if (commandStatusSucceeded(runArgvWithTimeout({"/usr/bin/python3", "-B", helper,
                    "--handoff-recover", config_.stagingDir, request.jobId, ownToken}, 30))) {
                reconcilePendingJobs();
                for (const auto& final : loadFinalStatuses()) {
                    if (final.jobId == request.jobId && final.occurrenceId == ownToken) { *status = final; break; }
                }
            }
#endif
            return;
        }
        const auto failedStage = status->stage.empty() ? std::string("unknown") : status->stage;
        bool rollbackAttempted = false;
        bool rollbackProcessSucceeded = false;
        bool rollbackSucceeded = false;
        bool finalized = false;
        bool safeEvidence = false;
        bool needRollback = false;
        try {
            if (intentPersisted) {
#ifndef _WIN32
                std::unique_ptr<JobLifecycleLock> failureLease;
#endif
                {
                    PendingJournalLock stateLock(joinPath(config_.stagingDir, "ota_state"));
                    const auto pending = decodePendingJob(readPendingJournal(
                        pendingJobPath(config_.stagingDir, request.jobId)));
                    if (pending.jobId == request.jobId && pending.version == request.version &&
                        pending.machineCode == machineCode && pending.token == ownToken &&
                        isActiveJob(config_.stagingDir, pending)) {
#ifndef _WIN32
                        if (!pending.lifecycle.empty()) {
                            failureLease.reset(new JobLifecycleLock(jobLifecyclePath(config_.stagingDir, pending), false));
                            if (failureLease->fd() < 0) throw std::runtime_error("OTA apply writer still alive");
                            if (!matchesLifecycleLease(pending, failureLease->fd()))
                                throw std::runtime_error("OTA lifecycle lease identity changed");
                        }
#endif
                        const auto healthPath = joinPath(config_.stagingDir, "health_" + request.jobId + ".txt");
#ifndef _WIN32
                        if (failureLease && strictlyAbsent(healthPath)) {
                            try { recordPreparedFailure(config_.stagingDir, pending); }
                            catch (const std::exception&) {
                                // No prepared proof does not invalidate a separately
                                // verified changed-job rollback path below.
                            }
                        }
#endif
                        if (pathExists(healthPath)) {
                            const auto health = decodeHealthResult(readPendingJournal(healthPath), pending);
                            rollbackSucceeded = health.status == "failed" && health.rollback == "succeeded";
                            safeEvidence = health.status == "failed" &&
                                (health.rollback == "unchanged" || health.rollback == "succeeded");
                        } else {
                            const auto appliedJobs = readPendingJournal(joinPath(config_.stagingDir, "applied_jobs.txt"));
                            const auto burned = ("\n" + appliedJobs).find("\n" + request.jobId + "\n") != std::string::npos;
                            const auto proofPath = joinPath(config_.stagingDir, "rollback_" + request.jobId + "_proof.txt");
                            const auto expectedProof = "jobId=" + request.jobId + "\nversion=" + request.version +
                                "\ntoken=" + ownToken + "\nstatus=succeeded\n";
                            const auto proof = readPendingJournal(proofPath);
                            const auto inProgress = readPendingJournal(joinPath(config_.stagingDir, "ota-in-progress"));
                            const auto ownsInProgress = inProgress.find("jobId=" + request.jobId + "\n") == 0 &&
                                inProgress.find("version=" + request.version + "\n") != std::string::npos &&
                                inProgress.find("stagingDir=" + config_.stagingDir + "\n") != std::string::npos;
                            if (burned && proof == expectedProof &&
                                !pathExists(joinPath(config_.stagingDir, "ota-in-progress"))) {
                                rollbackSucceeded = true;
                            } else if (burned && ownsInProgress && !pathExists(proofPath) &&
                                       !pathExists(joinPath(joinPath(config_.stagingDir, request.jobId),
                                                            "health_pending.txt")) &&
                                       !pathExists(joinPath(config_.stagingDir, "rollback_" + request.jobId + ".txt"))) {
                                needRollback = true;
                            }
                        }
                    }
                }
                if (needRollback) {
                    rollbackAttempted = true;
                    rollbackProcessSucceeded = tryRollback(request, artifactPath, ownToken,
#ifndef _WIN32
                        failureLease ? failureLease->fd() : -1
#else
                        -1
#endif
                    );
                }
                if (rollbackSucceeded || rollbackProcessSucceeded) {
                    PendingJournalLock stateLock(joinPath(config_.stagingDir, "ota_state"));
                    const auto pending = decodePendingJob(readPendingJournal(
                        pendingJobPath(config_.stagingDir, request.jobId)));
                    const auto proof = readPendingJournal(joinPath(config_.stagingDir,
                        "rollback_" + request.jobId + "_proof.txt"));
                    if (pending.jobId == request.jobId && pending.version == request.version &&
                        pending.machineCode == machineCode && pending.token == ownToken &&
                        isActiveJob(config_.stagingDir, pending) &&
                        proof == "jobId=" + request.jobId + "\nversion=" + request.version +
                                 "\ntoken=" + ownToken + "\nstatus=succeeded\n" &&
                        !pathExists(joinPath(config_.stagingDir, "ota-in-progress")) &&
                        !pathExists(joinPath(config_.stagingDir, "health_" + request.jobId + ".txt"))) {
                        rollbackSucceeded = true;
                        replacePendingJournal(joinPath(config_.stagingDir, "health_" + request.jobId + ".txt"),
                            "{\"format\":\"ota-health-v1\",\"jobId\":" + pendingJsonString(pending.jobId) +
                            ",\"version\":" + pendingJsonString(pending.version) +
                            ",\"token\":" + pendingJsonString(pending.token) +
                            ",\"status\":\"failed\",\"rollback\":\"succeeded\",\"ts\":\"" +
                            std::to_string(currentTimeMs()) + "\"}\n");
                        safeEvidence = true;
                    }
                }
#ifndef _WIN32
                failureLease.reset();
#endif
                if (safeEvidence) reconcilePendingJobs();
                PendingJournalLock stateLock(joinPath(config_.stagingDir, "ota_state"));
                const auto pending = decodePendingJob(readPendingJournal(
                    pendingJobPath(config_.stagingDir, request.jobId)));
                if (pending.jobId == request.jobId && pending.version == request.version &&
                    pending.machineCode == machineCode && pending.token == ownToken &&
                    isActiveJob(config_.stagingDir, pending) && hasValidFinal(config_.stagingDir, pending)) {
                    const auto health = decodeHealthResult(readPendingJournal(
                        joinPath(config_.stagingDir, "health_" + request.jobId + ".txt")), pending);
                    finalized = health.status == "failed" &&
                        (health.rollback == "unchanged" || health.rollback == "succeeded");
                }
            }
        } catch (const std::exception&) {
            // Uncertain or damaged evidence keeps the intent unresolved.
        }
        if (finalized) {
            status->stage = "failed";
            status->progress = 0;
            status->message = ex.what();
            status->ts = currentTimeMs();
        } else {
            appendFailureRecord(request, failedStage, ex.what(), rollbackAttempted, rollbackSucceeded, currentTimeMs());
            bool linkedSafeFailure = false;
            if (intentPersisted && safeEvidence) {
                PendingJournalLock stateLock(joinPath(config_.stagingDir, "ota_state"));
                const auto pending = decodePendingJob(readPendingJournal(
                    pendingJobPath(config_.stagingDir, request.jobId)));
                const auto healthBytes = readPendingJournal(joinPath(
                    config_.stagingDir, "health_" + request.jobId + ".txt"));
                const auto health = decodeHealthResult(healthBytes, pending);
                if (pending.jobId == request.jobId && pending.version == request.version &&
                    pending.machineCode == machineCode && pending.token == ownToken &&
                    health.status == "failed" &&
                    (health.rollback == "unchanged" || health.rollback == "succeeded")) {
                    status->stage = "failed";
                    status->progress = 0;
                    status->message = ex.what();
                    status->ts = currentTimeMs();
                    status->occurrenceId = newStatusOccurrence();
                    persistStatusSupersession(config_.stagingDir, pending, *status, healthBytes);
                    linkedSafeFailure = true;
                }
            }
            if (linkedSafeFailure) {
                appendPendingStatus(*status);
                if (publishStatus) {
                    try { publishStatus(*status); }
                    catch (...) { std::cerr << "ota failed status publish deferred; pending replay retained\n"; }
                }
            } else {
                reportStage(status, "failed", 0, ex.what(), currentTimeMs(), publishStatus);
            }
        }
        // An owned late refusal has already reported its own failure. Do not
        // let an entry worker turn it into a second, post-lease negative reply.
        if (rejection) throw std::runtime_error(ex.what());
        throw;
    }

#ifndef _WIN32
    applyLease.reset();
#endif
    const PendingOtaJob own{request.jobId, machineCode, request.version, artifactPath, previousVersion, ownToken};
    bool finalCommitted = false;
    bool outboxWritten = false;
    const char* phase = "health-reconciliation";
    try {
        reconcilePendingJobs();
        phase = "state-lock";
        PendingJournalLock stateLock(joinPath(config_.stagingDir, "ota_state"));
        try {
            phase = "intent";
            const auto pending = decodePendingJob(readPendingJournal(pendingJobPath(config_.stagingDir, request.jobId)));
            if (pending.jobId != own.jobId || pending.token != own.token || pending.version != own.version ||
                pending.machineCode != own.machineCode)
                throw std::runtime_error("OTA finalization identity changed");
            if (hasUnresolvedExecutionAudit(config_.stagingDir, pending))
                throw std::runtime_error("OTA finalization requires reconciliation");
            phase = "final-evidence";
            if (hasValidFinal(config_.stagingDir, pending)) {
                const auto finalPath = joinPath(config_.stagingDir, "ota_final_" + request.jobId + ".txt");
                const auto final = readPendingJournal(finalPath);
                const bool completed = final.find("status=completed\n") != std::string::npos;
                const auto healthPath = joinPath(config_.stagingDir, "health_" + request.jobId + ".txt");
                std::int64_t ts = currentTimeMs();
                if (!strictlyAbsent(healthPath)) {
                    const auto health = decodeHealthResult(readPendingJournal(healthPath), pending);
                    if (completed != (health.status == "healthy"))
                        throw std::runtime_error("OTA final conflicts with health evidence");
                    ts = health.ts;
                    bindProductionCompletion(*status, pending, health);
                } else if (!completed) {
                    throw std::runtime_error("OTA failed final requires health evidence");
                }
                status->stage = completed ? "completed" : "failed";
                status->progress = completed ? 100 : 0;
                status->message = "apply result already finalized";
                status->ts = ts;
                status->occurrenceId = pending.token;
                return;
            }
            if (!isActiveJob(config_.stagingDir, pending))
                throw std::runtime_error("OTA finalization active owner changed");
            phase = "pending-health";
            if (!strictlyAbsent(joinPath(joinPath(config_.stagingDir, request.jobId), "health_pending.txt"))) {
                reportStage(status, "applying", 100, "waiting for restart and health result", currentTimeMs(), publishStatus);
                return;
            }
            if (!strictlyAbsent(joinPath(config_.stagingDir, "health_" + request.jobId + ".txt")))
                throw std::runtime_error("OTA health evidence has not been finalized");

            phase = "version";
            writeVersionMarker(request, artifactPath, previousVersion);
            replacePendingJournal(joinPath(config_.stagingDir, "applied_version.txt"), request.version + "\n");
            phase = "history";
            appendUpgradeRecord(request, artifactPath, previousVersion, currentTimeMs());
            phase = "artifact-retention";
            cleanupOldArtifacts(fileNameOf(artifactPath));
            phase = "outbox-intent";
            persistFinalizationAudit(config_.stagingDir, pending, false, "final-marker",
                "apply-returned-success", "ota-terminal-outbox-v1");
            outboxWritten = true;
            phase = "final-marker";
            replacePendingJournal(joinPath(config_.stagingDir, "ota_final_" + request.jobId + ".txt"),
                "jobId=" + own.jobId + "\ntoken=" + own.token + "\nstatus=completed\n");
            finalCommitted = true;
            const auto terminalTs = finalizationAuditTimestamp(config_.stagingDir, pending);
            phase = "delivery-intent";
            persistFinalizationAudit(config_.stagingDir, pending, false, "terminal-journal",
                "apply-returned-success", "ota-terminal-outbox-v1", true, terminalTs);
            phase = "terminal-journal";
            status->stage = "completed";
            status->progress = 100;
            status->downloadedBytes = 0;
            status->totalBytes = 0;
            status->message = "upgrade completed after synchronous finalization reconciliation";
            status->ts = terminalTs;
            status->occurrenceId = pending.token;
            appendPendingStatus(*status);
            if (publishStatus) {
                try {
                    publishStatus(*status);
                } catch (...) {
                    std::cerr << "ota terminal status publish failed; pending replay retained\n";
                }
            }
        } catch (...) {
            if (!outboxWritten)
                auditSuccessfulFinalization(config_.stagingDir, own, finalCommitted, phase, "apply-returned-success");
            throw SuccessfulFinalizationError(own);
        }
    } catch (const SuccessfulFinalizationError& ex) {
        // Health reconciliation has already recorded its precise commit outcome.
        if (ex.jobId != own.jobId || ex.token != own.token)
            auditSuccessfulFinalization(config_.stagingDir, own, finalCommitted, phase, "apply-returned-success");
        throw;
    } catch (...) {
        auditSuccessfulFinalization(config_.stagingDir, own, finalCommitted, phase, "apply-returned-success");
        throw SuccessfulFinalizationError(own);
    }
}

std::string OtaService::resolveArtifactPath(const OtaRequest& request) const {
    std::string fileName;
    if (!request.version.empty()) {
        const auto packageExtension = request.packageType == "scada" ? "kyscada" : config_.packageType;
        fileName = request.version + "." + packageExtension;
    } else {
        fileName = artifactFileName(resolveArtifactSource(request));
    }
    if (fileName.empty()) {
        if (request.artifactUrl.empty()) {
            fileName = request.jobId + ".pkg";
        } else {
            throw std::runtime_error("invalid ota artifact file name");
        }
    }
    if (!isSafeRelativeArtifactName(fileName)) {
        throw std::runtime_error("invalid ota artifact file name");
    }
    return joinPath(config_.downloadDir, fileName);
}

std::string OtaService::resolveArtifactSource(const OtaRequest& request) const {
    if (!request.artifactUrl.empty()) {
        if (isHttpUrl(request.artifactUrl) || pathExists(request.artifactUrl)) {
            return request.artifactUrl;
        }
        if (!config_.artifactBaseUrl.empty()) {
            return joinPath(config_.artifactBaseUrl, request.artifactUrl);
        }
    }

    if (config_.storage.provider == "minio") {
        const auto minioUrl = buildMinioArtifactUrl(request);
        if (!minioUrl.empty()) {
            return minioUrl;
        }
    }

    if (!config_.artifactBaseUrl.empty() && !request.version.empty()) {
        return joinPath(config_.artifactBaseUrl, request.version + "." + config_.packageType);
    }

    return request.artifactUrl;
}

std::string OtaService::buildMinioArtifactUrl(const OtaRequest& request) const {
    const auto& minio = config_.storage.minio;
    if (!minio.publicBaseUrl.empty()) {
        if (!request.artifactUrl.empty() && !isHttpUrl(request.artifactUrl)) {
            return joinPath(minio.publicBaseUrl, request.artifactUrl);
        }
        if (!request.version.empty()) {
            return joinPath(minio.publicBaseUrl, request.version + "." + config_.packageType);
        }
    }
    if (minio.endpoint.empty() || minio.bucket.empty()) {
        return std::string();
    }
    std::string base = minio.endpoint;
    base = joinPath(base, minio.bucket);
    if (!minio.basePath.empty()) {
        base = joinPath(base, minio.basePath);
    }
    if (!request.artifactUrl.empty() && !isHttpUrl(request.artifactUrl)) {
        return joinPath(base, request.artifactUrl);
    }
    if (!request.version.empty()) {
        return joinPath(base, request.version + "." + config_.packageType);
    }
    return std::string();
}

void OtaService::ensureDirectory(const std::string& path) const {
    if (path.empty()) {
        return;
    }
    if (makeDir(path) != 0 && !pathExists(path)) {
        throw std::runtime_error("failed to create ota directory: " + path);
    }
}

std::string OtaService::readInstalledVersion() const {
    std::ifstream input(joinPath(config_.stagingDir, "current_version.txt").c_str());
    std::string line;
    std::string version;
    std::string fallback;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 15, "currentVersion=") == 0) version = line.substr(15);
        else if (line.compare(0, 8, "version=") == 0) fallback = line.substr(8);
    }
    if (!version.empty()) return version;
    if (!fallback.empty()) return fallback;
    return config_.currentVersion;
}

void OtaService::writeVersionMarker(
    const OtaRequest& request,
    const std::string& artifactPath,
    const std::string& previousVersion
) const {
    const auto versionFile = joinPath(config_.stagingDir, "current_version.txt");
    const auto jobBackupDir = joinPath(config_.backupDir, request.jobId);
    const auto workDir = joinPath(config_.stagingDir, request.jobId);
    std::ostringstream output;
    output << "jobId=" << request.jobId << "\n";
    output << "previousVersion=" << previousVersion << "\n";
    output << "currentVersion=" << (request.version.empty() ? previousVersion : request.version) << "\n";
    output << "artifactPath=" << artifactPath << "\n";
    output << "backupDir=" << jobBackupDir << "\n";
    output << "backupArtifact=" << joinPath(jobBackupDir, fileNameOf(artifactPath)) << "\n";
    output << "workDir=" << workDir << "\n";
    output << "retentionCount=" << config_.retentionCount << "\n";
    output << "autoReboot=" << (config_.autoReboot ? "true" : "false") << "\n";
    // Keep extra keys (packageType, scada*) that the apply script recorded for
    // this same job so rollback metadata is not lost.
    {
        std::ifstream existing(versionFile.c_str());
        std::vector<std::string> lines;
        std::string line;
        bool sameJob = false;
        while (std::getline(existing, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line == "jobId=" + request.jobId) sameJob = true;
            lines.push_back(line);
        }
        static const char* ownKeys[] = {"jobId=", "previousVersion=", "currentVersion=", "artifactPath=",
            "backupDir=", "backupArtifact=", "workDir=", "retentionCount=", "autoReboot="};
        for (const auto& kept : sameJob ? lines : std::vector<std::string>()) {
            bool own = kept.empty();
            for (const auto* key : ownKeys) {
                if (kept.compare(0, std::strlen(key), key) == 0) own = true;
            }
            if (!own) output << kept << "\n";
        }
    }
    try {
        replacePendingJournal(versionFile, output.str());
    } catch (const std::exception&) {
        throw std::runtime_error("failed to write ota version marker");
    }
}

void OtaService::ensureJobNotProcessed(const OtaRequest& request) const {
    // Job IDs are single-use once the apply stage has started: re-applying the
    // same job would overwrite its rollback backup with the new files.
    if (!strictlyAbsent(pendingJobPath(config_.stagingDir, request.jobId)) ||
        !strictlyAbsent(joinPath(config_.backupDir, request.jobId)) ||
        !strictlyAbsent(joinPath(config_.stagingDir, request.jobId))) {
        throw OtaRequestRejected(OtaRejectionReason::Duplicate, "ota jobId already processed");
    }
    const auto scan = [](const std::string& path, const std::function<bool(const std::string&)>& matches) {
        if (strictlyAbsent(path)) return false;
        struct stat info{};
        if (stat(path.c_str(), &info) != 0 || (info.st_mode & S_IFMT) != S_IFREG)
            throw std::runtime_error("cannot inspect OTA job history");
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open OTA job history");
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (matches(line)) return true;
        }
        if (input.bad() || !input.eof()) throw std::runtime_error("cannot read OTA job history");
        return false;
    };
    const auto needle = ",jobId=" + sanitizeJournalField(request.jobId) + ",";
    if (scan(joinPath(config_.stagingDir, "applied_jobs.txt"),
             [&](const std::string& line) { return line == request.jobId; }) ||
        scan(joinPath(config_.stagingDir, "upgrade_history.log"), [&](const std::string& line) {
            return line.find(",result=success") != std::string::npos && line.find(needle) != std::string::npos;
        })) throw OtaRequestRejected(OtaRejectionReason::Duplicate, "ota jobId already processed");
}

void OtaService::ensureTransportAllowed(const std::string& source) const {
    if (source.compare(0, 7, "http://") == 0 && !config_.allowInsecureHttp) {
        throw std::runtime_error("insecure http ota artifactUrl is disabled; use https or set ota.allowInsecureHttp");
    }
}

std::string OtaService::signatureDescriptor(const OtaRequest& request, const std::string& machineCode) {
    // Canonical descriptor "edge-ota-v1"; see doc/评审/OTA签名说明.md.
    return "edge-ota-v1\n"
        "jobId=" + request.jobId + "\n"
        "machineCode=" + machineCode + "\n"
        "packageType=" + request.packageType + "\n"
        "version=" + request.version + "\n"
        "sha256=" + toLower(request.sha256) + "\n"
        "size=" + std::to_string(request.size) + "\n";
}

void OtaService::verifySignature(const OtaRequest& request, const std::string& machineCode) const {
    if (!config_.requireSignature) {
        return;
    }
    if (request.signature.empty()) {
        throw std::runtime_error("ota signature is required");
    }
    if (request.sha256.empty()) {
        throw std::runtime_error("ota sha256 is required for signed ota");
    }
    if (machineCode.empty() || hasControlCharacter(machineCode) || hasControlCharacter(request.packageType)) {
        throw std::runtime_error("invalid ota signature descriptor");
    }
#ifdef _WIN32
    throw std::runtime_error("ota signature verification is not implemented on windows");
#else
    struct stat keyInfo {};
    if (config_.signaturePublicKeyPath.empty() || stat(config_.signaturePublicKeyPath.c_str(), &keyInfo) != 0 ||
        (keyInfo.st_mode & S_IFMT) != S_IFREG) {
        throw std::runtime_error("ota signature public key not found: " + config_.signaturePublicKeyPath);
    }
    const auto signatureBytes = decodeBase64(request.signature);
    ensureDirectory(config_.stagingDir);
    std::string descriptorPath;
    std::string signaturePath;
    writePrivateTempFile(config_.stagingDir, ".ota-descriptor", signatureDescriptor(request, machineCode), &descriptorPath);
    try {
        writePrivateTempFile(config_.stagingDir, ".ota-signature", signatureBytes, &signaturePath);
    } catch (...) {
        unlink(descriptorPath.c_str());
        throw;
    }
    const int status = runArgvWithTimeout({
        "openssl", "dgst", "-sha256",
        "-verify", config_.signaturePublicKeyPath,
        "-signature", signaturePath,
        descriptorPath
    }, 60);
    unlink(descriptorPath.c_str());
    unlink(signaturePath.c_str());
    if (!commandStatusSucceeded(status)) {
        throw std::runtime_error("ota signature verification failed");
    }
#endif
}

void OtaService::appendUpgradeRecord(
    const OtaRequest& request,
    const std::string& artifactPath,
    const std::string& previousVersion,
    std::int64_t ts
) const {
    const auto recordFile = joinPath(config_.stagingDir, "upgrade_history.log");
    std::ofstream output(recordFile.c_str(), std::ios::app);
    if (!output.is_open()) {
        throw std::runtime_error("failed to append ota upgrade history");
    }
    output << ts
           << ",result=success"
           << ",jobId=" << sanitizeJournalField(request.jobId)
           << ",fromVersion=" << sanitizeJournalField(previousVersion)
           << ",toVersion=" << sanitizeJournalField(request.version.empty() ? previousVersion : request.version)
           << ",artifactPath=" << sanitizeJournalField(artifactPath)
           << "\n";
}

void OtaService::appendFailureRecord(
    const OtaRequest& request,
    const std::string& stage,
    const std::string& message,
    bool rollbackAttempted,
    bool rollbackSucceeded,
    std::int64_t ts
) const {
    const auto recordFile = joinPath(config_.stagingDir, "upgrade_history.log");
    std::ofstream output(recordFile.c_str(), std::ios::app);
    if (!output.is_open()) {
        throw std::runtime_error("failed to append ota failure history");
    }
    output << ts
           << ",result=failure"
           << ",jobId=" << sanitizeJournalField(request.jobId)
           << ",stage=" << sanitizeJournalField(stage)
           << ",toVersion=" << sanitizeJournalField(request.version.empty() ? config_.currentVersion : request.version)
           << ",rollbackAttempted=" << (rollbackAttempted ? "true" : "false")
           << ",rollbackSucceeded=" << (rollbackSucceeded ? "true" : "false")
           << ",message=" << sanitizeJournalField(message)
           << "\n";
}

bool OtaService::tryRollback(const OtaRequest& request, const std::string& artifactPath,
                             const std::string& token, int lifecycleFd) const {
    if (config_.rollbackScript.empty()) {
        return false;
    }
    const auto rollbackCommand = buildScriptCommand(config_.rollbackScript, request, artifactPath, token);
    return commandStatusSucceeded(runShellCommandWithTimeout(rollbackCommand, config_.upgradeTimeoutSec, lifecycleFd));
}

void OtaService::cleanupOldArtifacts(const std::string& keepFileName) const {
    if (config_.retentionCount <= 0) {
        return;
    }
    auto files = listFilesInDirectory(config_.downloadDir);
    if (files.size() <= static_cast<std::size_t>(config_.retentionCount)) {
        return;
    }
    struct ArtifactFile {
        std::string path;
        std::string fileName;
        std::int64_t modifiedAt;
        bool isCurrent = false;
    };

    std::vector<ArtifactFile> artifacts;
    artifacts.reserve(files.size());
    for (const auto& path : files) {
        struct stat st {};
        if (stat(path.c_str(), &st) != 0) {
            continue;
        }
        const auto fileName = fileNameOf(path);
        artifacts.push_back(ArtifactFile{
            path,
            fileName,
            static_cast<std::int64_t>(st.st_mtime),
            fileName == keepFileName
        });
    }
    std::sort(artifacts.begin(), artifacts.end(), [](const ArtifactFile& left, const ArtifactFile& right) {
        if (left.isCurrent != right.isCurrent) {
            return left.isCurrent;
        }
        if (left.modifiedAt != right.modifiedAt) {
            return left.modifiedAt > right.modifiedAt;
        }
        return left.fileName > right.fileName;
    });

    for (std::size_t i = static_cast<std::size_t>(config_.retentionCount); i < artifacts.size(); ++i) {
        removeFilePath(artifacts[i].path);
    }
}

void OtaService::reportStage(
    OtaStatus* status,
    const std::string& stage,
    int progress,
    const std::string& message,
    std::int64_t ts,
    const std::function<void(const OtaStatus&)>& publishStatus
) const {
    status->stage = stage;
    status->progress = progress;
    status->message = message;
    status->ts = ts;
    status->occurrenceId.clear();
    appendPendingStatus(*status);
    if (publishStatus) {
        try {
            publishStatus(*status);
        } catch (const std::exception& ex) {
            std::cerr << "ota status publish failed stage=" << stage
                      << " error=" << ex.what()
                      << std::endl;
        } catch (...) {
            std::cerr << "ota status publish failed stage=" << stage
                      << " error=unknown"
                      << std::endl;
        }
    }
}

void OtaService::reportDownloadProgress(
    OtaStatus* status,
    std::uint64_t downloadedBytes,
    std::uint64_t totalBytes,
    const std::string& message,
    const std::function<void(const OtaStatus&)>& publishStatus
) const {
    if (status == nullptr) {
        return;
    }
    status->stage = "downloading";
    status->downloadedBytes = downloadedBytes;
    status->totalBytes = totalBytes;
    status->progress = downloadProgress(downloadedBytes, totalBytes);
    status->message = message;
    status->ts = currentTimeMs();
    status->occurrenceId.clear();
    appendPendingStatus(*status);
    if (publishStatus) {
        try {
            publishStatus(*status);
        } catch (const std::exception& ex) {
            std::cerr << "ota status publish failed stage=downloading"
                      << " error=" << ex.what()
                      << std::endl;
        } catch (...) {
            std::cerr << "ota status publish failed stage=downloading"
                      << " error=unknown"
                      << std::endl;
        }
    }
}

void OtaService::downloadArtifact(
    const OtaRequest& request,
    const std::string& targetPath,
    OtaStatus* status,
    const std::function<void(const OtaStatus&)>& publishStatus
) const {
    const auto source = resolveArtifactSource(request);
    if (source.empty()) {
        throw std::runtime_error("ota artifact source is empty");
    }
    ensureTransportAllowed(source);

    const int maxAttempts = std::max(1, config_.downloadRetryCount);
    std::string lastError;
    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        try {
            removeFilePath(targetPath);
            if (isHttpsUrl(source)) {
#ifdef _WIN32
                throw std::runtime_error("byte-accurate ota https download is not implemented on windows");
#else
                downloadExternalArtifact(request, source, targetPath, status, publishStatus);
#endif
                return;
            }

            if (isHttpUrl(source)) {
#ifdef _WIN32
                throw std::runtime_error("byte-accurate ota http download is not implemented on windows");
#else
                downloadHttpArtifact(request, source, targetPath, status, publishStatus);
#endif
                return;
            }

            copyArtifactWithProgress(request, source, targetPath, status, publishStatus);
            return;
        } catch (const std::exception& ex) {
            lastError = ex.what();
            removeFilePath(targetPath);
            if (attempt >= maxAttempts) {
                break;
            }
            reportDownloadProgress(
                status,
                0,
                request.size,
                "download retry " + std::to_string(attempt + 1) + "/" + std::to_string(maxAttempts) + ": " + lastError,
                publishStatus
            );
            const int backoffMs = std::max(0, config_.downloadRetryBackoffMs) * attempt;
            if (backoffMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
            }
        }
    }
    throw std::runtime_error("failed to download artifact after retries: " + lastError);
}

void OtaService::copyArtifactWithProgress(
    const OtaRequest& request,
    const std::string& source,
    const std::string& targetPath,
    OtaStatus* status,
    const std::function<void(const OtaStatus&)>& publishStatus
) const {
    std::ifstream input(source.c_str(), std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("artifact source not found: " + source);
    }
    input.seekg(0, std::ios::end);
    const auto fileSize = input.tellg();
    input.seekg(0, std::ios::beg);
    const auto totalBytes = fileSize > 0 ? static_cast<std::uint64_t>(fileSize) : request.size;
    validateOtaSize(config_, totalBytes);
    ensureFreeDiskSpace(config_.downloadDir, config_.minFreeBytes, totalBytes);

    std::ofstream output(targetPath.c_str(), std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("failed to open ota artifact target: " + targetPath);
    }
    reportDownloadProgress(status, 0, totalBytes, "copying artifact", publishStatus);
    std::vector<char> buffer(64 * 1024);
    std::uint64_t downloadedBytes = 0;
    std::int64_t lastReportMs = currentTimeMs();
    while (input.good()) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = input.gcount();
        if (got <= 0) {
            break;
        }
        output.write(buffer.data(), got);
        if (!output.good()) {
            throw std::runtime_error("failed to copy artifact to staging");
        }
        downloadedBytes += static_cast<std::uint64_t>(got);
        validateOtaSize(config_, downloadedBytes);
        const auto nowMs = currentTimeMs();
        if (nowMs - lastReportMs >= 500 || downloadedBytes == totalBytes) {
            reportDownloadProgress(status, downloadedBytes, totalBytes, "copying artifact", publishStatus);
            lastReportMs = nowMs;
        }
    }
    if (!output.good()) {
        throw std::runtime_error("failed to copy artifact to staging");
    }
    if (request.size > 0 && downloadedBytes != request.size) {
        throw std::runtime_error("ota artifact size mismatch");
    }
    reportDownloadProgress(status, downloadedBytes, totalBytes, "artifact copied", publishStatus);
}

void OtaService::downloadHttpArtifact(
    const OtaRequest& request,
    const std::string& source,
    const std::string& targetPath,
    OtaStatus* status,
    const std::function<void(const OtaStatus&)>& publishStatus
) const {
#ifdef _WIN32
    (void)request;
    (void)source;
    (void)targetPath;
    (void)status;
    (void)publishStatus;
    throw std::runtime_error("byte-accurate ota http download is not implemented on windows");
#else
    const auto url = parseHttpUrl(source);
    const auto sock = connectTcp(url.host, url.port, config_.upgradeTimeoutSec);
    SocketGuard guard(sock);
    setSocketTimeouts(sock, config_.upgradeTimeoutSec);

    std::ostringstream requestText;
    requestText << "GET " << url.path << " HTTP/1.0\r\n"
                << "Host: " << url.host << "\r\n"
                << "Accept: */*\r\n"
                << "Accept-Encoding: identity\r\n"
                << "Connection: close\r\n\r\n";
    sendAll(sock, requestText.str());

    std::string received;
    received.reserve(32 * 1024);
    std::vector<char> buffer(64 * 1024);
    std::size_t headerEnd = std::string::npos;
    const auto headerStartedAt = currentTimeMs();
    while (headerEnd == std::string::npos) {
        if (config_.upgradeTimeoutSec > 0 &&
            currentTimeMs() - headerStartedAt > static_cast<std::int64_t>(config_.upgradeTimeoutSec) * 1000) {
            throw std::runtime_error("ota http header timeout");
        }
        const auto rc = recv(sock, buffer.data(), buffer.size(), 0);
        if (rc <= 0) {
            throw std::runtime_error("ota http response ended before headers");
        }
        received.append(buffer.data(), static_cast<std::size_t>(rc));
        headerEnd = received.find("\r\n\r\n");
        if (received.size() > 256 * 1024) {
            throw std::runtime_error("ota http response header too large");
        }
    }

    const std::string headerText = received.substr(0, headerEnd);
    std::istringstream headerStream(headerText);
    std::string statusLine;
    std::getline(headerStream, statusLine);
    statusLine = trim(statusLine);
    std::istringstream statusParser(statusLine);
    std::string httpVersion;
    int statusCode = 0;
    statusParser >> httpVersion >> statusCode;
    if (statusCode < 200 || statusCode >= 300) {
        throw std::runtime_error("ota http download failed status=" + std::to_string(statusCode));
    }

    std::uint64_t contentLength = 0;
    bool chunked = false;
    std::string line;
    while (std::getline(headerStream, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const auto key = toLower(trim(line.substr(0, colon)));
        const auto value = trim(line.substr(colon + 1));
        if (key == "content-length" && !value.empty()) {
            contentLength = static_cast<std::uint64_t>(std::stoull(value));
        } else if (key == "transfer-encoding" && toLower(value).find("chunked") != std::string::npos) {
            chunked = true;
        }
    }
    if (chunked) {
        throw std::runtime_error("ota http chunked transfer is unsupported; server must return Content-Length");
    }
    validateOtaSize(config_, contentLength);

    const auto totalBytes = contentLength > 0 ? contentLength : request.size;
    ensureFreeDiskSpace(config_.downloadDir, config_.minFreeBytes, totalBytes);
    reportDownloadProgress(status, 0, totalBytes, "downloading artifact", publishStatus);
    const auto downloadStartedAt = currentTimeMs();

    std::ofstream output(targetPath.c_str(), std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("failed to open ota artifact target: " + targetPath);
    }

    std::uint64_t downloadedBytes = 0;
    std::int64_t lastReportMs = currentTimeMs();
    const auto bodyOffset = headerEnd + 4;
    if (received.size() > bodyOffset) {
        const auto bodySize = received.size() - bodyOffset;
        output.write(received.data() + bodyOffset, static_cast<std::streamsize>(bodySize));
        downloadedBytes += static_cast<std::uint64_t>(bodySize);
        validateOtaSize(config_, downloadedBytes);
    }
    if (!output.good()) {
        throw std::runtime_error("failed to write ota artifact");
    }
    if (downloadedBytes > 0) {
        reportDownloadProgress(status, downloadedBytes, totalBytes, "downloading artifact", publishStatus);
        lastReportMs = currentTimeMs();
    }

    while (true) {
        if (config_.upgradeTimeoutSec > 0 &&
            currentTimeMs() - downloadStartedAt > static_cast<std::int64_t>(config_.upgradeTimeoutSec) * 1000) {
            throw std::runtime_error("ota http download timeout");
        }
        const auto rc = recv(sock, buffer.data(), buffer.size(), 0);
        if (rc < 0) {
            throw std::runtime_error("ota http recv failed");
        }
        if (rc == 0) {
            break;
        }
        output.write(buffer.data(), rc);
        if (!output.good()) {
            throw std::runtime_error("failed to write ota artifact");
        }
        downloadedBytes += static_cast<std::uint64_t>(rc);
        validateOtaSize(config_, downloadedBytes);
        const auto nowMs = currentTimeMs();
        if (nowMs - lastReportMs >= 500 || (totalBytes > 0 && downloadedBytes >= totalBytes)) {
            reportDownloadProgress(status, downloadedBytes, totalBytes, "downloading artifact", publishStatus);
            lastReportMs = nowMs;
        }
        if (contentLength > 0 && downloadedBytes >= contentLength) {
            break;
        }
    }

    if (contentLength > 0 && downloadedBytes != contentLength) {
        throw std::runtime_error("ota http download incomplete");
    }
    if (request.size > 0 && downloadedBytes != request.size) {
        throw std::runtime_error("ota artifact size mismatch");
    }
    reportDownloadProgress(status, downloadedBytes, totalBytes, "artifact downloaded", publishStatus);
#endif
}

void OtaService::downloadExternalArtifact(
    const OtaRequest& request,
    const std::string& source,
    const std::string& targetPath,
    OtaStatus* status,
    const std::function<void(const OtaStatus&)>& publishStatus
) const {
#ifdef _WIN32
    (void)request;
    (void)source;
    (void)targetPath;
    (void)status;
    (void)publishStatus;
    throw std::runtime_error("byte-accurate ota external download is not implemented on windows");
#else
    if (!isSafeDownloadUrl(source)) {
        throw std::runtime_error("ota artifact url rejected: unsafe scheme, characters, or leading dash");
    }
    removeFilePath(targetPath);
    const std::uint64_t totalBytes = request.size;
    reportDownloadProgress(status, 0, totalBytes, "downloading artifact", publishStatus);

    // The URL and target path are passed as positional parameters ($1/$2),
    // never interpolated into the script text, so shell metacharacters cannot
    // break out. The "--" separator additionally prevents a URL or path from
    // being interpreted as a curl/wget option (argument injection), and
    // isSafeDownloadUrl() has already rejected a leading '-' and any control
    // characters as defence in depth.
    const char* script =
        "if command -v curl >/dev/null 2>&1; then "
        "curl -L --fail --silent --show-error --proto \"$4\" --proto-redir \"$4\" ${5:+--max-filesize} ${5:+\"$5\"} "
        "--connect-timeout 15 --max-time \"$3\" -o \"$1\" -- \"$2\"; "
        "elif [ \"$6\" = 1 ] && command -v wget >/dev/null 2>&1; then "
        "wget -q -T 15 -O \"$1\" -- \"$2\"; "
        "else "
        "echo \"curl required for secure OTA download (wget fallback requires allowInsecureHttp)\" >&2; exit 127; "
        "fi";

    const pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("failed to fork ota downloader");
    }
    if (pid == 0) {
        setsid();
        const std::string timeoutText = std::to_string(std::max(1, config_.upgradeTimeoutSec));
        const std::string protocols = config_.allowInsecureHttp ? "=http,https" : "=https";
        const std::string allowWget = config_.allowInsecureHttp ? "1" : "0";
        const std::uint64_t sizeLimit = config_.maxArtifactBytes > 0 ? config_.maxArtifactBytes : request.size;
        const std::string maxFileSize = sizeLimit > 0 ? std::to_string(sizeLimit) : std::string();
        execl("/bin/sh", "sh", "-c", script, "sh", targetPath.c_str(), source.c_str(), timeoutText.c_str(),
            protocols.c_str(), maxFileSize.c_str(), allowWget.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    int exitStatus = 0;
    std::uint64_t lastBytes = 0;
    std::int64_t lastReportMs = currentTimeMs();
    const auto startedAt = currentTimeMs();
    try {
        while (true) {
            const pid_t waited = waitpid(pid, &exitStatus, WNOHANG);
            if (waited == pid) {
                break;
            }
            if (waited < 0) {
                throw std::runtime_error("ota downloader waitpid failed");
            }
            const auto currentBytes = fileSizeBytes(targetPath);
            const auto nowMs = currentTimeMs();
            if (currentBytes != lastBytes || nowMs - lastReportMs >= 1000) {
                reportDownloadProgress(status, currentBytes, totalBytes, "downloading artifact", publishStatus);
                lastBytes = currentBytes;
                lastReportMs = nowMs;
            }
            validateOtaSize(config_, currentBytes);
            if (config_.upgradeTimeoutSec > 0 &&
                nowMs - startedAt > static_cast<std::int64_t>(config_.upgradeTimeoutSec) * 1000) {
                throw std::runtime_error("ota downloader timeout");
            }
            usleep(500 * 1000);
        }
    } catch (...) {
        // Never leave curl/wget writing into an unlinked inode after an abort.
        terminateProcess(pid);
        throw;
    }

    const auto finalBytes = fileSizeBytes(targetPath);
    validateOtaSize(config_, finalBytes);
    reportDownloadProgress(status, finalBytes, totalBytes, "artifact downloaded", publishStatus);
    if (!WIFEXITED(exitStatus) || WEXITSTATUS(exitStatus) != 0) {
        throw std::runtime_error("failed to download https artifact using curl/wget");
    }
    if (request.size > 0 && finalBytes != request.size) {
        throw std::runtime_error("ota artifact size mismatch");
    }
#endif
}

void OtaService::verifyChecksum(const OtaRequest& request, const std::string& artifactPath) const {
    if (!config_.checksumRequired && !config_.requireSignature) {
        return;
    }
    if (request.sha256.empty()) {
        throw std::runtime_error("ota sha256 is required");
    }
#ifdef _WIN32
    const std::string command =
        "powershell -Command \"$hash=(Get-FileHash -Algorithm SHA256 " +
        quoteArg(artifactPath) +
        ").Hash.ToLower(); if ($hash -ne " +
        quoteArg(toLower(request.sha256)) +
        ") { exit 2 }\"";
#else
    const std::string command =
        std::string("sh -c '"
        "line=$(sha256sum \"$1\" 2>/dev/null || true); "
        "actual=${line%% *}; "
        "if [ -z \"$actual\" ] && command -v openssl >/dev/null 2>&1; then "
        "line=$(openssl dgst -sha256 \"$1\" 2>/dev/null || true); "
        "actual=${line##* }; "
        "fi; "
        "[ \"$actual\" = \"$2\" ]"
        "' ") +
        " sh " + quoteArg(artifactPath) + " " + quoteArg(toLower(request.sha256));
#endif
    if (!commandStatusSucceeded(runShellCommandWithTimeout(command, config_.upgradeTimeoutSec))) {
        throw std::runtime_error("artifact sha256 mismatch");
    }
}

void OtaService::runScript(
    const std::string& scriptPath,
    const OtaRequest& request,
    const std::string& artifactPath,
    const std::string& token,
    int lifecycleFd
) const {
    if (scriptPath.empty()) {
        throw std::runtime_error("ota apply script is empty");
    }

    const auto command = fileNameOf(scriptPath) == "offline-runtime-upgrade.py" ?
        "/usr/bin/python3 -B " + quoteArg(scriptPath.substr(0, scriptPath.find_last_of('/') + 1) + "ota-health-result.py") +
        " --handoff " + quoteArg(config_.stagingDir) + " " + quoteArg(request.jobId) + " " + quoteArg(token) +
        " 9 " + quoteArg(scriptPath) : buildScriptCommand(scriptPath, request, artifactPath, token) +
        (lifecycleFd >= 0 ? " 9" : "");
    if (!commandStatusSucceeded(runShellCommandWithTimeout(command, config_.upgradeTimeoutSec, lifecycleFd))) {
        throw std::runtime_error("ota apply script failed");
    }
}

std::string OtaService::buildScriptCommand(
    const std::string& scriptPath,
    const OtaRequest& request,
    const std::string& artifactPath,
    const std::string& token
) const {
    std::ostringstream command;
#ifdef _WIN32
    command << quoteArg(scriptPath);
#else
    command << "sh " << quoteArg(scriptPath);
#endif
    command << " " << quoteArg(artifactPath)
            << " " << quoteArg(request.version)
            << " " << quoteArg(request.jobId)
            << " " << quoteArg(config_.backupDir)
            << " " << quoteArg(config_.stagingDir);
    if (!token.empty()) command << " " << quoteArg(token);
    return command.str();
}

bool OtaService::isHttpUrl(const std::string& value) const {
    return value.find("http://") == 0 || value.find("https://") == 0;
}

bool OtaService::isHttpsUrl(const std::string& value) const {
    return value.find("https://") == 0;
}

}  // namespace edge_gateway
