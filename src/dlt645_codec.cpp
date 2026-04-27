#include "edge_gateway/dlt645_codec.hpp"

#include <algorithm>
#include <cctype>
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

std::uint8_t checksum(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t sum = 0;
    for (const auto byte : bytes) {
        sum += byte;
    }
    return static_cast<std::uint8_t>(sum & 0xFF);
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
    decoded.value = decodeBcdLittleEndian(valueBytes, point.read.scale) + point.read.offset;
    std::ostringstream text;
    text << decoded.value;
    decoded.text = text.str();
    return decoded;
}

}  // namespace edge_gateway
