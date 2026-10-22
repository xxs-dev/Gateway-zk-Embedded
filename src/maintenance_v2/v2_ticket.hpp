#pragma once

// Field authorization tickets (handoff doc section 4): platform-signed,
// verified offline on the device.
//
//   wire   = base64url(JCS(payload)) "." base64url(Ed25519 signature)   (no padding)
//   signed = "GZ-FIELD-TICKET-V1\n" + JCS(payload)
//
// Check order and reject codes: ticket_required / malformed -> unknown_key ->
// bad_signature -> machine_mismatch -> not_yet_valid / expired (+-300 s) ->
// action_not_allowed / level_too_low -> path_not_allowed / point_not_allowed -> replayed

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace edge_gateway {
namespace maintenance_v2 {

struct Ticket {
    std::string ticketId;
    std::string issuer;
    std::string keyId;
    std::string machineCode;
    std::string operatorId;
    std::string operatorName;
    std::vector<std::string> actions;
    int level = 0;
    std::vector<std::string> configPaths;    // empty = unrestricted
    std::vector<std::uint32_t> pointIndexes;  // empty = unrestricted
    std::string reason;
    std::int64_t issuedAt = 0;
    std::int64_t notBefore = 0;
    std::int64_t expiresAt = 0;
    bool oneTime = false;

    std::string operatorLabel() const { return operatorName.empty() ? operatorId : operatorName; }
};

struct TrustedKey {
    std::string keyId;
    std::string publicKey;  // raw 32 bytes
    std::string source;     // "built-in" / file name
};

struct TicketRequirement {
    std::string action;
    int level = 0;
    std::string machineCode;
    std::vector<std::string> paths;
    std::vector<std::uint32_t> points;
    std::int64_t nowMs = 0;
};

struct TicketVerdict {
    bool ok = false;
    std::string code;     // empty when ok
    std::string message;  // English detail for logs
    Ticket ticket;
};

const std::int64_t kTicketClockSkewMs = 300000;

// Steps 1-7 (everything except the one-time replay check).
TicketVerdict verifyTicket(const std::string& wire, const std::vector<TrustedKey>& keys, const TicketRequirement& requirement);

// Built-in platform keys (firmware) plus `<materialDirectory>/trusted-ticket-keys.json`:
// {"keys":[{"keyId":"...","algorithm":"Ed25519","publicKey":"<base64 of 32 bytes>"}]}
std::vector<TrustedKey> loadTrustedKeys(const std::string& file, std::string* warning);

// One-time tickets already used on this device (kept until expiry + skew), persisted.
class ReplayStore {
public:
    explicit ReplayStore(std::string file) : file_(std::move(file)) {}
    bool used(const std::string& ticketId);
    // Returns false when the ticket was already used (race-free under the store mutex + file lock).
    bool markUsed(const Ticket& ticket, std::int64_t nowMs);

private:
    std::string file_;
    std::mutex mutex_;
};

// Level the device requires for an action (safety parameters raise control.batch to L3).
int requiredLevel(const std::string& action, const std::vector<std::uint32_t>& points);
bool isSafetyParameter(std::uint32_t index);

}  // namespace maintenance_v2
}  // namespace edge_gateway
