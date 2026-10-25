// Maintenance v2 offline upgrade endpoints (handoff doc 3.9): POST /ota/uploads, POST /ota/jobs, GET /ota/jobs/{id}.
#include "v2_service.hpp"

#include "edge_gateway/ota_service.hpp"
#include "v2_crypto.hpp"
#include "v2_files.hpp"
#include "v2_json.hpp"
#include "v2_ota.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iostream>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace edge_gateway {
namespace maintenance_v2 {

namespace {

struct OtaCheck {
    std::string rule;
    std::string severity;
    std::string message;
};

bool isHex64(const std::string& text) {
    return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string newUploadId() {
    const auto now = nowMs();
    const std::time_t seconds = static_cast<std::time_t>(now / 1000);
    std::tm tm {};
#ifndef _WIN32
    gmtime_r(&seconds, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    std::string random;
    Crypto::instance().randomBytes(3, &random);
    return std::string("UP-") + stamp + "-" + toHex(random);
}

void writeChecks(Writer& w, const std::vector<OtaCheck>& checks) {
    w.key("checks").beginArray();
    for (const auto& c : checks) w.beginObject().field("rule", c.rule).field("severity", c.severity).field("message", c.message).endObject();
    w.endArray();
}

}  // namespace

// ---------------------------------------------------------------- POST /ota/uploads

Response Service::otaUpload(const Request& r) {
    if (!r.bodyStreamed || !r.readBody) return errorResponse(400, "validation_failed", "package body is required");
    const auto declared = lower(r.header("x-gz-sha256"));
    if (!isHex64(declared)) return errorResponse(400, "validation_failed", "X-GZ-Sha256 (64 hex digits) is required");
    OtaConfig ota;
    std::string error;
    if (!loadOtaConfig(config_, &ota, &error)) return errorResponse(503, "ota_unavailable", error);
    if (!ota.enabled) return errorResponse(409, "ota_disabled", "OTA is disabled in the device configuration");
    if (r.contentLength == 0) return errorResponse(400, "validation_failed", "empty package");
    if (r.contentLength > ota.maxArtifactBytes) {
        return errorResponse(413, "package_too_large", "package exceeds ota.maxArtifactBytes (" + std::to_string(ota.maxArtifactBytes) + ")");
    }
    try {
        makeDirectories(otas_.uploadsDir());
    } catch (const std::exception& ex) {
        return errorResponse(500, "internal_error", ex.what());
    }
    FileLock uploading(otas_.uploadsDir() + "/upload.lock", false);
    if (!uploading.locked()) return errorResponse(409, "upload_in_progress", "another package is being uploaded");
    otas_.pruneUploads();  // an operator re-uploading makes older, unused uploads obsolete
    const auto available = freeBytes(otas_.uploadsDir());
    if (available > 0 && available < r.contentLength + ota.minFreeBytes) {
        return errorResponse(507, "insufficient_storage", "not enough free space for the package (" + std::to_string(available) + " bytes free)");
    }
#ifdef _WIN32
    return errorResponse(501, "not_implemented", "uploads need POSIX");
#else
    Sha256Stream hash;
    if (!hash.ok()) return errorResponse(503, "crypto_unavailable", "SHA-256 is not available on this device");
    const auto id = newUploadId();
    const auto part = otas_.uploadsDir() + "/" + id + ".pkg.part";
    const int fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return errorResponse(500, "internal_error", "cannot create the upload file");
    std::uint64_t received = 0;
    bool ok = true;
    auto consume = [&](const char* data, std::size_t size) {
        hash.update(data, size);
        std::size_t done = 0;
        while (done < size) {
            const auto n = ::write(fd, data + done, size - done);
            if (n <= 0) return false;
            done += static_cast<std::size_t>(n);
        }
        received += size;
        return true;
    };
    if (!r.body.empty()) ok = consume(r.body.data(), r.body.size());
    char buffer[65536];
    while (ok && received < r.contentLength) {
        const auto want = static_cast<int>(std::min<std::uint64_t>(sizeof(buffer), r.contentLength - received));
        const int n = r.readBody(buffer, want);
        if (n <= 0) {
            ok = false;
            error = "upload interrupted after " + std::to_string(received) + " of " + std::to_string(r.contentLength) + " bytes";
            break;
        }
        if (!consume(buffer, static_cast<std::size_t>(n))) {
            ok = false;
            error = "cannot write the upload file (disk full?)";
        }
    }
    if (ok && ::fsync(fd) != 0) {
        ok = false;
        error = "cannot flush the upload file";
    }
    ::close(fd);
    if (!ok) {
        removeFile(part);
        return errorResponse(400, "upload_incomplete", error);
    }
    const auto actual = hash.finishHex();
    if (actual != declared) {
        removeFile(part);
        return errorResponse(400, "sha256_mismatch", "uploaded bytes have sha256 " + actual + ", not the declared " + declared);
    }
    const auto pkg = otas_.uploadPath(id);
    if (::rename(part.c_str(), pkg.c_str()) != 0) {
        removeFile(part);
        return errorResponse(500, "internal_error", "cannot store the upload");
    }
    Writer meta;
    meta.beginObject().field("uploadId", id).field("sha256", actual).fieldInt("size", static_cast<std::int64_t>(received)).fieldInt("createdAt", nowMs()).endObject();
    try {
        writeFileAtomic(otas_.uploadsDir() + "/" + id + ".json", meta.text() + "\n");
    } catch (const std::exception& ex) {
        removeFile(pkg);
        return errorResponse(500, "internal_error", ex.what());
    }
    record("ota.upload", Ticket(), id + " sha256=" + actual + " size=" + std::to_string(received), "ok");
    Writer w;
    w.beginObject().fieldBool("success", true).field("uploadId", id).field("sha256", actual).fieldInt("size", static_cast<std::int64_t>(received)).endObject();
    return jsonResponse(200, w.text());
#endif
}

// ---------------------------------------------------------------- POST /ota/jobs

Response Service::otaJob(const Request& r) {
    JsonValue body;
    try {
        body = parseJson(r.body, 2000);
    } catch (const std::exception& ex) {
        return errorResponse(400, "validation_failed", ex.what());
    }
    const bool dryRun = boolean(body, "dryRun", true);
    OtaJobRecord job;
    job.jobId = str(body, "jobId");
    job.machineCode = str(body, "machineCode");
    job.packageType = str(body, "packageType");
    job.version = str(body, "version");
    job.sha256 = lower(str(body, "sha256"));
    job.keyId = str(body, "keyId");
    job.signature = str(body, "signature");
    job.uploadId = str(body, "uploadId");
    std::int64_t size = 0;
    if (!integer(body, "size", &size) || size <= 0) return errorResponse(400, "validation_failed", "size must be a positive integer");
    job.size = static_cast<std::uint64_t>(size);
    if (!safeOtaId(job.jobId)) return errorResponse(400, "validation_failed", "jobId must be 1-128 of [A-Za-z0-9._-]");
    const auto machine = deviceMachineCode();
    if (job.machineCode != machine) return errorResponse(400, "machine_mismatch", "machineCode must be " + machine);

    Ticket ticket;
    if (!dryRun) {
        // Authorization first (without using up a one-time ticket), then the package checks, then admission.
        const auto verdict = checkTicket(r, "ota.job", requiredLevel("ota.job", {}), {}, {}, std::string(), false);
        if (!verdict.ok) return errorResponse(403, verdict.code, verdict.message);
    }

    std::vector<OtaCheck> checks;
    int failStatus = 0;
    std::string failCode, failMessage;
    auto fail = [&](int status, const std::string& code, const std::string& rule, const std::string& message) {
        checks.push_back(OtaCheck{rule, "error", message});
        if (failStatus == 0) {
            failStatus = status;
            failCode = code;
            failMessage = message;
        }
    };
    auto info = [&](const std::string& rule, const std::string& message) { checks.push_back(OtaCheck{rule, "info", message}); };

    OtaConfig ota;
    std::string error;
    if (!loadOtaConfig(config_, &ota, &error)) return errorResponse(503, "ota_unavailable", error);
    OtaService service(ota);
    OtaUploadInfo upload;
    if (!otas_.loadUpload(job.uploadId, &upload)) {
        fail(404, "upload_not_found", "ota.upload", "upload " + job.uploadId + " is not on the device (upload the package first)");
    } else if (upload.sha256 != job.sha256 || upload.size != job.size) {
        fail(400, "validation_failed", "ota.package", "the uploaded package (sha256 " + upload.sha256.substr(0, 16) + "..., " + std::to_string(upload.size) +
                                                          " bytes) is not the one the job describes");
    } else {
        info("ota.package", "uploaded package matches the job: sha256 and size");
    }
    if (!ota.enabled) fail(409, "ota_disabled", "ota.enabled", "OTA is disabled in the device configuration");
    if (job.packageType != "full" && job.packageType != "config" && job.packageType != "scada") {
        fail(400, "unsupported_package_type", "ota.packageType", "packageType must be full, config or scada");
    } else {
        info("ota.packageType", "package type " + job.packageType + " is supported");
    }
    OtaJobRecord existing;
    if (otas_.loadJob(job.jobId, &existing)) fail(409, "job_exists", "ota.jobId", "jobId " + job.jobId + " was already used");
    for (const auto& id : otas_.jobIds()) {
        OtaJobRecord other;
        if (id == job.jobId || !otas_.loadJob(id, &other) || !other.finalStage.empty()) continue;
        // a job whose result is already final but whose helper has not closed it yet (it polls every 2 s) is not busy
        const auto view = otas_.view(ota, other);
        if (view.terminal()) {
            otas_.finish(&other, view);
            continue;
        }
        fail(409, "ota_busy", "ota.busy", "OTA job " + id + " has not finished");
        break;
    }
    OtaRequest request;
    request.jobId = job.jobId;
    request.machineCode = machine;
    request.packageType = job.packageType;
    request.version = job.version;
    request.sha256 = job.sha256;
    request.size = job.size;
    request.signature = job.signature;
    request.artifactUrl = otas_.uploadPath(job.uploadId);
    if (failStatus == 0) {
        OtaRejectionReason reason = OtaRejectionReason::NotAdmitted;
        if (!service.validateRequest(request, &error, &reason)) {
            if (reason == OtaRejectionReason::Duplicate || error.find("already") != std::string::npos) {
                fail(409, "job_exists", "ota.jobId", error);
            } else if (error.find("unresolved") != std::string::npos || error.find("in-progress") != std::string::npos) {
                fail(409, "ota_busy", "ota.busy", error);
            } else {
                fail(400, "validation_failed", "ota.request", error);
            }
        }
    }
    if (failStatus == 0) {
        // v2 accepts only signed packages and checks the signature before accepting the job (v1 checked after 202).
        try {
            service.verifySignatureStrict(request, machine);
            info("ota.signature", "edge-ota-v1 signature verified with the device OTA public key" +
                                      (job.keyId.empty() ? std::string() : " (keyId " + job.keyId + " recorded; the device has one OTA key)"));
        } catch (const std::exception& ex) {
            const std::string message = ex.what();
            if (message.find("public key not found") != std::string::npos) fail(503, "ota_key_unavailable", "ota.signature", message);
            else fail(400, "bad_package_signature", "ota.signature", message);
        }
    }
    if (failStatus == 0) {
        // OtaService needs room for its download copy, the backup and the work copy.
        const auto available = freeBytes(ota.downloadDir);
        const auto needed = ota.minFreeBytes + job.size * 3;
        if (available > 0 && available < needed) {
            fail(507, "insufficient_storage", "ota.space", std::to_string(available) + " bytes free under " + ota.downloadDir + ", " + std::to_string(needed) + " needed");
        } else if (available > 0) {
            info("ota.space", std::to_string(available / (1024 * 1024)) + " MB free under " + ota.downloadDir);
        }
        info("ota.version", "installed " + service.installedVersion() + " -> " + job.version);
    }

    if (dryRun) {
        Writer w;
        w.beginObject().fieldBool("success", failStatus == 0).fieldBool("dryRun", true);
        if (failStatus != 0) w.field("code", failCode);
        w.field("message", failStatus == 0 ? std::string("package verified; ready to install") : failMessage);
        w.field("jobId", job.jobId).field("currentVersion", service.installedVersion()).field("version", job.version);
        writeChecks(w, checks);
        w.fieldInt("ts", nowMs()).endObject();
        return jsonResponse(200, w.text());
    }
    if (failStatus != 0) {
        Writer w;
        w.beginObject().fieldBool("success", false).field("code", failCode).field("message", failMessage);
        writeChecks(w, checks);
        w.endObject();
        return jsonResponse(failStatus, w.text());
    }
    const auto verdict = checkTicket(r, "ota.job", requiredLevel("ota.job", {}), {}, {}, std::string(), true);
    if (!verdict.ok) return errorResponse(403, verdict.code, verdict.message);
    try {
        makeDirectories(otas_.jobsDir());
    } catch (const std::exception& ex) {
        return errorResponse(500, "internal_error", ex.what());
    }
    FileLock admission(otas_.jobsDir() + "/admission.lock", true);
    if (otas_.loadJob(job.jobId, &existing)) return errorResponse(409, "job_exists", "jobId " + job.jobId + " was already used");
    // A config transaction (or its service restart) and an OTA job never run at once: both restart gateway services.
    std::lock_guard<std::mutex> configLock(configMutex_);
    {
        Transaction active;
        if (txs_.findActive(&active)) {
            return errorResponse(409, "apply_in_progress", "transaction " + active.id + " is still " + (active.finished() ? "restarting services" : active.state));
        }
    }
    job.machineCode = machine;
    job.operatorName = verdict.ticket.operatorLabel();
    job.ticketId = verdict.ticket.ticketId;
    job.acceptedAt = nowMs();
    if (!otas_.saveJob(job, &error)) return errorResponse(500, "internal_error", error);
    if (!txs_.launchHelper(job.jobId, "ota", &error)) {
        OtaStatus failed;
        failed.stage = "failed";
        failed.message = "cannot start the OTA helper: " + error;
        otas_.appendStatus(job.jobId, failed);
        otas_.finish(&job, otas_.view(ota, job));
        return errorResponse(500, "helper_failed", failed.message);
    }
    record("ota.job", verdict.ticket, job.packageType + " " + job.version + " sha256=" + job.sha256, "accepted", job.jobId);
    Writer w;
    w.beginObject().fieldBool("success", true).field("jobId", job.jobId).field("stage", "accepted").field("version", job.version).endObject();
    return jsonResponse(202, w.text());
}

// ---------------------------------------------------------------- GET /ota/jobs/{id}

Response Service::otaJobStatus(const std::string& id) {
    OtaJobRecord job;
    if (!otas_.loadJob(id, &job)) return errorResponse(404, "job_not_found", "no OTA job " + id + " on this device");
    OtaConfig ota;
    std::string error;
    if (!loadOtaConfig(config_, &ota, &error)) return errorResponse(503, "ota_unavailable", error);
    const auto v = otas_.view(ota, job);
    if (v.terminal() && job.finalStage.empty()) otas_.finish(&job, v);  // close it now: the caller may start the next job right away
    Writer w;
    w.beginObject()
        .fieldBool("success", true)
        .field("jobId", job.jobId)
        .field("stage", v.stage)
        .fieldInt("progress", v.progress)
        .field("version", job.version)
        .field("packageType", job.packageType)
        .field("message", v.message)
        .fieldInt("acceptedAt", job.acceptedAt);
    if (v.healthKnown) w.key("health").beginObject().fieldBool("ok", v.healthOk).key("failed").beginArray().endArray().endObject();
    w.key("rollback").beginObject().fieldBool("performed", v.rollbackPerformed).fieldBool("failed", v.rollbackFailed).endObject();
    w.fieldBool("helperRunning", !v.terminal() && otas_.helperRunning(job.jobId)).fieldInt("ts", nowMs()).endObject();
    return jsonResponse(200, w.text());
}

// Jobs whose helper died (power loss, kill): watch the OtaService evidence again, or close out a job that
// never reached the OTA service; audit terminal results the helper did not get to.
bool Service::otaJobUnfinished(std::string* jobId) {
    const auto ids = otas_.jobIds();
    if (ids.empty()) return false;
    OtaConfig ota;
    std::string error;
    if (!loadOtaConfig(config_, &ota, &error)) return false;
    for (const auto& id : ids) {
        OtaJobRecord job;
        if (!otas_.loadJob(id, &job) || !job.finalStage.empty()) continue;
        const auto view = otas_.view(ota, job);
        if (view.terminal()) {
            otas_.finish(&job, view);
            continue;
        }
        if (jobId) *jobId = id;
        return true;
    }
    return false;
}

void Service::otaHousekeeping() {
    otas_.pruneStalePartials();
    const auto ids = otas_.jobIds();
    if (ids.empty()) return;
    OtaConfig ota;
    std::string error;
    if (!loadOtaConfig(config_, &ota, &error)) return;
    for (const auto& id : ids) {
        OtaJobRecord job;
        if (!otas_.loadJob(id, &job) || !job.finalStage.empty() || otas_.helperRunning(id)) continue;
        if (nowMs() - job.acceptedAt < 15000) continue;  // the helper may still be starting
        auto v = otas_.view(ota, job);
        if (v.terminal()) {
            otas_.finish(&job, v);
            continue;
        }
        if (!v.intent) {
            OtaStatus failed;
            failed.stage = "failed";
            failed.message = "the OTA helper stopped before the OTA service accepted the job (device unchanged)";
            otas_.appendStatus(id, failed);
            otas_.finish(&job, otas_.view(ota, job));
            continue;
        }
        const auto deadline = job.acceptedAt + (static_cast<std::int64_t>(std::max(60, ota.upgradeTimeoutSec)) + 300) * 1000;
        if (nowMs() > deadline) continue;  // unresolved: left for the OTA recovery service and the operator
        if (!txs_.launchHelper(id, "ota-watch", &error)) std::cerr << "maintenance v2: cannot watch OTA job " << id << ": " << error << std::endl;
    }
}

}  // namespace maintenance_v2
}  // namespace edge_gateway
