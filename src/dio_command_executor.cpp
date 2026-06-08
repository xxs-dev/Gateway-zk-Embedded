#include "edge_gateway/dio_command_executor.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <thread>
#include <utility>

namespace edge_gateway {

DioCommandExecutor::DioCommandExecutor(
    DeviceConfig config,
    MemoryPointStore& store,
    std::shared_ptr<IGpioPort> gpioPort
) : config_(std::move(config)),
    store_(store),
    gpioPort_(std::move(gpioPort)) {
    if (!gpioPort_) {
        throw std::invalid_argument("gpioPort is required");
    }
    store_.registerPoints(config_.machineCode, config_.meterCode, config_.points);
}

CommandResult DioCommandExecutor::executeByIndex(
    const std::string& cmdId,
    std::uint32_t index,
    double value,
    std::int64_t nowMs
) const {
    const auto& point = findPointByIndex(index);
    CommandResult result;
    result.cmdId = cmdId;
    result.machineCode = config_.machineCode;
    result.meterCode = config_.meterCode;
    result.pointCode = point.pointCode;
    result.index = point.index;
    result.ts = nowMs;
    result.requestedValue = value;
    result.verifyAttempted = point.write.verifyAfterWrite && point.write.verifyByRead;
    try {
        dispatchLocalDioWrite(point, value, nowMs);
        result.verifyPassed = result.verifyAttempted;
        result.success = true;
        result.message = "ok";
    } catch (const std::exception& ex) {
        result.success = false;
        result.message = ex.what();
    }
    return result;
}

const PointDefinition& DioCommandExecutor::findPointByIndex(std::uint32_t index) const {
    for (const auto& point : config_.points) {
        if (point.index == index) {
            return point;
        }
    }
    throw std::invalid_argument("point index not found: " + std::to_string(index));
}

void DioCommandExecutor::dispatchLocalDioWrite(
    const PointDefinition& point,
    double value,
    std::int64_t nowMs
) const {
    if (!point.write.enable) {
        throw std::invalid_argument("point write is disabled");
    }
    if (point.read.dataType != "digital_output" && point.write.dataType != "digital_output") {
        throw std::invalid_argument("local_dio write requires digital_output point");
    }
    if (point.read.gpio < 0) {
        throw std::invalid_argument("local_dio point missing read.gpio");
    }
    if (std::abs(value - 0.0) > 1e-9 && std::abs(value - 1.0) > 1e-9) {
        throw std::invalid_argument("local_dio write value must be 0 or 1");
    }
    if (point.write.minValue && value < *point.write.minValue) {
        throw std::invalid_argument("value below min");
    }
    if (point.write.maxValue && value > *point.write.maxValue) {
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

    const auto logicalHigh = value > 0.0;
    const auto gpioHigh = point.read.activeHigh ? logicalHigh : !logicalHigh;
    gpioPort_->exportGpio(point.read.gpio);
    gpioPort_->setDirection(point.read.gpio, "out");
    gpioPort_->writeValue(point.read.gpio, gpioHigh);

    if (point.write.verifyAfterWrite && point.write.verifyByRead) {
        if (point.write.verifyDelayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(point.write.verifyDelayMs));
        }
        const auto actualGpioHigh = gpioPort_->readValue(point.read.gpio);
        const auto actualValue = actualGpioHigh == point.read.activeHigh ? 1.0 : 0.0;
        if (std::abs(actualValue - value) > 1e-9) {
            throw std::runtime_error("verify failed");
        }
    }

    PointValue latest;
    latest.index = point.index;
    latest.machineCode = config_.machineCode;
    latest.meterCode = config_.meterCode;
    latest.pointCode = point.pointCode;
    latest.pointName = point.name;
    latest.category = point.category;
    latest.unit = point.read.unit;
    latest.value = value;
    latest.text = value > 0.0 ? "1" : "0";
    latest.rawHex = gpioHigh ? "01" : "00";
    latest.quality = 1;
    latest.qualityMsg = "ok";
    latest.ts = nowMs;
    latest.expireAt = nowMs + point.read.cachePolicy.ttlMs;
    latest.function = 0;
    latest.address = point.address;
    latest.length = 1;
    latest.isStore = point.isStore;
    latest.persistIntervalSec = point.persistIntervalSec;
    store_.putLatest(latest);
}

}  // namespace edge_gateway
