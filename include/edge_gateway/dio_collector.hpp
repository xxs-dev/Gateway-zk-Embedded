#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "edge_gateway/common/collector_base.hpp"

namespace edge_gateway {

class DioCollector : public CollectorBase {
public:
    DioCollector(
        DeviceConfig config,
        MemoryPointStore& store,
        std::shared_ptr<IGpioPort> gpioPort,
        std::shared_ptr<IMqttPublisher> mqttPublisher = nullptr
    );

    CollectCycleResult collectOnce(std::int64_t nowMs, bool realtimeFocused = false) override;

private:
    // Continuous output-state vs contact-feedback supervision of a DO with write.verifyIndex. Never
    // writes the output; it only raises / clears the alarm.
    struct FeedbackMonitor {
        std::size_t doPoint = 0;  // config_.points index
        double lastOutput = -1.0;
        std::int64_t outputChangedMono = 0;
        bool mismatching = false;
        std::int64_t mismatchSinceMono = 0;
        bool agreeing = false;
        std::int64_t agreeSinceMono = 0;
        bool alarmActive = false;
        double lastFeedback = -1.0;
        std::int64_t lastEvaluatedMono = 0;
    };

    void superviseFeedback(std::int64_t wallMs, std::int64_t monoMs);
    void setFeedbackAlarm(FeedbackMonitor& monitor, bool active, double output, double feedback, std::int64_t wallMs);
    PointValue feedbackAlarmValue(const PointDefinition& point, std::int64_t wallMs) const;

    // wallMs timestamps the value; debouncing runs on the steady clock so a wall-clock step cannot
    // hold back (or prematurely accept) an input change.
    PointValue collectLocalDioPoint(const PointDefinition& point, std::int64_t wallMs, std::int64_t monoMs);

    std::shared_ptr<IGpioPort> gpioPort_;
    std::unordered_set<int> initializedReadGpios_;
    std::unordered_map<std::uint32_t, double> lastDioRawValues_;
    std::unordered_map<std::uint32_t, double> lastDioStableValues_;
    std::unordered_map<std::uint32_t, std::int64_t> lastDioRawChangeMonoMs_;
    std::unordered_map<std::uint32_t, double> lastGoodValues_;  // last good logical value per point index
    std::vector<FeedbackMonitor> feedbackMonitors_;
};

}  // namespace edge_gateway
