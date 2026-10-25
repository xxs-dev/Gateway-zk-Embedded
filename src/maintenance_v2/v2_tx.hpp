#pragma once

// Journaled config transactions (handoff doc 3.3 / 3.5).
//
//   accepted -> validating -> applying -> restarting -> health-check -> committed
//   failure: rolled-back (files restored from the transaction backup, created files
//   removed), or failed (the restore itself did not succeed).
//
// The work runs in a helper process outside SystemMonitor (systemd-run unit or a
// detached child), because restarting gateway-services also restarts SystemMonitor.
// The journal (transactions/<id>/tx.json) is the single source of truth; the server
// only reads it. The helper holds transactions/<id>/lock while it runs.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "edge_gateway/maintenance_v2.hpp"

namespace edge_gateway {
namespace maintenance_v2 {

struct TxFile {
    std::string path;
    bool remove = false;  // config/delete: the file is removed (restored from the backup on rollback)
    bool existed = false;
    bool changed = false;
    bool written = false;
    std::string beforeSha256;
    std::string afterSha256;
};

struct TxService {
    std::string name;
    bool restarted = false;
    bool active = false;
};

struct Transaction {
    std::string id;
    std::string operation = "apply";  // apply / delete (audit action config.apply / config.delete)
    std::string requestId;
    std::string state;
    std::string message;
    std::int64_t createdAt = 0;
    std::int64_t updatedAt = 0;
    std::string operatorName;
    std::string operatorId;
    std::string ticketId;
    std::string reason;
    std::vector<TxFile> files;
    std::vector<TxService> services;
    bool healthKnown = false;
    bool healthOk = false;
    std::vector<std::string> healthFailed;
    bool rollbackPerformed = false;
    std::string rollbackReason;
    int restoredFiles = 0;
    std::string rollbackServices;  // "" / pending / restarting / done / failed
    bool rollbackHealthKnown = false;
    bool rollbackHealthOk = false;
    std::vector<std::string> rollbackHealthFailed;
    std::string restoreRequestId;
    std::string restoreTicketId;
    bool restartedServices = false;
    // Stamped on every save: staleness is judged by boot and monotonic time, not the wall clock (an NTP step
    // must neither cut a health check short nor keep an interrupted transaction from being recovered).
    std::string savedBootId;
    std::int64_t savedMonoMs = 0;

    // "restoring": the server restores a committed transaction (journaled first; recovery finishes it).
    bool finished() const { return state == "committed" || state == "rolled-back" || state == "failed"; }
    // Still owns the services: unfinished, or rolled back with its service restart not done yet.
    bool busy() const { return !finished() || rollbackServices == "pending" || rollbackServices == "restarting"; }
};

// No progress for graceMs: another boot since the last save, or that much monotonic time on this boot.
bool transactionStale(const Transaction& tx, std::int64_t graceMs);

std::string transactionJson(const Transaction& tx);  // client view (handoff 3.5)
std::string transactionAction(const Transaction& tx);  // config.apply / config.delete

struct PlannedFile {
    std::string path;
    bool remove = false;
    std::string content;
    bool existed = false;
    bool changed = false;
    std::string beforeSha256;
};

struct Services {
    explicit Services(const DirectConfig& config, const Options& options);
    std::vector<std::string> expectedUnits(std::string* error) const;  // gateway-services.service + desired units
    std::string state(const std::string& unit) const;
    bool restart(std::string* error, const std::function<bool()>& cancel = std::function<bool()>()) const;

private:
    std::string command_;
    std::string home_;
};

class TxStore {
public:
    TxStore(const DirectConfig& config, const Options& options);
    std::string directory(const std::string& id) const;
    bool load(const std::string& id, Transaction* tx) const;
    bool save(const Transaction& tx, std::string* error = nullptr) const;
    bool findByRequestId(const std::string& requestId, Transaction* tx) const;
    bool findActive(Transaction* tx) const;
    std::vector<std::string> ids() const;
    // Writes staged contents and the journal (state accepted). Throws on failure.
    Transaction create(const std::string& requestId, const std::string& operatorName, const std::string& operatorId,
                       const std::string& ticketId, const std::string& reason, const std::vector<PlannedFile>& files,
                       const std::string& operation = "apply");
    bool helperRunning(const std::string& id) const;
    bool launchHelper(const std::string& id, const std::string& mode, std::string* error) const;
    // Client restore of an in-progress transaction: the helper rolls back at its next step. False: marker not written.
    bool requestAbort(const std::string& id, const std::string& reason, const std::string& requestId, const std::string& ticketId) const;
    bool abortRequested(const std::string& id, std::string* reason, std::string* requestId = nullptr, std::string* ticketId = nullptr) const;
    void prune(std::size_t keep) const;
    const std::string& root() const { return root_; }

private:
    DirectConfig config_;
    Options options_;
    std::string root_;
};

// Relaunches helpers for transactions whose helper died (power loss, kill): roll back / finish a restore / finish
// the service restart. Runs from housekeeping, and on its own when the v2 server cannot start. Returns how many
// transactions still need a helper.
int recoverInterruptedTransactions(const TxStore& store);

// Helper side (runs in the helper process or, for restore, under the tx lock).
class TxRunner {
public:
    TxRunner(const DirectConfig& config, const Options& options);
    int apply(const std::string& id);
    int restartAfterRestore(const std::string& id);
    int recover(const std::string& id);
    // Restores the transaction's files (backup / delete created). Caller holds the lock.
    bool restoreFiles(Transaction* tx, std::string* error);

private:
    bool healthCheck(Transaction* tx, bool rollbackPhase, const std::string& abortId);
    // restartAlways: restart (and check) the services whenever a file was restored, even if this transaction
    // never restarted them (recovery after power loss: the services started on the half-written files).
    void rollback(Transaction* tx, const std::string& reason, bool restartAlways = false);
    void audit(const Transaction& tx, const std::string& action, const std::string& result);
    bool save(Transaction* tx);

    DirectConfig config_;
    Options options_;
    TxStore store_;
    Services services_;
};

}  // namespace maintenance_v2
}  // namespace edge_gateway
