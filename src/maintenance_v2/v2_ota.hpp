#pragma once

// Maintenance v2 offline upgrade (handoff doc 3.9): uploads into the maintenance state directory, jobs run by
// the existing OtaService (same signature, checksum, apply / rollback scripts and health evidence as platform
// OTA) inside a helper process that survives a gateway-services restart.
//
// State directory:
//   ota/uploads/<uploadId>.pkg, <uploadId>.json      uploaded package + {uploadId, sha256, size, createdAt}
//   ota/jobs/<jobId>/job.json, status.jsonl, lock    accepted job, OtaService status lines, helper lock

#include <cstdint>
#include <string>
#include <vector>

#include "edge_gateway/maintenance_v2.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {
namespace maintenance_v2 {

struct OtaJobRecord {
    std::string jobId;
    std::string uploadId;
    std::string machineCode;
    std::string packageType;
    std::string version;
    std::string sha256;
    std::uint64_t size = 0;
    std::string keyId;
    std::string signature;
    std::string operatorName;
    std::string ticketId;
    std::int64_t acceptedAt = 0;
    std::string finalStage;  // set once the terminal result is audited
};

// What GET /ota/jobs/{id} reports. stage: accepted -> verifying -> applying -> health-check ->
// completed | rolled-back | failed.
struct OtaJobView {
    std::string stage = "accepted";
    int progress = 0;
    std::string message;
    bool healthKnown = false;
    bool healthOk = false;
    bool rollbackPerformed = false;
    bool rollbackFailed = false;
    bool intent = false;  // OtaService persisted its job intent (the device may have changed)
    bool terminal() const { return stage == "completed" || stage == "rolled-back" || stage == "failed"; }
};

struct OtaUploadInfo {
    std::string uploadId;
    std::string sha256;
    std::uint64_t size = 0;
    std::int64_t createdAt = 0;
};

class OtaStore {
public:
    explicit OtaStore(const Options& options);
    std::string uploadsDir() const;
    std::string jobsDir() const;
    std::string uploadPath(const std::string& uploadId) const;
    bool loadUpload(const std::string& uploadId, OtaUploadInfo* out) const;
    void removeUpload(const std::string& uploadId) const;
    // Removes uploads (and stale partial files) that no unfinished job uses.
    void pruneUploads() const;

    bool saveJob(const OtaJobRecord& job, std::string* error) const;
    bool loadJob(const std::string& jobId, OtaJobRecord* out) const;
    std::vector<std::string> jobIds() const;
    std::string jobDirectory(const std::string& jobId) const;
    bool helperRunning(const std::string& jobId) const;
    void appendStatus(const std::string& jobId, const OtaStatus& status) const;
    // Reads the OtaService evidence (final marker, health result, pending health) and the helper's status lines.
    OtaJobView view(const OtaConfig& ota, const OtaJobRecord& job) const;
    // Audits the terminal result once (helper or housekeeping) and drops the upload.
    void finish(OtaJobRecord* job, const OtaJobView& view) const;

private:
    Options options_;
};

bool safeOtaId(const std::string& id);
bool loadOtaConfig(const DirectConfig& config, OtaConfig* out, std::string* error);
std::uint64_t freeBytes(const std::string& path);  // 0 when unknown

// Helper process: run the job (watchOnly=false) or only wait for its health evidence (watchOnly=true).
int runOtaHelper(const DirectConfig& config, const Options& options, const std::string& jobId, bool watchOnly);

}  // namespace maintenance_v2
}  // namespace edge_gateway
