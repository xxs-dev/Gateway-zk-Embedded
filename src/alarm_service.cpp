#include "edge_gateway/alarm_service.hpp"

#include <stdexcept>
#include <utility>

namespace edge_gateway {

namespace {

std::string normalizeType(const std::string& type) {
    if (type == "high" || type == "HIGH") {
        return "high";
    }
    if (type == "low" || type == "LOW") {
        return "low";
    }
    throw std::invalid_argument("unsupported alarm rule type: " + type);
}

}  // namespace

AlarmService::AlarmService(std::vector<DeviceConfig> deviceConfigs) {
    for (const auto& config : deviceConfigs) {
        if (!config.meters.empty()) {
            for (const auto& device : config.meters) {
                appendBindings(config.machineCode, device.meterCode, device.points);
            }
        }
        if (!config.points.empty()) {
            appendBindings(config.machineCode, config.meterCode, config.points);
        }
    }
}

std::vector<AlarmEvent> AlarmService::evaluate(const std::vector<StoredPointValue>& values) {
    std::vector<AlarmEvent> recoveries;
    std::vector<AlarmEvent> activations;

    for (const auto& value : values) {
        const auto bindingIt = bindingsByIndex_.find(value.index);
        if (bindingIt == bindingsByIndex_.end()) {
            continue;
        }

        for (const auto& binding : bindingIt->second) {
            AlarmStateKey key;
            key.index = value.index;
            key.type = binding.rule.type;

            const bool activeNow = isRuleActive(binding.rule, value.value);
            const auto stateIt = activeStates_.find(key);
            const bool wasActive = stateIt != activeStates_.end() && stateIt->second;

            if (wasActive && !activeNow) {
                if (binding.rule.reportRecovery) {
                    AlarmEvent event;
                    event.index = value.index;
                    event.machineCode = binding.machineCode;
                    event.meterCode = binding.meterCode;
                    event.pointCode = binding.pointCode;
                    event.alarmType = binding.rule.type;
                    event.active = false;
                    event.threshold = binding.rule.threshold;
                    event.value = value.value;
                    event.quality = value.quality;
                    event.ts = value.ts;
                    event.stale = value.stale;
                    event.persistValue = binding.rule.persistValue;
                    recoveries.push_back(event);
                }
                activeStates_[key] = false;
                continue;
            }

            if (!wasActive && activeNow) {
                AlarmEvent event;
                event.index = value.index;
                event.machineCode = binding.machineCode;
                event.meterCode = binding.meterCode;
                event.pointCode = binding.pointCode;
                event.alarmType = binding.rule.type;
                event.active = true;
                event.threshold = binding.rule.threshold;
                event.value = value.value;
                event.quality = value.quality;
                event.ts = value.ts;
                event.stale = value.stale;
                event.persistValue = binding.rule.persistValue;
                activations.push_back(event);
                activeStates_[key] = true;
            }
        }
    }

    recoveries.insert(recoveries.end(), activations.begin(), activations.end());
    return recoveries;
}

bool AlarmService::empty() const {
    return bindingsByIndex_.empty();
}

std::size_t AlarmService::AlarmStateKeyHash::operator()(const AlarmStateKey& key) const {
    return (static_cast<std::size_t>(key.index) << 1) ^ std::hash<std::string>()(key.type);
}

void AlarmService::appendBindings(
    const std::string& machineCode,
    const std::string& meterCode,
    const std::vector<PointDefinition>& points
) {
    for (const auto& point : points) {
        for (const auto& alarm : point.alarms) {
            AlarmBinding binding;
            binding.rule = alarm;
            binding.rule.type = normalizeType(alarm.type);
            binding.machineCode = machineCode;
            binding.meterCode = meterCode;
            binding.pointCode = point.pointCode;
            bindingsByIndex_[point.index].push_back(binding);
        }
    }
}

bool AlarmService::isRuleActive(const AlarmRuleConfig& rule, double value) const {
    if (rule.type == "high") {
        return value >= rule.threshold;
    }
    if (rule.type == "low") {
        return value <= rule.threshold;
    }
    return false;
}

}  // namespace edge_gateway
