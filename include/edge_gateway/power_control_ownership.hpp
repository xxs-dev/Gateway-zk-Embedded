#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "edge_gateway/compat.hpp"

namespace edge_gateway {

struct PowerControlCommandReceipt {
    std::string id;
    bool accepted = false;
    std::string fingerprint;
};

struct PowerControlOwnershipState {
    std::string scope;
    std::string owner;
    std::string sessionId;
    std::vector<std::uint32_t> targetIndexes;
    std::int64_t heartbeatAtMs = 0;
    std::int64_t expireAtMs = 0;
    std::uint32_t generation = 0;
    std::string lastCommandId;
    std::vector<PowerControlCommandReceipt> receipts;
};

struct PowerControlTakeoverResult {
    bool accepted = false;
    bool duplicate = false;
    std::uint32_t generation = 0;
    std::string message;
};

struct PowerControlAuthorizationResult {
    bool allowed = false;
    std::uint32_t generation = 0;
    std::string message;
};

struct PowerControlReceiptLookup {
    bool found = false;
    bool accepted = false;
    std::string fingerprint;
};

class PowerControlOwnership {
public:
    PowerControlOwnership(std::string path, std::string owner);

    bool enabled() const;
    Optional<PowerControlOwnershipState> active(std::int64_t nowMs) const;
    bool isBlocked(std::uint32_t index, const std::string& source, std::int64_t nowMs) const;
    bool acquire(
        const std::string& scope,
        const std::string& sessionId,
        const std::vector<std::uint32_t>& targetIndexes,
        std::int64_t nowMs,
        int ttlMs
    ) const;
    bool renew(const std::string& sessionId, std::int64_t nowMs, int ttlMs) const;
    void release(const std::string& sessionId = std::string()) const;
    PowerControlTakeoverResult acquireOrRenew(
        const std::string& scope,
        const std::string& sessionId,
        const std::vector<std::uint32_t>& targetIndexes,
        const std::string& commandId,
        std::int64_t nowMs,
        int ttlMs,
        const std::string& commandFingerprint = std::string()
    ) const;
    std::uint32_t releaseAndAdvance(const std::string& sessionId) const;
    PowerControlAuthorizationResult authorize(
        std::uint32_t index,
        const std::string& source,
        std::uint32_t commandGeneration,
        bool highPriority,
        std::int64_t nowMs
    ) const;
    PowerControlReceiptLookup lookupReceipt(const std::string& commandId) const;
    bool recordReceipt(
        const std::string& sessionId,
        const std::string& commandId,
        std::uint32_t generation,
        bool accepted,
        const std::string& commandFingerprint = std::string()
    ) const;
    bool recordDetachedReceipt(
        const std::string& commandId,
        bool accepted,
        const std::string& commandFingerprint
    ) const;

private:
    std::string path_;
    std::string owner_;
};

}  // namespace edge_gateway
