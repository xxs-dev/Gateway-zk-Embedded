#include "edge_gateway/dlt645_command_executor.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "edge_gateway/dlt645_codec.hpp"

namespace edge_gateway {

Dlt645CommandExecutor::Dlt645CommandExecutor(
    DeviceConfig config,
    MemoryPointStore& store,
    std::shared_ptr<Dlt645Client> client
) : config_(std::move(config)),
    store_(store),
    client_(std::move(client)) {
    if (!client_) {
        throw std::invalid_argument("dlt645 client is required");
    }
    store_.registerPoints(config_.machineCode, config_.meterCode, config_.points);
}

CommandResult Dlt645CommandExecutor::executeByIndex(
    const std::string& cmdId,
    std::uint32_t index,
    double value,
    std::int64_t nowMs
) const {
    CommandResult result;
    result.cmdId = cmdId;
    result.machineCode = config_.machineCode;
    result.meterCode = config_.meterCode;
    result.index = index;
    result.ts = nowMs;
    result.requestedValue = value;

    try {
        const auto& point = findPointByIndex(index);
        result.pointCode = point.pointCode;
        validateWrite(point, value);
        const auto payload = Dlt645Codec::encodeWritePayload(value, point.write);
        client_->writeData(
            config_.address,
            point.write.dlt645.di,
            config_.protocol.dlt645.write.password,
            config_.protocol.dlt645.write.operatorCode,
            payload
        );
        result.success = true;
        result.message = "ok; DLT645 command accepted, physical state not verified";
    } catch (const std::exception& ex) {
        result.success = false;
        result.message = ex.what();
    }
    return result;
}

const PointDefinition& Dlt645CommandExecutor::findPointByIndex(std::uint32_t index) const {
    for (const auto& point : config_.points) {
        if (point.index == index) {
            return point;
        }
    }
    throw std::invalid_argument("point index not found: " + std::to_string(index));
}

void Dlt645CommandExecutor::validateWrite(const PointDefinition& point, double value) const {
    if (config_.protocol.type != "dlt645_2007") {
        throw std::invalid_argument("DLT645 command executor requires protocol.type=dlt645_2007");
    }
    if (!config_.protocol.dlt645.write.enabled) {
        throw std::invalid_argument("DLT645 protocol write is disabled");
    }
    if (config_.protocol.dlt645.write.password.empty()) {
        throw std::invalid_argument("DLT645 protocol write password is not configured");
    }
    if (config_.address.empty()) {
        throw std::invalid_argument("DLT645 meter address is empty");
    }
    if (!point.enabled) {
        throw std::invalid_argument("point is disabled");
    }
    if (!point.write.enable) {
        throw std::invalid_argument("point write is disabled");
    }
    if (point.write.dlt645.di.empty()) {
        throw std::invalid_argument("DLT645 writable point missing write.dlt645.di");
    }
    if (point.write.minValue && value < *point.write.minValue - 1e-9) {
        throw std::invalid_argument("value below min");
    }
    if (point.write.maxValue && value > *point.write.maxValue + 1e-9) {
        throw std::invalid_argument("value above max");
    }
    if (!point.write.allowedValues.empty()) {
        const auto match = std::find_if(
            point.write.allowedValues.begin(),
            point.write.allowedValues.end(),
            [value](double candidate) {
                return std::abs(candidate - value) <= 1e-9;
            }
        );
        if (match == point.write.allowedValues.end()) {
            throw std::invalid_argument("value is not in allowedValues");
        }
    }
    if (point.write.step > 0.0) {
        const auto base = point.write.minValue ? *point.write.minValue : 0.0;
        const auto steps = (value - base) / point.write.step;
        if (std::abs(steps - std::round(steps)) > 1e-9) {
            throw std::invalid_argument("value does not match configured step");
        }
    }
}

}  // namespace edge_gateway
