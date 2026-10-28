#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

struct CanEncodedFrame {
    std::uint32_t frameId = 0;
    bool extended = false;
    bool remoteRequest = false;
    bool fd = false;
    std::vector<std::uint8_t> payload;
};

// decode() found raw bits listed in read.can.invalidRawValues ("not available").
class CanInvalidRawValue : public std::runtime_error {
public:
    explicit CanInvalidRawValue(const std::string& message) : std::runtime_error(message) {}
};

// Signal layout (one definition, see doc/协议与驱动/CAN接口接入设计.md):
//   endian "little" (Intel):  LSB at byte byteOffset, bit bitOffset counted from the byte's LSB;
//                             DBC Intel start bit S  -> byteOffset = S / 8, bitOffset = S % 8.
//   endian "big" (Motorola):  MSB at byte byteOffset, bit bitOffset counted from the byte's MSB
//                             (bitOffset 0 = bit 7); DBC Motorola start bit S (the MSB) ->
//                             byteOffset = S / 8, bitOffset = 7 - S % 8.
// bitOffset = 0 with a multiple of 8 bits is the byte-aligned signal in both orders.
class CanSignalCodec {
public:
    // "0x" + 1..8 hex digits; standard frames <= 0x7FF, extended frames <= 0x1FFFFFFF.
    static std::uint32_t parseFrameId(const std::string& value, bool extended);
    // Online frame list entries carry no frame type: any "0x" ID up to 0x1FFFFFFF.
    static std::uint32_t parseAnyFrameId(const std::string& value);
    static std::uint64_t frameKey(std::uint32_t frameId, bool extended) {
        return (static_cast<std::uint64_t>(frameId) << 1) | (extended ? 1U : 0U);
    }
    // bool, int8/uint8, int16/uint16, int32/uint32, int64/uint64, float32.
    static bool isSignalDataType(const std::string& dataType);
    static int signalBitLength(const CanSignalSpec& can, const std::string& dataType);
    // Payload a write frame starts from: payloadTemplate (exactly dlc bytes of hex) or dlc zero bytes.
    static std::vector<std::uint8_t> templatePayload(const CanSignalSpec& can);
    static bool isValidFdLength(int length);

    static DecodedValue decode(const std::vector<std::uint8_t>& payload, const ReadSpec& spec);
    // Overwrites only the signal's bits of payload (which must already have the frame length).
    // signedSignal: two's complement signal even for uintN types (read.signed=true).
    static void encodeInto(std::vector<std::uint8_t>& payload, double value, const WriteSpec& spec,
                           bool signedSignal = false);
    // Frame starting from basePayload when it has the frame length, else from templatePayload().
    static CanEncodedFrame encode(
        double value,
        const WriteSpec& spec,
        const std::vector<std::uint8_t>& basePayload = std::vector<std::uint8_t>(),
        bool signedSignal = false
    );
};

}  // namespace edge_gateway
