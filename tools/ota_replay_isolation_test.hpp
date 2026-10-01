// Public MQTT start/scan entry regressions; no Broker or network transport.
class OtaReplayPublisherFixture : public OtaPublisherFixture {
public:
    void publishFullSnapshot(const std::string&, const std::vector<edge_gateway::StoredPointValue>& values,
                             const std::string&) override {
        if (values.size() == 1 && values.front().value == 42.0) ++full;
    }
    void publishOnDemand(const std::string&, const std::vector<edge_gateway::StoredPointValue>& values,
                         const std::string&) override {
        if (values.size() == 1 && values.front().value == 42.0) ++realtime;
    }
    void publishOtaStatus(const std::string& topic, const edge_gateway::OtaStatus& status) override {
        ++otaAttempts;
        if (throwOta) throw std::runtime_error("fixture OTA transport error secret-credential");
        OtaPublisherFixture::publishOtaStatus(topic, status);
        if (afterOta) afterOta();
    }
    void publishJsonMessage(const std::string& topic, const std::string& payload) override {
        const bool replay = payload.find("\"event\":\"ota-") != std::string::npos;
        if (replay) ++replayAudits;
        OtaPublisherFixture::publishJsonMessage(topic, payload);
        if (replay && throwReplayAudit) throw std::runtime_error("fixture audit secret-credential");
    }
    std::atomic<int> full{0}, realtime{0}, otaAttempts{0}, replayAudits{0};
    bool throwOta = false;
    bool throwReplayAudit = false;
    std::function<void()> afterOta;
};

std::string replayFixtureJournal() {
    return "{\"format\":\"ota-status-v2\",\"jobId\":\"HISTORY\",\"machineCode\":\"GW_TEST\","
        "\"stage\":\"completed\",\"progress\":\"100\",\"downloadedBytes\":\"0\",\"totalBytes\":\"0\","
        "\"message\":\"already complete\",\"ts\":\"1000\",\"occurrenceId\":\"00000000-0000-4000-8000-000000000001\"}\n";
}

void seedReplayHealthFault(const std::string& staging) {
    writeFixtureFile(staging + "/ota_job_RECOVER.json",
        "{\"format\":\"ota-job-v1\",\"jobId\":\"RECOVER\",\"machineCode\":\"GW_TEST\","
        "\"version\":\"2.0.0\",\"artifactPath\":\"/fixture.tar.gz\",\"previousVersion\":\"1.0.0\","
        "\"token\":\"00000000-0000-4000-8000-000000000002\",\"lifecycle\":\"inherited-flock-v1\"}\n");
    writeFixtureFile(staging + "/ota_active_job.txt", "jobId=RECOVER\ntoken=00000000-0000-4000-8000-000000000002\n");
    writeFixtureFile(staging + "/ota_job_RECOVER.lock", "");
    writeFixtureFile(staging + "/health_RECOVER.txt",
        "{\"format\":\"ota-health-v1\",\"jobId\":\"RECOVER\",\"version\":\"2.0.0\","
        "\"token\":\"00000000-0000-4000-8000-000000000002\",\"status\":\"healthy\",\"rollback\":\"none\",\"ts\":\"1000\"}\n");
    require(mkdir((staging + "/current_version.txt").c_str(), 0700) == 0, "cannot inject reconciliation write fault");
}

void verifyOtaReplayIsolation(const std::string& fault, bool startWorker) {
    char pattern[] = "/tmp/ota-replay-isolation-XXXXXX";
    const char* created = mkdtemp(pattern);
    require(created != nullptr, "cannot create replay isolation fixture");
    const std::string root = created;
    const auto shm = root.substr(5);
    try {
        const auto config = lifecycleConfig(root);
        require(mkdir(config.stagingDir.c_str(), 0700) == 0, "cannot create replay staging");
        const bool reconcileFault = fault == "reconcile" || fault == "reconcile-audit";
        if (reconcileFault) seedReplayHealthFault(config.stagingDir);
        const auto journal = config.stagingDir + "/ota_status_pending.log";
        if (fault == "read") require(mkdir(journal.c_str(), 0700) == 0, "cannot inject journal read fault");
        else writeFixtureFile(journal, replayFixtureJournal());

        edge_gateway::MemoryStoreConfig storeConfig;
        storeConfig.sharedMemoryName = shm;
        edge_gateway::MemoryPointStore store(storeConfig);
        edge_gateway::PointStoreRouter router;
        router.addStore(shm, store);
        edge_gateway::DeviceConfig device;
        device.machineCode = "GW_TEST";
        device.memoryStore.sharedMemoryName = shm;
        edge_gateway::LogicalDeviceConfig meter;
        meter.meterCode = "METER";
        edge_gateway::PointDefinition point;
        point.index = 1001; point.pointCode = "P1"; point.enabled = true; point.fullUpload = true;
        point.read.enable = true; point.read.dataType = "uint16"; point.read.intervalMs = 100;
        meter.points.push_back(point); device.meters.push_back(meter);
        router.addRoutesFromDeviceConfigs({device}, shm);
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        edge_gateway::PointValue value;
        value.index = 1001; value.value = 42.0; value.ts = now; value.expireAt = now + 600000;
        require(router.putLatestByIndex(value).accepted, "cannot seed telemetry value");
        edge_gateway::MqttConfig mqtt;
        mqtt.enabled = true; mqtt.clientId = "GW_TEST"; mqtt.topicMachineCode = "GW_TEST";
        mqtt.fullTelemetryTopic = "fixture/full"; mqtt.realtimeTelemetryTopic = "fixture/realtime";
        mqtt.statusTopic = "fixture/audit"; mqtt.otaStatusTopic = "fixture/ota";
        edge_gateway::MqttDriverConfig driver;
        driver.scanIntervalMs = 50; driver.fullUploadIntervalMs = 100; driver.publishFullOnStart = true;
        driver.fullUploadIndexes = {1001}; driver.healthFile = root + "/mqtt-health.json";
        driver.healthPublishIntervalMs = 100;
        driver.priorityControlLeaseFile = root + "/priority.json";
        driver.powerControlOwnershipFile = root + "/ownership.json";
        auto publisher = std::make_shared<OtaReplayPublisherFixture>();
        publisher->throwReplayAudit = fault == "audit" || fault == "reconcile-audit";
        publisher->throwOta = fault == "publish";
        if (fault == "clear") publisher->afterOta = [&] {
            if (publisher->otaAttempts == 7) {
#ifdef OTA_FINALIZATION_FAULT_TEST
                failReplayJournalRename = true;
#endif
            }
        };
        edge_gateway::MqttIncomingMessage subscription;
        subscription.type = edge_gateway::MqttIncomingType::RealtimeRequest;
        subscription.payload = "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"L7\",\"indexes\":[1001],\"intervalMs\":250,\"ttlSec\":300}";
        publisher->incoming.push_back(subscription);
        edge_gateway::MqttDriverService service(mqtt, driver, {device}, router, publisher, nullptr,
            std::unique_ptr<edge_gateway::OtaService>(new edge_gateway::OtaService(config)));
        int escaped = 0;
        if (startWorker) {
            try { service.start(); } catch (const std::exception&) { ++escaped; }
            for (int i = 0; escaped == 0 && i < 100 && publisher->realtime < 3; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            service.stop();
        } else {
            for (int i = 0; i < 120; ++i) {
                try { service.runScanOnce(now + 10000 + i * 1000); }
                catch (const std::exception&) { ++escaped; }
            }
        }
        std::cout << "replay isolation " << fault << ' ' << (startWorker ? "start" : "scan")
                  << ": escaped=" << escaped << " full=" << publisher->full << " realtime=" << publisher->realtime << '\n';
        require(escaped == 0 && publisher->full >= 3 && publisher->realtime >= 3,
            "OTA fault must not interrupt actual start or continuous full/realtime scans");
        const auto health = edge_gateway::json::JsonParser(readFile(driver.healthFile)).parse();
        require(health.find("scanFailedCycles")->asNumber() == 0, "OTA replay failure must not mark main scan failed");
        require(publisher->failedStatuses("RECOVER") == 0 && publisher->negativeReplies("RECOVER") == 0,
            "replay isolation cannot fabricate task failure");
        if (!startWorker) {
            if (reconcileFault || fault == "read" || fault == "publish") {
                require(publisher->replayAudits == 5, "persistent replay failure needs bounded backoff, not per-scan audit/I/O");
                if (reconcileFault) require(!readFile(config.stagingDir + "/ota_reconcile_RECOVER.json").empty() &&
                    readFile(config.stagingDir + "/ota_final_RECOVER.txt").empty(), "reconcile write fault was not exercised");
            }
            if (fault == "clear") {
                bool clearFailed = false;
                for (const auto& audit : publisher->audits)
                    if (audit.find("\"phase\":\"journal-clear\"") != std::string::npos) clearFailed = true;
                require(clearFailed && readFile(journal) == replayFixtureJournal(),
                    "clear fault must preserve the original journal and stay inside OTA boundary");
            }
            // Only the fixture removes its fault; product does not unlock/clean state.
            if (reconcileFault) require(rmdir((config.stagingDir + "/current_version.txt").c_str()) == 0, "cannot clear version fault");
            if (fault == "read") require(rmdir(journal.c_str()) == 0, "cannot clear journal fixture fault");
            if (fault == "read" || fault == "audit") writeFixtureFile(journal, replayFixtureJournal());
#ifdef OTA_FINALIZATION_FAULT_TEST
            failReplayJournalRename = false;
#endif
            publisher->throwOta = false;
            publisher->afterOta = nullptr;
            const auto attempts = publisher->otaAttempts.load();
            const auto recoveryAt = now + 160000;
            service.runScanOnce(recoveryAt);
            require(publisher->otaAttempts > attempts && !readFile(journal).empty(), "cleared fault must resume bounded replay");
            for (int i = 1; i <= 5; ++i) service.runScanOnce(recoveryAt + i * 5000);
            require(!readFile(journal).empty(), "six successful rounds before 30 seconds cannot clear journal");
            service.runScanOnce(recoveryAt + 30000);
            require(readFile(journal).empty(), "journal must clear after successful rounds and 30-second window");
            if (reconcileFault) require(countStage(edge_gateway::OtaService(config).loadFinalStatuses(), "RECOVER", "completed") == 1,
                "trusted health reconciliation must recover after fault removal without applying");
            require(publisher->full >= 127 && publisher->realtime >= 127, "telemetry must continue throughout fault and recovery");
            const auto recoveredHealth = edge_gateway::json::JsonParser(readFile(driver.healthFile)).parse();
            require(recoveredHealth.find("scanFailedCycles")->asNumber() == 0, "clear/recovery must not increment scan failures");
            std::cout << "replay isolation " << fault << ": bounded retry/recovery, six-round/30s retention, full/realtime uninterrupted\n";
        }
    } catch (...) {
#ifdef OTA_FINALIZATION_FAULT_TEST
        failReplayJournalRename = false;
#endif
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(shm);
        removeEventStoreTestTree(root);
        throw;
    }
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(shm);
    removeEventStoreTestTree(root);
}

void verifyDirectOtaReplayIsolation(const std::string& fault) {
    char pattern[] = "/tmp/ota-direct-isolation-XXXXXX";
    const char* created = mkdtemp(pattern);
    require(created != nullptr, "cannot create Direct replay fixture");
    const std::string root = created;
    try {
        const auto config = lifecycleConfig(root);
        require(mkdir(config.stagingDir.c_str(), 0700) == 0, "cannot create Direct staging");
        seedReplayHealthFault(config.stagingDir);
        auto oldIntent = readFile(config.stagingDir + "/ota_job_RECOVER.json");
        oldIntent.replace(oldIntent.find("RECOVER"), 7, "OLD");
        writeFixtureFile(config.stagingDir + "/ota_job_OLD.json", oldIntent);
        writeFixtureFile(config.stagingDir + "/ota_job_OLD.lock", "");
        writeFixtureFile(config.stagingDir + "/ota_final_OLD.txt",
            "jobId=OLD\ntoken=00000000-0000-4000-8000-000000000002\nstatus=completed\n");
        edge_gateway::SystemMonitorConfig::DirectMaintenanceConfig direct;
        direct.otaAppConfigFile = root + "/app.json";
        direct.otaStatusFile = root + "/direct-status.jsonl";
        const std::string app = "{\"ota\":{\"enabled\":true,\"stagingDir\":\"" + config.stagingDir + "\"}}\n";
        const std::string status = "{\"jobId\":\"RECOVER\",\"machineCode\":\"GW_TEST\",\"stage\":\"applying\",\"progress\":100,\"ts\":1000}\n";
        writeFixtureFile(direct.otaAppConfigFile, fault == "config" ? "{secret-credential" : app);
        writeFixtureFile(direct.otaStatusFile, status);
        if (fault == "state") require(mkdir((config.stagingDir + "/ota_state.lock").c_str(), 0700) == 0,
            "cannot inject Direct state-lock failure");
        namespace dm = edge_gateway::system_monitor_direct_maintenance;
        const auto get = [&] { return dm::handleHttpRequest(direct, "GET", "/api/v1/ota/status", "", "", "", "127.0.0.1"); };
        std::string reply;
        bool escaped = false;
        try { reply = get(); } catch (...) { escaped = true; }
        std::cout << "Direct OTA " << fault << ": escaped=" << escaped << '\n';
        require(!escaped && reply.find("HTTP/1.1 200") == 0, "Direct OTA I/O fault must preserve readable existing status response");
        require(reply.find("\"reconciliationFailed\":true") != std::string::npos &&
            reply.find("\"stage\":\"applying\"") != std::string::npos && reply.find("\"stage\":\"failed\"") == std::string::npos,
            "Direct degradation must be top-level diagnostic, not synthesized task failure");
        require(reply.find("secret-credential") == std::string::npos && reply.find(root) == std::string::npos,
            "Direct degraded response must not expose original exception or credentials");
        if (fault == "metadata") require(reply.find("\"jobId\":\"OLD\"") != std::string::npos,
            "failed reconciliation must not prevent reading other trusted finals");
        require(readFile(direct.otaStatusFile) == status && readFile(config.stagingDir + "/ota_final_RECOVER.txt").empty(),
            "Direct failure must not append status or fabricate completed");
        if (fault == "state") require(rmdir((config.stagingDir + "/ota_state.lock").c_str()) == 0, "cannot clear Direct lock fault");
        writeFixtureFile(direct.otaAppConfigFile, app);
        require(rmdir((config.stagingDir + "/current_version.txt").c_str()) == 0, "cannot clear Direct version fault");
        const auto recovered = get();
        require(recovered.find("\"reconciliationFailed\":false") != std::string::npos &&
            recovered.find("\"stage\":\"applying\"") == std::string::npos &&
            countStage(edge_gateway::OtaService(config).loadFinalStatuses(), "RECOVER", "completed") == 1,
            "Direct existing healthy recovery must resume after fixture fault clears");
        std::cout << "Direct OTA " << fault << ": existing items/finals retained, explicit degradation, recovered\n";
    } catch (...) { removeEventStoreTestTree(root); throw; }
    removeEventStoreTestTree(root);
}

void verifyOtaReplayIsolationCases() {
    for (const std::string fault : {"reconcile", "reconcile-audit", "read", "audit", "publish"}) {
        verifyOtaReplayIsolation(fault, true);
        verifyOtaReplayIsolation(fault, false);
    }
    verifyOtaReplayIsolation("clear", false);
    for (const std::string fault : {"metadata", "state", "config"}) verifyDirectOtaReplayIsolation(fault);
}
