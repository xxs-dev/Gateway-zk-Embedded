#include "edge_gateway/ota_service.hpp"
#include "../src/ota_query_sha256.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <sys/stat.h>

int main(int argc, char** argv) {
    try {
        if (argc != 8) throw std::runtime_error("expected root entry artifact sha signature public-key size");
        edge_gateway::OtaConfig config;
        const std::string root = argv[1];
        config.enabled = true;
        config.downloadDir = root + "/downloads";
        config.stagingDir = root + "/staging";
        config.backupDir = root + "/backup";
        config.minFreeBytes = 0;
        config.applyScript = argv[2];
        config.signaturePublicKeyPath = argv[6];
        config.upgradeTimeoutSec = 40;
        edge_gateway::OtaRequest request;
        request.jobId = "HANDOFF_NATIVE";
        request.version = "1.0";
        request.packageType = "full";
        request.artifactUrl = argv[3];
        request.sha256 = argv[4];
        request.signature = argv[5];
        request.size = std::stoull(argv[7]);
        edge_gateway::OtaService service(config);
        edge_gateway::OtaReply reply;
        edge_gateway::OtaStatus status;
        service.execute(request, "LOCAL_ONLY", 1000, &reply, &status, nullptr);
        if (status.stage != "applying" && status.stage != "failed")
            throw std::runtime_error("handoff must never enter synchronous success tail");
        for (int i = 0; i < 100 && service.loadFinalStatuses().empty(); ++i) {
            service.reconcilePendingJobs();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const auto finals = service.loadFinalStatuses();
        if (finals.size() != 1 || finals[0].stage != "failed" ||
            finals[0].message.find("before runtime change") == std::string::npos)
            throw std::runtime_error("baseline denial requires cancelled, unchanged final");
        bool duplicate = false;
        try { service.rejectDuplicateRequest(request.jobId); }
        catch (const edge_gateway::OtaRequestRejected& error) {
            duplicate = error.reason() == edge_gateway::OtaRejectionReason::Duplicate;
        }
        if (!duplicate) throw std::runtime_error("processed handoff must not be replayed");
        if (std::ifstream(root + "/staging/current_version.txt").good() ||
            std::ifstream(root + "/staging/applied_jobs.txt").good())
            throw std::runtime_error("denied handoff changed installation state");
        const auto healthPath = config.stagingDir + "/health_" + request.jobId + ".txt";
        const auto finalPath = config.stagingDir + "/ota_final_" + request.jobId + ".txt";
        const auto read = [](const std::string& path) {
            std::ifstream stream(path);
            return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        };
        const auto originalHealth = read(healthPath), originalFinal = read(finalPath);
        const auto handoffPath = config.stagingDir + "/ota_handoff_" + request.jobId + ".json";
        const auto originalHandoff = read(handoffPath);
        if (originalHandoff.find("\"owner\":") == std::string::npos)
            throw std::runtime_error("native handoff never reached an independent systemd owner");
        for (const auto& damaged : {originalHandoff.substr(0, originalHandoff.size() - 1),
                                   "{\"runtimeMayChange\":false," + originalHandoff.substr(1)}) {
            std::ofstream(handoffPath) << damaged;
            if (!service.loadFinalStatuses().empty())
                throw std::runtime_error("cancelled handoff accepted incomplete/ambiguous proof");
        }
        std::ofstream(handoffPath) << originalHandoff;
        const auto token = finals[0].occurrenceId;
        for (const std::string rollback : {"none", "succeeded"}) {
            const auto healthy = rollback == "none";
            std::ofstream(healthPath) << "{\"format\":\"ota-health-v1\",\"jobId\":\"" << request.jobId
                << "\",\"version\":\"1.0\",\"token\":\"" << token << "\",\"status\":\""
                << (healthy ? "healthy" : "failed") << "\",\"rollback\":\"" << rollback << "\",\"ts\":\"1000\"}\n";
            service.reconcilePendingJobs();
            if (!service.loadFinalStatuses().empty())
                throw std::runtime_error("cancelled handoff accepted conflicting health decision");
        }
        std::ofstream(healthPath) << originalHealth;
        std::ofstream(finalPath) << "jobId=" << request.jobId << "\ntoken=" << token << "\nstatus=completed\n";
        if (!service.loadFinalStatuses().empty())
            throw std::runtime_error("cancelled handoff accepted conflicting completed final");
        std::ofstream(finalPath) << originalFinal;
        if (service.loadFinalStatuses().size() != 1)
            throw std::runtime_error("exact failed/unchanged decision must remain readable");
        // Production owner completion is a distinct bound proof, never CANCELLED.
        // These are private durable journal fixtures, not an actual runtime install.
        const auto jsonString = [](const std::string& value) {
            std::string escaped = "\"";
            for (const auto c : value) {
                if (c == '\n') escaped += "\\n";
                else if (c == '\"' || c == '\\') { escaped += '\\'; escaped += c; }
                else escaped += c;
            }
            return escaped + "\"";
        };
        for (const bool healthy : {true, false}) {
            auto productionConfig = config;
            productionConfig.stagingDir = root + (healthy ? "/promoted-journals" : "/recovered-journals");
            if (mkdir(productionConfig.stagingDir.c_str(), 0700)) throw std::runtime_error("cannot create production fixture");
            const auto staging = productionConfig.stagingDir;
            const std::string job = "PRODUCTION_FINISHED";
            const std::string tx = "OTA_" + edge_gateway::ota_query_detail::sha256Hex(job + token).substr(0, 32);
            const std::string result = healthy ? "healthy" : "failed", rollback = healthy ? "none" : "succeeded";
            const auto healthBytes = "{\"format\":\"ota-health-v1\",\"jobId\":\"" + job + "\",\"version\":\"1.1\",\"token\":\"" + token +
                "\",\"status\":\"" + result + "\",\"rollback\":\"" + rollback + "\",\"ts\":\"1000\"}\n";
            std::ofstream(staging + "/ota_job_" + job + ".lock");
            chmod((staging + "/ota_job_" + job + ".lock").c_str(), 0600);
            struct stat leaseInfo{};
            if (stat((staging + "/ota_job_" + job + ".lock").c_str(), &leaseInfo))
                throw std::runtime_error("cannot inspect production fixture lease");
            std::ofstream(staging + "/ota_job_" + job + ".json") <<
                "{\"format\":\"ota-job-v1\",\"jobId\":\"" << job << "\",\"machineCode\":\"LOCAL_ONLY\",\"version\":\"1.1\","
                "\"previousVersion\":\"1.0\",\"artifactPath\":\"/fixture.tar.gz\",\"token\":\"" << token << "\","
                "\"lifecycle\":\"systemd-handoff-v1\",\"artifactSha256\":\"" << std::string(64, 'a') <<
                "\",\"artifactSize\":\"1\",\"leaseDev\":\"" << leaseInfo.st_dev <<
                "\",\"leaseInode\":\"" << leaseInfo.st_ino << "\"}\n";
            std::ofstream(staging + "/ota_active_job.txt") << "jobId=" << job << "\ntoken=" << token << "\n";
            std::ofstream(staging + "/health_" + job + ".txt") << healthBytes;
            const auto record = "{\"format\":\"ota-handoff-v1\",\"jobId\":\"" + job + "\",\"version\":\"1.1\",\"token\":\"" + token +
                "\",\"phase\":\"OWNER_FINISHED\",\"runtimeMayChange\":true,\"productionTransaction\":\"" + tx +
                "\",\"completionProof\":{\"status\":\"" + result + "\",\"rollback\":\"" + rollback + "\",\"transaction\":\"" + tx +
                "\",\"healthSha256\":\"" + edge_gateway::ota_query_detail::sha256Hex(healthBytes) +
                "\",\"healthRecord\":" + jsonString(healthBytes) + "}}\n";
            const auto handoff = staging + "/ota_handoff_" + job + ".json";
            std::ofstream(handoff) << record;
            edge_gateway::OtaService production(productionConfig);
            production.reconcilePendingJobs();
            auto statuses = production.loadFinalStatuses();
            if (statuses.size() != 1 || statuses[0].stage != (healthy ? "completed" : "failed"))
                throw std::runtime_error("production completion/recovery proof did not reconcile original job");
            auto damaged = record;
            const auto bound = damaged.find("\"healthRecord\":");
            damaged.replace(bound, damaged.size() - bound, "\"healthRecord\":\"mismatched\"}}\n");
            std::ofstream(handoff) << damaged;
            if (!production.loadFinalStatuses().empty())
                throw std::runtime_error("production accepted unbound health bytes");
            std::ofstream(handoff) << record;
            bool productionDuplicate = false;
            try { production.rejectDuplicateRequest(job); }
            catch (const edge_gateway::OtaRequestRejected& error) {
                productionDuplicate = error.reason() == edge_gateway::OtaRejectionReason::Duplicate;
            }
            if (!productionDuplicate) throw std::runtime_error("production terminal job is not consumed");
        }
        std::cout << "C++ execute -> real systemd owner -> baseline deny -> cancellation -> durable final PASS\n";
        std::cout << "C++ production completed/recovered durable proof, conflicting health and duplicate PASS (journal fixture)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
