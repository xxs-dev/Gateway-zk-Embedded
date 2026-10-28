#include "edge_gateway/device_config_validation.hpp"

#include <map>
#include <stdexcept>
#include <string>

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
            if (point.read.enable) {
                if (!input && point.read.dataType != "digital_output") {
                    reject(label, "read.dataType", "must be digital_input or digital_output, got \"" +
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
}

}  // namespace edge_gateway
