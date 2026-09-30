#include "edge_gateway/ota_service.hpp"
#include "edge_gateway/json_value.hpp"
#include "edge_gateway/system_monitor_direct_maintenance.hpp"

#include <fstream>
#include <iostream>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#include "event_store_test_files.hpp"
#endif

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

int countStage(const std::vector<edge_gateway::OtaStatus>& statuses, const std::string& jobId,
               const std::string& stage) {
    int count = 0;
    for (const auto& status : statuses) {
        if (status.jobId == jobId && status.stage == stage) ++count;
    }
    return count;
}

#ifndef _WIN32
void verifyDurableHealthReconciliation() {
    char pattern[] = "/tmp/ota-final-state-XXXXXX";
    const char* created = mkdtemp(pattern);
    require(created != nullptr, "cannot create OTA final-state fixture");
    const std::string root = created;
    try {
        edge_gateway::OtaConfig config;
        config.enabled = true;
        config.downloadDir = root + "/downloads";
        config.stagingDir = root + "/staging";
        config.backupDir = root + "/backup";
        config.applyScript = root + "/apply.sh";
        config.rollbackScript.clear();
        config.checksumRequired = false;
        config.requireSignature = false;
        config.currentVersion = "1.0.0";
        config.packageType = "tar.gz";
        config.minFreeBytes = 0;
        {
            std::ofstream artifact(root + "/source.tar.gz", std::ios::binary);
            artifact << "payload";
            std::ofstream script(config.applyScript);
            script << "#!/bin/sh\nmkdir -p \"$5/$3\"\n"
                      "printf 'jobId=%s\\nversion=%s\\ntoken=%s\\n' \"$3\" \"$2\" \"$6\" > \"$5/$3/health_pending.txt\"\n"
                      "if [ \"$3\" = RACE_JOB ]; then printf '{\"format\":\"ota-health-v1\",\"jobId\":\"RACE_JOB\",\"version\":\"4.0.0\",\"token\":\"%s\",\"status\":\"healthy\",\"rollback\":\"none\",\"ts\":\"5000\"}\\n' \"$6\" > \"$5/health_RACE_JOB.txt\"; sleep 1; fi\n";
        }
        edge_gateway::OtaRequest request;
        request.jobId = "HEALTH_JOB_1";
        request.version = "2.0.0";
        request.artifactUrl = root + "/source.tar.gz";
        edge_gateway::OtaReply reply;
        edge_gateway::OtaStatus status;
        edge_gateway::OtaService(config).execute(request, "GW_TEST", 1000, &reply, &status, nullptr);
        require(status.stage == "applying", "asynchronous apply must not report completed");
        require(readFile(config.stagingDir + "/current_version.txt").empty(),
                "pending health must not write success marker");
        require(readFile(config.stagingDir + "/applied_version.txt").empty(),
                "pending health must not write applied version");
        require(readFile(config.stagingDir + "/upgrade_history.log").find("result=success") == std::string::npos,
                "pending health must not write success history");

        const auto intentPath = config.stagingDir + "/ota_job_HEALTH_JOB_1.json";
        const auto intent = edge_gateway::json::JsonParser(readFile(intentPath), 4, 32).parse();
        const auto token = intent.find("token")->asString();
        const auto healthPath = config.stagingDir + "/health_HEALTH_JOB_1.txt";
        {
            std::ofstream stale(healthPath);
            stale << "{\"format\":\"ota-health-v1\",\"jobId\":\"HEALTH_JOB_1\","
                     "\"version\":\"2.0.0\",\"token\":\"wrong\","
                     "\"status\":\"healthy\",\"rollback\":\"none\",\"ts\":\"2000\"}\n";
        }
        edge_gateway::OtaService restarted(config);
        restarted.reconcilePendingJobs();
        require(countStage(restarted.loadPendingStatuses(), request.jobId, "completed") == 0,
                "stale token must not complete OTA");
        {
            std::ofstream truncated(healthPath, std::ios::trunc);
            truncated << "{\"jobId\":\"HEALTH_JOB_1\"";
        }
        restarted.reconcilePendingJobs();
        require(countStage(restarted.loadPendingStatuses(), request.jobId, "completed") == 0,
                "truncated health must not complete OTA");
        {
            std::ofstream healthy(healthPath, std::ios::trunc);
            healthy << "{\"format\":\"ota-health-v1\",\"jobId\":\"HEALTH_JOB_1\","
                       "\"version\":\"2.0.0\",\"token\":\"" << token << "\","
                       "\"status\":\"healthy\",\"rollback\":\"none\",\"ts\":\"2000\"}\n";
        }
        restarted.reconcilePendingJobs();
        const auto firstStatuses = restarted.loadPendingStatuses();
        require(countStage(firstStatuses, request.jobId, "completed") == 1,
                "healthy job-bound result must complete exactly once");
        require(readFile(config.stagingDir + "/current_version.txt").find("currentVersion=2.0.0") != std::string::npos,
                "healthy result must write version marker");
        require(readFile(config.stagingDir + "/applied_version.txt") == "2.0.0\n",
                "healthy result must write applied version");
        require(readFile(config.stagingDir + "/upgrade_history.log").find("result=success") != std::string::npos,
                "healthy result must write success history");
        edge_gateway::OtaService replay(config);
        replay.reconcilePendingJobs();
        require(countStage(replay.loadPendingStatuses(), request.jobId, "completed") == 1,
                "restart reconciliation must not duplicate terminal occurrence");

        replay.clearPendingStatuses();
        require(replay.loadPendingStatuses().empty(), "journal clear fixture failed");
        const auto appConfigPath = root + "/app.json";
        { std::ofstream app(appConfigPath);
          app << "{\"ota\":{\"stagingDir\":\"" << config.stagingDir << "\"}}\n"; }
        edge_gateway::SystemMonitorConfig::DirectMaintenanceConfig direct;
        direct.otaAppConfigFile = appConfigPath;
        direct.otaStatusFile = root + "/direct-status.jsonl";
        { std::ofstream oldStatus(direct.otaStatusFile);
          oldStatus << "{\"jobId\":\"HEALTH_JOB_1\",\"stage\":\"applying\"}\n"; }
        const auto directReply = edge_gateway::system_monitor_direct_maintenance::handleHttpRequest(
            direct, "GET", "/api/v1/ota/status", "", "", "", "127.0.0.1");
        require(directReply.find("\"jobId\":\"HEALTH_JOB_1\"") != std::string::npos &&
                directReply.find("\"stage\":\"completed\"") != std::string::npos &&
                directReply.find("\"stage\":\"applying\"") == std::string::npos,
                "Direct must show durable terminal result after MQTT journal clear");
        require(replay.loadPendingStatuses().empty(), "Direct GET must not recreate MQTT journal");

        const auto finalPath = config.stagingDir + "/ota_final_HEALTH_JOB_1.txt";
        const auto goodFinal = readFile(finalPath);
        { std::ofstream corrupt(finalPath, std::ios::trunc); corrupt << "jobId=HEALTH_JOB_1\ntoken=wrong\n"; }
        edge_gateway::OtaRequest next = request;
        next.jobId = "HEALTH_JOB_2";
        next.version = "3.0.0";
        std::string rejection;
        require(!replay.validateRequest(next, &rejection), "invalid final must block next apply");
        replay.reconcilePendingJobs();
        require(countStage(replay.loadPendingStatuses(), request.jobId, "completed") == 0,
                "invalid final must not recreate terminal status");
        { std::ofstream truncated(finalPath, std::ios::trunc); truncated << "jobId=HEALTH_JOB_1\n"; }
        require(!replay.validateRequest(next, &rejection), "truncated final must block next apply");
        { std::ofstream restore(finalPath, std::ios::trunc); restore << goodFinal; }

        request = next;
        edge_gateway::OtaService(config).execute(request, "GW_TEST", 3000, &reply, &status, nullptr);
        next.jobId = "RACE_JOB";
        next.version = "4.0.0";
        require(!replay.validateRequest(next, &rejection), "unresolved job must block next apply");
        const auto failedIntent = edge_gateway::json::JsonParser(
            readFile(config.stagingDir + "/ota_job_HEALTH_JOB_2.json"), 4, 32).parse();
        {
            std::ofstream failed(config.stagingDir + "/health_HEALTH_JOB_2.txt");
            failed << "{\"format\":\"ota-health-v1\",\"jobId\":\"HEALTH_JOB_2\","
                      "\"version\":\"3.0.0\",\"token\":\"" << failedIntent.find("token")->asString()
                   << "\",\"status\":\"failed\",\"rollback\":\"succeeded\",\"ts\":\"4000\"}\n";
        }
        edge_gateway::OtaService(config).reconcilePendingJobs();
        const auto finalStatuses = edge_gateway::OtaService(config).loadPendingStatuses();
        require(countStage(finalStatuses, request.jobId, "failed") == 1,
                "failed health/rollback must publish one failed terminal status");
        require(readFile(config.stagingDir + "/current_version.txt").find("currentVersion=2.0.0") != std::string::npos,
                "failed job must not overwrite restored version metadata");
        require(readFile(config.stagingDir + "/applied_version.txt") == "2.0.0\n",
                "failed job must not overwrite applied version");

        edge_gateway::OtaStatus racingStatus;
        std::exception_ptr workerError;
        std::thread worker([&] {
            try { edge_gateway::OtaService(config).execute(next, "GW_TEST", 5000, &reply, &racingStatus, nullptr); }
            catch (...) { workerError = std::current_exception(); }
        });
        const auto raceHealth = config.stagingDir + "/health_RACE_JOB.txt";
        for (int i = 0; i < 200 && readFile(raceHealth).empty(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(!readFile(raceHealth).empty(), "race fixture did not create health result");
        edge_gateway::OtaService(config).reconcilePendingJobs();
        worker.join();
        if (workerError) std::rethrow_exception(workerError);
        const auto racedStatuses = replay.loadPendingStatuses();
        require(racingStatus.stage == "completed", "execute return must retain finalized status");
        require(countStage(racedStatuses, next.jobId, "completed") == 1,
                "race must append one terminal status");
        bool seenTerminal = false;
        for (const auto& item : racedStatuses) {
            if (item.jobId != next.jobId) continue;
            if (item.stage == "completed") seenTerminal = true;
            else if (seenTerminal && item.stage == "applying")
                throw std::runtime_error("execute appended applying after terminal");
        }
        // A late old result is ignored even after a newer version succeeds.
        { std::ofstream late(healthPath, std::ios::trunc);
          late << "{\"format\":\"ota-health-v1\",\"jobId\":\"HEALTH_JOB_1\",\"version\":\"2.0.0\",\"token\":\""
               << token << "\",\"status\":\"healthy\",\"rollback\":\"none\",\"ts\":\"6000\"}\n"; }
        require(std::remove(finalPath.c_str()) == 0, "cannot remove old final fixture");
        replay.reconcilePendingJobs();
        require(readFile(config.stagingDir + "/current_version.txt").find("currentVersion=4.0.0") != std::string::npos,
                "old result must not replace newer installed version");
        require(readFile(config.stagingDir + "/applied_version.txt") == "4.0.0\n",
                "old result must not replace newer applied version");
    } catch (...) {
        removeEventStoreTestTree(root);
        throw;
    }
    removeEventStoreTestTree(root);
}

void verifySynchronousRejectionIsTerminal() {
    char pattern[] = "/tmp/ota-sync-rejection-XXXXXX";
    const char* created = mkdtemp(pattern);
    require(created != nullptr, "cannot create synchronous OTA fixture");
    const std::string root = created;
    try {
        edge_gateway::OtaConfig config;
        config.enabled = true;
        config.downloadDir = root + "/downloads";
        config.stagingDir = root + "/staging";
        config.backupDir = root + "/backup";
        config.applyScript = root + "/apply.sh";
        config.rollbackScript.clear();
        config.checksumRequired = false;
        config.requireSignature = false;
        config.currentVersion = "1.0.0";
        config.packageType = "tar.gz";
        config.minFreeBytes = 0;
        { std::ofstream artifact(root + "/source.tar.gz"); artifact << "payload"; }
        { std::ofstream script(config.applyScript);
          script << "#!/bin/sh\n"
                    "[ \"$3\" != NEXT_JOB ] || exit 0\n"
                    "printf '{\"format\":\"ota-health-v1\",\"jobId\":\"%s\",\"version\":\"%s\",\"token\":\"%s\",\"status\":\"failed\",\"rollback\":\"unchanged\",\"ts\":\"2000\"}\\n' \"$3\" \"$2\" \"$6\" > \"$5/health_$3.txt\"\n"
                    "exit 4\n"; }
        edge_gateway::OtaRequest first;
        first.jobId = "SYNC_REJECTED";
        first.version = "2.0.0";
        first.artifactUrl = root + "/source.tar.gz";
        edge_gateway::OtaReply reply;
        edge_gateway::OtaStatus status;
        try { edge_gateway::OtaService(config).execute(first, "GW_TEST", 1000, &reply, &status, nullptr); }
        catch (const std::exception&) {}
        require(status.stage == "failed", "synchronous rejection must fail first job");
        edge_gateway::OtaRequest next = first;
        next.jobId = "NEXT_JOB";
        std::string rejection;
        require(edge_gateway::OtaService(config).validateRequest(next, &rejection),
                "proved unchanged rejection must allow a new job");
        require(!edge_gateway::OtaService(config).validateRequest(first, &rejection),
                "original job intent must not be replayed");
        edge_gateway::OtaService replay(config);
        replay.loadPendingStatuses();
        replay.clearPendingStatuses();
        const auto appConfigPath = root + "/app.json";
        { std::ofstream app(appConfigPath);
          app << "{\"ota\":{\"stagingDir\":\"" << config.stagingDir << "\"}}\n"; }
        edge_gateway::SystemMonitorConfig::DirectMaintenanceConfig direct;
        direct.otaAppConfigFile = appConfigPath;
        direct.otaStatusFile = root + "/direct-status.jsonl";
        const auto directReply = edge_gateway::system_monitor_direct_maintenance::handleHttpRequest(
            direct, "GET", "/api/v1/ota/status", "", "", "", "127.0.0.1");
        require(directReply.find("\"jobId\":\"SYNC_REJECTED\"") != std::string::npos &&
                directReply.find("\"stage\":\"failed\"") != std::string::npos,
                "Direct must show durable synchronous failure after MQTT journal clear");
        edge_gateway::OtaService(config).execute(next, "GW_TEST", 3000, &reply, &status, nullptr);
        require(status.stage == "completed", "new job must run after proved unchanged rejection");
    } catch (...) {
        removeEventStoreTestTree(root);
        throw;
    }
    removeEventStoreTestTree(root);
}

void verifySynchronousSuccessAndSameJobRace() {
    char pattern[] = "/tmp/ota-sync-success-XXXXXX";
    const char* created = mkdtemp(pattern);
    require(created != nullptr, "cannot create synchronous success fixture");
    const std::string root = created;
    try {
        edge_gateway::OtaConfig config;
        config.enabled = true;
        config.downloadDir = root + "/downloads";
        config.stagingDir = root + "/staging";
        config.backupDir = root + "/backup";
        config.applyScript = root + "/apply.sh";
        config.rollbackScript.clear();
        config.checksumRequired = false;
        config.requireSignature = false;
        config.currentVersion = "1.0.0";
        config.packageType = "tar.gz";
        config.minFreeBytes = 0;
        { std::ofstream artifact(root + "/source.tar.gz"); artifact << "payload"; }
        { std::ofstream script(config.applyScript); script << "#!/bin/sh\nexit 0\n"; }
        edge_gateway::OtaRequest request;
        request.jobId = "SHARED_JOB";
        request.version = "2.0.0";
        request.artifactUrl = root + "/source.tar.gz";
        std::mutex mutex;
        std::condition_variable changed;
        bool delayed = false;
        bool release = false;
        bool rejected = false;
        std::thread second([&] {
            edge_gateway::OtaReply reply;
            edge_gateway::OtaStatus status;
            try {
                edge_gateway::OtaService(config).execute(request, "GW_TEST", 1000, &reply, &status,
                    [&](const edge_gateway::OtaStatus& event) {
                        if (event.stage != "accepted") return;
                        std::unique_lock<std::mutex> guard(mutex);
                        delayed = true;
                        changed.notify_all();
                        changed.wait(guard, [&] { return release; });
                    });
            } catch (const std::exception&) { rejected = true; }
        });
        {
            std::unique_lock<std::mutex> guard(mutex);
            changed.wait(guard, [&] { return delayed; });
        }
        edge_gateway::OtaReply reply;
        edge_gateway::OtaStatus status;
        std::exception_ptr firstError;
        try { edge_gateway::OtaService(config).execute(request, "GW_TEST", 2000, &reply, &status, nullptr); }
        catch (...) { firstError = std::current_exception(); }
        const auto intentPath = config.stagingDir + "/ota_job_SHARED_JOB.json";
        const auto originalIntent = readFile(intentPath);
        {
            std::lock_guard<std::mutex> guard(mutex);
            release = true;
        }
        changed.notify_all();
        second.join();
        if (firstError) std::rethrow_exception(firstError);
        require(status.stage == "completed", "synchronous success must complete");
        require(rejected && readFile(intentPath) == originalIntent,
                "delayed duplicate must not replace a completed job intent");
        request.jobId = "NEXT_SUCCESS";
        request.version = "3.0.0";
        edge_gateway::OtaService(config).execute(request, "GW_TEST", 3000, &reply, &status, nullptr);
        require(status.stage == "completed", "synchronous success must allow next job");
    } catch (...) {
        removeEventStoreTestTree(root);
        throw;
    }
    removeEventStoreTestTree(root);
}

void verifySynchronousRollback(bool succeed, bool wrongToken, bool pendingHelper) {
    char pattern[] = "/tmp/ota-sync-rollback-XXXXXX";
    const char* created = mkdtemp(pattern);
    require(created != nullptr, "cannot create synchronous rollback fixture");
    const std::string root = created;
    try {
        edge_gateway::OtaConfig config;
        config.enabled = true;
        config.downloadDir = root + "/downloads";
        config.stagingDir = root + "/staging";
        config.backupDir = root + "/backup";
        config.applyScript = root + "/apply.sh";
        config.rollbackScript = root + "/rollback.sh";
        config.checksumRequired = false;
        config.requireSignature = false;
        config.currentVersion = "1.0.0";
        config.packageType = "tar.gz";
        config.minFreeBytes = 0;
        { std::ofstream artifact(root + "/source.tar.gz"); artifact << "payload"; }
        { std::ofstream runtime(root + "/runtime.txt"); runtime << "old\n"; }
        { std::ofstream script(config.applyScript);
          script << "#!/bin/sh\nprintf '%s\\n' \"$3\" > \"$5/applied_jobs.txt\"\n"
                    "printf 'jobId=%s\\nversion=%s\\nstagingDir=%s\\n' \"$3\" \"$2\" \"$5\" > \"$5/ota-in-progress\"\n"
                    "printf 'new\\n' > '" << root << "/runtime.txt'\n";
          if (pendingHelper) script << "mkdir -p \"$5/$3\"\nprintf 'jobId=%s\\nversion=%s\\ntoken=%s\\n' \"$3\" \"$2\" \"$6\" > \"$5/$3/health_pending.txt\"\n";
          script << "exit 4\n"; }
        { std::ofstream script(config.rollbackScript);
          script << "#!/bin/sh\nprintf 'called\\n' >> '" << root << "/rollback-calls.txt'\n";
          if (succeed) {
              script << "printf 'old\\n' > '" << root << "/runtime.txt'\nrm \"$5/ota-in-progress\"\n"
                        "printf 'jobId=%s\\nversion=%s\\ntoken=%s\\nstatus=succeeded\\n' \"$3\" \"$2\" "
                     << (wrongToken ? "wrong-token" : "\"$6\"") << " > \"$5/rollback_$3_proof.txt\"\nexit 0\n";
          } else {
              script << "printf 'status=succeeded\\nstatus=failed\\n' > \"$5/rollback_$3.txt\"\nexit 1\n";
          }
        }
        edge_gateway::OtaRequest request;
        request.jobId = "CHANGED_JOB";
        request.version = "2.0.0";
        request.artifactUrl = root + "/source.tar.gz";
        edge_gateway::OtaReply reply;
        edge_gateway::OtaStatus status;
        try { edge_gateway::OtaService(config).execute(request, "GW_TEST", 1000, &reply, &status, nullptr); }
        catch (const std::exception&) {}
        require(status.stage == "failed", "changed synchronous failure must report failed");
        require(readFile(root + "/runtime.txt") == (succeed && !pendingHelper ? "old\n" : "new\n"),
                "rollback fixture runtime mismatch");
        require(readFile(root + "/rollback-calls.txt").empty() == pendingHelper,
                "pending helper must retain rollback ownership");
        edge_gateway::OtaRequest next = request;
        next.jobId = "NEXT_JOB";
        std::string rejection;
        require(edge_gateway::OtaService(config).validateRequest(next, &rejection) ==
                (succeed && !wrongToken && !pendingHelper),
                "only matching proved rollback may allow next job");
        require(!edge_gateway::OtaService(config).validateRequest(request, &rejection),
                "changed original job must never replay");
        require(!readFile(config.stagingDir + "/ota_final_CHANGED_JOB.txt").empty() ==
                (succeed && !wrongToken && !pendingHelper),
                "rollback terminal marker must require matching proof");
        if (wrongToken) {
            const auto history = readFile(config.stagingDir + "/upgrade_history.log");
            require(history.find("rollbackSucceeded=false") != std::string::npos &&
                    history.find("rollbackSucceeded=true") == std::string::npos,
                    "wrong token must not be reported as a successful rollback");
        }
    } catch (...) {
        removeEventStoreTestTree(root);
        throw;
    }
    removeEventStoreTestTree(root);
}
#endif

}  // namespace

int main() {
    try {
#ifndef _WIN32
        verifyDurableHealthReconciliation();
        verifySynchronousRejectionIsTerminal();
        verifySynchronousSuccessAndSameJobRace();
        verifySynchronousRollback(true, false, false);
        verifySynchronousRollback(true, true, false);
        verifySynchronousRollback(false, false, false);
        verifySynchronousRollback(true, false, true);
#endif
        std::cout << "ota final-state test passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "ota final-state test failed: " << ex.what() << '\n';
        return 1;
    }
}
