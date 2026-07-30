#pragma once

#include <string>
#include <utility>
#include <vector>

using ScadaValueMap = std::vector<std::pair<double, std::string>>;

ScadaValueMap parseScadaValueMapJson(const std::string& json);
bool resolveScadaValueLabel(const ScadaValueMap& valueMap, double value, std::string* label);
bool matchesScadaCondition(double actual, const std::string& comparison, const std::string& expected);
