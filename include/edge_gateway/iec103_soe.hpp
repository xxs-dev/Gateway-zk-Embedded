#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "edge_gateway/models.hpp"

namespace edge_gateway {

struct Iec103SoeEvent {
    std::int64_t id = 0;
    std::string eventId, machineCode, deviceCode, name, state, rawHex, rawTimeHex;
    std::int64_t receivedMs = 0, deviceTimeMs = 0;
    int commonAddress = 0, typeId = 0, cause = 0, functionType = 0, informationNumber = 0;
    int dpi = 0, rawDpi = 0, relativeTimeMs = -1, faultNumber = -1;
    bool valueValid = false, timeValid = false, mapped = false;
};

// IEC103 CP32 contains no calendar date. deviceTimeMs is a nearest-day estimate,
// never a claim of synchronized device time; the original four bytes are retained.
std::vector<Iec103SoeEvent> decodeIec103Soe(const std::vector<std::uint8_t>& userData,
    const IecProtocolConfig& config, std::int64_t receivedMs);
void describeIec103Soe(Iec103SoeEvent& event, const DeviceConfig& config);

} // namespace edge_gateway
