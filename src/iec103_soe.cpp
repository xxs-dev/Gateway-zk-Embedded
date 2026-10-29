#include "edge_gateway/iec103_soe.hpp"
#include "edge_gateway/iec_codec.hpp"
#include <cstdlib>
#include <stdexcept>

namespace edge_gateway {
std::vector<Iec103SoeEvent> decodeIec103Soe(const std::vector<std::uint8_t>& data,
    const IecProtocolConfig& config, std::int64_t receivedMs) {
    const auto h = static_cast<std::size_t>(1 + config.linkAddressSize);
    if (data.size() < h + 4 || (data[h] != 1 && data[h] != 2) || data[h + 2] != 1) return {};
    // These time-tagged IEC103 messages carry exactly one information object.
    const auto size = h + 6 + (data[h] == 1 ? 6U : 9U);
    if ((data[h + 1] & 0x7f) != 1 || data.size() != size)
        throw std::runtime_error("IEC103 SOE malformed time-tagged information object");
    Iec103SoeEvent e;
    e.typeId = data[h]; e.cause = data[h + 2]; e.commonAddress = data[h + 3];
    e.functionType = data[h + 4]; e.informationNumber = data[h + 5];
    e.rawDpi = data[h + 6]; e.dpi = e.rawDpi & 3;
    e.valueValid = e.rawDpi == 1 || e.rawDpi == 2;
    e.receivedMs = receivedMs; e.rawHex = IecCodec::toHex(data);
    auto t = h + 7;
    if (e.typeId == 2) {
        e.relativeTimeMs = data[t] | (data[t + 1] << 8);
        e.faultNumber = data[t + 2] | (data[t + 3] << 8);
        t += 4;
    }
    e.rawTimeHex = IecCodec::toHex({data[t], data[t + 1], data[t + 2], data[t + 3]});
    const int ms = data[t] | (data[t + 1] << 8), minute = data[t + 2] & 63, hour = data[t + 3] & 31;
    e.timeValid = receivedMs > 0 && receivedMs < 4102444800000LL && ms < 60000 && minute < 60 && hour < 24
        && !(data[t + 2] & 0xc0) && !(data[t + 3] & 0x60);
    if (e.timeValid) {
        const std::int64_t day = 86400000, offset = static_cast<std::int64_t>(config.soe.utcOffsetMinutes) * 60000;
        const auto local = receivedMs + offset;
        const auto midnight = local - ((local % day + day) % day);
        auto candidate = midnight + hour * 3600000LL + minute * 60000 + ms - offset;
        if (candidate - receivedMs > day / 2) candidate -= day;
        if (receivedMs - candidate > day / 2) candidate += day;
        e.deviceTimeMs = candidate;
    }
    return {e};
}

void describeIec103Soe(Iec103SoeEvent& e, const DeviceConfig& config) {
    e.machineCode = config.machineCode;
    e.deviceCode = config.meterCode;
    e.name = "ASDU " + std::to_string(e.typeId) + " FUN " + std::to_string(e.functionType) + " INF " + std::to_string(e.informationNumber);
    e.state = "DPI=" + std::to_string(e.dpi);
    // Point names may identify a signal, but never prove the polarity of its DPI.
    for (const auto& point : config.points) {
        const auto& spec = point.read.iec;
        if (point.enabled && point.read.enable && spec.functionType == e.functionType &&
            spec.informationNumber == e.informationNumber &&
            (spec.commonAddress <= 0 || spec.commonAddress == e.commonAddress) && !point.name.empty()) {
            e.name = point.name; break;
        }
    }
    for (const auto& m : config.protocol.iec.soe.mappings) {
        if (m.commonAddress == e.commonAddress && m.typeId == e.typeId &&
            m.functionType == e.functionType && m.informationNumber == e.informationNumber) {
            e.name = m.name;
            if (e.valueValid) { e.state = e.dpi == 1 ? m.state1 : m.state2; e.mapped = true; }
            break;
        }
    }
}
} // namespace edge_gateway
