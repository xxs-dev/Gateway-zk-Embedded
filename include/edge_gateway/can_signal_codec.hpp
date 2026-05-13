#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

struct CanEncodedFrame {
    std::uint32_t frameId = 0;
    bool extended = false;
    bool remoteRequest = false;
    std::vector<std::uint8_t> payload;
};

class CanSignalCodec {
public:
    static std::uint32_t parseFrameId(const std::string& value);
    static bool frameMatches(std::uint32_t frameId, bool extended, const CanSignalSpec& spec);
    static DecodedValue decode(const std::vector<std::uint8_t>& payload, const ReadSpec& spec);
    static CanEncodedFrame encode(double value, const WriteSpec& spec);
};

}  // namespace edge_gateway
