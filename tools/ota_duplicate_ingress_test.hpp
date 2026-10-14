class OtaPublisherFixture : public edge_gateway::IMqttDriverPublisher {
public:
    void publishFullSnapshot(const std::string&, const std::vector<edge_gateway::StoredPointValue>&, const std::string&) override {}
    void publishAlarm(const std::string&, std::uint32_t, const edge_gateway::StoredPointValue&, const std::string&, bool) override {}
    void publishOnDemand(const std::string&, const std::vector<edge_gateway::StoredPointValue>&, const std::string&) override {}
    void publishChangeEvent(const std::string&, const edge_gateway::StoredPointValue&) override {}
    void publishCommandReply(const std::string&, const edge_gateway::MqttCommandReply&) override {}
    void publishOtaReply(const std::string&, const edge_gateway::OtaReply& reply) override {
        std::lock_guard<std::mutex> guard(mutex);
        replies.push_back(reply);
        if (reply.accepted && throwAcceptedReply.exchange(false)) throw std::runtime_error("injected accepted reply failure");
    }
    void publishOtaStatus(const std::string&, const edge_gateway::OtaStatus& status) override {
        std::lock_guard<std::mutex> guard(mutex);
        statuses.push_back(status);
    }
    void publishJsonMessage(const std::string&, const std::string& payload) override {
        std::lock_guard<std::mutex> guard(mutex);
        audits.push_back(payload);
        if (throwAudit) throw std::runtime_error("injected audit publication failure");
    }
    std::vector<edge_gateway::MqttIncomingMessage> pollIncoming(int) override {
        auto result = incoming;
        incoming.clear();
        return result;
    }
    int negativeReplies(const std::string& jobId) {
        std::lock_guard<std::mutex> guard(mutex);
        int count = 0;
        for (const auto& reply : replies) if (reply.jobId == jobId && !reply.accepted) ++count;
        return count;
    }
    int failedStatuses(const std::string& jobId) {
        std::lock_guard<std::mutex> guard(mutex);
        return countStage(statuses, jobId, "failed");
    }
    std::mutex mutex;
    std::atomic<bool> throwAudit{false};
    std::atomic<bool> throwAcceptedReply{false};
    std::vector<edge_gateway::OtaReply> replies;
    std::vector<edge_gateway::OtaStatus> statuses;
    std::vector<std::string> audits;
    std::vector<edge_gateway::MqttIncomingMessage> incoming;
};

int countDirectStatus(const std::string& bytes, const std::string& jobId, const std::string& stage) {
    std::istringstream rows(bytes);
    std::string row;
    int count = 0;
    while (std::getline(rows, row)) {
        try {
            const auto value = edge_gateway::ota_request_json::parseRoot(row);
            if (edge_gateway::ota_request_json::stringField(value, "jobId") == jobId &&
                edge_gateway::ota_request_json::stringField(value, "machineCode") == "GW_TEST" &&
                edge_gateway::ota_request_json::stringField(value, "stage") == stage) ++count;
        } catch (const std::exception&) {
            // The asynchronous worker may still be writing the last JSONL row.
        }
    }
    return count;
}

void verifyOtaRootParsing() {
    using namespace edge_gateway::ota_request_json;
    const auto root = parseRoot("{\"nested\":{\"jobId\":\"FAKE\",\"machineCode\":\"WRONG\"},"
                                "\"jobId\":\"ROOT\",\"machineCode\":\"GW_TEST\"}");
    const auto parsed = request(root);
    require(parsed.jobId == "ROOT" && parsed.machineCode == "GW_TEST", "OTA identity must use root fields");
    require(request(parseRoot("{}")).jobId.empty(), "OTA root parser must not invent an ID");
    for (const auto& bad : {
            "{\"jobId\":\"A\",\"jobId\":\"B\"}", "{\"machineCode\":\"A\",\"machineCode\":\"B\"}",
            "{\"jobId\":42}", "{\"machineCode\":{}}", "[]",
            "{\"size\":9007199254740992}", "{\"size\":9007199254740993}",
            "{\"ts\":9007199254740992}", "{\"ts\":-9007199254740993}"}) {
        bool rejected = false;
        try { (void)request(parseRoot(bad)); } catch (const std::exception&) { rejected = true; }
        require(rejected, "ambiguous identity or unsafe integer must reject");
    }
    const auto boundary = request(parseRoot("{\"size\":9007199254740991,\"ts\":-9007199254740991}"));
    require(boundary.size == 9007199254740991ULL && boundary.ts == -9007199254740991LL,
            "safe integer boundary must retain exact value");
    std::cout << "OTA root identity/type/duplicate-key and safe-integer boundaries passed\n";
}

void verifyPreIntentFailureRetainsLease() {
    char pattern[] = "/tmp/ota-failure-reentry-XXXXXX";
    const char* created = mkdtemp(pattern);
    require(created != nullptr, "cannot create failure reentry fixture");
    const std::string root = created;
    try {
        edge_gateway::OtaConfig config;
        config.enabled = true;
        config.downloadDir = root + "/downloads";
        config.stagingDir = root + "/staging";
        config.backupDir = root + "/backup";
        config.checksumRequired = false;
        config.requireSignature = false;
        config.minFreeBytes = 0;
        edge_gateway::OtaRequest request;
        request.jobId = "DOWNLOAD_FAIL";
        request.version = "2.0.0";
        request.artifactUrl = root + "/missing.tar.gz";
        edge_gateway::OtaReply reply;
        edge_gateway::OtaStatus status;
        bool duplicate = false;
        bool threw = false;
        int failures = 0;
        try {
            edge_gateway::OtaService(config).execute(request, "GW_TEST", 1000, &reply, &status,
                [&](const edge_gateway::OtaStatus& event) {
                    if (event.stage != "failed") return;
                    ++failures;
                    edge_gateway::OtaReply nestedReply;
                    edge_gateway::OtaStatus nestedStatus;
                    try { edge_gateway::OtaService(config).execute(request, "GW_TEST", 2000, &nestedReply, &nestedStatus, nullptr); }
                    catch (const edge_gateway::OtaRequestRejected& ex) {
                        duplicate = ex.reason() == edge_gateway::OtaRejectionReason::Duplicate;
                    }
                });
        } catch (const std::exception&) { threw = true; }
        require(threw && duplicate && failures == 1 && status.stage == "failed",
                "pre-intent real failure must retain lease throughout failed callback");
        require(countStage(edge_gateway::OtaService(config).loadPendingStatuses(), request.jobId, "failed") == 1 &&
                readFile(config.stagingDir + "/ota_job_DOWNLOAD_FAIL.json").empty() &&
                !readFile(config.stagingDir + "/upgrade_history.log").empty(),
                "actual download failure must persist exactly one failed result without intent");
        std::cout << "pre-intent failed callback reentry: Duplicate; real download failure persisted once\n";
    } catch (...) { removeEventStoreTestTree(root); throw; }
    removeEventStoreTestTree(root);
}

void verifyOtaDuplicateIngress(bool directFirst, bool invalidDuplicate, bool publicationFailure = false,
                               bool lateDirectRefusal = false) {
    char pattern[] = "/tmp/ota-duplicate-ingress-XXXXXX";
    const char* created = mkdtemp(pattern);
    require(created != nullptr, "cannot create duplicate ingress fixture");
    const std::string root = created;
    struct PathGuard {
        std::string old;
        ~PathGuard() { setenv("PATH", old.c_str(), 1); }
    } pathGuard{std::getenv("PATH") ? std::getenv("PATH") : ""};
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
        config.packageType = "tar.gz";
        config.minFreeBytes = 0;
        config.upgradeTimeoutSec = 5;
        writeFixtureFile(root + "/source.tar.gz", "payload");
        if (lateDirectRefusal) {
            writeFixtureFile(root + "/curl", "#!/bin/sh\nprintf ready > '" + root + "/download-ready'\n"
                "while [ ! -f '" + root + "/download-release' ]; do sleep 0.01; done\n"
                "while [ $# -gt 0 ]; do if [ \"$1\" = -o ]; then shift; printf payload > \"$1\"; exit 0; fi; shift; done\nexit 1\n");
            require(chmod((root + "/curl").c_str(), 0755) == 0, "cannot enable fake curl");
            setenv("PATH", (root + ":" + pathGuard.old).c_str(), 1);
        }
        writeFixtureFile(config.applyScript,
            "#!/bin/sh\nprintf 'called\\n' >> '" + root + "/apply-calls'\n"
            "while [ ! -f '" + root + "/release' ]; do sleep 0.01; done\nexit 0\n");
        edge_gateway::SystemMonitorConfig::DirectMaintenanceConfig direct;
        direct.otaAppConfigFile = root + "/app.json";
        direct.identityConfigFile = root + "/identity.json";
        direct.otaStatusFile = root + "/direct-status.jsonl";
        direct.credentialFile = root + "/credential.json";
        direct.authStateFile = root + "/auth.json";
        namespace dm = edge_gateway::system_monitor_direct_maintenance;
        std::string error;
        require(dm::provisionCredential(direct, "Maint#2026-secret", &error), "cannot provision test credential");
        const auto password = dm::passwordSha256Hex("Maint#2026-secret");
        require(dm::handleHttpRequest(direct, "POST", "/api/v1/auth/bootstrap-login", "", "",
                    "{\"passwordSha256\":\"" + password + "\"}", "127.0.0.1").find("HTTP/1.1 200") == 0,
                "cannot authorize test Direct session");
        writeFixtureFile(direct.identityConfigFile, "{\"machineCode\":\"GW_TEST\"}\n");
        writeFixtureFile(direct.otaAppConfigFile,
            "{\"ota\":{\"enabled\":true,\"requireSignature\":false,\"checksumRequired\":false,"
            "\"minFreeBytes\":0,\"upgradeTimeoutSec\":5,\"downloadDir\":\"" + config.downloadDir +
            "\",\"stagingDir\":\"" + config.stagingDir + "\",\"backupDir\":\"" + config.backupDir +
            "\",\"applyScript\":\"" + config.applyScript + "\",\"rollbackScript\":\"\"}}\n");
        auto publisher = std::make_shared<OtaPublisherFixture>();
        publisher->throwAudit.store(publicationFailure);
        publisher->throwAcceptedReply.store(publicationFailure);
        edge_gateway::PointStoreRouter router;
        edge_gateway::MqttConfig mqtt;
        mqtt.enabled = true;
        mqtt.clientId = "GW_TEST";
        mqtt.statusTopic = "test/audit";
        mqtt.otaReplyTopic = "test/reply";
        mqtt.otaStatusTopic = "test/ota-status";
        edge_gateway::MqttDriverConfig driver;
        driver.publishFullOnStart = false;
        driver.priorityControlLeaseFile = root + "/priority.json";
        edge_gateway::DeviceConfig device;
        device.machineCode = "GW_TEST";
        edge_gateway::MqttDriverService service(mqtt, driver, {device}, router, publisher, nullptr,
                                               std::unique_ptr<edge_gateway::OtaService>(new edge_gateway::OtaService(config)));
        const auto payload = [&](const std::string& id, bool invalid) {
            return "{\"passwordSha256\":\"" + password + "\",\"machineCode\":\"GW_TEST\",\"jobId\":\"" + id +
                "\",\"packageType\":\"config\",\"version\":\"2.0.0\",\"artifactUrl\":\"" +
                (invalid ? "" : (id == "LATE_DIRECT" ? "https://example.invalid/late.tar.gz" : root + "/source.tar.gz")) +
                "\",\"sha256\":\"" + (invalid ? "bad" : "") + "\"}";
        };
        const auto sendMqtt = [&](const std::string& body) {
            edge_gateway::MqttIncomingMessage message;
            message.type = edge_gateway::MqttIncomingType::OtaRequest;
            message.payload = body;
            publisher->incoming.push_back(message);
            service.runScanOnce(1000);
        };
        const auto sendDirect = [&](const std::string& body) {
            return dm::handleHttpRequest(direct, "POST", "/api/v1/ota/jobs", "", "", body, "127.0.0.1");
        };
        if (lateDirectRefusal) {
            require(sendDirect(payload("LATE_DIRECT", false)).find("HTTP/1.1 202") == 0, "late Direct request not queued");
            for (int i = 0; i < 500 && readFile(root + "/download-ready").empty(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (directFirst) require(sendDirect(payload("SHARED", false)).find("HTTP/1.1 202") == 0, "Direct owner not queued");
        else sendMqtt(payload("SHARED", false));
        for (int i = 0; i < 500 && readFile(root + "/apply-calls").empty(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const bool started = readFile(root + "/apply-calls") == "called\n";
        if (lateDirectRefusal) {
            // The fake downloader is gated; no network requests are made.
            writeFixtureFile(root + "/download-release", "release\n");
            for (int i = 0; i < 500 && countDirectStatus(readFile(direct.otaStatusFile), "LATE_DIRECT", "failed") == 0; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto intent = readFile(config.stagingDir + "/ota_job_SHARED.json");
        const auto history = readFile(config.stagingDir + "/upgrade_history.log");
        const auto failure = readFile(config.stagingDir + "/upgrade_failure.log");
        sendMqtt(payload("SHARED", invalidDuplicate));
        sendMqtt("{\"nested\":{\"jobId\":\"FAKE\",\"machineCode\":\"WRONG\"}," +
                 payload("SHARED", true).substr(1));
        sendMqtt("{\"jobId\":\"SHARED\"," + payload("SHARED", false).substr(1));
        const auto directRejection = sendDirect(payload("SHARED", invalidDuplicate));
        sendMqtt(payload("OTHER", true));
        sendMqtt(payload("DIFFERENT_BUSY", false));
        const auto directStatuses = readFile(direct.otaStatusFile);
        const bool rejectedOnly = publisher->negativeReplies("SHARED") == 0 && publisher->failedStatuses("SHARED") == 0 &&
            (lateDirectRefusal ? (countDirectStatus(directStatuses, "LATE_DIRECT", "failed") == 1 &&
                                  countStage(edge_gateway::OtaService(config).loadPendingStatuses(), "LATE_DIRECT", "failed") == 1 &&
                                  readFile(config.stagingDir + "/ota_job_LATE_DIRECT.json").empty()) :
                                 directStatuses.find("\"stage\":\"failed\"") == std::string::npos) &&
            directRejection.find("HTTP/1.1 400") == 0 && publisher->negativeReplies("OTHER") == 1 &&
            publisher->negativeReplies("DIFFERENT_BUSY") == 1 && publisher->failedStatuses("DIFFERENT_BUSY") == 0 &&
            publisher->failedStatuses("OTHER") == 0 &&
            readFile(config.stagingDir + "/ota_job_SHARED.json") == intent &&
            readFile(config.stagingDir + "/upgrade_history.log") == history &&
            readFile(config.stagingDir + "/upgrade_failure.log") == failure;
        writeFixtureFile(root + "/release", "release\n");
        for (int i = 0; i < 500 && readFile(config.stagingDir + "/ota_final_SHARED.txt").empty(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        // Wait for the MQTT worker's completion audit before destroying it.
        if (!directFirst) {
            for (int i = 0; i < 500; ++i) {
                bool completed = false;
                { std::lock_guard<std::mutex> guard(publisher->mutex);
                  for (const auto& audit : publisher->audits)
                      if (audit.find("ota-completed") != std::string::npos) completed = true; }
                if (completed) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            service.start();
            service.stop();
        }
        require(started && !intent.empty(), "ingress owner did not execute");
        require(rejectedOnly, "in-flight duplicate must not emit negative reply or failed task state/history");
        require(!readFile(config.stagingDir + "/ota_final_SHARED.txt").empty(), "owner did not complete");
        sendMqtt(payload("SHARED", invalidDuplicate));
        require(publisher->negativeReplies("SHARED") == 0 && publisher->failedStatuses("SHARED") == 0,
                "historical duplicate must not overwrite original completed result");
        require(readFile(root + "/apply-calls") == "called\n", "duplicate ran apply again");
        if (publicationFailure) {
            sendMqtt(payload("AFTER_AUDIT_FAILURE", false));
            service.start();
            service.stop();
            require(!readFile(config.stagingDir + "/ota_final_AFTER_AUDIT_FAILURE.txt").empty() &&
                    publisher->failedStatuses("AFTER_AUDIT_FAILURE") == 0 &&
                    publisher->negativeReplies("AFTER_AUDIT_FAILURE") == 0,
                    "audit/reply exception must not fail owner or leave MQTT busy");
        }
        if (directFirst && !invalidDuplicate) {
            const auto defaults = "{\"passwordSha256\":\"" + password +
                "\",\"jobId\":\"DIRECT_DEFAULTS\",\"artifactUrl\":\"" + root +
                "/source.tar.gz\",\"version\":\"3.0.0\"}";
            require(sendDirect(defaults).find("HTTP/1.1 202") == 0, "Direct omitted defaults rejected");
            for (int i = 0; i < 500 && readFile(config.stagingDir + "/ota_final_DIRECT_DEFAULTS.txt").empty(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            require(!readFile(config.stagingDir + "/ota_final_DIRECT_DEFAULTS.txt").empty(),
                    "Direct omitted package/size/upgradeMode defaults must actually execute");
        }
        std::cout << "duplicate ingress: owner=" << (directFirst ? "Direct" : "MQTT")
                  << " invalid=" << invalidDuplicate << " in-flight/history suppressed; different invalid ID replied\n";
        if (lateDirectRefusal) std::cout << "Direct post-accepted distinct-job refusal: own durable failed; original task completed\n";
    } catch (...) {
        removeEventStoreTestTree(root);
        throw;
    }
    removeEventStoreTestTree(root);
}
