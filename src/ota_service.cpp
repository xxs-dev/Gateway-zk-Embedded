#include "edge_gateway/ota_service.hpp"

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
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <netdb.h>
#include <sys/socket.h>
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

void keepFileTail(const std::string& path, std::size_t maxBytes) {
    if (maxBytes == 0) {
        return;
    }
    const auto size = fileSizeBytes(path);
    if (size <= maxBytes) {
        return;
    }
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input.is_open()) {
        return;
    }
    input.seekg(-static_cast<std::streamoff>(maxBytes), std::ios::end);
    std::string tail(maxBytes, '\0');
    input.read(&tail[0], static_cast<std::streamsize>(tail.size()));
    tail.resize(static_cast<std::size_t>(input.gcount()));
    const auto firstNewline = tail.find('\n');
    if (firstNewline != std::string::npos && firstNewline + 1 < tail.size()) {
        tail = tail.substr(firstNewline + 1);
    }
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    if (output.is_open()) {
        output << tail;
    }
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

void validateOtaRequestFields(const OtaRequest& request) {
    if (!isSafeOtaId(request.jobId)) {
        throw std::runtime_error("invalid ota jobId");
    }
    if (!request.version.empty() && !isSafeOtaId(request.version)) {
        throw std::runtime_error("invalid ota version");
    }
    if (!isSafeSha256(request.sha256)) {
        throw std::runtime_error("invalid ota sha256");
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

int runShellCommandWithTimeout(const std::string& command, int timeoutSec) {
    const pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        setsid();
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

bool commandStatusSucceeded(int status) {
    if (status < 0) {
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
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
int runShellCommandWithTimeout(const std::string& command, int) {
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

bool OtaService::validateRequest(const OtaRequest& request, std::string* errorMessage) const {
    try {
        validateOtaRequestFields(request);
        validateOtaSize(config_, request.size);
        (void)resolveArtifactPath(request);
        return true;
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

void OtaService::enforceStatusJournalLimit() const {
    if (config_.maxPendingStatusBytes == 0) {
        return;
    }
    keepFileTail(statusJournalPath(), config_.maxPendingStatusBytes);
}

void OtaService::appendPendingStatus(const OtaStatus& status) const {
    ensureDirectory(config_.stagingDir);
    std::ofstream output(statusJournalPath().c_str(), std::ios::app);
    if (!output.is_open()) {
        return;
    }
    output << sanitizeJournalField(status.jobId) << "\t"
           << sanitizeJournalField(status.machineCode) << "\t"
           << sanitizeJournalField(status.stage) << "\t"
           << status.progress << "\t"
           << status.downloadedBytes << "\t"
           << status.totalBytes << "\t"
           << sanitizeJournalField(status.message) << "\t"
           << status.ts << "\n";
    output.close();
    enforceStatusJournalLimit();
}

std::vector<OtaStatus> OtaService::loadPendingStatuses() const {
    std::vector<OtaStatus> statuses;
    std::ifstream input(statusJournalPath().c_str());
    if (!input.is_open()) {
        return statuses;
    }
    std::string line;
    while (std::getline(input, line)) {
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
    return statuses;
}

void OtaService::clearPendingStatuses() const {
    std::ofstream output(statusJournalPath().c_str(), std::ios::trunc);
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
    if (!config_.enabled) {
        throw std::runtime_error("ota is disabled");
    }
    validateOtaRequestFields(request);
    validateOtaSize(config_, request.size);

    reply->jobId = request.jobId;
    reply->machineCode = machineCode;
    reply->accepted = true;
    reply->message = "accepted";
    reply->ts = nowMs;

    status->jobId = request.jobId;
    status->machineCode = machineCode;
    status->downloadedBytes = 0;
    status->totalBytes = request.size;
    reportStage(status, "accepted", 0, "accepted", nowMs, publishStatus);

    ensureDirectory(config_.downloadDir);
    ensureDirectory(config_.stagingDir);
    ensureDirectory(config_.backupDir);

    const auto artifactPath = resolveArtifactPath(request);
    const auto startedAt = currentTimeMs();
    try {
        ensureFreeDiskSpace(config_.downloadDir, config_.minFreeBytes, request.size);

        downloadArtifact(request, artifactPath, status, publishStatus);
        if (config_.upgradeTimeoutSec > 0 && currentTimeMs() - startedAt > static_cast<std::int64_t>(config_.upgradeTimeoutSec) * 1000) {
            throw std::runtime_error("ota upgrade timeout after download");
        }

        reportStage(status, "verifying", 100, "verifying artifact", currentTimeMs(), publishStatus);
        verifyChecksum(request, artifactPath);
        if (config_.upgradeTimeoutSec > 0 && currentTimeMs() - startedAt > static_cast<std::int64_t>(config_.upgradeTimeoutSec) * 1000) {
            throw std::runtime_error("ota upgrade timeout after verify");
        }

        reportStage(status, "applying", 100, "running apply script", currentTimeMs(), publishStatus);
        runScript(config_.applyScript, request, artifactPath);
    } catch (const std::exception& ex) {
        const auto failedStage = status->stage.empty() ? std::string("unknown") : status->stage;
        bool rollbackAttempted = false;
        bool rollbackSucceeded = false;
        if (status->stage == "applying") {
            rollbackAttempted = true;
            reportStage(status, "rollback", 100, "running rollback script", currentTimeMs(), publishStatus);
            rollbackSucceeded = tryRollback(request, artifactPath);
        }
        appendFailureRecord(request, failedStage, ex.what(), rollbackAttempted, rollbackSucceeded, currentTimeMs());
        reportStage(status, "failed", 0, ex.what(), currentTimeMs(), publishStatus);
        throw;
    }

    writeVersionMarker(request, artifactPath);
    appendUpgradeRecord(request, artifactPath, currentTimeMs());
    cleanupOldArtifacts(fileNameOf(artifactPath));
    reportStage(status, "completed", 100, "upgrade completed", currentTimeMs(), publishStatus);
}

std::string OtaService::resolveArtifactPath(const OtaRequest& request) const {
    std::string fileName;
    if (!request.version.empty()) {
        fileName = request.version + "." + config_.packageType;
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

void OtaService::writeVersionMarker(const OtaRequest& request, const std::string& artifactPath) const {
    const auto versionFile = joinPath(config_.stagingDir, "current_version.txt");
    const auto jobBackupDir = joinPath(config_.backupDir, request.jobId);
    const auto workDir = joinPath(config_.stagingDir, request.jobId);
    std::ofstream output(versionFile.c_str(), std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("failed to write ota version marker");
    }
    output << "jobId=" << request.jobId << "\n";
    output << "previousVersion=" << config_.currentVersion << "\n";
    output << "currentVersion=" << (request.version.empty() ? config_.currentVersion : request.version) << "\n";
    output << "artifactPath=" << artifactPath << "\n";
    output << "backupDir=" << jobBackupDir << "\n";
    output << "backupArtifact=" << joinPath(jobBackupDir, fileNameOf(artifactPath)) << "\n";
    output << "workDir=" << workDir << "\n";
    output << "retentionCount=" << config_.retentionCount << "\n";
    output << "autoReboot=" << (config_.autoReboot ? "true" : "false") << "\n";
}

void OtaService::appendUpgradeRecord(const OtaRequest& request, const std::string& artifactPath, std::int64_t ts) const {
    const auto recordFile = joinPath(config_.stagingDir, "upgrade_history.log");
    std::ofstream output(recordFile.c_str(), std::ios::app);
    if (!output.is_open()) {
        throw std::runtime_error("failed to append ota upgrade history");
    }
    output << ts
           << ",result=success"
           << ",jobId=" << sanitizeJournalField(request.jobId)
           << ",fromVersion=" << sanitizeJournalField(config_.currentVersion)
           << ",toVersion=" << sanitizeJournalField(request.version.empty() ? config_.currentVersion : request.version)
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

bool OtaService::tryRollback(const OtaRequest& request, const std::string& artifactPath) const {
    if (config_.rollbackScript.empty()) {
        return false;
    }
    const auto rollbackCommand = buildScriptCommand(config_.rollbackScript, request, artifactPath);
    return commandStatusSucceeded(runShellCommandWithTimeout(rollbackCommand, config_.upgradeTimeoutSec));
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
    removeFilePath(targetPath);
    const std::uint64_t totalBytes = request.size;
    reportDownloadProgress(status, 0, totalBytes, "downloading artifact", publishStatus);

    const char* script =
        "if command -v curl >/dev/null 2>&1; then "
        "curl -L --fail --silent --show-error --connect-timeout 15 --max-time \"$3\" -o \"$1\" \"$2\"; "
        "elif command -v wget >/dev/null 2>&1; then "
        "wget -q -T 15 -O \"$1\" \"$2\"; "
        "else "
        "echo \"curl/wget not found\" >&2; exit 127; "
        "fi";

    const pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("failed to fork ota downloader");
    }
    if (pid == 0) {
        setsid();
        const std::string timeoutText = std::to_string(std::max(1, config_.upgradeTimeoutSec));
        execl("/bin/sh", "sh", "-c", script, "sh", targetPath.c_str(), source.c_str(), timeoutText.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    int exitStatus = 0;
    std::uint64_t lastBytes = 0;
    std::int64_t lastReportMs = currentTimeMs();
    const auto startedAt = currentTimeMs();
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
            terminateProcess(pid);
            throw std::runtime_error("ota downloader timeout");
        }
        usleep(500 * 1000);
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
    if (!config_.checksumRequired || request.sha256.empty()) {
        return;
    }
#ifdef _WIN32
    const std::string command =
        "powershell -Command \"$hash=(Get-FileHash -Algorithm SHA256 " +
        quoteArg(artifactPath) +
        ").Hash.ToLower(); if ($hash -ne " +
        quoteArg(request.sha256) +
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
        " sh " + quoteArg(artifactPath) + " " + quoteArg(request.sha256);
#endif
    if (!commandStatusSucceeded(runShellCommandWithTimeout(command, config_.upgradeTimeoutSec))) {
        throw std::runtime_error("artifact sha256 mismatch");
    }
}

void OtaService::runScript(
    const std::string& scriptPath,
    const OtaRequest& request,
    const std::string& artifactPath
) const {
    if (scriptPath.empty()) {
        throw std::runtime_error("ota apply script is empty");
    }

    const auto command = buildScriptCommand(scriptPath, request, artifactPath);
    if (!commandStatusSucceeded(runShellCommandWithTimeout(command, config_.upgradeTimeoutSec))) {
        throw std::runtime_error("ota apply script failed");
    }
}

std::string OtaService::buildScriptCommand(
    const std::string& scriptPath,
    const OtaRequest& request,
    const std::string& artifactPath
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
    return command.str();
}

bool OtaService::isHttpUrl(const std::string& value) const {
    return value.find("http://") == 0 || value.find("https://") == 0;
}

bool OtaService::isHttpsUrl(const std::string& value) const {
    return value.find("https://") == 0;
}

}  // namespace edge_gateway
