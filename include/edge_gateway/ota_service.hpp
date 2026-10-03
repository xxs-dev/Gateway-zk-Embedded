#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <stdexcept>
#include <vector>

#include "edge_gateway/models.hpp"
#include "edge_gateway/ota_query_protocol.hpp"

namespace edge_gateway {

enum class OtaRejectionReason { NotAdmitted, Duplicate };

struct OtaOriginalJobEvidence {
    std::string outcome = "UNKNOWN";
    std::string jobId;
    std::string machineCode;
    std::string occurrenceId;
    std::string version;
    std::string artifactSha256;
    std::uint64_t artifactSize = 0;
    std::string productionTransaction;
    std::string productionCandidateSha256;
    std::string intentSha256;
    std::string healthSha256;
    std::int64_t healthTs = 0;
};

struct OtaDisplayIdentity {
    std::string jobId;
    std::string machineCode;
    std::string occurrenceId;
};

struct OtaReadOnlyFinalStatuses {
    std::vector<OtaStatus> items;
    // At most one identity per inspected directory entry (4096); includes trimmed finals.
    std::vector<OtaDisplayIdentity> terminalJobs;
    bool truncated = false;
};

class OtaRequestRejected : public std::runtime_error {
public:
    OtaRequestRejected(OtaRejectionReason reason, const std::string& message)
        : std::runtime_error(message), reason_(reason) {}
    OtaRejectionReason reason() const { return reason_; }
private:
    OtaRejectionReason reason_;
};

class OtaService {
public:
    explicit OtaService(OtaConfig config);

    OtaService(const OtaService&) = delete;
    OtaService& operator=(const OtaService&) = delete;

    bool enabled() const;
    bool validateRequest(const OtaRequest& request, std::string* errorMessage = nullptr,
                         OtaRejectionReason* reason = nullptr) const;
    // Caller must establish the target device before probing a safe job ID.
    // A duplicate is not a new failure of the original task.
    void rejectDuplicateRequest(const std::string& jobId) const;
    OtaReply createAcceptedReply(const OtaRequest& request, const std::string& machineCode, std::int64_t nowMs) const;
    // JSON-lines v2 retains exact strings, decimal integers and occurrenceId.
    // Legacy TSV retains its original timestamp and empty occurrenceId: never
    // invent a new identity while replaying already-published legacy statuses.
    // One replay consumer per instance: clear removes only the last load snapshot,
    // leaving subsequent appends intact. I/O/corruption errors are not acceptance.
    // Topic is not journaled. A configured topic change is a new destination;
    // cross-destination idempotence is not guaranteed.
    // maxPendingStatusBytes evicts old nonterminal records only, wherever they
    // occur. One terminal record of bounded headroom may exceed that limit;
    // an all-terminal backlog fails closed until local delivery is resolved.
    // Linux acceptance requires file and directory fsync; storage failures throw
    // before publish. This does not promise durability on dishonest hardware.
    std::vector<OtaStatus> loadPendingStatuses() const;
    void reconcilePendingJobs() const;
    std::vector<OtaStatus> loadFinalStatuses() const;
    // Local evidence only. No transport authentication is implied by this result.
    // This method never creates a file, takes an execution/journal lock, or repairs state.
    OtaOriginalJobEvidence readOriginalJob(const std::string& jobId) const;
    OtaOriginalJobEvidence readOriginalJob(const std::string& jobId,
                                          const std::string& expectedMachineCode,
                                          const std::string& expectedOccurrenceId) const;
    OtaQueryObservation answerOriginalJobQuery(const OtaQueryChallenge& challenge,
                                               const std::string& expectedMachineCode) const;
    OtaReadOnlyFinalStatuses loadFinalStatusesReadOnly(
        std::size_t displayReadBudget = 64 * 1024 * 1024) const;
    void clearPendingStatuses() const;
    void execute(
        const OtaRequest& request,
        const std::string& machineCode,
        std::int64_t nowMs,
        OtaReply* reply,
        OtaStatus* status,
        const std::function<void(const OtaStatus&)>& publishStatus
    ) const;
    // Canonical "edge-ota-v1" descriptor covered by OtaRequest::signature
    // (see doc/评审/OTA签名说明.md). machineCode is the target gateway.
    static std::string signatureDescriptor(const OtaRequest& request, const std::string& machineCode);

private:
    OtaOriginalJobEvidence readOriginalJobBounded(const std::string& jobId,
                                                  std::size_t* displayReadBudget) const;
    friend struct OtaServiceTestAccess;
    friend struct OtaChecksumTestAccess;
    std::string resolveArtifactPath(const OtaRequest& request) const;
    std::string resolveArtifactSource(const OtaRequest& request) const;
    std::string buildMinioArtifactUrl(const OtaRequest& request) const;
    void ensureDirectory(const std::string& path) const;
    void writeVersionMarker(const OtaRequest& request, const std::string& artifactPath,
                            const std::string& previousVersion) const;
    void appendUpgradeRecord(const OtaRequest& request, const std::string& artifactPath,
                             const std::string& previousVersion, std::int64_t ts) const;
    std::string readInstalledVersion() const;
    void ensureJobNotProcessed(const OtaRequest& request) const;
    void ensureNoUnresolvedJob() const;
    void ensureTransportAllowed(const std::string& source) const;
    void verifySignature(const OtaRequest& request, const std::string& machineCode) const;
    void appendFailureRecord(
        const OtaRequest& request,
        const std::string& stage,
        const std::string& message,
        bool rollbackAttempted,
        bool rollbackSucceeded,
        std::int64_t ts
    ) const;
    bool tryRollback(const OtaRequest& request, const std::string& artifactPath,
                     const std::string& token, int lifecycleFd = -1) const;
    void cleanupOldArtifacts(const std::string& keepFileName) const;
    std::string statusJournalPath() const;
    void appendPendingStatus(OtaStatus& status) const;
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
    void runScript(const std::string& scriptPath, const OtaRequest& request,
                   const std::string& artifactPath, const std::string& token, int lifecycleFd = -1) const;
    std::string buildScriptCommand(const std::string& scriptPath, const OtaRequest& request,
                                   const std::string& artifactPath, const std::string& token = {}) const;
    bool isHttpUrl(const std::string& value) const;
    bool isHttpsUrl(const std::string& value) const;

    OtaConfig config_;
    mutable std::mutex pendingMutex_;
    mutable std::string pendingSnapshot_;
};

}  // namespace edge_gateway
