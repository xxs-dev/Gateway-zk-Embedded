#include "edge_gateway/dlt645_codec.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace edge_gateway {

namespace {

std::uint8_t hexNibble(char ch) {
    if (ch >= '0' && ch <= '9') {
        return static_cast<std::uint8_t>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return static_cast<std::uint8_t>(10 + ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return static_cast<std::uint8_t>(10 + ch - 'A');
    }
    throw std::invalid_argument("invalid hex character");
}

std::string bytesToHex(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        out << std::setw(2) << static_cast<int>(byte);
    }
    return out.str();
}

double decodeBcdLittleEndian(const std::vector<std::uint8_t>& bytes, double scale) {
    std::string digits;
    digits.reserve(bytes.size() * 2);
    bool negative = false;
    for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {
        auto high = static_cast<std::uint8_t>((*it >> 4) & 0x0F);
        auto low = static_cast<std::uint8_t>(*it & 0x0F);
        if (it == bytes.rbegin() && high >= 0x0A) {
            negative = (high == 0x0A || high == 0x0B);
            high = 0;
        }
        if (high > 9 || low > 9) {
            throw std::runtime_error("invalid DLT645 BCD digit");
        }
        digits.push_back(static_cast<char>('0' + high));
        digits.push_back(static_cast<char>('0' + low));
    }
    while (!digits.empty() && digits.front() == '0') {
        digits.erase(digits.begin());
    }
    if (digits.empty()) {
        digits = "0";
    }
    const auto value = std::stod(digits) * scale;
    return negative ? -value : value;
}

double decodeUnsignedLittleEndian(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() > sizeof(std::uint64_t)) {
        throw std::runtime_error("DLT645 unsigned payload exceeds 64 bits");
    }
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8U);
    }
    return static_cast<double>(value);
}

std::uint8_t checksum(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t sum = 0;
    for (const auto byte : bytes) {
        sum += byte;
    }
    return static_cast<std::uint8_t>(sum & 0xFF);
}

std::vector<std::uint8_t> parseHexBytes(
    const std::string& text,
    std::size_t expectedBytes,
    const char* fieldName
) {
    std::string hex;
    hex.reserve(text.size());
    for (const auto ch : text) {
        if (std::isspace(static_cast<unsigned char>(ch)) != 0 || ch == '-' || ch == ':') {
            continue;
        }
        hex.push_back(ch);
    }
    if (hex.size() != expectedBytes * 2U) {
        throw std::invalid_argument(
            std::string(fieldName) + " must contain exactly " +
            std::to_string(expectedBytes * 2U) + " hex characters"
        );
    }

    std::vector<std::uint8_t> result;
    result.reserve(expectedBytes);
    for (std::size_t i = 0; i < hex.size(); i += 2U) {
        result.push_back(static_cast<std::uint8_t>((hexNibble(hex[i]) << 4U) | hexNibble(hex[i + 1U])));
    }
    return result;
}

std::vector<std::uint8_t> encodeAddress(const std::string& normalizedAddress) {
    std::vector<std::uint8_t> result;
    result.reserve(6);
    for (int i = static_cast<int>(normalizedAddress.size()) - 2; i >= 0; i -= 2) {
        result.push_back(static_cast<std::uint8_t>(
            (hexNibble(normalizedAddress[static_cast<std::size_t>(i)]) << 4U) |
            hexNibble(normalizedAddress[static_cast<std::size_t>(i + 1)])
        ));
    }
    return result;
}

std::uint8_t encodeBcdByte(int value) {
    if (value < 0 || value > 99) {
        throw std::invalid_argument("DLT645 BCD byte must be between 0 and 99");
    }
    return static_cast<std::uint8_t>(((value / 10) << 4U) | (value % 10));
}

std::uint64_t scaledInteger(double value, const WriteSpec& spec) {
    if (std::abs(spec.scale) < 1e-12) {
        throw std::invalid_argument("DLT645 write scale cannot be zero");
    }
    const auto raw = (value - spec.offset) / spec.scale;
    if (!std::isfinite(raw) || raw < 0.0 || std::abs(raw - std::round(raw)) > 1e-9) {
        throw std::invalid_argument("DLT645 write value cannot be represented as an unsigned integer");
    }
    return static_cast<std::uint64_t>(std::llround(raw));
}

}  // namespace

std::string Dlt645Codec::normalizeAddress(const std::string& meterAddress) {
    std::string digits;
    digits.reserve(meterAddress.size());
    for (const auto ch : meterAddress) {
        if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
            digits.push_back(ch);
        }
    }
    if (digits.empty() || digits.size() > 12) {
        throw std::invalid_argument("DLT645 address must contain 1..12 digits");
    }
    while (digits.size() < 12) {
        digits.insert(digits.begin(), '0');
    }
    return digits;
}

std::vector<std::uint8_t> Dlt645Codec::parseDataId(const std::string& dataIdHex) {
    std::string hex;
    hex.reserve(dataIdHex.size());
    for (const auto ch : dataIdHex) {
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            hex.push_back(ch);
        }
    }
    if (hex.size() != 8) {
        throw std::invalid_argument("DLT645 data id must be 8 hex chars");
    }
    std::vector<std::uint8_t> result;
    result.reserve(4);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        result.push_back(static_cast<std::uint8_t>((hexNibble(hex[i]) << 4) | hexNibble(hex[i + 1])));
    }
    std::reverse(result.begin(), result.end());
    return result;
}

std::vector<std::uint8_t> Dlt645Codec::buildReadFrame(
    const std::string& meterAddress,
    const std::string& dataIdHex
) {
    const auto normalizedAddress = normalizeAddress(meterAddress);
    const auto dataId = parseDataId(dataIdHex);

    std::vector<std::uint8_t> frame = {0x68};
    for (int i = static_cast<int>(normalizedAddress.size()) - 2; i >= 0; i -= 2) {
        frame.push_back(static_cast<std::uint8_t>(
            (hexNibble(normalizedAddress[static_cast<std::size_t>(i)]) << 4) |
            hexNibble(normalizedAddress[static_cast<std::size_t>(i + 1)])
        ));
    }
    frame.push_back(0x68);
    frame.push_back(0x11);
    frame.push_back(0x04);
    for (const auto byte : dataId) {
        frame.push_back(static_cast<std::uint8_t>(byte + 0x33));
    }
    frame.push_back(checksum(frame));
    frame.push_back(0x16);
    return frame;
}

std::vector<std::uint8_t> Dlt645Codec::buildWriteFrame(
    const std::string& meterAddress,
    const std::string& dataIdHex,
    const std::string& passwordHex,
    const std::string& operatorCodeHex,
    const std::vector<std::uint8_t>& payload
) {
    const auto normalizedAddress = normalizeAddress(meterAddress);
    const auto dataId = parseDataId(dataIdHex);
    const auto password = parseHexBytes(passwordHex, 4U, "DLT645 write password");
    const auto operatorCode = parseHexBytes(operatorCodeHex, 4U, "DLT645 operator code");

    std::vector<std::uint8_t> data;
    data.reserve(12U + payload.size());
    data.insert(data.end(), dataId.begin(), dataId.end());
    data.insert(data.end(), password.begin(), password.end());
    data.insert(data.end(), operatorCode.begin(), operatorCode.end());
    data.insert(data.end(), payload.begin(), payload.end());
    if (data.size() > 200U) {
        throw std::invalid_argument("DLT645 write data exceeds 200 bytes");
    }

    std::vector<std::uint8_t> frame = {0x68};
    const auto address = encodeAddress(normalizedAddress);
    frame.insert(frame.end(), address.begin(), address.end());
    frame.push_back(0x68);
    frame.push_back(0x14);
    frame.push_back(static_cast<std::uint8_t>(data.size()));
    for (const auto byte : data) {
        frame.push_back(static_cast<std::uint8_t>(byte + 0x33U));
    }
    frame.push_back(checksum(frame));
    frame.push_back(0x16);
    return frame;
}

std::vector<std::uint8_t> Dlt645Codec::encodeWritePayload(
    double value,
    const WriteSpec& spec
) {
    const auto dataType = spec.dlt645.dataType.empty() ? spec.dataType : spec.dlt645.dataType;
    if (dataType == "dlt645_none") {
        return {};
    }
    if (dataType == "dlt645_scheduled_control") {
        if (value < 0.0 || value > 99.0 || std::abs(value - std::round(value)) > 1e-9) {
            throw std::invalid_argument("DLT645 scheduled control delay must be an integer from 0 to 99");
        }
        if (spec.dlt645.unit != 2 && spec.dlt645.unit != 3) {
            throw std::invalid_argument("DLT645 scheduled control unit must be 2 (minutes) or 3 (hours)");
        }
        return {
            encodeBcdByte(static_cast<int>(std::llround(value))),
            encodeBcdByte(spec.dlt645.unit)
        };
    }
    if (dataType == "dlt645_fixed_hex") {
        if (spec.dlt645.byteCount < 0) {
            throw std::invalid_argument("DLT645 fixed payload byteCount cannot be negative");
        }
        const auto expectedBytes = static_cast<std::size_t>(spec.dlt645.byteCount);
        return parseHexBytes(spec.dlt645.fixedDataHex, expectedBytes, "DLT645 fixed payload");
    }
    if (spec.dlt645.byteCount <= 0 || spec.dlt645.byteCount > 8) {
        throw std::invalid_argument("DLT645 numeric write byteCount must be between 1 and 8");
    }

    auto raw = scaledInteger(value, spec);
    std::vector<std::uint8_t> payload;
    payload.reserve(static_cast<std::size_t>(spec.dlt645.byteCount));
    if (dataType == "dlt645_uint_le") {
        for (int i = 0; i < spec.dlt645.byteCount; ++i) {
            payload.push_back(static_cast<std::uint8_t>(raw & 0xFFU));
            raw >>= 8U;
        }
    } else if (dataType == "dlt645_bcd") {
        for (int i = 0; i < spec.dlt645.byteCount; ++i) {
            payload.push_back(encodeBcdByte(static_cast<int>(raw % 100U)));
            raw /= 100U;
        }
    } else {
        throw std::invalid_argument("unsupported DLT645 write dataType: " + dataType);
    }
    if (raw != 0U) {
        throw std::invalid_argument("DLT645 write value exceeds configured byteCount");
    }
    return payload;
}

void Dlt645Codec::validateWriteResponse(
    const std::vector<std::uint8_t>& frame,
    const std::string& meterAddress
) {
    if (frame.size() < 12U || frame.front() != 0x68 || frame[7] != 0x68 || frame.back() != 0x16) {
        throw std::runtime_error("DLT645 write response frame marker invalid");
    }
    const auto length = static_cast<std::size_t>(frame[9]);
    if (frame.size() != length + 12U) {
        throw std::runtime_error("DLT645 write response length mismatch");
    }
    if (checksum(std::vector<std::uint8_t>(frame.begin(), frame.end() - 2)) != frame[frame.size() - 2]) {
        throw std::runtime_error("DLT645 write response checksum mismatch");
    }

    const auto expectedAddress = encodeAddress(normalizeAddress(meterAddress));
    if (!std::equal(expectedAddress.begin(), expectedAddress.end(), frame.begin() + 1)) {
        throw std::runtime_error("DLT645 write response address mismatch");
    }
    if (frame[8] == 0xD4) {
        if (length != 1U) {
            throw std::runtime_error("DLT645 write exception response length mismatch");
        }
        const auto error = static_cast<std::uint8_t>(frame[10] - 0x33U);
        std::ostringstream message;
        message << "DLT645 write rejected ERR=0x"
                << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(error);
        throw std::runtime_error(message.str());
    }
    if (frame[8] != 0x94 || length != 0U) {
        throw std::runtime_error("DLT645 write response control code invalid");
    }
}

std::vector<std::uint8_t> Dlt645Codec::decodeFrameData(
    const std::vector<std::uint8_t>& frame,
    const std::vector<std::uint8_t>& expectedDataId
) {
    if (frame.size() < 16) {
        if (frame.size() >= 13 && frame.size() == static_cast<std::size_t>(frame[9]) + 12U &&
            frame.front() == 0x68 && frame[7] == 0x68 && frame.back() == 0x16 &&
            (frame[8] & 0x40U) != 0) {
            std::ostringstream message;
            message << "DLT645 exception response code=0x"
                    << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
                    << static_cast<int>(static_cast<std::uint8_t>(frame[10] - 0x33));
            throw std::runtime_error(message.str());
        }
        throw std::runtime_error("DLT645 response too short");
    }
    if (frame.front() != 0x68 || frame[7] != 0x68 || frame.back() != 0x16) {
        throw std::runtime_error("DLT645 frame marker invalid");
    }
    if (checksum(std::vector<std::uint8_t>(frame.begin(), frame.end() - 2)) != frame[frame.size() - 2]) {
        throw std::runtime_error("DLT645 checksum mismatch");
    }
    const auto control = frame[8];
    if ((control & 0x80U) == 0) {
        throw std::runtime_error("DLT645 response control code invalid");
    }
    const auto len = static_cast<std::size_t>(frame[9]);
    if (frame.size() != len + 12) {
        throw std::runtime_error("DLT645 response length mismatch");
    }
    if (len < 4) {
        throw std::runtime_error("DLT645 response data too short");
    }
    std::vector<std::uint8_t> data(frame.begin() + 10, frame.begin() + 10 + static_cast<std::ptrdiff_t>(len));
    for (auto& byte : data) {
        byte = static_cast<std::uint8_t>(byte - 0x33);
    }
    if (!std::equal(expectedDataId.begin(), expectedDataId.end(), data.begin())) {
        throw std::runtime_error("DLT645 response data id mismatch");
    }
    return std::vector<std::uint8_t>(data.begin() + 4, data.end());
}

DecodedValue Dlt645Codec::decodeReadResponse(
    const std::vector<std::uint8_t>& frame,
    const PointDefinition& point
) {
    const auto expectedDataId = parseDataId(point.read.dlt645Di);
    const auto payload = decodeFrameData(frame, expectedDataId);
    if (point.read.dlt645ByteCount > 0 && payload.size() < static_cast<std::size_t>(point.read.dlt645ByteCount)) {
        throw std::runtime_error("DLT645 payload shorter than expected byteCount");
    }

    std::vector<std::uint8_t> valueBytes = payload;
    if (point.read.dlt645ByteCount > 0) {
        valueBytes.resize(static_cast<std::size_t>(point.read.dlt645ByteCount));
    }

    DecodedValue decoded;
    decoded.rawHex = bytesToHex(valueBytes);
    if (point.read.dataType == "dlt645_ascii") {
        decoded.text.assign(valueBytes.begin(), valueBytes.end());
        decoded.value = 0.0;
        return decoded;
    }
    if (point.read.dataType == "dlt645_datetime") {
        decoded.text = bytesToHex(valueBytes);
        decoded.value = 0.0;
        return decoded;
    }
    if (point.read.dataType == "dlt645_hex") {
        decoded.text = decoded.rawHex;
        decoded.value = 0.0;
        return decoded;
    }
    if (point.read.dataType == "dlt645_uint_le" ||
        point.read.dataType == "dlt645_bitfield_le") {
        if (point.read.bit >= 0) {
            const auto bit = static_cast<std::size_t>(point.read.bit);
            const auto byteIndex = bit / 8U;
            if (byteIndex >= valueBytes.size()) {
                throw std::runtime_error("DLT645 bit index exceeds payload length");
            }
            decoded.value = static_cast<double>((valueBytes[byteIndex] >> (bit % 8U)) & 0x01U);
        } else {
            decoded.value = decodeUnsignedLittleEndian(valueBytes) * point.read.scale + point.read.offset;
        }
        std::ostringstream text;
        text << decoded.value;
        decoded.text = text.str();
        return decoded;
    }
    decoded.value = decodeBcdLittleEndian(valueBytes, point.read.scale) + point.read.offset;
    std::ostringstream text;
    text << decoded.value;
    decoded.text = text.str();
    return decoded;
}

}  // namespace edge_gateway
