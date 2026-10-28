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
        const bool readBack = hasReadBack(point);
        const auto label = readBack ? readBackLabel(point) : std::string();
        const auto expected = readBack && point.write.dlt645.verifyValue
            ? *point.write.dlt645.verifyValue
            : value;
        if (readBack) {
            double current = 0.0;
            try {
                current = readBackValue(point, 0).value;
            } catch (const std::exception& ex) {
                throw std::runtime_error(
                    "DLT645 pre-check failed, command not sent: cannot read " + label + ": " + ex.what()
                );
            }
            if (std::abs(current - expected) <= 1e-6) {
                result.success = true;
                result.verifyAttempted = true;
                result.verifyPassed = true;
                result.message = "ok; " + label + " already shows the target value, DLT645 command not sent";
                return result;
            }
        }
        const auto sentAtMs = wallNowMs();
        const bool remoteControl = dlt645WriteDataType(point.write) == "dlt645_remote_control";
        if (remoteControl) {
            // C=1CH: the command stays valid until now + validitySeconds (meter's local time).
            client_->remoteControl(
                config_.address,
                config_.protocol.dlt645.write.password,
                config_.protocol.dlt645.write.operatorCode,
                Dlt645Codec::remoteControlCode(point.write.dlt645.controlType),
                Dlt645Codec::localTime(sentAtMs + static_cast<std::int64_t>(point.write.dlt645.validitySeconds) * 1000)
            );
        } else {
            client_->writeData(
                config_.address,
                point.write.dlt645.di,
                config_.protocol.dlt645.write.password,
                config_.protocol.dlt645.write.operatorCode,
                payload
            );
        }
        if (!readBack) {
            result.success = true;
            result.message = "ok; DLT645 command accepted, physical state not verified";
            return result;
        }
        if (dlt645WriteDataType(point.write) == "dlt645_scheduled_control" && value > 0.0) {
            result.success = true;
            result.message = "ok; DLT645 delayed command accepted, " + label +
                " changes after the delay and is not verified";
            return result;
        }
        confirmByReadBack(point, label, expected, sentAtMs, result);
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

std::int64_t Dlt645CommandExecutor::wallNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool Dlt645CommandExecutor::hasReadBack(const PointDefinition& point) {
    // Trip/close points cannot load without a read-back (validateDlt645PointList).
    return point.write.verifyAfterWrite && point.write.verifyByRead;
}

std::string Dlt645CommandExecutor::readBackLabel(const PointDefinition& point) {
    if (point.write.dlt645.verifyIndex != 0) {
        return "index " + std::to_string(point.write.dlt645.verifyIndex);
    }
    return point.write.dlt645.verifyPointCode;
}

const PointDefinition& Dlt645CommandExecutor::localReadBackPoint(const PointDefinition& point) const {
    for (const auto& candidate : config_.points) {
        if (candidate.pointCode == point.write.dlt645.verifyPointCode && candidate.enabled &&
            candidate.read.enable && !candidate.read.dlt645Di.empty()) {
            return candidate;
        }
    }
    throw std::invalid_argument(
        "DLT645 read-back point '" + point.write.dlt645.verifyPointCode + "' not found; command not sent"
    );
}

Dlt645CommandExecutor::ReadBackSample Dlt645CommandExecutor::readBackValue(
    const PointDefinition& point,
    std::int64_t notBeforeMs
) const {
    ReadBackSample sample;
    const auto storeIndex = point.write.dlt645.verifyIndex;
    if (storeIndex == 0) {
        // Same meter: read the DI from the meter itself.
        const auto& readBack = localReadBackPoint(point);
        const auto data = client_->readData(config_.address, readBack.read.dlt645Di);
        sample.value = Dlt645Codec::decodePayload(data, readBack).value;
        sample.fresh = true;
        return sample;
    }
    // Another device on this gateway (e.g. a DIO auxiliary contact): its collector publishes the
    // value to the shared point store. Only a good, non-stale value counts.
    const auto stored = store_.getLatestByIndex(storeIndex, wallNowMs());
    if (!stored) {
        throw std::runtime_error("point index " + std::to_string(storeIndex) + " has no value in the point store");
    }
    if (stored->stale || stored->quality != 1) {
        throw std::runtime_error("point index " + std::to_string(storeIndex) + " is stale or bad quality");
    }
    sample.value = stored->value;
    // Strictly after: a sample from the millisecond the command left was taken before it acted.
    sample.fresh = notBeforeMs <= 0 || stored->ts > notBeforeMs;
    return sample;
}

void Dlt645CommandExecutor::confirmByReadBack(
    const PointDefinition& point,
    const std::string& label,
    double expected,
    std::int64_t sentAtMs,
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
            // Store-backed read-backs only count values collected after the command was sent.
            const auto sample = readBackValue(point, sentAtMs);
            if (sample.fresh) {
                lastValue = sample.value;
                haveValue = true;
                if (std::abs(lastValue - expected) <= 1e-6) {
                    result.success = true;
                    result.verifyPassed = true;
                    result.message = "ok; DLT645 command accepted and confirmed by " + label;
                    return;
                }
            } else {
                lastError = "no value collected since the command was sent";
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
        message << "DLT645 command acknowledged but " << label
                << " could not be read back: " << lastError << "; outcome unknown";
    } else {
        message << "DLT645 command acknowledged but " << label << "=" << lastValue
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
    if (!isDlt645Protocol(config_.protocol.type)) {
        throw std::invalid_argument("DLT645 command executor requires protocol.type dlt645_2007 or dlt645_1997");
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
    if (point.write.dlt645.di.empty() && dlt645WriteDataType(point.write) != "dlt645_remote_control") {
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
