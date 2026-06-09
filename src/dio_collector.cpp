#include "edge_gateway/dio_collector.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace edge_gateway {

namespace {

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
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
    CollectCycleResult result;
    const auto points = duePoints(nowMs);
    bool hasSuccessfulRead = false;
    std::string firstFailureMessage;
    for (const auto& point : points) {
        PointValue value;
        const auto pointNowMs = currentTimeMs();
        try {
            value = collectLocalDioPoint(point, pointNowMs);
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
    if (mqttPublisher_ && !result.values.empty()) {
        mqttPublisher_->publishTelemetry(config_.machineCode, result.values);
    }
    if (!points.empty() && !hasSuccessfulRead && !firstFailureMessage.empty()) {
        throw std::runtime_error(firstFailureMessage);
    }
    return result;
}

PointValue DioCollector::collectLocalDioPoint(const PointDefinition& point, std::int64_t nowMs) {
    if (point.read.gpio < 0) {
        throw std::runtime_error("local_dio point missing read.gpio: " + point.pointCode);
    }
    if (point.read.dataType != "digital_input" && point.read.dataType != "digital_output") {
        throw std::runtime_error("unsupported local_dio read.dataType: " + point.read.dataType);
    }
    gpioPort_->exportGpio(point.read.gpio);
    if (point.read.dataType == "digital_input") {
        gpioPort_->setDirection(point.read.gpio, "in");
    }
    const auto gpioHigh = gpioPort_->readValue(point.read.gpio);
    auto logicalValue = gpioHigh == point.read.activeHigh ? 1.0 : 0.0;
    if (point.read.debounceMs > 0) {
        const auto raw = lastDioRawValues_.find(point.index);
        if (raw == lastDioRawValues_.end()) {
            lastDioRawValues_[point.index] = logicalValue;
            lastDioStableValues_[point.index] = logicalValue;
            lastDioRawChangeMs_[point.index] = nowMs;
        } else if (raw->second != logicalValue) {
            lastDioRawValues_[point.index] = logicalValue;
            lastDioRawChangeMs_[point.index] = nowMs;
        }

        const auto stable = lastDioStableValues_.find(point.index);
        const auto changedAt = lastDioRawChangeMs_.find(point.index);
        if (stable != lastDioStableValues_.end() && stable->second != logicalValue &&
            changedAt != lastDioRawChangeMs_.end() && nowMs - changedAt->second >= point.read.debounceMs) {
            lastDioStableValues_[point.index] = logicalValue;
        }
        logicalValue = lastDioStableValues_[point.index];
    }

    DecodedValue decoded;
    decoded.value = logicalValue;
    decoded.text = logicalValue > 0.0 ? "1" : "0";
    decoded.rawHex = gpioHigh ? "01" : "00";
    return buildPointValue(point, decoded, nowMs);
}

}  // namespace edge_gateway
