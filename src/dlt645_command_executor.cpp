#include "edge_gateway/dlt645_command_executor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#include "edge_gateway/dlt645_codec.hpp"
#include "edge_gateway/dlt645_point_rules.hpp"

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
        const auto* readBack = readBackPoint(point);
        const auto expected = readBack != nullptr && point.write.dlt645.verifyValue
            ? *point.write.dlt645.verifyValue
            : value;
        if (readBack != nullptr) {
            double current = 0.0;
            try {
                current = readValue(*readBack);
            } catch (const std::exception& ex) {
                throw std::runtime_error(
                    "DLT645 pre-check failed, command not sent: cannot read " + readBack->pointCode + ": " + ex.what()
                );
            }
            if (std::abs(current - expected) <= 1e-6) {
                result.success = true;
                result.verifyAttempted = true;
                result.verifyPassed = true;
                result.message = "ok; " + readBack->pointCode + " already shows the target value, DLT645 command not sent";
                return result;
            }
        }
        client_->writeData(
            config_.address,
            point.write.dlt645.di,
            config_.protocol.dlt645.write.password,
            config_.protocol.dlt645.write.operatorCode,
            payload
        );
        if (readBack == nullptr) {
            result.success = true;
            result.message = "ok; DLT645 command accepted, physical state not verified";
            return result;
        }
        if (isDlt645SwitchingControl(point.write) && value > 0.0) {
            result.success = true;
            result.message = "ok; DLT645 delayed command accepted, " + readBack->pointCode +
                " changes after the delay and is not verified";
            return result;
        }
        confirmByReadBack(point, *readBack, expected, result);
    } catch (const Dlt645OutcomeUnknownError& ex) {
        result.success = false;
        result.outcomeUnknown = true;
        result.message = ex.what();
    } catch (const std::exception& ex) {
        result.success = false;
        result.message = ex.what();
    }
    return result;
}

const PointDefinition* Dlt645CommandExecutor::readBackPoint(const PointDefinition& point) const {
    // Trip/close points cannot load without a read-back (validateDlt645PointList).
    if (!point.write.verifyAfterWrite || !point.write.verifyByRead) {
        return nullptr;
    }
    for (const auto& candidate : config_.points) {
        if (candidate.pointCode == point.write.dlt645.verifyPointCode && candidate.enabled &&
            candidate.read.enable && !candidate.read.dlt645Di.empty()) {
            return &candidate;
        }
    }
    throw std::invalid_argument(
        "DLT645 read-back point '" + point.write.dlt645.verifyPointCode + "' not found; command not sent"
    );
}

double Dlt645CommandExecutor::readValue(const PointDefinition& point) const {
    const auto data = client_->readData(config_.address, point.read.dlt645Di);
    return Dlt645Codec::decodePayload(data, point).value;
}

void Dlt645CommandExecutor::confirmByReadBack(
    const PointDefinition& point,
    const PointDefinition& readBack,
    double expected,
    CommandResult& result
) const {
    result.verifyAttempted = true;
    const auto delayMs = std::max(0, point.write.verifyDelayMs);
    const auto pollMs = std::max(100, delayMs);
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(std::max(delayMs, point.write.dlt645.verifyTimeoutMs));
    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    bool haveValue = false;
    double lastValue = 0.0;
    std::string lastError;
    while (true) {
        try {
            lastValue = readValue(readBack);
            haveValue = true;
            if (std::abs(lastValue - expected) <= 1e-6) {
                result.success = true;
                result.verifyPassed = true;
                result.message = "ok; DLT645 command accepted and confirmed by " + readBack.pointCode;
                return;
            }
        } catch (const std::exception& ex) {
            lastError = ex.what();
        }
        if (std::chrono::steady_clock::now() + std::chrono::milliseconds(pollMs) > deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
    }
    result.success = false;
    result.verifyPassed = false;
    std::ostringstream message;
    if (!haveValue) {
        result.outcomeUnknown = true;
        message << "DLT645 command accepted (94H) but " << readBack.pointCode
                << " could not be read back: " << lastError << "; outcome unknown";
    } else {
        message << "DLT645 command accepted (94H) but " << readBack.pointCode << "=" << lastValue
                << ", expected " << expected << " within " << point.write.dlt645.verifyTimeoutMs << " ms";
    }
    result.message = message.str();
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
