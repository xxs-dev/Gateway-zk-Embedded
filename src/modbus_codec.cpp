#include "edge_gateway/modbus_codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace edge_gateway {

namespace {

std::uint16_t readUint16BigEndian(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != 2) {
        throw std::invalid_argument("unexpected byte width");
    }
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[0] << 8) | bytes[1]
    );
}

std::uint32_t readUint32BigEndian(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != 4) {
        throw std::invalid_argument("unexpected byte width");
    }
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

std::uint64_t readUint64BigEndian(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != 8) {
        throw std::invalid_argument("unexpected byte width");
    }
    std::uint64_t raw = 0;
    for (std::uint8_t byte : bytes) {
        raw = (raw << 8) | byte;
    }
    return raw;
}

std::vector<std::uint8_t> writeUint16BigEndian(std::uint16_t value) {
    return {
        static_cast<std::uint8_t>((value >> 8) & 0xFF),
        static_cast<std::uint8_t>(value & 0xFF)
    };
}

std::vector<std::uint8_t> writeUint32BigEndian(std::uint32_t value) {
    return {
        static_cast<std::uint8_t>((value >> 24) & 0xFF),
        static_cast<std::uint8_t>((value >> 16) & 0xFF),
        static_cast<std::uint8_t>((value >> 8) & 0xFF),
        static_cast<std::uint8_t>(value & 0xFF)
    };
}

std::vector<std::uint8_t> writeUint64BigEndian(std::uint64_t value) {
    return {
        static_cast<std::uint8_t>((value >> 56) & 0xFF),
        static_cast<std::uint8_t>((value >> 48) & 0xFF),
        static_cast<std::uint8_t>((value >> 40) & 0xFF),
        static_cast<std::uint8_t>((value >> 32) & 0xFF),
        static_cast<std::uint8_t>((value >> 24) & 0xFF),
        static_cast<std::uint8_t>((value >> 16) & 0xFF),
        static_cast<std::uint8_t>((value >> 8) & 0xFF),
        static_cast<std::uint8_t>(value & 0xFF)
    };
}

template <typename T>
T readBigEndian(const std::vector<std::uint8_t>& bytes);

template <>
std::uint16_t readBigEndian<std::uint16_t>(const std::vector<std::uint8_t>& bytes) {
    return readUint16BigEndian(bytes);
}

template <>
std::int16_t readBigEndian<std::int16_t>(const std::vector<std::uint8_t>& bytes) {
    const auto raw = readUint16BigEndian(bytes);
    std::int16_t value = 0;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

template <>
std::uint32_t readBigEndian<std::uint32_t>(const std::vector<std::uint8_t>& bytes) {
    return readUint32BigEndian(bytes);
}

template <>
std::int32_t readBigEndian<std::int32_t>(const std::vector<std::uint8_t>& bytes) {
    const auto raw = readUint32BigEndian(bytes);
    std::int32_t value = 0;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

template <>
float readBigEndian<float>(const std::vector<std::uint8_t>& bytes) {
    const auto raw = readUint32BigEndian(bytes);
    float value = 0.0F;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

template <typename T>
std::vector<std::uint8_t> writeBigEndian(T value);

template <>
std::vector<std::uint8_t> writeBigEndian<std::uint16_t>(std::uint16_t value) {
    return writeUint16BigEndian(value);
}

template <>
std::vector<std::uint8_t> writeBigEndian<std::int16_t>(std::int16_t value) {
    std::uint16_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    return writeUint16BigEndian(raw);
}

template <>
std::vector<std::uint8_t> writeBigEndian<std::uint32_t>(std::uint32_t value) {
    return writeUint32BigEndian(value);
}

template <>
std::vector<std::uint8_t> writeBigEndian<std::int32_t>(std::int32_t value) {
    std::uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    return writeUint32BigEndian(raw);
}

template <>
std::vector<std::uint8_t> writeBigEndian<float>(float value) {
    std::uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    return writeUint32BigEndian(raw);
}

double applyReadScale(double rawValue, const ReadSpec& spec) {
    return rawValue * spec.scale + spec.offset;
}

double removeWriteScale(double businessValue, const WriteSpec& spec) {
    if (std::abs(spec.scale) < std::numeric_limits<double>::epsilon()) {
        throw std::invalid_argument("write.scale must not be zero");
    }
    return (businessValue - spec.offset) / spec.scale;
}

std::string formatDouble(double value) {
    std::ostringstream oss;
    oss << std::setprecision(12) << value;
    return oss.str();
}

std::vector<std::uint8_t> applyByteOrderForEncode(
    const std::vector<std::uint8_t>& canonicalBytes,
    const std::string& byteOrder
) {
    if (byteOrder.empty()) {
        return canonicalBytes;
    }

    std::vector<std::uint8_t> reordered(byteOrder.size());
    for (std::size_t i = 0; i < byteOrder.size(); ++i) {
        const auto token = byteOrder[i];
        if (token < 'A' || static_cast<std::size_t>(token - 'A') >= canonicalBytes.size()) {
            throw std::invalid_argument("invalid byteOrder token");
        }
        reordered[static_cast<std::size_t>(token - 'A')] = canonicalBytes[i];
    }
    return reordered;
}

}  // namespace

DecodedValue ModbusCodec::decodeReadValue(
    const std::vector<std::uint16_t>& registers,
    const PointDefinition& point
) {
    if (!point.read.enable) {
        throw std::invalid_argument("point read is disabled");
    }
    if (registers.size() < static_cast<std::size_t>(point.read.length)) {
        throw std::invalid_argument("not enough registers");
    }

    double rawValue = 0.0;
    std::vector<std::uint8_t> rawBytes;
    if (point.read.dataType == "bit") {
        if (point.read.function == 1 || point.read.function == 2) {
            rawValue = registers.empty() || registers.front() == 0 ? 0.0 : 1.0;
            rawBytes.push_back(static_cast<std::uint8_t>(rawValue > 0.0 ? 1 : 0));
        } else {
            const auto flattened = flattenRegisters(registers);
            const auto byteCount = static_cast<std::size_t>(point.read.length) * 2;
            rawBytes.assign(flattened.begin(), flattened.begin() + static_cast<std::ptrdiff_t>(byteCount));
            const auto orderedBytes = reorderBytes(rawBytes, point.read.byteOrder);
            if (point.read.bit < 0 || point.read.bit > 15) {
                throw std::invalid_argument("bit dataType requires read.bit between 0 and 15 for register reads");
            }
            const auto word = readBigEndian<std::uint16_t>(orderedBytes);
            rawValue = static_cast<double>((word >> point.read.bit) & 0x1);
        }
    } else {
        const auto flattened = flattenRegisters(registers);
        const auto byteCount = static_cast<std::size_t>(point.read.length) * 2;
        rawBytes.assign(flattened.begin(), flattened.begin() + static_cast<std::ptrdiff_t>(byteCount));
        const auto orderedBytes = reorderBytes(rawBytes, point.read.byteOrder);
        if (point.read.dataType == "uint16") {
            rawValue = static_cast<double>(readBigEndian<std::uint16_t>(orderedBytes));
        } else if (point.read.dataType == "int16") {
            rawValue = static_cast<double>(readBigEndian<std::int16_t>(orderedBytes));
        } else if (point.read.dataType == "uint32") {
            rawValue = static_cast<double>(readBigEndian<std::uint32_t>(orderedBytes));
        } else if (point.read.dataType == "int32") {
            rawValue = static_cast<double>(readBigEndian<std::int32_t>(orderedBytes));
        } else if (point.read.dataType == "float32") {
            rawValue = static_cast<double>(readBigEndian<float>(orderedBytes));
        } else {
            throw std::invalid_argument("unsupported read.dataType: " + point.read.dataType);
        }
    }

    const auto actualValue = applyReadScale(rawValue, point.read);
    DecodedValue decoded;
    decoded.value = actualValue;
    decoded.text = formatDouble(actualValue);
    const auto mapped = point.valueMap.find(formatDouble(rawValue));
    if (mapped != point.valueMap.end()) {
        decoded.text = mapped->second;
    } else {
        const auto mappedInt = point.valueMap.find(std::to_string(static_cast<long long>(std::llround(rawValue))));
        if (mappedInt != point.valueMap.end()) {
            decoded.text = mappedInt->second;
        }
    }
    decoded.rawHex = bytesToHex(rawBytes);
    return decoded;
}

std::vector<std::uint16_t> ModbusCodec::encodeWriteValue(
    double businessValue,
    const PointDefinition& point
) {
    if (!point.write.enable) {
        throw std::invalid_argument("point write is disabled");
    }

    const auto validation = validateWriteValue(businessValue, point);
    if (!validation.ok) {
        throw std::invalid_argument(validation.message);
    }

    const auto rawValue = removeWriteScale(businessValue, point.write);
    std::vector<std::uint8_t> canonicalBytes;
    if (point.write.dataType == "bit" || point.write.dataType == "bool") {
        canonicalBytes = writeBigEndian(static_cast<std::uint16_t>(std::abs(rawValue) > 0.0 ? 1 : 0));
    } else if (point.write.dataType == "uint16") {
        canonicalBytes = writeBigEndian(static_cast<std::uint16_t>(std::llround(rawValue)));
    } else if (point.write.dataType == "int16") {
        canonicalBytes = writeBigEndian(static_cast<std::int16_t>(std::llround(rawValue)));
    } else if (point.write.dataType == "uint32") {
        canonicalBytes = writeBigEndian(static_cast<std::uint32_t>(std::llround(rawValue)));
    } else if (point.write.dataType == "int32") {
        canonicalBytes = writeBigEndian(static_cast<std::int32_t>(std::llround(rawValue)));
    } else if (point.write.dataType == "float32") {
        canonicalBytes = writeBigEndian(static_cast<float>(rawValue));
    } else {
        throw std::invalid_argument("unsupported write.dataType: " + point.write.dataType);
    }

    const auto deviceBytes = applyByteOrderForEncode(canonicalBytes, point.write.byteOrder);
    std::vector<std::uint16_t> registers;
    registers.reserve(deviceBytes.size() / 2);
    for (std::size_t i = 0; i < deviceBytes.size(); i += 2) {
        const auto hi = static_cast<std::uint16_t>(deviceBytes[i]);
        const auto lo = static_cast<std::uint16_t>(deviceBytes[i + 1]);
        registers.push_back(static_cast<std::uint16_t>((hi << 8) | lo));
    }
    return registers;
}

WriteValidationResult ModbusCodec::validateWriteValue(
    double businessValue,
    const PointDefinition& point
) {
    if (!point.write.enable) {
        return {false, "point write is disabled"};
    }

    if (point.write.minValue && businessValue < *point.write.minValue) {
        return {false, "value below min"};
    }
    if (point.write.maxValue && businessValue > *point.write.maxValue) {
        return {false, "value above max"};
    }
    if (point.write.step > 0.0 && point.write.minValue) {
        const auto ratio = (businessValue - *point.write.minValue) / point.write.step;
        if (std::abs(ratio - std::round(ratio)) > 1e-9) {
            return {false, "value does not match step"};
        }
    }
    if (!point.write.allowedValues.empty()) {
        const auto match = std::find_if(
            point.write.allowedValues.begin(),
            point.write.allowedValues.end(),
            [businessValue](double candidate) {
                return std::abs(candidate - businessValue) <= 1e-9;
            }
        );
        if (match == point.write.allowedValues.end()) {
            return {false, "value is not in allowedValues"};
        }
    }

    return {true, "ok"};
}

std::vector<std::uint8_t> ModbusCodec::flattenRegisters(const std::vector<std::uint16_t>& registers) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(registers.size() * 2);
    for (const auto reg : registers) {
        bytes.push_back(static_cast<std::uint8_t>((reg >> 8) & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>(reg & 0xFF));
    }
    return bytes;
}

std::vector<std::uint8_t> ModbusCodec::reorderBytes(
    const std::vector<std::uint8_t>& bytes,
    const std::string& byteOrder
) {
    if (byteOrder.empty()) {
        return bytes;
    }
    if (byteOrder.size() != bytes.size()) {
        throw std::invalid_argument("byteOrder size does not match byte length");
    }

    std::vector<std::uint8_t> ordered;
    ordered.reserve(bytes.size());
    for (char token : byteOrder) {
        if (token < 'A' || static_cast<std::size_t>(token - 'A') >= bytes.size()) {
            throw std::invalid_argument("invalid byteOrder token");
        }
        ordered.push_back(bytes[static_cast<std::size_t>(token - 'A')]);
    }
    return ordered;
}

std::string ModbusCodec::bytesToHex(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setfill('0');
    for (std::uint8_t byte : bytes) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

}  // namespace edge_gateway
