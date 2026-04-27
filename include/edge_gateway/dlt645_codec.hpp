#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

class Dlt645Codec {
public:
    static std::vector<std::uint8_t> buildReadFrame(
        const std::string& meterAddress,
        const std::string& dataIdHex
    );

    static DecodedValue decodeReadResponse(
        const std::vector<std::uint8_t>& frame,
        const PointDefinition& point
    );

    static std::string normalizeAddress(const std::string& meterAddress);
    static std::vector<std::uint8_t> parseDataId(const std::string& dataIdHex);
    static std::vector<std::uint8_t> decodeFrameData(
        const std::vector<std::uint8_t>& frame,
        const std::vector<std::uint8_t>& expectedDataId
    );
};

}  // namespace edge_gateway
