#include "edge_gateway/ota_service.hpp"
#include "edge_gateway/json_value.hpp"
#include "edge_gateway/system_monitor_direct_maintenance.hpp"

#include <fstream>
#include <iostream>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <dirent.h>
#include <cerrno>
#include <signal.h>
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
void verifyOfficialEarlyFailure(const std::string& root, const std::string& mode) {
    const bool missingPython = mode == "missing-python";
    const bool natural = mode == "invalid-manifest" || mode == "missing-helper";
    const bool changed = mode == "scada-marker-kill" || mode == "manifest-marker-kill";
    const bool recoverable = missingPython || natural || mode == "early-kill";
    edge_gateway::OtaConfig config;
    config.enabled = true;
    config.downloadDir = root + "/downloads";
    config.stagingDir = root + "/staging";
    config.backupDir = root + "/backup";
    config.applyScript = "/opt/modbus-gateway/bin/ota-apply.sh";
    config.rollbackScript.clear();
    config.checksumRequired = false;
    config.requireSignature = false;
    config.packageType = "tar.gz";
    config.minFreeBytes = 0;
    config.upgradeTimeoutSec = 8;
    edge_gateway::OtaRequest request;
    request.jobId = "EARLY_JOB";
    request.version = "2.0.0";
    request.artifactUrl = root + "/source.tar.gz";
    if (mode == "scada-marker-kill") {
        request.packageType = "scada";
        request.artifactUrl = root + "/source.kyscada";
    }
    edge_gateway::OtaReply reply;
    edge_gateway::OtaStatus status;
    bool failed = false;
    std::thread worker([&] {
        try { edge_gateway::OtaService(config).execute(request, "GW_TEST", 1000, &reply, &status, nullptr); }
        catch (const std::exception&) { failed = true; }
    });
    if (!missingPython && !natural) {
        const auto ready = root + "/apply.pgid";
        for (int i = 0; i < 500 && readFile(ready).empty(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto pgid = readFile(ready);
        if (pgid.empty()) { worker.join(); throw std::runtime_error("official kill fixture did not reach copy gate"); }
        if (kill(-std::stoi(pgid), SIGKILL) != 0) {
            worker.join(); throw std::runtime_error("cannot kill official apply group");
        }
    }
    worker.join();
    require(failed, "official early failure must reject apply");
    const auto intent = readFile(config.stagingDir + "/ota_job_EARLY_JOB.json");
    std::cout << "official " << mode
              << ": intent=" << !intent.empty()
              << " health=" << !readFile(config.stagingDir + "/health_EARLY_JOB.txt").empty()
              << " marker=" << !readFile(config.stagingDir + "/ota-in-progress").empty() << '\n';
    edge_gateway::OtaService restarted(config);
    restarted.reconcilePendingJobs();
    edge_gateway::OtaRequest next = request;
    next.jobId = "NEXT_JOB";
    std::string rejection;
    require(restarted.validateRequest(next, &rejection) == recoverable,
            "official failure must recover only with sufficient prepared lifecycle proof");
    if (!recoverable) {
        require(readFile(config.stagingDir + "/ota_final_EARLY_JOB.txt").empty(), "uncertain official window cannot finalize");
        const auto barrier = readFile(config.stagingDir + "/ota_barrier_EARLY_JOB.json");
        if (changed) {
            const auto document = edge_gateway::json::JsonParser(barrier, 4, 32).parse();
            require(document.find("state")->asString() == "runtime-may-change", "both live-write paths require changed barrier before marker");
            require(!readFile(config.stagingDir + "/ota-in-progress").empty(), "after-marker gate did not reach marker");
        }
        if (mode == "registration-kill") require(barrier.empty(), "registration gate must stop before prepared proof");
        if (mode == "pending-kill") require(!readFile(config.stagingDir + "/EARLY_JOB/health_pending.txt").empty(),
                                            "pending gate must preserve async-pending evidence");
        require(readFile("/opt/modbus-gateway/config/lifecycle-sentinel.txt") == "unchanged\n", "blocked fixture unexpectedly changed sentinel");
        std::cout << "official " << mode << ": remains blocked\n";
        return;
    }
    if (missingPython) require(intent.empty(), "missing dependency must be rejected before intent");
    else require(countStage(restarted.loadFinalStatuses(), request.jobId, "failed") == 1,
                 "official early kill needs a durable failed terminal");
    if (natural) require(access((config.stagingDir + "/EARLY_JOB/health_pending.txt").c_str(), F_OK) != 0,
                         "normal official rejection must remove disposable pending health");
    require(readFile("/opt/modbus-gateway/config/lifecycle-sentinel.txt") == "unchanged\n",
            "official early failure changed runtime");
    // Repair only the dependency/copy gate, then exercise the real service and
    // official script again. The deliberately malformed archive rejects safely.
    std::remove((root + "/shims/python3").c_str());
    std::remove((root + "/shims/cp").c_str());
    if (mode == "missing-helper") {
        std::ofstream restored("/opt/modbus-gateway/bin/ota-health-result.py");
        restored << readFile(root + "/helper-held.py");
        require(restored.good(), "cannot restore helper across isolated mounts");
    }
    bool nextFailed = false;
    try { restarted.execute(next, "GW_TEST", 2000, &reply, &status, nullptr); }
    catch (const std::exception&) { nextFailed = true; }
    require(nextFailed && countStage(restarted.loadFinalStatuses(), next.jobId, "failed") == 1,
            "repaired dependency must let the next actual official execute reach a safe rejection");
    require(!readFile(config.stagingDir + "/ota_job_NEXT_JOB.json").empty(),
            "next actual execute did not persist its intent");
    std::cout << "official next actual execute: durable failed/unchanged, fresh intent\n";
}

void writeFixtureFile(const std::string& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
    require(output.good(), "cannot write lifecycle fixture");
}

edge_gateway::OtaConfig lifecycleConfig(const std::string& root) {
    edge_gateway::OtaConfig config;
    config.enabled = true;
    config.downloadDir = root + "/downloads";
    config.stagingDir = root + "/staging";
    config.backupDir = root + "/backup";
    config.applyScript = root + "/apply.sh";
    config.rollbackScript.clear();
    config.checksumRequired = false;
    config.requireSignature = false;
    config.packageType = "tar.gz";
    config.minFreeBytes = 0;
    return config;
}

std::string fixtureBarrier(const std::string& job, const std::string& token, const std::string& state = "prepared") {
    return "{\"format\":\"official-ota-apply-barrier-v1\",\"jobId\":\"" + job +
        "\",\"version\":\"2.0.0\",\"token\":\"" + token + "\",\"state\":\"" + state + "\"}\n";
}

void verifyLifecycleEvidenceGuards() {
    for (const std::string variant : {"prepared", "missing-barrier", "damaged-barrier", "wrong-token",
            "changed-barrier", "pending-health", "burned", "damaged-applied", "unreadable-applied",
            "marker", "missing-lock", "old-intent", "damaged-health"}) {
        char pattern[] = "/tmp/ota-lifecycle-evidence-XXXXXX";
        const char* created = mkdtemp(pattern);
        require(created != nullptr, "cannot create lifecycle evidence fixture");
        const std::string root = created;
        try {
            auto config = lifecycleConfig(root);
            require(mkdir(config.stagingDir.c_str(), 0700) == 0, "cannot create staging");
            const std::string token = "00000000-0000-4000-8000-000000000001";
            writeFixtureFile(config.stagingDir + "/ota_job_JOB.json",
                "{\"format\":\"ota-job-v1\",\"jobId\":\"JOB\",\"machineCode\":\"GW_TEST\","
                "\"version\":\"2.0.0\",\"artifactPath\":\"/fixture.tar.gz\",\"previousVersion\":\"1.0.0\","
                "\"token\":\"" + token + "\"" + (variant == "old-intent" ? "" : ",\"lifecycle\":\"inherited-flock-v1\"") + "}\n");
            writeFixtureFile(config.stagingDir + "/ota_active_job.txt", "jobId=JOB\ntoken=" + token + "\n");
            if (variant != "missing-lock") writeFixtureFile(config.stagingDir + "/ota_job_JOB.lock", "");
            if (variant != "missing-barrier") writeFixtureFile(config.stagingDir + "/ota_barrier_JOB.json",
                variant == "damaged-barrier" ? "{broken\n" : fixtureBarrier("JOB", variant == "wrong-token" ? "wrong" : token,
                    variant == "changed-barrier" ? "runtime-may-change" : "prepared"));
            if (variant == "pending-health") {
                require(mkdir((config.stagingDir + "/JOB").c_str(), 0700) == 0, "cannot create job dir");
                writeFixtureFile(config.stagingDir + "/JOB/health_pending.txt", "pending\n");
            }
            if (variant == "marker") writeFixtureFile(config.stagingDir + "/ota-in-progress", "marker\n");
            if (variant == "burned" || variant == "damaged-applied")
                writeFixtureFile(config.stagingDir + "/applied_jobs.txt", variant == "burned" ? "JOB\n" : "partial");
            if (variant == "unreadable-applied")
                require(mkdir((config.stagingDir + "/applied_jobs.txt").c_str(), 0700) == 0, "cannot create non-file evidence");
            if (variant == "damaged-health") writeFixtureFile(config.stagingDir + "/health_JOB.txt", "{broken\n");
            edge_gateway::OtaService service(config);
            service.reconcilePendingJobs();
            edge_gateway::OtaRequest next;
            next.jobId = "NEXT_JOB"; next.version = "3.0.0"; next.artifactUrl = "/fixture.tar.gz";
            std::string rejection;
            require(service.validateRequest(next, &rejection) == (variant == "prepared"),
                    "only exact prepared lifecycle proof may unlock missing-result intent");
            std::cout << "lifecycle evidence " << variant << ": " << (variant == "prepared" ? "recovered" : "blocked") << '\n';
        } catch (...) { removeEventStoreTestTree(root); throw; }
        removeEventStoreTestTree(root);
    }
}

void verifyIntentForkLease() {
    char pattern[] = "/tmp/ota-lifecycle-handoff-XXXXXX";
    const char* created = mkdtemp(pattern);
    require(created != nullptr, "cannot create lifecycle handoff fixture");
    const std::string root = created;
    try {
        const auto config = lifecycleConfig(root);
        writeFixtureFile(root + "/source.tar.gz", "payload");
        writeFixtureFile(config.applyScript, "#!/bin/sh\nexit 4\n");
        edge_gateway::OtaRequest request;
        request.jobId = "JOB"; request.version = "2.0.0"; request.artifactUrl = root + "/source.tar.gz";
        edge_gateway::OtaReply reply;
        edge_gateway::OtaStatus status;
        bool observed = false;
        try {
            edge_gateway::OtaService(config).execute(request, "GW_TEST", 1000, &reply, &status,
                [&](const edge_gateway::OtaStatus& progress) {
                    if (progress.stage != "applying") return;
                    const auto path = config.stagingDir + "/ota_job_JOB.lock";
                    const int probe = open(path.c_str(), O_RDWR | O_CLOEXEC);
                    require(probe >= 0, "missing pre-fork lease");
                    const int locked = flock(probe, LOCK_EX | LOCK_NB);
                    const auto error = errno;
                    struct stat expected{};
                    require(fstat(probe, &expected) == 0, "cannot inspect lease");
                    close(probe);
                    require(locked < 0 && (error == EWOULDBLOCK || error == EAGAIN), "intent-to-fork lease gap");
                    bool parentCloexec = false;
                    auto* directory = opendir("/proc/self/fd");
                    require(directory != nullptr, "cannot inspect parent descriptors");
                    while (const auto* entry = readdir(directory)) {
                        char* end = nullptr;
                        const int fd = static_cast<int>(std::strtol(entry->d_name, &end, 10));
                        if (!end || *end || fd < 0) continue;
                        struct stat candidate{};
                        if (fstat(fd, &candidate) == 0 && candidate.st_dev == expected.st_dev && candidate.st_ino == expected.st_ino)
                            parentCloexec = (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0;
                    }
                    closedir(directory);
                    require(parentCloexec, "parent lease must retain CLOEXEC");
                    edge_gateway::OtaService(config).reconcilePendingJobs();
                    require(readFile(config.stagingDir + "/health_JOB.txt").empty(), "reconciler raced pre-fork owner");
                    observed = true;
                });
        } catch (const std::exception&) {}
        require(observed, "handoff callback was not reached");
        edge_gateway::OtaService(config).reconcilePendingJobs();
        require(readFile(config.stagingDir + "/ota_final_JOB.txt").empty(), "unregistered custom failure must remain blocked");
        std::cout << "intent-to-fork: held before fork, parent CLOEXEC; unregistered custom failure blocked\n";
    } catch (...) { removeEventStoreTestTree(root); throw; }
    removeEventStoreTestTree(root);
}

void verifyLiveChildLease(bool existingHealth, bool timeout) {
    char pattern[] = "/tmp/ota-lifecycle-child-XXXXXX";
    const char* created = mkdtemp(pattern);
    require(created != nullptr, "cannot create child lifecycle fixture");
    const std::string root = created;
    int pgid = 0;
    try {
        auto config = lifecycleConfig(root);
        if (timeout) config.upgradeTimeoutSec = 1;
        writeFixtureFile(root + "/source.tar.gz", "payload");
        writeFixtureFile(config.applyScript,
            "#!/bin/sh\n"
            "printf '{\"format\":\"official-ota-apply-barrier-v1\",\"jobId\":\"%s\",\"version\":\"%s\",\"token\":\"%s\",\"state\":\"prepared\"}\\n' \"$3\" \"$2\" \"$6\" > \"$5/ota_barrier_$3.json\"\n"
            "ps -o pgid= -p $$ > \"$5/child.pgid\"\n"
            "(trap '' TERM; : > \"$5/child.ready\"; while [ ! -f \"$5/release-child\" ]; do sleep 0.05; done) &\n"
            "while [ ! -f \"$5/child.ready\" ]; do sleep 0.01; done\n" +
            std::string(existingHealth ?
            "printf '{\"format\":\"ota-health-v1\",\"jobId\":\"%s\",\"version\":\"%s\",\"token\":\"%s\",\"status\":\"failed\",\"rollback\":\"unchanged\",\"ts\":\"2000\"}\\n' \"$3\" \"$2\" \"$6\" > \"$5/health_$3.txt\"\n" : "") +
            (timeout ? "sleep 60\n" : "exit 4\n"));
        edge_gateway::OtaRequest request;
        request.jobId = "JOB"; request.version = "2.0.0"; request.artifactUrl = root + "/source.tar.gz";
        edge_gateway::OtaReply reply;
        edge_gateway::OtaStatus status;
        try { edge_gateway::OtaService(config).execute(request, "GW_TEST", 1000, &reply, &status, nullptr); }
        catch (const std::exception&) {}
        pgid = std::stoi(readFile(config.stagingDir + "/child.pgid"));
        edge_gateway::OtaService service(config);
        service.reconcilePendingJobs();
        require(readFile(config.stagingDir + "/ota_final_JOB.txt").empty(), "live child must block terminal even with unchanged health");
        edge_gateway::OtaRequest next = request; next.jobId = "NEXT_JOB";
        std::string rejection;
        require(!service.validateRequest(next, &rejection), "live child must block fresh job");
        writeFixtureFile(config.stagingDir + "/release-child", "release\n");
        for (int i = 0; i < 200 && readFile(config.stagingDir + "/ota_final_JOB.txt").empty(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            service.reconcilePendingJobs();
        }
        require(service.validateRequest(next, &rejection), "last child close must permit exact prepared recovery");
        std::cout << "parent " << (timeout ? "timeout" : "exit") << ", live child, health=" << existingHealth
                  << ": blocked until child closes inherited lease\n";
    } catch (...) {
        if (pgid > 0) kill(-pgid, SIGKILL);
        removeEventStoreTestTree(root);
        throw;
    }
    removeEventStoreTestTree(root);
}

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

void verifySynchronousRollback(bool succeed, bool wrongToken, bool pendingHelper,
                               bool concurrentRequest = false, bool damagedBarrier = false) {
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
          if (damagedBarrier) script << "printf '{broken\\n' > \"$5/ota_barrier_$3.json\"\n";
          if (pendingHelper) script << "mkdir -p \"$5/$3\"\nprintf 'jobId=%s\\nversion=%s\\ntoken=%s\\n' \"$3\" \"$2\" \"$6\" > \"$5/$3/health_pending.txt\"\n";
          script << "exit 4\n"; }
        { std::ofstream script(config.rollbackScript);
          script << "#!/bin/sh\nprintf 'called\\n' >> '" << root << "/rollback-calls.txt'\n";
          if (concurrentRequest) script << ": > '" << root << "/rollback.ready'\n"
                "while [ ! -f '" << root << "/rollback.release' ]; do sleep 0.01; done\n";
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
        std::thread worker([&] {
            try { edge_gateway::OtaService(config).execute(request, "GW_TEST", 1000, &reply, &status, nullptr); }
            catch (const std::exception&) {}
        });
        bool ready = true;
        bool rejected = true;
        if (concurrentRequest) {
            for (int i = 0; i < 500 && access((root + "/rollback.ready").c_str(), F_OK) != 0; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            ready = access((root + "/rollback.ready").c_str(), F_OK) == 0;
            edge_gateway::OtaRequest racing = request; racing.jobId = "RACING_JOB";
            edge_gateway::OtaReply racingReply;
            edge_gateway::OtaStatus racingStatus;
            try {
                edge_gateway::OtaService(config).execute(racing, "GW_TEST", 1500, &racingReply, &racingStatus, nullptr);
                rejected = false;
            } catch (const std::exception&) {}
            writeFixtureFile(root + "/rollback.release", "release\n");
        }
        worker.join();
        require(ready && rejected, "request during rollback must be rejected before new intent");
        if (concurrentRequest) {
            require(readFile(config.stagingDir + "/ota_job_RACING_JOB.json").empty(), "rollback race must not admit intent");
            std::cout << "rollback in progress: next actual execute rejected\n";
        }
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

int main(int argc, char** argv) {
    try {
#ifndef _WIN32
        if (argc == 4 && std::string(argv[1]) == "--official-early-failure") {
            verifyOfficialEarlyFailure(argv[2], argv[3]);
            return 0;
        }
        verifyDurableHealthReconciliation();
        verifyLifecycleEvidenceGuards();
        verifyIntentForkLease();
        verifyLiveChildLease(false, false);
        verifyLiveChildLease(true, false);
        verifyLiveChildLease(true, true);
        verifySynchronousRejectionIsTerminal();
        verifySynchronousSuccessAndSameJobRace();
        verifySynchronousRollback(true, false, false);
        verifySynchronousRollback(true, true, false);
        verifySynchronousRollback(false, false, false);
        verifySynchronousRollback(true, false, true);
        verifySynchronousRollback(true, false, false, true);
        verifySynchronousRollback(true, false, false, false, true);
#endif
        std::cout << "ota final-state test passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "ota final-state test failed: " << ex.what() << '\n';
        return 1;
    }
}
