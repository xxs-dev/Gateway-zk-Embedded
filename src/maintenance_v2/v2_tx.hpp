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

    bool finished() const { return state == "committed" || state == "rolled-back" || state == "failed"; }
};

std::string transactionJson(const Transaction& tx);  // client view (handoff 3.5)

struct PlannedFile {
    std::string path;
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
                       const std::string& ticketId, const std::string& reason, const std::vector<PlannedFile>& files);
    bool helperRunning(const std::string& id) const;
    bool launchHelper(const std::string& id, const std::string& mode, std::string* error) const;
    // Client restore of an in-progress transaction: the helper rolls back at its next step.
    void requestAbort(const std::string& id, const std::string& reason, const std::string& requestId, const std::string& ticketId) const;
    bool abortRequested(const std::string& id, std::string* reason, std::string* requestId = nullptr, std::string* ticketId = nullptr) const;
    void prune(std::size_t keep) const;
    const std::string& root() const { return root_; }

private:
    DirectConfig config_;
    Options options_;
    std::string root_;
};

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
    void rollback(Transaction* tx, const std::string& reason);
    void audit(const Transaction& tx, const std::string& action, const std::string& result);
    void save(Transaction* tx);

    DirectConfig config_;
    Options options_;
    TxStore store_;
    Services services_;
};

}  // namespace maintenance_v2
}  // namespace edge_gateway
