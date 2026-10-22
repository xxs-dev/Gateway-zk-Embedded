// Maintenance v2: request dispatch, read-only endpoints, session, debug mode,
// device audit and controlled point writes.
#include "v2_service.hpp"

#include "edge_gateway/control_dedup_store.hpp"
#include "edge_gateway/power_control_ownership.hpp"
#include "edge_gateway/priority_control_lease.hpp"
#include "edge_gateway/system_monitor_direct_maintenance.hpp"
#include "edge_gateway/system_monitor_direct_maintenance_bridge.hpp"
#include "edge_gateway/system_monitor_service.hpp"
#include "v2_crypto.hpp"
#include "v2_files.hpp"
#include "v2_json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <limits>
#include <set>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace edge_gateway {
namespace maintenance_v2 {

namespace bridge = system_monitor_direct_maintenance::bridge;

namespace {

const char* kPrefix = "/api/v2";
const std::size_t kMaxQueryPoints = 5000;
const char* kControlSource = "system-monitor-direct-maintenance";

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string urlDecode(const std::string& value) {
    std::string out;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            out += ' ';
        } else if (value[i] == '%' && i + 2 < value.size() && std::isxdigit(static_cast<unsigned char>(value[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(value[i + 2]))) {
            out += static_cast<char>(std::stoi(value.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += value[i];
        }
    }
    return out;
}

void writePoint(Writer& w, const StoredPointValue& p) {
    const bool finite = std::isfinite(p.value);
    w.beginObject()
        .fieldInt("index", p.index)
        .field("machineCode", p.machineCode)
        .field("meterCode", p.meterCode)
        .field("pointCode", p.pointCode)
        .fieldNum("value", p.value)
        .fieldInt("quality", finite ? p.quality : 0)
        .fieldInt("ts", p.ts)
        .fieldBool("stale", p.stale)
        .endObject();
}

std::string pointsBody(std::int64_t ts, const std::vector<StoredPointValue>& values, std::size_t limit) {
    const auto count = std::min(values.size(), limit);
    Writer w;
    w.beginObject().fieldInt("ts", ts).fieldInt("count", static_cast<std::int64_t>(count)).fieldInt("total", static_cast<std::int64_t>(values.size()))
        .fieldBool("truncated", count < values.size());
    w.key("points").beginArray();
    for (std::size_t i = 0; i < count; ++i) writePoint(w, values[i]);
    w.endArray().endObject();
    return w.text();
}

bool isEmsVirtualParameter(const PointStoreRoute& route) {
    return !route.derived && route.protocolType == "ems_virtual" && route.writable && route.write.enable;
}

// Same range rules as the Modbus codec / EMS virtual parameter validation.
std::string rangeProblem(const PointStoreRoute& route, double value) {
    if (!std::isfinite(value)) return "value must be finite";
    const auto& w = route.write;
    if (w.minValue && value < *w.minValue) return "value below min";
    if (w.maxValue && value > *w.maxValue) return "value above max";
    if (w.step > 0.0) {
        const double base = w.minValue ? *w.minValue : 0.0;
        const double ratio = (value - base) / w.step;
        if (std::fabs(ratio - std::round(ratio)) > 1e-7) return "value does not match step";
    }
    if (!w.allowedValues.empty() &&
        std::none_of(w.allowedValues.begin(), w.allowedValues.end(), [value](double c) { return std::fabs(c - value) <= 1e-9; })) {
        return "value is not in allowedValues";
    }
    return std::string();
}

std::string randomId(const char* prefix) {
    std::string raw;
    Crypto::instance().randomBytes(8, &raw);
    return std::string(prefix) + toHex(raw);
}

}  // namespace

std::string Request::header(const std::string& name) const {
    const auto it = headers.find(lower(name));
    return it == headers.end() ? std::string() : it->second;
}

std::string Request::queryValue(const std::string& name) const {
    std::size_t pos = 0;
    while (pos <= query.size()) {
        const auto amp = query.find('&', pos);
        const auto part = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const auto eq = part.find('=');
        if (urlDecode(part.substr(0, eq)) == name) return eq == std::string::npos ? std::string() : urlDecode(part.substr(eq + 1));
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return std::string();
}

Response jsonResponse(int status, const std::string& body) {
    Response r;
    r.status = status;
    r.body = body;
    return r;
}

Response errorResponse(int status, const std::string& code, const std::string& message) {
    return jsonResponse(status, errorBody(code, message));
}

Service::Service(const DirectConfig& config, const Options& options, std::string certFingerprint)
    : config_(config),
      options_(options),
      fingerprint_(std::move(certFingerprint)),
      sessions_(options.stateDirectory + "/sessions.json", static_cast<std::int64_t>(config.v2.sessionTtlSec) * 1000,
                static_cast<std::int64_t>(config.v2.sessionMaxSec) * 1000),
      replay_(options.stateDirectory + "/used-tickets.json"),
      audit_(options.stateDirectory + "/audit.jsonl"),
      debug_(options.stateDirectory + "/debug-mode.json"),
      txs_(config, options) {}

std::string Service::deviceMachineCode() {
    return bridge::machineCode(config_);
}

std::vector<std::string> Service::expectedUnits() {
    std::lock_guard<std::mutex> lock(unitsMutex_);
    const auto now = nowMs();
    if (unitsCache_.empty() || now - unitsCachedAt_ > 10000) {
        std::string error;
        unitsCache_ = Services(config_, options_).expectedUnits(&error);
        unitsCachedAt_ = now;
    }
    return unitsCache_;
}

void Service::record(const std::string& action, const Ticket& ticket, const std::string& target, const std::string& result,
                     const std::string& transactionId) {
    AuditEntry e;
    e.operatorName = ticket.operatorLabel();
    e.ticketId = ticket.ticketId;
    e.action = action;
    e.target = target.size() > 2000 ? target.substr(0, 2000) : target;
    e.result = result;
    e.transactionId = transactionId;
    std::string error;
    if (!audit_.append(e, &error)) std::cerr << "maintenance v2 audit: " << error << std::endl;
}

bool Service::authenticate(const Request& request, Response* deny, Auth* auth) {
    const auto header = request.header("authorization");
    const std::string bearer = "Bearer ";
    if (header.size() <= bearer.size() || header.compare(0, bearer.size(), bearer) != 0) {
        *deny = errorResponse(401, "token_required", "log in with POST /api/v2/auth/login");
        return false;
    }
    auth->token = header.substr(bearer.size());
    if (sessions_.touch(auth->token, nowMs(), nullptr) != SessionStore::Check::Ok) {
        *deny = errorResponse(401, "token_expired", "session expired; log in again");
        return false;
    }
    DebugMode ended;
    if (debug_.touch(nowMs(), static_cast<std::int64_t>(config_.v2.debugIdleSec) * 1000, &ended)) {
        Ticket t;
        t.ticketId = ended.ticketId;
        t.operatorName = ended.operatorName;
        record("debug.exit", t, std::string(), "idle-timeout");
    }
    return true;
}

TicketVerdict Service::checkTicket(const Request& request, const std::string& action, int level, const std::vector<std::string>& paths,
                                   const std::vector<std::uint32_t>& points, const std::string& replayOf, bool consume) {
    std::string warning;
    const auto keys = loadTrustedKeys(options_.materialDirectory + "/trusted-ticket-keys.json", &warning);
    if (!warning.empty()) std::cerr << "maintenance v2: " << warning << std::endl;
    TicketRequirement req;
    req.action = action;
    req.level = level;
    req.machineCode = deviceMachineCode();
    req.paths = paths;
    req.points = points;
    req.nowMs = nowMs();
    auto verdict = verifyTicket(request.header("x-gz-field-ticket"), keys, req);
    if (!verdict.ok || !verdict.ticket.oneTime || verdict.ticket.ticketId == replayOf) return verdict;
    if (consume ? !replay_.markUsed(verdict.ticket, req.nowMs) : replay_.used(verdict.ticket.ticketId)) {
        verdict.ok = false;
        verdict.code = "replayed";
        verdict.message = "one-time ticket already used";
    }
    return verdict;
}

void Service::housekeeping() {
    DebugMode ended;
    if (debug_.touch(nowMs(), static_cast<std::int64_t>(config_.v2.debugIdleSec) * 1000, &ended)) {
        Ticket t;
        t.ticketId = ended.ticketId;
        t.operatorName = ended.operatorName;
        record("debug.exit", t, std::string(), "idle-timeout");
    }
    // Transactions whose helper died (power loss, kill): roll back / finish the restart.
    for (const auto& id : txs_.ids()) {
        Transaction tx;
        if (!txs_.load(id, &tx)) continue;
        const bool pendingRestart = tx.state == "rolled-back" && tx.rollbackServices == "pending";
        if ((tx.finished() && !pendingRestart) || nowMs() - tx.updatedAt < 15000 || txs_.helperRunning(id)) continue;
        std::string error;
        if (!txs_.launchHelper(id, pendingRestart ? "restart" : "recover", &error)) {
            std::cerr << "maintenance v2: cannot recover " << id << ": " << error << std::endl;
        }
    }
}

Response Service::handle(const Request& r) {
    const std::string base = kPrefix;
    if (r.path.compare(0, base.size(), base) != 0) return errorResponse(404, "not_found", "endpoint not found");
    const auto p = r.path.substr(base.size());
    const bool get = r.method == "GET";
    const bool post = r.method == "POST";
    try {
        if (get && p == "/health") return health();
        if (get && p == "/identity") return identity();
        if (get && p == "/auth/state") return authState();
        if (get && p == "/system/status") return systemStatus();
        if (get && p == "/telemetry/full") return telemetryFull();
        if (post && p == "/points/query") return pointsQuery(r);
        if (get && p == "/ota/capabilities") return jsonResponse(200, system_monitor_direct_maintenance::otaCapabilitiesJson());
        if (post && p == "/auth/login") return login(r);

        Response deny;
        Auth auth;
        if (!authenticate(r, &deny, &auth)) return deny;
        if (post && p == "/auth/logout") return logout(r, auth);
        if (get && p == "/ota/status") return otaStatus();
        if (post && p == "/config/snapshot") return snapshot(r);
        if (post && p == "/diag/execute") return diag(r);
        if (get && p == "/debug-mode") return debugGet();
        if (post && p == "/debug-mode") return debugPost(r);
        if (get && p == "/audit") return auditGet(r);
        if (post && p == "/control/batch") return control(r);
        if (post && p == "/config/apply") return configApply(r);
        if (post && p == "/config/restore") return configRestore(r);
        const std::string txPrefix = "/config/transactions/";
        if (get && p.compare(0, txPrefix.size(), txPrefix) == 0) return transaction(p.substr(txPrefix.size()));
        if (p == "/config/delete" || p.compare(0, 8, "/ota/job") == 0 || p == "/ota/uploads" || p == "/logs" || p == "/realtime/stream") {
            return errorResponse(501, "not_implemented", "not available in this firmware (maintenance v2 phase 2)");
        }
        return errorResponse(404, "not_found", "endpoint not found");
    } catch (const std::exception& ex) {
        return errorResponse(500, "internal_error", ex.what());
    }
}

// ---------------------------------------------------------------- read-only

Response Service::health() {
    Writer w;
    w.beginObject().fieldBool("success", true).field("status", "ok").field("apiVersion", "2").field("maintenanceApi", "v2").fieldInt("ts", nowMs())
        .field("message", "SystemMonitor maintenance v2 is running").endObject();
    return jsonResponse(200, w.text());
}

Response Service::identity() {
    const auto id = bridge::identity(config_);
    Writer w;
    w.beginObject()
        .field("machineCode", id.machineCode.empty() ? deviceMachineCode() : id.machineCode)
        .field("imei", id.imei)
        .field("serialNumber", id.serialNumber)
        .field("model", id.model)
        .field("hardwareVersion", id.hardwareVersion)
        .field("firmwareVersion", id.firmwareVersion)
        .field("certFingerprint", fingerprint_)
        .field("apiVersion", "2");
    // No "mappingStage": the edge mapping stage (CQ15) is not in this firmware.
    w.fieldStrings("features", std::vector<std::string>());
    w.endObject();
    return jsonResponse(200, w.text());
}

Response Service::authState() {
    const auto s = bridge::authState(config_);
    Writer w;
    w.beginObject()
        .fieldBool("paired", s.paired)
        .fieldBool("usingDefaultPassword", s.usingDefaultPassword)
        .fieldBool("bootstrapMode", s.bootstrapMode)
        .fieldBool("sensitiveApiEnabled", s.sensitiveApiEnabled)
        .endObject();
    return jsonResponse(200, w.text());
}

Response Service::systemStatus() {
    const auto m = bridge::metrics();
    Services services(config_, options_);
    Writer w;
    w.beginObject()
        .field("machineCode", deviceMachineCode())
        .fieldNum("cpuUsage", m.cpuUsage)
        .fieldNum("memUsage", m.memUsage)
        .fieldNum("diskUsage", m.diskUsage)
        .fieldNum("load1", m.load1)
        .fieldInt("processCount", m.processCount);
    w.key("services").beginArray();
    for (const auto& unit : expectedUnits()) {
        w.beginObject().field("name", unit).field("state", services.state(unit)).fieldBool("expected", true).field("message", "").endObject();
    }
    w.endArray().fieldInt("ts", nowMs()).endObject();
    return jsonResponse(200, w.text());
}

Response Service::telemetryFull() {
    bridge::Realtime rt(config_);
    const auto ts = nowMs();
    const auto values = rt.router().getAllLatest(ts);
    const auto limit = config_.maxRealtimePoints <= 0 ? values.size() : static_cast<std::size_t>(config_.maxRealtimePoints);
    return jsonResponse(200, pointsBody(ts, values, limit));
}

Response Service::pointsQuery(const Request& r) {
    JsonValue body;
    try {
        body = parseJson(r.body, 100000);
    } catch (const std::exception& ex) {
        return errorResponse(400, "validation_failed", ex.what());
    }
    std::set<std::uint32_t> wanted;
    if (const auto* list = body.find("indexes")) {
        if (!list->isArray()) return errorResponse(400, "validation_failed", "indexes must be an array");
        for (const auto& item : list->asArray().values) {
            const double d = item->isNumber() ? item->asNumber() : -1;
            if (!(d >= 0 && d <= 4294967295.0) || std::floor(d) != d) return errorResponse(400, "validation_failed", "indexes must be unsigned integers");
            wanted.insert(static_cast<std::uint32_t>(d));
        }
    }
    const auto meters = strings(body, "meters");
    if (wanted.size() + meters.size() == 0) return errorResponse(400, "validation_failed", "indexes or meters is required");
    if (wanted.size() > kMaxQueryPoints) return errorResponse(400, "validation_failed", "too many indexes");
    bridge::Realtime rt(config_);
    const auto ts = nowMs();
    const auto machine = rt.machineCode();
    std::vector<StoredPointValue> values;
    std::set<std::uint32_t> seen;
    if (!wanted.empty()) {
        for (auto& v : rt.router().getLatestByIndexes(std::vector<std::uint32_t>(wanted.begin(), wanted.end()), ts)) {
            if (seen.insert(v.index).second) values.push_back(v);
        }
    }
    for (const auto& meter : meters) {
        for (auto& v : rt.router().getLatestByMeter(machine, meter, ts)) {
            if (seen.insert(v.index).second) values.push_back(v);
        }
    }
    std::sort(values.begin(), values.end(), [](const StoredPointValue& a, const StoredPointValue& b) { return a.index < b.index; });
    return jsonResponse(200, pointsBody(ts, values, kMaxQueryPoints));
}

// ---------------------------------------------------------------- session

Response Service::login(const Request& r) {
    std::string hash;
    try {
        hash = str(parseJson(r.body, 100), "passwordSha256");
    } catch (const std::exception&) {
        return errorResponse(400, "validation_failed", "body must be {\"passwordSha256\":\"<hex>\"}");
    }
    switch (bridge::authenticate(config_, hash, r.clientIp)) {
    case bridge::LoginResult::LockedOut:
        return errorResponse(429, "locked_out", "too many failed attempts; retry later");
    case bridge::LoginResult::NotProvisioned:
        return errorResponse(503, "not_provisioned", "maintenance credential is not provisioned (SystemMonitor --set-maintenance-password)");
    case bridge::LoginResult::Invalid:
        return errorResponse(401, "invalid_password", "invalid maintenance password");
    case bridge::LoginResult::Ok:
        break;
    }
    if (config_.requireSensitiveApiEnabled && !bridge::authState(config_).sensitiveApiEnabled) {
        return errorResponse(403, "sensitive_api_disabled", "authState.sensitiveApiEnabled=false");
    }
    std::int64_t expiresAt = 0;
    const auto token = sessions_.create(r.clientIp, nowMs(), &expiresAt);
    if (token.empty()) return errorResponse(503, "crypto_unavailable", "cannot create a session token (libcrypto)");
    Writer w;
    w.beginObject().fieldBool("success", true).field("token", token).fieldInt("expiresAt", expiresAt).field("scope", "maintenance").endObject();
    return jsonResponse(200, w.text());
}

Response Service::logout(const Request&, const Auth& auth) {
    sessions_.revoke(auth.token);
    return jsonResponse(200, "{\"success\":true}");
}

Response Service::otaStatus() {
    return jsonResponse(200, bridge::otaStatusJson(config_));
}

Response Service::snapshot(const Request& r) {
    std::string body;
    try {
        body = bridge::configSnapshotJson(config_, r.body);
    } catch (const std::exception& ex) {
        return errorResponse(400, "validation_failed", ex.what());
    }
    // v2 also writes runtime apps/devices/logic files the app config does not reference:
    // list them too, so the client sees what is really on the device.
    try {
        auto root = parseJson(body, 2000000);
        if (str(root, "scope") != "config") return jsonResponse(200, body);
        const auto* files = root.find("files");
        if (files == nullptr || !files->isArray()) return jsonResponse(200, body);
        std::set<std::string> listed;
        for (const auto& f : files->asArray().values) listed.insert(str(*f, "path"));
        Writer extra;
        extra.beginArray();
        std::int64_t added = 0;
        std::int64_t bytes = 0;
        const auto runtime = parentOf(config_.identityConfigFile);
        for (const char* sub : {"/apps", "/devices", "/logic"}) {
            const auto dir = runtime + sub;
            auto names = listDirectory(dir);
            std::sort(names.begin(), names.end());
            for (const auto& name : names) {
                const auto path = dir + "/" + name;
                if (name.size() < 6 || name.compare(name.size() - 5, 5, ".json") != 0 || listed.count(path) || !isRegularFile(path)) continue;
                std::string content;
                if (!readFile(path, &content) || content.size() > 4U * 1024U * 1024U) continue;
                struct stat st {};
                ::stat(path.c_str(), &st);
                extra.beginObject()
                    .field("path", path)
                    .fieldInt("sizeBytes", static_cast<std::int64_t>(content.size()))
                    .fieldInt("modifiedAtMs", static_cast<std::int64_t>(st.st_mtime) * 1000)
                    .field("encoding", "utf8")
                    .field("content", system_monitor_config_safety::redactSecrets(content))
                    .endObject();
                ++added;
                bytes += static_cast<std::int64_t>(content.size());
            }
        }
        extra.endArray();
        if (added == 0) return jsonResponse(200, body);
        std::string merged = toJson(*files);
        merged.pop_back();  // ']'
        if (!files->asArray().values.empty()) merged += ",";
        merged += extra.text().substr(1);  // items + ']'
        std::int64_t count = 0;
        std::int64_t total = 0;
        integer(root, "fileCount", &count);
        integer(root, "totalBytes", &total);
        Writer w;
        w.beginObject();
        for (const auto& m : root.asObject().values) {
            if (m.key == "files") w.key("files").raw(merged);
            else if (m.key == "fileCount") w.fieldInt("fileCount", count + added);
            else if (m.key == "totalBytes") w.fieldInt("totalBytes", total + bytes);
            else w.key(m.key).raw(toJson(*m.value));
        }
        w.endObject();
        return jsonResponse(200, w.text());
    } catch (const std::exception&) {
        return jsonResponse(200, body);
    }
}

Response Service::diag(const Request& r) {
    std::string out;
    try {
        const int status = bridge::diagExecute(config_, r.body, &out);
        return jsonResponse(status, out);
    } catch (const std::exception& ex) {
        return errorResponse(400, "validation_failed", ex.what());
    }
}

// ---------------------------------------------------------------- debug mode

Response Service::debugGet() {
    const auto m = debug_.get();
    Writer w;
    w.beginObject().fieldBool("active", m.active).fieldInt("since", m.since).field("operator", m.operatorName).field("reason", m.reason)
        .field("ticketId", m.ticketId).endObject();
    return jsonResponse(200, w.text());
}

Response Service::debugPost(const Request& r) {
    JsonValue body;
    try {
        body = parseJson(r.body, 100);
    } catch (const std::exception& ex) {
        return errorResponse(400, "validation_failed", ex.what());
    }
    if (!boolean(body, "enter", false)) {
        const auto m = debug_.get();
        if (m.active) {
            debug_.set(DebugMode());
            Ticket t;
            t.ticketId = m.ticketId;
            t.operatorName = m.operatorName;
            record("debug.exit", t, std::string(), "ok");
        }
        return jsonResponse(200, "{\"success\":true,\"active\":false}");
    }
    const auto verdict = checkTicket(r, "debug.enter", requiredLevel("debug.enter", {}), {}, {}, std::string(), true);
    if (!verdict.ok) return errorResponse(403, verdict.code, verdict.message);
    DebugMode m;
    m.active = true;
    m.since = nowMs();
    m.lastActivityAt = m.since;
    m.operatorName = verdict.ticket.operatorLabel();
    m.operatorId = verdict.ticket.operatorId;
    m.reason = str(body, "reason");
    m.ticketId = verdict.ticket.ticketId;
    debug_.set(m);
    record("debug.enter", verdict.ticket, m.reason, "ok");
    Writer w;
    w.beginObject().fieldBool("success", true).fieldBool("active", true).fieldInt("since", m.since).field("operator", m.operatorName)
        .field("ticketId", m.ticketId).endObject();
    return jsonResponse(200, w.text());
}

// ---------------------------------------------------------------- device audit

Response Service::auditGet(const Request& r) {
    std::int64_t sinceSeq = 0;
    std::size_t limit = 500;
    try {
        const auto s = r.queryValue("sinceSeq");
        if (!s.empty()) sinceSeq = std::stoll(s);
        const auto l = r.queryValue("limit");
        if (!l.empty()) limit = static_cast<std::size_t>(std::max(1LL, std::min(5000LL, std::stoll(l))));
    } catch (...) {
        return errorResponse(400, "validation_failed", "sinceSeq / limit must be integers");
    }
    const auto entries = audit_.since(sinceSeq, limit);
    std::string body = "{\"success\":true,\"count\":" + std::to_string(entries.size()) + ",\"entries\":[";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) body += ",";
        body += AuditLog::toJson(entries[i], true);
    }
    body += "]}";
    return jsonResponse(200, body);
}

// ---------------------------------------------------------------- controlled writes

Response Service::control(const Request& r) {
    JsonValue body;
    try {
        body = parseJson(r.body, 20000);
    } catch (const std::exception& ex) {
        return errorResponse(400, "validation_failed", ex.what());
    }
    static const char* kForbidden[] = {"highPriority", "priority", "priorityControl"};
    for (const auto* key : kForbidden) {
        if (hasKey(body, key)) return errorResponse(400, "high_priority_not_allowed", "external requests cannot raise the arbitration priority");
    }
    const auto device = deviceMachineCode();
    const auto machine = str(body, "machineCode");
    if (machine.empty() || machine != device) return errorResponse(400, "machine_mismatch", "machineCode must be " + device);
    const bool dryRun = boolean(body, "dryRun", true);
    const auto requestId = str(body, "requestId", randomId("CTRL_"));
    const auto* list = body.find("commands");
    if (list == nullptr || !list->isArray() || list->asArray().values.empty() || list->asArray().values.size() > 128) {
        return errorResponse(400, "validation_failed", "commands must contain 1..128 items");
    }
    struct Command {
        std::uint32_t index = 0;
        double value = 0;
        std::string cmdId;
    };
    std::vector<Command> commands;
    std::vector<std::uint32_t> points;
    for (const auto& item : list->asArray().values) {
        if (!item->isObject()) return errorResponse(400, "validation_failed", "command must be an object");
        for (const auto* key : kForbidden) {
            if (hasKey(*item, key)) return errorResponse(400, "high_priority_not_allowed", "external requests cannot raise the arbitration priority");
        }
        if (hasKey(*item, "source")) return errorResponse(400, "validation_failed", "source is set by the device");
        Command c;
        std::int64_t index = 0;
        if (!integer(*item, "index", &index) || index <= 0 || index > 4294967295LL) return errorResponse(400, "validation_failed", "index is required");
        const auto* value = item->find("value");
        if (value == nullptr || !value->isNumber() || !std::isfinite(value->asNumber())) return errorResponse(400, "validation_failed", "value must be a finite number");
        c.index = static_cast<std::uint32_t>(index);
        c.value = value->asNumber();
        c.cmdId = str(*item, "cmdId", randomId("V2_"));
        try {
            validateControlCommandId(c.cmdId);
        } catch (const std::exception& ex) {
            return errorResponse(400, "validation_failed", ex.what());
        }
        if (std::find(points.begin(), points.end(), c.index) != points.end()) return errorResponse(400, "validation_failed", "duplicate index");
        points.push_back(c.index);
        commands.push_back(c);
    }
    Ticket ticket;
    if (!dryRun) {
        const auto verdict = checkTicket(r, "control.batch", requiredLevel("control.batch", points), {}, points, std::string(), true);
        if (!verdict.ok) return errorResponse(403, verdict.code, verdict.message);
        ticket = verdict.ticket;
    }

    bridge::Realtime rt(config_);
    auto& router = rt.router();
    const auto& app = rt.app();
    PowerControlOwnership ownership(app.mqttDriver.powerControlOwnershipFile, "system-monitor");
    PriorityControlLease priority(app.mqttDriver.priorityControlLeaseFile, kControlSource);
    int accepted = 0;
    Writer results;
    results.beginArray();
    std::string target;
    for (const auto& c : commands) {
        results.beginObject().fieldInt("index", c.index).field("cmdId", c.cmdId).fieldNum("requested", c.value)
            .fieldInt("level", isSafetyParameter(c.index) ? 3 : 2);
        std::string reject;
        const auto route = router.routeByIndex(c.index);
        if (!route) {
            reject = "point not found";
        } else if (route->machineCode != device) {
            reject = "point belongs to " + route->machineCode;
        } else if (route->commandMailbox) {
            reject = "command mailbox routes are not writable through maintenance v2";
        } else if (!route->writable) {
            reject = "point write is disabled";
        } else {
            reject = rangeProblem(*route, c.value);
        }
        const auto now = nowMs();
        if (reject.empty() && priority.enabled() && priority.activeLease(now)) reject = "priority control in progress";
        if (reject.empty() && ownership.enabled()) {
            const auto authorization = ownership.authorize(c.index, kControlSource, 0, false, now);
            if (!authorization.allowed) reject = "power control ownership: " + authorization.message;
        }
        const std::string level = route && isEmsVirtualParameter(*route) ? "ems-parameter" : "device-write";
        if (route) results.field("meterCode", route->meterCode).field("pointCode", route->pointCode);
        if (!reject.empty()) {
            results.fieldBool("accepted", false).key("adoptedValue").null().field("arbitrationLevel", level).field("rejectReason", reject)
                .field("stage", dryRun ? "dry-run" : "rejected").endObject();
            if (!dryRun) target += (target.empty() ? "" : ",") + std::to_string(c.index) + ":rejected";
            continue;
        }
        if (dryRun) {
            ++accepted;
            results.fieldBool("accepted", true).fieldNum("adoptedValue", c.value).field("arbitrationLevel", level).field("stage", "dry-run").endObject();
            continue;
        }
        PendingWriteCommand command;
        command.cmdId = c.cmdId;
        command.index = c.index;
        command.value = c.value;
        command.source = kControlSource;
        command.ts = now;
        command.acceptedAt = now;
        command.highPriority = false;
        command.durableControl = router.isOrdinaryPhysicalWrite(c.index);
        const auto submit = router.submitWriteCommand(command);
        Optional<WritebackResultRecord> writeback = submit.writeback;
        if (submit.accepted && !writeback) {
            writeback = bridge::waitForWriteback(router, submit.route, c.cmdId, command.acceptedAt, app.mqttDriver.controlResultWaitTimeoutMs);
        }
        const bool ok = submit.accepted && writeback && writeback->success;
        const std::string message = !submit.accepted ? submit.message : !writeback ? "writeback result timeout" : writeback->message;
        double readback = std::numeric_limits<double>::quiet_NaN();
        for (const auto& v : router.getLatestByIndexes({c.index}, nowMs())) {
            if (v.index == c.index && v.quality > 0) readback = v.value;
        }
        results.fieldBool("accepted", ok);
        if (ok) {
            ++accepted;
            results.fieldNum("adoptedValue", c.value);
        } else {
            results.key("adoptedValue").null().field("rejectReason", message);
        }
        results.field("arbitrationLevel", level)
            .field("stage", !submit.accepted ? "rejected" : writeback ? writeback->stage : "writeback-timeout")
            .field("message", message)
            .fieldInt("writebackTs", writeback ? (writeback->completedAt > 0 ? writeback->completedAt : nowMs()) : 0)
            .fieldNum("readback", readback)
            .endObject();
        target += (target.empty() ? "" : ",") + std::to_string(c.index) + "=" + number(c.value) + (ok ? "" : ":failed");
    }
    results.endArray();
    const int total = static_cast<int>(commands.size());
    if (!dryRun) record("control.batch", ticket, target, accepted == total ? "ok" : accepted == 0 ? "failed" : "partial");
    Writer w;
    w.beginObject()
        .fieldBool("success", accepted == total)
        .fieldBool("dryRun", dryRun)
        .field("requestId", requestId)
        .field("machineCode", device)
        .fieldInt("total", total)
        .fieldInt("accepted", accepted)
        .fieldInt("failed", total - accepted)
        .key("results").raw(results.text())
        .fieldInt("ts", nowMs())
        .endObject();
    return jsonResponse(200, w.text());
}

}  // namespace maintenance_v2
}  // namespace edge_gateway
