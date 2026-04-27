#include "edge_gateway/ota_service.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace edge_gateway {

namespace {

std::string quoteArg(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const auto ch : value) {
        if (ch == '"') {
            escaped += "\\\"";
        } else {
            escaped.push_back(ch);
        }
    }
    return std::string("\"") + escaped + "\"";
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

std::string sanitizeJournalField(std::string value) {
    for (auto& ch : value) {
        if (ch == '\t' || ch == '\r' || ch == '\n') {
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

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}


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

std::string OtaService::statusJournalPath() const {
    return joinPath(config_.stagingDir, "ota_status_pending.log");
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
           << sanitizeJournalField(status.message) << "\t"
           << status.ts << "\n";
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
            status.ts = static_cast<std::int64_t>(std::stoll(parts[5]));
        } catch (...) {
            continue;
        }
        status.message = parts[4];
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

    reply->jobId = request.jobId;
    reply->machineCode = machineCode;
    reply->accepted = true;
    reply->message = "accepted";
    reply->ts = nowMs;

    status->jobId = request.jobId;
    status->machineCode = machineCode;
    reportStage(status, "accepted", 0, "accepted", nowMs, publishStatus);

    ensureDirectory(config_.downloadDir);
    ensureDirectory(config_.stagingDir);
    ensureDirectory(config_.backupDir);

    const auto artifactPath = resolveArtifactPath(request);
    const auto startedAt = currentTimeMs();

    reportStage(status, "downloading", 10, "downloading artifact", nowMs, publishStatus);
    downloadArtifact(request, artifactPath);
    if (config_.upgradeTimeoutSec > 0 && currentTimeMs() - startedAt > static_cast<std::int64_t>(config_.upgradeTimeoutSec) * 1000) {
        throw std::runtime_error("ota upgrade timeout after download");
    }

    reportStage(status, "verifying", 40, "verifying artifact", currentTimeMs(), publishStatus);
    verifyChecksum(request, artifactPath);
    if (config_.upgradeTimeoutSec > 0 && currentTimeMs() - startedAt > static_cast<std::int64_t>(config_.upgradeTimeoutSec) * 1000) {
        throw std::runtime_error("ota upgrade timeout after verify");
    }

    try {
        reportStage(status, "applying", 70, "running apply script", currentTimeMs(), publishStatus);
        runScript(config_.applyScript, request, artifactPath);
    } catch (const std::exception& ex) {
        reportStage(status, "rollback", 85, "running rollback script", currentTimeMs(), publishStatus);
        const bool rollbackSucceeded = tryRollback(request, artifactPath);
        appendFailureRecord(request, "applying", ex.what(), true, rollbackSucceeded, currentTimeMs());
        throw;
    }

    writeVersionMarker(request, artifactPath);
    appendUpgradeRecord(request, artifactPath, currentTimeMs());
    cleanupOldArtifacts();
    reportStage(status, "completed", 100, "upgrade completed", currentTimeMs(), publishStatus);
}

std::string OtaService::resolveArtifactPath(const OtaRequest& request) const {
    std::string fileName;
    if (!request.version.empty()) {
        fileName = request.version + "." + config_.packageType;
    } else {
        fileName = fileNameOf(resolveArtifactSource(request));
    }
    if (fileName.empty()) {
        fileName = request.jobId + ".pkg";
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
    std::ofstream output(versionFile.c_str(), std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("failed to write ota version marker");
    }
    output << "previousVersion=" << config_.currentVersion << "\n";
    output << "currentVersion=" << (request.version.empty() ? config_.currentVersion : request.version) << "\n";
    output << "artifactPath=" << artifactPath << "\n";
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
           << ",jobId=" << request.jobId
           << ",fromVersion=" << config_.currentVersion
           << ",toVersion=" << (request.version.empty() ? config_.currentVersion : request.version)
           << ",artifactPath=" << artifactPath
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
           << ",jobId=" << request.jobId
           << ",stage=" << stage
           << ",toVersion=" << (request.version.empty() ? config_.currentVersion : request.version)
           << ",rollbackAttempted=" << (rollbackAttempted ? "true" : "false")
           << ",rollbackSucceeded=" << (rollbackSucceeded ? "true" : "false")
           << ",message=" << message
           << "\n";
}

bool OtaService::tryRollback(const OtaRequest& request, const std::string& artifactPath) const {
    if (config_.rollbackScript.empty()) {
        return false;
    }
    const auto rollbackCommand = buildScriptCommand(config_.rollbackScript, request, artifactPath);
    return std::system(rollbackCommand.c_str()) == 0;
}

void OtaService::cleanupOldArtifacts() const {
    if (config_.retentionCount <= 0) {
        return;
    }
    auto files = listFilesInDirectory(config_.downloadDir);
    if (files.size() <= static_cast<std::size_t>(config_.retentionCount)) {
        return;
    }
    const auto removable = files.size() - static_cast<std::size_t>(config_.retentionCount);
    for (std::size_t i = 0; i < removable; ++i) {
        removeFilePath(files[i]);
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
        publishStatus(*status);
    }
}

void OtaService::downloadArtifact(const OtaRequest& request, const std::string& targetPath) const {
    const auto source = resolveArtifactSource(request);
    if (source.empty()) {
        throw std::runtime_error("ota artifact source is empty");
    }

    if (isHttpUrl(source)) {
#ifdef _WIN32
        const std::string command =
            "powershell -Command \"Invoke-WebRequest -UseBasicParsing -Uri " +
            quoteArg(source) +
            " -OutFile " +
            quoteArg(targetPath) +
            "\"";
#else
        const std::string command =
            std::string("sh -c '"
            "if command -v curl >/dev/null 2>&1; then "
            "curl -L --fail -o \"$1\" \"$2\"; "
            "elif command -v wget >/dev/null 2>&1; then "
            "wget -O \"$1\" \"$2\"; "
            "else "
            "echo \"curl/wget not found\" >&2; exit 127; "
            "fi"
            "' ") +
            " sh " + quoteArg(targetPath) + " " + quoteArg(source);
#endif
        if (std::system(command.c_str()) != 0) {
            throw std::runtime_error("failed to download artifact using curl/wget");
        }
        return;
    }

    std::ifstream input(source.c_str(), std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("artifact source not found: " + source);
    }

    std::ofstream output(targetPath.c_str(), std::ios::binary | std::ios::trunc);
    output << input.rdbuf();
    if (!output.good()) {
        throw std::runtime_error("failed to copy artifact to staging");
    }
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
    if (std::system(command.c_str()) != 0) {
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
    if (std::system(command.c_str()) != 0) {
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

}  // namespace edge_gateway
