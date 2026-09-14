#include "edge_gateway/ota_service.hpp"
#include "edge_gateway/builtin_mqtt_driver_publisher.hpp"
#include "edge_gateway/json_value.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <limits>
#include <set>
#ifndef _WIN32
#include "event_store_test_files.hpp"
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef OTA_PENDING_FAULT_TEST
// Standalone fault build: -DOTA_PENDING_FAULT_TEST
// -Wl,--wrap=fsync -Wl,--wrap=rename -Wl,--wrap=write
static std::atomic<int> pendingSyncCalls{0};
static std::atomic<int> pendingSyncFailure{0};
static std::atomic<bool> pendingRenameFailure{false};
static std::atomic<bool> pendingWriteFailure{false};
extern "C" int __real_fsync(int);
extern "C" int __real_rename(const char*, const char*);
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" int __wrap_fsync(int fd) {
    if (++pendingSyncCalls == pendingSyncFailure) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
extern "C" int __wrap_rename(const char* oldPath, const char* newPath) {
    if (pendingRenameFailure) { errno = EIO; return -1; }
    return __real_rename(oldPath, newPath);
}
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t size) {
    if (pendingWriteFailure) { errno = ENOSPC; return -1; }
    return __real_write(fd, data, size);
}
#endif

namespace edge_gateway {
struct OtaServiceTestAccess {
    static void verifyChecksum(const OtaService& service, const OtaRequest& request, const std::string& path) {
        service.verifyChecksum(request, path);
    }
    static void append(const OtaService& service, OtaStatus& status) { service.appendPendingStatus(status); }
    static void report(const OtaService& service, OtaStatus& status,
        const std::function<void(const OtaStatus&)>& publish) {
        service.reportStage(&status, status.stage, status.progress, status.message, status.ts, publish);
    }
};
}

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireRejected(const edge_gateway::OtaService& service, edge_gateway::OtaRequest request, const std::string& expected) {
    std::string error;
    require(!service.validateRequest(request, &error), "ota request should be rejected");
    if (error.find(expected) == std::string::npos) {
        throw std::runtime_error("unexpected ota validation error: " + error);
    }
}

bool containsText(const std::string& path, const std::string& expected) {
    std::ifstream input(path.c_str());
    if (!input.is_open()) {
        return false;
    }
    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return content.find(expected) != std::string::npos;
}

edge_gateway::OtaStatus specialStatus() {
    edge_gateway::OtaStatus status;
    status.jobId = "job,\t\r\n\"\\";
    status.machineCode = "GW\t\xe4\xb8\xad\xe6\x96\x87";
    status.stage = "accepted";
    status.progress = 19;
    status.downloadedBytes = 9007199254740993ULL;
    status.totalBytes = std::numeric_limits<std::uint64_t>::max();
    status.message = std::string("nul\0", 4) + "comma,tab\tCR\rLF\nquote\"slash\\";
    status.ts = 9007199254740993LL;
    return status;
}

void sameStatus(const edge_gateway::OtaStatus& a, const edge_gateway::OtaStatus& b) {
    require(a.jobId == b.jobId && a.machineCode == b.machineCode && a.stage == b.stage &&
        a.progress == b.progress && a.downloadedBytes == b.downloadedBytes && a.totalBytes == b.totalBytes &&
        a.message == b.message && a.ts == b.ts && a.occurrenceId == b.occurrenceId,
        "pending status did not roundtrip losslessly");
}

template<class F> void requireThrows(F&& fn) {
    bool threw = false;
    try { fn(); } catch (const std::exception&) { threw = true; }
    require(threw, "pending failure must not be reported as durable acceptance");
}

std::string readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

#ifndef _WIN32
struct PendingDirectory {
    std::string path;
    PendingDirectory() {
        char name[] = "/tmp/ota-pending-test-XXXXXX";
        const auto* created = mkdtemp(name);
        require(created != nullptr, "cannot create isolated pending fixture");
        path = created;
    }
    ~PendingDirectory() { removeEventStoreTestTree(path); }
};

void checksumTests() {
    using namespace edge_gateway;
    PendingDirectory directory;
    const auto artifactPath = directory.path + "/artifact.tar.gz";
    { std::ofstream artifact(artifactPath, std::ios::binary); artifact << "abc"; }
    OtaConfig config;
    config.enabled = true;
    config.checksumRequired = true;
    config.downloadDir = directory.path + "/downloads";
    // A regular file blocks staging even if a broken preflight accepts the request.
    config.stagingDir = artifactPath;
    config.backupDir = directory.path + "/backup";
    config.applyScript.clear();
    config.rollbackScript.clear();
    config.autoReboot = false;
    config.minFreeBytes = 0;
    config.upgradeTimeoutSec = 5;
    OtaService service(config);
    auto optionalConfig = config;
    optionalConfig.checksumRequired = false;
    OtaService optional(optionalConfig);
    OtaRequest request;
    request.jobId = "CHECKSUM_TEST";
    request.version = "checksum-test";
    request.artifactUrl = artifactPath;
    request.sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    int failures = 0;
    const auto check = [&](const char* name, const std::function<void()>& test) {
        try {
            test();
            std::cout << "ota checksum " << name << " passed\n";
        } catch (const std::exception& ex) {
            ++failures;
            std::cerr << "ota checksum " << name << " failed: " << ex.what() << "\n";
        }
    };
    const auto expectError = [](const std::function<void()>& operation, const std::string& expected) {
        std::string error;
        try { operation(); } catch (const std::exception& ex) { error = ex.what(); }
        if (error != expected) throw std::runtime_error("expected '" + expected + "', got '" + error + "'");
    };
    auto missing = request;
    missing.sha256.clear();
    check("required-missing-validation", [&] {
        requireRejected(service, missing, "ota sha256 is required");
    });
    check("required-missing-before-download", [&] {
        OtaReply reply;
        OtaStatus status;
        bool published = false;
        expectError([&] {
            service.execute(missing, "GW_TEST", 1000, &reply, &status,
                [&](const OtaStatus&) { published = true; });
        }, "ota sha256 is required");
        require(!reply.accepted && status.stage.empty() && !published,
            "missing checksum must fail before acceptance or status publication");
        require(access(config.downloadDir.c_str(), F_OK) != 0,
            "missing checksum must fail before creating downloads");
    });
    check("required-missing-verification", [&] {
        expectError([&] { OtaServiceTestAccess::verifyChecksum(service, missing, artifactPath); },
            "ota sha256 is required");
    });
    check("invalid-format", [&] {
        for (const auto& hash : {std::string(63, 'a'), std::string(65, 'a'), std::string(64, 'g'),
                                std::string(64, ' ')}) {
            auto invalid = request;
            invalid.sha256 = hash;
            requireRejected(service, invalid, "invalid ota sha256");
            requireRejected(optional, invalid, "invalid ota sha256");
        }
    });
    check("matching-lowercase", [&] {
        require(service.validateRequest(request), "valid checksum request rejected");
        OtaServiceTestAccess::verifyChecksum(service, request, artifactPath);
    });
    check("matching-mixed-case", [&] {
        auto mixed = request;
        mixed.sha256 = "BA7816bf8F01CFEA414140de5dae2223B00361A396177A9CB410FF61F20015AD";
        require(service.validateRequest(mixed), "valid mixed-case checksum request rejected");
        OtaServiceTestAccess::verifyChecksum(service, mixed, artifactPath);
    });
    auto mismatch = request;
    mismatch.sha256 = std::string(64, '0');
    check("mismatch", [&] {
        require(service.validateRequest(mismatch), "well-formed checksum request rejected");
        expectError([&] { OtaServiceTestAccess::verifyChecksum(service, mismatch, artifactPath); },
            "artifact sha256 mismatch");
    });
    check("optional-compatibility", [&] {
        require(optional.validateRequest(missing), "optional empty checksum rejected");
        require(optional.validateRequest(request), "optional valid checksum rejected");
        require(optional.validateRequest(mismatch), "optional well-formed checksum rejected");
        const auto absent = directory.path + "/does-not-exist";
        OtaServiceTestAccess::verifyChecksum(optional, missing, absent);
        OtaServiceTestAccess::verifyChecksum(optional, request, absent);
        OtaServiceTestAccess::verifyChecksum(optional, mismatch, absent);
    });
    require(failures == 0, "ota checksum regression failures (no apply scripts executed)");
}

void pendingProcess(const char* mode, const std::string& path, const std::string& occurrence = {}) {
    const auto pid = fork();
    require(pid >= 0, "pending subprocess fork failed");
    if (pid == 0) {
        execl("/proc/self/exe", "ota_service_test", mode, path.c_str(), occurrence.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    require(waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "pending subprocess failed");
}

void pendingJournalTests() {
    using namespace edge_gateway;
    PendingDirectory directory;
    OtaConfig config;
    config.stagingDir = directory.path + "/staging";
    config.maxPendingStatusBytes = 0;
    OtaService service(config);
    require(service.loadPendingStatuses().empty(), "missing journal is not empty");
    auto absent = config;
    absent.stagingDir += "/missing-parent/missing-staging";
    require(OtaService(absent).loadPendingStatuses().empty(), "loading missing journal tried to create directories");
    auto status = specialStatus();
    OtaServiceTestAccess::append(service, status);
    require(status.occurrenceId.size() == 36, "new status has no occurrence UUID");
    const auto path = config.stagingDir + "/ota_status_pending.log";
    require(containsText(path, "\"format\":\"ota-status-v2\""), "new journal is not JSON v2");
    OtaService restarted(config);
    sameStatus(restarted.loadPendingStatuses().at(0), status);
    std::string publishedPayload;
    BuiltinMqttDriverPublisher publisher(MqttConfig{}, MqttPublisherMode::Bidirectional,
        MqttEventOutboxOwnership::External,
        [&](const std::string& type, const std::string&, const std::string& payload, std::int64_t ts) {
            require(type == "ota_status" && ts == status.ts, "Builtin changed pending type/timestamp");
            publishedPayload = payload;
        });
    publisher.publishOtaStatus("ota/status", status);
    const auto originalPayload = publishedPayload;
    publisher.publishOtaStatus("ota/status", restarted.loadPendingStatuses().at(0));
    require(publishedPayload == originalPayload, "restart changed Builtin payload bytes");
    const auto encoded = json::JsonParser(publishedPayload).parse();
    require(encoded.find("occurrenceId") && encoded.find("occurrenceId")->asString() == status.occurrenceId,
        "Builtin omitted occurrence ID");
    auto legacyStatus = status;
    legacyStatus.occurrenceId.clear();
    publisher.publishOtaStatus("ota/status", legacyStatus);
    require(publishedPayload == originalPayload.substr(0, originalPayload.find(",\"occurrenceId\":")) + "}",
        "empty occurrence changed legacy payload");
    auto second = status;
    OtaServiceTestAccess::report(service, second, [&](const OtaStatus& published) {
        OtaService reader(config);
        sameStatus(reader.loadPendingStatuses().back(), published);
    });
    require(second.occurrenceId != status.occurrenceId, "distinct identical occurrences were collapsed");
    // Clearing an earlier snapshot must not remove a later append.
    restarted.clearPendingStatuses();
    const auto remaining = restarted.loadPendingStatuses();
    require(remaining.size() == 1, "snapshot clear removed a concurrent append");
    sameStatus(remaining[0], second);
    restarted.clearPendingStatuses();
    require(OtaService(config).loadPendingStatuses().empty(), "clear not persistent after restart");

    // Both historical layouts remain readable, even in a mixed journal.
    {
        std::ofstream legacy(path, std::ios::binary);
        legacy << "old6\tGW\taccepted\t0\told message\t1780000000123\n"
               << "old8\tGW\tdownloading\t12\t9007199254740993\t18446744073709551615\tcomma, preserved\t1780000000456\n";
    }
    auto old = service.loadPendingStatuses();
    require(old.size() == 2 && old[0].downloadedBytes == 0 && old[0].totalBytes == 0 &&
        old[0].ts == 1780000000123LL && old[0].occurrenceId.empty() && old[1].occurrenceId.empty(),
        "legacy 6/8-field identity/timestamp changed");
    require(old[1].downloadedBytes == 9007199254740993ULL && old[1].totalBytes == UINT64_MAX &&
        old[1].message == "comma, preserved", "legacy byte counts/message changed");
    auto mixed = specialStatus();
    OtaServiceTestAccess::append(service, mixed);
    auto loaded = OtaService(config).loadPendingStatuses();
    require(loaded.size() == 3, "mixed journal lost rows");
    sameStatus(loaded[0], old[0]); sameStatus(loaded[1], old[1]); sameStatus(loaded[2], mixed);
    service.loadPendingStatuses(); service.clearPendingStatuses();

    // Exit without destructors, then exec a separate reader using the saved ID.
    pendingProcess("--pending-write", config.stagingDir);
    loaded = OtaService(config).loadPendingStatuses();
    require(loaded.size() == 1, "process exit lost durable pending record");
    pendingProcess("--pending-read", config.stagingDir, loaded[0].occurrenceId);
    service.loadPendingStatuses(); service.clearPendingStatuses();

    // Existing retention keeps whole recent records, never a partial JSON line.
    auto first = specialStatus();
    OtaServiceTestAccess::append(service, first);
    const auto oneSize = readFile(path).size();
    auto limited = config;
    limited.maxPendingStatusBytes = oneSize;
    OtaService bounded(limited);
    auto next = specialStatus();
    OtaServiceTestAccess::append(bounded, next);
    loaded = bounded.loadPendingStatuses();
    require(loaded.size() == 1 && readFile(path).size() == oneSize, "retention split a record");
    sameStatus(loaded[0], next);
    auto oversized = specialStatus(); oversized.message += "too large";
    requireThrows([&] { OtaServiceTestAccess::append(bounded, oversized); });
    sameStatus(bounded.loadPendingStatuses().at(0), next);

    // Corrupt new records fail closed and cannot authorize deleting the journal.
    const auto valid = readFile(path);
    { std::ofstream output(path, std::ios::binary); output << valid.substr(0, valid.size() - 2); }
    requireThrows([&] { bounded.loadPendingStatuses(); });
    bounded.clearPendingStatuses();
    require(!readFile(path).empty(), "failed load authorized clear");
    { std::ofstream output(path, std::ios::binary); output << valid; }

    // Separate instances serialize read-modify-replace rather than losing rows.
    service.loadPendingStatuses(); service.clearPendingStatuses();
    std::vector<std::thread> workers;
    std::atomic<int> failures{0};
    for (int i = 0; i < 4; ++i) workers.emplace_back([&] {
        try {
            OtaService writer(config);
            for (int j = 0; j < 8; ++j) {
                auto event = specialStatus(); OtaServiceTestAccess::append(writer, event);
            }
        } catch (...) { ++failures; }
    });
    for (auto& worker : workers) worker.join();
    loaded = service.loadPendingStatuses();
    std::set<std::string> occurrences;
    for (const auto& event : loaded) occurrences.insert(event.occurrenceId);
    require(failures == 0 && loaded.size() == 32 && occurrences.size() == 32, "concurrent append lost occurrences");
    std::cout << "ota pending JSON/legacy/restart/retention/concurrency passed\n";
}

#ifdef OTA_PENDING_FAULT_TEST
void pendingFaultTests() {
    using namespace edge_gateway;
    PendingDirectory directory;
    OtaConfig config; config.stagingDir = directory.path;
    OtaService service(config);
    auto first = specialStatus();
    OtaServiceTestAccess::append(service, first);
    const auto path = config.stagingDir + "/ota_status_pending.log";
    const auto original = readFile(path);
    auto next = specialStatus();
    for (int fault = 0; fault < 3; ++fault) {
        pendingSyncCalls = 0;
        pendingSyncFailure = fault == 0 ? 1 : 0;
        pendingRenameFailure = fault == 1;
        pendingWriteFailure = fault == 2;
        requireThrows([&] { OtaServiceTestAccess::append(service, next); });
        pendingSyncFailure = 0; pendingRenameFailure = false; pendingWriteFailure = false;
        require(readFile(path) == original, "pre-replace failure damaged original journal");
    }
    const auto retainedId = next.occurrenceId;
    pendingSyncCalls = 0; pendingSyncFailure = 2;
    requireThrows([&] { OtaServiceTestAccess::append(service, next); });
    pendingSyncFailure = 0;
    require(service.loadPendingStatuses().size() == 2, "directory-sync failure fixture did not replace journal");
    OtaServiceTestAccess::append(service, next);
    require(next.occurrenceId == retainedId && service.loadPendingStatuses().size() == 2,
        "uncertain persistence retry regenerated occurrence or duplicated pending row");
    const auto beforeClear = readFile(path);
    pendingSyncCalls = 0; pendingSyncFailure = 1;
    requireThrows([&] { service.clearPendingStatuses(); });
    pendingSyncFailure = 0;
    require(readFile(path) == beforeClear, "failed clear lost pending records");
    pendingSyncCalls = 0; pendingSyncFailure = 2;
    requireThrows([&] { service.clearPendingStatuses(); });
    pendingSyncFailure = 0;
    require(OtaService(config).loadPendingStatuses().empty(), "uncertain clear fixture did not replace journal");

    // A storage failure must never enter the publish callback.
    bool published = false;
    pendingWriteFailure = true;
    requireThrows([&] { OtaServiceTestAccess::report(service, next, [&](const OtaStatus&) { published = true; }); });
    pendingWriteFailure = false;
    require(!published, "unpersisted occurrence was published");
    std::cout << "ota pending write/fsync/rename/clear failure injection passed\n";
}
#endif
#endif

}  // namespace

int main(int argc, char** argv) {
    using namespace edge_gateway;
#ifndef _WIN32
    if (argc == 2 && std::string(argv[1]) == "--checksum-only") {
        try { checksumTests(); return 0; }
        catch (const std::exception& ex) { std::cerr << ex.what() << "\n"; return 1; }
    }
#endif
// Standalone checksum builds use section GC and never link or enter the apply tests.
#ifdef OTA_CHECKSUM_TEST_ONLY
    std::cerr << "usage: ota_service_test --checksum-only\n";
    return 2;
#else
#ifndef _WIN32
    if (argc == 4 && (std::string(argv[1]) == "--pending-write" || std::string(argv[1]) == "--pending-read")) {
        OtaConfig config; config.stagingDir = argv[2];
        OtaService service(config);
        auto expected = specialStatus();
        if (std::string(argv[1]) == "--pending-write") {
            OtaServiceTestAccess::append(service, expected);
            _exit(0);
        }
        expected.occurrenceId = argv[3];
        const auto loaded = service.loadPendingStatuses();
        require(loaded.size() == 1, "exec reader found wrong pending count");
        sameStatus(loaded[0], expected);
        return 0;
    }
    if (!(argc == 2 && std::string(argv[1]) == "--pending-only")) checksumTests();
    pendingJournalTests();
#ifdef OTA_PENDING_FAULT_TEST
    pendingFaultTests();
#endif
    if (argc == 2 && std::string(argv[1]) == "--pending-only") return 0;
#else
    (void)argc; (void)argv;
#endif

    const std::string otaApplyScript = "deploy/ota-apply.sh";
    for (const auto* binary : {
        "ModbusRtu",
        "Dlt645Driver",
        "DioDriver",
        "CanDriver",
        "IecDriver",
        "MqttDriver",
        "EventEngine",
        "ComputeEngine",
        "EmsClusterCoordinator",
        "AgcAvcController",
        "EmsParityCheck",
        "SystemMonitor",
        "LocalDisplay",
        "QtDisplayBridge",
        "CameraService",
        "pointctl"
    }) {
        const auto target = std::string("\"/opt/modbus-gateway/bin/") + binary + "\"";
        require(containsText(otaApplyScript, target), "ota apply script must allow shipped production binaries");
    }

    OtaConfig config;
    config.enabled = true;
    config.downloadDir = "/tmp/gateway-ota-test";
    config.packageType = "tar.gz";
    OtaService service(config);

    OtaRequest ok;
    ok.jobId = "JOB_20260522_001";
    ok.version = "config-20260522.1";
    ok.artifactUrl = "http://127.0.0.1:8090/api/config/ota/config-packages/gateway-config-GW0001-config-1.tar.gz?apiToken=secret";
    ok.sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    std::string error;
    require(service.validateRequest(ok, &error), "valid ota request should pass");

    auto tooLarge = ok;
    tooLarge.size = config.maxArtifactBytes + 1;
    requireRejected(service, tooLarge, "ota artifact size exceeds maxArtifactBytes");

    auto badJob = ok;
    badJob.jobId = "JOB;reboot";
    requireRejected(service, badJob, "invalid ota jobId");

    auto badVersion = ok;
    badVersion.version = "../config";
    requireRejected(service, badVersion, "invalid ota version");

    auto badSha = ok;
    badSha.sha256 = "not-a-sha";
    requireRejected(service, badSha, "invalid ota sha256");

    auto badName = ok;
    badName.version.clear();
    badName.artifactUrl = "http://127.0.0.1:8090/download/../bad.tar.gz";
    requireRejected(service, badName, "invalid ota artifactUrl");

    auto badQuery = ok;
    badQuery.artifactUrl = "http://127.0.0.1:8090/api/config/ota/config-packages/gateway.tar.gz?otaToken=abc%0a";
    requireRejected(service, badQuery, "invalid ota artifactUrl");

    auto localTraversal = ok;
    localTraversal.version.clear();
    localTraversal.artifactUrl = "../gateway.tar.gz";
    requireRejected(service, localTraversal, "invalid ota artifactUrl");

    OtaConfig markerConfig;
    markerConfig.enabled = true;
    markerConfig.downloadDir = "/tmp/gateway-ota-marker/downloads";
    markerConfig.stagingDir = "/tmp/gateway-ota-marker/staging";
    markerConfig.backupDir = "/tmp/gateway-ota-marker/backup";
    markerConfig.applyScript = "/tmp/gateway-ota-marker/apply-ok.sh";
    markerConfig.rollbackScript.clear();
    markerConfig.checksumRequired = false;
    markerConfig.currentVersion = "1.0.0";
    markerConfig.packageType = "tar.gz";
    markerConfig.minFreeBytes = 0;
    OtaService markerService(markerConfig);
    OtaRequest markerRequest;
    markerRequest.jobId = "JOB_MARKER_001";
    markerRequest.version = "2.0.0";
    markerRequest.artifactUrl = "/tmp/gateway-ota-marker/source.tar.gz";
    {
        std::system("rm -rf /tmp/gateway-ota-marker");
        std::system("mkdir -p /tmp/gateway-ota-marker");
        std::ofstream artifact(markerRequest.artifactUrl.c_str(), std::ios::binary | std::ios::trunc);
        artifact << "payload";
        std::ofstream script(markerConfig.applyScript.c_str(), std::ios::trunc);
        script << "#!/bin/sh\nexit 0\n";
    }
    OtaReply reply;
    OtaStatus status;
    markerService.execute(markerRequest, "GW_TEST", 1770000000000LL, &reply, &status, nullptr);
    const std::string markerPath = markerConfig.stagingDir + "/current_version.txt";
    require(containsText(markerPath, "jobId=JOB_MARKER_001"), "version marker should include jobId");
    require(containsText(markerPath, "backupDir=/tmp/gateway-ota-marker/backup/JOB_MARKER_001"), "version marker should include job backup dir");
    require(containsText(markerPath, "workDir=/tmp/gateway-ota-marker/staging/JOB_MARKER_001"), "version marker should include work dir");

    OtaConfig scadaConfig = markerConfig;
    scadaConfig.downloadDir = "/tmp/gateway-ota-scada/downloads";
    scadaConfig.stagingDir = "/tmp/gateway-ota-scada/staging";
    scadaConfig.backupDir = "/tmp/gateway-ota-scada/backup";
    scadaConfig.applyScript = "/tmp/gateway-ota-scada/apply-ok.sh";
    OtaService scadaService(scadaConfig);
    OtaRequest scadaRequest;
    scadaRequest.jobId = "JOB_SCADA_001";
    scadaRequest.version = "2.1.0";
    scadaRequest.packageType = "scada";
    scadaRequest.artifactUrl = "/tmp/gateway-ota-scada/source.kyscada";
    {
        std::system("rm -rf /tmp/gateway-ota-scada");
        std::system("mkdir -p /tmp/gateway-ota-scada");
        std::ofstream artifact(scadaRequest.artifactUrl.c_str(), std::ios::binary | std::ios::trunc);
        artifact << "payload";
        std::ofstream script(scadaConfig.applyScript.c_str(), std::ios::trunc);
        script << "#!/bin/sh\nexit 0\n";
    }
    OtaReply scadaReply;
    OtaStatus scadaStatus;
    scadaService.execute(scadaRequest, "GW_TEST", 1770000000000LL, &scadaReply, &scadaStatus, nullptr);
    require(std::ifstream("/tmp/gateway-ota-scada/downloads/2.1.0.kyscada").good(),
            "SCADA artifact should retain the kyscada extension");
    require(!std::ifstream("/tmp/gateway-ota-scada/downloads/source.kyscada").good(),
            "SCADA artifact should use the versioned file name");

    OtaConfig mismatchConfig;
    mismatchConfig.enabled = true;
    mismatchConfig.downloadDir = "/tmp/gateway-ota-mismatch/downloads";
    mismatchConfig.stagingDir = "/tmp/gateway-ota-mismatch/staging";
    mismatchConfig.backupDir = "/tmp/gateway-ota-mismatch/backup";
    mismatchConfig.applyScript = "/tmp/gateway-ota-mismatch/apply-ok.sh";
    mismatchConfig.rollbackScript.clear();
    mismatchConfig.checksumRequired = false;
    mismatchConfig.packageType = "tar.gz";
    mismatchConfig.minFreeBytes = 0;
    OtaService mismatchService(mismatchConfig);
    OtaRequest mismatchRequest;
    mismatchRequest.jobId = "JOB_MISMATCH_001";
    mismatchRequest.version = "3.0.0";
    mismatchRequest.artifactUrl = "/tmp/gateway-ota-mismatch/source.tar.gz";
    mismatchRequest.size = 8;
    {
        std::system("rm -rf /tmp/gateway-ota-mismatch");
        std::system("mkdir -p /tmp/gateway-ota-mismatch");
        std::ofstream artifact(mismatchRequest.artifactUrl.c_str(), std::ios::binary | std::ios::trunc);
        artifact << "payload";
        std::ofstream script(mismatchConfig.applyScript.c_str(), std::ios::trunc);
        script << "#!/bin/sh\nexit 0\n";
    }
    bool rejectedMismatch = false;
    try {
        OtaReply mismatchReply;
        OtaStatus mismatchStatus;
        mismatchService.execute(mismatchRequest, "GW_TEST", 1770000000000LL, &mismatchReply, &mismatchStatus, nullptr);
    } catch (const std::exception& ex) {
        rejectedMismatch = std::string(ex.what()).find("ota artifact size mismatch") != std::string::npos;
    }
    require(rejectedMismatch, "ota local copy should reject mismatched actual size");
    require(containsText(mismatchConfig.stagingDir + "/upgrade_history.log", "result=failure"), "size mismatch should be recorded");

    OtaConfig retentionConfig;
    retentionConfig.enabled = true;
    retentionConfig.downloadDir = "/tmp/gateway-ota-retention/downloads";
    retentionConfig.stagingDir = "/tmp/gateway-ota-retention/staging";
    retentionConfig.backupDir = "/tmp/gateway-ota-retention/backup";
    retentionConfig.applyScript = "/tmp/gateway-ota-retention/apply-ok.sh";
    retentionConfig.rollbackScript.clear();
    retentionConfig.checksumRequired = false;
    retentionConfig.retentionCount = 2;
    retentionConfig.packageType = "tar.gz";
    retentionConfig.minFreeBytes = 0;
    OtaService retentionService(retentionConfig);
    OtaRequest retentionRequest;
    retentionRequest.jobId = "JOB_RETENTION_001";
    retentionRequest.version = "1.10.0";
    retentionRequest.artifactUrl = "/tmp/gateway-ota-retention/source.tar.gz";
    {
        std::system("rm -rf /tmp/gateway-ota-retention");
        std::system("mkdir -p /tmp/gateway-ota-retention/downloads /tmp/gateway-ota-retention/staging /tmp/gateway-ota-retention/backup");
        std::ofstream artifact(retentionRequest.artifactUrl.c_str(), std::ios::binary | std::ios::trunc);
        artifact << "payload";
        std::ofstream script(retentionConfig.applyScript.c_str(), std::ios::trunc);
        script << "#!/bin/sh\nexit 0\n";
    }
    for (const auto& version : {"1.2.0", "1.10.0", "1.3.0"}) {
        std::ofstream extra((std::string("/tmp/gateway-ota-retention/downloads/") + version + ".tar.gz").c_str(), std::ios::binary | std::ios::trunc);
        extra << version;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    OtaReply retentionReply;
    OtaStatus retentionStatus;
    retentionService.execute(retentionRequest, "GW_TEST", 1770000000000LL, &retentionReply, &retentionStatus, nullptr);
    require(!std::ifstream("/tmp/gateway-ota-retention/downloads/1.2.0.tar.gz").good(), "oldest artifact should be removed by retention");
    require(std::ifstream("/tmp/gateway-ota-retention/downloads/1.10.0.tar.gz").good(), "current artifact should be retained");
    require(std::ifstream("/tmp/gateway-ota-retention/downloads/1.3.0.tar.gz").good(), "newer artifact should be retained");

    std::cout << "ota_service_test passed" << std::endl;
    return 0;
#endif
}
