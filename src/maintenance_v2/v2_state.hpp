#pragma once

// Maintenance v2 persistent state: session tokens, device audit (hash chain)
// and debug mode. All files live in the state directory (root only, 0600).

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace edge_gateway {
namespace maintenance_v2 {

// Bearer tokens: only SHA-256(token) is stored, so sessions survive a
// SystemMonitor restart (e.g. the restart done by a config transaction).
class SessionStore {
public:
    SessionStore(std::string file, std::int64_t ttlMs, std::int64_t maxMs);
    enum class Check { Ok, Missing, Expired };
    std::string create(const std::string& client, std::int64_t now, std::int64_t* expiresAt);
    Check touch(const std::string& token, std::int64_t now, std::int64_t* expiresAt);
    // Same check as touch() without refreshing the session (used before reading a request body).
    bool valid(const std::string& token, std::int64_t now);
    void revoke(const std::string& token);

private:
    struct Entry {
        std::string hash;
        std::string client;
        std::int64_t createdAt = 0;
        std::int64_t lastUsedAt = 0;
    };
    void load();
    void save();
    std::string file_;
    std::int64_t ttlMs_;
    std::int64_t maxMs_;
    std::mutex mutex_;
    std::vector<Entry> entries_;
    bool loaded_ = false;
    std::int64_t savedAt_ = 0;
};

struct AuditEntry {
    std::int64_t seq = 0;
    std::int64_t ts = 0;
    std::string operatorName;
    std::string ticketId;
    std::string action;
    std::string target;
    std::string result;
    std::string transactionId;
    std::string clientIp;  // refusals only; omitted from the JSON (and the hash) when empty
    std::string prevHash;
    std::string hash;
};

// Append-only, hash-chained: hash = SHA-256(prevHash + JCS(entry without "hash")),
// prevHash of the first entry = 64 x "0". Safe across processes (flock).
class AuditLog {
public:
    explicit AuditLog(std::string file) : file_(std::move(file)) {}
    bool append(AuditEntry entry, std::string* error = nullptr);
    std::vector<AuditEntry> since(std::int64_t sinceSeq, std::size_t limit) const;
    static std::string hashOf(const AuditEntry& entry);
    static std::string toJson(const AuditEntry& entry, bool withHash);

private:
    std::string file_;
};

struct DebugMode {
    bool active = false;
    std::int64_t since = 0;
    std::string operatorName;
    std::string operatorId;
    std::string reason;
    std::string ticketId;
    std::int64_t lastActivityAt = 0;
};

class DebugModeStore {
public:
    explicit DebugModeStore(std::string file) : file_(std::move(file)) {}
    DebugMode get();
    void set(const DebugMode& mode);
    // Ends debug mode; returns true (and the mode that ended) when it was active.
    bool exit(DebugMode* ended);
    // Records activity; returns true when an idle timeout just ended debug mode.
    // Read-check-write happens under one lock, so a request that read the mode just before an exit
    // can never write the old (active) mode back over it.
    // activity=false (housekeeping): only checks the idle limit, never counts as activity.
    bool touch(std::int64_t now, std::int64_t idleMs, DebugMode* ended, bool activity = true);

private:
    DebugMode readLocked() const;
    void writeLocked(const DebugMode& mode) const;

    std::string file_;
    std::mutex mutex_;
};

}  // namespace maintenance_v2
}  // namespace edge_gateway
