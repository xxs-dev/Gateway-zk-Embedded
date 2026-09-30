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
    // Same as encode(), but starts from basePayload (when it has the frame's
    // DLC) so the other signals multiplexed in the same frame are preserved.
    // signedSignal: the signal is two's complement (e.g. read.signed=true on a
    // uint16 signal), so the accepted raw range is [-2^(n-1), 2^(n-1)-1].
    static CanEncodedFrame encode(
        double value,
        const WriteSpec& spec,
        const std::vector<std::uint8_t>& basePayload,
        bool signedSignal = false
    );
};

}  // namespace edge_gateway
