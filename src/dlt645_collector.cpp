#include "edge_gateway/collector.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

#include "edge_gateway/dlt645_client.hpp"
#include "edge_gateway/dlt645_codec.hpp"

namespace edge_gateway {

namespace {

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

}  // namespace

Dlt645Collector::Dlt645Collector(
    DeviceConfig config,
    MemoryPointStore& store,
    std::shared_ptr<Dlt645Client> dlt645Client,
    std::shared_ptr<IMqttPublisher> mqttPublisher
) : CollectorBase(std::move(config), store, std::move(mqttPublisher)),
    dlt645Client_(std::move(dlt645Client)) {
    if (!dlt645Client_) {
        throw std::invalid_argument("dlt645Client is required");
    }
}

CollectCycleResult Dlt645Collector::collectOnce(std::int64_t nowMs, bool realtimeFocused) {
    (void)realtimeFocused;
    CollectCycleResult result;
    const auto points = duePoints(nowMs);
    bool hasSuccessfulRead = false;
    std::string firstFailureMessage;
    for (const auto& point : points) {
        PointValue value;
        const auto pointNowMs = currentTimeMs();
        try {
            if (config_.address.empty()) {
                throw std::runtime_error("DLT645 meter address is empty");
            }
            if (point.read.dlt645Di.empty()) {
                throw std::runtime_error("DLT645 point missing read.dlt645.di: " + point.pointCode);
            }
            const auto response = dlt645Client_->readData(config_.address, point.read.dlt645Di);
            const auto decoded = Dlt645Codec::decodeReadResponse(response, point);
            value = buildPointValue(point, decoded, pointNowMs);
            hasSuccessfulRead = true;
        } catch (const std::exception& ex) {
            std::string message = ex.what();
            if (message.find("DLT645 read failed") == std::string::npos) {
                message = std::string("DLT645 read failed address=") + config_.address +
                    " di=" + point.read.dlt645Di +
                    " pointCode=" + point.pointCode +
                    " error=" + ex.what();
            }
            if (firstFailureMessage.empty()) {
                firstFailureMessage = message;
            }
            value = buildFailedPointValue(point, message, currentTimeMs());
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

}  // namespace edge_gateway
