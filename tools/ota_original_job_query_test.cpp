#define OTA_QUERY_FAULT_TEST
#include "../src/ota_service.cpp"
#include "edge_gateway/ota_request_json.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

#ifdef OTA_FULL_QUERY_CLOCK_TEST
#if !defined(__linux__) || !defined(__GNUC__)
#error OTA_FULL_QUERY_CLOCK_TEST requires the Linux GNU system_clock link wrapper
#endif
#include <optional>
#include "../src/ota_query_crypto.cpp"

namespace {
std::int64_t fullQueryClockMs = 0;
std::uint64_t fullQueryClockCalls = 0;
}

// This dedicated binary freezes wall time only; its supervisor uses monotonic time.
extern "C" std::chrono::system_clock::time_point fullQuerySystemNow()
    asm("__wrap__ZNSt6chrono3_V212system_clock3nowEv");
extern "C" std::chrono::system_clock::time_point fullQuerySystemNow() {
    ++fullQueryClockCalls;
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(fullQueryClockMs));
}
#endif

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

edge_gateway::OtaConfig privateOwnerConfig(const std::string& staging) {
    const std::filesystem::path stage(staging);
    const auto home = stage.parent_path().parent_path();
    require(stage.is_absolute() && stage.filename() == "staging" &&
        stage.parent_path().filename() == "ota" &&
        home.string().rfind("/tmp/production-runtime-", 0) == 0,
        "owner probe escaped private fixture home");
    edge_gateway::OtaConfig config;
    config.stagingDir = staging;
    config.downloadDir = (home / "ota/downloads").string();
    config.backupDir = (home / "ota/backup").string();
    config.applyScript = (home / "bin/offline-runtime-upgrade.py").string();
    config.rollbackScript = (home / "bin/ota-rollback.sh").string();
    config.signaturePublicKeyPath =
        (home.parent_path().parent_path() / "etc/modbus-gateway/ota.pub").string();
    return config;
}

#ifdef OTA_FULL_QUERY_CLOCK_TEST
void checkFullQueryClocks(const std::string& requestPath, const std::string& platformPublicKey,
                         const std::string& devicePrivateKey, const std::string& devicePublicKey,
                         const std::string& staging) {
    using namespace edge_gateway;
    const auto challenge = ota_full_query_protocol::parseChallenge(get(requestPath));
    require(challenge.jobId == "JOB_1" && challenge.machineCode == "GW_TEST" &&
        challenge.nonce == std::string(64, 'b') && challenge.packageType == "full" &&
        challenge.version == "2.0.0" && challenge.artifactSha256 == std::string(64, 'a') &&
        challenge.artifactSize == 13 && challenge.issuedAtMs == 1700000000000LL &&
        challenge.expiresAtMs == 1700000060000LL && challenge.signature != "AA==",
        "clock input must be an actually signed Java request-normal field set");
    auto config = privateOwnerConfig(staging);
    config.requireSignature = true;
    config.signaturePublicKeyPath = platformPublicKey;
    config.querySigningPrivateKeyPath = devicePrivateKey;
    struct stat directory{};
    require(lstat(staging.c_str(), &directory) == 0 && S_ISDIR(directory.st_mode) &&
        directory.st_uid == 0 && (directory.st_mode & 0022) == 0,
        "clock fixture requires a trusted root-owned private staging directory");
    const auto before = tree(staging);
    require(before.empty(), "clock fixture staging must be empty");
    OtaService service(config);
    struct Case { const char* name; std::int64_t receivedAt; bool accepted; };
    const Case cases[] = {
        {"clock-future-skew-exact-30000", 1699999970000LL, true},
        {"clock-future-skew-30001", 1699999969999LL, false},
        {"clock-expiry-minus-one", 1700000059999LL, true},
        {"clock-at-expiry", 1700000060000LL, false},
        {"clock-negative-received", -1, false},
    };
    for (const auto& item : cases) {
        require(ota_query_crypto::verifyChallenge(challenge, platformPublicKey),
            "clock case never reached a valid production-verified signature");
        fullQueryClockMs = item.receivedAt;
        require(currentTimeMs() == item.receivedAt, "clock link wrapper did not bind");
        const auto callsBefore = fullQueryClockCalls;
        std::optional<OtaFullQueryObservation> response;
        bool expired = false;
        try { response = service.answerOriginalJobQuery(challenge, challenge.machineCode); }
        catch (const OtaQueryRejected& error) {
            require(error.reason() == OtaQueryFailure::Expired,
                "clock case rejected for a non-time reason");
            expired = true;
        }
        require(fullQueryClockCalls > callsBefore, "production answer did not consult wrapped time");
        require(tree(staging) == before, "clock query changed staging evidence or created a lock");
        if (item.accepted) {
            require(response.has_value() && !expired, "valid clock boundary did not answer");
            const auto& value = *response;
            require(value.outcome == "UNKNOWN" && value.jobId == challenge.jobId &&
                value.machineCode == challenge.machineCode && value.nonce == challenge.nonce &&
                value.packageType == "full" && value.version == challenge.version &&
                value.artifactSha256 == challenge.artifactSha256 && value.artifactSize == challenge.artifactSize &&
                value.occurrenceId.empty() && value.candidateSha256.empty() && value.transaction.empty() &&
                value.parentSha256.empty() && value.intentSha256.empty() && value.healthSha256.empty() &&
                value.healthTs == 0 && !value.signature.empty() && value.signature != "AA==",
                "clock UNKNOWN lost original fields or invented resolved proof");
            const auto descriptor = ota_full_query_protocol::observationDescriptor(value);
            // Compile the unmodified crypto source into this test-only translation unit.
            require(ota_query_crypto::verifyDescriptor(descriptor, value.signature, devicePublicKey),
                "clock response did not verify with the actual device test public key");
            std::cout << "{\"case\":" << pendingJsonString(item.name)
                << ",\"receivedAtMs\":" << pendingJsonString(std::to_string(item.receivedAt))
                << ",\"signatureVerified\":true,\"responseVerified\":true,\"response\":"
                << ota_full_query_protocol::encodeObservation(value) << "}\n";
        } else {
            require(expired && !response.has_value(), "invalid clock boundary produced a response");
            std::cout << "{\"case\":" << pendingJsonString(item.name)
                << ",\"receivedAtMs\":" << pendingJsonString(std::to_string(item.receivedAt))
                << ",\"signatureVerified\":true,\"rejection\":\"EXPIRED\",\"response\":null}\n";
        }
    }
    std::cout << "full-query production clock boundaries 5 PASS\n";
}
#endif

void check(const std::string& root) {
    using namespace edge_gateway;
    const auto duplicate = json::JsonParser("{\"nested\":{\"value\":\"first\",\"value\":\"second\"}}", 4, 8).parse();
    require(duplicate.find("nested")->find("value")->asString() == "first",
        "duplicate fixture did not reach first-key parser behavior");
    bool duplicateRejected = false;
    try { requireUniqueQueryJson(duplicate); }
    catch (const std::runtime_error&) { duplicateRejected = true; }
    require(duplicateRejected, "nested duplicate query JSON passed uniqueness guard");
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
    const std::string previousSha = ota_query_detail::sha256Hex(previous);
    const std::string candidateRecord = "{\"baselineId\":\"BASE_NEW\",\"format\":\"runtime-baseline-v2\",\"machineCode\":\"GW_QUERY\",\"release\":\"2.0.0\",\"parentSha256\":" + pendingJsonString(previousSha) + "}\n";
    const std::string candidate = ota_query_detail::sha256Hex(candidateRecord);
    require(candidate == "89258c1a2980b9e5a29cecbddd0289639214f00c262591e9d1dfd1331bced206",
        "Python parent-bound candidate digest mismatch");
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
    const std::string healthSha = ota_query_detail::sha256Hex(health);
    require(healthSha == "ba169a0e93044ab61b4ddb9a82b92010ecf99b35d3c4e351d3f87ff636b9b51c",
        "Python parent-bound health digest mismatch");
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
    const int stagingFd = open(staging.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    require(stagingFd >= 0, "cannot open private query fixture directory");
    std::size_t tinyDisplayBudget = 1;
    std::size_t fileRemaining = 4096;
    bool budgetRejected = false;
    try {
        (void)readOriginalEvidenceFile(stagingFd, "ota_job_" + jobId + ".json", 4096,
            fileRemaining, &tinyDisplayBudget);
    } catch (const std::runtime_error&) { budgetRejected = true; }
    close(stagingFd);
    require(budgetRejected && tinyDisplayBudget == 0,
        "display query exceeded its shared evidence-byte budget");
    OtaConfig config;
    config.stagingDir = staging;
    OtaService service(config);
    const auto original = tree(staging);
    auto outcome = service.readOriginalJob(jobId);
    require(outcome.outcome == "HEALTHY_APPLIED" && outcome.jobId == jobId &&
        outcome.machineCode == machine && outcome.occurrenceId == token &&
        outcome.packageType == "config" && outcome.productionParentSha256 == previousSha &&
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
    const auto candidatePath = txDir + "/candidate.json";
    const auto coherentParentCases = [&](bool healthy, const std::string& otaHealth,
                                         const std::string& ownedHandoff, const std::string& installState) {
        const auto replaceBound = [&](std::string bytes, const std::string& from, const std::string& to) {
            const auto at = bytes.find(from);
            require(at != std::string::npos && bytes.find(from, at + from.size()) == std::string::npos,
                "coherent fixture replacement must bind exactly once");
            bytes.replace(at, from.size(), to);
            return bytes;
        };
        // Rebind every digest/embedded record; only the selected parent relationship is invalid.
        const auto writeVariant = [&](const std::string& candidateParent, const std::string& approvalParent) {
            const auto parentAt = candidateRecord.find(",\"parentSha256\"");
            require(parentAt != std::string::npos, "coherent candidate template lost parent");
            const auto nextCandidate = candidateRecord.substr(0, parentAt) +
                (candidateParent.empty() ? "}\n" : ",\"parentSha256\":" + pendingJsonString(candidateParent) + "}\n");
            const auto nextCandidateSha = ota_query_detail::sha256Hex(nextCandidate);
            const auto nextApproval = "{\"format\":\"runtime-provisioning-v2\",\"transaction\":" +
                pendingJsonString(transaction) + ",\"gatewayHome\":" + pendingJsonString(home) +
                ",\"machineCode\":\"GW_QUERY\"" +
                (approvalParent.empty() ? "" : ",\"parentSha256\":" + pendingJsonString(approvalParent)) +
                ",\"previous\":" + previous.substr(0, previous.size() - 1) +
                ",\"candidate\":" + nextCandidate.substr(0, nextCandidate.size() - 1) + "}\n";
            const auto nextApprovalSha = ota_query_detail::sha256Hex(nextApproval);
            const auto nextState = replaceBound(replaceBound(installState, approvalSha, nextApprovalSha),
                candidate, nextCandidateSha);
            const auto nextHealth = healthy ? replaceBound(otaHealth, candidate, nextCandidateSha) : otaHealth;
            auto nextHandoff = replaceBound(ownedHandoff, approvalSha, nextApprovalSha);
            nextHandoff = replaceBound(nextHandoff, ota_query_detail::sha256Hex(otaHealth),
                ota_query_detail::sha256Hex(nextHealth));
            nextHandoff = replaceBound(nextHandoff, pendingJsonString(otaHealth), pendingJsonString(nextHealth));
            const auto nextLineage = replaceBound(replaceBound(lineage, approvalSha, nextApprovalSha),
                candidate, nextCandidateSha);
            const auto parsedApproval = json::JsonParser(nextApproval, 12, 4096).parse();
            const auto parsedState = json::JsonParser(nextState, 6, 128).parse();
            const auto parsedHandoff = json::JsonParser(nextHandoff, 12, 4096).parse();
            require(sameQueryJson(*parsedApproval.find("candidate"), json::JsonParser(nextCandidate).parse()) &&
                sameQueryJson(*parsedApproval.find("previous"), json::JsonParser(previous).parse()) &&
                pendingField(parsedState, "candidateSha256") == nextCandidateSha &&
                pendingField(parsedState, "previousSha256") == previousSha &&
                pendingField(parsedState, "approvalSha256") == nextApprovalSha &&
                pendingField(parsedHandoff, "productionApprovalSha256") == nextApprovalSha &&
                pendingField(*parsedHandoff.find("completionProof"), "healthSha256") ==
                    ota_query_detail::sha256Hex(nextHealth) &&
                pendingField(*parsedHandoff.find("completionProof"), "healthRecord") == nextHealth,
                "parent fixture lost an unrelated digest/JSON binding");
            put(candidatePath, nextCandidate);
            put(txDir + "/approval.json", nextApproval);
            put(statePath, nextState);
            put(txDir + "/health.json", replaceBound(productionHealth, candidate, nextCandidateSha));
            put(production + "/boot-lineage/" + nextCandidateSha + "-" + transaction + ".json", nextLineage);
            put(production + "/baselines/BASE_NEW.json", nextCandidate);
            put(staging + "/health_" + jobId + ".txt", nextHealth);
            put(handoffPath, nextHandoff);
            return nextCandidateSha;
        };
        const auto expected = healthy ? "HEALTHY_APPLIED" : "FAILED_ROLLED_BACK";
        for (const bool changeCandidate : {true, false}) {
            for (const auto& parent : {std::string(), std::string(64, 'f')}) {
                writeVariant(previousSha, previousSha);
                const auto valid = service.readOriginalJob(jobId);
                require(valid.outcome == expected && valid.productionCandidateSha256 == candidate &&
                    valid.productionParentSha256 == previousSha && valid.machineCode == machine &&
                    valid.occurrenceId == token && valid.intentSha256 == intentSha &&
                    valid.healthSha256 == ota_query_detail::sha256Hex(otaHealth),
                    "coherent parent positive baseline did not resolve");
                const auto changedSha = writeVariant(changeCandidate ? parent : previousSha,
                    changeCandidate ? previousSha : parent);
                const auto beforeQuery = tree(staging);
                const auto proof = service.readOriginalJob(jobId);
                const auto name = std::string(healthy ? "healthy-" : "rollback-") +
                    (changeCandidate ? "candidate-" : "approval-") + (parent.empty() ? "missing" : "wrong");
                std::cout << "coherent-parent case=" << name << " baseline=" << expected
                    << " actual=" << proof.outcome << " candidateSha256=" << changedSha << std::endl;
                require(proof.outcome == "UNKNOWN", "coherent parent negative resolved trusted evidence");
                require(proof.jobId == jobId && proof.machineCode.empty() && proof.occurrenceId.empty() &&
                    proof.packageType.empty() && proof.version.empty() && proof.artifactSha256.empty() &&
                    proof.artifactSize == 0 && proof.productionTransaction.empty() &&
                    proof.productionCandidateSha256.empty() && proof.productionParentSha256.empty() &&
                    proof.intentSha256.empty() && proof.healthSha256.empty() && proof.healthTs == 0,
                    "coherent parent rejection leaked partial trusted proof");
                require(tree(staging) == beforeQuery, "coherent parent query mutated staging");
                writeVariant(previousSha, previousSha);
                if (changedSha != candidate)
                    std::filesystem::remove(production + "/boot-lineage/" + changedSha + "-" + transaction + ".json");
            }
        }
        require(service.readOriginalJob(jobId).outcome == expected, "parent fixture did not restore baseline");
    };
    coherentParentCases(true, health, handoff, state);
    for (const auto& parent : {std::string(), std::string(64, 'f')}) {
        const auto broken = candidateRecord.substr(0, candidateRecord.find(",\"parentSha256\"")) +
            (parent.empty() ? "}\n" : ",\"parentSha256\":" + pendingJsonString(parent) + "}\n");
        put(candidatePath, broken);
        require(service.readOriginalJob(jobId).outcome == "UNKNOWN", "missing/wrong candidate parent resolved healthy");
    }
    put(candidatePath, candidateRecord);
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
    require(outcome.outcome == "FAILED_ROLLED_BACK" && outcome.productionCandidateSha256 == candidate &&
        outcome.productionParentSha256 == previousSha,
        "original rollback lost admitted candidate");
    coherentParentCases(false, failedHealth, failedHandoff, recovered);
    put(txDir + "/previous-pointer.json", "{\"baselineId\":\"BASE_OLD\",\"sha256\":\"" +
        std::string(64, 'f') + "\"}\n");
    require(service.readOriginalJob(jobId).outcome == "UNKNOWN", "wrong prior pointer resolved rollback");
    put(txDir + "/previous-pointer.json", "{\"baselineId\":\"BASE_OLD\",\"sha256\":" +
        pendingJsonString(previousSha) + "}\n");
    put(candidatePath, candidateRecord.substr(0, candidateRecord.find(",\"parentSha256\"")) + "}\n");
    require(service.readOriginalJob(jobId).outcome == "UNKNOWN", "missing candidate parent resolved rollback");
    put(candidatePath, candidateRecord);
    const std::string unresolvedHealth = "{\"format\":\"ota-health-v1\",\"jobId\":\"" + jobId +
        "\",\"version\":\"2.0.0\",\"token\":\"" + token +
        "\",\"status\":\"failed\",\"rollback\":\"failed\",\"ts\":\"2000\"}\n";
    const std::string unresolvedHandoff = "{\"format\":\"ota-handoff-v1\",\"jobId\":\"" + jobId +
        "\",\"version\":\"2.0.0\",\"token\":\"" + token +
        "\",\"intentSha256\":\"" + intentSha + "\",\"gatewayHome\":" + pendingJsonString(home) +
        ",\"productionApprovalSha256\":" + pendingJsonString(approvalSha) +
        ",\"phase\":\"OWNER_FINISHED\",\"runtimeMayChange\":true,\"productionTransaction\":\"" + transaction +
        "\",\"completionProof\":{\"status\":\"failed\",\"rollback\":\"failed\",\"transaction\":\"" + transaction +
        "\",\"healthSha256\":" + pendingJsonString(ota_query_detail::sha256Hex(unresolvedHealth)) +
        ",\"healthRecord\":" + pendingJsonString(unresolvedHealth) + "}}\n";
    put(healthPath, unresolvedHealth);
    put(handoffPath, unresolvedHandoff);
    const auto unknown = service.readOriginalJob(jobId);
    require(unknown.outcome == "UNKNOWN" && unknown.jobId == jobId &&
        unknown.productionTransaction.empty() && unknown.productionCandidateSha256.empty() &&
        unknown.healthSha256.empty() && unknown.machineCode.empty(),
        "unresolved rollback leaked partial query proof");
    put(healthPath, failedHealth);
    put(handoffPath, failedHandoff);
    require(service.readOriginalJob(jobId).outcome == "FAILED_ROLLED_BACK",
        "repaired rollback proof did not resolve");
    const auto displayStaging = root + "/display/ota/staging";
    std::filesystem::create_directories(displayStaging);
    const std::string displayJob = "DISPLAY_FINAL";
    const std::string displayIntent = "{\"format\":\"ota-job-v1\",\"jobId\":\"" + displayJob +
        "\",\"machineCode\":\"GW_QUERY\",\"version\":\"2.0.0\","
        "\"artifactPath\":\"/private/fixture.tar.gz\",\"previousVersion\":\"1.0.0\","
        "\"token\":\"" + token + "\",\"lifecycle\":\"inherited-flock-v1\"}\n";
    const std::string displayFinal = "jobId=" + displayJob + "\ntoken=" + token +
        "\nstatus=completed\n";
    put(displayStaging + "/ota_job_" + displayJob + ".json", displayIntent);
    put(displayStaging + "/ota_final_" + displayJob + ".txt", displayFinal);
    OtaConfig displayConfig;
    displayConfig.stagingDir = displayStaging;
    const auto exactBudget = 3 * (displayIntent.size() + displayFinal.size());
    const auto exactPage = OtaService(displayConfig).loadFinalStatusesReadOnly(exactBudget);
    require(exactPage.items.size() == 1 && exactPage.items.front().jobId == displayJob &&
        exactPage.truncated && exactPage.terminalJobs.size() == 1,
        "exact display budget dropped the last fully verified final");
    const auto proofStaging = root + "/bounded-proof/ota/staging";
    std::filesystem::create_directories(proofStaging);
    PendingOtaJob rejectedJob = job;
    rejectedJob.jobId = "BOUNDED_PROOF";
    rejectedJob.packageType = "full";
    const auto rejectedIntent = encodePendingJob(rejectedJob);
    const auto rejectedHealth = "{\"format\":\"ota-health-v1\",\"jobId\":\"BOUNDED_PROOF\","
        "\"version\":\"2.0.0\",\"token\":\"" + token +
        "\",\"status\":\"failed\",\"rollback\":\"unchanged\",\"ts\":\"3000\"}\n";
    const auto rejectedHandoff = "{\"format\":\"ota-handoff-v1\",\"jobId\":\"BOUNDED_PROOF\","
        "\"version\":\"2.0.0\",\"token\":\"" + token +
        "\",\"intentSha256\":\"" + ota_query_detail::sha256Hex(rejectedIntent) +
        "\",\"phase\":\"CANCELLED\",\"runtimeMayChange\":false,\"cancelProof\":{\"noWriter\":true,"
        "\"unit\":\"ota-runtime-BOUNDED_PROOF-" + token +
        ".service\",\"mainPid\":\"0\",\"job\":\"\"}}\n";
    const std::string rejectedFinal = "jobId=BOUNDED_PROOF\ntoken=" + token + "\nstatus=failed\n";
    const std::string rejectedAudit = "{\"format\":\"ota-reconcile-v1\","
        "\"reason\":\"ota-terminal-outbox-v1\",\"jobId\":\"BOUNDED_PROOF\","
        "\"token\":\"" + token + "\",\"version\":\"2.0.0\","
        "\"finalState\":\"delivery-pending\",\"phase\":\"terminal-journal\","
        "\"evidence\":\"verified-health\",\"ts\":\"3000\"}\n";
    put(proofStaging + "/ota_job_BOUNDED_PROOF.json", rejectedIntent);
    put(proofStaging + "/health_BOUNDED_PROOF.txt", rejectedHealth);
    put(proofStaging + "/ota_handoff_BOUNDED_PROOF.json", rejectedHandoff);
    put(proofStaging + "/ota_final_BOUNDED_PROOF.txt", rejectedFinal);
    put(proofStaging + "/ota_reconcile_BOUNDED_PROOF.json", rejectedAudit);
    OtaConfig proofConfig;
    proofConfig.stagingDir = proofStaging;
    OtaService proofService(proofConfig);
    require(proofService.readOriginalJob(rejectedJob.jobId).outcome == "REJECTED_UNCHANGED",
        "bounded fixture did not reach verified handoff proof");
    const auto proofBudget = 2 * (rejectedIntent.size() + rejectedHealth.size() +
        rejectedHandoff.size() + rejectedFinal.size() + rejectedAudit.size());
    const auto oneShort = proofService.loadFinalStatusesReadOnly(proofBudget - 1);
    require(oneShort.items.empty() && oneShort.truncated,
        "under-budget handoff proof unexpectedly resolved");
    const auto exactProof = proofService.loadFinalStatusesReadOnly(proofBudget);
    require(exactProof.items.size() == 1 && exactProof.items.front().jobId == rejectedJob.jobId &&
        exactProof.items.front().stage == "failed" && exactProof.truncated &&
        exactProof.terminalJobs.size() == 1 &&
        exactProof.terminalJobs.front().occurrenceId == token,
        "exact budget dropped a fully verified handoff proof");
    for (int i = 0; i < 4100; ++i)
        put(staging + "/noise_" + std::to_string(i), "x");
    require(service.readOriginalJob(jobId).outcome == "FAILED_ROLLED_BACK",
        "unrelated directory entries blocked exact original-job query");
    require(!std::filesystem::exists(staging + "/ota_state.lock") &&
        !std::filesystem::exists(staging + "/ota_status_pending.log.lock") &&
        !std::filesystem::exists(staging + "/ota_job_" + jobId + ".lock"), "query created a lock");
    std::cout << "original-job private read-only evidence PASS\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (geteuid() != 0) throw std::runtime_error("root-owned private fixture required");
#ifdef OTA_FULL_QUERY_CLOCK_TEST
        require(argc == 7 && std::string(argv[1]) == "--full-query-clock-vectors",
            "dedicated clock binary requires request, platform public, device private/public and staging");
        checkFullQueryClocks(argv[2], argv[3], argv[4], argv[5], argv[6]);
        return 0;
#else
        if (argc >= 2 && std::string(argv[1]) == "--full-query-clock-vectors")
            throw std::runtime_error("clock selector requires OTA_FULL_QUERY_CLOCK_TEST and GNU link wrapper");
#endif
        if (argc >= 2 && (std::string(argv[1]) == "--admit-owner-local" || std::string(argv[1]) == "--admit-full-owner-local")) {
            const bool full = std::string(argv[1]) == "--admit-full-owner-local";
            require(argc == 6, "admit-owner-local requires request, public key, artifact and staging");
            auto config = privateOwnerConfig(argv[5]);
            const auto home = std::filesystem::path(argv[5]).parent_path().parent_path();
            config.enabled = true;
            config.signaturePublicKeyPath = argv[3];
            config.applyScript = (home / "local-admission/offline-runtime-upgrade.py").string();
            config.minFreeBytes = 0;
            config.downloadRetryCount = 1;
            edge_gateway::OtaService service(config);
            const auto root = edge_gateway::ota_request_json::parseRoot(get(argv[2]));
            auto request = edge_gateway::ota_request_json::request(root);
            require(request.machineCode == "FLEET_NODE_NOT_CORE" && request.packageType == (full ? "full" : "config"),
                "local admission target mismatch");
            require(request.artifactUrl.rfind("https://", 0) == 0,
                "platform installation input lost its HTTPS URL");
            // Private transport substitute only; all signed descriptor fields are unchanged.
            request.artifactUrl = argv[4];
            edge_gateway::OtaReply reply;
            edge_gateway::OtaStatus status;
            try {
                service.execute(request, request.machineCode, edge_gateway::currentTimeMs(),
                    &reply, &status, [](const edge_gateway::OtaStatus&) {});
            } catch (const std::exception& error) {
                std::cerr << "native admission rejected stage=" << status.stage
                          << " cause=" << error.what() << '\n';
                throw;
            }
            if (status.stage != "applying")
                std::cerr << "native admission rejected stage=" << status.stage
                          << " cause=" << status.message << '\n';
            require(status.stage == "applying", "signed native admission did not reach handoff gap");
            const auto intent = get(config.stagingDir + "/ota_job_" + request.jobId + ".json");
            const auto pending = edge_gateway::decodePendingJob(intent);
            require(pending.jobId == request.jobId && pending.machineCode == request.machineCode &&
                pending.lifecycle == "systemd-handoff-v1" && pending.artifactSha256 == request.sha256 &&
                pending.artifactSize == request.size && pending.version == request.version,
                "native admission lost signed artifact identity");
            std::cout << intent;
            return 0;
        }
        if (argc >= 2 && std::string(argv[1]) == "--reconcile-existing") {
            require(argc == 4, "reconcile-existing requires staging and job");
            edge_gateway::OtaService service(privateOwnerConfig(argv[2]));
            service.reconcilePendingJobs();
            std::cout << "reconciled " << argv[3] << '\n';
            return 0;
        }
        if (argc >= 2 && (std::string(argv[1]) == "--read-existing" || std::string(argv[1]) == "--read-full-existing")) {
            const bool full = std::string(argv[1]) == "--read-full-existing";
            require(argc == 6, "read-existing requires staging, job, machine and token");
            edge_gateway::OtaService service(privateOwnerConfig(argv[2]));
            const auto proof = service.readOriginalJob(argv[3], argv[4], argv[5]);
            require(proof.outcome == "HEALTHY_APPLIED" && proof.jobId == argv[3] &&
                proof.machineCode == argv[4] && proof.occurrenceId == argv[5] &&
                proof.productionTransaction.rfind("OTA_", 0) == 0 &&
                proof.productionCandidateSha256.size() == 64 &&
                proof.intentSha256.size() == 64 && proof.healthSha256.size() == 64,
                "owner result did not resolve into bound original-job proof");
            require(!full || (proof.packageType == "full" &&
                edge_gateway::ota_query_protocol::digest(proof.productionParentSha256)),
                "full owner result did not bind package type/parent");
            std::cout << "{\"outcome\":" << edge_gateway::pendingJsonString(proof.outcome)
                << ",\"jobId\":" << edge_gateway::pendingJsonString(proof.jobId)
                << ",\"machineCode\":" << edge_gateway::pendingJsonString(proof.machineCode)
                << ",\"occurrenceId\":" << edge_gateway::pendingJsonString(proof.occurrenceId)
                << ",\"version\":" << edge_gateway::pendingJsonString(proof.version)
                << ",\"artifactSha256\":" << edge_gateway::pendingJsonString(proof.artifactSha256)
                << ",\"artifactSize\":" << proof.artifactSize
                << ",\"transaction\":" << edge_gateway::pendingJsonString(proof.productionTransaction)
                << ",\"candidateSha256\":" << edge_gateway::pendingJsonString(proof.productionCandidateSha256)
                << (full ? ",\"packageType\":\"full\",\"parentSha256\":" + edge_gateway::pendingJsonString(proof.productionParentSha256) : "")
                << ",\"intentSha256\":" << edge_gateway::pendingJsonString(proof.intentSha256)
                << ",\"healthSha256\":" << edge_gateway::pendingJsonString(proof.healthSha256)
                << ",\"healthTs\":" << proof.healthTs << "}\n";
            return 0;
        }
        require(argc == 1, "unknown original-job query selector");
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
