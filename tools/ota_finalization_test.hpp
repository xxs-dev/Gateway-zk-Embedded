// Included by ota_final_state_test.cpp; uses its isolated native fixtures.
void verifySuccessfulFinalization(const std::string& mode) {
    char pattern[] = "/tmp/ota-finalization-XXXXXX";
    const char* created = mkdtemp(pattern);
    require(created != nullptr, "cannot create finalization fixture");
    const std::string root = created;
    try {
        auto config = lifecycleConfig(root);
        config.rollbackScript = root + "/rollback.sh";
        writeFixtureFile(config.rollbackScript, "#!/bin/sh\nprintf 'rollback\\n' >> '" + root + "/calls'\n");
        const bool health = mode.find("health-") == 0;
        const bool existingFinal = mode.find("existing-final") == 0;
        const bool committed = mode == "order" || mode == "journal" || mode == "callback" ||
            mode == "crash" || existingFinal || mode == "health-journal";
        writeFixtureFile(root + "/source.tar.gz", "payload");
        std::string script = "#!/bin/sh\nset -eu\nprintf 'apply\\n' >> '" + root + "/calls'\n"
            "cp \"$5/ota_job_$3.json\" '" + root + "/own-intent'\n";
        if (health) script +=
            "printf '{\"format\":\"ota-health-v1\",\"jobId\":\"%s\",\"version\":\"%s\","
            "\"token\":\"%s\",\"status\":\"healthy\",\"rollback\":\"none\",\"ts\":\"1234\"}\\n' "
            "\"$3\" \"$2\" \"$6\" > \"$5/health_$3.txt\"\n";
        if (mode == "version" || mode == "health-version")
            script += "mkdir \"$5/current_version.txt\"\n";
        if (mode == "applied-version") script += "mkdir \"$5/applied_version.txt\"\n";
        if (mode == "intent") script += "printf '{broken\\n' > \"$5/ota_job_$3.json\"\n";
        if (mode == "identity") script +=
            "sed 's/\"version\":\"2.0.0\"/\"version\":\"9.0.0\"/' '" + root +
            "/own-intent' > \"$5/ota_job_$3.json\"\n";
        if (mode == "active") script += "printf 'wrong\\n' > \"$5/ota_active_job.txt\"\n";
        if (mode == "state") script += "mv \"$5/ota_state.lock\" \"$5/fixture-state-lock\"\nmkdir \"$5/ota_state.lock\"\n";
        if (mode == "journal" || mode == "health-journal")
            script += "mv \"$5/ota_status_pending.log\" \"$5/fixture-status\"\nmkdir \"$5/ota_status_pending.log\"\n";
        if (existingFinal) script +=
            "printf 'jobId=%s\\ntoken=%s\\nstatus=completed\\n' \"$3\" \"$6\" > \"$5/ota_final_$3.txt\"\n";
        if (mode == "existing-final-new-active")
            script += "printf 'jobId=NEWER_JOB\\ntoken=newer-token\\n' > \"$5/ota_active_job.txt\"\n";
        script += "exit 0\n";
        writeFixtureFile(config.applyScript, script);
        edge_gateway::OtaRequest request;
        request.jobId = "FINAL_JOB"; request.version = "2.0.0";
        request.artifactUrl = root + "/source.tar.gz";
        edge_gateway::OtaReply reply;
        edge_gateway::OtaStatus status;
        bool threw = false;
        bool observedFinal = false;
        std::vector<edge_gateway::OtaStatus> published;
        const auto execute = [&] {
#ifdef OTA_FINALIZATION_FAULT_TEST
            finalizationFault = mode == "final" || mode == "health-final" || existingFinal ? 1 :
                mode == "directory-sync" ? 2 : mode == "audit" ? 3 : 0;
#endif
            try {
                edge_gateway::OtaService(config).execute(request, "GW_TEST", 1000, &reply, &status,
                    [&](const edge_gateway::OtaStatus& value) {
                        if (value.stage == "completed") {
                            const auto own = edge_gateway::json::JsonParser(readFile(root + "/own-intent")).parse();
                            observedFinal = readFile(config.stagingDir + "/ota_final_FINAL_JOB.txt") ==
                                "jobId=FINAL_JOB\ntoken=" + own.find("token")->asString() + "\nstatus=completed\n";
                            if (mode == "crash") _exit(observedFinal ? 73 : 74);
                            if (mode == "callback") throw std::runtime_error("fixture notification fault https://secret.invalid/token");
                        }
                        published.push_back(value);
                    });
            } catch (const std::exception&) { threw = true; }
#ifdef OTA_FINALIZATION_FAULT_TEST
            finalizationFault = 0;
#endif
        };
        if (mode == "crash") {
            const pid_t child = fork();
            require(child >= 0, "cannot fork finalization crash fixture");
            if (child == 0) { execute(); _exit(75); }
            int childStatus = 0;
            require(waitpid(child, &childStatus, 0) == child && WIFEXITED(childStatus) &&
                WEXITSTATUS(childStatus) == 73, "crash before publication must leave committed final");
        } else {
            std::ostringstream diagnostic;
            auto* saved = mode == "audit" ? std::cerr.rdbuf(diagnostic.rdbuf()) : nullptr;
            execute();
            if (saved) {
                std::cerr.rdbuf(saved);
                require(diagnostic.str().find("audit NOT durable") != std::string::npos &&
                    diagnostic.str().find("stderr is best effort only") != std::string::npos,
                    "audit persistence failure must diagnose best-effort stderr honestly");
                std::cout << diagnostic.str();
            }
            if (mode == "order") require(observedFinal, "completed callback must observe its durable final first");
            if (existingFinal) require(!threw && status.stage == "completed",
                "synchronous final without health must stay completed");
            else require(threw == (mode != "order" && mode != "callback"), "successful apply fault must escape only as finalization error");
        }
        const auto own = edge_gateway::json::JsonParser(readFile(root + "/own-intent")).parse();
        if (!committed) require(countStage(edge_gateway::OtaService(config).loadPendingStatuses(), request.jobId, "completed") == 0 &&
            countStage(published, request.jobId, "completed") == 0,
            "uncommitted final must never journal or publish completed");
        const auto auditBytes = readFile(config.stagingDir + "/ota_reconcile_FINAL_JOB.json");
        if (mode == "audit") require(auditBytes.empty(), "injected audit failure must not claim durable evidence");
        if (existingFinal) require(auditBytes.empty(), "existing terminal must not be rewritten or poisoned by a newer owner");
        if (mode == "existing-final-new-active")
            require(readFile(config.stagingDir + "/ota_active_job.txt") == "jobId=NEWER_JOB\ntoken=newer-token\n",
                "old finalized execute must not overwrite newer active marker");
        if (mode != "order" && mode != "crash" && !existingFinal && mode != "audit" && mode != "callback") {
            require(!auditBytes.empty(), "successful apply finalization error needs durable audit");
            const auto audit = edge_gateway::json::JsonParser(auditBytes).parse();
            require(audit.find("jobId")->asString() == "FINAL_JOB" &&
                audit.find("token")->asString() == own.find("token")->asString() &&
                audit.find("version")->asString() == "2.0.0", "audit must bind immutable own identity");
            require(audit.find("finalState")->asString() == (committed ? "committed" : "unknown"),
                "audit must distinguish committed terminal from unknown finalization");
            require(auditBytes.find("secret.invalid") == std::string::npos && auditBytes.find(root) == std::string::npos,
                "audit must exclude artifact paths and exception secrets");
        }
        if (mode == "state") require(rmdir((config.stagingDir + "/ota_state.lock").c_str()) == 0 &&
            rename((config.stagingDir + "/fixture-state-lock").c_str(), (config.stagingDir + "/ota_state.lock").c_str()) == 0,
            "cannot restore fixture state lock for read assertions");
        if (mode == "journal" || mode == "health-journal")
            require(rmdir((config.stagingDir + "/ota_status_pending.log").c_str()) == 0,
                "cannot remove fixture journal fault");
        edge_gateway::OtaService restarted(config);
        const auto pending = restarted.loadPendingStatuses();
        require(countStage(pending, request.jobId, "failed") == 0 && countStage(published, request.jobId, "failed") == 0,
            "successful runtime apply must never become failed");
        if (!committed) require(countStage(pending, request.jobId, "completed") == 0 &&
            countStage(published, request.jobId, "completed") == 0,
            "uncommitted final must never journal or publish completed");
        require(countStage(restarted.loadFinalStatuses(), request.jobId, "completed") == (committed ? 1 : 0),
            "restart must reconstruct only committed successful final");
        if (committed) {
            writeFixtureFile(config.stagingDir + "/ota_status_pending.log", "");
            require(countStage(edge_gateway::OtaService(config).loadFinalStatuses(), request.jobId, "completed") == 1,
                "status journal loss must not erase committed final");
        }
        edge_gateway::OtaRequest next = request; next.jobId = "NEXT_JOB";
        std::string rejection;
        require(restarted.validateRequest(next, &rejection) == committed,
            "unknown finalization cannot admit another job");
        if (!committed) {
            bool nextRejected = false;
            try { restarted.execute(next, "GW_TEST", 1900, &reply, &status, nullptr); }
            catch (const std::exception&) { nextRejected = true; }
            require(nextRejected && readFile(config.stagingDir + "/ota_job_NEXT_JOB.json").empty(),
                "unknown finalization must reject actual execute before next intent");
        }
        if (!health) require(readFile(config.stagingDir + "/health_FINAL_JOB.txt").empty(),
            "synchronous finalization must not fabricate health success");
        bool duplicateRejected = false;
        try { restarted.execute(request, "GW_TEST", 2000, &reply, &status, nullptr); }
        catch (const std::exception&) { duplicateRejected = true; }
        require(duplicateRejected && readFile(root + "/calls") == "apply\n",
            "finalization faults must not authorize blind reapply");
        require(readFile(config.stagingDir + "/upgrade_history.log").find("result=failure") == std::string::npos,
            "successful apply must not append failure history");
        if (health && !committed) {
            if (mode == "health-version")
                require(rmdir((config.stagingDir + "/current_version.txt").c_str()) == 0, "cannot clear fixture version fault");
            if (mode == "health-final") {
                const auto active = readFile(config.stagingDir + "/ota_active_job.txt");
                writeFixtureFile(config.stagingDir + "/ota_active_job.txt", "jobId=NEWER_JOB\ntoken=newer-token\n");
                restarted.reconcilePendingJobs();
                require(restarted.loadFinalStatuses().empty(), "healthy audit repair still requires active ownership");
                writeFixtureFile(config.stagingDir + "/ota_active_job.txt", active);
                const auto healthPath = config.stagingDir + "/health_FINAL_JOB.txt";
                const auto healthyBytes = readFile(healthPath);
                auto wrongToken = healthyBytes;
                const auto token = own.find("token")->asString();
                wrongToken.replace(wrongToken.find(token), token.size(), "wrong-token");
                writeFixtureFile(healthPath, wrongToken);
                restarted.reconcilePendingJobs();
                require(restarted.loadFinalStatuses().empty(), "healthy audit repair requires matching token");
                writeFixtureFile(healthPath, healthyBytes);
                const int writer = open((config.stagingDir + "/ota_job_FINAL_JOB.lock").c_str(), O_RDWR);
                require(writer >= 0 && flock(writer, LOCK_EX | LOCK_NB) == 0, "cannot hold fixture lifecycle lease");
                restarted.reconcilePendingJobs();
                const bool blocked = restarted.loadFinalStatuses().empty();
                close(writer);
                require(blocked, "healthy audit repair must wait for the lifecycle writer");
            }
            restarted.reconcilePendingJobs();
            require(countStage(restarted.loadFinalStatuses(), request.jobId, "completed") == 1 &&
                restarted.validateRequest(next, &rejection), "trusted healthy recovery must survive a transient finalization fault");
            const auto repaired = edge_gateway::json::JsonParser(readFile(config.stagingDir + "/ota_reconcile_FINAL_JOB.json")).parse();
            require(repaired.find("finalState")->asString() == "committed" && readFile(root + "/calls") == "apply\n",
                "healthy recovery must update audit without reapply or rollback");
            std::cout << "finalization " << mode << ": recovered via matched health only\n";
        } else if (!committed && mode != "audit" && mode != "intent" && mode != "identity" && mode != "active") {
            writeFixtureFile(config.stagingDir + "/ota_barrier_FINAL_JOB.json",
                "{\"format\":\"official-ota-apply-barrier-v1\",\"jobId\":\"FINAL_JOB\",\"version\":\"2.0.0\","
                "\"token\":\"" + own.find("token")->asString() + "\",\"state\":\"prepared\"}\n");
            restarted.reconcilePendingJobs();
            require(countStage(restarted.loadFinalStatuses(), request.jobId, "failed") == 0 &&
                !restarted.validateRequest(next, &rejection) && readFile(root + "/calls") == "apply\n",
                "unknown no-health outcome must stay fenced without rollback or prepared failure");
        }
        std::cout << "finalization " << mode << ": initial-final=" << (committed ? "committed" : "unknown")
                  << " no false failed/reapply\n";
    } catch (...) { removeEventStoreTestTree(root); throw; }
    removeEventStoreTestTree(root);
}

void verifySuccessfulFinalizationCases() {
    for (const std::string mode : {"order", "final", "version", "applied-version", "intent", "identity", "active",
            "state", "audit", "journal", "callback", "crash", "existing-final", "existing-final-new-active",
            "health-final", "health-version", "health-journal", "directory-sync"})
        verifySuccessfulFinalization(mode);
}
