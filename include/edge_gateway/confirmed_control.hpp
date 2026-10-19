#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include "edge_gateway/models.hpp"

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
    bool observeReceipt(const WritebackResultRecord& receipt, std::int64_t now) {
        if (!pending || receipt.cmdId != cmdId || receipt.index != index ||
            !std::isfinite(receipt.value) || receipt.value != value ||
            receipt.requestedAt != requestedAt || receipt.completedAt < requestedAt ||
            receipt.completedAt > now) return false;
        pending = false;
        confirmed = receipt.success && receipt.verifyAttempted && receipt.verifyPassed;
        if (!confirmed) trip(-1);
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
} // namespace edge_gateway
