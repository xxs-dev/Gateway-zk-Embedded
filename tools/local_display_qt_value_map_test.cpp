#include <iostream>
#include <stdexcept>
#include <string>

#include "local_display_qt_value_map.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void verifyChineseLabelsAndNumericKeys() {
    const auto valueMap = parseScadaValueMapJson(
        R"({"0":"\u5c31\u5730","1.0":"\u8fdc\u7a0b"})"
    );
    require(valueMap.size() == 2, "two numeric value mappings should parse");
    std::string label;
    require(resolveScadaValueLabel(valueMap, 0.0, &label) && label == "\xE5\xB0\xB1\xE5\x9C\xB0", "zero label mismatch");
    require(resolveScadaValueLabel(valueMap, 1.0, &label) && label == "\xE8\xBF\x9C\xE7\xA8\x8B", "one label mismatch");
}

void verifyInvalidAndUnknownValuesFallBack() {
    require(parseScadaValueMapJson("not-json").empty(), "invalid JSON should be ignored");
    require(parseScadaValueMapJson(R"(["0","1"])").empty(), "non-object JSON should be ignored");

    const auto valueMap = parseScadaValueMapJson(R"({"0":"off","1":"on","bad":"ignored","2":2})");
    require(valueMap.size() == 2, "invalid entries should be ignored");
    std::string label;
    require(!resolveScadaValueLabel(valueMap, 2.0, &label), "unknown values should use numeric fallback");
}

void verifyAlarmWordConditions() {
    require(matchesScadaCondition(0.0, "maskNone", "3"), "zero alarm word should match maskNone");
    require(!matchesScadaCondition(0.0, "maskAny", "3"), "zero alarm word must not trigger maskAny");
    require(matchesScadaCondition(2.0, "maskAny", "3"), "set bit in mask should trigger maskAny");
    require(matchesScadaCondition(0x4000, "bitSet", "14"), "bitSet should read the selected bit");
    require(matchesScadaCondition(0x4000, "bitClear", "13"), "bitClear should read the selected bit");
    require(!matchesScadaCondition(0x4000, "bitClear", "14"), "set bit must not match bitClear");
    require(!matchesScadaCondition(1.5, "maskAny", "1"), "bit comparisons require an integral source value");
    require(!matchesScadaCondition(1.0, "bitSet", "64"), "bit index above 63 must be rejected");
}

}  // namespace

int main() {
    try {
        verifyChineseLabelsAndNumericKeys();
        verifyInvalidAndUnknownValuesFallBack();
        verifyAlarmWordConditions();
        std::cout << "local_display_qt_value_map_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "local_display_qt_value_map_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
