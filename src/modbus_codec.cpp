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

template <>
std::uint64_t readBigEndian<std::uint64_t>(const std::vector<std::uint8_t>& bytes) {
    return readUint64BigEndian(bytes);
}

template <>
std::int64_t readBigEndian<std::int64_t>(const std::vector<std::uint8_t>& bytes) {
    const auto raw = readUint64BigEndian(bytes);
    std::int64_t value = 0;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

template <>
double readBigEndian<double>(const std::vector<std::uint8_t>& bytes) {
    const auto raw = readUint64BigEndian(bytes);
    double value = 0.0;
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

template <>
std::vector<std::uint8_t> writeBigEndian<std::uint64_t>(std::uint64_t value) {
    return writeUint64BigEndian(value);
}

template <>
std::vector<std::uint8_t> writeBigEndian<std::int64_t>(std::int64_t value) {
    std::uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    return writeUint64BigEndian(raw);
}

template <>
std::vector<std::uint8_t> writeBigEndian<double>(double value) {
    std::uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    return writeUint64BigEndian(raw);
}

// Largest integer magnitude a double holds exactly; 64-bit register values beyond it are rejected
// for writes instead of being rounded silently.
constexpr double kMaxExactDoubleInteger = 9007199254740992.0;

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
    const std::string& byteOrder,
    const std::string& dataType
) {
    // Same contract as the read side: the order must name every byte of the type exactly once.
    // A shorter order used to drop the low bytes of 32-bit values without any error.
    if (byteOrder.size() != canonicalBytes.size() || !ModbusCodec::isByteOrderPermutation(byteOrder)) {
        throw std::invalid_argument(
            "write.byteOrder \"" + byteOrder + "\" does not match write.dataType " + dataType + " (" +
            std::to_string(canonicalBytes.size()) + " bytes)");
    }
    std::vector<std::uint8_t> reordered(byteOrder.size());
    for (std::size_t i = 0; i < byteOrder.size(); ++i) {
        reordered[static_cast<std::size_t>(byteOrder[i] - 'A')] = canonicalBytes[i];
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
    if (point.read.dataType == "bit" ||
        (point.read.dataType == "bool" && (point.read.bit != -1 ||
         point.read.function == 1 || point.read.function == 2))) {
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
        if (point.read.dataType == "bool") {
            rawValue = readBigEndian<std::uint16_t>(orderedBytes) == 0 ? 0.0 : 1.0;
        } else if (point.read.dataType == "uint16") {
            rawValue = static_cast<double>(readBigEndian<std::uint16_t>(orderedBytes));
        } else if (point.read.dataType == "int16") {
            rawValue = static_cast<double>(readBigEndian<std::int16_t>(orderedBytes));
        } else if (point.read.dataType == "uint32") {
            rawValue = static_cast<double>(readBigEndian<std::uint32_t>(orderedBytes));
        } else if (point.read.dataType == "int32") {
            rawValue = static_cast<double>(readBigEndian<std::int32_t>(orderedBytes));
        } else if (point.read.dataType == "float32") {
            rawValue = static_cast<double>(readBigEndian<float>(orderedBytes));
        } else if (point.read.dataType == "uint64") {
            rawValue = static_cast<double>(readBigEndian<std::uint64_t>(orderedBytes));
        } else if (point.read.dataType == "int64") {
            rawValue = static_cast<double>(readBigEndian<std::int64_t>(orderedBytes));
        } else if (point.read.dataType == "float64") {
            rawValue = readBigEndian<double>(orderedBytes);
        } else {
            throw std::invalid_argument("unsupported read.dataType: " + point.read.dataType);
        }
    }

    if (!std::isfinite(rawValue)) {
        throw std::invalid_argument("decoded register value is not finite");
    }
    const auto actualValue = applyReadScale(rawValue, point.read);
    if (!std::isfinite(actualValue)) {
        throw std::invalid_argument("scaled register value is not finite");
    }
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
    } else if (point.write.dataType == "uint64") {
        canonicalBytes = writeBigEndian(static_cast<std::uint64_t>(std::llround(rawValue)));
    } else if (point.write.dataType == "int64") {
        canonicalBytes = writeBigEndian(static_cast<std::int64_t>(std::llround(rawValue)));
    } else if (point.write.dataType == "float64") {
        canonicalBytes = writeBigEndian(rawValue);
    } else {
        throw std::invalid_argument("unsupported write.dataType: " + point.write.dataType);
    }

    const auto deviceBytes = applyByteOrderForEncode(canonicalBytes, point.write.byteOrder, point.write.dataType);
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

    // Reject values the register type cannot represent instead of letting the
    // integer cast wrap (e.g. 40000 on int16 would be written as -25536).
    if (!std::isfinite(businessValue)) {
        return {false, "value must be finite"};
    }
    const auto& type = point.write.dataType;
    if (type != "bit" && type != "bool") {
        if (std::abs(point.write.scale) < std::numeric_limits<double>::epsilon()) {
            return {false, "write.scale must not be zero"};
        }
        const auto raw = (businessValue - point.write.offset) / point.write.scale;
        const auto outside = [raw](double low, double high) {
            return !std::isfinite(raw) || raw <= low - 0.5 || raw >= high + 0.5;
        };
        if ((type == "uint16" && outside(0.0, 65535.0)) ||
            (type == "int16" && outside(-32768.0, 32767.0)) ||
            (type == "uint32" && outside(0.0, 4294967295.0)) ||
            (type == "int32" && outside(-2147483648.0, 2147483647.0)) ||
            (type == "uint64" && outside(0.0, kMaxExactDoubleInteger)) ||
            (type == "int64" && outside(-kMaxExactDoubleInteger, kMaxExactDoubleInteger)) ||
            (type == "float64" && !std::isfinite(raw)) ||
            (type == "float32" && (!std::isfinite(raw) ||
                std::abs(raw) > static_cast<double>(std::numeric_limits<float>::max())))) {
            return {false, "value out of range for write.dataType " + type};
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
    if (byteOrder.size() != bytes.size()) {
        throw std::invalid_argument("byteOrder \"" + byteOrder + "\" does not match " +
            std::to_string(bytes.size()) + " register bytes");
    }
    if (!isByteOrderPermutation(byteOrder)) {
        throw std::invalid_argument("byteOrder \"" + byteOrder + "\" must use each byte letter exactly once");
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

std::uint16_t ModbusCodec::setRegisterBit(std::uint16_t deviceRegister, int bit, bool value,
                                          const std::string& byteOrder) {
    if (bit < 0 || bit > 15) {
        throw std::invalid_argument("register bit write requires read.bit between 0 and 15");
    }
    const auto ordered = reorderBytes(flattenRegisters({deviceRegister}), byteOrder);
    auto word = static_cast<std::uint16_t>((static_cast<std::uint16_t>(ordered[0]) << 8) | ordered[1]);
    const auto mask = static_cast<std::uint16_t>(1U << bit);
    word = value ? static_cast<std::uint16_t>(word | mask) : static_cast<std::uint16_t>(word & ~mask);
    const auto device = applyByteOrderForEncode(writeUint16BigEndian(word), byteOrder, "uint16");
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(device[0]) << 8) | device[1]);
}

int ModbusCodec::registerTypeByteWidth(const std::string& dataType) {
    if (dataType == "bit" || dataType == "bool" || dataType == "uint16" || dataType == "int16") {
        return 2;
    }
    if (dataType == "uint32" || dataType == "int32" || dataType == "float32") {
        return 4;
    }
    if (dataType == "uint64" || dataType == "int64" || dataType == "float64") {
        return 8;
    }
    return 0;
}

std::string ModbusCodec::canonicalByteOrder(const std::string& dataType) {
    const auto width = registerTypeByteWidth(dataType);
    std::string order;
    for (int i = 0; i < width; ++i) {
        order.push_back(static_cast<char>('A' + i));
    }
    return order;
}

bool ModbusCodec::isByteOrderPermutation(const std::string& byteOrder) {
    if (byteOrder.empty() || byteOrder.size() > 8) {
        return false;
    }
    unsigned seen = 0;
    for (const char token : byteOrder) {
        if (token < 'A' || static_cast<std::size_t>(token - 'A') >= byteOrder.size()) {
            return false;
        }
        const unsigned bit = 1U << static_cast<unsigned>(token - 'A');
        if ((seen & bit) != 0U) {
            return false;
        }
        seen |= bit;
    }
    return true;
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
