#include "edge_gateway/iec_collector.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

#include "edge_gateway/iec_codec.hpp"

namespace edge_gateway {

namespace {

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

}  // namespace

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
    CollectCycleResult result;
    const auto points = duePoints(nowMs);
    if (points.empty()) {
        return result;
    }

    std::vector<IecDataValue> dataValues;
    bool pollSucceeded = false;
    std::string firstFailureMessage;
    try {
        dataValues = client_->poll();
        pollSucceeded = true;
    } catch (const std::exception& ex) {
        firstFailureMessage = ex.what();
    }

    bool hasSuccessfulRead = false;
    for (const auto& point : points) {
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
            const auto decoded = IecCodec::decodePointValue(point, *match);
            value = buildPointValue(point, decoded, pointNowMs);
            hasSuccessfulRead = true;
        } else {
            auto message = pollSucceeded
                ? std::string("IEC point not found in poll response: ") + point.pointCode
                : std::string("IEC poll failed: ") + firstFailureMessage;
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

    publishDeviceOnlineStatus(hasSuccessfulRead, nowMs);
    if (!hasSuccessfulRead && !firstFailureMessage.empty()) {
        throw std::runtime_error(firstFailureMessage);
    }
    return result;
}

}  // namespace edge_gateway
