#pragma once

// Runs a point mapping (see point_mapping.hpp) inside ComputeEngine: inputs before the strategy graphs,
// outputs after them, and a status file for maintenance (run/point-mapping-status.json).

#include <cstdint>
#include <string>
#include <vector>

#include "edge_gateway/point_mapping.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace edge_gateway {

struct PointMappingRunResult {
    std::size_t latestWrites = 0;
    std::size_t deviceWrites = 0;
    std::size_t deviceWritesSkipped = 0;
    std::vector<std::string> errors;  // rejected device writes (counted like graph node errors)
};

class PointMappingStage {
public:
    // outputTtlMs: how long an input virtual value stays valid without a refresh (ComputeEngine stopped -> expires).
    // maxFutureSkewMs: source values stamped further in the future are invalid (<= 0 disables).
    PointMappingStage(PointMapping mapping, PointStoreRouter& router, std::int64_t outputTtlMs, std::int64_t maxFutureSkewMs);

    PointMappingRunResult runInputs(std::int64_t nowMs);
    PointMappingRunResult runOutputs(std::int64_t nowMs, std::size_t maxDeviceWrites);
    std::string statusJson(std::int64_t nowMs) const;
    std::size_t size() const { return mapping_.entries.size(); }

private:
    struct EntryState {
        // inputs
        bool published = false;
        bool lastValid = false;
        double lastValue = 0.0;
        std::int64_t lastPublishAt = 0;
        // outputs
        bool sent = false;
        double sentValue = 0.0;
        std::int64_t sentAt = 0;
        std::string cmdId;
        std::int64_t verifyDeadline = 0;
        double commandValue = 0.0;  // EMS value behind the last command
        std::uint64_t verifyFailures = 0;
        // both
        std::string state = "idle";  // in: valid / invalid; out: no-command / verifying / ok / readback-timeout / writeback-failed / rejected / enum-miss
        std::string message;
    };

    bool sourceValid(const Optional<StoredPointValue>& v, std::int64_t nowMs, std::int64_t maxAgeMs, std::string* why) const;
    void setState(EntryState& s, const PointMappingEntry& e, const std::string& state, const std::string& message);

    PointMapping mapping_;
    PointStoreRouter& router_;
    std::int64_t outputTtlMs_;
    std::int64_t maxFutureSkewMs_;
    std::vector<EntryState> states_;
    std::uint64_t sequence_ = 0;
};

}  // namespace edge_gateway
