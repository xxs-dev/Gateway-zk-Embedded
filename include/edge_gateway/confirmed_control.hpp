#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include "edge_gateway/models.hpp"
#include "edge_gateway/writeback_outcome.hpp"

namespace edge_gateway {
// One graph-owned transaction at a time. Restoring a transaction never restores arming.
struct ConfirmedControl {
    std::string cmdId;
    std::uint32_t index = 0;
    double value = 0;
    std::int64_t requestedAt = 0;
    bool pending = false;
    int fault = 0; // -1 rejected/failed, -2 outcome unknown, -3 storage, -4 invalid inputs/gap
    bool armed = false, confirmed = false;
    std::int64_t lastScan = 0, resetLowTs = 0, resetLatestTs = 0;
    bool resetLowSeen = false;

    int status() const { return fault ? fault : pending ? 2 : !armed ? 0 : confirmed ? 3 : 1; }
    void trip(int reason) { if (!fault) fault = reason; armed = false; resetLowSeen = false; }
    bool observeReset(int value, std::int64_t ts) {
        if ((value != 0 && value != 1) || ts <= 0 || ts < resetLatestTs) {
            resetLowSeen = false; return false;
        }
        resetLatestTs = ts;
        if (value == 0) { resetLowSeen = true; resetLowTs = ts; return false; }
        const bool edge = resetLowSeen && ts > resetLowTs;
        resetLowSeen = false;
        return edge;
    }
    // Only a final receipt of this exact transaction resolves it. Confirmation requires the driver's
    // own verification (verifyPassed): "submitted" (SocketCAN queue, DO latch without contact
    // feedback) and "written without read-back" are not an action confirmation. A definite failure
    // trips -1; anything that may have taken effect (unknown, mismatch, unconfirmed success) trips -2.
    bool observeReceipt(const WritebackResultRecord& receipt, std::int64_t now) {
        if (!pending || receipt.cmdId != cmdId || receipt.index != index ||
            !std::isfinite(receipt.value) || receipt.value != value ||
            receipt.requestedAt != requestedAt || receipt.completedAt < requestedAt ||
            receipt.completedAt > now) return false;
        // A ledger replay of a reserved dispatch without a result: the final result may still follow.
        if (isUnconfirmedWriteback(receipt)) return false;
        pending = false;
        confirmed = receipt.success && receipt.verifyAttempted && receipt.verifyPassed &&
                    receipt.stage != kWritebackSubmittedStage;
        if (!confirmed) trip(std::string(writebackOutcome(receipt)) == "failed" ? -1 : -2);
        // A late success resolves the in-flight transaction but never clears a timeout fault.
        return true;
    }
    void checkTimeout(std::int64_t now, std::int64_t timeout) {
        if (pending && (now < requestedAt || now - requestedAt >= timeout)) trip(-2);
    }
    bool reset(bool rising, bool stopped, int permit) {
        if (!rising || !stopped || permit != 0 || pending) return false;
        fault = 0; confirmed = false; armed = true;
        return true;
    }
};
// Where a confirmedControlWrite target gets its action confirmation and its readback.
//   modbus/dlt645/iec...: the driver's read-back of the written point (write.verifyAfterWrite and
//                         verifyByRead); readback = the target point.
//   local_dio:            an auxiliary contact DI (write.verifyIndex); the output latch read-back is
//                         only the output state. readback = the contact, mapped to the output state
//                         (write.verifyValue: contact value while the output is 1, default 1).
//   can_socketcan:        a device status signal (write.verifyIndex); a SocketCAN send is only a
//                         submission. readback = the status signal. Pulse signals have no level.
struct ConfirmedControlTargetSpec {
    std::string refusal;  // non-empty: this target cannot confirm an action
    std::uint32_t readbackIndex = 0;
    bool contactFeedback = false;
    double contactActiveValue = 1.0;
};

inline ConfirmedControlTargetSpec confirmedControlTargetSpec(
    const std::string& protocolType, bool writable, const WriteSpec& write, std::uint32_t targetIndex
) {
    ConfirmedControlTargetSpec spec;
    spec.readbackIndex = targetIndex;
    if (!writable) {
        spec.refusal = "target is not writable";
    } else if (protocolType == "local_dio") {
        if (write.verifyIndex == 0) {
            spec.refusal = "local_dio target needs write.verifyIndex (auxiliary contact DI); the output latch "
                           "read-back is not an action confirmation";
        } else if (write.verifyIndex == targetIndex) {
            spec.refusal = "local_dio write.verifyIndex must not be the output itself";
        } else {
            spec.readbackIndex = write.verifyIndex;
            spec.contactFeedback = true;
            spec.contactActiveValue = write.verifyValue ? *write.verifyValue : 1.0;
            if (spec.contactActiveValue != 0.0 && spec.contactActiveValue != 1.0)
                spec.refusal = "local_dio write.verifyValue must be 0 or 1";
        }
    } else if (protocolType == "can_socketcan") {
        if (write.verifyIndex == 0) {
            spec.refusal = "CAN target needs write.verifyIndex (device status signal); a SocketCAN send is "
                           "only a submission";
        } else if (write.verifyIndex == targetIndex) {
            spec.refusal = "CAN write.verifyIndex must not be the command signal itself";
        } else if (write.can.pulse) {
            spec.refusal = "CAN pulse signals have no level to confirm";
        } else {
            spec.readbackIndex = write.verifyIndex;
        }
    } else if (!write.verifyAfterWrite || !write.verifyByRead) {
        spec.refusal = "target needs driver read verification (write.verifyAfterWrite and verifyByRead)";
    }
    return spec;
}

// The readback in the target's value space; false when the sample cannot be used (a contact that is
// neither 0 nor 1).
inline bool confirmedControlReadback(const ConfirmedControlTargetSpec& spec, double raw, double* value) {
    if (!std::isfinite(raw)) return false;
    if (!spec.contactFeedback) {
        *value = raw;
        return true;
    }
    if (raw != 0.0 && raw != 1.0) return false;
    *value = raw == spec.contactActiveValue ? 1.0 : 0.0;
    return true;
}

} // namespace edge_gateway
