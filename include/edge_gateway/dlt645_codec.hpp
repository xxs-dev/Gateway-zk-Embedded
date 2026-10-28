#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "edge_gateway/dlt645_version.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {

// DL/T 645 frame: 68 A0..A5 68 C L DATA(+33H) CS 16. A reply carries C|80H; C|A0H announces
// follow-up frames, C|C0H is an exception with a one-byte error word.
//   2007: read 11H (follow-up 12H + frame sequence), write 14H, read address 13H,
//         remote control 1CH (trip/close/alarm/keep power), broadcast time 08H; DI = 4 bytes
//   1997: read 01H (follow-up 02H), write 04H (no operator code), broadcast time 08H; DI = 2 bytes
class Dlt645Codec {
public:
    static constexpr std::uint8_t kBroadcastTime = 0x08;
    static constexpr std::uint8_t kReadAddress = 0x13;
    static constexpr std::uint8_t kRemoteControl = 0x1C;

    static std::uint8_t readControl(Dlt645Version version);
    static std::uint8_t followUpControl(Dlt645Version version);
    static std::uint8_t writeControl(Dlt645Version version);

    static std::vector<std::uint8_t> buildReadFrame(
        const std::string& meterAddress,
        const std::string& dataIdHex,
        Dlt645Version version = Dlt645Version::V2007
    );

    // 2007: DI + frame sequence number; 1997: DI only (sequence ignored).
    static std::vector<std::uint8_t> buildReadFollowUpFrame(
        const std::string& meterAddress,
        const std::string& dataIdHex,
        std::uint8_t sequence,
        Dlt645Version version = Dlt645Version::V2007
    );

    // 2007: DI(4) PA P0 P1 P2 C0..C3 data; 1997: DI(2) PA P0 P1 P2 data (operator code unused).
    static std::vector<std::uint8_t> buildWriteFrame(
        const std::string& meterAddress,
        const std::string& dataIdHex,
        const std::string& passwordHex,
        const std::string& operatorCodeHex,
        const std::vector<std::uint8_t>& payload,
        Dlt645Version version = Dlt645Version::V2007
    );

    // 2007 C=1CH: PA P0 P1 P2, C0..C3, N1 (control type), N2 (00), N3..N8 = validity deadline
    // ss mm hh DD MM YY (local time).
    static std::vector<std::uint8_t> buildRemoteControlFrame(
        const std::string& meterAddress,
        const std::string& passwordHex,
        const std::string& operatorCodeHex,
        std::uint8_t controlTypeCode,
        const std::tm& validUntil
    );

    // Local calendar time of a wall-clock instant (milliseconds since the epoch).
    static std::tm localTime(std::int64_t epochMs);

    // C=08H to the broadcast address 999999999999, data ss mm hh DD MM YY (local time). Both editions.
    static std::vector<std::uint8_t> buildBroadcastTimeFrame(const std::tm& localTime);

    // Address query for a single meter: all-AA pattern -> 2007 C=13H; otherwise (or 1997) a read of
    // the meter's own address/number DI (2007 04000401, 1997 C032) sent to the wildcard pattern.
    static std::vector<std::uint8_t> buildAddressQueryFrame(const std::string& pattern, Dlt645Version version);
    // Pattern of 12 characters A5..A0: decimal digits, optionally with the leading (high) bytes
    // shortened to AA, e.g. AAAAAAAAAAAA or AAAAAAAA0012.
    static std::vector<std::uint8_t> encodeAddressPattern(const std::string& pattern);
    // Address field of a frame as 12 characters A5..A0.
    static std::string frameAddress(const std::vector<std::uint8_t>& frame);

    // N1 of a dlt645_remote_control point (trip 1AH, close_permit 1BH, close 1CH, alarm 2AH,
    // alarm_release 2BH, keep_power 3AH, keep_power_release 3BH); throws for anything else.
    static std::uint8_t remoteControlCode(const std::string& controlType);
    // "ERR=0xNN (meaning)" for an exception reply's error word.
    static std::string describeError(std::uint8_t error, Dlt645Version version);

    static std::vector<std::uint8_t> encodeWritePayload(
        double value,
        const WriteSpec& spec
    );

    // Checks the reply to a command (write / remote control): normal reply C|80H without data, or
    // throws "rejected ERR=..." for the exception reply C|C0H.
    static void validateCommandResponse(
        const std::vector<std::uint8_t>& frame,
        const std::string& meterAddress,
        std::uint8_t requestControl,
        Dlt645Version version = Dlt645Version::V2007
    );

    // Decodes one point from a single complete normal reply frame.
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
    // 8 hex digits DI3..DI0 (2007) or 4 hex digits DI1 DI0 (1997); returned low byte first.
    static std::vector<std::uint8_t> parseDataId(const std::string& dataIdHex);
    // Data of a normal reply after the expected DI (33H removed); throws on an exception reply.
    static std::vector<std::uint8_t> decodeFrameData(
        const std::vector<std::uint8_t>& frame,
        const std::vector<std::uint8_t>& expectedDataId,
        Dlt645Version version = Dlt645Version::V2007
    );

    // Receive-side framing with resynchronisation: drops garbage and truncated frames in front of
    // the first complete frame (both 68H markers, length, checksum and 16H valid) and moves that
    // frame from buffer into frame. Returns false until a complete frame has arrived.
    static bool takeFrame(std::vector<std::uint8_t>& buffer, std::vector<std::uint8_t>& frame);

    // True when the frame's address field equals the (normalized) meter address.
    static bool frameAddressMatches(const std::vector<std::uint8_t>& frame, const std::string& meterAddress);
};

}  // namespace edge_gateway
