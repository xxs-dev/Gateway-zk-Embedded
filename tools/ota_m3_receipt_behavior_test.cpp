#include "../src/ota_service.cpp"
#include "edge_gateway/builtin_mqtt_driver_publisher.hpp"

#include <filesystem>
#include <functional>
#include <atomic>
#include <thread>

extern "C" int __real_rename(const char*, const char*);
extern "C" int __real_fsync(int);

std::atomic<bool> armFinalSyncFault{false};
std::atomic<bool> failNextDirectorySync{false};
std::atomic<bool> armSyncPendingFault{false};
std::atomic<bool> syncPendingFaultSetupFailed{false};
std::atomic<bool> armPreparedFinalFault{false};

extern "C" int __wrap_rename(const char* source, const char* destination) {
    if (armPreparedFinalFault.load() &&
        std::string(destination).find("ota_final_M3_W1_PREPARED.txt") != std::string::npos) {
        armPreparedFinalFault.store(false);
        errno = EIO;
        return -1;
    }
    const int result = __real_rename(source, destination);
    if (result == 0 && armFinalSyncFault.load() &&
        std::string(destination).find("ota_final_M3_PRODUCTION_JOB.txt") != std::string::npos) {
        armFinalSyncFault.store(false);
        failNextDirectorySync.store(true);
    }
    if (result == 0 && armSyncPendingFault.load() &&
        std::string(destination).find("ota_final_M3_SYNC_FAULT.txt") != std::string::npos) {
        armSyncPendingFault.store(false);
        const std::string path(destination);
        const auto lock = path.substr(0, path.find_last_of('/')) + "/ota_status_pending.log.lock";
        if (__real_rename(lock.c_str(), (lock + ".saved").c_str()) != 0 ||
            mkdir(lock.c_str(), 0700) != 0) syncPendingFaultSetupFailed.store(true);
    }
    return result;
}

extern "C" int __wrap_fsync(int fd) {
    struct stat info{};
    if (failNextDirectorySync.load() && fstat(fd, &info) == 0 && S_ISDIR(info.st_mode)) {
        failNextDirectorySync.store(false);
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

namespace edge_gateway {

struct OtaServiceTestAccess {
    static void append(OtaService& service, OtaStatus& status) { service.appendPendingStatus(status); }
};

void requireM3(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void rejectsM3(const std::function<void()>& call, const char* message) {
    try { call(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}

void checkEncoded(const std::string& bytes, const OtaStatus& expected, bool production) {
    const auto value = json::JsonParser(bytes, 4, 32).parse();
    requireM3(pendingField(value, "jobId") == expected.jobId, "serializer lost original job");
    if (expected.occurrenceId.empty()) {
        requireM3(!value.find("occurrenceId"), "serializer invented historical occurrence");
    } else {
        requireM3(pendingField(value, "occurrenceId") == expected.occurrenceId, "serializer lost occurrence");
    }
    requireM3(pendingField(value, "message") == expected.message, "serializer damaged escaped text");
    if (production) {
        requireM3(pendingField(value, "appliedVersion") == expected.appliedVersion,
                  "serializer lost applied version");
        requireM3(pendingField(value, "appliedSha256") == expected.appliedSha256,
                  "serializer lost artifact SHA");
        requireM3(pendingField(value, "productionTransaction") == expected.productionTransaction,
                  "serializer lost transaction");
        requireM3(pendingField(value, "productionCandidateSha256") == expected.productionCandidateSha256,
                  "serializer lost candidate SHA");
        requireM3(pendingField(value, "productionHealth") == expected.productionHealth,
                  "serializer lost health result");
    } else {
        requireM3(!value.find("productionTransaction") && !value.find("productionCandidateSha256") &&
                  !value.find("productionHealth") && !value.find("appliedVersion"),
                  "legacy serializer invented production proof");
    }
}

void checkSerializers(const OtaStatus& status, bool production) {
    checkEncoded(encodeBuiltinOtaStatusJson(status), status, production);
}

void checkPending(const std::string& staging, const OtaStatus& expected) {
    replacePendingJournal(joinPath(staging, "ota_status_pending.log"), encodePendingStatus(expected));
    OtaConfig config;
    config.stagingDir = staging;
    const auto loaded = OtaService(config).loadPendingStatuses();
    requireM3(loaded.size() == 1, "restarted service lost pending status");
    const auto& actual = loaded.front();
    requireM3(actual.jobId == expected.jobId && actual.occurrenceId == expected.occurrenceId &&
              actual.productionTransaction == expected.productionTransaction &&
              actual.productionCandidateSha256 == expected.productionCandidateSha256 &&
              actual.productionHealth == expected.productionHealth,
              "pending journal lost production binding");
    auto damaged = encodePendingStatus(expected);
    const auto key = damaged.find("\"productionCandidateSha256\":\"");
    requireM3(key != std::string::npos, "pending fixture missing candidate");
    const auto valueEnd = damaged.find('"', key + std::string("\"productionCandidateSha256\":\"").size());
    requireM3(valueEnd != std::string::npos, "pending fixture candidate was unterminated");
    damaged.replace(key, valueEnd - key + 1, "\"productionCandidateSha256\":42");
    requireM3(json::JsonParser(damaged, 4, 32).parse().find("productionCandidateSha256")->isNumber(),
              "pending negative must be syntactically valid numeric JSON");
    replacePendingJournal(joinPath(staging, "ota_status_pending.log"), damaged);
    rejectsM3([&] { (void)OtaService(config).loadPendingStatuses(); },
              "non-string production pending receipt was accepted");
}

void checkFinal(const std::string& staging, const OtaStatus& expected, bool production) {
    PendingOtaJob job;
    job.jobId = expected.jobId;
    job.machineCode = expected.machineCode;
    job.version = expected.appliedVersion.empty() ? "legacy-1" : expected.appliedVersion;
    job.artifactPath = "/tmp/ota-m3-fixture-artifact";
    job.previousVersion = "old";
    job.token = expected.occurrenceId;
    if (production) {
        job.lifecycle = "systemd-handoff-v1";
        job.packageType = "config";
        job.artifactSha256 = expected.appliedSha256;
        job.artifactSize = 1;
        job.leaseInode = 1;
    }
    const auto health = "{\"format\":\"ota-health-v1\",\"jobId\":" + pendingJsonString(job.jobId) +
        ",\"version\":" + pendingJsonString(job.version) +
        ",\"token\":" + pendingJsonString(job.token) +
        ",\"status\":\"healthy\",\"rollback\":\"none\",\"ts\":\"1000\"" +
        (production ? ",\"productionTransaction\":" + pendingJsonString(expected.productionTransaction) +
            ",\"productionCandidateSha256\":" + pendingJsonString(expected.productionCandidateSha256) : "") + "}\n";
    const auto healthPath = joinPath(staging, "health_" + job.jobId + ".txt");
    replacePendingJournal(pendingJobPath(staging, job.jobId), encodePendingJob(job));
    replacePendingJournal(healthPath, health);
    replacePendingJournal(joinPath(staging, "ota_final_" + job.jobId + ".txt"),
                          "jobId=" + job.jobId + "\ntoken=" + job.token + "\nstatus=completed\n");
    std::string proofPath;
    std::string proofBytes;
    if (production) {
        const auto proof = "{\"format\":\"ota-handoff-v1\",\"jobId\":" + pendingJsonString(job.jobId) +
            ",\"version\":" + pendingJsonString(job.version) +
            ",\"token\":" + pendingJsonString(job.token) +
            ",\"phase\":\"OWNER_FINISHED\",\"runtimeMayChange\":true,\"productionTransaction\":" +
            pendingJsonString(expected.productionTransaction) +
            ",\"completionProof\":{\"status\":\"healthy\",\"rollback\":\"none\",\"transaction\":" +
            pendingJsonString(expected.productionTransaction) +
            ",\"healthSha256\":" + pendingJsonString(expected.appliedSha256) +
            ",\"healthRecord\":" + pendingJsonString(health) + "}}\n";
        proofPath = joinPath(staging, "ota_handoff_" + job.jobId + ".json");
        proofBytes = proof;
        replacePendingJournal(proofPath, proofBytes);
    }
    OtaConfig config;
    config.stagingDir = staging;
    const auto loaded = OtaService(config).loadFinalStatuses();
    requireM3(loaded.size() == 1 && loaded[0].stage == "completed", "restarted service lost final");
    requireM3(loaded[0].jobId == job.jobId && loaded[0].occurrenceId == job.token,
              "final changed original job or occurrence");
    if (production) {
        requireM3(loaded[0].productionTransaction == expected.productionTransaction &&
                  loaded[0].productionCandidateSha256 == expected.productionCandidateSha256 &&
                  loaded[0].productionHealth == "healthy" &&
                  loaded[0].appliedSha256 == expected.appliedSha256,
                  "final lost production proof");
        checkSerializers(loaded[0], true);
        const auto key = health.find(",\"productionTransaction\":");
        requireM3(key != std::string::npos, "health fixture missing transaction");
        const auto invalid = health.substr(0, key) + "}\n";
        replacePendingJournal(healthPath, invalid);
        requireM3(OtaService(config).loadFinalStatuses().empty(),
                  "missing production health binding became terminal");
        replacePendingJournal(healthPath, health);
        auto wrong = health;
        const auto sha = wrong.find(expected.productionCandidateSha256);
        wrong.replace(sha, expected.productionCandidateSha256.size(), std::string(64, 'z'));
        rejectsM3([&] { (void)decodeHealthResult(wrong, job); },
                  "health parser accepted invalid candidate digest");
        replacePendingJournal(healthPath, wrong);
        requireM3(OtaService(config).loadFinalStatuses().empty(),
                  "invalid candidate SHA became terminal");
        replacePendingJournal(healthPath, health);
        requireM3(OtaService(config).loadFinalStatuses().size() == 1,
                  "valid original-job proof did not recover after invalid evidence");
        auto wrongTransaction = health;
        const auto transactionAt = wrongTransaction.find(expected.productionTransaction);
        requireM3(transactionAt != std::string::npos, "health fixture missing transaction");
        wrongTransaction.replace(transactionAt, expected.productionTransaction.size(),
                                 "OTA_" + std::string(32, 'd'));
        requireM3(json::JsonParser(wrongTransaction, 4, 32).parse().isObject(),
                  "wrong transaction negative must be valid JSON");
        auto matchedProof = proofBytes;
        const auto healthAt = matchedProof.find(pendingJsonString(health));
        requireM3(healthAt != std::string::npos, "proof fixture missing health bytes");
        matchedProof.replace(healthAt, pendingJsonString(health).size(), pendingJsonString(wrongTransaction));
        replacePendingJournal(proofPath, matchedProof);
        replacePendingJournal(healthPath, wrongTransaction);
        requireM3(OtaService(config).loadFinalStatuses().empty(),
                  "health transaction different from original-job proof became terminal");
        replacePendingJournal(proofPath, proofBytes);
        replacePendingJournal(healthPath, health);
        for (const auto& replacement : {std::pair<std::string, std::string>{job.jobId, "OTHER_JOB"},
                                        {job.token, "ffffffff-ffff-ffff-ffff-ffffffffffff"}}) {
            auto wrongIdentity = health;
            const auto offset = wrongIdentity.find(replacement.first);
            requireM3(offset != std::string::npos, "health fixture missing identity");
            wrongIdentity.replace(offset, replacement.first.size(), replacement.second);
            rejectsM3([&] { (void)decodeHealthResult(wrongIdentity, job); },
                      "health parser accepted wrong original job or occurrence");
            replacePendingJournal(healthPath, wrongIdentity);
            requireM3(OtaService(config).loadFinalStatuses().empty(),
                      "wrong original-job health became terminal");
        }
        replacePendingJournal(healthPath, "");
        requireM3(OtaService(config).loadFinalStatuses().empty(), "empty health became terminal");
        replacePendingJournal(healthPath, std::string(4097, 'x'));
        requireM3(OtaService(config).loadFinalStatuses().empty(), "oversize health became terminal");
        std::filesystem::remove(healthPath);
        requireM3(OtaService(config).loadFinalStatuses().empty(), "missing health became terminal");
        replacePendingJournal(healthPath, health);
    } else {
        requireM3(loaded[0].productionTransaction.empty(), "legacy final invented production proof");
        checkSerializers(loaded[0], false);
    }
}

OtaStatus checkReconcile(const std::string& staging, const OtaStatus& expected,
                         bool finalBeforeStatus = false, bool wrongLease = false,
                         bool failPendingAppend = false, bool failFinalSync = false) {
    PendingOtaJob job;
    job.jobId = expected.jobId;
    job.machineCode = expected.machineCode;
    job.version = expected.appliedVersion;
    job.artifactPath = staging + "/synthetic-artifact.tar.gz";
    job.previousVersion = "old";
    job.token = expected.occurrenceId;
    job.lifecycle = "systemd-handoff-v1";
    job.packageType = "config";
    job.artifactSha256 = expected.appliedSha256;
    job.artifactSize = 1;
    const auto health = "{\"format\":\"ota-health-v1\",\"jobId\":" + pendingJsonString(job.jobId) +
        ",\"version\":" + pendingJsonString(job.version) +
        ",\"token\":" + pendingJsonString(job.token) +
        ",\"status\":\"healthy\",\"rollback\":\"none\",\"ts\":" +
        pendingJsonString(std::to_string(expected.ts)) +
        ",\"productionTransaction\":" + pendingJsonString(expected.productionTransaction) +
        ",\"productionCandidateSha256\":" + pendingJsonString(expected.productionCandidateSha256) + "}\n";
    const auto proof = "{\"format\":\"ota-handoff-v1\",\"jobId\":" + pendingJsonString(job.jobId) +
        ",\"version\":" + pendingJsonString(job.version) +
        ",\"token\":" + pendingJsonString(job.token) +
        ",\"phase\":\"OWNER_FINISHED\",\"runtimeMayChange\":true,\"productionTransaction\":" +
        pendingJsonString(expected.productionTransaction) +
        ",\"completionProof\":{\"status\":\"healthy\",\"rollback\":\"none\",\"transaction\":" +
        pendingJsonString(expected.productionTransaction) +
        ",\"healthSha256\":" + pendingJsonString(expected.appliedSha256) +
        ",\"healthRecord\":" + pendingJsonString(health) + "}}\n";
    replacePendingJournal(joinPath(staging, "health_" + job.jobId + ".txt"), health);
    replacePendingJournal(joinPath(staging, "ota_active_job.txt"),
        "jobId=" + job.jobId + "\ntoken=" + job.token + "\n");
    const auto lockPath = joinPath(staging, "ota_job_" + job.jobId + ".lock");
    const int lockFd = open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    requireM3(lockFd >= 0, "cannot create private job lease");
    struct stat lockInfo{};
    requireM3(fstat(lockFd, &lockInfo) == 0, "cannot pin private job lease");
    job.leaseDev = lockInfo.st_dev;
    job.leaseInode = lockInfo.st_ino;
    close(lockFd);
    replacePendingJournal(pendingJobPath(staging, job.jobId), encodePendingJob(job));
    if (wrongLease) {
        std::filesystem::rename(lockPath, lockPath + ".old");
        const int replacement = open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        requireM3(replacement >= 0, "cannot replace private job lease");
        struct stat replacementInfo{};
        requireM3(fstat(replacement, &replacementInfo) == 0 &&
                  replacementInfo.st_ino != lockInfo.st_ino,
                  "private lease replacement did not change inode");
        close(replacement);
    }
    OtaConfig config;
    config.stagingDir = staging;
    config.backupDir = staging + "/backup";
    OtaService(config).reconcilePendingJobs();
    requireM3(OtaService(config).loadPendingStatuses().empty(),
              "reconcile accepted missing owner proof");
    requireM3(!std::filesystem::exists(joinPath(staging, "ota_final_" + job.jobId + ".txt")),
              "reconcile wrote final without owner proof");

    replacePendingJournal(joinPath(staging, "ota_handoff_" + job.jobId + ".json"), proof);
    if (finalBeforeStatus) {
        persistFinalizationAudit(staging, job, false, "final-marker", "verified-health",
            "ota-terminal-outbox-v1");
        replacePendingJournal(joinPath(staging, "ota_final_" + job.jobId + ".txt"),
            "jobId=" + job.jobId + "\ntoken=" + job.token + "\nstatus=completed\n");
        OtaConfig nextConfig = config;
        nextConfig.downloadDir = staging + "/next-download";
        nextConfig.checksumRequired = false;
        nextConfig.requireSignature = false;
        OtaRequest next;
        next.jobId = "M3_LATER_JOB";
        next.version = "later";
        next.artifactUrl = "https://example.invalid/private-fixture";
        std::string reason;
        requireM3(!OtaService(nextConfig).validateRequest(next, &reason) &&
                  reason == "previous successful OTA requires finalization reconciliation",
                  "visible but unconfirmed final marker admitted a later job");
    }
    const auto pendingLockPath = joinPath(staging, "ota_status_pending.log.lock");
    if (failPendingAppend) std::filesystem::create_directory(pendingLockPath);
    if (failFinalSync) armFinalSyncFault.store(true);
    try {
        OtaService(config).reconcilePendingJobs();
        requireM3(!failPendingAppend && !failFinalSync, "finalization fault was not reached");
    } catch (const SuccessfulFinalizationError&) {
        requireM3(failPendingAppend || failFinalSync, "unexpected finalization error");
    }
    if (wrongLease) {
        requireM3(!std::filesystem::exists(joinPath(staging, "ota_final_" + job.jobId + ".txt")) &&
                  OtaService(config).loadPendingStatuses().empty(),
                  "reconcile accepted a replaced lifecycle lease");
        return {};
    }
    if (failFinalSync) {
        requireM3(!armFinalSyncFault.load() && !failNextDirectorySync.load() &&
                  hasValidFinal(staging, job) && hasUnresolvedExecutionAudit(staging, job),
                  "directory fsync fault did not preserve uncertain execution audit");
        OtaConfig nextConfig = config;
        nextConfig.downloadDir = staging + "/next-download";
        nextConfig.checksumRequired = false;
        nextConfig.requireSignature = false;
        OtaRequest next;
        next.jobId = "M3_LATER_JOB";
        next.version = "later";
        next.artifactUrl = "https://example.invalid/private-fixture";
        std::string reason;
        requireM3(!OtaService(nextConfig).validateRequest(next, &reason) &&
                  reason == "previous successful OTA requires finalization reconciliation",
                  "failed final-marker fsync admitted a later job");
        OtaService(config).reconcilePendingJobs();
    }
    if (failPendingAppend) {
        requireM3(hasValidFinal(staging, job) &&
                  readPendingJournal(joinPath(staging, "ota_status_pending.log")).empty(),
                  "pending append fault did not leave final-before-status window");
        const auto audit = json::JsonParser(readPendingJournal(
            joinPath(staging, "ota_reconcile_" + job.jobId + ".json")), 4, 32).parse();
        requireM3(pendingField(audit, "reason") == "ota-terminal-outbox-v1" &&
                  pendingField(audit, "finalState") == "delivery-pending",
                  "append failure lost durable delivery intent");
        OtaConfig nextConfig = config;
        nextConfig.downloadDir = staging + "/next-download";
        nextConfig.checksumRequired = false;
        nextConfig.requireSignature = false;
        OtaRequest next;
        next.jobId = "M3_LATER_JOB";
        next.version = "later";
        next.artifactUrl = "https://example.invalid/private-fixture";
        std::string admissionError;
        requireM3(OtaService(nextConfig).validateRequest(next, &admissionError),
                  "verified A final blocked later B admission");
        replacePendingJournal(joinPath(staging, "ota_active_job.txt"),
            "jobId=M3_LATER_JOB\ntoken=bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb\n");
        replacePendingJournal(joinPath(staging, "applied_version.txt"), "later\n");
        const auto beforeHistory = readPendingJournal(joinPath(staging, "upgrade_history.log"));
        std::filesystem::remove(pendingLockPath);
        OtaService(config).reconcilePendingJobs();
        requireM3(readPendingJournal(joinPath(staging, "applied_version.txt")) == "later\n" &&
                  readPendingJournal(joinPath(staging, "upgrade_history.log")) == beforeHistory,
                  "historical A delivery changed B runtime version or history");
    }
    const auto final = readPendingJournal(joinPath(staging, "ota_final_" + job.jobId + ".txt"));
    requireM3(final == "jobId=" + job.jobId + "\ntoken=" + job.token + "\nstatus=completed\n",
              "reconcile did not persist the original-job final marker");
    const auto history = readPendingJournal(joinPath(staging, "upgrade_history.log"));
    requireM3(history.find(",result=success,jobId=" + job.jobId + ",") != std::string::npos,
              "reconcile lost original-job history");
    const auto pending = OtaService(config).loadPendingStatuses();
    requireM3(pending.size() == 1 && pending[0].jobId == job.jobId &&
              pending[0].occurrenceId == job.token &&
              pending[0].productionTransaction == expected.productionTransaction &&
              pending[0].productionCandidateSha256 == expected.productionCandidateSha256 &&
              pending[0].productionHealth == "healthy", "reconcile did not append bound pending terminal");
    requireM3(OtaService(config).loadFinalStatuses().size() == 1,
              "verified final was hidden by local delivery backlog");
    const auto healthPath = joinPath(staging, "health_" + job.jobId + ".txt");
    replacePendingJournal(healthPath, "damaged\n");
    requireM3(OtaService(config).loadFinalStatuses().empty(),
              "damaged health evidence became a verified final");
    replacePendingJournal(healthPath, health);
    checkSerializers(pending[0], true);
    const auto pendingBytes = readPendingJournal(joinPath(staging, "ota_status_pending.log"));
    OtaService(config).reconcilePendingJobs();
    requireM3(readPendingJournal(joinPath(staging, "ota_status_pending.log")) == pendingBytes &&
              readPendingJournal(joinPath(staging, "upgrade_history.log")) == history,
              "repeated reconciliation changed completed journals");
    OtaService subscriber(config);
    requireM3(subscriber.loadPendingStatuses().size() == 1, "pending terminal was not available for publication");
    std::atomic<bool> start{false};
    std::exception_ptr clearError;
    std::exception_ptr replayError;
    std::thread clearer([&] {
        while (!start.load()) std::this_thread::yield();
        try { subscriber.clearPendingStatuses(); }
        catch (...) { clearError = std::current_exception(); }
    });
    std::thread replayer([&] {
        while (!start.load()) std::this_thread::yield();
        try { subscriber.reconcilePendingJobs(); }
        catch (...) { replayError = std::current_exception(); }
    });
    start.store(true);
    clearer.join();
    replayer.join();
    if (clearError) std::rethrow_exception(clearError);
    if (replayError) std::rethrow_exception(replayError);
    requireM3(readPendingJournal(joinPath(staging, "ota_status_pending.log")).empty(),
              "published terminal was not acknowledged");
    OtaService(config).reconcilePendingJobs();
    requireM3(readPendingJournal(joinPath(staging, "ota_status_pending.log")).empty() &&
              OtaService(config).loadFinalStatuses().size() == 1,
              "acknowledged terminal was reappended after recovery");
    return pending[0];
}

void checkFailedReconcile(const std::string& staging, const OtaStatus& expected) {
    PendingOtaJob job;
    job.jobId = "M3_SAFE_FAILURE";
    job.machineCode = expected.machineCode;
    job.version = expected.appliedVersion;
    job.artifactPath = staging + "/synthetic-artifact.tar.gz";
    job.previousVersion = "old";
    job.token = expected.occurrenceId;
    job.lifecycle = "systemd-handoff-v1";
    job.packageType = "config";
    job.artifactSha256 = expected.appliedSha256;
    job.artifactSize = 1;
    const auto health = "{\"format\":\"ota-health-v1\",\"jobId\":" + pendingJsonString(job.jobId) +
        ",\"version\":" + pendingJsonString(job.version) +
        ",\"token\":" + pendingJsonString(job.token) +
        ",\"status\":\"failed\",\"rollback\":\"unchanged\",\"ts\":\"1000\"}\n";
    const auto proof = "{\"format\":\"ota-handoff-v1\",\"jobId\":" + pendingJsonString(job.jobId) +
        ",\"version\":" + pendingJsonString(job.version) +
        ",\"token\":" + pendingJsonString(job.token) +
        ",\"phase\":\"CANCELLED\",\"runtimeMayChange\":false,\"cancelProof\":{\"noWriter\":true," +
        "\"unit\":" + pendingJsonString("ota-runtime-" + job.jobId + "-" + job.token + ".service") +
        ",\"mainPid\":\"0\",\"job\":\"\"}}\n";
    replacePendingJournal(pendingJobPath(staging, job.jobId), encodePendingJob(job));
    replacePendingJournal(joinPath(staging, "health_" + job.jobId + ".txt"), health);
    replacePendingJournal(joinPath(staging, "ota_handoff_" + job.jobId + ".json"), proof);
    replacePendingJournal(joinPath(staging, "ota_active_job.txt"),
        "jobId=" + job.jobId + "\ntoken=" + job.token + "\n");
    const auto lockPath = joinPath(staging, "ota_job_" + job.jobId + ".lock");
    const int lockFd = open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    requireM3(lockFd >= 0, "cannot create private failure lease");
    struct stat lockInfo{};
    requireM3(fstat(lockFd, &lockInfo) == 0, "cannot pin private failure lease");
    job.leaseDev = lockInfo.st_dev;
    job.leaseInode = lockInfo.st_ino;
    close(lockFd);
    replacePendingJournal(pendingJobPath(staging, job.jobId), encodePendingJob(job));
    persistFinalizationAudit(staging, job, false, "final-marker", "verified-health",
        "ota-terminal-outbox-v1");
    replacePendingJournal(joinPath(staging, "ota_final_" + job.jobId + ".txt"),
        "jobId=" + job.jobId + "\ntoken=" + job.token + "\nstatus=failed\n");
    OtaConfig config;
    config.stagingDir = staging;
    config.backupDir = staging + "/backup";
    const auto pendingLockPath = joinPath(staging, "ota_status_pending.log.lock");
    std::filesystem::create_directory(pendingLockPath);
    rejectsM3([&] { OtaService(config).reconcilePendingJobs(); },
              "safe failure pending append fault was not reached");
    requireM3(hasValidFinal(staging, job) &&
              needsFinalizationReconciliation(staging, job),
              "safe failure append fault lost its replay intent");
    std::filesystem::remove(pendingLockPath);
    OtaService(config).reconcilePendingJobs();
    const auto pending = OtaService(config).loadPendingStatuses();
    requireM3(pending.size() == 1 && pending[0].stage == "failed" &&
              pending[0].jobId == job.jobId && pending[0].occurrenceId == job.token,
              "safe failure final did not recover pending status");
    OtaService subscriber(config);
    requireM3(subscriber.loadPendingStatuses().size() == 1, "safe failure status unavailable");
    subscriber.clearPendingStatuses();
    OtaService(config).reconcilePendingJobs();
    requireM3(OtaService(config).loadPendingStatuses().empty(),
              "acknowledged safe failure was reappended");
}

void checkSynchronousTailReplay(const std::string& staging, const OtaStatus& expected) {
    PendingOtaJob job;
    job.jobId = "M3_SYNC_TAIL";
    job.machineCode = expected.machineCode;
    job.version = expected.appliedVersion;
    job.artifactPath = staging + "/synthetic-artifact.tar.gz";
    job.previousVersion = "old";
    job.token = expected.occurrenceId;
    job.lifecycle = "inherited-flock-v1";
    replacePendingJournal(pendingJobPath(staging, job.jobId), encodePendingJob(job));
    replacePendingJournal(joinPath(staging, "ota_active_job.txt"),
        "jobId=" + job.jobId + "\ntoken=" + job.token + "\n");
    const auto lockPath = joinPath(staging, "ota_job_" + job.jobId + ".lock");
    const int lockFd = open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    requireM3(lockFd >= 0, "cannot create private sync-tail lease");
    close(lockFd);
    replacePendingJournal(joinPath(staging, "ota_reconcile_" + job.jobId + ".json"),
        "{\"format\":\"ota-reconcile-v1\",\"reason\":\"successful-apply-finalization-error\","
        "\"jobId\":" + pendingJsonString(job.jobId) +
        ",\"token\":" + pendingJsonString(job.token) +
        ",\"version\":" + pendingJsonString(job.version) +
        ",\"finalState\":\"unknown\",\"phase\":\"version\","
        "\"evidence\":\"apply-returned-success\",\"ts\":\"1000\"}\n");
    OtaConfig config;
    config.stagingDir = staging;
    config.backupDir = staging + "/backup";
    OtaService(config).reconcilePendingJobs();
    const auto first = readPendingJournal(joinPath(staging, "ota_status_pending.log"));
    const auto pending = OtaService(config).loadPendingStatuses();
    requireM3(pending.size() == 1 && pending[0].ts == 1000 && pending[0].jobId == job.jobId,
              "sync-tail did not reuse durable audit timestamp");
    OtaService(config).reconcilePendingJobs();
    requireM3(readPendingJournal(joinPath(staging, "ota_status_pending.log")) == first,
              "sync-tail replay generated a distinct terminal record");
}

void checkLegacyExecute(const std::string& root) {
    const auto staging = root + "/staging";
    const auto downloads = root + "/downloads";
    const auto backup = root + "/backup";
    std::filesystem::create_directory(staging);
    std::filesystem::create_directory(downloads);
    std::filesystem::create_directory(backup);
    const auto source = root + "/source.tar.gz";
    const auto script = root + "/apply-ok.sh";
    {
        std::ofstream artifact(source, std::ios::binary | std::ios::trunc);
        artifact << "private-test-payload";
        std::ofstream apply(script, std::ios::binary | std::ios::trunc);
        apply << "#!/bin/sh\n"
              << "printf '{\"format\":\"ota-health-v1\",\"jobId\":\"%s\",\"version\":\"%s\","
              << "\"token\":\"%s\",\"status\":\"healthy\",\"rollback\":\"none\","
              << "\"ts\":\"1000\"}\\n' \"$3\" \"$2\" \"$6\" > \"$5/health_$3.txt\"\n"
              << "exit 0\n";
    }
    std::filesystem::permissions(script, std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::add);
    OtaConfig config;
    config.enabled = true;
    config.downloadDir = downloads;
    config.stagingDir = staging;
    config.backupDir = backup;
    config.applyScript = script;
    config.rollbackScript.clear();
    config.checksumRequired = false;
    config.requireSignature = false;
    config.currentVersion = "legacy-1";
    config.packageType = "tar.gz";
    config.minFreeBytes = 0;
    OtaRequest request;
    request.jobId = "M3_LEGACY_EXECUTE";
    request.version = "legacy-2";
    request.artifactUrl = source;
    OtaReply reply;
    OtaStatus status;
    OtaService(config).execute(request, "LOCAL_ONLY", currentTimeMs(), &reply, &status, nullptr);
    const auto final = OtaService(config).loadFinalStatuses();
    const auto pending = OtaService(config).loadPendingStatuses();
    requireM3(reply.accepted && status.stage == "completed" &&
              status.jobId == request.jobId && !status.occurrenceId.empty(),
              "normal legacy execute misread delivery-only intent as execution failure");
    requireM3(final.size() == 1 && final[0].stage == "completed" &&
              std::any_of(pending.begin(), pending.end(), [&](const OtaStatus& event) {
                  return event.jobId == request.jobId && event.occurrenceId == status.occurrenceId &&
                      event.stage == "completed";
              }),
              "normal legacy execute did not preserve final and pending status");
    const auto job = decodePendingJob(readPendingJournal(pendingJobPath(staging, request.jobId)));
    const auto audit = json::JsonParser(readPendingJournal(
        joinPath(staging, "ota_reconcile_" + request.jobId + ".json")), 4, 32).parse();
    requireM3(readPendingJournal(joinPath(staging, "health_" + request.jobId + ".txt")).find(
                  "\"token\":\"" + job.token + "\"") != std::string::npos &&
              pendingField(audit, "reason") == "ota-terminal-outbox-v1" &&
              pendingField(audit, "finalState") == "delivery-pending" &&
              hasValidFinal(staging, job) && !hasUnresolvedExecutionAudit(staging, job),
              "legacy execute did not exercise healthy delivery-only finalization");
}

void checkSynchronousExecute(const std::string& root, bool failAppend, bool throwCallback = false,
                             bool boundedProgress = false) {
    const auto staging = root + "/staging";
    const auto downloads = root + "/downloads";
    const auto backup = root + "/backup";
    std::filesystem::create_directory(staging);
    std::filesystem::create_directory(downloads);
    std::filesystem::create_directory(backup);
    const auto source = root + "/source.tar.gz";
    const auto script = root + "/apply-ok.sh";
    {
        std::ofstream artifact(source, std::ios::binary | std::ios::trunc);
        artifact << "private-sync-payload";
        std::ofstream apply(script, std::ios::binary | std::ios::trunc);
        apply << "#!/bin/sh\nexit 0\n";
    }
    OtaConfig config;
    config.enabled = true;
    config.downloadDir = downloads;
    config.stagingDir = staging;
    config.backupDir = backup;
    config.applyScript = script;
    config.rollbackScript.clear();
    config.checksumRequired = false;
    config.requireSignature = false;
    config.currentVersion = "legacy-1";
    config.packageType = "tar.gz";
    config.minFreeBytes = 0;
    if (boundedProgress) {
        OtaStatus protectedStatus;
        protectedStatus.jobId = "M3_PROTECTED_BACKLOG";
        protectedStatus.machineCode = "LOCAL_ONLY";
        protectedStatus.stage = "failed";
        protectedStatus.ts = 1000;
        protectedStatus.occurrenceId = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
        OtaStatus oldProgress = protectedStatus;
        oldProgress.jobId = "M3_OLD_PROGRESS";
        oldProgress.stage = "downloading";
        oldProgress.occurrenceId = "bbbbbbbb-cccc-dddd-eeee-ffffffffffff";
        const auto protectedRecord = encodePendingStatus(protectedStatus);
        config.maxPendingStatusBytes = protectedRecord.size() + 650;
        replacePendingJournal(joinPath(staging, "ota_status_pending.log"),
            protectedRecord + encodePendingStatus(oldProgress));
    }
    OtaRequest request;
    request.jobId = failAppend ? "M3_SYNC_FAULT" : throwCallback ? "M3_SYNC_CALLBACK" :
        boundedProgress ? "M3_SYNC_BOUNDED" : "M3_SYNC_OK";
    request.version = "legacy-2";
    request.artifactUrl = source;
    OtaReply reply;
    OtaStatus status;
    if (failAppend) armSyncPendingFault.store(true);
    try {
        OtaService(config).execute(request, "LOCAL_ONLY", currentTimeMs(), &reply, &status,
            throwCallback ? std::function<void(const OtaStatus&)>([](const OtaStatus& event) {
                if (event.stage == "completed") throw std::runtime_error("private transport failure");
            }) : nullptr);
        requireM3(!failAppend, "synchronous pending append fault was not reached");
    } catch (const SuccessfulFinalizationError&) {
        requireM3(failAppend, "unexpected synchronous finalization error");
    }
    requireM3(!armSyncPendingFault.load() && !syncPendingFaultSetupFailed.load(),
              "private synchronous append fault setup failed");
    const auto job = decodePendingJob(readPendingJournal(pendingJobPath(staging, request.jobId)));
    const auto auditPath = joinPath(staging, "ota_reconcile_" + request.jobId + ".json");
    const auto audit = json::JsonParser(readPendingJournal(auditPath), 4, 32).parse();
    const auto ts = pendingInteger<std::int64_t>(pendingField(audit, "ts"));
    requireM3(hasValidFinal(staging, job) && !hasUnresolvedExecutionAudit(staging, job) &&
              pendingField(audit, "reason") == "ota-terminal-outbox-v1" &&
              pendingField(audit, "finalState") == "delivery-pending" &&
              pendingField(audit, "evidence") == "apply-returned-success" &&
              OtaService(config).loadFinalStatuses().size() == 1,
              "synchronous final was not durable before local delivery");
    if (failAppend) {
        const auto lock = joinPath(staging, "ota_status_pending.log.lock");
        std::filesystem::remove(lock);
        std::filesystem::rename(lock + ".saved", lock);
        OtaService(config).reconcilePendingJobs();
    }
    const auto before = readPendingJournal(joinPath(staging, "ota_status_pending.log"));
    if (boundedProgress) {
        requireM3(before.find("M3_PROTECTED_BACKLOG") != std::string::npos &&
                  before.find("M3_OLD_PROGRESS") == std::string::npos,
                  "real execute did not reclaim progress behind the protected terminal");
    }
    OtaService(config).reconcilePendingJobs();
    requireM3(readPendingJournal(joinPath(staging, "ota_status_pending.log")) == before,
              "synchronous terminal replay produced different bytes");
    const auto pending = OtaService(config).loadPendingStatuses();
    std::size_t completed = 0;
    for (const auto& event : pending) {
        if (event.stage == "completed") {
            ++completed;
            requireM3(event.jobId == request.jobId && event.occurrenceId == job.token && event.ts == ts,
                      "synchronous terminal lost stable identity or time");
        }
    }
    requireM3(completed == 1, "synchronous terminal missing or duplicated");
    OtaRequest next;
    next.jobId = "M3_SYNC_LATER";
    next.version = "later";
    next.artifactUrl = source;
    std::string admissionError;
    requireM3(OtaService(config).validateRequest(next, &admissionError),
              "offline local delivery blocked a later safe OTA admission");
    OtaService subscriber(config);
    requireM3(!subscriber.loadPendingStatuses().empty(), "synchronous pending snapshot missing");
    subscriber.clearPendingStatuses();
    const auto cleared = json::JsonParser(readPendingJournal(auditPath), 4, 32).parse();
    requireM3(pendingField(cleared, "finalState") == "committed" &&
              pendingField(cleared, "evidence") == "apply-returned-success" &&
              pendingInteger<std::int64_t>(pendingField(cleared, "ts")) == ts,
              "local delivery clear erased original synchronous apply provenance");
}

void checkMixedTerminalClear(const std::string& staging) {
    OtaStatus bad;
    bad.jobId = "M3_BAD_CLEAR";
    bad.machineCode = "LOCAL_ONLY";
    bad.stage = "completed";
    bad.progress = 100;
    bad.ts = 1000;
    bad.occurrenceId = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
    OtaStatus good = bad;
    good.jobId = "M3_GOOD_CLEAR";
    good.occurrenceId = "bbbbbbbb-cccc-dddd-eeee-ffffffffffff";
    OtaStatus laterTerminal = bad;
    laterTerminal.jobId = "M3_LATER_CLEAR";
    laterTerminal.occurrenceId = "cccccccc-dddd-eeee-ffff-aaaaaaaaaaaa";
    const auto writeJob = [&](const OtaStatus& status) {
        PendingOtaJob job;
        job.jobId = status.jobId;
        job.machineCode = status.machineCode;
        job.version = "legacy-2";
        job.artifactPath = staging + "/synthetic.tar.gz";
        job.previousVersion = "legacy-1";
        job.token = status.occurrenceId;
        job.lifecycle = "inherited-flock-v1";
        replacePendingJournal(pendingJobPath(staging, job.jobId), encodePendingJob(job));
        replacePendingJournal(joinPath(staging, "ota_final_" + job.jobId + ".txt"),
            "jobId=" + job.jobId + "\ntoken=" + job.token + "\nstatus=completed\n");
        const auto health = "{\"format\":\"ota-health-v1\",\"jobId\":" +
            pendingJsonString(job.jobId) + ",\"version\":" + pendingJsonString(job.version) +
            ",\"token\":" + pendingJsonString(job.token) +
            ",\"status\":\"healthy\",\"rollback\":\"none\",\"ts\":\"1000\"}\n";
        replacePendingJournal(joinPath(staging, "health_" + job.jobId + ".txt"), health);
        persistFinalizationAudit(staging, job, false, "terminal-journal", "verified-health",
            "ota-terminal-outbox-v1", true, 1000);
        return health;
    };
    const auto badHealth = writeJob(bad);
    writeJob(good);
    writeJob(laterTerminal);
    const auto badRecord = encodePendingStatus(bad);
    replacePendingJournal(joinPath(staging, "ota_status_pending.log"),
        badRecord + encodePendingStatus(good));
    replacePendingJournal(joinPath(staging, "health_" + bad.jobId + ".txt"), "damaged\n");
    OtaConfig config;
    config.stagingDir = staging;
    OtaConfig backlogConfig = config;
    backlogConfig.maxPendingStatusBytes = std::max({badRecord.size(),
        encodePendingStatus(good).size(), encodePendingStatus(laterTerminal).size() + 100});
    OtaService(backlogConfig).reconcilePendingJobs();
    requireM3(OtaService(backlogConfig).loadPendingStatuses().size() == 2,
              "full terminal journal blocked old replay or lost pending intent");
    OtaService subscriber(config);
    requireM3(subscriber.loadPendingStatuses().size() == 2, "mixed terminal snapshot missing");
    subscriber.clearPendingStatuses();
    requireM3(readPendingJournal(joinPath(staging, "ota_status_pending.log")) == badRecord &&
              readPendingJournal(joinPath(staging, "health_" + bad.jobId + ".txt")) == "damaged\n",
              "bad terminal was discarded or healthy terminal was retained");
    const auto goodAudit = json::JsonParser(readPendingJournal(
        joinPath(staging, "ota_reconcile_" + good.jobId + ".json")), 4, 32).parse();
    requireM3(pendingField(goodAudit, "finalState") == "committed" &&
              pendingField(goodAudit, "evidence") == "verified-health",
              "healthy terminal was not independently cleared");
    OtaService(backlogConfig).reconcilePendingJobs();
    OtaService laterSubscriber(backlogConfig);
    const auto laterSnapshot = laterSubscriber.loadPendingStatuses();
    requireM3(laterSnapshot.size() == 2 &&
              std::any_of(laterSnapshot.begin(), laterSnapshot.end(), [&](const OtaStatus& event) {
                  return event.jobId == laterTerminal.jobId && event.stage == "completed";
              }), "freed queue space did not replay later durable terminal");
    laterSubscriber.clearPendingStatuses();
    requireM3(readPendingJournal(joinPath(staging, "ota_status_pending.log")) == badRecord,
              "later healthy terminal did not clear independently of damaged evidence");
    OtaConfig limited = config;
    limited.maxPendingStatusBytes = badRecord.size();
    OtaService bounded(limited);
    OtaStatus later;
    later.jobId = "M3_LATER_PROGRESS";
    later.machineCode = "LOCAL_ONLY";
    later.stage = "accepted";
    later.ts = 1001;
    rejectsM3([&] { OtaServiceTestAccess::append(bounded, later); },
              "bounded journal evicted unresolved terminal");
    requireM3(readPendingJournal(joinPath(staging, "ota_status_pending.log")) == badRecord,
              "bounded append changed protected terminal bytes");
    replacePendingJournal(joinPath(staging, "health_" + bad.jobId + ".txt"), badHealth);
    OtaService repaired(config);
    requireM3(repaired.loadPendingStatuses().size() == 1, "repaired terminal snapshot missing");
    repaired.clearPendingStatuses();
    requireM3(readPendingJournal(joinPath(staging, "ota_status_pending.log")).empty(),
              "repaired terminal was not cleared");
}

void checkPreparedFailureSupersession(const std::string& root) {
    const auto staging = root + "/staging";
    const auto downloads = root + "/downloads";
    const auto backup = root + "/backup";
    std::filesystem::create_directory(staging);
    std::filesystem::create_directory(downloads);
    std::filesystem::create_directory(backup);
    const auto source = root + "/source.tar.gz";
    const auto script = root + "/apply-prepared-fail.sh";
    {
        std::ofstream artifact(source, std::ios::binary | std::ios::trunc);
        artifact << "private-prepared-payload";
        std::ofstream apply(script, std::ios::binary | std::ios::trunc);
        apply << "#!/bin/sh\n"
              << "printf '{\"format\":\"official-ota-apply-barrier-v1\",\"jobId\":\"%s\","
              << "\"version\":\"%s\",\"token\":\"%s\",\"state\":\"prepared\"}\\n' "
              << "\"$3\" \"$2\" \"$6\" > \"$5/ota_barrier_$3.json\"\n"
              << "exit 1\n";
    }
    std::filesystem::permissions(script, std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::add);
    OtaConfig config;
    config.enabled = true;
    config.downloadDir = downloads;
    config.stagingDir = staging;
    config.backupDir = backup;
    config.applyScript = script;
    config.checksumRequired = false;
    config.requireSignature = false;
    config.currentVersion = "legacy-1";
    config.packageType = "tar.gz";
    config.minFreeBytes = 0;
    OtaRequest request;
    request.jobId = "M3_W1_PREPARED";
    request.version = "legacy-2";
    request.artifactUrl = source;
    OtaReply reply;
    OtaStatus status;
    armPreparedFinalFault.store(true);
    rejectsM3([&] { OtaService(config).execute(request, "LOCAL_ONLY", currentTimeMs(),
        &reply, &status, nullptr); }, "prepared failure did not reach real execute failure");
    requireM3(!armPreparedFinalFault.load(), "prepared final-marker fault was not reached");
    const auto job = decodePendingJob(readPendingJournal(pendingJobPath(staging, request.jobId)));
    const auto health = decodeHealthResult(readPendingJournal(
        joinPath(staging, "health_" + request.jobId + ".txt")), job);
    requireM3(health.status == "failed" && health.rollback == "unchanged" &&
              !hasValidFinal(staging, job), "prepared failure lacked bound safe evidence before retry");
    const auto before = OtaService(config).loadPendingStatuses();
    requireM3(std::any_of(before.begin(), before.end(), [&](const OtaStatus& event) {
        return event.jobId == job.jobId && event.stage == "failed" && event.occurrenceId != job.token;
    }), "failed execute did not emit the mismatched occurrence");
    const auto old = *std::find_if(before.begin(), before.end(), [&](const OtaStatus& event) {
        return event.jobId == job.jobId && event.stage == "failed" && event.occurrenceId != job.token;
    });
    OtaService(config).reconcilePendingJobs();
    const auto final = OtaService(config).loadFinalStatuses();
    requireM3(final.size() == 1 && final[0].stage == "failed" &&
              final[0].occurrenceId == job.token, "prepared failure did not officially reconcile");
    OtaService subscriber(config);
    requireM3(subscriber.loadPendingStatuses().size() > before.size(),
              "bound terminal was not appended after recovery");
    const auto snapshot = readPendingJournal(joinPath(staging, "ota_status_pending.log"));
    const auto supersessionPath = statusSupersessionPath(staging, job);
    const auto supersession = readPendingJournal(supersessionPath);
    requireM3(hasValidatedStatusSupersession(staging, snapshot, old, job),
              "safe supersession linkage was not durable");
    replacePendingJournal(supersessionPath, "damaged\n");
    rejectsM3([&] { (void)hasValidatedStatusSupersession(staging, snapshot, old, job); },
              "unknown supersession parser was accepted");
    subscriber.clearPendingStatuses();
    const auto retained = readPendingJournal(joinPath(staging, "ota_status_pending.log"));
    requireM3(retained == encodePendingStatus(old),
              "damaged supersession did not retain only the unverified old terminal");
    replacePendingJournal(supersessionPath, supersession);
    requireM3(!hasValidatedStatusSupersession(staging, encodePendingStatus(old), old, job),
              "old failure was discarded without a bound replacement in the same snapshot");
    OtaStatus foreign = old;
    foreign.jobId = "M3_FOREIGN_JOB";
    const auto foreignRow = encodePendingStatus(foreign);
    requireM3(!hasValidatedStatusSupersession(staging, foreignRow, foreign, job, false),
              "foreign job occurrence was accepted as supersession source");
    replacePendingJournal(joinPath(staging, "ota_status_pending.log"), foreignRow);
    OtaService(config).reconcilePendingJobs();
    requireM3(readPendingJournal(joinPath(staging, "ota_status_pending.log")) == foreignRow,
              "foreign job occurrence caused a bound terminal replay");
    foreign.jobId = job.jobId;
    foreign.machineCode = "OTHER_MACHINE";
    requireM3(!hasValidatedStatusSupersession(staging, encodePendingStatus(foreign), foreign, job, false),
              "foreign device occurrence was accepted as supersession source");
    replacePendingJournal(joinPath(staging, "ota_status_pending.log"), retained);
    OtaService(config).reconcilePendingJobs();
    OtaService recovered(config);
    const auto replayed = recovered.loadPendingStatuses();
    requireM3(replayed.size() == 2 &&
              std::any_of(replayed.begin(), replayed.end(), [&](const OtaStatus& event) {
                  return event.jobId == job.jobId && event.occurrenceId == job.token && event.stage == "failed";
              }), "recovery did not restore the bound replacement after its earlier clear");
    OtaStatus late;
    late.jobId = "M3_LATE_PROGRESS";
    late.machineCode = "LOCAL_ONLY";
    late.stage = "downloading";
    late.ts = currentTimeMs();
    OtaService writer(config);
    OtaServiceTestAccess::append(writer, late);
    recovered.clearPendingStatuses();
    OtaService later(config);
    const auto remaining = later.loadPendingStatuses();
    requireM3(remaining.size() == 1 && remaining[0].jobId == late.jobId &&
              remaining[0].occurrenceId == late.occurrenceId,
              "supersession clear lost an append after the loaded snapshot");
    later.clearPendingStatuses();
    requireM3(OtaService(config).loadPendingStatuses().empty(),
              "safe later terminal did not supersede the earlier failed occurrence");
}

void checkHistoricalDeliveryFaultIsolation(const std::string& root) {
    const auto staging = root + "/staging";
    const auto downloads = root + "/downloads";
    const auto backup = root + "/backup";
    std::filesystem::create_directory(staging);
    std::filesystem::create_directory(downloads);
    std::filesystem::create_directory(backup);
    PendingOtaJob historical;
    historical.jobId = "M3_A_HISTORICAL";
    historical.machineCode = "LOCAL_ONLY";
    historical.version = "legacy-old";
    historical.artifactPath = root + "/historical.tar.gz";
    historical.previousVersion = "legacy-before";
    historical.token = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
    historical.lifecycle = "inherited-flock-v1";
    {
        std::ofstream lifecycle(joinPath(staging, "ota_job_" + historical.jobId + ".lock"),
                                std::ios::binary | std::ios::trunc);
        requireM3(lifecycle.is_open(), "cannot create private historical lifecycle lock");
    }
    replacePendingJournal(pendingJobPath(staging, historical.jobId), encodePendingJob(historical));
    replacePendingJournal(joinPath(staging, "health_" + historical.jobId + ".txt"),
        "{\"format\":\"ota-health-v1\",\"jobId\":\"M3_A_HISTORICAL\","
        "\"version\":\"legacy-old\",\"token\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\","
        "\"status\":\"failed\",\"rollback\":\"unchanged\",\"ts\":\"1000\"}\n");
    replacePendingJournal(joinPath(staging, "ota_final_" + historical.jobId + ".txt"),
        "jobId=" + historical.jobId + "\ntoken=" + historical.token + "\nstatus=failed\n");
    persistFinalizationAudit(staging, historical, false, "terminal-journal", "verified-health",
        "ota-terminal-outbox-v1", true, 1000);
    const auto source = root + "/source.tar.gz";
    const auto script = root + "/apply-lock-fault.sh";
    {
        std::ofstream artifact(source, std::ios::binary | std::ios::trunc);
        artifact << "private-historical-delivery";
        std::ofstream apply(script, std::ios::binary | std::ios::trunc);
        apply << "#!/bin/sh\n"
              << "mv \"$5/ota_status_pending.log.lock\" \"$5/ota_status_pending.log.lock.saved\"\n"
              << "mkdir \"$5/ota_status_pending.log.lock\"\n"
              << "exit 0\n";
    }
    std::filesystem::permissions(script, std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::add);
    OtaConfig config;
    config.enabled = true;
    config.downloadDir = downloads;
    config.stagingDir = staging;
    config.backupDir = backup;
    config.applyScript = script;
    config.checksumRequired = false;
    config.requireSignature = false;
    config.currentVersion = "legacy-1";
    config.packageType = "tar.gz";
    config.minFreeBytes = 0;
    OtaRequest request;
    request.jobId = "M3_Z_CURRENT";
    request.version = "legacy-2";
    request.artifactUrl = source;
    std::string admissionError;
    const bool admitted = OtaService(config).validateRequest(request, &admissionError);
    requireM3(admitted, admissionError.c_str());
    OtaReply reply;
    OtaStatus status;
    rejectsM3([&] { OtaService(config).execute(request, "LOCAL_ONLY", currentTimeMs(),
        &reply, &status, nullptr); }, "private historical delivery append fault was not reached");
    const auto current = decodePendingJob(readPendingJournal(pendingJobPath(staging, request.jobId)));
    const auto ownAudit = json::JsonParser(readPendingJournal(
        joinPath(staging, "ota_reconcile_" + request.jobId + ".json")), 4, 32).parse();
    requireM3(pendingField(ownAudit, "reason") == "ota-terminal-outbox-v1" &&
              pendingField(ownAudit, "phase") == "terminal-journal" &&
              pendingField(ownAudit, "finalState") == "delivery-pending" &&
              hasValidFinal(staging, current),
              "historical delivery I/O fault contaminated current successful finalization");
    const auto lock = joinPath(staging, "ota_status_pending.log.lock");
    std::filesystem::remove(lock);
    std::filesystem::rename(lock + ".saved", lock);
    OtaService(config).reconcilePendingJobs();
    const auto pending = OtaService(config).loadPendingStatuses();
    requireM3(std::any_of(pending.begin(), pending.end(), [&](const OtaStatus& event) {
                  return event.jobId == historical.jobId && event.stage == "failed";
              }) && std::any_of(pending.begin(), pending.end(), [&](const OtaStatus& event) {
                  return event.jobId == current.jobId && event.stage == "completed" &&
                      event.occurrenceId == current.token;
              }), "historical delivery and current final were not replayable after storage repair");
}

void checkLegacyTsv(const std::string& staging) {
    const std::string bytes = "LEGACY_TSV_JOB\tLOCAL_ONLY\tcompleted\t100\tlegacy result\t1000\n";
    replacePendingJournal(joinPath(staging, "ota_status_pending.log"), bytes);
    OtaConfig config;
    config.stagingDir = staging;
    const auto loaded = OtaService(config).loadPendingStatuses();
    requireM3(loaded.size() == 1 && loaded[0].jobId == "LEGACY_TSV_JOB" &&
              loaded[0].occurrenceId.empty() && loaded[0].productionTransaction.empty() &&
              loaded[0].message == "legacy result" && loaded[0].ts == 1000,
              "historical TSV pending status changed on replay");
    checkSerializers(loaded[0], false);
}

}  // namespace edge_gateway

int main(int argc, char** argv) {
    try {
        if (argc != 1 && (argc != 9 || std::string(argv[1]) != "--emit"))
            throw std::runtime_error("invalid private fixture arguments");
        if (geteuid() != 0) throw std::runtime_error("private production journal test requires root owner");
        char pattern[] = "/tmp/ota-m3-local-XXXXXX";
        const char* created = mkdtemp(pattern);
        edge_gateway::requireM3(created != nullptr, "cannot create private test root");
        const std::string root = created;
        edge_gateway::requireM3(root.rfind("/tmp/ota-m3-local-", 0) == 0,
                                "test root escaped private /tmp prefix");
        if (argc == 9) {
            edge_gateway::OtaStatus status;
            status.jobId = argv[2];
            status.machineCode = argv[3];
            status.appliedVersion = argv[4];
            status.appliedSha256 = argv[5];
            status.productionCandidateSha256 = argv[6];
            status.occurrenceId = argv[7];
            status.productionTransaction = argv[8];
            status.productionHealth = "healthy";
            status.ts = edge_gateway::currentTimeMs();
            edge_gateway::requireM3(edge_gateway::isSafeOtaId(status.jobId) &&
                    edge_gateway::isSafeOtaId(status.appliedVersion) &&
                    edge_gateway::isSafeSha256(status.appliedSha256) &&
                    status.appliedSha256.size() == 64 &&
                    edge_gateway::isSafeSha256(status.productionCandidateSha256) &&
                    status.productionCandidateSha256.size() == 64 &&
                    status.occurrenceId.size() == 36 &&
                    status.productionTransaction.size() == 36,
                    "invalid private fixture identity");
            const auto staging = root + "/reconciled";
            std::filesystem::create_directory(staging);
            const auto actual = edge_gateway::checkReconcile(staging, status);
            const auto builtin = edge_gateway::encodeBuiltinOtaStatusJson(actual);
            edge_gateway::checkEncoded(builtin, actual, true);
            std::filesystem::remove_all(root);
            std::cout << builtin << '\n';
            return 0;
        }
        const auto production = root + "/production";
        const auto legacy = root + "/legacy";
        const auto reconciled = root + "/reconciled";
        const auto oldTsv = root + "/old-tsv";
        const auto crashFinal = root + "/crash-final";
        const auto safeFailure = root + "/safe-failure";
        const auto syncTail = root + "/sync-tail";
        const auto wrongLease = root + "/wrong-lease";
        const auto appendFault = root + "/append-fault";
        const auto legacyExecute = root + "/legacy-execute";
        const auto finalSyncFault = root + "/final-sync-fault";
        const auto syncExecute = root + "/sync-execute";
        const auto syncAppendFault = root + "/sync-append-fault";
        const auto mixedClear = root + "/mixed-clear";
        const auto syncCallback = root + "/sync-callback";
        const auto preparedFailure = root + "/prepared-failure";
        const auto boundedExecute = root + "/bounded-execute";
        const auto historicalFault = root + "/historical-fault";
        std::filesystem::create_directory(production);
        std::filesystem::create_directory(legacy);
        std::filesystem::create_directory(reconciled);
        std::filesystem::create_directory(oldTsv);
        std::filesystem::create_directory(crashFinal);
        std::filesystem::create_directory(safeFailure);
        std::filesystem::create_directory(syncTail);
        std::filesystem::create_directory(wrongLease);
        std::filesystem::create_directory(appendFault);
        std::filesystem::create_directory(legacyExecute);
        std::filesystem::create_directory(finalSyncFault);
        std::filesystem::create_directory(syncExecute);
        std::filesystem::create_directory(syncAppendFault);
        std::filesystem::create_directory(mixedClear);
        std::filesystem::create_directory(syncCallback);
        std::filesystem::create_directory(preparedFailure);
        std::filesystem::create_directory(boundedExecute);
        std::filesystem::create_directory(historicalFault);
        edge_gateway::OtaStatus status;
        status.jobId = "M3_PRODUCTION_JOB";
        status.machineCode = "LOCAL_ONLY";
        status.stage = "completed";
        status.progress = 100;
        status.message = "escaped \"text\" \\ newline\n";
        status.ts = 1000;
        status.occurrenceId = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
        status.appliedVersion = "production-1";
        status.appliedSha256 = std::string(64, 'a');
        // SHA-256("M3_PRODUCTION_JOB" + occurrenceId)[:32], matching the owner contract.
        status.productionTransaction = "OTA_950ac198812538db28a25c3f1117f501";
        status.productionCandidateSha256 = std::string(64, 'c');
        status.productionHealth = "healthy";
        edge_gateway::checkSerializers(status, true);
        edge_gateway::checkPending(production, status);
        edge_gateway::checkFinal(production, status, true);
        edge_gateway::checkReconcile(reconciled, status);
        edge_gateway::checkReconcile(crashFinal, status, true);
        edge_gateway::checkFailedReconcile(safeFailure, status);
        edge_gateway::checkSynchronousTailReplay(syncTail, status);
        edge_gateway::checkReconcile(wrongLease, status, false, true);
        edge_gateway::checkReconcile(appendFault, status, false, false, true);
        edge_gateway::checkLegacyExecute(legacyExecute);
        edge_gateway::checkSynchronousExecute(syncExecute, false);
        edge_gateway::checkSynchronousExecute(syncAppendFault, true);
        edge_gateway::checkSynchronousExecute(syncCallback, false, true);
        edge_gateway::checkSynchronousExecute(boundedExecute, false, false, true);
        edge_gateway::checkMixedTerminalClear(mixedClear);
        edge_gateway::checkPreparedFailureSupersession(preparedFailure);
        edge_gateway::checkHistoricalDeliveryFaultIsolation(historicalFault);
        edge_gateway::checkReconcile(finalSyncFault, status, false, false, false, true);
        status.jobId = "M3_LEGACY_JOB";
        status.appliedVersion.clear();
        status.appliedSha256.clear();
        status.productionTransaction.clear();
        status.productionCandidateSha256.clear();
        status.productionHealth.clear();
        edge_gateway::checkSerializers(status, false);
        edge_gateway::checkPending(legacy, status);
        edge_gateway::checkFinal(legacy, status, false);
        edge_gateway::checkLegacyTsv(oldTsv);
        std::filesystem::remove_all(root);
        std::cout << "M3 pending/final, active-job reconciliation, historical TSV and both MQTT encoders PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
