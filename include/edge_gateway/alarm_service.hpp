#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

class AlarmService {
public:
    explicit AlarmService(std::vector<DeviceConfig> deviceConfigs);

    std::vector<AlarmEvent> evaluate(const std::vector<StoredPointValue>& values);
    bool empty() const;

private:
    struct AlarmBinding {
        AlarmRuleConfig rule;
        std::string machineCode;
        std::string meterCode;
        std::string pointCode;
    };

    struct AlarmStateKey {
        std::uint32_t index = 0;
        std::string type;

        bool operator==(const AlarmStateKey& other) const {
            return index == other.index && type == other.type;
        }
    };

    struct AlarmStateKeyHash {
        std::size_t operator()(const AlarmStateKey& key) const;
    };

    void appendBindings(
        const std::string& machineCode,
        const std::string& meterCode,
        const std::vector<PointDefinition>& points
    );

    bool isRuleActive(const AlarmRuleConfig& rule, double value) const;

    std::unordered_map<std::uint32_t, std::vector<AlarmBinding> > bindingsByIndex_;
    std::unordered_map<AlarmStateKey, bool, AlarmStateKeyHash> activeStates_;
};

}  // namespace edge_gateway
