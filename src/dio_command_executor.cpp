#include "edge_gateway/dio_command_executor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace edge_gateway {

bool isDioOutputPoint(const PointDefinition& point) {
    return point.write.enable && point.write.dataType == "digital_output";
}

double dioExpectedFeedback(const PointDefinition& point, double outputState) {
    const bool active = outputState > 0.5;
    if (!point.write.verifyValue) {
        return active ? 1.0 : 0.0;
    }
    const bool activeFeedback = *point.write.verifyValue > 0.5;
    return (active == activeFeedback) ? 1.0 : 0.0;
}

bool dioStartupGpioHigh(const PointDefinition& point) {
    const auto logicalHigh = point.write.startupValue && *point.write.startupValue > 0.5;
    return point.read.activeHigh ? logicalHigh : !logicalHigh;
}

std::vector<std::string> applyDioStartupOutputs(const DeviceConfig& config, IGpioPort& gpioPort) {
    std::vector<const PointDefinition*> points;
    if (config.meters.empty()) {
        for (const auto& point : config.points) points.push_back(&point);
    } else {
        for (const auto& meter : config.meters) {
            if (!meter.enabled) continue;
            for (const auto& point : meter.points) points.push_back(&point);
        }
    }
    std::vector<std::string> failures;
    for (const auto* point : points) {
        if (!point->enabled || !isDioOutputPoint(*point) || point->read.gpio < 0) {
            continue;
        }
        try {
            // Power-on (pin not yet an output): drive the startup state. A driver restart finds the
            // pin already an output and keeps its level, so a relay held by EMS does not drop.
            const auto high = dioStartupGpioHigh(*point);
            if (gpioPort.ensureOutput(point->read.gpio, high)) {
                gpioPort.writeValue(point->read.gpio, high);
            }
        } catch (const std::exception& ex) {
            failures.push_back(point->pointCode + ": " + ex.what());
            std::cerr << "DO startup state failed pointCode=" << point->pointCode
                      << " gpio=" << point->read.gpio << " error=" << ex.what() << std::endl;
        }
    }
    return failures;
}

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

namespace {

std::string logical(double value) {
    return value > 0.5 ? "1" : "0";
}

std::int64_t wallClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

// Wording of results: a DO write sets the GPIO output latch, reported as "输出状态". Reading it back
// proves the latch only, never that the relay contact moved; only a feedback DI (auxiliary contact,
// write.verifyIndex) confirms the action and is reported as "触点反馈". A DO without one says
// "未配置反馈". A latch or contact showing another value is a feedback mismatch (feedbackMismatch):
// the output was written, so it is not reported as "not executed" and must not be re-sent.
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
    try {
        const auto gpioHigh = driveOutput(point, value);
        const auto sentAtWallMs = wallClockMs();
        std::string outputState = "输出状态已置为 " + logical(value) + "（未回读）";
        if (point.write.verifyAfterWrite && point.write.verifyByRead) {
            result.verifyAttempted = true;
            if (point.write.verifyDelayMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(point.write.verifyDelayMs));
            }
            const auto latchValue = gpioPort_->readValue(point.read.gpio) == point.read.activeHigh ? 1.0 : 0.0;
            if (std::abs(latchValue - value) > 1e-9) {
                result.feedbackMismatch = true;
                result.message = "输出状态回读不一致：已写入 " + logical(value) + "，GPIO 输出锁存回读 " +
                    logical(latchValue) + "；输出写入已执行，不得自动重发";
                return result;
            }
            outputState = "输出状态回读=" + logical(value) + "（GPIO 输出锁存）";
        }
        publishOutputState(point, value, gpioHigh, nowMs);
        if (point.write.verifyIndex != 0) {
            confirmByFeedback(point, value, sentAtWallMs, result);
            if (result.success) {
                result.message = "ok：" + outputState + "；" + result.message;
            }
            return result;
        }
        result.success = true;
        result.verifyPassed = result.verifyAttempted;
        result.message = "ok：" + outputState + "；未配置反馈，继电器触点是否动作未确认";
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

bool DioCommandExecutor::driveOutput(const PointDefinition& point, double value) const {
    if (!point.write.enable) {
        throw std::invalid_argument("point write is disabled");
    }
    if (!isDioOutputPoint(point) || point.read.dataType == "digital_input") {
        throw std::invalid_argument("local_dio write requires a digital_output point");
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
    gpioPort_->setOutputDirection(point.read.gpio, gpioHigh);
    return gpioHigh;
}

void DioCommandExecutor::publishOutputState(const PointDefinition& point, double value, bool gpioHigh,
                                            std::int64_t nowMs) const {
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
    latest.persistOnChange = point.persistOnChange;
    latest.persistIntervalSec = point.persistIntervalSec;
    store_.putLatest(latest);
}

void DioCommandExecutor::confirmByFeedback(const PointDefinition& point, double written,
                                           std::int64_t sentAtWallMs, CommandResult& result) const {
    result.verifyAttempted = true;
    const auto expected = dioExpectedFeedback(point, written);
    const auto timeoutMs = std::max(1, point.write.verifyTimeoutMs);
    const auto label = "触点反馈(index " + std::to_string(point.write.verifyIndex) + ")";
    const PointDefinition* local = nullptr;
    for (const auto& candidate : config_.points) {
        if (candidate.index == point.write.verifyIndex && candidate.enabled && candidate.read.enable &&
            candidate.read.dataType == "digital_input" && candidate.read.gpio >= 0) {
            local = &candidate;
        }
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    bool haveValue = false;
    double lastValue = 0.0;
    std::string lastError = "no value";
    if (local != nullptr) {
        // Same device: read the auxiliary contact itself, accepted once it is stable for debounceMs.
        double candidateValue = -1.0;
        auto candidateSince = std::chrono::steady_clock::now();
        while (true) {
            const auto now = std::chrono::steady_clock::now();
            try {
                const auto value = gpioPort_->readValue(local->read.gpio) == local->read.activeHigh ? 1.0 : 0.0;
                if (value != candidateValue) {
                    candidateValue = value;
                    candidateSince = now;
                }
                if (now - candidateSince >= std::chrono::milliseconds(std::max(0, local->read.debounceMs))) {
                    haveValue = true;
                    lastValue = candidateValue;
                    if (std::abs(lastValue - expected) <= 1e-6) {
                        break;
                    }
                }
            } catch (const std::exception& ex) {
                lastError = ex.what();
            }
            if (now >= deadline) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!haveValue && candidateValue >= 0.0) {
            lastError = "input not stable for " + std::to_string(local->read.debounceMs) + " ms";
        }
    } else {
        // Another device: only a good value its collector stored after the output was set counts.
        while (true) {
            const auto stored = store_.getLatestByIndex(point.write.verifyIndex, wallClockMs());
            if (!stored) {
                lastError = "no value in the point store";
            } else if (stored->stale || stored->quality != 1) {
                lastError = "stale or bad quality";
            } else if (stored->ts > sentAtWallMs) {
                haveValue = true;
                lastValue = stored->value;
                if (std::abs(lastValue - expected) <= 1e-6) {
                    break;
                }
            } else {
                lastError = "no value collected since the output was set";
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    if (haveValue && std::abs(lastValue - expected) <= 1e-6) {
        result.success = true;
        result.verifyPassed = true;
        result.message = label + "=" + logical(lastValue) + "，动作已确认";
        return;
    }
    result.success = false;
    result.verifyPassed = false;
    if (!haveValue) {
        // The output is set; whether the equipment moved is not known.
        result.outcomeUnknown = true;
        result.message = "输出状态已置为 " + logical(written) + "；" + label + " 在 " + std::to_string(timeoutMs) +
            " ms 内没有新值（" + lastError + "），动作未确认，结果未知";
        return;
    }
    // The output was set and the contact shows the other state: the equipment may or may not have
    // acted (stuck contact, wiring). Not a failure to execute; never re-sent automatically.
    result.feedbackMismatch = true;
    result.message = "反馈不一致：输出状态已置为 " + logical(written) + "，" + label + "=" + logical(lastValue) +
        "，期望 " + logical(expected) + "（" + std::to_string(timeoutMs) + " ms 内）；输出写入已执行，不得自动重发";
}

}  // namespace edge_gateway
