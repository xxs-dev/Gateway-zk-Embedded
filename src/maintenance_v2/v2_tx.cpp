#include "v2_tx.hpp"

#include "edge_gateway/system_monitor_direct_maintenance_bridge.hpp"
#include "edge_gateway/system_monitor_service.hpp"
#include "v2_crypto.hpp"
#include "v2_files.hpp"
#include "v2_json.hpp"
#include "v2_process.hpp"
#include "v2_state.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <thread>

#ifndef _WIN32
#include <time.h>
#include <unistd.h>
#endif

namespace edge_gateway {
namespace maintenance_v2 {

namespace bridge = system_monitor_direct_maintenance::bridge;

namespace {

bool validUnitName(const std::string& unit) {
    if (unit.empty() || unit.size() > 200) return false;
    for (const char c : unit) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '@' || c == '.' || c == '_' || c == '-' || c == ':')) return false;
    }
    return true;
}

std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string currentBootId() {
    static const std::string id = [] {
        std::ifstream in("/proc/sys/kernel/random/boot_id");
        std::string line;
        std::getline(in, line);
        return trim(line);
    }();
    return id;
}

// Monotonic milliseconds including suspend (CLOCK_BOOTTIME); steady_clock elsewhere.
std::int64_t monoNowMs() {
#if defined(CLOCK_BOOTTIME)
    timespec ts {};
    if (::clock_gettime(CLOCK_BOOTTIME, &ts) == 0) return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
#endif
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string homeFromConfig(const DirectConfig& config) {
    const std::string suffix = "/config/runtime/device_identity.json";
    const auto& p = config.identityConfigFile;
    if (p.size() > suffix.size() && p.compare(p.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return p.substr(0, p.size() - suffix.size());
    }
    return "/opt/modbus-gateway";
}

std::string newTransactionId() {
    const auto now = std::time(nullptr);
    std::tm local {};
    localtime_r(&now, &local);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    std::string random;
    Crypto::instance().randomBytes(3, &random);
    return std::string("TX-") + stamp + "-" + toHex(random);
}

void writeStrings(Writer& w, const char* name, const std::vector<std::string>& values) {
    w.fieldStrings(name, values);
}

std::string joinPaths(const Transaction& tx) {
    std::string out;
    for (const auto& f : tx.files) {
        if (!out.empty()) out += ",";
        out += f.path;
    }
    return out;
}

std::string internalJson(const Transaction& tx) {
    Writer w;
    w.beginObject()
        .field("transactionId", tx.id)
        .field("operation", tx.operation)
        .field("requestId", tx.requestId)
        .field("state", tx.state)
        .field("message", tx.message)
        .fieldInt("createdAt", tx.createdAt)
        .fieldInt("updatedAt", tx.updatedAt)
        .field("operator", tx.operatorName)
        .field("operatorId", tx.operatorId)
        .field("ticketId", tx.ticketId)
        .field("reason", tx.reason)
        .fieldBool("restartedServices", tx.restartedServices);
    w.key("files").beginArray();
    for (const auto& f : tx.files) {
        w.beginObject()
            .field("path", f.path)
            .fieldBool("remove", f.remove)
            .fieldBool("existed", f.existed)
            .fieldBool("changed", f.changed)
            .fieldBool("written", f.written)
            .field("beforeSha256", f.beforeSha256)
            .field("afterSha256", f.afterSha256)
            .endObject();
    }
    w.endArray().key("services").beginArray();
    for (const auto& s : tx.services) {
        w.beginObject().field("name", s.name).fieldBool("restarted", s.restarted).fieldBool("active", s.active).endObject();
    }
    w.endArray();
    w.key("health").beginObject().fieldBool("known", tx.healthKnown).fieldBool("ok", tx.healthOk);
    writeStrings(w, "failed", tx.healthFailed);
    w.endObject();
    w.key("rollback").beginObject()
        .fieldBool("performed", tx.rollbackPerformed)
        .field("reason", tx.rollbackReason)
        .fieldInt("restoredFiles", tx.restoredFiles)
        .field("services", tx.rollbackServices)
        .fieldBool("healthKnown", tx.rollbackHealthKnown)
        .fieldBool("healthOk", tx.rollbackHealthOk);
    writeStrings(w, "healthFailed", tx.rollbackHealthFailed);
    w.endObject();
    w.field("restoreRequestId", tx.restoreRequestId).field("restoreTicketId", tx.restoreTicketId);
    w.field("bootId", currentBootId()).fieldInt("monoMs", monoNowMs());
    w.endObject();
    return w.text();
}

bool parseInternal(const std::string& text, Transaction* tx) {
    try {
        const auto v = parseJson(text);
        tx->id = str(v, "transactionId");
        tx->operation = str(v, "operation", "apply");
        tx->requestId = str(v, "requestId");
        tx->state = str(v, "state");
        tx->message = str(v, "message");
        integer(v, "createdAt", &tx->createdAt);
        integer(v, "updatedAt", &tx->updatedAt);
        tx->operatorName = str(v, "operator");
        tx->operatorId = str(v, "operatorId");
        tx->ticketId = str(v, "ticketId");
        tx->reason = str(v, "reason");
        tx->restartedServices = boolean(v, "restartedServices", false);
        tx->files.clear();
        if (const auto* files = v.find("files")) {
            for (const auto& item : files->asArray().values) {
                TxFile f;
                f.path = str(*item, "path");
                f.remove = boolean(*item, "remove", false);
                f.existed = boolean(*item, "existed", false);
                f.changed = boolean(*item, "changed", false);
                f.written = boolean(*item, "written", false);
                f.beforeSha256 = str(*item, "beforeSha256");
                f.afterSha256 = str(*item, "afterSha256");
                tx->files.push_back(f);
            }
        }
        tx->services.clear();
        if (const auto* services = v.find("services")) {
            for (const auto& item : services->asArray().values) {
                TxService s;
                s.name = str(*item, "name");
                s.restarted = boolean(*item, "restarted", false);
                s.active = boolean(*item, "active", false);
                tx->services.push_back(s);
            }
        }
        if (const auto* h = v.find("health")) {
            tx->healthKnown = boolean(*h, "known", false);
            tx->healthOk = boolean(*h, "ok", false);
            tx->healthFailed = strings(*h, "failed");
        }
        if (const auto* r = v.find("rollback")) {
            tx->rollbackPerformed = boolean(*r, "performed", false);
            tx->rollbackReason = str(*r, "reason");
            std::int64_t restored = 0;
            integer(*r, "restoredFiles", &restored);
            tx->restoredFiles = static_cast<int>(restored);
            tx->rollbackServices = str(*r, "services");
            tx->rollbackHealthKnown = boolean(*r, "healthKnown", false);
            tx->rollbackHealthOk = boolean(*r, "healthOk", false);
            tx->rollbackHealthFailed = strings(*r, "healthFailed");
        }
        tx->restoreRequestId = str(v, "restoreRequestId");
        tx->restoreTicketId = str(v, "restoreTicketId");
        tx->savedBootId = str(v, "bootId");
        integer(v, "monoMs", &tx->savedMonoMs);
        return !tx->id.empty() && !tx->state.empty();
    } catch (...) {
        return false;
    }
}

std::string sha256OfFile(const std::string& path) {
    std::string content;
    if (!isRegularFile(path) || !readFile(path, &content)) return std::string();
    return Crypto::instance().sha256Hex(content);
}

}  // namespace

bool transactionStale(const Transaction& tx, std::int64_t graceMs) {
    const auto boot = currentBootId();
    if (!tx.savedBootId.empty() && !boot.empty()) {
        if (tx.savedBootId != boot) return true;  // helpers do not survive a reboot
        return monoNowMs() - tx.savedMonoMs >= graceMs;
    }
    const auto delta = nowMs() - tx.updatedAt;  // journal from before this field: wall clock either way round
    return delta >= graceMs || delta <= -graceMs;
}

std::string transactionAction(const Transaction& tx) { return tx.operation == "delete" ? "config.delete" : "config.apply"; }

std::string transactionJson(const Transaction& tx) {
    Writer w;
    w.beginObject()
        .fieldBool("success", tx.state != "failed")
        .field("transactionId", tx.id)
        .field("operation", tx.operation)
        .field("requestId", tx.requestId)
        .field("state", tx.state)
        .field("message", tx.message)
        .fieldInt("createdAt", tx.createdAt)
        .fieldInt("updatedAt", tx.updatedAt)
        .field("operator", tx.operatorName)
        .field("ticketId", tx.ticketId);
    w.key("files").beginArray();
    for (const auto& f : tx.files) {
        w.beginObject().field("path", f.path).fieldBool("changed", f.changed).fieldBool("created", !f.existed && !f.remove).fieldBool("deleted", f.remove).endObject();
    }
    w.endArray().key("services").beginArray();
    for (const auto& s : tx.services) w.beginObject().field("name", s.name).fieldBool("restarted", s.restarted).fieldBool("active", s.active).endObject();
    w.endArray();
    if (tx.healthKnown) {
        w.key("health").beginObject().fieldBool("ok", tx.healthOk);
        writeStrings(w, "failed", tx.healthFailed);
        w.endObject();
    }
    w.key("rollback").beginObject()
        .fieldBool("performed", tx.rollbackPerformed)
        .field("reason", tx.rollbackReason)
        .fieldInt("restoredFiles", tx.restoredFiles)
        .field("services", tx.rollbackServices);
    if (tx.rollbackHealthKnown) {
        w.key("health").beginObject().fieldBool("ok", tx.rollbackHealthOk);
        writeStrings(w, "failed", tx.rollbackHealthFailed);
        w.endObject();
    }
    w.endObject();
    if (!tx.restoreTicketId.empty()) w.field("restoreTicketId", tx.restoreTicketId);
    w.endObject();
    return w.text();
}

// ---------------------------------------------------------------- services

Services::Services(const DirectConfig& config, const Options& options)
    : command_(options.servicesCommand), home_(homeFromConfig(config)) {}

std::vector<std::string> Services::expectedUnits(std::string* error) const {
    std::vector<std::string> units;
    units.push_back("gateway-services.service");
    CommandResult r;
    if (!command_.empty()) {
        r = runCommand({command_, "list"}, 30000);
    } else {
        std::string sh;
        if (!findExecutable("sh", &sh)) {
            if (error) *error = "sh not found";
            return units;
        }
        r = runCommand({sh, home_ + "/bin/gateway-services.sh", "list"}, 30000);
    }
    if (r.exitCode != 0) {
        if (error) *error = "gateway-services list failed (exit " + std::to_string(r.exitCode) + ")";
        return units;
    }
    std::size_t pos = 0;
    while (pos < r.output.size()) {
        const auto end = r.output.find('\n', pos);
        const auto line = trim(r.output.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
        pos = end == std::string::npos ? r.output.size() : end + 1;
        if (line.empty() || line[0] == '#' || !validUnitName(line)) continue;
        if (std::find(units.begin(), units.end(), line) == units.end()) units.push_back(line);
    }
    return units;
}

std::string Services::state(const std::string& unit) const {
    if (!validUnitName(unit)) return "unknown";
    CommandResult r;
    if (!command_.empty()) {
        r = runCommand({command_, "state", unit}, 10000, 256);
    } else {
        std::string systemctl;
        if (!findExecutable("systemctl", &systemctl)) return "unknown";
        r = runCommand({systemctl, "is-active", unit}, 10000, 256);
    }
    const auto s = trim(r.output);
    return s.empty() ? (r.exitCode == 0 ? "active" : "unknown") : s;
}

bool Services::restart(std::string* error, const std::function<bool()>& cancel) const {
    CommandResult r;
    if (!command_.empty()) {
        r = runCommand({command_, "restart"}, 180000, 4096, cancel);
    } else {
        std::string systemctl;
        if (!findExecutable("systemctl", &systemctl)) {
            if (error) *error = "systemctl not found";
            return false;
        }
        r = runCommand({systemctl, "restart", "gateway-services.service"}, 180000, 4096, cancel);
    }
    if (r.cancelled) {
        if (error) *error = "cancelled";
        return false;
    }
    if (r.timedOut || r.exitCode != 0) {
        if (error) *error = r.timedOut ? "restart timed out" : "restart failed (exit " + std::to_string(r.exitCode) + "): " + trim(r.output).substr(0, 300);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- store

TxStore::TxStore(const DirectConfig& config, const Options& options)
    : config_(config), options_(options), root_(options.stateDirectory + "/transactions") {}

std::string TxStore::directory(const std::string& id) const {
    return root_ + "/" + id;
}

bool TxStore::load(const std::string& id, Transaction* tx) const {
    if (id.empty() || id.find('/') != std::string::npos || id.find("..") != std::string::npos) return false;
    std::string text;
    return readFile(directory(id) + "/tx.json", &text) && parseInternal(text, tx);
}

bool TxStore::save(const Transaction& tx, std::string* error) const {
    try {
        writeFileAtomic(directory(tx.id) + "/tx.json", internalJson(tx));
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    }
}

std::vector<std::string> TxStore::ids() const {
    auto names = listDirectory(root_);
    names.erase(std::remove_if(names.begin(), names.end(), [](const std::string& n) { return n.compare(0, 3, "TX-") != 0; }), names.end());
    std::sort(names.begin(), names.end());
    return names;
}

bool TxStore::findByRequestId(const std::string& requestId, Transaction* tx) const {
    if (requestId.empty()) return false;
    for (const auto& id : ids()) {
        Transaction t;
        if (load(id, &t) && t.requestId == requestId) {
            *tx = t;
            return true;
        }
    }
    return false;
}

bool TxStore::findActive(Transaction* tx) const {
    for (const auto& id : ids()) {
        Transaction t;
        if (load(id, &t) && t.busy()) {
            *tx = t;
            return true;
        }
    }
    return false;
}

Transaction TxStore::create(const std::string& requestId, const std::string& operatorName, const std::string& operatorId,
                            const std::string& ticketId, const std::string& reason, const std::vector<PlannedFile>& files,
                            const std::string& operation) {
    Transaction tx;
    tx.id = newTransactionId();
    tx.operation = operation;
    tx.requestId = requestId;
    tx.state = "accepted";
    tx.message = "accepted";
    tx.createdAt = nowMs();
    tx.updatedAt = tx.createdAt;
    tx.operatorName = operatorName;
    tx.operatorId = operatorId;
    tx.ticketId = ticketId;
    tx.reason = reason;
    const auto dir = directory(tx.id);
    makeDirectories(dir + "/staged", 0700);
    makeDirectories(dir + "/backup", 0700);
    for (std::size_t i = 0; i < files.size(); ++i) {
        writeFileAtomic(dir + "/staged/" + std::to_string(i), files[i].content);
        TxFile f;
        f.path = files[i].path;
        f.remove = files[i].remove;
        f.existed = files[i].existed;
        f.changed = files[i].changed;
        f.beforeSha256 = files[i].beforeSha256;
        f.afterSha256 = f.remove ? std::string() : Crypto::instance().sha256Hex(files[i].content);  // removed: absent afterwards
        tx.files.push_back(f);
    }
    std::string error;
    if (!save(tx, &error)) throw std::runtime_error(error);
    return tx;
}

bool TxStore::helperRunning(const std::string& id) const {
    FileLock lock(directory(id) + "/lock", false);
    return !lock.locked();
}

bool TxStore::launchHelper(const std::string& id, const std::string& mode, std::string* error) const {
    if (options_.helperCommand.empty()) {
        *error = "no helper command configured";
        return false;
    }
    std::vector<std::string> argv = options_.helperCommand;
    argv.push_back("--maintenance-v2-tx");
    argv.push_back(id);
    argv.push_back("--maintenance-v2-tx-mode");
    argv.push_back(mode);
    std::string systemdRun;
    const bool useSystemd = options_.helperLauncher == "systemd-run" ||
                            (options_.helperLauncher == "auto" && systemdRunning() && findExecutable("systemd-run", &systemdRun));
    if (useSystemd) {
        if (systemdRun.empty() && !findExecutable("systemd-run", &systemdRun)) {
            *error = "systemd-run not found";
            return false;
        }
        std::string unit = "gz-maint-v2-" + mode + "-" + id;
        for (auto& c : unit) {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-')) c = '-';
        }
        std::vector<std::string> wrapped = {systemdRun, "--unit=" + unit + "-" + std::to_string(nowMs() % 100000), "--collect", "--quiet",
                                            "--property=TimeoutStopSec=30", "--"};
        wrapped.insert(wrapped.end(), argv.begin(), argv.end());
        const auto r = runCommand(wrapped, 30000, 4096);
        if (r.exitCode != 0) {
            *error = "systemd-run failed: " + trim(r.output).substr(0, 300);
            return false;
        }
        return true;
    }
    return spawnDetached(argv, error);
}

bool TxStore::requestAbort(const std::string& id, const std::string& reason, const std::string& requestId, const std::string& ticketId) const {
    Writer w;
    w.beginObject().field("reason", reason.empty() ? std::string("client restore") : reason).field("requestId", requestId).field("ticketId", ticketId).endObject();
    try {
        writeFileAtomic(directory(id) + "/abort", w.text());
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "maintenance v2: cannot write the abort marker of " << id << ": " << ex.what() << std::endl;
        return false;
    }
}

bool TxStore::abortRequested(const std::string& id, std::string* reason, std::string* requestId, std::string* ticketId) const {
    std::string text;
    if (!readFile(directory(id) + "/abort", &text)) return false;
    try {
        const auto v = parseJson(text);
        if (reason) *reason = str(v, "reason");
        if (requestId) *requestId = str(v, "requestId");
        if (ticketId) *ticketId = str(v, "ticketId");
    } catch (...) {
        if (reason) *reason = "client restore";
    }
    return true;
}

void TxStore::prune(std::size_t keep) const {
    auto list = ids();
    if (list.size() <= keep) return;
    for (std::size_t i = 0; i + keep < list.size(); ++i) {
        Transaction t;
        if (!load(list[i], &t) || !t.finished()) continue;
        const auto dir = directory(list[i]);
        for (const auto& sub : {std::string("/staged"), std::string("/backup")}) {
            for (const auto& name : listDirectory(dir + sub)) removeFile(dir + sub + "/" + name);
            ::rmdir((dir + sub).c_str());
        }
        for (const auto& name : listDirectory(dir)) removeFile(dir + "/" + name);
        ::rmdir(dir.c_str());
    }
}

int recoverInterruptedTransactions(const TxStore& store) {
    int open = 0;
    for (const auto& id : store.ids()) {
        Transaction tx;
        if (!store.load(id, &tx)) continue;
        const bool pendingRestart = tx.state == "rolled-back" && (tx.rollbackServices == "pending" || tx.rollbackServices == "restarting");
        if (tx.finished() && !pendingRestart) continue;
        ++open;
        if (!transactionStale(tx, 15000) || store.helperRunning(id)) continue;
        std::string error;
        if (!store.launchHelper(id, pendingRestart ? "restart" : "recover", &error)) {
            std::cerr << "maintenance v2: cannot recover " << id << ": " << error << std::endl;
        }
    }
    return open;
}

// ---------------------------------------------------------------- runner

TxRunner::TxRunner(const DirectConfig& config, const Options& options)
    : config_(config), options_(options), store_(config, options), services_(config, options) {}

bool TxRunner::save(Transaction* tx) {
    tx->updatedAt = nowMs();
    std::string error;
    if (store_.save(*tx, &error)) return true;
    std::cerr << "maintenance v2: cannot save transaction " << tx->id << ": " << error << std::endl;
    return false;
}

void TxRunner::audit(const Transaction& tx, const std::string& action, const std::string& result) {
    AuditEntry e;
    e.operatorName = tx.operatorName;
    e.ticketId = action == "config.restore" ? tx.restoreTicketId : tx.ticketId;
    e.action = action;
    e.target = joinPaths(tx);
    e.result = result;
    e.transactionId = tx.id;
    std::string error;
    if (!AuditLog(options_.stateDirectory + "/audit.jsonl").append(e, &error)) {
        std::cerr << "maintenance v2: " << error << std::endl;
    }
}

bool TxRunner::restoreFiles(Transaction* tx, std::string* error) {
    const auto dir = store_.directory(tx->id);
    int restored = 0;
    bool ok = true;
    for (std::size_t i = tx->files.size(); i-- > 0;) {
        auto& f = tx->files[i];
        if (!f.written) continue;
        try {
            if (f.existed) {
                std::string content;
                if (!readFile(dir + "/backup/" + std::to_string(i), &content)) throw std::runtime_error("backup missing for " + f.path);
                system_monitor_config_safety::writeFileAtomically(f.path, content, true);
            } else if (exists(f.path) && !removeFile(f.path)) {  // already gone: an interrupted restore removed it
                throw std::runtime_error("cannot remove created file " + f.path);
            }
            f.written = false;
            ++restored;
        } catch (const std::exception& ex) {
            ok = false;
            if (error) *error += std::string(ex.what()) + "; ";
        }
    }
    tx->restoredFiles += restored;
    return ok;
}

bool TxRunner::healthCheck(Transaction* tx, bool rollbackPhase, const std::string& abortId) {
    const auto timeoutMs = static_cast<std::int64_t>(std::max(1, config_.v2.healthTimeoutSec)) * 1000;
    const auto stableMs = static_cast<std::int64_t>(std::max(0, config_.v2.healthStableSec)) * 1000;
    const auto deadline = monoNowMs() + timeoutMs;  // monotonic: an NTP step does not shorten or stretch the check
    std::int64_t stableSince = 0;
    bool stable = false;
    std::vector<std::string> failed;
    while (true) {
        if (!abortId.empty() && store_.abortRequested(abortId, nullptr)) return false;
        std::string listError;
        const auto units = services_.expectedUnits(&listError);
        failed.clear();
        std::vector<TxService> states;
        for (const auto& unit : units) {
            TxService s;
            s.name = unit;
            s.restarted = true;
            s.active = services_.state(unit) == "active";
            if (!s.active) failed.push_back(unit);
            states.push_back(s);
        }
        if (!listError.empty()) failed.push_back("gateway-services list: " + listError);
        if (!rollbackPhase) {
            tx->services = states;
            save(tx);
        }
        const auto now = monoNowMs();
        if (failed.empty()) {
            if (stableSince == 0) stableSince = now;
            if (now - stableSince >= stableMs) {
                stable = true;
                break;
            }
        } else {
            stableSince = 0;
        }
        if (now >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    // Healthy only after healthStableSec without a failure: a unit that keeps restarting and happens to be active
    // at the deadline is not healthy.
    const bool ok = stable;
    if (!ok && failed.empty()) failed.push_back("services were not stable for " + std::to_string(stableMs / 1000) + " s within the health timeout");
    if (rollbackPhase) {
        tx->rollbackHealthKnown = true;
        tx->rollbackHealthOk = ok;
        tx->rollbackHealthFailed = failed;
    } else {
        tx->healthKnown = true;
        tx->healthOk = ok;
        tx->healthFailed = failed;
    }
    return ok;
}

void TxRunner::rollback(Transaction* tx, const std::string& reason, bool restartAlways) {
    std::string error;
    const auto restoredBefore = tx->restoredFiles;
    if (!restoreFiles(tx, &error)) {
        tx->state = "failed";
        tx->rollbackPerformed = true;
        tx->rollbackReason = reason;
        tx->message = "rollback failed: " + error;
        save(tx);
        audit(*tx, transactionAction(*tx), "failed");
        return;
    }
    tx->state = "rolled-back";
    tx->rollbackPerformed = true;
    tx->rollbackReason = reason;
    tx->message = reason + "; restored the backup taken before applying";
    const bool restart = tx->restartedServices || (restartAlways && tx->restoredFiles > restoredBefore);
    tx->rollbackServices = restart ? "restarting" : "";
    save(tx);
    audit(*tx, reason.compare(0, 18, "restored by client") == 0 ? std::string("config.restore") : transactionAction(*tx), "rolled-back");
    if (!restart) return;
    std::string restartError;
    const bool restarted = services_.restart(&restartError);
    const bool healthy = restarted && healthCheck(tx, true, std::string());
    if (!restarted) {
        tx->rollbackHealthKnown = true;
        tx->rollbackHealthOk = false;
        tx->rollbackHealthFailed = {"gateway-services.service: " + restartError};
    }
    tx->rollbackServices = healthy ? "done" : "failed";
    save(tx);
}

int TxRunner::apply(const std::string& id) {
    FileLock lock(store_.directory(id) + "/lock", true);
    Transaction tx;
    if (!lock.locked() || !store_.load(id, &tx)) {
        std::cerr << "maintenance v2: transaction " << id << " not found" << std::endl;
        return 2;
    }
    if (tx.finished()) return 0;
    if (tx.state != "accepted") return recover(id);
    std::string abortReason;
    auto aborted = [&]() {
        if (!store_.abortRequested(id, &abortReason, &tx.restoreRequestId, &tx.restoreTicketId)) return false;
        return true;
    };
    if (aborted()) {
        tx.state = "rolled-back";
        tx.rollbackReason = "restored by client: " + abortReason;
        tx.message = "cancelled before any file was written";
        save(&tx);
        audit(tx, "config.restore", "rolled-back");
        return 0;
    }

    tx.state = "validating";
    tx.message = "validating";
    save(&tx);
    const auto dir = store_.directory(id);
    std::vector<std::string> staged(tx.files.size());
    std::string problem;
    for (std::size_t i = 0; i < tx.files.size() && problem.empty(); ++i) {
        const auto& f = tx.files[i];
        if (!readFile(dir + "/staged/" + std::to_string(i), &staged[i])) {
            problem = "staged content missing for " + f.path;
        } else if (isSymlink(f.path)) {
            problem = f.path + " is a symbolic link";
        } else if (f.existed ? sha256OfFile(f.path) != f.beforeSha256 : exists(f.path)) {
            problem = f.path + " changed on the device after the dry-run";
        } else if (f.remove) {
            // nothing to validate: the dry-run checked that no remaining config references it
        } else {
            try {
                bridge::validateConfigContent(config_, f.path, staged[i]);
            } catch (const std::exception& ex) {
                problem = f.path + ": " + ex.what();
            }
        }
    }
    if (problem.empty() && !config_.identityConfigFile.empty() &&
        system_monitor_config_safety::requiresProductionConfigTransaction(config_.identityConfigFile)) {
        problem = "journal-owned production runtime";
    }
    if (!problem.empty()) {
        tx.state = "rolled-back";
        tx.rollbackReason = "validation failed: " + problem;
        tx.message = "nothing was written: " + problem;
        save(&tx);
        audit(tx, transactionAction(tx), "rolled-back");
        return 1;
    }

    tx.state = "applying";
    tx.message = "writing (backup first)";
    save(&tx);
    for (std::size_t i = 0; i < tx.files.size(); ++i) {
        auto& f = tx.files[i];
        if (!f.changed) continue;
        try {
            if (f.existed) {
                std::string current;
                if (!readFile(f.path, &current)) throw std::runtime_error("cannot read " + f.path);
                writeFileAtomic(dir + "/backup/" + std::to_string(i), current);
            } else {
                makeDirectories(parentOf(f.path), 0755);
            }
            f.written = true;  // journal before the write: a crash in between restores the backup
            if (!save(&tx)) {
                f.written = false;  // not journaled: do not touch the file
                throw std::runtime_error("cannot journal the write of " + f.path + " (disk full?)");
            }
            if (f.remove) {
                if (!removeFile(f.path)) throw std::runtime_error("cannot remove " + f.path);
            } else {
                system_monitor_config_safety::writeFileAtomically(f.path, staged[i], true);
            }
        } catch (const std::exception& ex) {
            rollback(&tx, std::string("write failed: ") + ex.what());
            return 1;
        }
    }
    if (aborted()) {
        rollback(&tx, "restored by client: " + abortReason);
        return 0;
    }
    bool anyChanged = false;
    for (const auto& f : tx.files) anyChanged = anyChanged || f.changed;
    if (!anyChanged) {
        tx.state = "committed";
        tx.message = "no file changed; services were not restarted";
        tx.healthKnown = true;
        tx.healthOk = true;
        save(&tx);
        audit(tx, transactionAction(tx), "committed");
        return 0;
    }

    tx.state = "restarting";
    tx.message = "restarting gateway services";
    tx.restartedServices = true;
    save(&tx);
    std::string restartError;
    const bool restarted = services_.restart(&restartError, aborted);
    if (aborted()) {
        rollback(&tx, "restored by client: " + abortReason);
        return 0;
    }
    tx.state = "health-check";
    tx.message = "waiting for services";
    save(&tx);
    const bool healthy = restarted && healthCheck(&tx, false, id);
    if (aborted()) {
        rollback(&tx, "restored by client: " + abortReason);
        return 0;
    }
    if (!restarted) {
        tx.healthKnown = true;
        tx.healthOk = false;
        tx.healthFailed = {"gateway-services.service: " + restartError};
    }
    if (!healthy) {
        rollback(&tx, restarted ? "health check failed" : "service restart failed");
        return 1;
    }
    tx.state = "committed";
    tx.message = "applied; services healthy";
    save(&tx);
    audit(tx, transactionAction(tx), "committed");
    return 0;
}

int TxRunner::restartAfterRestore(const std::string& id) {
    FileLock lock(store_.directory(id) + "/lock", true);
    Transaction tx;
    if (!lock.locked() || !store_.load(id, &tx)) return 2;
    // "restarting": the previous restart helper died (power loss) before it finished.
    if (tx.rollbackServices != "pending" && tx.rollbackServices != "restarting") return 0;
    tx.rollbackServices = "restarting";
    save(&tx);
    std::string error;
    const bool restarted = services_.restart(&error);
    const bool healthy = restarted && healthCheck(&tx, true, std::string());
    if (!restarted) {
        tx.rollbackHealthKnown = true;
        tx.rollbackHealthOk = false;
        tx.rollbackHealthFailed = {"gateway-services.service: " + error};
    }
    tx.rollbackServices = healthy ? "done" : "failed";
    save(&tx);
    return healthy ? 0 : 1;
}

int TxRunner::recover(const std::string& id) {
    Transaction tx;
    if (!store_.load(id, &tx)) return 2;
    if (tx.finished()) return 0;
    bool written = false;
    for (const auto& f : tx.files) written = written || f.written;
    if (!written && (tx.state == "accepted" || tx.state == "validating")) {
        tx.state = "rolled-back";
        tx.rollbackReason = "interrupted before writing";
        tx.message = "the transaction was interrupted before any file was written";
        save(&tx);
        audit(tx, transactionAction(tx), "rolled-back");
        return 0;
    }
    if (tx.state == "restoring") {
        // The server was restoring this committed transaction when it stopped: finish the restore.
        rollback(&tx, tx.rollbackReason.empty() ? std::string("restored by client") : tx.rollbackReason, true);
        return 0;
    }
    rollback(&tx, "interrupted (helper stopped during " + tx.state + ")", true);
    return 0;
}

}  // namespace maintenance_v2
}  // namespace edge_gateway
