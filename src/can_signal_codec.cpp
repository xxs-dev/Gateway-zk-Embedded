#include "edge_gateway/can_signal_codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace edge_gateway {

namespace {

constexpr std::uint32_t kMaxStandardFrameId = 0x7FFU;
constexpr std::uint32_t kMaxExtendedFrameId = 0x1FFFFFFFU;

int defaultBitLength(const std::string& type) {
    if (type == "bool") return 1;
    if (type == "uint8" || type == "int8") return 8;
    if (type == "uint16" || type == "int16") return 16;
    if (type == "uint32" || type == "int32" || type == "float32") return 32;
    if (type == "uint64" || type == "int64") return 64;
    return 0;
}

bool isSignedType(const std::string& type) {
    return type == "int8" || type == "int16" || type == "int32" || type == "int64";
}

std::string toHex(std::uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << value;
    return out.str();
}

// Validates the layout against a payload of payloadBytes and returns the first bit in reading order.
int checkedStartBit(const CanSignalSpec& can, const std::string& dataType, int bitLength, std::size_t payloadBytes,
                    bool payloadFromWire) {
    if (!CanSignalCodec::isSignalDataType(dataType)) {
        throw std::invalid_argument("unsupported CAN dataType: " + dataType);
    }
    if (can.endian != "little" && can.endian != "big") {
        throw std::invalid_argument("CAN endian must be little or big: " + can.endian);
    }
    if (can.bitOrder != "lsb0") {
        throw std::invalid_argument("CAN bitOrder must be lsb0: " + can.bitOrder);
    }
    if (can.byteOffset < 0 || can.bitOffset < 0 || can.bitOffset > 7 || bitLength <= 0 || bitLength > 64) {
        throw std::invalid_argument("invalid CAN signal bit range");
    }
    if (dataType == "float32" && bitLength != 32) {
        throw std::invalid_argument("float32 CAN signal requires 32 bits");
    }
    const auto startBit = can.byteOffset * 8 + can.bitOffset;
    if (startBit + bitLength > static_cast<int>(payloadBytes * 8)) {
        if (payloadFromWire) {
            throw std::runtime_error("CAN payload shorter than signal range (" + std::to_string(payloadBytes) + " bytes)");
        }
        throw std::invalid_argument("CAN signal exceeds the frame dlc");
    }
    return startBit;
}

// Byte and bit (0 = LSB of the byte) of the i-th signal bit in reading order, and the value bit it holds.
struct BitPosition {
    int byteIndex;
    int bitInByte;
    int valueBit;
};

BitPosition bitPosition(bool bigEndian, int startBit, int bitLength, int i) {
    const auto position = startBit + i;
    if (bigEndian) {
        // Motorola: MSB first, walking from bit 7 down to bit 0 and on to bit 7 of the next byte.
        return BitPosition{position / 8, 7 - position % 8, bitLength - 1 - i};
    }
    // Intel: LSB first, walking from bit 0 up to bit 7 and on to bit 0 of the next byte.
    return BitPosition{position / 8, position % 8, i};
}

std::uint64_t extractBits(const std::vector<std::uint8_t>& payload, const CanSignalSpec& can, int startBit,
                          int bitLength) {
    const bool bigEndian = can.endian == "big";
    std::uint64_t value = 0;
    for (int i = 0; i < bitLength; ++i) {
        const auto at = bitPosition(bigEndian, startBit, bitLength, i);
        if ((payload[static_cast<std::size_t>(at.byteIndex)] >> at.bitInByte) & 0x01U) {
            value |= std::uint64_t{1} << at.valueBit;
        }
    }
    return value;
}

void writeBits(std::vector<std::uint8_t>& payload, const CanSignalSpec& can, int startBit, int bitLength,
               std::uint64_t value) {
    const bool bigEndian = can.endian == "big";
    for (int i = 0; i < bitLength; ++i) {
        const auto at = bitPosition(bigEndian, startBit, bitLength, i);
        auto& byte = payload[static_cast<std::size_t>(at.byteIndex)];
        const auto mask = static_cast<std::uint8_t>(1U << at.bitInByte);
        if ((value >> at.valueBit) & 0x01U) {
            byte = static_cast<std::uint8_t>(byte | mask);
        } else {
            byte = static_cast<std::uint8_t>(byte & static_cast<std::uint8_t>(~mask));
        }
    }
}

std::int64_t signExtend(std::uint64_t value, int bitLength) {
    if (bitLength <= 0 || bitLength >= 64) {
        return static_cast<std::int64_t>(value);
    }
    const auto signBit = std::uint64_t{1} << (bitLength - 1);
    if ((value & signBit) == 0) {
        return static_cast<std::int64_t>(value);
    }
    return static_cast<std::int64_t>(value | ((~std::uint64_t{0}) << bitLength));
}

std::string selectedRawHex(const std::vector<std::uint8_t>& payload, const CanSignalSpec& can, int bitLength) {
    const auto byteCount = static_cast<std::size_t>((can.bitOffset + bitLength + 7) / 8);
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < byteCount; ++i) {
        const auto index = static_cast<std::size_t>(can.byteOffset) + i;
        if (index >= payload.size()) {
            break;
        }
        out << std::setw(2) << static_cast<int>(payload[index]);
    }
    return out.str();
}

std::uint64_t toRawInteger(double value, double scale, double offset, bool signedSignal, int bitLength) {
    if (std::abs(scale) < 1e-12) {
        throw std::invalid_argument("CAN write scale must not be zero");
    }
    const auto unrounded = (value - offset) / scale;
    if (!std::isfinite(unrounded)) {
        throw std::invalid_argument("CAN write value is not finite");
    }
    const auto raw = std::round(unrounded);
    const auto minValue = signedSignal ? -std::ldexp(1.0, bitLength - 1) : 0.0;
    const auto maxExclusive = std::ldexp(1.0, signedSignal ? bitLength - 1 : bitLength);
    if ((!signedSignal && unrounded < 0.0) || raw < minValue || raw >= maxExclusive) {
        throw std::out_of_range("CAN write value out of range for " + std::to_string(bitLength) + "-bit signal");
    }
    return raw < 0.0 ? static_cast<std::uint64_t>(static_cast<std::int64_t>(raw))
                     : static_cast<std::uint64_t>(raw);
}

std::uint32_t parseHexFrameId(const std::string& value) {
    if (value.size() < 3 || value.size() > 10 || value[0] != '0' || (value[1] != 'x' && value[1] != 'X') ||
        value.find_first_not_of("0123456789abcdefABCDEF", 2) != std::string::npos) {
        throw std::invalid_argument("CAN frameId must be hex with a 0x prefix (e.g. 0x18FF50E5): \"" + value + "\"");
    }
    const auto parsed = std::stoul(value.substr(2), nullptr, 16);
    if (parsed > kMaxExtendedFrameId) {
        throw std::invalid_argument("CAN frameId above 0x1FFFFFFF: " + value);
    }
    return static_cast<std::uint32_t>(parsed);
}

}  // namespace

std::uint32_t CanSignalCodec::parseFrameId(const std::string& value, bool extended) {
    const auto parsed = parseHexFrameId(value);
    if (!extended && parsed > kMaxStandardFrameId) {
        throw std::invalid_argument("standard (11-bit) CAN frameId above 0x7FF: " + value +
                                    " (set extended=true for a 29-bit ID)");
    }
    return parsed;
}

std::uint32_t CanSignalCodec::parseAnyFrameId(const std::string& value) {
    return parseHexFrameId(value);
}

bool CanSignalCodec::isSignalDataType(const std::string& dataType) {
    return defaultBitLength(dataType) > 0;
}

int CanSignalCodec::signalBitLength(const CanSignalSpec& can, const std::string& dataType) {
    return can.bitLength > 0 ? can.bitLength : defaultBitLength(dataType);
}

bool CanSignalCodec::isValidFdLength(int length) {
    return (length >= 0 && length <= 8) || length == 12 || length == 16 || length == 20 || length == 24 ||
        length == 32 || length == 48 || length == 64;
}

std::vector<std::uint8_t> CanSignalCodec::templatePayload(const CanSignalSpec& can) {
    if (can.dlc < 0 || can.dlc > 64) {
        throw std::invalid_argument("CAN dlc must be between 0 and 64");
    }
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(can.dlc), 0);
    if (can.payloadTemplate.empty()) {
        return payload;
    }
    if (can.payloadTemplate.size() != payload.size() * 2 ||
        can.payloadTemplate.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        throw std::invalid_argument("CAN payloadTemplate must be exactly " + std::to_string(can.dlc) +
                                    " bytes of hex without separators");
    }
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>(std::stoul(can.payloadTemplate.substr(i * 2, 2), nullptr, 16));
    }
    return payload;
}

DecodedValue CanSignalCodec::decode(const std::vector<std::uint8_t>& payload, const ReadSpec& spec) {
    const auto bitLength = signalBitLength(spec.can, spec.dataType);
    const auto startBit = checkedStartBit(spec.can, spec.dataType, bitLength, payload.size(), true);
    const auto raw = extractBits(payload, spec.can, startBit, bitLength);
    if (std::find(spec.can.invalidRawValues.begin(), spec.can.invalidRawValues.end(), raw) !=
        spec.can.invalidRawValues.end()) {
        throw CanInvalidRawValue("CAN raw value " + toHex(raw) + " is marked invalid");
    }

    DecodedValue result;
    result.rawHex = selectedRawHex(payload, spec.can, bitLength);
    const auto& type = spec.dataType;
    if (type == "bool") {
        result.value = raw == 0 ? 0.0 : 1.0;
        result.text = result.value > 0.0 ? "1" : "0";
        return result;
    }
    if (type == "float32") {
        const auto raw32 = static_cast<std::uint32_t>(raw);
        float value = 0.0F;
        std::memcpy(&value, &raw32, sizeof(value));
        if (!std::isfinite(value)) {
            throw std::runtime_error("CAN float32 signal is not finite (NaN/Inf)");
        }
        result.value = static_cast<double>(value) * spec.scale + spec.offset;
    } else if (isSignedType(type) || spec.signedFlag) {
        result.value = static_cast<double>(signExtend(raw, bitLength)) * spec.scale + spec.offset;
    } else {
        result.value = static_cast<double>(raw) * spec.scale + spec.offset;
    }
    if (!std::isfinite(result.value)) {
        throw std::runtime_error("CAN decoded signal value is not finite");
    }
    result.text = std::to_string(result.value);
    return result;
}

void CanSignalCodec::encodeInto(std::vector<std::uint8_t>& payload, double value, const WriteSpec& spec,
                                bool signedSignal) {
    if (!std::isfinite(value) || !std::isfinite(spec.scale) || !std::isfinite(spec.offset)) {
        throw std::invalid_argument("CAN write value or scaling is not finite");
    }
    const auto bitLength = signalBitLength(spec.can, spec.dataType);
    const auto startBit = checkedStartBit(spec.can, spec.dataType, bitLength, payload.size(), false);
    const auto& type = spec.dataType;
    if (type == "float32") {
        if (std::abs(spec.scale) < 1e-12) {
            throw std::invalid_argument("CAN write scale must not be zero");
        }
        const auto scaled = static_cast<float>((value - spec.offset) / spec.scale);
        if (!std::isfinite(scaled)) {
            throw std::invalid_argument("CAN float32 write value is not finite");
        }
        std::uint32_t raw32 = 0;
        std::memcpy(&raw32, &scaled, sizeof(raw32));
        writeBits(payload, spec.can, startBit, bitLength, raw32);
    } else if (type == "bool") {
        writeBits(payload, spec.can, startBit, bitLength, value == 0.0 ? 0U : 1U);
    } else {
        writeBits(payload, spec.can, startBit, bitLength,
                  toRawInteger(value, spec.scale, spec.offset, signedSignal || isSignedType(type), bitLength));
    }
}

CanEncodedFrame CanSignalCodec::encode(
    double value,
    const WriteSpec& spec,
    const std::vector<std::uint8_t>& basePayload,
    bool signedSignal
) {
    CanEncodedFrame frame;
    frame.frameId = parseFrameId(spec.can.frameId, spec.can.extended);
    frame.extended = spec.can.extended;
    frame.remoteRequest = spec.can.remoteRequest;
    frame.fd = spec.can.fd;
    frame.payload = basePayload.size() == static_cast<std::size_t>(std::max(0, spec.can.dlc))
        ? basePayload
        : templatePayload(spec.can);
    encodeInto(frame.payload, value, spec, signedSignal);
    return frame;
}

}  // namespace edge_gateway
