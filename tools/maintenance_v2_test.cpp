// Maintenance v2 unit tests: canonical JSON, field-ticket vectors (shared with the
// Windows client), device audit hash chain, sessions, certificate fingerprint and the
// config transaction runner (apply / health-check rollback / restore) with a fake
// services adapter. No network, no systemd, no real device.
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/system_monitor_direct_maintenance.hpp"
#include "../src/maintenance_v2/v2_crypto.hpp"
#include "../src/maintenance_v2/v2_files.hpp"
#include "../src/maintenance_v2/v2_json.hpp"
#include "../src/maintenance_v2/v2_ota.hpp"
#include "../src/maintenance_v2/v2_state.hpp"
#include "../src/maintenance_v2/v2_ticket.hpp"
#include "../src/maintenance_v2/v2_tx.hpp"

using namespace edge_gateway;
using namespace edge_gateway::maintenance_v2;

namespace {

// Test data lives in the source tree; on a target board (tests copied to /tmp) GZ_EDGE_TEST_SOURCE_DIR points at the copied tree.
std::string testdataDir() {
    const char* root = std::getenv("GZ_EDGE_TEST_SOURCE_DIR");
    return root != nullptr && *root != '\0' ? std::string(root) + "/tools/testdata" : std::string(MAINTENANCE_V2_TESTDATA);
}

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::cerr << "FAIL: " << what << std::endl;
    }
}

// Scratch directories go under $TMPDIR (default /tmp); on a target board the caller points TMPDIR at its own directory and removes it.
std::string tempDir(const std::string& name) {
    const char* base = std::getenv("TMPDIR");
    char pattern[512];
    std::snprintf(pattern, sizeof(pattern), "%s/gz-maint-v2-%s-XXXXXX", base != nullptr && *base != '\0' ? base : "/tmp", name.c_str());
    const char* dir = ::mkdtemp(pattern);
    if (dir == nullptr) throw std::runtime_error("mkdtemp failed");
    return dir;
}

void writeText(const std::string& path, const std::string& text, mode_t mode = 0644) {
    makeDirectories(parentOf(path), 0755);
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    ::chmod(path.c_str(), mode);
}

void testCanonicalJson() {
    const auto v = parseJson("{\"b\":2,\"a\":{\"z\":[1,true,null,\"x\\u0001\"],\"\\u00e9\":1,\"y\":-0},\"\\ud83d\\ude00\":\"\\u20ac\"}");
    check(canonicalJson(v) == "{\"a\":{\"y\":0,\"z\":[1,true,null,\"x\\u0001\"],\"\xc3\xa9\":1},\"b\":2,\"\xf0\x9f\x98\x80\":\"\xe2\x82\xac\"}",
          "JCS key order (UTF-16) and escaping: " + canonicalJson(v));
    bool threw = false;
    try {
        canonicalJson(parseJson("{\"a\":1.5}"));
    } catch (...) {
        threw = true;
    }
    check(threw, "JCS rejects non-integer numbers");
    threw = false;
    try {
        canonicalJson(parseJson("{\"a\":1,\"a\":2}"));
    } catch (...) {
        threw = true;
    }
    check(threw, "JCS rejects duplicate keys");
    check(number(1792801002000.0) == "1792801002000", "integer formatting");
    check(number(0.1) == "0.1", "short round-trip formatting");
    check(number(std::numeric_limits<double>::quiet_NaN()) == "null", "NaN -> null");
    std::string out;
    check(base64UrlDecode("AQID", &out) && out == std::string("\x01\x02\x03"), "base64url decode");
    check(!base64UrlDecode("AQID=", &out), "base64url rejects padding");
    check(!base64Decode("A", &out), "base64 rejects bad length");
}

void testTicketVectors() {
    std::string text;
    check(readFile(testdataDir() + "/maintenance-v2-ticket-vectors.json", &text), "vectors file");
    const auto root = parseJson(text, 1000000);
    std::vector<TrustedKey> keys;
    for (const auto& k : root.find("platformKeys")->asArray().values) {
        TrustedKey key;
        key.keyId = str(*k, "keyId");
        check(base64Decode(str(*k, "publicKeyBase64"), &key.publicKey) && key.publicKey.size() == 32, "platform key decode");
        keys.push_back(key);
    }
    const auto dir = tempDir("replay");
    ReplayStore replay(dir + "/used-tickets.json");
    int n = 0;
    for (const auto& v : root.find("vectors")->asArray().values) {
        const auto name = str(*v, "name");
        const auto* device = v->find("device");
        TicketRequirement req;
        req.machineCode = str(*device, "machineCode");
        integer(*device, "nowMs", &req.nowMs);
        req.action = str(*device, "action");
        std::int64_t level = 0;
        integer(*device, "level", &level);
        req.level = static_cast<int>(level);
        req.paths = strings(*device, "paths");
        if (const auto* points = device->find("points")) {
            for (const auto& p : points->asArray().values) req.points.push_back(static_cast<std::uint32_t>(p->asNumber()));
        }
        auto verdict = verifyTicket(str(*v, "ticket"), keys, req);
        if (verdict.ok && verdict.ticket.oneTime && !replay.markUsed(verdict.ticket, req.nowMs)) {
            verdict.ok = false;
            verdict.code = "replayed";
        }
        const auto expect = str(*v, "expect");
        const auto got = verdict.ok ? std::string("accept") : "reject:" + verdict.code;
        check(got == expect, "vector " + name + ": expected " + expect + ", got " + got + " (" + verdict.message + ")");
        if (verdict.ok) check(canonicalJson(parseJson(str(*v, "payloadJcs"))) == str(*v, "payloadJcs"), "vector payload is canonical");
        ++n;
    }
    check(n == 12, "12 ticket vectors");
    // A trailing byte in the payload is not canonical any more.
    TicketRequirement req;
    check(verifyTicket("", keys, req).code == "ticket_required", "missing ticket");
    check(verifyTicket("e30.AA", keys, req).code == "malformed", "non-canonical / short ticket is malformed");
    check(requiredLevel("control.batch", {455}) == 2 && requiredLevel("control.batch", {455, 161}) == 3, "safety parameters need L3");
    check(requiredLevel("config.apply", {}) == 3 && requiredLevel("debug.enter", {}) == 2, "action levels");
}

void testFingerprint() {
    std::string text;
    readFile(testdataDir() + "/maintenance-v2-ticket-vectors.json", &text);
    const auto root = parseJson(text, 1000000);
    const auto* tls = root.find("tlsFingerprint");
    check(certificateFingerprint(str(*tls, "certPem")) == str(*tls, "sha256OfDer"), "certificate fingerprint format");
}

void testAudit() {
    const auto dir = tempDir("audit");
    AuditLog log(dir + "/audit.jsonl");
    for (int i = 0; i < 3; ++i) {
        AuditEntry e;
        e.operatorName = "\xe7\x8e\xb0\xe5\x9c\xba";
        e.ticketId = "FT-" + std::to_string(i);
        e.action = i == 2 ? "control.batch" : "config.apply";
        e.target = "/opt/x\"y";
        e.result = "ok";
        check(log.append(e), "audit append");
    }
    const auto entries = log.since(0, 100);
    check(entries.size() == 3, "audit entries");
    std::string prev(64, '0');
    for (std::size_t i = 0; i < entries.size(); ++i) {
        check(entries[i].seq == static_cast<std::int64_t>(i + 1), "audit seq");
        check(entries[i].prevHash == prev, "audit chain links");
        check(AuditLog::hashOf(entries[i]) == entries[i].hash, "audit hash recomputes");
        prev = entries[i].hash;
    }
    // Hash input is prevHash + JCS(entry without hash): the writer emits JCS order.
    const auto jcs = canonicalJson(parseJson(AuditLog::toJson(entries[0], false)));
    check(jcs == AuditLog::toJson(entries[0], false), "audit entry serialisation is JCS");
    check(log.since(2, 100).size() == 1, "audit sinceSeq");
}

void testSessions() {
    const auto dir = tempDir("sessions");
    SessionStore store(dir + "/sessions.json", 1000, 5000);
    std::int64_t expires = 0;
    const auto token = store.create("10.0.0.1", 1000000, &expires);
    check(token.size() >= 40 && expires == 1001000, "session token");
    check(store.touch(token, 1000500, nullptr) == SessionStore::Check::Ok, "session valid");
    SessionStore reloaded(dir + "/sessions.json", 1000, 5000);
    check(reloaded.touch(token, 1000900, nullptr) == SessionStore::Check::Ok, "session survives restart");
    check(reloaded.touch(token, 1003000, nullptr) == SessionStore::Check::Expired, "idle session expires");
    check(reloaded.touch("nope", 1000000, nullptr) == SessionStore::Check::Expired, "unknown token");
    const auto t2 = reloaded.create("10.0.0.1", 2000000, &expires);
    reloaded.revoke(t2);
    check(reloaded.touch(t2, 2000001, nullptr) != SessionStore::Check::Ok, "revoked token");
}

struct TxFixture {
    std::string root;
    std::string runtime;
    std::string stateFile;
    DirectConfig config;
    Options options;
};

TxFixture makeTxFixture(const std::string& name) {
    TxFixture f;
    f.root = tempDir(name);
    f.runtime = f.root + "/config/runtime";
    writeText(f.runtime + "/device_identity.json", "{\"schemaVersion\":\"1.0.0\",\"machineCode\":\"TEST0001\"}\n");
    writeText(f.runtime + "/logic/graph.json", "{\"version\":1}\n");
    f.stateFile = f.root + "/unit-state";
    writeText(f.stateFile, "active\n");
    const auto script = f.root + "/fake-services.sh";
    writeText(script,
              "#!/bin/sh\n"
              "case \"$1\" in\n"
              "  list) echo fake-a.service; echo '# comment' ;;\n"
              "  state) cat '" + f.stateFile + "' ;;\n"
              "  restart) echo restarted >> '" + f.root + "/restarts' ;;\n"
              "esac\n",
              0755);
    f.config.identityConfigFile = f.runtime + "/device_identity.json";
    f.config.v2.enabled = true;
    f.config.v2.healthTimeoutSec = 2;
    f.config.v2.healthStableSec = 0;
    f.options.stateDirectory = f.root + "/state";
    f.options.materialDirectory = f.root + "/material";
    f.options.servicesCommand = script;
    makeDirectories(f.options.stateDirectory + "/transactions", 0700);
    return f;
}

PlannedFile plan(const std::string& path, const std::string& content) {
    PlannedFile p;
    p.path = path;
    p.content = content;
    p.existed = isRegularFile(path);
    std::string current;
    if (p.existed) readFile(path, &current);
    p.changed = !p.existed || current != content;
    p.beforeSha256 = p.existed ? Crypto::instance().sha256Hex(current) : std::string();
    return p;
}

int countLines(const std::string& path) {
    std::string text;
    readFile(path, &text);
    int n = 0;
    for (const char c : text) n += c == '\n';
    return n;
}

void testTransactions() {
    // 1. healthy: committed, files written, services restarted once
    {
        auto f = makeTxFixture("tx-ok");
        TxStore store(f.config, f.options);
        const auto graph = f.runtime + "/logic/graph.json";
        const auto created = f.runtime + "/logic/new-graph.json";
        auto tx = store.create("REQ-1", "op", "u-1", "FT-1", "test", {plan(graph, "{\"version\":2}\n"), plan(created, "{\"a\":1}\n")});
        Transaction found;
        check(store.findByRequestId("REQ-1", &found) && found.id == tx.id, "find by requestId");
        check(store.findActive(&found), "accepted transaction is active");
        TxRunner runner(f.config, f.options);
        check(runner.apply(tx.id) == 0, "apply returns 0");
        check(store.load(tx.id, &found) && found.state == "committed", "committed: " + found.state + " " + found.message);
        check(readFileOr(graph) == "{\"version\":2}\n" && readFileOr(created) == "{\"a\":1}\n", "files written");
        check(countLines(f.root + "/restarts") == 1, "services restarted once");
        check(found.services.size() == 2 && found.services[1].name == "fake-a.service" && found.services[1].active, "services recorded");
        check(!store.findActive(&found), "no active transaction after commit");
        // client restore after commit
        check(store.load(tx.id, &found), "reload");
        std::string error;
        check(runner.restoreFiles(&found, &error), "restore files: " + error);
        check(readFileOr(graph) == "{\"version\":1}\n" && !exists(created), "restore returns the backup and removes created files");
        check(found.restoredFiles == 2, "restored file count");
    }
    // 2. unhealthy: automatic rollback
    {
        auto f = makeTxFixture("tx-bad");
        writeText(f.stateFile, "failed\n");
        TxStore store(f.config, f.options);
        const auto graph = f.runtime + "/logic/graph.json";
        auto tx = store.create("REQ-2", "op", "u-1", "FT-2", "test", {plan(graph, "{\"version\":3}\n")});
        TxRunner runner(f.config, f.options);
        runner.apply(tx.id);
        Transaction found;
        check(store.load(tx.id, &found) && found.state == "rolled-back", "unhealthy -> rolled-back: " + found.state);
        check(found.healthKnown && !found.healthOk && !found.healthFailed.empty() && found.healthFailed[0] == "gateway-services.service",
              "health failure recorded");
        check(found.rollbackPerformed && found.restoredFiles == 1 && found.rollbackServices == "failed", "rollback recorded");
        check(readFileOr(graph) == "{\"version\":1}\n", "file restored after unhealthy apply");
        check(countLines(f.root + "/restarts") == 2, "restart after rollback");
        const auto audit = AuditLog(f.options.stateDirectory + "/audit.jsonl").since(0, 10);
        check(!audit.empty() && audit.back().result == "rolled-back" && audit.back().transactionId == tx.id, "rollback audited");
    }
    // 3. file changed on the device after the dry-run: nothing written
    {
        auto f = makeTxFixture("tx-race");
        TxStore store(f.config, f.options);
        const auto graph = f.runtime + "/logic/graph.json";
        auto tx = store.create("REQ-3", "op", "u-1", "FT-3", "test", {plan(graph, "{\"version\":4}\n")});
        writeText(graph, "{\"version\":99}\n");
        TxRunner runner(f.config, f.options);
        runner.apply(tx.id);
        Transaction found;
        check(store.load(tx.id, &found) && found.state == "rolled-back" && !found.rollbackPerformed, "changed file: nothing written");
        check(readFileOr(graph) == "{\"version\":99}\n", "concurrent edit kept");
    }
    // 4. client restore while in progress (abort marker before the helper runs)
    {
        auto f = makeTxFixture("tx-abort");
        TxStore store(f.config, f.options);
        const auto graph = f.runtime + "/logic/graph.json";
        auto tx = store.create("REQ-4", "op", "u-1", "FT-4", "test", {plan(graph, "{\"version\":5}\n")});
        store.requestAbort(tx.id, "health not confirmed", "RESTORE-4", "FT-R");
        TxRunner runner(f.config, f.options);
        runner.apply(tx.id);
        Transaction found;
        check(store.load(tx.id, &found) && found.state == "rolled-back" && found.restoreTicketId == "FT-R", "abort before writing");
        check(readFileOr(graph) == "{\"version\":1}\n", "abort leaves the file");
    }
    // 5. invalid staged content is rejected by the loader before writing
    {
        auto f = makeTxFixture("tx-invalid");
        TxStore store(f.config, f.options);
        const auto graph = f.runtime + "/logic/graph.json";
        auto tx = store.create("REQ-5", "op", "u-1", "FT-5", "test", {plan(graph, "[1,2]\n")});
        TxRunner runner(f.config, f.options);
        runner.apply(tx.id);
        Transaction found;
        check(store.load(tx.id, &found) && found.state == "rolled-back" && found.message.find("nothing was written") == 0, "loader rejects");
    }
    // 6. interrupted after writing: recover() restores the backup
    {
        auto f = makeTxFixture("tx-recover");
        TxStore store(f.config, f.options);
        const auto graph = f.runtime + "/logic/graph.json";
        auto tx = store.create("REQ-6", "op", "u-1", "FT-6", "test", {plan(graph, "{\"version\":6}\n")});
        // simulate a helper that died during health-check
        writeText(store.directory(tx.id) + "/backup/0", "{\"version\":1}\n", 0600);
        writeText(graph, "{\"version\":6}\n");
        Transaction t;
        store.load(tx.id, &t);
        t.state = "health-check";
        t.files[0].written = true;
        t.restartedServices = true;
        store.save(t);
        TxRunner runner(f.config, f.options);
        runner.recover(tx.id);
        Transaction found;
        check(store.load(tx.id, &found) && found.state == "rolled-back" && readFileOr(graph) == "{\"version\":1}\n", "recover rolls back");
    }
}

PlannedFile planRemove(const std::string& path) {
    PlannedFile p;
    p.path = path;
    p.remove = true;
    p.existed = isRegularFile(path);
    std::string current;
    if (p.existed) readFile(path, &current);
    p.changed = p.existed;
    p.beforeSha256 = p.existed ? Crypto::instance().sha256Hex(current) : std::string();
    return p;
}

void testDeleteTransactions() {
    // 1. delete + edit in one transaction: committed, file gone, audited as config.delete; client restore brings it back
    {
        auto f = makeTxFixture("tx-del");
        const auto graph = f.runtime + "/logic/graph.json";
        const auto extra = f.runtime + "/devices/device_extra.json";
        writeText(extra, "{\"x\":1}\n");
        TxStore store(f.config, f.options);
        auto tx = store.create("REQ-D1", "op", "u-1", "FT-D1", "test", {plan(graph, "{\"version\":7}\n"), planRemove(extra)}, "delete");
        TxRunner runner(f.config, f.options);
        check(runner.apply(tx.id) == 0, "delete apply returns 0");
        Transaction found;
        check(store.load(tx.id, &found) && found.state == "committed" && found.operation == "delete", "delete committed: " + found.state);
        check(!exists(extra) && readFileOr(graph) == "{\"version\":7}\n", "file removed, other file written");
        check(found.files.size() == 2 && found.files[1].remove && found.files[1].afterSha256.empty(), "removed file journaled without an after hash");
        const auto view = transactionJson(found);
        check(view.find("\"deleted\":true") != std::string::npos && view.find("\"operation\":\"delete\"") != std::string::npos, "client view marks the deletion");
        const auto audit = AuditLog(f.options.stateDirectory + "/audit.jsonl").since(0, 10);
        check(!audit.empty() && audit.back().action == "config.delete" && audit.back().result == "committed", "audited as config.delete");
        std::string error;
        check(runner.restoreFiles(&found, &error), "restore after delete: " + error);
        check(readFileOr(extra) == "{\"x\":1}\n" && readFileOr(graph) == "{\"version\":1}\n", "restore recreates the deleted file");
    }
    // 2. unhealthy after delete: automatic rollback recreates the file
    {
        auto f = makeTxFixture("tx-del-bad");
        writeText(f.stateFile, "failed\n");
        const auto extra = f.runtime + "/devices/device_extra.json";
        writeText(extra, "{\"x\":2}\n");
        TxStore store(f.config, f.options);
        auto tx = store.create("REQ-D2", "op", "u-1", "FT-D2", "test", {planRemove(extra)}, "delete");
        TxRunner runner(f.config, f.options);
        runner.apply(tx.id);
        Transaction found;
        check(store.load(tx.id, &found) && found.state == "rolled-back" && readFileOr(extra) == "{\"x\":2}\n", "unhealthy delete rolled back");
    }
    // 3. the file changed after the dry-run: nothing deleted
    {
        auto f = makeTxFixture("tx-del-race");
        const auto extra = f.runtime + "/devices/device_extra.json";
        writeText(extra, "{\"x\":3}\n");
        TxStore store(f.config, f.options);
        auto tx = store.create("REQ-D3", "op", "u-1", "FT-D3", "test", {planRemove(extra)}, "delete");
        writeText(extra, "{\"x\":4}\n");
        TxRunner runner(f.config, f.options);
        runner.apply(tx.id);
        Transaction found;
        check(store.load(tx.id, &found) && found.state == "rolled-back" && readFileOr(extra) == "{\"x\":4}\n", "changed file not deleted");
    }
}

// The "ota": {...} object of an app config replaced (brace matching, strings skipped).
std::string replaceOtaSection(const std::string& text, const std::string& replacement) {
    const auto key = text.find("\"ota\"");
    const auto open = key == std::string::npos ? std::string::npos : text.find('{', key);
    if (open == std::string::npos) return text;
    int depth = 0;
    bool inString = false;
    for (std::size_t i = open; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            if (c == '\\') ++i;
            else if (c == '"') inString = false;
        } else if (c == '"') {
            inString = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}' && --depth == 0) {
            return text.substr(0, open) + replacement + text.substr(i + 1);
        }
    }
    return text;
}

std::string lastAuditResult(const Options& options, std::string* action) {
    const auto entries = AuditLog(options.stateDirectory + "/audit.jsonl").since(0, 1000);
    if (entries.empty()) return std::string();
    if (action) *action = entries.back().action;
    return entries.back().result;
}

void testOta() {
    {
        std::string data(200000, 'x');
        data[12345] = 'y';
        Sha256Stream s;
        s.update(data.data(), 70000);
        s.update(data.data() + 70000, data.size() - 70000);
        check(s.ok() && s.finishHex() == Crypto::instance().sha256Hex(data), "incremental sha256 matches one-shot sha256");
    }
    auto f = makeTxFixture("ota");
    std::string factory;
    check(readFile(testdataDir() + "/../../config/factory/runtime/apps/mqtt-service.json", &factory), "factory mqtt config");
    // The helper runs the device's OTA service (signature is checked at admission, so the device config may leave it off here).
    const auto otaJson = "{\"enabled\": true, \"currentVersion\": \"1.0.0\", \"downloadDir\": \"" + f.root + "/ota/downloads\", \"stagingDir\": \"" + f.root +
                         "/ota/staging\", \"backupDir\": \"" + f.root + "/ota/backup\", \"applyScript\": \"" + f.root +
                         "/apply.sh\", \"rollbackScript\": \"\", \"checksumRequired\": true, \"requireSignature\": false, \"upgradeTimeoutSec\": 30,"
                         " \"downloadRetryCount\": 1, \"minFreeBytes\": 0}";
    writeText(f.root + "/ota-app.json", replaceOtaSection(factory, otaJson));
    // Like the official ota-apply.sh: a failure after the "prepared" barrier and before the runtime changes proves "unchanged".
    writeText(f.root + "/apply.sh",
              "#!/bin/sh\necho \"$@\" >> '" + f.root + "/apply-args'\n"
              "if [ -f '" + f.root + "/apply-fail' ]; then\n"
              "  printf '{\"format\":\"official-ota-apply-barrier-v1\",\"jobId\":\"%s\",\"version\":\"%s\",\"token\":\"%s\",\"state\":\"prepared\"}\\n' \"$3\" \"$2\" \"$6\" > \"$5/ota_barrier_$3.json\"\n"
              "  exit 1\n"
              "fi\nexit 0\n",
              0755);
    f.config.otaAppConfigFile = f.root + "/ota-app.json";
    makeDirectories(f.root + "/ota", 0755);  // on a device /opt/modbus-gateway/ota exists; the OTA service creates only its leaf directories
    OtaConfig ota;
    std::string error;
    check(loadOtaConfig(f.config, &ota, &error) && ota.enabled && ota.applyScript == f.root + "/apply.sh", "ota config loads: " + error);
    OtaStore store(f.options);
    auto stage = [&](const std::string& uploadId, const std::string& jobId, const std::string& version, const std::string& bytes) {
        writeText(store.uploadPath(uploadId), bytes, 0600);
        writeText(store.uploadsDir() + "/" + uploadId + ".json",
                  "{\"uploadId\":\"" + uploadId + "\",\"sha256\":\"" + Crypto::instance().sha256Hex(bytes) + "\",\"size\":" + std::to_string(bytes.size()) + "}\n");
        OtaJobRecord job;
        job.jobId = jobId;
        job.uploadId = uploadId;
        job.machineCode = "TEST0001";
        job.packageType = "full";
        job.version = version;
        job.sha256 = Crypto::instance().sha256Hex(bytes);
        job.size = bytes.size();
        job.operatorName = "tester";
        job.ticketId = "FT-OTA";
        job.acceptedAt = nowMs();
        std::string e;
        check(store.saveJob(job, &e), "save job " + jobId + ": " + e);
        return job;
    };

    // 1. success: the OTA service copies the upload, verifies the checksum, runs the apply script and finalizes
    auto job = stage("UP-1", "JOB-1", "1.2.0", std::string("pkg-1\n") + std::string(5000, 'a'));
    OtaUploadInfo info;
    check(store.loadUpload("UP-1", &info) && info.size == 5006, "upload metadata");
    check(runOtaHelper(f.config, f.options, "JOB-1", false) == 0, "ota helper completes");
    OtaJobRecord loaded;
    check(store.loadJob("JOB-1", &loaded) && loaded.finalStage == "completed", "job finalized as completed: " + loaded.finalStage);
    auto view = store.view(ota, loaded);
    check(view.stage == "completed" && view.healthOk && view.progress == 100, "view completed: " + view.stage);
    check(readFileOr(f.root + "/apply-args").find("1.2.0 JOB-1") != std::string::npos, "apply script called with version and job");
    check(!exists(store.uploadPath("UP-1")), "upload removed after the job");
    std::string action;
    check(lastAuditResult(f.options, &action) == "completed" && action == "ota.job", "audited ota.job/completed");
    check(runOtaHelper(f.config, f.options, "JOB-1", true) == 0, "watching a finished job returns at once");
    check(AuditLog(f.options.stateDirectory + "/audit.jsonl").since(0, 1000).size() == 1, "terminal result audited once");

    // 2. apply script fails before touching the runtime: failed, device unchanged
    writeText(f.root + "/apply-fail", "1");
    stage("UP-2", "JOB-2", "1.3.0", "pkg-2\n");
    check(runOtaHelper(f.config, f.options, "JOB-2", false) == 1, "failing job returns 1");
    check(store.loadJob("JOB-2", &loaded) && loaded.finalStage == "failed", "job finalized as failed: " + loaded.finalStage);
    view = store.view(ota, loaded);
    check(view.stage == "failed" && !view.rollbackPerformed && view.message.find("device unchanged") != std::string::npos, "failed view: " + view.message);
    check(readFileOr(ota.stagingDir + "/current_version.txt").find("1.2.0") != std::string::npos, "installed version unchanged");
    removeFile(f.root + "/apply-fail");

    // 3. evidence mapping (what the apply script / health helper write on a real device)
    OtaJobRecord synthetic;
    synthetic.jobId = "JOB-3";
    writeText(ota.stagingDir + "/ota_final_JOB-3.txt", "jobId=JOB-3\ntoken=t\nstatus=failed\n");
    writeText(ota.stagingDir + "/health_JOB-3.txt", "{\"format\":\"ota-health-v1\",\"status\":\"failed\",\"rollback\":\"succeeded\"}\n");
    view = store.view(ota, synthetic);
    check(view.stage == "rolled-back" && view.rollbackPerformed && view.healthKnown && !view.healthOk, "rolled back after unhealthy restart");
    writeText(ota.stagingDir + "/health_JOB-3.txt", "{\"format\":\"ota-health-v1\",\"status\":\"failed\",\"rollback\":\"failed\"}\n");
    view = store.view(ota, synthetic);
    check(view.stage == "failed" && view.rollbackFailed, "rollback failure is reported");
    synthetic.jobId = "JOB-4";
    writeText(ota.stagingDir + "/JOB-4/health_pending.txt", "pending\n");
    view = store.view(ota, synthetic);
    check(view.stage == "health-check" && !view.terminal(), "restart and health check pending");
    synthetic.jobId = "JOB-5";
    OtaStatus s;
    s.stage = "downloading";
    s.downloadedBytes = 50;
    s.totalBytes = 100;
    makeDirectories(store.jobDirectory("JOB-5"));
    store.appendStatus("JOB-5", s);
    view = store.view(ota, synthetic);
    check(view.stage == "verifying" && view.progress == 30, "copying the upload reports verifying");
    s.stage = "applying";
    s.message = "waiting for restart and health result";
    store.appendStatus("JOB-5", s);
    check(store.view(ota, synthetic).stage == "health-check", "waiting for restart is the health check");

    // 4. uploads no unfinished job uses are pruned
    stage("UP-6", "JOB-6", "1.4.0", "pkg-6\n");
    writeText(store.uploadPath("UP-7"), "stale", 0600);
    writeText(store.uploadsDir() + "/UP-7.json", "{}\n");
    writeText(store.uploadsDir() + "/UP-8.pkg.part", "partial", 0600);
    store.pruneUploads();
    check(exists(store.uploadPath("UP-6")) && !exists(store.uploadPath("UP-7")) && !exists(store.uploadsDir() + "/UP-8.pkg.part"), "prune keeps only uploads in use");
    check(!safeOtaId("../x") && !safeOtaId("") && safeOtaId("JOB-1.2_a"), "ids are path-safe");
}

void testV1ReadOnlyWhileV2() {
    DirectConfig config;
    config.v2.enabled = true;
    auto call = [&](const std::string& path, const std::string& body) {
        return system_monitor_direct_maintenance::handleHttpRequest(config, "POST", path, "", "", body, "127.0.0.1");
    };
    for (const auto* path : {"/api/v1/control/batch", "/api/v1/config/delete", "/api/v1/config/restore", "/api/v1/ota/jobs"}) {
        check(call(path, "{}").find("use_maintenance_v2") != std::string::npos, std::string("v1 write blocked: ") + path);
    }
    check(call("/api/v1/config/apply", "{\"dryRun\":false}").find("use_maintenance_v2") != std::string::npos, "v1 apply blocked");
    check(call("/api/v1/config/apply", "{\"dryRun\":true}").find("use_maintenance_v2") == std::string::npos, "v1 dry-run still allowed");
    config.v2.v1ReadOnly = false;
    check(call("/api/v1/control/batch", "{}").find("use_maintenance_v2") == std::string::npos, "v1ReadOnly=false keeps v1 writes");
    config.v2.enabled = false;
    config.v2.v1ReadOnly = true;
    check(call("/api/v1/control/batch", "{}").find("use_maintenance_v2") == std::string::npos, "v2 disabled: v1 unchanged");
}

void testConfigParse() {
    std::string text;
    check(readFile(testdataDir() + "/../../config/factory/runtime/apps/monitor-service.json", &text), "factory monitor config");
    const std::string marker = "\"maxRealtimePoints\": 2000";
    const auto pos = text.find(marker);
    check(pos != std::string::npos, "factory config has maxRealtimePoints");
    const auto dir = tempDir("config");
    auto withV2 = text;
    withV2.insert(pos + marker.size(), ",\"v2\":{\"enabled\":true,\"listenPort\":9555,\"healthTimeoutSec\":5,\"sessionTtlSec\":10,\"v1ReadOnly\":false}");
    writeText(dir + "/monitor-service.json", withV2);
    const auto app = ConfigLoader::loadAppConfigFromFile(dir + "/monitor-service.json");
    const auto& v2 = app.systemMonitor.directMaintenance.v2;
    check(v2.enabled && v2.listenPort == 9555 && !v2.v1ReadOnly, "v2 config parsed");
    check(v2.healthTimeoutSec == 10 && v2.sessionTtlSec == 60, "v2 config bounds applied");
    writeText(dir + "/plain.json", text);
    const auto plain = ConfigLoader::loadAppConfigFromFile(dir + "/plain.json");
    check(!plain.systemMonitor.directMaintenance.v2.enabled && plain.systemMonitor.directMaintenance.v2.listenPort == 9444, "v2 defaults");
}

}  // namespace

void testDebugMode() {
    const auto dir = tempDir("debug");
    const auto file = dir + "/debug-mode.json";
    DebugModeStore store(file);
    DebugMode m;
    m.active = true;
    m.since = 1000;
    m.lastActivityAt = 1000;
    m.operatorName = "张工";
    store.set(m);
    struct stat st {};
    check(::stat(file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0644, "debug-mode.json is 0644 (local display reads it)");
    DebugMode ended;
    check(store.exit(&ended) && ended.operatorName == "张工" && !store.get().active, "exit ends debug mode");
    check(!store.exit(&ended), "exit when not active");
    check(!store.touch(1000000000, 1800000, &ended) && !store.get().active, "touch after exit keeps it ended");
    // A request touching the mode (rewrite path: last activity > 60 s ago) races with an exit:
    // the exit must win every time, never the old active state written back.
    bool resurrected = false;
    for (int round = 0; round < 20 && !resurrected; ++round) {
        store.set(m);
        std::thread toucher([&] {
            for (int i = 1; i <= 40; ++i) store.touch(1000 + static_cast<std::int64_t>(i) * 61000, std::int64_t(1) << 50, nullptr);
        });
        std::thread exiter([&] {
            std::this_thread::sleep_for(std::chrono::microseconds(200 * (round % 5)));
            store.exit(nullptr);
        });
        toucher.join();
        exiter.join();
        resurrected = store.get().active;
    }
    check(!resurrected, "touch racing with exit never writes the active mode back");
}

int main() {
    if (!Crypto::instance().available()) {
        std::cerr << "libcrypto unavailable: " << Crypto::instance().error() << std::endl;
        return 1;
    }
    try {
        testCanonicalJson();
        testTicketVectors();
        testFingerprint();
        testAudit();
        testSessions();
        testDebugMode();
        testTransactions();
        testDeleteTransactions();
        testOta();
        testV1ReadOnlyWhileV2();
        testConfigParse();
    } catch (const std::exception& ex) {
        std::cerr << "exception: " << ex.what() << std::endl;
        return 1;
    }
    std::cout << "maintenance_v2_test: " << g_checks << " checks, " << g_failures << " failures" << std::endl;
    return g_failures == 0 ? 0 : 1;
}
