#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "edge_gateway/common/collector_base.hpp"

namespace edge_gateway {

class Dlt645Client;

// Scheduling: every DI is one request serving all points that share it (block DIs use
// read.dlt645.offset). Each cycle reads the DIs that are due, most overdue first: realtime DIs
// (collectPriority 0) up to maxTasksPerMeterPerCycle (realtime lease: realtimeMax...), then
// background DIs (collectPriority > 0) in the unused slots plus maxBackgroundTasksPerMeterPerCycle
// every backgroundTaskIntervalMs. The first read timeout ends the cycle, so a dead meter costs one
// timeout per cycle and does not starve the other meters on the bus.
// Failures: a DI keeps its last good value until failureBadQualityThreshold consecutive failures
// (or failureGoodValueGraceMs); offlineFailureThreshold consecutive cycles without any reply mark
// the meter offline and every point bad quality; recoverySuccessThreshold cycles bring it back.
class Dlt645Collector : public CollectorBase {
public:
    Dlt645Collector(
        DeviceConfig config,
        MemoryPointStore& store,
        std::shared_ptr<Dlt645Client> dlt645Client,
        std::shared_ptr<IMqttPublisher> mqttPublisher = nullptr
    );

    CollectCycleResult collectOnce(std::int64_t nowMs, bool realtimeFocused = false) override;

    bool meterOnline() const { return online_; }

private:
    struct ReadGroup {
        std::string dataId;
        std::vector<std::size_t> points;  // positions in config_.points
        bool realtime = false;            // any point with collectPriority 0
        int consecutiveFailures = 0;
        int retryRounds = 0;
    };

    std::int64_t dueAtMs(const ReadGroup& group) const;
    void storeValue(const PointDefinition& point, PointValue value, std::int64_t nowMs, CollectCycleResult& result);
    void storeGroupData(const ReadGroup& group, const std::vector<std::uint8_t>& data,
                        std::int64_t nowMs, CollectCycleResult& result);
    void storeGroupError(const ReadGroup& group, const std::string& message,
                         std::int64_t nowMs, CollectCycleResult& result);
    void noteGroupFailure(ReadGroup& group, const std::string& message,
                          std::int64_t nowMs, CollectCycleResult& result);
    void markMeterOffline(const std::string& message, std::int64_t nowMs, CollectCycleResult& result);
    void onSchedulingClockStepped(std::int64_t deltaMs) override;

    std::shared_ptr<Dlt645Client> dlt645Client_;
    std::vector<ReadGroup> groups_;
    std::int64_t lastBackgroundReadMs_ = -1;
    int failedCycles_ = 0;
    int successfulCycles_ = 0;
    bool online_ = false;
    bool offlineMarked_ = false;
    std::set<std::uint32_t> storedThisCycle_;
};

}  // namespace edge_gateway
