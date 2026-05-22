#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

class OtaService {
public:
    explicit OtaService(OtaConfig config);

    OtaService(const OtaService&) = delete;
    OtaService& operator=(const OtaService&) = delete;

    bool enabled() const;
    bool validateRequest(const OtaRequest& request, std::string* errorMessage = nullptr) const;
    OtaReply createAcceptedReply(const OtaRequest& request, const std::string& machineCode, std::int64_t nowMs) const;
    std::vector<OtaStatus> loadPendingStatuses() const;
    void clearPendingStatuses() const;
    void execute(
        const OtaRequest& request,
        const std::string& machineCode,
        std::int64_t nowMs,
        OtaReply* reply,
        OtaStatus* status,
        const std::function<void(const OtaStatus&)>& publishStatus
    ) const;

private:
    std::string resolveArtifactPath(const OtaRequest& request) const;
    std::string resolveArtifactSource(const OtaRequest& request) const;
    std::string buildMinioArtifactUrl(const OtaRequest& request) const;
    void ensureDirectory(const std::string& path) const;
    void writeVersionMarker(const OtaRequest& request, const std::string& artifactPath) const;
    void appendUpgradeRecord(const OtaRequest& request, const std::string& artifactPath, std::int64_t ts) const;
    void appendFailureRecord(
        const OtaRequest& request,
        const std::string& stage,
        const std::string& message,
        bool rollbackAttempted,
        bool rollbackSucceeded,
        std::int64_t ts
    ) const;
    bool tryRollback(const OtaRequest& request, const std::string& artifactPath) const;
    void cleanupOldArtifacts() const;
    std::string statusJournalPath() const;
    void enforceStatusJournalLimit() const;
    void appendPendingStatus(const OtaStatus& status) const;
    void reportStage(
        OtaStatus* status,
        const std::string& stage,
        int progress,
        const std::string& message,
        std::int64_t ts,
        const std::function<void(const OtaStatus&)>& publishStatus
    ) const;
    void reportDownloadProgress(
        OtaStatus* status,
        std::uint64_t downloadedBytes,
        std::uint64_t totalBytes,
        const std::string& message,
        const std::function<void(const OtaStatus&)>& publishStatus
    ) const;
    void downloadArtifact(
        const OtaRequest& request,
        const std::string& targetPath,
        OtaStatus* status,
        const std::function<void(const OtaStatus&)>& publishStatus
    ) const;
    void downloadHttpArtifact(
        const OtaRequest& request,
        const std::string& source,
        const std::string& targetPath,
        OtaStatus* status,
        const std::function<void(const OtaStatus&)>& publishStatus
    ) const;
    void downloadExternalArtifact(
        const OtaRequest& request,
        const std::string& source,
        const std::string& targetPath,
        OtaStatus* status,
        const std::function<void(const OtaStatus&)>& publishStatus
    ) const;
    void copyArtifactWithProgress(
        const OtaRequest& request,
        const std::string& source,
        const std::string& targetPath,
        OtaStatus* status,
        const std::function<void(const OtaStatus&)>& publishStatus
    ) const;
    void verifyChecksum(const OtaRequest& request, const std::string& artifactPath) const;
    void runScript(const std::string& scriptPath, const OtaRequest& request, const std::string& artifactPath) const;
    std::string buildScriptCommand(const std::string& scriptPath, const OtaRequest& request, const std::string& artifactPath) const;
    bool isHttpUrl(const std::string& value) const;
    bool isHttpsUrl(const std::string& value) const;

    OtaConfig config_;
};

}  // namespace edge_gateway
