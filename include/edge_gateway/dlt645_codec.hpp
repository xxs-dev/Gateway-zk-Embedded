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

    static std::vector<std::uint8_t> buildWriteFrame(
        const std::string& meterAddress,
        const std::string& dataIdHex,
        const std::string& passwordHex,
        const std::string& operatorCodeHex,
        const std::vector<std::uint8_t>& payload
    );

    static std::vector<std::uint8_t> encodeWritePayload(
        double value,
        const WriteSpec& spec
    );

    static void validateWriteResponse(
        const std::vector<std::uint8_t>& frame,
        const std::string& meterAddress
    );

    static DecodedValue decodeReadResponse(
        const std::vector<std::uint8_t>& frame,
        const PointDefinition& point
    );

    // Rejects a read response that is not a C=91H/B1H/D1H reply from the
    // addressed meter (e.g. a late answer from another meter on the bus).
    static void validateReadResponseAddress(
        const std::vector<std::uint8_t>& frame,
        const std::string& meterAddress
    );

    static std::string normalizeAddress(const std::string& meterAddress);
    static std::vector<std::uint8_t> parseDataId(const std::string& dataIdHex);
    static std::vector<std::uint8_t> decodeFrameData(
        const std::vector<std::uint8_t>& frame,
        const std::vector<std::uint8_t>& expectedDataId
    );
};

}  // namespace edge_gateway
