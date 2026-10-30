#include "edge_gateway/dio_collector.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "edge_gateway/dio_command_executor.hpp"

namespace edge_gateway {

namespace {

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::int64_t steadyTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

}  // namespace

DioCollector::DioCollector(
    DeviceConfig config,
    MemoryPointStore& store,
    std::shared_ptr<IGpioPort> gpioPort,
    std::shared_ptr<IMqttPublisher> mqttPublisher
) : CollectorBase(std::move(config), store, std::move(mqttPublisher)),
    gpioPort_(std::move(gpioPort)) {
    if (!gpioPort_) {
        throw std::invalid_argument("gpioPort is required");
    }
    for (std::size_t i = 0; i < config_.points.size(); ++i) {
        const auto& point = config_.points[i];
        if (point.enabled && isDioOutputPoint(point) && point.write.verifyIndex != 0 && point.read.gpio >= 0) {
            FeedbackMonitor monitor;
            monitor.doPoint = i;
            feedbackMonitors_.push_back(monitor);
        }
    }
}

CollectCycleResult DioCollector::collectOnce(std::int64_t nowMs, bool realtimeFocused) {
    (void)realtimeFocused;
    observeSchedulingClock(nowMs);
    CollectCycleResult result;
    if (shouldSkipFailedCollectionCycle()) {
        return result;
    }
    const auto points = duePoints(nowMs);
    bool hasSuccessfulRead = false;
    std::string firstFailureMessage;
    for (const auto& point : points) {
        PointValue value;
        const auto pointNowMs = currentTimeMs();
        try {
            if (point.read.dataType == "feedback_alarm") {
                value = feedbackAlarmValue(point, pointNowMs);
            } else {
                value = collectLocalDioPoint(point, pointNowMs, steadyTimeMs());
                hasSuccessfulRead = true;
                lastGoodValues_[point.index] = value.value;
            }
        } catch (const std::exception& ex) {
            if (firstFailureMessage.empty()) {
                firstFailureMessage = ex.what();
            }
            lastGoodValues_.erase(point.index);  // feedback supervision must not use an old value
            value = buildFailedPointValue(point, ex.what(), currentTimeMs());
        }
        lastReadMs_[point.index] = nowMs;
        lastValueUpdateMs_[point.index] = nowMs;
        if (point.read.cachePolicy.storeLatest) {
            store_.putLatest(value);
        }
        result.values.push_back(std::move(value));
    }
    superviseFeedback(currentTimeMs(), steadyTimeMs());
    if (!points.empty()) {
        publishDeviceOnlineStatus(hasSuccessfulRead, nowMs);
    }
    publishTelemetryBestEffort(result.values);
    if (!points.empty() && !hasSuccessfulRead && !firstFailureMessage.empty()) {
        recordCollectionCycleFailure();
        throw std::runtime_error(firstFailureMessage);
    }
    if (hasSuccessfulRead) {
        recordCollectionCycleSuccess();
    }
    return result;
}

PointValue DioCollector::collectLocalDioPoint(const PointDefinition& point, std::int64_t wallMs, std::int64_t monoMs) {
    if (point.read.gpio < 0) {
        throw std::runtime_error("local_dio point missing read.gpio: " + point.pointCode);
    }
    if (point.read.dataType != "digital_input" && point.read.dataType != "digital_output") {
        throw std::runtime_error("unsupported local_dio read.dataType: " + point.read.dataType);
    }
    if (point.read.dataType == "digital_input") {
        if (initializedReadGpios_.find(point.read.gpio) == initializedReadGpios_.end()) {
            gpioPort_->exportGpio(point.read.gpio);
            gpioPort_->setDirection(point.read.gpio, "in");
            initializedReadGpios_.insert(point.read.gpio);
        }
    } else if (isDioOutputPoint(point)) {
        // The startup state is applied before collection starts; a pin that was not available then
        // (or was switched to input externally) is driven to it here instead of reporting the
        // floating input level as the output state. An existing output is left untouched.
        if (gpioPort_->ensureOutput(point.read.gpio, dioStartupGpioHigh(point))) {
            std::cerr << "DO gpio " << point.read.gpio << " was not an output; driven to its startup state"
                      << " pointCode=" << point.pointCode << std::endl;
        }
    } else {
        gpioPort_->exportGpio(point.read.gpio);
    }
    const auto gpioHigh = gpioPort_->readValue(point.read.gpio);
    auto logicalValue = gpioHigh == point.read.activeHigh ? 1.0 : 0.0;
    if (point.read.debounceMs > 0) {
        const auto raw = lastDioRawValues_.find(point.index);
        if (raw == lastDioRawValues_.end()) {
            lastDioRawValues_[point.index] = logicalValue;
            lastDioStableValues_[point.index] = logicalValue;
            lastDioRawChangeMonoMs_[point.index] = monoMs;
        } else if (raw->second != logicalValue) {
            lastDioRawValues_[point.index] = logicalValue;
            lastDioRawChangeMonoMs_[point.index] = monoMs;
        }

        const auto stable = lastDioStableValues_.find(point.index);
        const auto changedAt = lastDioRawChangeMonoMs_.find(point.index);
        if (stable != lastDioStableValues_.end() && stable->second != logicalValue &&
            changedAt != lastDioRawChangeMonoMs_.end() && monoMs - changedAt->second >= point.read.debounceMs) {
            lastDioStableValues_[point.index] = logicalValue;
        }
        logicalValue = lastDioStableValues_[point.index];
    }

    DecodedValue decoded;
    decoded.value = logicalValue;
    decoded.text = logicalValue > 0.0 ? "1" : "0";
    decoded.rawHex = gpioHigh ? "01" : "00";
    return buildPointValue(point, decoded, wallMs);
}

void DioCollector::superviseFeedback(std::int64_t wallMs, std::int64_t monoMs) {
    for (auto& monitor : feedbackMonitors_) {
        if (monitor.lastEvaluatedMono > 0 && monoMs - monitor.lastEvaluatedMono < 50) {
            continue;
        }
        monitor.lastEvaluatedMono = std::max<std::int64_t>(1, monoMs);
        const auto& point = config_.points[monitor.doPoint];
        double output = 0.0;
        const auto collected = lastGoodValues_.find(point.index);
        if (collected != lastGoodValues_.end()) {
            output = collected->second;
        } else {
            try {
                output = gpioPort_->readValue(point.read.gpio) == point.read.activeHigh ? 1.0 : 0.0;
            } catch (const std::exception&) {
                continue;
            }
        }
        if (output != monitor.lastOutput) {
            // Every change of the output starts the grace time in which the contact may still move.
            monitor.lastOutput = output;
            monitor.outputChangedMono = monoMs;
            monitor.mismatching = false;
        }
        // Feedback: a DI of this device from this collector, any other point from the point store.
        double feedback = 0.0;
        const auto local = lastGoodValues_.find(point.write.verifyIndex);
        if (local != lastGoodValues_.end()) {
            feedback = local->second;
        } else {
            const auto stored = store_.getLatestByIndex(point.write.verifyIndex, wallMs);
            if (!stored || stored->stale || stored->quality != 1) {
                continue;  // no usable feedback: neither raise nor clear
            }
            feedback = stored->value;
        }
        monitor.lastFeedback = feedback;
        if (std::abs(feedback - dioExpectedFeedback(point, output)) > 1e-6) {
            monitor.agreeing = false;
            if (monoMs - monitor.outputChangedMono < std::max(0, point.write.verifyTimeoutMs)) {
                monitor.mismatching = false;  // still within the action grace time
                continue;
            }
            if (!monitor.mismatching) {
                monitor.mismatching = true;
                monitor.mismatchSinceMono = monoMs;
            }
            if (!monitor.alarmActive && monoMs - monitor.mismatchSinceMono >= std::max(0, point.write.feedbackAlarmDelayMs)) {
                setFeedbackAlarm(monitor, true, output, feedback, wallMs);
            }
        } else {
            monitor.mismatching = false;
            if (!monitor.alarmActive) {
                continue;
            }
            if (!monitor.agreeing) {
                monitor.agreeing = true;
                monitor.agreeSinceMono = monoMs;
            }
            if (monoMs - monitor.agreeSinceMono >= std::max(0, point.write.feedbackRecoveryMs)) {
                setFeedbackAlarm(monitor, false, output, feedback, wallMs);
            }
        }
    }
}

void DioCollector::setFeedbackAlarm(FeedbackMonitor& monitor, bool active, double output, double feedback,
                                    std::int64_t wallMs) {
    monitor.alarmActive = active;
    monitor.agreeing = false;
    const auto& point = config_.points[monitor.doPoint];
    std::cerr << (active ? "DO feedback mismatch alarm" : "DO feedback mismatch cleared")
              << " doIndex=" << point.index << " pointCode=" << point.pointCode
              << " output=" << output << " feedbackIndex=" << point.write.verifyIndex
              << " feedback=" << feedback << " (no output is re-sent)" << std::endl;
    if (mqttPublisher_) {
        std::ostringstream payload;
        payload << "{\"service\":\"dio-daemon\",\"event\":\""
                << (active ? "do-feedback-mismatch" : "do-feedback-restored") << "\",\"ts\":" << wallMs
                << ",\"meterCode\":\"" << config_.meterCode << "\",\"doIndex\":" << point.index
                << ",\"feedbackIndex\":" << point.write.verifyIndex << ",\"outputValue\":" << output
                << ",\"feedbackValue\":" << feedback << "}";
        try {
            mqttPublisher_->publishStatusMessage(config_.machineCode, payload.str());
        } catch (const std::exception& ex) {
            std::cerr << "DO feedback alarm event publish failed: " << ex.what() << std::endl;
        }
    }
    if (point.write.feedbackAlarmIndex == 0) {
        return;
    }
    for (const auto& candidate : config_.points) {
        if (candidate.index == point.write.feedbackAlarmIndex && candidate.enabled && candidate.read.enable) {
            store_.putLatest(feedbackAlarmValue(candidate, wallMs));
        }
    }
}

PointValue DioCollector::feedbackAlarmValue(const PointDefinition& point, std::int64_t wallMs) const {
    for (const auto& monitor : feedbackMonitors_) {
        const auto& output = config_.points[monitor.doPoint];
        if (output.write.feedbackAlarmIndex != point.index) {
            continue;
        }
        DecodedValue decoded;
        decoded.value = monitor.alarmActive ? 1.0 : 0.0;
        std::ostringstream text;
        if (monitor.lastOutput < 0.0 || monitor.lastFeedback < 0.0) {
            text << "输出状态/触点反馈尚未取得";
        } else {
            text << (monitor.alarmActive ? "触点反馈不一致：" : "一致：") << "DO(index " << output.index
                 << ") 输出状态=" << monitor.lastOutput << "，触点反馈(index " << output.write.verifyIndex
                 << ")=" << monitor.lastFeedback;
        }
        decoded.text = text.str();
        auto value = buildPointValue(point, decoded, wallMs);
        value.quality = 1;
        value.qualityMsg = "ok";
        return value;
    }
    throw std::runtime_error("feedback_alarm point " + point.pointCode + " is not referenced by any DO");
}

}  // namespace edge_gateway
