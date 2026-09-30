#include "edge_gateway/ota_service.hpp"
#include "edge_gateway/json_value.hpp"

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
#include <memory>
#include <random>
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
        auto directory = separator == std::string::npos ? "." : path.substr(0, separator);
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
        ",\"occurrenceId\":" + pendingJsonString(status.occurrenceId) + "}\n";
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
    const auto existing = bytes.find(record);
    if (existing == std::string::npos || (existing != 0 && bytes[existing - 1] != '\n')) bytes += record;
    if (config_.maxPendingStatusBytes && bytes.size() > config_.maxPendingStatusBytes) {
        const auto start = bytes.find('\n', bytes.size() - config_.maxPendingStatusBytes - 1);
        bytes.erase(0, start + 1);
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
        (job.lifecycle.empty() ? "" : ",\"lifecycle\":" + pendingJsonString(job.lifecycle)) + "}\n";
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
        if (job.lifecycle != "inherited-flock-v1")
            throw std::runtime_error("unsupported OTA lifecycle protocol");
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
    if (result.ts <= 0 ||
        !((result.status == "healthy" && result.rollback == "none") ||
          (result.status == "failed" &&
           (result.rollback == "succeeded" || result.rollback == "failed" ||
            result.rollback == "skipped" || result.rollback == "unchanged"))))
        throw std::runtime_error("invalid OTA health terminal result");
    return result;
}

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

bool isActiveJob(const std::string& stagingDir, const PendingOtaJob& job) {
    const auto path = joinPath(stagingDir, "ota_active_job.txt");
    if (fileSizeBytes(path) > 256) return false;
    return readPendingJournal(path) == "jobId=" + job.jobId + "\ntoken=" + job.token + "\n";
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
        }
#endif
        if (!hasValidFinal(config_.stagingDir, job))
            throw std::runtime_error("previous OTA job has no verified terminal result");
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
    PendingJournalLock stateLock(joinPath(config_.stagingDir, "ota_state"));
    for (const auto& path : pendingJobFiles(config_.stagingDir)) {
        PendingOtaJob job;
        OtaHealthResult health;
#ifndef _WIN32
        std::unique_ptr<JobLifecycleLock> lease;
#endif
        try {
            if (fileSizeBytes(path) > 4096) continue;
            job = decodePendingJob(readPendingJournal(path));
            if (path != pendingJobPath(config_.stagingDir, job.jobId)) continue;
            if (hasValidFinal(config_.stagingDir, job)) continue;
            if (!isActiveJob(config_.stagingDir, job)) continue;
#ifndef _WIN32
            if (!job.lifecycle.empty()) {
                lease.reset(new JobLifecycleLock(jobLifecyclePath(config_.stagingDir, job), false));
                if (lease->fd() < 0) continue;
            }
#endif
            const auto healthPath = joinPath(config_.stagingDir, "health_" + job.jobId + ".txt");
#ifndef _WIN32
            if (lease && strictlyAbsent(healthPath)) recordPreparedFailure(config_.stagingDir, job);
#endif
            if (fileSizeBytes(healthPath) == 0 || fileSizeBytes(healthPath) > 4096) continue;
            health = decodeHealthResult(readPendingJournal(healthPath), job);
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
        terminal.stage = health.status == "healthy" ? "completed" : "failed";
        terminal.progress = health.status == "healthy" ? 100 : 0;
        terminal.message = health.status == "healthy" ? "upgrade completed after health verification" :
            health.rollback == "unchanged" ? "OTA rejected before runtime change" :
            "OTA failed; rollback " + health.rollback;
        terminal.ts = health.ts;
        terminal.occurrenceId = job.token;
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
        if (health.status == "healthy") {
            writeVersionMarker(request, job.artifactPath, job.previousVersion);
            replacePendingJournal(joinPath(config_.stagingDir, "applied_version.txt"), job.version + "\n");
            if (!recorded("success"))
                appendUpgradeRecord(request, job.artifactPath, job.previousVersion, health.ts);
        } else if (!recorded("failure")) {
            appendFailureRecord(request, "health", terminal.message,
                health.rollback == "succeeded" || health.rollback == "failed",
                health.rollback == "succeeded", health.ts);
        }
        appendPendingStatus(terminal);
        replacePendingJournal(joinPath(config_.stagingDir, "ota_final_" + job.jobId + ".txt"),
            "jobId=" + job.jobId + "\ntoken=" + job.token + "\nstatus=" + terminal.stage + "\n");
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
            if (path != pendingJobPath(config_.stagingDir, job.jobId) ||
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
            } else {
                const auto final = readPendingJournal(joinPath(config_.stagingDir,
                    "ota_final_" + job.jobId + ".txt"));
                if (final.find("status=completed\n") == std::string::npos) continue;
                status.stage = "completed";
                status.progress = 100;
                status.message = "upgrade completed";
            }
            statuses.push_back(std::move(status));
        } catch (const std::exception&) {
            // Incomplete or stale state cannot be shown as terminal.
        }
    }
    return statuses;
}

void OtaService::clearPendingStatuses() const {
    std::lock_guard<std::mutex> guard(pendingMutex_);
    if (pendingSnapshot_.empty()) return;
    ensureDirectory(config_.stagingDir);
    PendingJournalLock lock(statusJournalPath());
    const auto bytes = readPendingJournal(statusJournalPath());
    if (bytes.compare(0, pendingSnapshot_.size(), pendingSnapshot_) != 0)
        throw std::runtime_error("OTA pending snapshot changed; reload before clearing");
    replacePendingJournal(statusJournalPath(), bytes.substr(pendingSnapshot_.size()));
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
#ifndef _WIN32
    std::unique_ptr<JobLifecycleLock> applyLease;
#endif
    try {
        rejectDuplicateRequest(request.jobId);
        if (!config_.enabled) throw std::runtime_error("ota is disabled");
        validateOtaRequestFields(request, config_);
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
    } catch (const std::exception& ex) {
        const auto* rejection = dynamic_cast<const OtaRequestRejected*>(&ex);
        if (rejection && rejection->reason() == OtaRejectionReason::Duplicate) throw;
#ifndef _WIN32
        if (intentPersisted) applyLease.reset();
#endif
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
            reportStage(status, "failed", 0, ex.what(), currentTimeMs(), publishStatus);
        }
        // An owned late refusal has already reported its own failure. Do not
        // let an entry worker turn it into a second, post-lease negative reply.
        if (rejection) throw std::runtime_error(ex.what());
        throw;
    }

#ifndef _WIN32
    applyLease.reset();
#endif
    reconcilePendingJobs();
    PendingJournalLock stateLock(joinPath(config_.stagingDir, "ota_state"));
    const auto pending = decodePendingJob(readPendingJournal(pendingJobPath(config_.stagingDir, request.jobId)));
    if (hasValidFinal(config_.stagingDir, pending)) {
        const auto health = decodeHealthResult(readPendingJournal(
            joinPath(config_.stagingDir, "health_" + request.jobId + ".txt")), pending);
        status->stage = health.status == "healthy" ? "completed" : "failed";
        status->progress = health.status == "healthy" ? 100 : 0;
        status->message = "restart and health result already finalized";
        status->ts = health.ts;
        return;
    }
    if (pathExists(joinPath(joinPath(config_.stagingDir, request.jobId), "health_pending.txt"))) {
        reportStage(status, "applying", 100, "waiting for restart and health result", currentTimeMs(), publishStatus);
        return;
    }

    writeVersionMarker(request, artifactPath, previousVersion);
    replacePendingJournal(joinPath(config_.stagingDir, "applied_version.txt"), request.version + "\n");
    appendUpgradeRecord(request, artifactPath, previousVersion, currentTimeMs());
    cleanupOldArtifacts(fileNameOf(artifactPath));
    reportStage(status, "completed", 100, "upgrade completed", currentTimeMs(), publishStatus);
    replacePendingJournal(joinPath(config_.stagingDir, "ota_final_" + request.jobId + ".txt"),
        "jobId=" + pending.jobId + "\ntoken=" + pending.token + "\nstatus=completed\n");
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

    const auto command = buildScriptCommand(scriptPath, request, artifactPath, token) +
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
