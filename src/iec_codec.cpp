#include "edge_gateway/iec_codec.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace edge_gateway {

namespace {

int readLe(const std::vector<std::uint8_t>& bytes, std::size_t offset, int width) {
    if (width < 0 || width > 4 || offset + static_cast<std::size_t>(width) > bytes.size()) {
        throw std::runtime_error("iec field out of range");
    }
    int value = 0;
    for (int i = 0; i < width; ++i) {
        value |= static_cast<int>(bytes[offset + static_cast<std::size_t>(i)]) << (8 * i);
    }
    return value;
}

void appendLe(std::vector<std::uint8_t>& bytes, int value, int width) {
    for (int i = 0; i < width; ++i) {
        bytes.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
    }
}

std::int16_t readInt16Le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    const auto raw = static_cast<std::uint16_t>(readLe(bytes, offset, 2));
    std::int16_t value = 0;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

std::int32_t readInt32Le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    const auto raw = static_cast<std::uint32_t>(readLe(bytes, offset, 4));
    std::int32_t value = 0;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

float readFloatLe(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    const auto raw = static_cast<std::uint32_t>(readLe(bytes, offset, 4));
    float value = 0.0F;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

std::size_t typePayloadSize(int typeId) {
    switch (typeId) {
        case 1: return 1;   // M_SP_NA_1
        case 3: return 1;   // M_DP_NA_1
        case 5: return 3;   // M_ST_NA_1
        case 7: return 5;   // M_BO_NA_1
        case 9: return 3;   // M_ME_NA_1
        case 11: return 3;  // M_ME_NB_1
        case 13: return 5;  // M_ME_NC_1
        case 15: return 5;  // M_IT_NA_1
        case 30: return 8;  // M_SP_TB_1
        case 31: return 8;  // M_DP_TB_1
        case 34: return 10; // M_ME_TD_1
        case 35: return 10; // M_ME_TE_1
        case 36: return 12; // M_ME_TF_1
        case 37: return 12; // M_IT_TB_1
        default:
            return 0;
    }
}

bool hasCp56Time(int typeId) {
    return typeId >= 30 && typeId <= 40;
}

double normalizeValue(const PointDefinition& point, double raw) {
    return raw * point.read.scale + point.read.offset;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::uint8_t ft12Checksum(const std::vector<std::uint8_t>& bytes, std::size_t begin, std::size_t end) {
    std::uint32_t sum = 0;
    for (auto i = begin; i < end && i < bytes.size(); ++i) {
        sum += bytes[i];
    }
    return static_cast<std::uint8_t>(sum & 0xFF);
}

void appendFt12VariableFrame(std::vector<std::uint8_t>& frame, const std::vector<std::uint8_t>& userData) {
    if (userData.size() > 255) {
        throw std::invalid_argument("iec ft1.2 frame too large");
    }
    frame.push_back(0x68);
    frame.push_back(static_cast<std::uint8_t>(userData.size()));
    frame.push_back(static_cast<std::uint8_t>(userData.size()));
    frame.push_back(0x68);
    frame.insert(frame.end(), userData.begin(), userData.end());
    frame.push_back(ft12Checksum(frame, 4, frame.size()));
    frame.push_back(0x16);
}

IecDataValue decodeInformationObject(
    const std::vector<std::uint8_t>& bytes,
    std::size_t payloadOffset,
    int typeId,
    int cause,
    int commonAddress,
    int ioa
) {
    IecDataValue value;
    value.commonAddress = commonAddress;
    value.ioa = ioa;
    value.typeId = typeId;
    value.cause = cause;
    value.rawHex = IecCodec::toHex(bytes);
    const auto payloadSize = typePayloadSize(typeId);
    if (payloadSize == 0 || payloadOffset + payloadSize > bytes.size()) {
        throw std::runtime_error("unsupported IEC ASDU typeId: " + std::to_string(typeId));
    }

    switch (typeId) {
        case 1:
        case 30:
            value.value = (bytes[payloadOffset] & 0x01U) != 0 ? 1.0 : 0.0;
            value.text = value.value > 0.0 ? "on" : "off";
            break;
        case 3:
        case 31:
            value.value = static_cast<double>(bytes[payloadOffset] & 0x03U);
            value.text = std::to_string(static_cast<int>(value.value));
            break;
        case 5:
            value.value = static_cast<double>(readInt16Le(bytes, payloadOffset));
            value.text = std::to_string(value.value);
            break;
        case 7:
            value.value = static_cast<double>(readInt32Le(bytes, payloadOffset));
            value.text = std::to_string(value.value);
            break;
        case 9:
        case 34:
            value.value = static_cast<double>(readInt16Le(bytes, payloadOffset)) / 32768.0;
            value.text = std::to_string(value.value);
            break;
        case 11:
        case 35:
            value.value = static_cast<double>(readInt16Le(bytes, payloadOffset));
            value.text = std::to_string(value.value);
            break;
        case 13:
        case 36:
            value.value = static_cast<double>(readFloatLe(bytes, payloadOffset));
            value.text = std::to_string(value.value);
            break;
        case 15:
        case 37:
            value.value = static_cast<double>(readInt32Le(bytes, payloadOffset));
            value.text = std::to_string(value.value);
            break;
        default:
            throw std::runtime_error("unsupported IEC ASDU typeId: " + std::to_string(typeId));
    }
    return value;
}

}  // namespace

std::vector<std::uint8_t> IecCodec::buildIec104StartDtAct() {
    return {0x68, 0x04, 0x07, 0x00, 0x00, 0x00};
}

std::vector<std::uint8_t> IecCodec::buildIec104InterrogationCommand(
    const IecProtocolConfig& config,
    std::uint16_t sendSequence,
    std::uint16_t receiveSequence
) {
    std::vector<std::uint8_t> asdu;
    asdu.push_back(100);
    asdu.push_back(1);
    appendLe(asdu, config.interrogationCot, config.cotSize);
    appendLe(asdu, config.commonAddress, config.caSize);
    appendLe(asdu, 0, config.ioaSize);
    asdu.push_back(static_cast<std::uint8_t>(config.interrogationQualifier));

    std::vector<std::uint8_t> frame;
    frame.push_back(0x68);
    frame.push_back(static_cast<std::uint8_t>(4 + asdu.size()));
    const auto send = static_cast<std::uint16_t>(sendSequence << 1);
    const auto recv = static_cast<std::uint16_t>(receiveSequence << 1);
    frame.push_back(static_cast<std::uint8_t>(send & 0xFF));
    frame.push_back(static_cast<std::uint8_t>((send >> 8) & 0xFF));
    frame.push_back(static_cast<std::uint8_t>(recv & 0xFF));
    frame.push_back(static_cast<std::uint8_t>((recv >> 8) & 0xFF));
    frame.insert(frame.end(), asdu.begin(), asdu.end());
    return frame;
}

bool IecCodec::isIec104Frame(const std::vector<std::uint8_t>& frame) {
    return frame.size() >= 6 && frame[0] == 0x68 && frame[1] == frame.size() - 2;
}

bool IecCodec::isIec104IFrame(const std::vector<std::uint8_t>& frame) {
    return isIec104Frame(frame) && (frame[2] & 0x01U) == 0;
}

bool IecCodec::isIec104StartDtCon(const std::vector<std::uint8_t>& frame) {
    return frame == std::vector<std::uint8_t>{0x68, 0x04, 0x0B, 0x00, 0x00, 0x00};
}

std::uint16_t IecCodec::iec104SendSequence(const std::vector<std::uint8_t>& frame) {
    if (!isIec104IFrame(frame)) {
        return 0;
    }
    return static_cast<std::uint16_t>((readLe(frame, 2, 2) >> 1) & 0x7FFF);
}

std::uint16_t IecCodec::iec104ReceiveSequence(const std::vector<std::uint8_t>& frame) {
    if (!isIec104IFrame(frame)) {
        return 0;
    }
    return static_cast<std::uint16_t>((readLe(frame, 4, 2) >> 1) & 0x7FFF);
}

std::vector<std::uint8_t> IecCodec::iec104AsduPayload(const std::vector<std::uint8_t>& frame) {
    if (!isIec104IFrame(frame) || frame.size() <= 6) {
        return {};
    }
    return std::vector<std::uint8_t>(frame.begin() + 6, frame.end());
}

std::vector<std::uint8_t> IecCodec::buildIec101InterrogationFrame(const IecProtocolConfig& config) {
    std::vector<std::uint8_t> userData;
    userData.push_back(0x73);
    appendLe(userData, config.linkAddress, config.linkAddressSize);
    userData.push_back(100);
    userData.push_back(1);
    appendLe(userData, config.interrogationCot, config.cotSize);
    appendLe(userData, config.commonAddress, config.caSize);
    appendLe(userData, 0, config.ioaSize);
    userData.push_back(static_cast<std::uint8_t>(config.interrogationQualifier));

    std::vector<std::uint8_t> frame;
    appendFt12VariableFrame(frame, userData);
    return frame;
}

std::vector<std::uint8_t> IecCodec::buildIec103GeneralInterrogationFrame(const IecProtocolConfig& config) {
    std::vector<std::uint8_t> userData;
    userData.push_back(0x73);
    appendLe(userData, config.linkAddress, config.linkAddressSize);
    userData.push_back(7);
    userData.push_back(1);
    appendLe(userData, config.interrogationCot, 1);
    appendLe(userData, config.commonAddress, 1);
    userData.push_back(0xFF);
    userData.push_back(0x00);

    std::vector<std::uint8_t> frame;
    appendFt12VariableFrame(frame, userData);
    return frame;
}

bool IecCodec::isFt12VariableFrame(const std::vector<std::uint8_t>& frame) {
    if (frame.size() < 6 || frame.front() != 0x68 || frame.back() != 0x16) {
        return false;
    }
    if (frame[1] != frame[2] || frame[3] != 0x68) {
        return false;
    }
    const auto length = static_cast<std::size_t>(frame[1]);
    if (frame.size() != length + 6) {
        return false;
    }
    return frame[frame.size() - 2] == ft12Checksum(frame, 4, frame.size() - 2);
}

std::vector<std::uint8_t> IecCodec::ft12UserData(const std::vector<std::uint8_t>& frame, int linkAddressSize) {
    if (!isFt12VariableFrame(frame)) {
        throw std::runtime_error("invalid IEC FT1.2 variable frame");
    }
    const auto begin = static_cast<std::size_t>(4);
    const auto end = frame.size() - 2;
    if (end <= begin + static_cast<std::size_t>(1 + std::max(0, linkAddressSize))) {
        return {};
    }
    return std::vector<std::uint8_t>(frame.begin() + begin, frame.begin() + end);
}

IecAsdu IecCodec::decodeAsdu(const std::vector<std::uint8_t>& asdu, const IecProtocolConfig& config) {
    if (asdu.size() < static_cast<std::size_t>(2 + config.cotSize + config.caSize)) {
        throw std::runtime_error("IEC ASDU too short");
    }
    IecAsdu result;
    result.typeId = asdu[0];
    const auto vsq = asdu[1];
    const auto count = static_cast<int>(vsq & 0x7FU);
    const bool sequential = (vsq & 0x80U) != 0;
    std::size_t offset = 2;
    result.cause = readLe(asdu, offset, config.cotSize) & 0x3F;
    offset += static_cast<std::size_t>(config.cotSize);
    result.commonAddress = readLe(asdu, offset, config.caSize);
    offset += static_cast<std::size_t>(config.caSize);

    const auto payloadSize = typePayloadSize(result.typeId);
    if (payloadSize == 0) {
        return result;
    }
    if (count <= 0) {
        return result;
    }
    if (sequential) {
        const int baseIoa = readLe(asdu, offset, config.ioaSize);
        offset += static_cast<std::size_t>(config.ioaSize);
        for (int i = 0; i < count; ++i) {
            if (offset + payloadSize > asdu.size()) {
                break;
            }
            result.values.push_back(decodeInformationObject(asdu, offset, result.typeId, result.cause, result.commonAddress, baseIoa + i));
            offset += payloadSize;
        }
    } else {
        for (int i = 0; i < count; ++i) {
            if (offset + static_cast<std::size_t>(config.ioaSize) + payloadSize > asdu.size()) {
                break;
            }
            const int ioa = readLe(asdu, offset, config.ioaSize);
            offset += static_cast<std::size_t>(config.ioaSize);
            result.values.push_back(decodeInformationObject(asdu, offset, result.typeId, result.cause, result.commonAddress, ioa));
            offset += payloadSize;
        }
    }
    (void)hasCp56Time;
    return result;
}

std::vector<IecDataValue> IecCodec::decodeIec103Data(
    const std::vector<std::uint8_t>& userData,
    const IecProtocolConfig& config
) {
    std::vector<IecDataValue> values;
    const auto headerSize = static_cast<std::size_t>(1 + std::max(0, config.linkAddressSize));
    if (userData.size() <= headerSize + 4) {
        return values;
    }
    std::size_t offset = headerSize;
    const int typeId = userData[offset++];
    const auto vsq = userData[offset++];
    const int count = std::max(1, static_cast<int>(vsq & 0x7FU));
    const int cause = userData[offset++];
    const int commonAddress = userData[offset++];
    for (int i = 0; i < count && offset + 2 <= userData.size(); ++i) {
        IecDataValue value;
        value.typeId = typeId;
        value.cause = cause;
        value.commonAddress = commonAddress;
        value.functionType = userData[offset++];
        value.informationNumber = userData[offset++];
        value.ioa = value.functionType * 256 + value.informationNumber;
        if (offset < userData.size()) {
            if (typeId == 9 && offset + 5 <= userData.size()) {
                const auto raw = readInt16Le(userData, offset);
                const auto exponent = static_cast<int>(userData[offset + 2] & 0x7FU);
                const double divisor = exponent > 0 ? std::pow(10.0, exponent) : 1.0;
                value.value = static_cast<double>(raw) / divisor;
                offset += 5;
            } else if (typeId == 1 || typeId == 2 || typeId == 6) {
                value.value = static_cast<double>(userData[offset++] & 0x03U);
            } else if (offset + 2 <= userData.size()) {
                value.value = static_cast<double>(readInt16Le(userData, offset));
                offset += 2;
            }
        }
        value.text = std::to_string(value.value);
        value.rawHex = toHex(userData);
        values.push_back(value);
    }
    return values;
}

bool IecCodec::pointMatches(const PointDefinition& point, const IecDataValue& value) {
    const auto& spec = point.read.iec;
    const int configuredIoa = spec.ioa >= 0 ? spec.ioa : (point.address > 0 ? point.address : -1);
    if (configuredIoa >= 0 && configuredIoa != value.ioa) {
        return false;
    }
    if (spec.commonAddress > 0 && spec.commonAddress != value.commonAddress) {
        return false;
    }
    if (spec.typeId > 0 && spec.typeId != value.typeId) {
        return false;
    }
    if (spec.cause > 0 && spec.cause != value.cause) {
        return false;
    }
    if (spec.functionType >= 0 && spec.functionType != value.functionType) {
        return false;
    }
    if (spec.informationNumber >= 0 && spec.informationNumber != value.informationNumber) {
        return false;
    }
    return true;
}

DecodedValue IecCodec::decodePointValue(const PointDefinition& point, const IecDataValue& value) {
    DecodedValue decoded;
    const auto dataType = lowercase(point.read.dataType.empty() ? point.read.iec.valueKind : point.read.dataType);
    double raw = value.value;
    if (dataType == "bit" || dataType == "bool" || dataType == "single_point" || dataType == "double_point") {
        raw = value.value > 0.0 ? value.value : 0.0;
    }
    decoded.value = normalizeValue(point, raw);
    decoded.text = value.text.empty() ? std::to_string(decoded.value) : value.text;
    decoded.rawHex = value.rawHex;
    return decoded;
}

std::string IecCodec::toHex(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        out << std::setw(2) << static_cast<int>(byte);
    }
    return out.str();
}

}  // namespace edge_gateway
