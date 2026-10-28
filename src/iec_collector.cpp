#include "edge_gateway/iec_collector.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "edge_gateway/iec_codec.hpp"

namespace edge_gateway {

namespace {

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// Propagate the device's own quality descriptor (IV/NT/OV/BL/SB, DPI 0/3) to the published value.
void applyDeviceQuality(PointValue& value, const IecDataValue& dataValue) {
    if (dataValue.quality == 0) {
        value.quality = 0;
        value.qualityMsg = dataValue.qualityMsg.empty() ? "IEC device reported invalid quality" : dataValue.qualityMsg;
    }
}

// Test transmissions (IEC 101/104 COT T=1, IEC 103 COT 7 test mode) never update production points.
std::vector<IecDataValue> withoutTestValues(std::vector<IecDataValue> values) {
    values.erase(
        std::remove_if(values.begin(), values.end(), [](const IecDataValue& value) {
            return value.test;
        }),
        values.end());
    return values;
}

}  // namespace

PointValue IecCollector::buildIecPointValue(
    const PointDefinition& point,
    const IecDataValue& dataValue,
    std::int64_t receivedMs
) const {
    const auto decoded = IecCodec::decodePointValue(point, dataValue);
    // Expiry counts from reception; the device time tag only dates the value.
    auto value = buildPointValue(point, decoded, receivedMs);
    applyDeviceQuality(value, dataValue);
    if (IecCodec::holdsPreviousValue(point, dataValue)) {
        const auto previous = store_.getLatestByIndex(point.index, receivedMs);
        value.value = previous ? previous->value : 0.0;
        value.text = dataValue.qualityMsg;
        value.quality = 0;
    }
    if (dataValue.timestampMs > 0) {
        value.ts = dataValue.timestampMs;
    }
    return value;
}

IecCollector::IecCollector(
    DeviceConfig config,
    MemoryPointStore& store,
    std::shared_ptr<IecClient> client,
    std::shared_ptr<IMqttPublisher> mqttPublisher
) : CollectorBase(std::move(config), store, std::move(mqttPublisher)),
    client_(std::move(client)) {
    if (!client_) {
        throw std::invalid_argument("iecClient is required");
    }
}

CollectCycleResult IecCollector::collectOnce(std::int64_t nowMs, bool realtimeFocused) {
    (void)realtimeFocused;
    observeSchedulingClock(nowMs);
    CollectCycleResult result;
    if (shouldSkipFailedCollectionCycle()) {
        return result;
    }
    auto dataValues = withoutTestValues(client_->drainBufferedValues());
    std::unordered_map<std::uint32_t, bool> updatedByPush;
    for (const auto& dataValue : dataValues) {
        for (const auto& point : config_.points) {
            if (!point.enabled || !point.read.enable || point.read.dataType == "device_online") {
                continue;
            }
            if (!IecCodec::pointMatches(point, dataValue)) {
                continue;
            }
            auto value = buildIecPointValue(point, dataValue, currentTimeMs());
            lastReadMs_[point.index] = nowMs;
            lastValueUpdateMs_[point.index] = nowMs;
            updatedByPush[point.index] = true;
            if (point.read.cachePolicy.storeLatest) {
                store_.putLatest(value);
            }
            result.values.push_back(std::move(value));
        }
    }

    const auto points = duePoints(nowMs);
    if (points.empty()) {
        if (!result.values.empty()) {
            publishDeviceOnlineStatus(true, nowMs);
            recordCollectionCycleSuccess();
        }
        return result;
    }

    bool pollSucceeded = false;
    std::string firstFailureMessage;
    const auto attempts = std::max(1, config_.protocol.transport.readRetryCount + 1);
    for (int attemptIndex = 0; attemptIndex < attempts && !pollSucceeded; ++attemptIndex) {
        try {
            auto polledValues = withoutTestValues(client_->poll());
            dataValues.insert(dataValues.end(), polledValues.begin(), polledValues.end());
            pollSucceeded = true;
        } catch (const std::exception& ex) {
            firstFailureMessage = ex.what();
        }
    }

    bool hasSuccessfulRead = false;
    for (const auto& point : points) {
        if (updatedByPush[point.index]) {
            hasSuccessfulRead = true;
            continue;
        }
        PointValue value;
        const auto pointNowMs = currentTimeMs();
        const auto match = std::find_if(
            dataValues.rbegin(),
            dataValues.rend(),
            [&](const IecDataValue& candidate) {
                return IecCodec::pointMatches(point, candidate);
            }
        );
        if (match != dataValues.rend()) {
            value = buildIecPointValue(point, *match, pointNowMs);
            hasSuccessfulRead = true;
        } else if (pollSucceeded) {
            // IEC polls are incremental: one successful class response may omit
            // unchanged values or values delivered by another data class. Keep
            // their last good samples until normal TTL expiry marks them stale.
            continue;
        } else {
            auto message = std::string("IEC poll failed: ") + firstFailureMessage;
            if (firstFailureMessage.empty()) {
                firstFailureMessage = message;
            }
            value = buildFailedPointValue(point, message, pointNowMs);
        }
        lastReadMs_[point.index] = nowMs;
        lastValueUpdateMs_[point.index] = nowMs;
        if (point.read.cachePolicy.storeLatest) {
            store_.putLatest(value);
        }
        result.values.push_back(std::move(value));
    }

    const bool collectionSucceeded = pollSucceeded || hasSuccessfulRead;
    publishDeviceOnlineStatus(collectionSucceeded, nowMs);
    if (!collectionSucceeded && !firstFailureMessage.empty()) {
        recordCollectionCycleFailure();
        throw std::runtime_error(firstFailureMessage);
    }
    if (collectionSucceeded) {
        recordCollectionCycleSuccess();
    }
    return result;
}

}  // namespace edge_gateway
