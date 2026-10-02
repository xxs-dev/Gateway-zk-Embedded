#include "edge_gateway/system_monitor_service.hpp"
#include "edge_gateway/json_value.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace edge_gateway {
struct SystemMonitorDiagnosticTestAccess {
    static void expire(SystemMonitorService& service, const std::string& id) {
        service.diagnosticRequests_.at(id).expiresAt = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    }
    static std::chrono::steady_clock::time_point expiry(SystemMonitorService& service,
            const std::string& id) {
        return service.diagnosticRequests_.at(id).expiresAt;
    }
    static std::int64_t effectiveNow(SystemMonitorService& service, std::int64_t wall,
            std::chrono::steady_clock::time_point steady) {
        return service.diagnosticEffectiveNow(wall, steady);
    }
};
}
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
#ifndef _WIN32
using namespace edge_gateway;
bool removeContents(int directory) {
    DIR* entries = fdopendir(fcntl(directory, F_DUPFD_CLOEXEC, 3));
    if (!entries) return false;
    bool ok = true;
    for (dirent* entry = readdir(entries); entry && ok; entry = readdir(entries)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        struct stat before{};
        if (fstatat(directory, name.c_str(), &before, AT_SYMLINK_NOFOLLOW) != 0) { ok = false; break; }
        if (S_ISDIR(before.st_mode)) {
            const int child = openat(directory, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            struct stat opened{};
            ok = child >= 0 && fstat(child, &opened) == 0 && before.st_dev == opened.st_dev &&
                before.st_ino == opened.st_ino && removeContents(child);
            if (child >= 0) close(child);
        }
        struct stat current{};
        ok = ok && fstatat(directory, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0 &&
            before.st_dev == current.st_dev && before.st_ino == current.st_ino &&
            unlinkat(directory, name.c_str(), S_ISDIR(before.st_mode) ? AT_REMOVEDIR : 0) == 0;
    }
    closedir(entries); return ok;
}
class Publisher : public IMqttDriverPublisher {
public:
    void publishFullSnapshot(const std::string&, const std::vector<StoredPointValue>&, const std::string&) override {}
    void publishAlarm(const std::string&, std::uint32_t, const StoredPointValue&, const std::string&, bool) override {}
    void publishOnDemand(const std::string&, const std::vector<StoredPointValue>&, const std::string&) override {}
    void publishChangeEvent(const std::string&, const StoredPointValue&) override {}
    void publishCommandReply(const std::string&, const MqttCommandReply&) override {}
    void publishOtaReply(const std::string&, const OtaReply&) override {}
    void publishOtaStatus(const std::string&, const OtaStatus&) override {}
    void publishJsonMessage(const std::string& topic, const std::string& body) override {
        if (failOnce && topic == "diag/reply") { failOnce = false; throw std::runtime_error("private publish failure"); }
        if (topic == "diag/reply") replies.push_back(body);
        if (topic == "monitor/reply") ++monitorReplies;
        if (body.find("scada-offline-safety") != std::string::npos) ++safetyEvents;
    }
    std::vector<MqttIncomingMessage> pollIncoming(int) override {
        auto result = incoming; incoming.clear(); return result;
    }
    void request(const std::string& id, const std::string& command = "top_once",
                 const std::string& fields = "", std::int64_t requestTs = 1000) {
        MqttIncomingMessage message; message.type = MqttIncomingType::DiagRequest;
        message.payload = "{\"cmdId\":\"" + id + "\",\"machineCode\":\"PRIVATE_DIAG\",\"command\":\"" + command + "\",\"requestTs\":" + std::to_string(requestTs) + fields + "}";
        incoming.push_back(message);
    }
    std::vector<MqttIncomingMessage> incoming;
    std::vector<std::string> replies;
    int safetyEvents = 0, monitorReplies = 0;
    bool failOnce = false;
};
struct Fixture {
    std::string root, previousPath;
    Fixture() {
        char path[] = "/tmp/gateway-r10-private-XXXXXX";
        const auto made = mkdtemp(path); require(made != nullptr, "private test root");
        root = made; previousPath = getenv("PATH") ? getenv("PATH") : "";
        require(mkdir((root + "/bin").c_str(), 0700) == 0, "private bin directory");
        write("bin/top", "#!/bin/sh\necho run >> \"$DIAG_FIXTURE_DIR/runs\"\n"
                         "case \"$DIAG_FIXTURE_MODE\" in stall) sleep 30;; failure) exit 1;; control) printf '\\001\\b\\f\\033';; utf8) printf '\\344\\270\\255\\377';; boundary) head -c 243 /dev/zero | tr '\\000' X; printf '\\344\\270\\255abcdefghijklmnop';; *) printf 'done\\n';; esac\n");
        require(chmod((root + "/bin/top").c_str(), 0700) == 0, "private top mode");
        setenv("PATH", (root + "/bin:" + previousPath).c_str(), 1);
        setenv("DIAG_FIXTURE_DIR", root.c_str(), 1);
        setenv("DIAG_FIXTURE_MODE", "stall", 1);
        require(mkdir((root + "/project").c_str(), 0700) == 0, "private project directory");
        write("project/manifest.json", R"({"schemaVersion":"2.0","projectId":"private","projectName":"Private","packageVersion":"1.0.0","entryScreen":"","packageRole":"edgeNode"})");
        write("project/topology.json", R"({"mode":"upperComputer","scadaHost":"fixture","emsHost":"fixture","dataTransport":"directPreferredMqttFallback","offlinePolicy":"edgeSafeState","upperComputerOfflinePolicy":{"timeoutMs":1000,"action":"executeConfiguredActions","retainLocalSafetyRules":true,"requireFreshLeaseForControl":true,"safetyActions":[{"actionId":"private-safe","nodeId":"private","tagId":"private.setpoint","value":0,"highPriority":false}]}})");
        write("project/nodes.json", R"([{"nodeId":"private","machineCode":"PRIVATE_DIAG","displayName":"Private","roles":["safety"]}])");
        write("project/tags.json", R"([{"tagId":"private.setpoint","nodeId":"private","deviceId":"private","meterCode":"PRIVATE","pointCode":"SET","semanticRole":"pcs.powerSetpoint","displayName":"Private","unit":"kW","dataType":"float64","access":"readWrite","indexFallback":101}])");
        write("project/runtime-map.json", R"([{"nodeId":"private","tagId":"private.setpoint","sharedMemoryName":"private-unused-no-store","index":101,"writable":true,"dataType":"float64","unit":"kW"}])");
    }
    ~Fixture() {
        setenv("PATH", previousPath.c_str(), 1); unsetenv("DIAG_FIXTURE_DIR"); unsetenv("DIAG_FIXTURE_MODE");
        // Exact mkdtemp-owned root, after service destruction. Never follow links.
        const int directory = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        const bool removed = directory >= 0 && removeContents(directory);
        if (directory >= 0) close(directory);
        if (!removed || rmdir(root.c_str()) != 0) std::cerr << "private fixture cleanup not confirmed: " << root << "\n";
    }
    void write(const std::string& name, const std::string& content) {
        std::ofstream file(root + "/" + name); file << content; require(bool(file), "private fixture write");
    }
    int runs() const {
        std::ifstream file(root + "/runs"); std::string line; int count = 0;
        while (std::getline(file, line)) ++count;
        return count;
    }
    SystemMonitorConfig config(bool safety = false) const {
        SystemMonitorConfig config;
        config.defaultIntervalMs = 3600000; config.maxDiagOutputBytes = 256;
        config.allowedCommands = {"top_once", "systemctl_status"};
        config.realtimeMeterLeaseFile = root + "/leases.json";
        config.scadaUpperComputerSafety.enabled = safety;
        config.scadaUpperComputerSafety.projectDirectory = root + "/project";
        config.scadaUpperComputerSafety.leaseFile = root + "/heartbeat.json";
        config.scadaUpperComputerSafety.priorityControlLeaseFile = root + "/priority.json";
        config.scadaUpperComputerSafety.clockStepToleranceMs = 0;
        config.scadaUpperComputerSafety.heartbeatSessionPrefix = "SCADA_";
        return config;
    }
};
MqttConfig mqtt() {
    MqttConfig config; config.diagReplyTopic = "diag/reply"; config.systemMonitorReplyTopic = "monitor/reply";
    config.statusTopic = "status"; return config;
}
void tick(SystemMonitorService& service, std::int64_t now) {
    const auto before = std::chrono::steady_clock::now(); service.runOnce(now);
    require(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(250),
            "diagnostic blocked actual monitor/safety loop");
}
void waitReply(SystemMonitorService& service, Publisher& publisher, std::size_t count) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (publisher.replies.size() < count && std::chrono::steady_clock::now() < until) {
        tick(service, 3000); std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(publisher.replies.size() == count, "bounded terminal reply wait");
}
void cases() {
    Fixture fixture;
    {
        auto publisher = std::make_shared<Publisher>(); PointStoreRouter emptyRouter;
        SystemMonitorService service(fixture.config(true), mqtt(), publisher, "PRIVATE_DIAG", {}, &emptyRouter);
        publisher->request("stall"); tick(service, 1000);
        const auto launchedBy = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (fixture.runs() == 0 && std::chrono::steady_clock::now() < launchedBy) {
            tick(service, 1000); std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(fixture.runs() == 1, "private stalled command did not launch");
        publisher->request("stall"); tick(service, 1001);
        require(publisher->replies.empty(), "in-flight duplicate published false terminal");
        MqttIncomingMessage observe; observe.type = MqttIncomingType::SystemMonitorRequest;
        observe.payload = R"({"machineCode":"PRIVATE_DIAG","action":"subscribe","sessionId":"observer"})";
        publisher->incoming.push_back(observe); tick(service, 2001);
        require(publisher->monitorReplies == 1 && publisher->safetyEvents > 0,
                "independent monitor and safety processing did not advance");
        publisher->request("other"); tick(service, 2002);
        require(publisher->replies.back().find("\"reason\":\"busy\"") != std::string::npos &&
                publisher->replies.back().find("\"replyKind\":\"admission_rejection\"") != std::string::npos &&
                publisher->replies.back().find("\"cmdId\":") == std::string::npos &&
                publisher->replies.back().find("\"cleanupConfirmed\":") == std::string::npos,
                "busy must not forge a terminal or inherit another owner's cleanup state");
        publisher->request("stall", "systemctl_status", ",\"service\":\"private.service\""); tick(service, 2003);
        require(publisher->replies.back().find("\"replyKind\":\"admission_rejection\"") != std::string::npos &&
                publisher->replies.back().find("\"attemptedCmdId\":\"stall\"") != std::string::npos &&
                publisher->replies.back().find("\"command\":\"systemctl_status\"") != std::string::npos &&
                publisher->replies.back().find("\"arg\":\"private.service\"") != std::string::npos &&
                publisher->replies.back().find("\"cmdId\":") == std::string::npos,
                "in-flight conflict must not occupy the admitted terminal identity");
        publisher->request("invalid", "systemctl_status", ",\"service\":\"x; touch nope\""); tick(service, 2004);
        require(publisher->replies.back().find("invalid_command_argument") != std::string::npos &&
                publisher->replies.back().find("\"cmdId\":") == std::string::npos, "argument rejected without terminal");
        publisher->request("forbidden", "arbitrary_command"); tick(service, 2005);
        require(publisher->replies.back().find("command_not_allowed") != std::string::npos &&
                publisher->replies.back().find("\"cmdId\":") == std::string::npos, "whitelist rejected without terminal");
        publisher->request("bounded_tail", "journal_tail", ",\"tailLines\":1000"); tick(service, 2006);
        require(publisher->replies.back().find("\"arg\":\"500\"") != std::string::npos &&
                publisher->replies.back().find("command_not_allowed") != std::string::npos,
                "existing journal_tail clamp changed during strict parsing");
        service.stop();
        require(publisher->replies.back().find("\"reason\":\"cancelled\"") != std::string::npos &&
                publisher->replies.back().find("\"replyKind\":\"terminal\"") != std::string::npos &&
                publisher->replies.back().find("\"cmdId\":\"stall\"") != std::string::npos &&
                publisher->replies.back().find("\"command\":\"top_once\"") != std::string::npos &&
                publisher->replies.back().find("\"arg\":\"\"") != std::string::npos,
                "conflict-first must preserve the original terminal result");
        require(fixture.runs() == 1, "duplicate/rejected requests executed again");
        std::cout << "PASS actual-loop/monitor/safety/inflight/conflict/busy/whitelist/stop\n";
    }
    setenv("DIAG_FIXTURE_MODE", "failure", 1);
    {
        auto publisher = std::make_shared<Publisher>();
        SystemMonitorService service(fixture.config(), mqtt(), publisher, "PRIVATE_DIAG");
        publisher->request("exit"); tick(service, 3000); waitReply(service, *publisher, 1);
        const auto terminal = publisher->replies.back();
        require(terminal.find("\"exitCode\":1") != std::string::npos, "exit1 was raw wait status");
        publisher->request("exit", "systemctl_status", ",\"service\":\"private.service\""); tick(service, 3001);
        require(publisher->replies.back().find("\"replyKind\":\"admission_rejection\"") != std::string::npos &&
                publisher->replies.back().find("\"cmdId\":") == std::string::npos,
                "terminal-first conflict must remain nonterminal");
        publisher->request("exit"); tick(service, 3001);
        require(publisher->replies.back() == terminal && fixture.runs() == 2, "completed idempotent replay");
        SystemMonitorDiagnosticTestAccess::expire(service, "exit");
        publisher->request("exit"); tick(service, 3002); waitReply(service, *publisher, 4);
        require(fixture.runs() == 3, "expired completion did not release same id");
        std::cout << "PASS normalized-exit/completed-replay/expiry\n";
    }
    setenv("DIAG_FIXTURE_MODE", "control", 1);
    {
        auto publisher = std::make_shared<Publisher>();
        SystemMonitorService service(fixture.config(), mqtt(), publisher, "PRIVATE_DIAG");
        publisher->request("control"); tick(service, 3500); waitReply(service, *publisher, 1);
        const auto parsed = json::JsonParser(publisher->replies.back(), 16, 64).parse();
        const auto* output = parsed.find("stdout");
        require(output && output->asString() == std::string("\001\b\f\033", 4),
                "diagnostic control bytes did not survive strict JSON roundtrip");
        std::cout << "PASS diagnostic-control-json-roundtrip\n";
    }
    for (const auto& mode : {"utf8", "boundary"}) {
        setenv("DIAG_FIXTURE_MODE", mode, 1);
        auto publisher = std::make_shared<Publisher>();
        SystemMonitorService service(fixture.config(), mqtt(), publisher, "PRIVATE_DIAG");
        publisher->request(mode); tick(service, 3600); waitReply(service, *publisher, 1);
        const auto parsed = json::JsonParser(publisher->replies.back(), 16, 64).parse();
        const auto* output = parsed.find("stdout");
        const auto* truncated = parsed.find("truncated");
        const auto replacement = std::string("\xef\xbf\xbd", 3);
        const auto expected = std::string(mode) == "utf8" ? std::string("\xe4\xb8\xad", 3) + replacement :
            std::string(243, 'X') + replacement + "\n[truncated]";
        require(output && output->asString() == expected && truncated &&
                truncated->asBool() == (std::string(mode) == "boundary"),
                "diagnostic UTF-8 output or truncated boundary was not valid JSON");
    }
    setenv("DIAG_FIXTURE_MODE", "success", 1);
    {
        auto publisher = std::make_shared<Publisher>();
        SystemMonitorService service(fixture.config(), mqtt(), publisher, "PRIVATE_DIAG");
        MqttIncomingMessage windows; windows.type = MqttIncomingType::DiagRequest;
        windows.payload = R"({"cmdId":"windows_unicode","machineCode":"PRIVATE_DIAG","operator":"\u5F20\u4E09\uD83D\uDE00\u0027\u002B","command":"top_once","service":"","tailLines":100,"requestTs":7000})";
        publisher->incoming.push_back(windows); tick(service, 7000); waitReply(service, *publisher, 1);
        const auto parsed = json::JsonParser(publisher->replies.back(), 16, 64).parse();
        require(parsed.find("operator") && parsed.find("operator")->asString() ==
                std::string("\xe5\xbc\xa0\xe4\xb8\x89\xf0\x9f\x98\x80'+"),
                "Windows JSON escaped Unicode operator did not roundtrip through real handler");
        const int runs = fixture.runs();
        for (const auto& malformed : {
                R"({"cmdId":"bad","machineCode":"PRIVATE_DIAG","operator":"\uD800","command":"top_once","requestTs":7001})",
                R"({"cmdId":"bad","machineCode":"PRIVATE_DIAG","operator":"x","command":"top_once","requestTs":7001,"requestTs":7001})",
                R"({"cmdId":"bad","machineCode":"PRIVATE_DIAG","operator":1,"command":"top_once","requestTs":7001})"}) {
            windows.payload = malformed; publisher->incoming.push_back(windows); tick(service, 7001);
        }
        require(fixture.runs() == runs && publisher->replies.size() == 1,
                "malformed or ambiguous diagnostic identity reached execution");
        std::cout << "PASS Windows-escaped-Unicode/strict-malformed-identity\n";
    }
    {
        auto publisher = std::make_shared<Publisher>();
        SystemMonitorService service(fixture.config(), mqtt(), publisher, "PRIVATE_DIAG");
        publisher->request("freshness"); tick(service, 7200); waitReply(service, *publisher, 1);
        const int runs = fixture.runs();
        publisher->request("freshness"); tick(service, 400000);
        require(publisher->replies.size() == 2 && fixture.runs() == runs,
                "cached terminal must replay before freshness rejection");
        SystemMonitorDiagnosticTestAccess::expire(service, "freshness");
        publisher->request("freshness"); tick(service, 400001);
        require(fixture.runs() == runs && publisher->replies.size() == 2,
                "expired command ID executed after request freshness window");
        publisher->request("clock_floor", "top_once", "", 400002); tick(service, 400002);
        waitReply(service, *publisher, 3);
        const int floorRuns = fixture.runs();
        publisher->request("clock_floor", "top_once", "", 400002); tick(service, 1000);
        require(publisher->replies.size() == 4 && fixture.runs() == floorRuns,
                "cached terminal did not replay after wall-clock rollback");
        publisher->request("rollback_new"); tick(service, 1000);
        require(publisher->replies.size() == 4 && fixture.runs() == floorRuns,
                "wall-clock rollback admitted a stale new ID");
        std::cout << "PASS cached-replay-before-age-gate/expired-ID/clock-rollback\n";
    }
    {
        auto publisher = std::make_shared<Publisher>();
        SystemMonitorService service(fixture.config(), mqtt(), publisher, "PRIVATE_DIAG");
        publisher->request("future_boundary", "top_once", "", 1299999);
        tick(service, 1000000); waitReply(service, *publisher, 1);
        const auto remaining = SystemMonitorDiagnosticTestAccess::expiry(service, "future_boundary") -
            std::chrono::steady_clock::now();
        require(remaining > std::chrono::minutes(10) - std::chrono::seconds(2) &&
                remaining <= std::chrono::minutes(10),
                "terminal retention must cover request age plus future skew");
        SystemMonitorService clock(fixture.config(), mqtt(), publisher, "PRIVATE_DIAG");
        const auto anchor = std::chrono::steady_clock::time_point{} + std::chrono::seconds(1);
        require(SystemMonitorDiagnosticTestAccess::effectiveNow(clock, 1000000, anchor) == 1000000,
                "clock anchor initialization");
        for (int i = 1; i <= 1000; ++i) {
            const auto effective = SystemMonitorDiagnosticTestAccess::effectiveNow(clock, 999000,
                anchor + std::chrono::microseconds(i * 500));
            require(effective == 1000000 + i / 2, "submillisecond duration was discarded");
        }
        const auto afterRetention = SystemMonitorDiagnosticTestAccess::effectiveNow(clock, 999000,
            anchor + std::chrono::minutes(10));
        require(afterRetention == 1600000 && 1299999 <= afterRetention - 300000,
                "future-edge timestamp stayed fresh after ten-minute retention");
        std::cout << "PASS future-boundary/ten-minute-retention/submillisecond-steady-floor\n";
    }
    {
        auto publisher = std::make_shared<Publisher>();
        setenv("DIAG_FIXTURE_MODE", "stall", 1);
        {
            SystemMonitorService service(fixture.config(), mqtt(), publisher, "PRIVATE_DIAG");
            publisher->request("held"); tick(service, 8000);
            const auto launchedBy = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            const int before = fixture.runs();
            while (fixture.runs() == before && std::chrono::steady_clock::now() < launchedBy) {
                tick(service, 8000); std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            publisher->request("restart_case"); tick(service, 8001);
            require(publisher->replies.back().find("\"reason\":\"busy\"") != std::string::npos &&
                    publisher->replies.back().find("\"cmdId\":") == std::string::npos,
                    "pre-restart refusal must be nonterminal");
            service.stop();
        }
        setenv("DIAG_FIXTURE_MODE", "success", 1);
        const auto priorReplies = publisher->replies.size();
        SystemMonitorService restarted(fixture.config(), mqtt(), publisher, "PRIVATE_DIAG");
        publisher->request("restart_case"); tick(restarted, 8002);
        waitReply(restarted, *publisher, priorReplies + 1);
        require(publisher->replies.back().find("\"cmdId\":\"restart_case\"") != std::string::npos &&
                publisher->replies.back().find("\"replyKind\":\"terminal\"") != std::string::npos,
                "restart delivery must not contradict an earlier terminal refusal");
        std::cout << "PASS restart-window-refusal-is-unknown-until-terminal\n";
    }
    {
        auto publisher = std::make_shared<Publisher>();
        SystemMonitorService service(fixture.config(), mqtt(), publisher, "PRIVATE_DIAG");
        for (int i = 0; i < 16; ++i) {
            publisher->request("saved_" + std::to_string(i)); tick(service, 4000 + i);
            waitReply(service, *publisher, i + 1);
        }
        const int runs = fixture.runs();
        publisher->request("overflow"); tick(service, 5000);
        require(publisher->replies.back().find("cache_full") != std::string::npos &&
                publisher->replies.back().find("\"replyKind\":\"admission_rejection\"") != std::string::npos &&
                publisher->replies.back().find("\"cmdId\":") == std::string::npos &&
                fixture.runs() == runs, "bounded cache evicted unexpired id or launched overflow");
        std::cout << "PASS bounded-cache-no-extra-execution\n";
    }
    {
        auto publisher = std::make_shared<Publisher>(); publisher->failOnce = true;
        SystemMonitorService service(fixture.config(), mqtt(), publisher, "PRIVATE_DIAG");
        publisher->request("retry"); tick(service, 6000);
        for (int i = 0; i < 100; ++i) { tick(service, 6001); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        const int runs = fixture.runs();
        publisher->request("retry"); tick(service, 6002);
        require(publisher->replies.size() == 1 && fixture.runs() == runs &&
                publisher->replies.back().find("\"success\":true") != std::string::npos,
                "delivery failure lost idempotent terminal result");
        std::cout << "PASS failed-delivery-replay\n";
    }
}
#endif
}
int main() {
#ifdef _WIN32
    std::cout << "SKIP real SystemMonitor/POSIX fixture; no Linux substitute\n"; return 77;
#else
    try { cases(); std::cout << "system_monitor_diagnostic_test passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << "\n"; return 1; }
#endif
}
