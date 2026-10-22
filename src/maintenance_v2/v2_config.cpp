// Maintenance v2: config dry-run / transactional apply / restore (handoff doc 3.3, 3.5).
#include "v2_service.hpp"

#include "edge_gateway/system_monitor_direct_maintenance_bridge.hpp"
#include "edge_gateway/system_monitor_service.hpp"
#include "v2_crypto.hpp"
#include "v2_files.hpp"
#include "v2_json.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <thread>

namespace edge_gateway {
namespace maintenance_v2 {

namespace bridge = system_monitor_direct_maintenance::bridge;

namespace {

const std::size_t kMaxFiles = 64;
const std::size_t kMaxFileBytes = 8U * 1024U * 1024U;

struct Check {
    std::string rule;
    std::string severity;
    std::string message;
};

struct FilePlan {
    PlannedFile file;
    bool ok = true;
    std::string message = "ok";
    std::vector<Check> checks;
};

bool safeName(const std::string& name) {
    if (name.empty() || name[0] == '.' || name.size() > 128) return false;
    for (const char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')) return false;
    }
    return name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0;
}

// v2 may create these runtime files when missing (removed again on rollback).
bool creatablePath(const DirectConfig& config, const std::string& path) {
    const auto runtime = parentOf(config.identityConfigFile);
    if (runtime.empty() || runtime == ".") return false;
    if (path == runtime + "/device_identity.json") return true;
    const char* dirs[] = {"/apps/", "/devices/", "/logic/"};
    for (const auto* dir : dirs) {
        const auto prefix = runtime + dir;
        if (path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 && safeName(path.substr(prefix.size()))) return true;
    }
    return false;
}

bool pathShapeOk(const std::string& path) {
    return !path.empty() && path[0] == '/' && path.find("..") == std::string::npos && path.find("//") == std::string::npos &&
           path.back() != '/' && path.find('\\') == std::string::npos && path.find('\0') == std::string::npos;
}

void fail(FilePlan* plan, const std::string& rule, const std::string& message) {
    plan->ok = false;
    plan->message = message;
    Check c;
    c.rule = rule;
    c.severity = "error";
    c.message = message;
    plan->checks.push_back(c);
}

FilePlan planFile(const DirectConfig& config, const JsonValue& item) {
    FilePlan plan;
    plan.file.path = str(item, "path");
    const auto& path = plan.file.path;
    if (!pathShapeOk(path)) {
        fail(&plan, "path.allowed", "path must be absolute without .. or //");
        return plan;
    }
    if (isSymlink(path)) {
        fail(&plan, "path.allowed", "symbolic links are not written");
        return plan;
    }
    plan.file.existed = isRegularFile(path);
    // Existing files: the v1 snapshot set, or a runtime apps/devices/logic file the app config does not reference (yet).
    if (plan.file.existed ? !(bridge::isAllowedExistingConfigPath(config, path) || creatablePath(config, path)) : !creatablePath(config, path)) {
        fail(&plan, "path.allowed", "path is not allowed");
        return plan;
    }
    if (baseName(path) == "point-mapping.json" && path.find("/logic/") != std::string::npos) {
        fail(&plan, "mapping.points", "mapping stage is not available on this device");
        return plan;
    }
    const auto encoding = str(item, "encoding", "utf8");
    auto content = str(item, "content");
    if (encoding == "base64") {
        std::string decoded;
        if (!base64Decode(content, &decoded)) {
            fail(&plan, "encoding", "content is not valid base64");
            return plan;
        }
        content.swap(decoded);
    } else if (encoding != "utf8") {
        fail(&plan, "encoding", "encoding must be utf8 or base64");
        return plan;
    }
    if (content.size() > kMaxFileBytes) {
        fail(&plan, "size", "file is too large");
        return plan;
    }
    std::string current;
    if (plan.file.existed && !readFile(path, &current)) {
        fail(&plan, "read", "cannot read the current file");
        return plan;
    }
    if (encoding == "utf8" && content.find(system_monitor_config_safety::kRedactedSecret) != std::string::npos) {
        try {
            content = system_monitor_config_safety::restoreRedactedSecrets(content, current);
        } catch (const std::exception& ex) {
            fail(&plan, "secrets.restore", ex.what());
            return plan;
        }
        if (content.find(system_monitor_config_safety::kRedactedSecret) != std::string::npos) {
            fail(&plan, "secrets.restore", "a redacted placeholder has no value on the device");
            return plan;
        }
    }
    try {
        bridge::validateConfigContent(config, path, content);
        Check c;
        c.rule = "config.load";
        c.severity = "info";
        c.message = "accepted by the device loader";
        plan.checks.push_back(c);
    } catch (const std::exception& ex) {
        fail(&plan, "config.load", ex.what());
        return plan;
    }
    plan.file.changed = !plan.file.existed || current != content;
    plan.file.beforeSha256 = plan.file.existed ? Crypto::instance().sha256Hex(current) : std::string();
    plan.file.content.swap(content);
    if (!plan.file.existed) plan.message = "new file";
    return plan;
}

void writePlans(Writer& w, const std::vector<FilePlan>& plans) {
    w.key("files").beginArray();
    for (const auto& p : plans) {
        w.beginObject()
            .field("path", p.file.path)
            .fieldBool("success", p.ok)
            .fieldBool("changed", p.ok && p.file.changed)
            .fieldBool("created", p.ok && !p.file.existed)
            .field("message", p.message);
        w.key("checks").beginArray();
        for (const auto& c : p.checks) w.beginObject().field("rule", c.rule).field("severity", c.severity).field("message", c.message).endObject();
        w.endArray().endObject();
    }
    w.endArray();
}

std::string currentSha(const std::string& path) {
    std::string content;
    if (!isRegularFile(path) || !readFile(path, &content)) return std::string();
    return Crypto::instance().sha256Hex(content);
}

}  // namespace

Response Service::configApply(const Request& r) {
    JsonValue body;
    try {
        body = parseJson(r.body, 2000000);
    } catch (const std::exception& ex) {
        return errorResponse(400, "validation_failed", ex.what());
    }
    const auto device = deviceMachineCode();
    const auto machine = str(body, "machineCode");
    if (machine.empty() || machine != device) return errorResponse(400, "machine_mismatch", "machineCode must be " + device);
    const bool dryRun = boolean(body, "dryRun", true);
    const auto requestId = str(body, "requestId");
    const auto reason = str(body, "reason");
    if (!dryRun && requestId.empty()) return errorResponse(400, "validation_failed", "requestId is required");
    const auto* list = body.find("files");
    if (list == nullptr || !list->isArray() || list->asArray().values.empty() || list->asArray().values.size() > kMaxFiles) {
        return errorResponse(400, "validation_failed", "files must contain 1..64 items");
    }
    std::vector<std::string> paths;
    for (const auto& item : list->asArray().values) {
        if (!item->isObject()) return errorResponse(400, "validation_failed", "file item must be an object");
        const auto path = str(*item, "path");
        if (std::find(paths.begin(), paths.end(), path) != paths.end()) return errorResponse(400, "validation_failed", "duplicate path " + path);
        paths.push_back(path);
    }

    if (!dryRun) {
        if (system_monitor_config_safety::requiresProductionConfigTransaction(config_.identityConfigFile)) {
            return errorResponse(409, "production_runtime_locked",
                                 "journal-owned production runtime: config changes go through the provision journal, not direct maintenance");
        }
        Transaction existing;
        if (txs_.findByRequestId(requestId, &existing)) {
            // Idempotent resend (the client did not see the 202): same transaction, applied once.
            const auto verdict = checkTicket(r, "config.apply", 3, paths, {}, existing.ticketId, false);
            if (!verdict.ok) return errorResponse(403, verdict.code, verdict.message);
            Writer w;
            w.beginObject().fieldBool("success", true).field("transactionId", existing.id).field("state", existing.state).field("requestId", requestId)
                .fieldBool("replayed", true).endObject();
            return jsonResponse(202, w.text());
        }
        const auto verdict = checkTicket(r, "config.apply", 3, paths, {}, std::string(), false);
        if (!verdict.ok) return errorResponse(403, verdict.code, verdict.message);
    }

    std::vector<FilePlan> plans;
    bool allOk = true;
    int changed = 0;
    for (const auto& item : list->asArray().values) {
        plans.push_back(planFile(config_, *item));
        allOk = allOk && plans.back().ok;
        changed += plans.back().ok && plans.back().file.changed ? 1 : 0;
    }
    std::vector<std::string> affected;
    if (allOk && changed > 0) affected = expectedUnits();  // apply restarts gateway-services (all desired units)

    if (dryRun || !allOk) {
        Writer w;
        w.beginObject()
            .fieldBool("success", allOk)
            .fieldBool("dryRun", dryRun)
            .field("code", allOk ? "" : "validation_failed")
            .field("machineCode", device)
            .field("requestId", requestId)
            .fieldInt("changedFiles", changed)
            .field("message", allOk ? (dryRun ? "config apply dry-run passed" : "") : "config apply dry-run failed");
        writePlans(w, plans);
        w.fieldStrings("affectedServices", affected).fieldInt("ts", nowMs()).endObject();
        return jsonResponse(dryRun ? 200 : 400, w.text());
    }

    std::lock_guard<std::mutex> lock(configMutex_);
    Transaction active;
    if (txs_.findActive(&active)) return errorResponse(409, "apply_in_progress", "transaction " + active.id + " is still " + active.state);
    const auto verdict = checkTicket(r, "config.apply", 3, paths, {}, std::string(), true);  // consumes a one-time ticket
    if (!verdict.ok) return errorResponse(403, verdict.code, verdict.message);
    std::vector<PlannedFile> files;
    for (auto& p : plans) files.push_back(p.file);
    Transaction tx;
    try {
        tx = txs_.create(requestId, verdict.ticket.operatorLabel(), verdict.ticket.operatorId, verdict.ticket.ticketId, reason, files);
    } catch (const std::exception& ex) {
        return errorResponse(500, "internal_error", std::string("cannot create the transaction journal: ") + ex.what());
    }
    std::string target;
    for (const auto& f : files) target += (target.empty() ? "" : ",") + f.path;
    record("config.apply", verdict.ticket, target, "accepted", tx.id);
    std::string error;
    if (!txs_.launchHelper(tx.id, "apply", &error)) {
        tx.state = "rolled-back";
        tx.rollbackReason = "helper did not start: " + error;
        tx.message = "nothing was written: " + error;
        txs_.save(tx);
        record("config.apply", verdict.ticket, target, "rolled-back", tx.id);
        return errorResponse(500, "internal_error", tx.message);
    }
    txs_.prune(50);
    Writer w;
    w.beginObject().fieldBool("success", true).field("transactionId", tx.id).field("state", tx.state).field("requestId", requestId).endObject();
    return jsonResponse(202, w.text());
}

Response Service::transaction(const std::string& id) {
    Transaction tx;
    if (!txs_.load(id, &tx)) return errorResponse(404, "transaction_not_found", "no transaction " + id);
    return jsonResponse(200, transactionJson(tx));
}

Response Service::configRestore(const Request& r) {
    JsonValue body;
    try {
        body = parseJson(r.body, 1000);
    } catch (const std::exception& ex) {
        return errorResponse(400, "validation_failed", ex.what());
    }
    const auto device = deviceMachineCode();
    const auto machine = str(body, "machineCode");
    if (machine.empty() || machine != device) return errorResponse(400, "machine_mismatch", "machineCode must be " + device);
    const auto id = str(body, "transactionId");
    const auto requestId = str(body, "requestId");
    const auto reason = str(body, "reason", "client restore");
    Transaction tx;
    if (!txs_.load(id, &tx)) return errorResponse(404, "transaction_not_found", "no transaction " + id);
    std::vector<std::string> paths;
    for (const auto& f : tx.files) paths.push_back(f.path);
    std::string abortRequestId;
    std::string abortTicketId;
    const bool aborting = txs_.abortRequested(id, nullptr, &abortRequestId, &abortTicketId);
    bool resend = !requestId.empty() && tx.restoreRequestId == requestId;
    std::string boundTicket = tx.restoreTicketId;
    if (!resend && aborting && !requestId.empty() && abortRequestId == requestId) {
        resend = true;  // resent while the helper is still rolling back
        boundTicket = abortTicketId;
    }
    const auto verdict = checkTicket(r, "config.restore", 3, paths, {}, resend ? boundTicket : std::string(), !resend);
    if (!verdict.ok) return errorResponse(403, verdict.code, verdict.message);
    if (tx.state == "rolled-back" || (resend && tx.finished())) return jsonResponse(200, transactionJson(tx));

    std::lock_guard<std::mutex> lock(configMutex_);
    if (!tx.finished()) {
        // In progress: the helper rolls back at its next step (it polls the abort marker).
        // Answer within the client's request timeout; an unfinished state means "poll the transaction".
        if (!aborting) txs_.requestAbort(id, reason, requestId, verdict.ticket.ticketId);
        const auto deadline = nowMs() + 5000;
        bool helperGone = false;
        while (nowMs() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (txs_.load(id, &tx) && tx.finished()) break;
            if (nowMs() - tx.updatedAt > 15000 && !txs_.helperRunning(id)) {
                helperGone = true;  // helper died: restore here
                break;
            }
        }
        if (tx.finished() || !helperGone) return jsonResponse(tx.finished() ? 200 : 202, transactionJson(tx));
    }
    // committed, failed, or in progress without a live helper: restore here under the transaction lock.
    FileLock txLock(txs_.directory(id) + "/lock", false);
    if (!txLock.locked()) return jsonResponse(200, transactionJson(tx));  // helper still finishing; client polls / retries
    if (!txs_.load(id, &tx)) return errorResponse(404, "transaction_not_found", "no transaction " + id);
    if (tx.state == "rolled-back") return jsonResponse(200, transactionJson(tx));
    for (const auto& f : tx.files) {
        if (!f.written) continue;
        const auto sha = currentSha(f.path);
        if (sha != f.afterSha256) {
            return errorResponse(409, "restore_conflict", f.path + " changed after this transaction; restore would overwrite newer content");
        }
    }
    tx.restoreRequestId = requestId;
    tx.restoreTicketId = verdict.ticket.ticketId;
    TxRunner runner(config_, options_);
    std::string error;
    if (!runner.restoreFiles(&tx, &error)) {
        tx.state = "failed";
        tx.message = "restore failed: " + error;
        txs_.save(tx);
        record("config.restore", verdict.ticket, id, "failed", id);
        return jsonResponse(200, transactionJson(tx));
    }
    tx.state = "rolled-back";
    tx.rollbackPerformed = true;
    tx.rollbackReason = "restored by client: " + reason;
    tx.message = tx.rollbackReason;
    tx.rollbackServices = tx.restartedServices ? "pending" : "";
    tx.updatedAt = nowMs();
    txs_.save(tx);
    std::string target;
    for (const auto& f : tx.files) target += (target.empty() ? "" : ",") + f.path;
    record("config.restore", verdict.ticket, target, "rolled-back", id);
    if (tx.restartedServices) {
        std::string launchError;
        if (!txs_.launchHelper(id, "restart", &launchError)) {
            std::cerr << "maintenance v2: restart after restore not started: " << launchError << std::endl;
        }
    }
    return jsonResponse(200, transactionJson(tx));
}

}  // namespace maintenance_v2
}  // namespace edge_gateway
