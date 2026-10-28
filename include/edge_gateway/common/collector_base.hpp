#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {

struct CollectCycleResult {
    std::vector<ReadTask> executedTasks;
    std::vector<PointValue> values;
    // Replan promptly after a time slice, still honoring the daemon's control lease.
    bool workDeferred = false;
    // Positive values bound the wait until the next completion-based due time.
    int nextDelayMs = 0;
};

class ICollector {
public:
    virtual ~ICollector() = default;

    virtual CollectCycleResult collectOnce(std::int64_t nowMs, bool realtimeFocused = false) = 0;
    virtual void publishDeviceOnlineStatus(bool online, std::int64_t nowMs) const = 0;
    // The collector's own online verdict (failure thresholds applied), published when collectOnce()
    // throws. Collectors without thresholds report offline on any failed cycle.
    virtual bool deviceOnline() const { return false; }
};

class CollectorBase : public ICollector {
public:
    CollectorBase(
        DeviceConfig config,
        MemoryPointStore& store,
        std::shared_ptr<IMqttPublisher> mqttPublisher = nullptr
    );

    void publishDeviceOnlineStatus(bool online, std::int64_t nowMs) const override;

protected:
    PointValue buildFailedPointValue(
        const PointDefinition& point,
        const std::string& message,
        std::int64_t nowMs
    ) const;
    PointValue buildPointValue(
        const PointDefinition& point,
        const DecodedValue& decoded,
        std::int64_t nowMs
    ) const;
    PointValue buildDeviceOnlineValue(
        const PointDefinition& point,
        bool online,
        std::int64_t nowMs
    ) const;
    std::vector<PointDefinition> duePoints(std::int64_t nowMs, bool forceDue = false);
    int effectiveIntervalMs(const PointDefinition& point) const;
    // Expiry for a freshly read value: ts + min(cachePolicy.ttlMs, stale horizon) (CollectConfig).
    std::int64_t valueExpireAt(const PointDefinition& point, std::int64_t nowMs) const;
    // Call first thing in collectOnce(). nowMs is wall-clock time (it timestamps values); when it moves
    // backwards (NTP or a manual clock set) every stored scheduling time is shifted by the same amount,
    // so intervals and backoffs keep their remaining length instead of freezing collection.
    // Returns the shift applied (negative) or 0.
    std::int64_t observeSchedulingClock(std::int64_t nowMs);
    // Subclasses shift their own scheduling times (backoffs, last-attempt times) by deltaMs.
    virtual void onSchedulingClockStepped(std::int64_t deltaMs) { (void)deltaMs; }
    bool shouldSkipFailedCollectionCycle();
    void recordCollectionCycleSuccess();
    void recordCollectionCycleFailure();
    void publishTelemetryBestEffort(const std::vector<PointValue>& values) const;

    DeviceConfig config_;
    MemoryPointStore& store_;
    std::shared_ptr<IMqttPublisher> mqttPublisher_;
    std::unordered_map<std::uint32_t, std::int64_t> lastReadMs_;
    std::unordered_map<std::uint32_t, std::int64_t> lastValueUpdateMs_;
    std::int64_t lastObservedNowMs_ = 0;
    int collectionCycleFailures_ = 0;
    int collectionSkipCyclesRemaining_ = 0;
};

}  // namespace edge_gateway
