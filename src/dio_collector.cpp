#include "edge_gateway/dio_collector.hpp"

#include <chrono>
#include <iostream>
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
            value = collectLocalDioPoint(point, pointNowMs, steadyTimeMs());
            hasSuccessfulRead = true;
        } catch (const std::exception& ex) {
            if (firstFailureMessage.empty()) {
                firstFailureMessage = ex.what();
            }
            value = buildFailedPointValue(point, ex.what(), currentTimeMs());
        }
        lastReadMs_[point.index] = nowMs;
        lastValueUpdateMs_[point.index] = nowMs;
        if (point.read.cachePolicy.storeLatest) {
            store_.putLatest(value);
        }
        result.values.push_back(std::move(value));
    }
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

}  // namespace edge_gateway
