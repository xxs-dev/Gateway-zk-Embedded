#include "edge_gateway/dlt645_codec.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <ctime>
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

// Unsigned BCD, least significant byte first. Every nibble must be 0..9: A..F (including the
// FFH "no data" filler some meters send) is an invalid value, never a sign or a digit.
double decodeBcdLittleEndian(const std::vector<std::uint8_t>& bytes, double scale) {
    double value = 0.0;
    for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {
        const auto high = static_cast<std::uint8_t>((*it >> 4) & 0x0F);
        const auto low = static_cast<std::uint8_t>(*it & 0x0F);
        if (high > 9 || low > 9) {
            throw std::runtime_error("invalid DLT645 BCD digit");
        }
        value = value * 100.0 + static_cast<double>(high * 10 + low);
    }
    return value * scale;
}

// DL/T 645-2007 signed quantities (current, power, power factor, demand, combined energy): bit 7
// of the most significant byte is the sign, the remaining bits are BCD digits.
double decodeSignedBcdLittleEndian(std::vector<std::uint8_t> bytes, double scale) {
    if (bytes.empty()) {
        return 0.0;
    }
    const bool negative = (bytes.back() & 0x80U) != 0;
    bytes.back() = static_cast<std::uint8_t>(bytes.back() & 0x7FU);
    const auto magnitude = decodeBcdLittleEndian(bytes, scale);
    return negative ? -magnitude : magnitude;
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


// Frame with the 33H offset applied to the data field.
std::vector<std::uint8_t> frameWith(
    const std::vector<std::uint8_t>& address,
    std::uint8_t control,
    const std::vector<std::uint8_t>& data
) {
    if (data.size() > 200U) {
        throw std::invalid_argument("DLT645 data field exceeds 200 bytes");
    }
    std::vector<std::uint8_t> frame = {0x68};
    frame.insert(frame.end(), address.begin(), address.end());
    frame.push_back(0x68);
    frame.push_back(control);
    frame.push_back(static_cast<std::uint8_t>(data.size()));
    for (const auto byte : data) {
        frame.push_back(static_cast<std::uint8_t>(byte + 0x33U));
    }
    frame.push_back(checksum(frame));
    frame.push_back(0x16);
    return frame;
}

// ss mm hh DD MM YY, BCD (two-digit year).
std::vector<std::uint8_t> bcdDateTime(const std::tm& time) {
    return {
        encodeBcdByte(time.tm_sec > 59 ? 59 : time.tm_sec),
        encodeBcdByte(time.tm_min),
        encodeBcdByte(time.tm_hour),
        encodeBcdByte(time.tm_mday),
        encodeBcdByte(time.tm_mon + 1),
        encodeBcdByte((time.tm_year + 1900) % 100)
    };
}

void checkDataIdLength(const std::string& dataIdHex, Dlt645Version version) {
    if (dataIdHex.size() != dlt645DataIdHexLength(version)) {
        throw std::invalid_argument(std::string("DLT645 data id ") + dataIdHex + " does not fit " +
            dlt645ProtocolName(version) + " (" + std::to_string(dlt645DataIdHexLength(version)) + " hex digits)");
    }
}

std::string hexByte(std::uint8_t value) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(value);
    return out.str();
}

}  // namespace

std::uint8_t Dlt645Codec::readControl(Dlt645Version version) {
    return version == Dlt645Version::V1997 ? 0x01 : 0x11;
}

std::uint8_t Dlt645Codec::followUpControl(Dlt645Version version) {
    return version == Dlt645Version::V1997 ? 0x02 : 0x12;
}

std::uint8_t Dlt645Codec::writeControl(Dlt645Version version) {
    return version == Dlt645Version::V1997 ? 0x04 : 0x14;
}

std::string Dlt645Codec::normalizeAddress(const std::string& meterAddress) {
    if (meterAddress.empty() || meterAddress.size() > 12) {
        throw std::invalid_argument("DLT645 address must contain 1..12 decimal digits");
    }
    for (const auto ch : meterAddress) {
        if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
            throw std::invalid_argument("DLT645 address must contain only decimal digits");
        }
    }
    return std::string(12U - meterAddress.size(), '0') + meterAddress;
}

std::vector<std::uint8_t> Dlt645Codec::parseDataId(const std::string& dataIdHex) {
    if (dataIdHex.size() != 8 && dataIdHex.size() != 4) {
        throw std::invalid_argument("DLT645 data id must be 8 (2007) or 4 (1997) hex digits");
    }
    std::vector<std::uint8_t> result;
    for (std::size_t i = 0; i < dataIdHex.size(); i += 2) {
        result.push_back(static_cast<std::uint8_t>((hexNibble(dataIdHex[i]) << 4) | hexNibble(dataIdHex[i + 1])));
    }
    std::reverse(result.begin(), result.end());
    return result;
}

std::vector<std::uint8_t> Dlt645Codec::buildReadFrame(
    const std::string& meterAddress,
    const std::string& dataIdHex,
    Dlt645Version version
) {
    checkDataIdLength(dataIdHex, version);
    return frameWith(encodeAddress(normalizeAddress(meterAddress)), readControl(version), parseDataId(dataIdHex));
}

std::vector<std::uint8_t> Dlt645Codec::buildReadFollowUpFrame(
    const std::string& meterAddress,
    const std::string& dataIdHex,
    std::uint8_t sequence,
    Dlt645Version version
) {
    checkDataIdLength(dataIdHex, version);
    auto data = parseDataId(dataIdHex);
    if (version == Dlt645Version::V2007) {
        data.push_back(sequence);
    }
    return frameWith(encodeAddress(normalizeAddress(meterAddress)), followUpControl(version), data);
}

std::vector<std::uint8_t> Dlt645Codec::buildWriteFrame(
    const std::string& meterAddress,
    const std::string& dataIdHex,
    const std::string& passwordHex,
    const std::string& operatorCodeHex,
    const std::vector<std::uint8_t>& payload,
    Dlt645Version version
) {
    checkDataIdLength(dataIdHex, version);
    const auto address = encodeAddress(normalizeAddress(meterAddress));
    auto data = parseDataId(dataIdHex);
    const auto password = parseHexBytes(passwordHex, 4U, "DLT645 write password");
    data.insert(data.end(), password.begin(), password.end());
    if (version == Dlt645Version::V2007) {
        const auto operatorCode = parseHexBytes(operatorCodeHex, 4U, "DLT645 operator code");
        data.insert(data.end(), operatorCode.begin(), operatorCode.end());
    }
    data.insert(data.end(), payload.begin(), payload.end());
    return frameWith(address, writeControl(version), data);
}

std::vector<std::uint8_t> Dlt645Codec::buildRemoteControlFrame(
    const std::string& meterAddress,
    const std::string& passwordHex,
    const std::string& operatorCodeHex,
    std::uint8_t controlTypeCode,
    const std::tm& validUntil
) {
    const auto address = encodeAddress(normalizeAddress(meterAddress));
    auto data = parseHexBytes(passwordHex, 4U, "DLT645 control password");
    const auto operatorCode = parseHexBytes(operatorCodeHex, 4U, "DLT645 operator code");
    data.insert(data.end(), operatorCode.begin(), operatorCode.end());
    data.push_back(controlTypeCode);
    data.push_back(0x00);  // N2 reserved
    const auto deadline = bcdDateTime(validUntil);
    data.insert(data.end(), deadline.begin(), deadline.end());
    return frameWith(address, kRemoteControl, data);
}

std::tm Dlt645Codec::localTime(std::int64_t epochMs) {
    const auto seconds = static_cast<std::time_t>(epochMs / 1000);
    std::tm result{};
#ifdef _WIN32
    localtime_s(&result, &seconds);
#else
    localtime_r(&seconds, &result);
#endif
    return result;
}

std::vector<std::uint8_t> Dlt645Codec::buildBroadcastTimeFrame(const std::tm& localTime) {
    return frameWith(std::vector<std::uint8_t>(6, 0x99), kBroadcastTime, bcdDateTime(localTime));
}

std::vector<std::uint8_t> Dlt645Codec::encodeAddressPattern(const std::string& pattern) {
    if (pattern.size() != 12) {
        throw std::invalid_argument("DLT645 address pattern must have 12 characters (A5..A0)");
    }
    std::vector<std::uint8_t> address;
    bool wildcardAllowed = true;  // AA bytes only at the high end (shortened address)
    std::vector<std::uint8_t> highFirst;
    for (std::size_t i = 0; i < 12; i += 2) {
        const auto pair = pattern.substr(i, 2);
        if (pair == "AA" || pair == "aa") {
            if (!wildcardAllowed) {
                throw std::invalid_argument("DLT645 address pattern: AA may only shorten the high bytes");
            }
            highFirst.push_back(0xAA);
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(pair[0])) == 0 || std::isdigit(static_cast<unsigned char>(pair[1])) == 0) {
            throw std::invalid_argument("DLT645 address pattern: digits or leading AA bytes only");
        }
        wildcardAllowed = false;
        highFirst.push_back(static_cast<std::uint8_t>(((pair[0] - '0') << 4) | (pair[1] - '0')));
    }
    address.assign(highFirst.rbegin(), highFirst.rend());
    return address;
}

std::vector<std::uint8_t> Dlt645Codec::buildAddressQueryFrame(const std::string& pattern, Dlt645Version version) {
    const auto address = encodeAddressPattern(pattern);
    const bool allWildcard = std::all_of(address.begin(), address.end(), [](std::uint8_t byte) { return byte == 0xAA; });
    if (version == Dlt645Version::V2007 && allWildcard) {
        return frameWith(address, kReadAddress, {});
    }
    const auto dataId = version == Dlt645Version::V1997 ? std::string("C032") : std::string("04000401");
    return frameWith(address, readControl(version), parseDataId(dataId));
}

std::string Dlt645Codec::frameAddress(const std::vector<std::uint8_t>& frame) {
    if (frame.size() < 7U) {
        throw std::invalid_argument("DLT645 frame too short for an address");
    }
    std::string text;
    for (int i = 6; i >= 1; --i) {
        text += hexByte(frame[static_cast<std::size_t>(i)]);
    }
    return text;
}

std::uint8_t Dlt645Codec::remoteControlCode(const std::string& controlType) {
    if (controlType == "trip") return 0x1A;
    if (controlType == "close_permit") return 0x1B;
    if (controlType == "close") return 0x1C;
    if (controlType == "alarm") return 0x2A;
    if (controlType == "alarm_release") return 0x2B;
    if (controlType == "keep_power") return 0x3A;
    if (controlType == "keep_power_release") return 0x3B;
    throw std::invalid_argument("unknown DLT645 remote control type: " + controlType);
}

std::string Dlt645Codec::describeError(std::uint8_t error, Dlt645Version version) {
    static const char* const k2007[] = {
        "other error", "no requested data", "password error/unauthorized", "baud rate cannot be changed",
        "too many year time zones", "too many daily periods", "too many tariffs", "reserved"
    };
    static const char* const k1997[] = {
        "illegal data", "wrong data identifier", "password error", "reserved",
        "too many year time zones", "too many daily periods", "too many tariffs", "reserved"
    };
    const auto* names = version == Dlt645Version::V1997 ? k1997 : k2007;
    std::string text;
    for (int bit = 0; bit < 8; ++bit) {
        if ((error >> bit) & 0x01U) {
            if (!text.empty()) text += ", ";
            text += names[bit];
        }
    }
    return "ERR=0x" + hexByte(error) + " (" + (text.empty() ? std::string("unspecified") : text) + ")";
}

std::vector<std::uint8_t> Dlt645Codec::encodeWritePayload(
    double value,
    const WriteSpec& spec
) {
    const auto dataType = spec.dlt645.dataType.empty() ? spec.dataType : spec.dlt645.dataType;
    if (dataType == "dlt645_none") {
        return {};
    }
    if (dataType == "dlt645_remote_control") {
        // C=1CH carries no DI and no value: the point is a trigger, written with 1.
        if (std::abs(value - 1.0) > 1e-9) {
            throw std::invalid_argument("DLT645 remote control command is triggered with value 1");
        }
        (void)remoteControlCode(spec.dlt645.controlType);
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

void Dlt645Codec::validateCommandResponse(
    const std::vector<std::uint8_t>& frame,
    const std::string& meterAddress,
    std::uint8_t requestControl,
    Dlt645Version version
) {
    if (frame.size() < 12U || frame.front() != 0x68 || frame[7] != 0x68 || frame.back() != 0x16) {
        throw std::runtime_error("DLT645 command response frame marker invalid");
    }
    const auto length = static_cast<std::size_t>(frame[9]);
    if (frame.size() != length + 12U) {
        throw std::runtime_error("DLT645 command response length mismatch");
    }
    if (checksum(std::vector<std::uint8_t>(frame.begin(), frame.end() - 2)) != frame[frame.size() - 2]) {
        throw std::runtime_error("DLT645 command response checksum mismatch");
    }
    if (!frameAddressMatches(frame, meterAddress)) {
        throw std::runtime_error("DLT645 command response address mismatch");
    }
    const auto what = requestControl == kRemoteControl ? "remote control" : "write";
    if (frame[8] == static_cast<std::uint8_t>(requestControl | 0xC0U)) {
        if (length != 1U) {
            throw std::runtime_error(std::string("DLT645 ") + what + " exception response length mismatch");
        }
        throw std::runtime_error(std::string("DLT645 ") + what + " rejected " +
            describeError(static_cast<std::uint8_t>(frame[10] - 0x33U), version));
    }
    if (frame[8] != static_cast<std::uint8_t>(requestControl | 0x80U) || length != 0U) {
        throw std::runtime_error(std::string("DLT645 ") + what + " response control code invalid");
    }
}

std::vector<std::uint8_t> Dlt645Codec::decodeFrameData(
    const std::vector<std::uint8_t>& frame,
    const std::vector<std::uint8_t>& expectedDataId,
    Dlt645Version version
) {
    if (frame.size() < 12U || frame.front() != 0x68 || frame[7] != 0x68 || frame.back() != 0x16) {
        throw std::runtime_error("DLT645 frame marker invalid");
    }
    const auto len = static_cast<std::size_t>(frame[9]);
    if (frame.size() != len + 12U) {
        throw std::runtime_error("DLT645 response length mismatch");
    }
    if (checksum(std::vector<std::uint8_t>(frame.begin(), frame.end() - 2)) != frame[frame.size() - 2]) {
        throw std::runtime_error("DLT645 checksum mismatch");
    }
    const auto control = frame[8];
    if ((control & 0x80U) == 0) {
        throw std::runtime_error("DLT645 response control code invalid");
    }
    if ((control & 0x40U) != 0) {
        const auto error = len >= 1U ? static_cast<std::uint8_t>(frame[10] - 0x33U) : static_cast<std::uint8_t>(0);
        throw std::runtime_error("DLT645 exception response " + describeError(error, version));
    }
    if (len < expectedDataId.size()) {
        throw std::runtime_error("DLT645 response data too short");
    }
    std::vector<std::uint8_t> data(frame.begin() + 10, frame.begin() + 10 + static_cast<std::ptrdiff_t>(len));
    for (auto& byte : data) {
        byte = static_cast<std::uint8_t>(byte - 0x33);
    }
    if (!std::equal(expectedDataId.begin(), expectedDataId.end(), data.begin())) {
        throw std::runtime_error("DLT645 response data id mismatch");
    }
    return std::vector<std::uint8_t>(data.begin() + static_cast<std::ptrdiff_t>(expectedDataId.size()), data.end());
}

DecodedValue Dlt645Codec::decodeReadResponse(
    const std::vector<std::uint8_t>& frame,
    const PointDefinition& point
) {
    const auto dataId = parseDataId(point.read.dlt645Di);
    const auto version = dataId.size() == 2U ? Dlt645Version::V1997 : Dlt645Version::V2007;
    return decodePayload(decodeFrameData(frame, dataId, version), point);
}

DecodedValue Dlt645Codec::decodePayload(
    const std::vector<std::uint8_t>& data,
    const PointDefinition& point
) {
    const auto offset = static_cast<std::size_t>(std::max(0, point.read.dlt645Offset));
    const auto byteCount = static_cast<std::size_t>(std::max(0, point.read.dlt645ByteCount));
    if (offset > data.size() || (byteCount > 0 && offset + byteCount > data.size())) {
        throw std::runtime_error("DLT645 payload shorter than expected byteCount");
    }
    const auto first = data.begin() + static_cast<std::ptrdiff_t>(offset);
    const std::vector<std::uint8_t> valueBytes(
        first,
        byteCount > 0 ? first + static_cast<std::ptrdiff_t>(byteCount) : data.end()
    );

    DecodedValue decoded;
    decoded.rawHex = bytesToHex(valueBytes);
    const auto& dataType = point.read.dataType;
    if (dataType == "dlt645_ascii") {
        decoded.text.assign(valueBytes.begin(), valueBytes.end());
        decoded.value = 0.0;
        return decoded;
    }
    if (dataType == "dlt645_datetime" || dataType == "dlt645_hex") {
        decoded.text = decoded.rawHex;
        decoded.value = 0.0;
        return decoded;
    }
    if (dataType == "dlt645_uint_le" || dataType == "dlt645_bitfield_le") {
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
    } else if (dataType == "dlt645_bcd_signed") {
        decoded.value = decodeSignedBcdLittleEndian(valueBytes, point.read.scale) + point.read.offset;
    } else if (dataType == "dlt645_bcd") {
        decoded.value = decodeBcdLittleEndian(valueBytes, point.read.scale) + point.read.offset;
    } else {
        throw std::runtime_error("unsupported DLT645 read dataType: " + dataType);
    }
    std::ostringstream text;
    text << decoded.value;
    decoded.text = text.str();
    return decoded;
}

bool Dlt645Codec::takeFrame(std::vector<std::uint8_t>& buffer, std::vector<std::uint8_t>& frame) {
    std::size_t pending = buffer.size();
    for (std::size_t start = 0; start < buffer.size(); ++start) {
        if (buffer[start] != 0x68) {
            continue;
        }
        const auto available = buffer.size() - start;
        if (available < 10) {
            pending = std::min(pending, start);
            break;
        }
        if (buffer[start + 7] != 0x68) {
            continue;
        }
        const auto size = static_cast<std::size_t>(buffer[start + 9]) + 12U;
        if (available < size) {
            // Either still arriving or a truncated frame; a complete frame behind it wins.
            pending = std::min(pending, start);
            continue;
        }
        const auto first = buffer.begin() + static_cast<std::ptrdiff_t>(start);
        const auto last = first + static_cast<std::ptrdiff_t>(size);
        if (*(last - 1) != 0x16 || checksum(std::vector<std::uint8_t>(first, last - 2)) != *(last - 2)) {
            continue;
        }
        frame.assign(first, last);
        buffer.erase(buffer.begin(), last);
        return true;
    }
    buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(pending));
    if (buffer.size() > 512U) {
        buffer.erase(buffer.begin(), buffer.end() - 256);
    }
    return false;
}

bool Dlt645Codec::frameAddressMatches(const std::vector<std::uint8_t>& frame, const std::string& meterAddress) {
    if (frame.size() < 8U) {
        return false;
    }
    const auto expected = encodeAddress(normalizeAddress(meterAddress));
    return std::equal(expected.begin(), expected.end(), frame.begin() + 1);
}

}  // namespace edge_gateway
