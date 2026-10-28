#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

// DL/T 645-2007 frame: 68 A0..A5 68 C L DATA(+33H) CS 16. Reads use C=11H (reply 91H, B1H when
// follow-up frames exist, D1H on error); follow-up reads use C=12H with a frame sequence number
// (reply 92H/B2H/D2H); writes use C=14H (reply 94H, D4H on error).
class Dlt645Codec {
public:
    static std::vector<std::uint8_t> buildReadFrame(
        const std::string& meterAddress,
        const std::string& dataIdHex
    );

    static std::vector<std::uint8_t> buildReadFollowUpFrame(
        const std::string& meterAddress,
        const std::string& dataIdHex,
        std::uint8_t sequence
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

    // Decodes one point from a single complete 91H reply frame.
    static DecodedValue decodeReadResponse(
        const std::vector<std::uint8_t>& frame,
        const PointDefinition& point
    );

    // Decodes one point from the data of its DI (bytes after the DI, 33H already removed, follow-up
    // frames appended). read.dlt645.offset/byteCount select the point's bytes inside a block DI.
    static DecodedValue decodePayload(
        const std::vector<std::uint8_t>& data,
        const PointDefinition& point
    );

    // 1..12 decimal digits, left-padded to 12; anything else is rejected.
    static std::string normalizeAddress(const std::string& meterAddress);
    static std::vector<std::uint8_t> parseDataId(const std::string& dataIdHex);
    static std::vector<std::uint8_t> decodeFrameData(
        const std::vector<std::uint8_t>& frame,
        const std::vector<std::uint8_t>& expectedDataId
    );

    // Receive-side framing with resynchronisation: drops garbage and truncated frames in front of
    // the first complete frame (both 68H markers, length, checksum and 16H valid) and moves that
    // frame from buffer into frame. Returns false until a complete frame has arrived.
    static bool takeFrame(std::vector<std::uint8_t>& buffer, std::vector<std::uint8_t>& frame);

    // True when the frame's address field equals the (normalized) meter address.
    static bool frameAddressMatches(const std::vector<std::uint8_t>& frame, const std::string& meterAddress);
};

}  // namespace edge_gateway
