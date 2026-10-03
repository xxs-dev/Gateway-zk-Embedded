#define OTA_QUERY_FAULT_TEST
#include "../src/ota_service.cpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

void put(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
    require(out.good(), "private fixture write failed");
}

std::string get(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::map<std::string, std::string> tree(const std::string& path) {
    std::map<std::string, std::string> result;
    for (const auto& item : std::filesystem::directory_iterator(path)) {
        require(item.is_regular_file(), "query fixture contains unexpected object");
        result.emplace(item.path().filename().string(), get(item.path().string()));
    }
    return result;
}

void check(const std::string& root) {
    using namespace edge_gateway;
    require(ota_query_detail::sha256Hex("") ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" &&
        ota_query_detail::sha256Hex("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 fixture failed");
    const std::pair<std::size_t, const char*> vectors[] = {
        {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"}
    };
    for (const auto& vector : vectors) {
        const auto actual = ota_query_detail::sha256Hex(std::string(vector.first, 'a'));
        if (actual != vector.second)
            throw std::runtime_error("SHA-256 length " + std::to_string(vector.first) +
                " expected " + vector.second + " actual " + actual);
    }
    const std::string jobId = "QUERY_JOB";
    const std::string machine = "GW_QUERY";
    const std::string token = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    const std::string transaction = "OTA_b2ff4873e361550ecbba12a333ec6e75";
    require("OTA_" + ota_query_detail::sha256Hex(jobId + token).substr(0, 32) == transaction,
        "Python transaction digest mismatch");
    const auto home = root + "/home";
    const auto staging = home + "/ota/staging";
    const auto production = home + "/data/runtime-production";
    std::filesystem::create_directories(staging);
    std::filesystem::create_directories(production + "/transactions/" + transaction);
    std::filesystem::create_directories(production + "/boot-lineage");
    std::filesystem::create_directories(production + "/baselines");
    const std::string previous = "{\"baselineId\":\"BASE_OLD\",\"format\":\"runtime-baseline-v2\",\"machineCode\":\"GW_QUERY\",\"release\":\"1.0.0\"}\n";
    const std::string candidateRecord = "{\"baselineId\":\"BASE_NEW\",\"format\":\"runtime-baseline-v2\",\"machineCode\":\"GW_QUERY\",\"release\":\"2.0.0\"}\n";
    const std::string candidate = ota_query_detail::sha256Hex(candidateRecord);
    require(candidate == "a8da8b59d749be398f88422e90afd8eee499f878b90d12b4ed8c2f8fc6c7dd76",
        "Python candidate digest mismatch");
    const std::string previousSha = ota_query_detail::sha256Hex(previous);
    const std::string approval = "{\"format\":\"runtime-provisioning-v2\",\"transaction\":" +
        pendingJsonString(transaction) + ",\"gatewayHome\":" + pendingJsonString(home) +
        ",\"machineCode\":\"GW_QUERY\",\"parentSha256\":" + pendingJsonString(previousSha) +
        ",\"previous\":" + previous.substr(0, previous.size() - 1) +
        ",\"candidate\":" + candidateRecord.substr(0, candidateRecord.size() - 1) + "}\n";
    const auto approvalSha = ota_query_detail::sha256Hex(approval);
    const std::string state = "{\"format\":\"runtime-install-v1\",\"transaction\":" +
        pendingJsonString(transaction) + ",\"approvalSha256\":" + pendingJsonString(approvalSha) +
        ",\"adoption\":false,\"phase\":\"COMPLETED\",\"candidateSha256\":" +
        pendingJsonString(candidate) + ",\"previousSha256\":" + pendingJsonString(previousSha) + "}\n";
    const std::string productionHealth = "{\"format\":\"runtime-health-v2\",\"status\":\"healthy\",\"transaction\":" +
        pendingJsonString(transaction) + ",\"baselineSha256\":" + pendingJsonString(candidate) + "}\n";
    const std::string lineage = "{\"transaction\":" + pendingJsonString(transaction) +
        ",\"approvalSha256\":" + pendingJsonString(approvalSha) +
        ",\"candidateSha256\":" + pendingJsonString(candidate) +
        ",\"previousSha256\":" + pendingJsonString(previousSha) + "}\n";
    const auto txDir = production + "/transactions/" + transaction;
    put(txDir + "/approval.json", approval);
    put(txDir + "/candidate.json", candidateRecord);
    put(txDir + "/previous.json", previous);
    put(txDir + "/state.json", state);
    put(txDir + "/health.json", productionHealth);
    put(production + "/boot-lineage/" + candidate + "-" + transaction + ".json", lineage);
    put(production + "/baselines/BASE_NEW.json", candidateRecord);
    PendingOtaJob job;
    job.jobId = jobId;
    job.machineCode = machine;
    job.version = "2.0.0";
    job.artifactPath = "/tmp/ota-m3-local-query-artifact";
    job.previousVersion = "1.0.0";
    job.token = token;
    job.lifecycle = "systemd-handoff-v1";
    job.packageType = "config";
    job.artifactSha256 = std::string(64, 'a');
    job.artifactSize = 13;
    job.leaseDev = 1;
    job.leaseInode = 2;
    const auto intent = encodePendingJob(job);
    const std::string intentSha = "708bd78213dddef0a7ec017b732aa8212d5f2b549eee0ead451db52ef54aacbd";
    require(ota_query_detail::sha256Hex(intent) == intentSha, "Python intent digest mismatch");
    const auto health = "{\"format\":\"ota-health-v1\",\"jobId\":\"" + jobId +
        "\",\"version\":\"2.0.0\",\"token\":\"" + token +
        "\",\"status\":\"healthy\",\"rollback\":\"none\",\"ts\":\"1000\",\"productionTransaction\":\"" +
        transaction + "\",\"productionCandidateSha256\":\"" + candidate + "\"}\n";
    const std::string healthSha = "4a8e7d2af3732e33bd04b614ec31ca3ea344c9255b80c1993947417691d0fc27";
    require(ota_query_detail::sha256Hex(health) == healthSha, "Python health digest mismatch");
    const auto handoff = "{\"format\":\"ota-handoff-v1\",\"jobId\":\"" + jobId +
        "\",\"version\":\"2.0.0\",\"token\":\"" + token +
        "\",\"intentSha256\":\"" + intentSha +
        "\",\"gatewayHome\":" + pendingJsonString(home) +
        ",\"productionApprovalSha256\":" + pendingJsonString(approvalSha) +
        ",\"phase\":\"OWNER_FINISHED\",\"runtimeMayChange\":true,\"productionTransaction\":\"" + transaction +
        "\",\"completionProof\":{\"status\":\"healthy\",\"rollback\":\"none\",\"transaction\":\"" + transaction +
        "\",\"healthSha256\":\"" + healthSha +
        "\",\"healthRecord\":" + pendingJsonString(health) + "}}\n";
    const auto final = "jobId=" + jobId + "\ntoken=" + token + "\nstatus=completed\n";
    const auto audit = "{\"format\":\"ota-reconcile-v1\",\"reason\":\"ota-terminal-outbox-v1\",\"jobId\":\"" + jobId +
        "\",\"token\":\"" + token + "\",\"version\":\"2.0.0\",\"finalState\":\"delivery-pending\",\"phase\":\"terminal-journal\",\"evidence\":\"verified-health\",\"ts\":\"1000\"}\n";
    put(staging + "/ota_job_" + jobId + ".json", intent);
    put(staging + "/health_" + jobId + ".txt", health);
    put(staging + "/ota_handoff_" + jobId + ".json", handoff);
    put(staging + "/ota_final_" + jobId + ".txt", final);
    put(staging + "/ota_reconcile_" + jobId + ".json", audit);
    OtaConfig config;
    config.stagingDir = staging;
    OtaService service(config);
    const auto original = tree(staging);
    auto outcome = service.readOriginalJob(jobId);
    require(outcome.outcome == "HEALTHY_APPLIED" && outcome.jobId == jobId &&
        outcome.machineCode == machine && outcome.occurrenceId == token &&
        outcome.productionTransaction == transaction && outcome.productionCandidateSha256 == candidate,
        "original healthy result not bound");
    require(tree(staging) == original, "query mutated journal or created a lock");
    require(service.readOriginalJob(jobId, machine, token).outcome == "HEALTHY_APPLIED" &&
        service.readOriginalJob(jobId, "OTHER_MACHINE", token).outcome == "UNKNOWN" &&
        service.readOriginalJob(jobId, machine, "bbbbbbbb-bbbb-4ccc-8ddd-eeeeeeeeeeee").outcome == "UNKNOWN",
        "original-job scoped identity was not enforced");
    require(service.readOriginalJob("../foreign").outcome == "UNKNOWN" &&
        service.readOriginalJob("OTHER_JOB").outcome == "UNKNOWN", "invalid/missing job resolved");
    const auto handoffPath = staging + "/ota_handoff_" + jobId + ".json";
    const auto auditPath = staging + "/ota_reconcile_" + jobId + ".json";
    const auto statePath = txDir + "/state.json";
    put(handoffPath, handoff.substr(0, handoff.size() - 2) + ",\"phase\":\"CANCELLED\"}\n");
    require(service.readOriginalJob(jobId).outcome == "UNKNOWN", "duplicate handoff phase resolved");
    put(handoffPath, handoff);
    put(auditPath, audit.substr(0, audit.size() - 2) + ",\"finalState\":\"committed\"}\n");
    require(service.readOriginalJob(jobId).outcome == "UNKNOWN", "duplicate audit state resolved");
    put(auditPath, audit);
    auto invalidAudit = audit;
    invalidAudit.replace(invalidAudit.find("ota-reconcile-v1"), 16, "wrong-audit-v1");
    put(auditPath, invalidAudit);
    require(service.readOriginalJob(jobId).outcome == "UNKNOWN", "wrong audit format resolved");
    invalidAudit = audit;
    invalidAudit.replace(invalidAudit.find("\"ts\":\"1000\""), 11, "\"ts\":\"0\"");
    put(auditPath, invalidAudit);
    require(service.readOriginalJob(jobId).outcome == "UNKNOWN", "nonpositive audit timestamp resolved");
    put(auditPath, audit);
    put(statePath, state.substr(0, state.size() - 2) + ",\"phase\":\"RECOVERED\"}\n");
    require(service.readOriginalJob(jobId).outcome == "UNKNOWN", "duplicate production state resolved");
    put(statePath, state);
    std::filesystem::remove(statePath);
    require(service.readOriginalJob(jobId).outcome == "UNKNOWN", "missing production state resolved");
    put(statePath, state);
    const auto lineagePath = production + "/boot-lineage/" + candidate + "-" + transaction + ".json";
    put(lineagePath, lineage.substr(0, lineage.size() - 2) + ",\"candidateSha256\":\"" +
        std::string(64, 'f') + "\"}\n");
    require(service.readOriginalJob(jobId).outcome == "UNKNOWN", "conflicting lineage resolved");
    put(lineagePath, lineage);
    auto promoting = state;
    promoting.replace(promoting.find("COMPLETED"), 9, "PROMOTING");
    put(statePath, promoting);
    require(service.readOriginalJob(jobId).outcome == "UNKNOWN", "uncommitted promotion resolved");
    put(statePath, state);
    const auto healthPath = staging + "/health_" + jobId + ".txt";
    put(healthPath, health + " ");
    require(service.readOriginalJob(jobId).outcome == "UNKNOWN", "tampered health resolved");
    put(healthPath, health);
    put(healthPath, std::string(4097, 'x'));
    require(service.readOriginalJob(jobId).outcome == "UNKNOWN", "oversized health resolved");
    put(healthPath, health);
    const auto renamed = staging + "/health-saved.txt";
    std::filesystem::rename(healthPath, renamed);
    std::filesystem::create_symlink(renamed, healthPath);
    require(service.readOriginalJob(jobId).outcome == "UNKNOWN", "symlink health resolved");
    std::filesystem::remove(healthPath);
    std::filesystem::rename(renamed, healthPath);
    std::filesystem::create_hard_link(healthPath, renamed);
    require(service.readOriginalJob(jobId).outcome == "UNKNOWN", "multilink health resolved");
    std::filesystem::remove(renamed);
    put(staging + "/ota_final_" + jobId + ".txt", "jobId=OTHER_JOB\ntoken=" + token + "\nstatus=completed\n");
    require(service.readOriginalJob(jobId).outcome == "UNKNOWN", "foreign final resolved");
    put(staging + "/ota_final_" + jobId + ".txt", final);
    queryAfterFirstSnapshot = [&] { put(healthPath, health + " "); };
    require(service.readOriginalJob(jobId).outcome == "UNKNOWN", "mixed generation resolved");
    queryAfterFirstSnapshot = {};
    put(healthPath, health);
    require(service.readOriginalJob(jobId).outcome == "HEALTHY_APPLIED", "query did not recover after fixture repair");
    const std::string failedHealth = "{\"format\":\"ota-health-v1\",\"jobId\":\"" + jobId +
        "\",\"version\":\"2.0.0\",\"token\":\"" + token +
        "\",\"status\":\"failed\",\"rollback\":\"succeeded\",\"ts\":\"2000\"}\n";
    const std::string failedHandoff = "{\"format\":\"ota-handoff-v1\",\"jobId\":\"" + jobId +
        "\",\"version\":\"2.0.0\",\"token\":\"" + token +
        "\",\"intentSha256\":\"" + intentSha + "\",\"gatewayHome\":" + pendingJsonString(home) +
        ",\"productionApprovalSha256\":" + pendingJsonString(approvalSha) +
        ",\"phase\":\"OWNER_FINISHED\",\"runtimeMayChange\":true,\"productionTransaction\":\"" + transaction +
        "\",\"completionProof\":{\"status\":\"failed\",\"rollback\":\"succeeded\",\"transaction\":\"" + transaction +
        "\",\"healthSha256\":" + pendingJsonString(ota_query_detail::sha256Hex(failedHealth)) +
        ",\"healthRecord\":" + pendingJsonString(failedHealth) + "}}\n";
    auto recovered = state;
    recovered.replace(recovered.find("COMPLETED"), 9, "RECOVERED");
    put(statePath, recovered);
    put(txDir + "/previous-pointer.json", "{\"baselineId\":\"BASE_OLD\",\"sha256\":" +
        pendingJsonString(previousSha) + "}\n");
    put(production + "/baselines/BASE_OLD.json", previous);
    put(healthPath, failedHealth);
    put(handoffPath, failedHandoff);
    put(staging + "/ota_final_" + jobId + ".txt", "jobId=" + jobId + "\ntoken=" + token + "\nstatus=failed\n");
    outcome = service.readOriginalJob(jobId);
    require(outcome.outcome == "FAILED_ROLLED_BACK" && outcome.productionCandidateSha256 == candidate,
        "original rollback lost admitted candidate");
    put(txDir + "/previous-pointer.json", "{\"baselineId\":\"BASE_OLD\",\"sha256\":\"" +
        std::string(64, 'f') + "\"}\n");
    require(service.readOriginalJob(jobId).outcome == "UNKNOWN", "wrong prior pointer resolved rollback");
    require(!std::filesystem::exists(staging + "/ota_state.lock") &&
        !std::filesystem::exists(staging + "/ota_status_pending.log.lock") &&
        !std::filesystem::exists(staging + "/ota_job_" + jobId + ".lock"), "query created a lock");
    std::cout << "original-job private read-only evidence PASS\n";
}

}  // namespace

int main() {
    try {
        if (geteuid() != 0) throw std::runtime_error("root-owned private fixture required");
        char pattern[] = "/tmp/ota-m3-local-query-XXXXXX";
        const char* created = mkdtemp(pattern);
        require(created != nullptr, "private root creation failed");
        const std::string root(created);
        require(root.rfind("/tmp/ota-m3-local-query-", 0) == 0, "private root escaped /tmp");
        try { check(root); }
        catch (...) { std::filesystem::remove_all(root); throw; }
        std::filesystem::remove_all(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
