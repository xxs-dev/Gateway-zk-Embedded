#include "../src/ota_service.cpp"
#include "edge_gateway/builtin_mqtt_driver_publisher.hpp"
#include "edge_gateway/mosquitto_mqtt_driver_publisher.hpp"

#include <filesystem>
#include <functional>

namespace edge_gateway {

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
    checkEncoded(encodeMosquittoOtaStatusJson(status), status, production);
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

void checkReconcile(const std::string& staging, const OtaStatus& expected) {
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
    job.leaseInode = 1;
    const auto health = "{\"format\":\"ota-health-v1\",\"jobId\":" + pendingJsonString(job.jobId) +
        ",\"version\":" + pendingJsonString(job.version) +
        ",\"token\":" + pendingJsonString(job.token) +
        ",\"status\":\"healthy\",\"rollback\":\"none\",\"ts\":\"1000\"" +
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
    replacePendingJournal(pendingJobPath(staging, job.jobId), encodePendingJob(job));
    replacePendingJournal(joinPath(staging, "health_" + job.jobId + ".txt"), health);
    replacePendingJournal(joinPath(staging, "ota_active_job.txt"),
        "jobId=" + job.jobId + "\ntoken=" + job.token + "\n");
    const auto lockPath = joinPath(staging, "ota_job_" + job.jobId + ".lock");
    const int lockFd = open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    requireM3(lockFd >= 0, "cannot create private job lease");
    close(lockFd);
    OtaConfig config;
    config.stagingDir = staging;
    config.backupDir = staging + "/backup";
    OtaService(config).reconcilePendingJobs();
    requireM3(OtaService(config).loadPendingStatuses().empty(),
              "reconcile accepted missing owner proof");
    requireM3(!std::filesystem::exists(joinPath(staging, "ota_final_" + job.jobId + ".txt")),
              "reconcile wrote final without owner proof");

    replacePendingJournal(joinPath(staging, "ota_handoff_" + job.jobId + ".json"), proof);
    OtaService(config).reconcilePendingJobs();
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
    checkSerializers(pending[0], true);
    const auto pendingBytes = readPendingJournal(joinPath(staging, "ota_status_pending.log"));
    OtaService(config).reconcilePendingJobs();
    requireM3(readPendingJournal(joinPath(staging, "ota_status_pending.log")) == pendingBytes &&
              readPendingJournal(joinPath(staging, "upgrade_history.log")) == history,
              "repeated reconciliation changed completed journals");
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

int main() {
    try {
        if (geteuid() != 0) throw std::runtime_error("private production journal test requires root owner");
        char pattern[] = "/tmp/ota-m3-local-XXXXXX";
        const char* created = mkdtemp(pattern);
        edge_gateway::requireM3(created != nullptr, "cannot create private test root");
        const std::string root = created;
        edge_gateway::requireM3(root.rfind("/tmp/ota-m3-local-", 0) == 0,
                                "test root escaped private /tmp prefix");
        const auto production = root + "/production";
        const auto legacy = root + "/legacy";
        const auto reconciled = root + "/reconciled";
        const auto oldTsv = root + "/old-tsv";
        std::filesystem::create_directory(production);
        std::filesystem::create_directory(legacy);
        std::filesystem::create_directory(reconciled);
        std::filesystem::create_directory(oldTsv);
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
