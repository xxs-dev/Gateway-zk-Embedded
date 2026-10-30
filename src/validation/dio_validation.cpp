#include "edge_gateway/device_config_validation.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace edge_gateway {

namespace {

[[noreturn]] void reject(const std::string& label, const std::string& field, const std::string& message) {
    throw std::invalid_argument("local_dio " + label + ": " + field + ": " + message);
}

struct GpioUse {
    std::string inputOwner;   // digital_input point
    std::string outputOwner;  // point that drives the pin (write.enable)
    std::string outputReader; // read-only digital_output point
};

// DO action confirmation (write.verifyIndex): a feedback DI defined in this config must be a DI;
// one outside it is read from the shared point store and cannot be checked here.
void validateFeedbackInputs(const DeviceConfig& config) {
    std::map<std::uint32_t, const PointDefinition*> byIndex;
    std::vector<const PointDefinition*> points;
    const auto collect = [&](const std::vector<PointDefinition>& list) {
        for (const auto& point : list) {
            if (point.enabled) {
                byIndex[point.index] = &point;
                points.push_back(&point);
            }
        }
    };
    if (config.meters.empty()) {
        collect(config.points);
    }
    for (const auto& meter : config.meters) {
        if (meter.enabled) {
            collect(meter.points);
        }
    }
    std::map<std::uint32_t, std::string> alarmOwners;  // feedback_alarm index -> DO label
    for (const auto* point : points) {
        const auto& write = point->write;
        const auto label = pointLabel(config, *point);
        if (write.verifyIndex == 0) {
            if (write.verifyValue) {
                configWarning("local_dio " + label + ": write.verifyValue has no effect without write.verifyIndex");
            }
            if (write.feedbackAlarmIndex != 0) {
                reject(label, "write.feedbackAlarmIndex", "needs write.verifyIndex (the contact feedback it supervises)");
            }
            continue;
        }
        if (!write.enable) {
            configWarning("local_dio " + label + ": write.verifyIndex has no effect on a point that is not written");
            continue;
        }
        if (write.verifyIndex == point->index) {
            reject(label, "write.verifyIndex", "must name a feedback DI, not the DO itself");
        }
        if (write.verifyValue && *write.verifyValue != 0.0 && *write.verifyValue != 1.0) {
            reject(label, "write.verifyValue", "must be 0 or 1");
        }
        if (write.verifyTimeoutMs <= 0 || write.verifyTimeoutMs > 60000) {
            reject(label, "write.verifyTimeoutMs", "must be 1..60000");
        }
        if (write.feedbackAlarmDelayMs < 0 || write.feedbackAlarmDelayMs > 600000) {
            reject(label, "write.feedbackAlarmDelayMs", "must be 0..600000");
        }
        if (write.feedbackRecoveryMs < 0 || write.feedbackRecoveryMs > 600000) {
            reject(label, "write.feedbackRecoveryMs", "must be 0..600000");
        }
        if (write.feedbackAlarmIndex != 0) {
            const auto alarm = byIndex.find(write.feedbackAlarmIndex);
            if (alarm == byIndex.end() || !alarm->second->read.enable ||
                alarm->second->read.dataType != "feedback_alarm") {
                reject(label, "write.feedbackAlarmIndex", "index " + std::to_string(write.feedbackAlarmIndex) +
                       " must be an enabled read.dataType=feedback_alarm point of this config");
            }
            if (!alarmOwners.emplace(write.feedbackAlarmIndex, label).second) {
                reject(label, "write.feedbackAlarmIndex", "already used by " + alarmOwners[write.feedbackAlarmIndex]);
            }
        } else {
            configWarning("local_dio " + label + ": contact feedback mismatch is supervised but only logged; set "
                          "write.feedbackAlarmIndex to a feedback_alarm point to raise an alarm");
        }
        const auto feedback = byIndex.find(write.verifyIndex);
        if (feedback == byIndex.end()) {
            continue;
        }
        const auto& input = *feedback->second;
        if (!input.read.enable || input.read.dataType != "digital_input") {
            reject(label, "write.verifyIndex", "index " + std::to_string(write.verifyIndex) +
                   " in this config is not an enabled digital_input");
        }
        if (input.read.debounceMs >= write.verifyTimeoutMs) {
            reject(label, "write.verifyTimeoutMs", "must be longer than the feedback DI's debounceMs (" +
                   std::to_string(input.read.debounceMs) + ")");
        }
    }
    for (const auto* point : points) {
        if (point->read.enable && point->read.dataType == "feedback_alarm" &&
            alarmOwners.find(point->index) == alarmOwners.end()) {
            reject(pointLabel(config, *point), "read.dataType",
                   "feedback_alarm point is not the write.feedbackAlarmIndex of any DO");
        }
    }
}

}  // namespace

void validateDioDeviceConfig(const DeviceConfig& config) {
    std::map<int, GpioUse> gpios;
    const auto validatePoints = [&](const std::vector<PointDefinition>& points) {
        for (const auto& point : points) {
            if (!point.enabled) {
                continue;
            }
            const auto label = pointLabel(config, point);
            const auto gpio = point.read.gpio;
            const bool input = point.read.dataType == "digital_input";
            if (point.read.dataType == "feedback_alarm") {
                // Alarm state of a DO's contact feedback supervision, published by the collector.
                if (gpio >= 0 || point.write.enable) {
                    reject(label, "read.dataType", "a feedback_alarm point has no gpio and cannot be written");
                }
                continue;
            }
            if (point.read.enable) {
                if (!input && point.read.dataType != "digital_output") {
                    reject(label, "read.dataType", "must be digital_input, digital_output or feedback_alarm, got \"" +
                           point.read.dataType + "\"");
                }
                if (gpio < 0) {
                    reject(label, "read.gpio", "is required");
                }
                if (point.read.debounceMs < 0) {
                    reject(label, "read.debounceMs", "must not be negative");
                }
            }
            if (point.write.enable) {
                if (input) {
                    reject(label, "write.enable", "a digital_input cannot be written");
                }
                if (point.write.dataType != "digital_output") {
                    reject(label, "write.dataType", "must be digital_output, got \"" + point.write.dataType + "\"");
                }
                if (gpio < 0) {
                    reject(label, "read.gpio", "is required for a DO");
                }
                if (point.write.startupValue && *point.write.startupValue != 0.0 && *point.write.startupValue != 1.0) {
                    reject(label, "write.startupValue", "must be 0 or 1");
                }
            }
            if (gpio < 0 || (!point.read.enable && !point.write.enable)) {
                continue;
            }
            auto& use = gpios[gpio];
            const auto gpioText = "gpio " + std::to_string(gpio);
            if (input) {
                if (!use.outputOwner.empty() || !use.outputReader.empty()) {
                    reject(label, "read.gpio", gpioText + " is also a DO of " +
                           (use.outputOwner.empty() ? use.outputReader : use.outputOwner));
                }
                if (!use.inputOwner.empty()) {
                    configWarning("local_dio " + label + ": " + gpioText + " is also read by " + use.inputOwner);
                }
                use.inputOwner = label;
                continue;
            }
            if (!use.inputOwner.empty()) {
                reject(label, "read.gpio", gpioText + " is also a DI of " + use.inputOwner);
            }
            if (point.write.enable) {
                if (!use.outputOwner.empty()) {
                    reject(label, "read.gpio", gpioText + " is already driven by " + use.outputOwner);
                }
                use.outputOwner = label;
            } else {
                use.outputReader = label;
            }
        }
    };
    if (config.meters.empty()) {
        validatePoints(config.points);
    }
    for (const auto& meter : config.meters) {
        if (meter.enabled) {
            validatePoints(meter.points);
        }
    }
    validateFeedbackInputs(config);
}

}  // namespace edge_gateway
