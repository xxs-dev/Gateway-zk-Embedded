#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>

namespace edge_gateway {

class PersistentFlushSchedule {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr int kCheckIntervalMs = 250;

    explicit PersistentFlushSchedule(int intervalMs, Clock::time_point now = Clock::now())
        : interval_(std::max(1000, intervalMs)), nextPeriodic_(now) {}

    static std::size_t highWatermark(std::size_t limit) {
        // ceil(3 * limit / 4), without overflowing at large configured limits.
        return std::max<std::size_t>(1, limit - limit / 4);
    }

    bool shouldFlush(std::size_t count, std::size_t limit, Clock::time_point now = Clock::now()) {
        if (retryPending_ && now < retryAt_) return false;
        const bool periodicDue = now >= nextPeriodic_;
        if (periodicDue) {
            nextPeriodic_ += interval_;
            if (nextPeriodic_ <= now) nextPeriodic_ = now + interval_;
        }
        // Pressure flushes do not postpone the configured periodic deadline.
        return count > 0 && (periodicDue || retryPending_ || count >= highWatermark(limit));
    }

    void succeeded() { retryPending_ = false; }

    void failed(Clock::time_point now = Clock::now()) {
        retryPending_ = true;
        retryAt_ = now + std::chrono::milliseconds(1000);
    }

private:
    std::chrono::milliseconds interval_;
    Clock::time_point nextPeriodic_;
    Clock::time_point retryAt_{};
    bool retryPending_ = false;
};

}  // namespace edge_gateway
