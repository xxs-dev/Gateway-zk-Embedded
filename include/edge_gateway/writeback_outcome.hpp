#pragma once

#include <string>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

// One classification for every control receipt (shared memory, control ledger, MQTT reply, direct
// maintenance, third-party forwarder, pointctl). outcome decides whether a caller may act on it:
//   succeeded  stage writeback-succeeded (or a local/mailbox commit stage)
//   failed     the write definitely did not take effect: writeback-failed (refused before the wire
//              or a conclusive device rejection), expired, control-rejected, writeback-skipped, ...
//   unknown    the write may have taken effect; never retry it automatically or under a new cmdId:
//              writeback-mismatch     accepted, but read-back/feedback shows another value
//              writeback-timeout      sent, no conclusive reply
//              writeback-unconfirmed  dispatch reserved in the control ledger, result never recorded
//                                     (executing now, or interrupted); never dispatched again
// The executor message is kept verbatim in every stage.
constexpr const char* kWritebackSucceededStage = "writeback-succeeded";
constexpr const char* kWritebackFailedStage = "writeback-failed";
constexpr const char* kWritebackExpiredStage = "expired";
constexpr const char* kWritebackMismatchStage = "writeback-mismatch";
constexpr const char* kWritebackTimeoutStage = "writeback-timeout";
constexpr const char* kWritebackUnconfirmedStage = "writeback-unconfirmed";

inline bool isUnknownOutcomeStage(const std::string& stage) {
    return stage == kWritebackMismatchStage || stage == kWritebackTimeoutStage ||
           stage == kWritebackUnconfirmedStage;
}

// Makes stage and flags agree (the ledger persists the stage; shared memory persists the flags).
inline void normalizeWritebackOutcome(WritebackResultRecord& result) {
    if (result.success) {
        result.outcomeUnknown = false;
        result.feedbackMismatch = false;
        return;
    }
    if (result.feedbackMismatch || result.stage == kWritebackMismatchStage) {
        result.feedbackMismatch = true;
        result.outcomeUnknown = true;
        result.stage = kWritebackMismatchStage;
        return;
    }
    if (isUnknownOutcomeStage(result.stage)) {
        result.outcomeUnknown = true;
    } else if (result.outcomeUnknown) {
        result.stage = kWritebackTimeoutStage;
    }
}

inline const char* writebackOutcome(const WritebackResultRecord& result) {
    if (result.success) {
        return "succeeded";
    }
    return result.outcomeUnknown || result.feedbackMismatch || isUnknownOutcomeStage(result.stage)
        ? "unknown" : "failed";
}

// A receipt written by a release without the three-state classification (dabb8fd, 29f5068): its stage
// is kept verbatim. Only a stage that never reaches a device stays a definite failure; any other
// failure may have taken effect (e.g. a dabb8fd Modbus timeout or a 29f5068 read-back mismatch, both
// recorded as writeback-failed), so it is reported unknown and never offered for a retry.
inline void applyLegacyWritebackOutcome(WritebackResultRecord& result) {
    result.feedbackMismatch = false;
    if (result.success) {
        result.outcomeUnknown = false;
        return;
    }
    const auto& stage = result.stage;
    const bool neverDispatched = stage == kWritebackExpiredStage || stage == "control-rejected" ||
        stage == "rejected" || stage == "rate-limited" || stage == "writeback-skipped" ||
        stage == "malformed-rejected" || stage == "owner-died";
    if (!neverDispatched) result.outcomeUnknown = true;
}

// A ledger replay of a reserved dispatch without a recorded result: a final result may still follow.
inline bool isUnconfirmedWriteback(const WritebackResultRecord& result) {
    return !result.success && result.stage == kWritebackUnconfirmedStage;
}

}  // namespace edge_gateway
