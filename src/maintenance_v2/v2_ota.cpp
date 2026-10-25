// Maintenance v2 offline upgrade: upload / job state, status view and the job helper (see v2_ota.hpp).
#include "v2_ota.hpp"

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/ota_service.hpp"
#include "v2_files.hpp"
#include "v2_json.hpp"
#include "v2_state.hpp"

#include <algorithm>
#include <memory>
#include <cctype>
#include <chrono>
#include <iostream>
#include <thread>

#ifndef _WIN32
#include <sys/statvfs.h>
#endif

namespace edge_gateway {
namespace maintenance_v2 {

namespace {

bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string lastLine(const std::string& text) {
    std::size_t end = text.size();
    while (end > 0 && (text[end - 1] == '\n' || text[end - 1] == '\r')) --end;
    const auto start = text.rfind('\n', end == 0 ? 0 : end - 1);
    return text.substr(start == std::string::npos ? 0 : start + 1, end - (start == std::string::npos ? 0 : start + 1));
}

// Last parseable status line written by the helper (OtaService publishStatus).
bool lastStatus(const std::string& file, OtaStatus* out) {
    std::string text;
    if (!readFile(file, &text)) return false;
    std::size_t end = text.size();
    while (end > 0) {
        const auto start = text.rfind('\n', end - 1);
        const auto from = start == std::string::npos ? 0 : start + 1;
        const auto line = text.substr(from, end - from);
        end = start == std::string::npos ? 0 : start;
        if (line.empty() || line[0] != '{') continue;
        try {
            const auto v = parseJson(line, 64);
            out->stage = str(v, "stage");
            out->message = str(v, "message");
            std::int64_t n = 0;
            if (integer(v, "progress", &n)) out->progress = static_cast<int>(n);
            if (integer(v, "downloadedBytes", &n) && n >= 0) out->downloadedBytes = static_cast<std::uint64_t>(n);
            if (integer(v, "totalBytes", &n) && n >= 0) out->totalBytes = static_cast<std::uint64_t>(n);
            if (integer(v, "ts", &n)) out->ts = n;
            if (!out->stage.empty()) return true;
        } catch (...) {
        }
    }
    return false;
}

}  // namespace

bool safeOtaId(const std::string& id) {
    if (id.empty() || id.size() > 128 || id == "." || id == ".." || id.find("..") != std::string::npos) return false;
    for (const char c : id) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

bool loadOtaConfig(const DirectConfig& config, OtaConfig* out, std::string* error) {
    try {
        *out = ConfigLoader::loadAppConfigFromFile(config.otaAppConfigFile).ota;
        return true;
    } catch (const std::exception& ex) {
        *error = std::string("cannot read the OTA configuration: ") + ex.what();
        return false;
    }
}

std::uint64_t freeBytes(const std::string& path) {
#ifndef _WIN32
    struct statvfs s {};
    if (::statvfs(path.c_str(), &s) != 0) return 0;
    return static_cast<std::uint64_t>(s.f_bavail) * static_cast<std::uint64_t>(s.f_frsize);
#else
    (void)path;
    return 0;
#endif
}

OtaStore::OtaStore(const Options& options) : options_(options) {}

std::string OtaStore::uploadsDir() const { return options_.stateDirectory + "/ota/uploads"; }
std::string OtaStore::jobsDir() const { return options_.stateDirectory + "/ota/jobs"; }
std::string OtaStore::uploadPath(const std::string& uploadId) const { return uploadsDir() + "/" + uploadId + ".pkg"; }
std::string OtaStore::jobDirectory(const std::string& jobId) const { return jobsDir() + "/" + jobId; }

bool OtaStore::loadUpload(const std::string& uploadId, OtaUploadInfo* out) const {
    if (!safeOtaId(uploadId)) return false;
    std::string text;
    if (!readFile(uploadsDir() + "/" + uploadId + ".json", &text) || !isRegularFile(uploadPath(uploadId))) return false;
    try {
        const auto v = parseJson(text, 64);
        out->uploadId = str(v, "uploadId");
        out->sha256 = str(v, "sha256");
        std::int64_t n = 0;
        if (!integer(v, "size", &n) || n < 0) return false;
        out->size = static_cast<std::uint64_t>(n);
        integer(v, "createdAt", &out->createdAt);
        return out->uploadId == uploadId && out->sha256.size() == 64;
    } catch (...) {
        return false;
    }
}

void OtaStore::removeUpload(const std::string& uploadId) const {
    if (!safeOtaId(uploadId)) return;
    removeFile(uploadPath(uploadId));
    removeFile(uploadsDir() + "/" + uploadId + ".json");
}

void OtaStore::pruneUploads() const {
    std::vector<std::string> inUse;
    for (const auto& id : jobIds()) {
        OtaJobRecord job;
        if (loadJob(id, &job) && job.finalStage.empty()) inUse.push_back(job.uploadId);
    }
    for (const auto& name : listDirectory(uploadsDir())) {
        const auto path = uploadsDir() + "/" + name;
        std::string id;
        if (endsWith(name, ".pkg")) id = name.substr(0, name.size() - 4);
        else if (endsWith(name, ".json")) id = name.substr(0, name.size() - 5);
        else if (name.find(".part") != std::string::npos) {
            // Left by an interrupted upload: callers prune while holding the upload lock.
            removeFile(path);
            continue;
        } else {
            continue;
        }
        if (std::find(inUse.begin(), inUse.end(), id) == inUse.end()) removeFile(path);
    }
}

void OtaStore::pruneStalePartials() const {
    FileLock uploading(uploadsDir() + "/upload.lock", false);
    if (!uploading.locked()) return;  // an upload is writing its partial file right now
    for (const auto& name : listDirectory(uploadsDir())) {
        if (name.find(".part") != std::string::npos) removeFile(uploadsDir() + "/" + name);
    }
}

bool OtaStore::saveJob(const OtaJobRecord& job, std::string* error) const {
    Writer w;
    w.beginObject()
        .field("jobId", job.jobId)
        .field("uploadId", job.uploadId)
        .field("machineCode", job.machineCode)
        .field("packageType", job.packageType)
        .field("version", job.version)
        .field("sha256", job.sha256)
        .fieldInt("size", static_cast<std::int64_t>(job.size))
        .field("keyId", job.keyId)
        .field("signature", job.signature)
        .field("operator", job.operatorName)
        .field("ticketId", job.ticketId)
        .fieldInt("acceptedAt", job.acceptedAt)
        .field("finalStage", job.finalStage)
        .endObject();
    try {
        makeDirectories(jobDirectory(job.jobId));
        writeFileAtomic(jobDirectory(job.jobId) + "/job.json", w.text() + "\n");
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    }
}

bool OtaStore::loadJob(const std::string& jobId, OtaJobRecord* out) const {
    if (!safeOtaId(jobId)) return false;
    std::string text;
    if (!readFile(jobDirectory(jobId) + "/job.json", &text)) return false;
    try {
        const auto v = parseJson(text, 64);
        out->jobId = str(v, "jobId");
        out->uploadId = str(v, "uploadId");
        out->machineCode = str(v, "machineCode");
        out->packageType = str(v, "packageType");
        out->version = str(v, "version");
        out->sha256 = str(v, "sha256");
        std::int64_t n = 0;
        if (integer(v, "size", &n) && n >= 0) out->size = static_cast<std::uint64_t>(n);
        out->keyId = str(v, "keyId");
        out->signature = str(v, "signature");
        out->operatorName = str(v, "operator");
        out->ticketId = str(v, "ticketId");
        integer(v, "acceptedAt", &out->acceptedAt);
        out->finalStage = str(v, "finalStage");
        return out->jobId == jobId;
    } catch (...) {
        return false;
    }
}

std::vector<std::string> OtaStore::jobIds() const {
    std::vector<std::string> out;
    for (const auto& name : listDirectory(jobsDir())) {
        if (safeOtaId(name) && isDirectory(jobsDir() + "/" + name)) out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool OtaStore::helperRunning(const std::string& jobId) const {
    FileLock lock(jobDirectory(jobId) + "/lock", false);
    return !lock.locked();
}

void OtaStore::appendStatus(const std::string& jobId, const OtaStatus& status) const {
    Writer w;
    w.beginObject()
        .field("stage", status.stage)
        .fieldInt("progress", status.progress)
        .fieldInt("downloadedBytes", static_cast<std::int64_t>(status.downloadedBytes))
        .fieldInt("totalBytes", static_cast<std::int64_t>(status.totalBytes))
        .field("message", status.message.size() > 1000 ? status.message.substr(0, 1000) : status.message)
        .fieldInt("ts", status.ts > 0 ? status.ts : nowMs())
        .endObject();
    appendLineDurable(jobDirectory(jobId) + "/status.jsonl", w.text());
}

OtaJobView OtaStore::view(const OtaConfig& ota, const OtaJobRecord& job) const {
    OtaJobView v;
    const auto staging = ota.stagingDir;
    v.intent = isRegularFile(staging + "/ota_job_" + job.jobId + ".json");
    OtaStatus last;
    const bool haveStatus = lastStatus(jobDirectory(job.jobId) + "/status.jsonl", &last);
    if (haveStatus) v.message = last.message;

    // Health evidence written by ota-apply.sh / ota-health-result.py (status healthy|failed, rollback none|succeeded|failed|unchanged|skipped).
    std::string healthStatus, rollback;
    std::string healthText;
    if (readFile(staging + "/health_" + job.jobId + ".txt", &healthText)) {
        try {
            const auto h = parseJson(lastLine(healthText), 64);
            healthStatus = str(h, "status");
            rollback = str(h, "rollback");
        } catch (...) {
        }
    }
    std::string final;
    if (readFile(staging + "/ota_final_" + job.jobId + ".txt", &final)) {
        if (final.find("\nstatus=completed\n") != std::string::npos) {
            v.stage = "completed";
            v.progress = 100;
            v.healthKnown = true;
            v.healthOk = true;
            if (v.message.empty() || last.stage != "completed") v.message = "upgrade completed; services healthy";
            return v;
        }
        if (final.find("\nstatus=failed\n") != std::string::npos) {
            v.progress = 100;
            v.healthKnown = healthStatus == "failed" || healthStatus == "healthy";
            v.healthOk = healthStatus == "healthy";
            if (rollback == "succeeded") {
                v.stage = "rolled-back";
                v.rollbackPerformed = true;
                v.message = "unhealthy after the upgrade; previous version restored";
            } else if (rollback == "failed") {
                v.stage = "failed";
                v.rollbackPerformed = true;
                v.rollbackFailed = true;
                v.message = "unhealthy after the upgrade and the rollback failed; manual recovery required";
            } else {
                v.stage = "failed";
                if (rollback == "unchanged") v.message = "upgrade failed before the runtime was changed (device unchanged)" +
                                                         (last.stage == "failed" && !last.message.empty() ? ": " + last.message : std::string());
            }
            return v;
        }
    }
    if (!healthStatus.empty() || isRegularFile(staging + "/" + job.jobId + "/health_pending.txt")) {
        v.stage = "health-check";
        v.progress = 90;
        v.healthKnown = !healthStatus.empty();
        v.healthOk = healthStatus == "healthy";
        if (v.message.empty()) v.message = "services restarted; waiting for the health result";
        return v;
    }
    if (!haveStatus) return v;
    if (last.stage == "downloading") {
        v.stage = "verifying";
        v.progress = last.totalBytes > 0 ? 10 + static_cast<int>(40 * std::min<std::uint64_t>(last.downloadedBytes, last.totalBytes) / last.totalBytes) : 10;
    } else if (last.stage == "verifying") {
        v.stage = "verifying";
        v.progress = 55;
    } else if (last.stage == "applying") {
        const bool waiting = last.message.find("waiting for restart") != std::string::npos;
        v.stage = waiting ? "health-check" : "applying";
        v.progress = waiting ? 85 : 70;
    } else if (last.stage == "completed") {
        v.stage = "completed";
        v.progress = 100;
        v.healthKnown = true;
        v.healthOk = true;
    } else if (last.stage == "failed") {
        if (v.intent) {
            // OtaService persisted its intent and has not resolved it (no final marker yet): the device may still be
            // changing, so this is not a final result. The job stays open until the OTA service decides.
            v.stage = "applying";
            v.progress = 80;
            v.message += " (the OTA service has not resolved this job yet; waiting for its result before the next upgrade)";
        } else {
            v.stage = "failed";
            v.progress = 100;
        }
        if (isRegularFile(staging + "/ota-in-progress")) v.message += " (ota-in-progress marker present)";
    }
    return v;
}

void OtaStore::finish(OtaJobRecord* job, const OtaJobView& view) const {
    if (!view.terminal() || !job->finalStage.empty()) return;
    // The helper, the status endpoint, admission and housekeeping may all see the terminal result:
    // only the first one audits it.
    FileLock lock(jobDirectory(job->jobId) + "/finish.lock", true);
    OtaJobRecord current;
    if (loadJob(job->jobId, &current) && !current.finalStage.empty()) {
        *job = current;
        return;
    }
    AuditEntry e;
    e.operatorName = job->operatorName;
    e.ticketId = job->ticketId;
    e.action = "ota.job";
    e.target = job->packageType + " " + job->version + " sha256=" + job->sha256;
    e.result = view.stage;
    e.transactionId = job->jobId;
    std::string error;
    if (!AuditLog(options_.stateDirectory + "/audit.jsonl").append(e, &error)) std::cerr << "maintenance v2 audit: " << error << std::endl;
    job->finalStage = view.stage;
    if (!saveJob(*job, &error)) std::cerr << "maintenance v2 ota: " << error << std::endl;
    removeUpload(job->uploadId);
}

int runOtaHelper(const DirectConfig& config, const Options& options, const std::string& jobId, bool watchOnly) {
    OtaStore store(options);
    OtaJobRecord job;
    if (!store.loadJob(jobId, &job)) {
        std::cerr << "ota helper: unknown job " << jobId << std::endl;
        return 2;
    }
    // Status probes (GET /ota/jobs/{id}, housekeeping) take the job lock for an instant: wait a little before
    // concluding that another helper owns the job.
    std::unique_ptr<FileLock> lock;
    for (int attempt = 0; attempt < 50; ++attempt) {
        lock.reset(new FileLock(store.jobDirectory(jobId) + "/lock", false));
        if (lock->locked()) break;
        lock.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!lock) return 0;  // another helper owns the job
    OtaConfig ota;
    std::string error;
    if (!loadOtaConfig(config, &ota, &error)) {
        OtaStatus failed;
        failed.stage = "failed";
        failed.message = error;
        store.appendStatus(jobId, failed);
        store.finish(&job, store.view(ota, job));
        return 1;
    }
    OtaService service(ota);
    if (!watchOnly) {
        OtaRequest request;
        request.jobId = job.jobId;
        request.machineCode = job.machineCode;
        request.packageType = job.packageType;
        request.artifactUrl = store.uploadPath(job.uploadId);
        request.version = job.version;
        request.sha256 = job.sha256;
        request.size = job.size;
        request.upgradeMode = "download_install_reboot";
        request.signature = job.signature;
        request.ts = nowMs();
        OtaReply reply;
        OtaStatus status;
        try {
            service.execute(request, job.machineCode, nowMs(), &reply, &status,
                            [&](const OtaStatus& s) { store.appendStatus(jobId, s); });
        } catch (const OtaRequestRejected& ex) {
            // Refused before the OTA service accepted it (nothing changed).
            OtaStatus failed;
            failed.stage = "failed";
            failed.message = std::string("not admitted: ") + ex.what();
            store.appendStatus(jobId, failed);
        } catch (const std::exception& ex) {
            // execute reports its own failure stage; keep the reason when it did not.
            OtaStatus last;
            if (!lastStatus(store.jobDirectory(jobId) + "/status.jsonl", &last) || last.stage != "failed") {
                OtaStatus failed;
                failed.stage = "failed";
                failed.message = ex.what();
                store.appendStatus(jobId, failed);
            }
        }
    }
    // The restart and health check run in their own unit; wait for the evidence and finalize it.
    const std::int64_t deadline = job.acceptedAt + (static_cast<std::int64_t>(std::max(60, ota.upgradeTimeoutSec)) + 300) * 1000;
    while (true) {
        auto view = store.view(ota, job);
        if (view.terminal()) {
            store.finish(&job, view);
            return view.stage == "completed" ? 0 : 1;
        }
        if (nowMs() > deadline) {
            std::cerr << "ota helper: no result for " << jobId << " before the deadline" << std::endl;
            return 1;
        }
        try {
            service.reconcilePendingJobs();
        } catch (const std::exception& ex) {
            std::cerr << "ota helper: reconcile: " << ex.what() << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

}  // namespace maintenance_v2
}  // namespace edge_gateway
