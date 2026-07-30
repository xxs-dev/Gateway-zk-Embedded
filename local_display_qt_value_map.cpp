#include "local_display_qt_value_map.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <limits>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

ScadaValueMap parseScadaValueMapJson(const std::string& json) {
    ScadaValueMap result;
    if (json.empty()) return result;

    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(json), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return result;

    const auto object = document.object();
    for (auto item = object.constBegin(); item != object.constEnd(); ++item) {
        bool numericKey = false;
        const auto value = item.key().toDouble(&numericKey);
        if (!numericKey || !std::isfinite(value) || !item.value().isString()) continue;

        const auto label = item.value().toString().toStdString();
        if (label.empty()) continue;
        const auto duplicate = std::find_if(result.begin(), result.end(), [value](const auto& entry) {
            return std::fabs(entry.first - value) <= 1e-9;
        });
        if (duplicate == result.end()) result.emplace_back(value, label);
    }

    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    return result;
}

bool resolveScadaValueLabel(const ScadaValueMap& valueMap, double value, std::string* label) {
    if (label == nullptr || !std::isfinite(value)) return false;
    for (const auto& entry : valueMap) {
        const auto tolerance = std::max(1e-9, std::fabs(entry.first) * 1e-9);
        if (std::fabs(entry.first - value) <= tolerance) {
            *label = entry.second;
            return true;
        }
    }
    return false;
}

bool matchesScadaCondition(double actual, const std::string& comparison, const std::string& expected) {
    if (!std::isfinite(actual) || expected.empty()) return false;

    char* end = nullptr;
    errno = 0;
    const auto expectedValue = std::strtod(expected.c_str(), &end);
    double parsedExpected = expectedValue;
    if (errno != 0 || end == expected.c_str() || *end != '\0' || !std::isfinite(expectedValue)) {
        if (expected == "true" || expected == "on") parsedExpected = 1.0;
        else if (expected == "false" || expected == "off") parsedExpected = 0.0;
        else return false;
    }

    if (comparison == "bitSet" || comparison == "bitClear" ||
        comparison == "maskAny" || comparison == "maskNone") {
        if (actual < 0.0 || parsedExpected < 0.0 ||
            actual > static_cast<double>(std::numeric_limits<std::int64_t>::max()) ||
            parsedExpected > static_cast<double>(std::numeric_limits<std::int64_t>::max()) ||
            std::fabs(actual - std::round(actual)) > 1e-9) {
            return false;
        }
        const auto raw = static_cast<std::uint64_t>(std::llround(actual));
        if (comparison == "bitSet" || comparison == "bitClear") {
            if (parsedExpected > 63.0 || std::fabs(parsedExpected - std::round(parsedExpected)) > 1e-9) return false;
            const auto set = (raw & (std::uint64_t{1} << static_cast<unsigned>(parsedExpected))) != 0;
            return comparison == "bitSet" ? set : !set;
        }
        if (std::fabs(parsedExpected - std::round(parsedExpected)) > 1e-9) return false;
        const auto mask = static_cast<std::uint64_t>(std::llround(parsedExpected));
        const auto any = (raw & mask) != 0;
        return comparison == "maskAny" ? any : !any;
    }

    if (comparison == "ne") return std::fabs(actual - parsedExpected) > 1e-9;
    if (comparison == "gt") return actual > parsedExpected;
    if (comparison == "gte") return actual >= parsedExpected;
    if (comparison == "lt") return actual < parsedExpected;
    if (comparison == "lte") return actual <= parsedExpected;
    return std::fabs(actual - parsedExpected) <= 1e-9;
}
