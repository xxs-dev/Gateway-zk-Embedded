#include "edge_gateway/can_signal_codec.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace edge_gateway {

namespace {

std::string normalizeType(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

int defaultBitLength(const std::string& dataType) {
    const auto type = normalizeType(dataType);
    if (type == "bool" || type == "bit") {
        return 1;
    }
    if (type == "uint8" || type == "int8" || type == "byte") {
        return 8;
    }
    if (type == "uint16" || type == "int16") {
        return 16;
    }
    if (type == "uint32" || type == "int32" || type == "float32" || type == "float") {
        return 32;
    }
    if (type == "uint64" || type == "int64" || type == "double") {
        return 64;
    }
    return 16;
}

int signalBitLength(const CanSignalSpec& can, const std::string& dataType) {
    return can.bitLength > 0 ? can.bitLength : defaultBitLength(dataType);
}

void validateSignalBounds(const std::vector<std::uint8_t>& payload, const CanSignalSpec& can, int bitLength) {
    if (can.byteOffset < 0 || can.bitOffset < 0 || can.bitOffset > 7 || bitLength <= 0 || bitLength > 64) {
        throw std::invalid_argument("invalid CAN signal bit range");
    }
    const auto startBit = can.byteOffset * 8 + can.bitOffset;
    const auto endBit = startBit + bitLength;
    if (endBit > static_cast<int>(payload.size() * 8)) {
        throw std::runtime_error("CAN payload shorter than signal range");
    }
}

int physicalBitIndex(const CanSignalSpec& can, int signalBit) {
    const auto bitOrder = normalizeType(can.bitOrder);
    const auto baseBit = can.byteOffset * 8 + can.bitOffset;
    if (bitOrder == "msb0") {
        const auto logicalBit = baseBit + signalBit;
        const auto byteIndex = logicalBit / 8;
        const auto bitInByte = logicalBit % 8;
        return byteIndex * 8 + (7 - bitInByte);
    }
    return baseBit + signalBit;
}

std::uint64_t extractUnsignedBits(const std::vector<std::uint8_t>& payload, const CanSignalSpec& can, int bitLength) {
    const auto endian = normalizeType(can.endian);
    const bool bigEndian = endian == "big" || endian == "be";
    if (can.bitOffset == 0 && bitLength % 8 == 0 && normalizeType(can.bitOrder) == "lsb0") {
        std::uint64_t value = 0;
        const auto byteCount = bitLength / 8;
        for (int i = 0; i < byteCount; ++i) {
            const auto byte = payload[static_cast<std::size_t>(can.byteOffset + i)];
            if (bigEndian) {
                value = (value << 8) | byte;
            } else {
                value |= (static_cast<std::uint64_t>(byte) << (8 * i));
            }
        }
        return value;
    }

    std::uint64_t value = 0;
    for (int i = 0; i < bitLength; ++i) {
        const auto targetBit = bigEndian ? (bitLength - 1 - i) : i;
        const auto physical = physicalBitIndex(can, i);
        const auto byteIndex = physical / 8;
        const auto bitIndex = physical % 8;
        if ((payload[byteIndex] >> bitIndex) & 0x01U) {
            value |= (std::uint64_t{1} << targetBit);
        }
    }
    return value;
}

void writeUnsignedBits(
    std::vector<std::uint8_t>& payload,
    const CanSignalSpec& can,
    int bitLength,
    std::uint64_t value
) {
    const auto endian = normalizeType(can.endian);
    const bool bigEndian = endian == "big" || endian == "be";
    if (can.bitOffset == 0 && bitLength % 8 == 0 && normalizeType(can.bitOrder) == "lsb0") {
        const auto byteCount = bitLength / 8;
        for (int i = 0; i < byteCount; ++i) {
            const auto shift = bigEndian ? 8 * (byteCount - 1 - i) : 8 * i;
            payload[static_cast<std::size_t>(can.byteOffset + i)] =
                static_cast<std::uint8_t>((value >> shift) & 0xFFU);
        }
        return;
    }

    for (int i = 0; i < bitLength; ++i) {
        const auto sourceBit = bigEndian ? (bitLength - 1 - i) : i;
        const auto physical = physicalBitIndex(can, i);
        const auto byteIndex = physical / 8;
        const auto bitIndex = physical % 8;
        const auto mask = static_cast<std::uint8_t>(1U << bitIndex);
        if ((value >> sourceBit) & 0x01U) {
            payload[byteIndex] = static_cast<std::uint8_t>(payload[byteIndex] | mask);
        } else {
            payload[byteIndex] = static_cast<std::uint8_t>(payload[byteIndex] & static_cast<std::uint8_t>(~mask));
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
    const auto mask = (~std::uint64_t{0}) << bitLength;
    return static_cast<std::int64_t>(value | mask);
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

double applyScale(double raw, double scale, double offset) {
    return raw * scale + offset;
}

std::uint64_t toRawInteger(double value, double scale, double offset) {
    if (std::abs(scale) < 1e-12) {
        throw std::invalid_argument("CAN write scale must not be zero");
    }
    return static_cast<std::uint64_t>(std::llround((value - offset) / scale));
}

}  // namespace

std::uint32_t CanSignalCodec::parseFrameId(const std::string& value) {
    if (value.empty()) {
        throw std::invalid_argument("CAN frameId is required");
    }
    std::size_t processed = 0;
    const auto base = value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X') ? 16 : 10;
    const auto parsed = std::stoul(value, &processed, base);
    if (processed != value.size()) {
        throw std::invalid_argument("invalid CAN frameId: " + value);
    }
    if (parsed > 0x1FFFFFFFU) {
        throw std::invalid_argument("CAN frameId out of range: " + value);
    }
    return static_cast<std::uint32_t>(parsed);
}

bool CanSignalCodec::frameMatches(std::uint32_t frameId, bool extended, const CanSignalSpec& spec) {
    if (spec.frameId.empty()) {
        return false;
    }
    return frameId == parseFrameId(spec.frameId) && extended == spec.extended;
}

DecodedValue CanSignalCodec::decode(const std::vector<std::uint8_t>& payload, const ReadSpec& spec) {
    const auto bitLength = signalBitLength(spec.can, spec.dataType);
    validateSignalBounds(payload, spec.can, bitLength);

    DecodedValue result;
    result.rawHex = selectedRawHex(payload, spec.can, bitLength);

    const auto type = normalizeType(spec.dataType);
    const auto raw = extractUnsignedBits(payload, spec.can, bitLength);
    if (type == "bool" || type == "bit") {
        result.value = raw == 0 ? 0.0 : 1.0;
        result.text = result.value > 0.0 ? "1" : "0";
        return result;
    }
    if (type == "float32" || type == "float") {
        if (bitLength != 32) {
            throw std::invalid_argument("float32 CAN signal requires 32 bits");
        }
        const auto raw32 = static_cast<std::uint32_t>(raw);
        float value = 0.0F;
        std::memcpy(&value, &raw32, sizeof(value));
        result.value = applyScale(static_cast<double>(value), spec.scale, spec.offset);
    } else if (type == "int8" || type == "int16" || type == "int32" || type == "int64" || spec.signedFlag) {
        result.value = applyScale(static_cast<double>(signExtend(raw, bitLength)), spec.scale, spec.offset);
    } else {
        result.value = applyScale(static_cast<double>(raw), spec.scale, spec.offset);
    }
    result.text = std::to_string(result.value);
    return result;
}

CanEncodedFrame CanSignalCodec::encode(double value, const WriteSpec& spec) {
    CanEncodedFrame frame;
    frame.frameId = parseFrameId(spec.can.frameId);
    frame.extended = spec.can.extended;
    frame.remoteRequest = spec.can.remoteRequest;
    const auto dlc = spec.can.dlc > 0 ? spec.can.dlc : 8;
    if (dlc > 64) {
        throw std::invalid_argument("CAN dlc must be <= 64");
    }
    frame.payload.assign(static_cast<std::size_t>(dlc), 0);

    const auto bitLength = signalBitLength(spec.can, spec.dataType);
    validateSignalBounds(frame.payload, spec.can, bitLength);
    const auto type = normalizeType(spec.dataType);
    if (type == "float32" || type == "float") {
        if (bitLength != 32) {
            throw std::invalid_argument("float32 CAN write requires 32 bits");
        }
        const auto scaled = static_cast<float>((value - spec.offset) / spec.scale);
        std::uint32_t raw32 = 0;
        std::memcpy(&raw32, &scaled, sizeof(raw32));
        writeUnsignedBits(frame.payload, spec.can, bitLength, raw32);
    } else if (type == "bool" || type == "bit") {
        writeUnsignedBits(frame.payload, spec.can, bitLength, value == 0.0 ? 0 : 1);
    } else {
        writeUnsignedBits(frame.payload, spec.can, bitLength, toRawInteger(value, spec.scale, spec.offset));
    }
    return frame;
}

}  // namespace edge_gateway
