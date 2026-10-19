#pragma once

#include <cstdint>

namespace edge_gateway {

// A run-permission latch, not an actuator or a physical-stop acknowledgement.
// Deliberately never restored from the graph state file or a cached output.
class ManualRecovery {
public:
    enum State { Locked = 0, Ready = 1, Running = 2 };
    enum Reason { None = 0, Startup = 1, InvalidInput = 2, Fault = 3,
                  PermitWithdrawn = 4, ConflictingActions = 5, ScanGap = 6 };
    struct Inputs {
        // Exact binary values; -1 means missing, stale, bad quality or non-binary.
        int healthy = -1, fault = -1, permit = -1, stopped = -1;
        int reset = -1, start = -1;
        std::int64_t resetTs = 0, startTs = 0;
    };
    struct Result { State state; Reason reason; bool permitted; };

    Result update(const Inputs& input, std::int64_t now, std::int64_t maxGapMs) {
        const bool gap = initialized_ && (now <= lastAt_ || now - lastAt_ > maxGapMs);
        const bool first = !initialized_;
        initialized_ = true;
        lastAt_ = now;
        if (gap) lock(ScanGap);
        const bool reset = reset_.observe(input.reset, input.resetTs);
        const bool start = start_.observe(input.start, input.startTs);
        const bool valid = binary(input.healthy) && binary(input.fault) &&
            binary(input.permit) && binary(input.stopped) &&
            binary(input.reset) && binary(input.start);
        if (!valid || input.healthy != 1) {
            lock(InvalidInput);
        } else if (input.fault != 0) {
            lock(Fault);
        } else if (input.reset == 1 && input.start == 1) {
            lock(ConflictingActions);
        } else if (state_ == Running && input.permit != 1) {
            lock(PermitWithdrawn);
        } else if (!first && !gap) {
            if (state_ == Locked && reset && input.start == 0 &&
                input.permit == 0 && input.stopped == 1) {
                state_ = Ready;
                reason_ = None;
                start_ = Edge{}; // A start edge must be observed AFTER reset.
            } else if (state_ == Ready && start && input.reset == 0 &&
                       input.permit == 1 && input.stopped == 1) {
                state_ = Running;
                reason_ = None;
            }
        }
        return {state_, reason_, state_ == Running};
    }

private:
    struct Edge {
        bool lowSeen = false;
        std::int64_t lowTs = 0, latestTs = 0;
        bool observe(int value, std::int64_t ts) {
            if (!binary(value) || ts <= 0 || ts < latestTs) {
                *this = Edge{};
                return false;
            }
            latestTs = ts;
            if (value == 0) { lowSeen = true; lowTs = ts; return false; }
            const bool rising = lowSeen && ts > lowTs;
            lowSeen = false;
            return rising;
        }
    };
    static bool binary(int value) { return value == 0 || value == 1; }
    void lock(Reason reason) {
        if (state_ != Locked) reason_ = reason; // Preserve the first trip cause.
        state_ = Locked;
        reset_ = Edge{};
        start_ = Edge{};
    }
    State state_ = Locked;
    Reason reason_ = Startup;
    bool initialized_ = false;
    std::int64_t lastAt_ = 0;
    Edge reset_, start_;
};

} // namespace edge_gateway
