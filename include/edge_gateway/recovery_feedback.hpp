#pragma once
#include <cmath>
#include <cstdint>
#include "edge_gateway/manual_recovery.hpp"

namespace edge_gateway {
// Numerical execution monitoring. This deliberately makes no claim about a command receipt.
// A moving target does not restart the deadline. Matching must remain stable before clearing it.
class RecoveryFeedback {
    struct Track {
        std::int64_t mismatchAt = -1, matchingAt = -1;
        bool timedOut(bool matches, std::int64_t now, std::int64_t timeout, std::int64_t stable) {
            if (matches) {
                if (matchingAt < 0) matchingAt = now;
                if (now - matchingAt >= stable) { mismatchAt = -1; return false; }
            } else {
                matchingAt = -1;
                if (mismatchAt < 0) mismatchAt = now;
            }
            return mismatchAt >= 0 && now - mismatchAt >= timeout;
        }
    };
    Track request_, feedback_;
public:
    void clear() { request_ = Track{}; feedback_ = Track{}; }
    ManualRecovery::Reason update(double request, double readback, double actual,
            double requestTolerance, double feedbackTolerance,
            std::int64_t now, std::int64_t requestTimeout, std::int64_t feedbackTimeout,
            std::int64_t stable) {
        const bool requestTimeoutReached = request_.timedOut(std::abs(request - readback) <= requestTolerance,
            now, requestTimeout, stable);
        const bool feedbackTimeoutReached = feedback_.timedOut(std::abs(request - actual) <= feedbackTolerance,
            now, feedbackTimeout, stable);
        return requestTimeoutReached ? ManualRecovery::RequestTimeout :
            feedbackTimeoutReached ? ManualRecovery::FeedbackTimeout : ManualRecovery::None;
    }
};
} // namespace edge_gateway
