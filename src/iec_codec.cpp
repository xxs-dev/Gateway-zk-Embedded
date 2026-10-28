#include "edge_gateway/iec_codec.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "iec_codec_detail.hpp"

namespace edge_gateway {

using iec_detail::appendCp56Time2a;
using iec_detail::appendLe;
using iec_detail::applyQualityDescriptor;
using iec_detail::decodeCp24Time2a;
using iec_detail::decodeCp56Time2a;
using iec_detail::markBad;
using iec_detail::qualityFlagNames;
using iec_detail::readFloatLe;
using iec_detail::readInt16Le;
using iec_detail::readInt32Le;
using iec_detail::readLe;
using iec_detail::readUnsignedLe;

namespace {

// Information element size (without IOA) of each supported ASDU type.
std::size_t typePayloadSize(int typeId) {
    switch (typeId) {
        case 1: return 1;   // M_SP_NA_1: SIQ
        case 2: return 4;   // M_SP_TA_1: SIQ + CP24Time2a
        case 3: return 1;   // M_DP_NA_1: DIQ
        case 4: return 4;   // M_DP_TA_1
        case 5: return 2;   // M_ST_NA_1: VTI + QDS
        case 6: return 5;   // M_ST_TA_1
        case 7: return 5;   // M_BO_NA_1: BSI + QDS
        case 8: return 8;   // M_BO_TA_1
        case 9: return 3;   // M_ME_NA_1: NVA + QDS
        case 10: return 6;  // M_ME_TA_1
        case 11: return 3;  // M_ME_NB_1: SVA + QDS
        case 12: return 6;  // M_ME_TB_1
        case 13: return 5;  // M_ME_NC_1: IEEE STD 754 + QDS
        case 14: return 8;  // M_ME_TC_1
        case 15: return 5;  // M_IT_NA_1: BCR
        case 16: return 8;  // M_IT_TA_1
        case 17: return 6;  // M_EP_TA_1: SEP + CP16Time2a + CP24Time2a
        case 18: return 7;  // M_EP_TB_1: SPE + QDP + CP16Time2a + CP24Time2a
        case 19: return 7;  // M_EP_TC_1: OCI + QDP + CP16Time2a + CP24Time2a
        case 20: return 5;  // M_PS_NA_1: SCD + QDS
        case 21: return 2;  // M_ME_ND_1: NVA
        case 30: return 8;  // M_SP_TB_1
        case 31: return 8;  // M_DP_TB_1
        case 32: return 9;  // M_ST_TB_1
        case 33: return 12; // M_BO_TB_1
        case 34: return 10; // M_ME_TD_1
        case 35: return 10; // M_ME_TE_1
        case 36: return 12; // M_ME_TF_1
        case 37: return 12; // M_IT_TB_1
        case 38: return 10; // M_EP_TD_1
        case 39: return 11; // M_EP_TE_1
        case 40: return 11; // M_EP_TF_1
        case 45: return 1;  // C_SC_NA_1: SCO
        case 46: return 1;  // C_DC_NA_1: DCO
        case 47: return 1;  // C_RC_NA_1: RCO
        case 48: return 3;  // C_SE_NA_1: NVA + QOS
        case 49: return 3;  // C_SE_NB_1: SVA + QOS
        case 50: return 5;  // C_SE_NC_1: IEEE STD 754 + QOS
        case 51: return 4;  // C_BO_NA_1: BSI
        case 58: return 8;  // C_SC_TA_1
        case 59: return 8;  // C_DC_TA_1
        case 60: return 8;  // C_RC_TA_1
        case 61: return 10; // C_SE_TA_1
        case 62: return 10; // C_SE_TB_1
        case 63: return 12; // C_SE_TC_1
        case 64: return 11; // C_BO_TA_1
        default:
            return 0;
    }
}

int timeTagSize(int typeId) {
    if ((typeId >= 30 && typeId <= 40) || (typeId >= 58 && typeId <= 64)) {
        return 7;
    }
    if ((typeId >= 2 && typeId <= 16 && (typeId % 2) == 0) || (typeId >= 17 && typeId <= 19)) {
        return 3;
    }
    return 0;
}

// Command types with a CP56Time2a time tag map onto their untimed counterparts.
int untimedCommandType(int typeId) {
    if (typeId >= 58 && typeId <= 63) {
        return typeId - 13;
    }
    if (typeId == 64) {
        return 51;
    }
    return typeId;
}

bool isParameterType(int typeId) {
    return typeId >= 110 && typeId <= 113;
}

bool isFileType(int typeId) {
    return typeId >= 120 && typeId <= 126;
}

bool isProtectionType(int typeId) {
    return (typeId >= 17 && typeId <= 19) || (typeId >= 38 && typeId <= 40);
}

void appendCot(std::vector<std::uint8_t>& bytes, int cause, const IecProtocolConfig& config) {
    if (config.cotSize <= 1) {
        appendLe(bytes, static_cast<std::uint32_t>(cause & 0x3F), 1);
        return;
    }
    appendLe(bytes, static_cast<std::uint32_t>((cause & 0x3F) | ((config.originatorAddress & 0xFF) << 8)), config.cotSize);
}

void appendAsduHeader(
    std::vector<std::uint8_t>& asdu,
    int typeId,
    int cause,
    int commonAddress,
    const IecProtocolConfig& config
) {
    asdu.push_back(static_cast<std::uint8_t>(typeId));
    asdu.push_back(1);
    appendCot(asdu, cause, config);
    appendLe(asdu, static_cast<std::uint32_t>(commonAddress), config.caSize);
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string pointDataType(const PointDefinition& point) {
    return lowercase(point.read.dataType.empty() ? point.read.iec.valueKind : point.read.dataType);
}

bool isBooleanDataType(const std::string& dataType) {
    return dataType == "bit" || dataType == "bool" || dataType == "boolean" || dataType == "single_point";
}

int commandIoaOf(const PointDefinition& point) {
    if (point.write.iec.ioa >= 0) {
        return point.write.iec.ioa;
    }
    if (point.read.iec.ioa >= 0) {
        return point.read.iec.ioa;
    }
    if (point.write.address >= 0) {
        return point.write.address;
    }
    return point.address;
}

int commandCommonAddressOf(const IecProtocolConfig& config, const PointDefinition& point) {
    if (point.write.iec.commonAddress > 0) {
        return point.write.iec.commonAddress;
    }
    if (point.read.iec.commonAddress > 0) {
        return point.read.iec.commonAddress;
    }
    return config.commonAddress;
}

// SCO/DCO/RCO: QU in bits 2..6. QOS: QL in bits 0..6. Bit 7 is S/E in both.
std::uint8_t commandQualifierByte(const PointDefinition& point, int untimedType, bool select, std::uint8_t lowBits) {
    const bool setpoint = untimedType >= 48 && untimedType <= 50;
    const auto mask = static_cast<std::uint8_t>(setpoint ? 0x7FU : 0x7CU);
    auto qualifier = static_cast<std::uint8_t>((static_cast<unsigned>(point.write.iec.qualifier) & mask) | lowBits);
    if (select) {
        qualifier = static_cast<std::uint8_t>(qualifier | 0x80U);
    }
    return qualifier;
}

std::vector<std::uint8_t> buildIec104IFrame(
    const std::vector<std::uint8_t>& asdu,
    std::uint16_t sendSequence,
    std::uint16_t receiveSequence
) {
    // APDU length octet max 253 = 4 control octets + 249 ASDU octets.
    if (asdu.size() > 249) {
        throw std::invalid_argument("IEC104 ASDU too large");
    }
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

int quantizeNormalized(double rawValue) {
    int raw = static_cast<int>(std::round(rawValue * 32768.0));
    return std::max(-32768, std::min(32767, raw));
}

int quantizeScaled(double rawValue) {
    int raw = static_cast<int>(std::round(rawValue));
    return std::max(-32768, std::min(32767, raw));
}

std::uint32_t quantizeBitstring(double value) {
    if (!(value >= 0.0) || value > 4294967295.0 || std::floor(value) != value) {
        throw std::invalid_argument("IEC bitstring command value must be an integer 0..4294967295");
    }
    return static_cast<std::uint32_t>(value);
}

IecDataValue baseValue(
    const std::vector<std::uint8_t>& bytes,
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
    return value;
}

void decodeInformationObject(
    std::vector<IecDataValue>& out,
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    int typeId,
    int cause,
    int commonAddress,
    int ioa,
    const iec_detail::TimeTagContext& time
) {
    const auto payloadSize = typePayloadSize(typeId);
    if (payloadSize == 0 || offset + payloadSize > bytes.size()) {
        throw std::runtime_error("unsupported IEC ASDU typeId: " + std::to_string(typeId));
    }
    auto value = baseValue(bytes, typeId, cause, commonAddress, ioa);
    const int tagSize = timeTagSize(typeId);
    if (tagSize == 7 && !(typeId >= 58 && typeId <= 64)) {
        value.timestampMs = decodeCp56Time2a(bytes, offset + payloadSize - 7, time.utc);
    } else if (tagSize == 3) {
        value.timestampMs = decodeCp24Time2a(bytes, offset + payloadSize - 3, time);
    }

    switch (untimedCommandType(typeId)) {
        case 1:
        case 2:
        case 30:
            value.value = (bytes[offset] & 0x01U) != 0 ? 1.0 : 0.0;
            value.text = value.value > 0.0 ? "on" : "off";
            applyQualityDescriptor(value, bytes[offset], false);
            break;
        case 3:
        case 4:
        case 31: {
            const int dpi = bytes[offset] & 0x03;
            value.value = static_cast<double>(dpi);
            value.text = std::to_string(dpi);
            value.doublePoint = true;
            applyQualityDescriptor(value, bytes[offset], false);
            if (dpi == 0 || dpi == 3) {
                markBad(value, std::string("double point ") + (dpi == 0 ? "intermediate" : "indeterminate") +
                    " (DPI=" + std::to_string(dpi) + ")");
            }
            break;
        }
        case 5:
        case 6:
        case 32: {
            const auto vti = bytes[offset];
            int step = vti & 0x7FU;
            if ((step & 0x40) != 0) step -= 0x80;
            value.value = static_cast<double>(step);
            value.text = std::to_string(value.value);
            value.transient = (vti & 0x80U) != 0;
            applyQualityDescriptor(value, bytes[offset + 1], true);
            break;
        }
        case 7:
        case 8:
        case 33:
            // BSI is a 32-bit string, never a signed number.
            value.value = static_cast<double>(readUnsignedLe(bytes, offset, 4));
            value.text = std::to_string(readUnsignedLe(bytes, offset, 4));
            applyQualityDescriptor(value, bytes[offset + 4], true);
            break;
        case 9:
        case 10:
        case 34:
            value.value = static_cast<double>(readInt16Le(bytes, offset)) / 32768.0;
            value.text = std::to_string(value.value);
            applyQualityDescriptor(value, bytes[offset + 2], true);
            break;
        case 11:
        case 12:
        case 35:
            value.value = static_cast<double>(readInt16Le(bytes, offset));
            value.text = std::to_string(value.value);
            applyQualityDescriptor(value, bytes[offset + 2], true);
            break;
        case 13:
        case 14:
        case 36:
            value.value = static_cast<double>(readFloatLe(bytes, offset));
            value.text = std::to_string(value.value);
            applyQualityDescriptor(value, bytes[offset + 4], true);
            if (!std::isfinite(value.value)) {
                markBad(value, "IEC short float is not finite");
            }
            break;
        case 15:
        case 16:
        case 37: {
            // BCR: counter reading + sequence octet (SQ 0..4, CY 0x20, CA 0x40, IV 0x80).
            value.value = static_cast<double>(readInt32Le(bytes, offset));
            value.text = std::to_string(readInt32Le(bytes, offset));
            const auto sequence = bytes[offset + 4];
            if ((sequence & 0x80U) != 0) {
                markBad(value, "IEC counter IV (invalid)");
            }
            if ((sequence & 0x20U) != 0) {
                markBad(value, "IEC counter CY (carry/overflow in period)");
            }
            break;
        }
        case 20: {
            // SCD: ST status bits 0..15, CD change-detection bits 16..31. One value per bit.
            const auto scd = readUnsignedLe(bytes, offset, 4);
            const auto qds = bytes[offset + 4];
            for (int bit = 0; bit < 16; ++bit) {
                auto element = value;
                element.elementIndex = bit;
                element.value = ((scd >> bit) & 0x01U) != 0 ? 1.0 : 0.0;
                element.text = element.value > 0.0 ? "on" : "off";
                applyQualityDescriptor(element, qds, true);
                out.push_back(std::move(element));
            }
            return;
        }
        case 21:
            value.value = static_cast<double>(readInt16Le(bytes, offset)) / 32768.0;
            value.text = std::to_string(value.value);
            break;
        case 45:
            value.value = (bytes[offset] & 0x01U) != 0 ? 1.0 : 0.0;
            value.text = value.value > 0.0 ? "on" : "off";
            value.commandQualifier = bytes[offset];
            break;
        case 46:
        case 47:
            value.value = static_cast<double>(bytes[offset] & 0x03U);
            value.text = std::to_string(static_cast<int>(value.value));
            value.commandQualifier = bytes[offset];
            break;
        case 48:
            value.value = static_cast<double>(readInt16Le(bytes, offset)) / 32768.0;
            value.text = std::to_string(value.value);
            value.commandQualifier = bytes[offset + 2];
            break;
        case 49:
            value.value = static_cast<double>(readInt16Le(bytes, offset));
            value.text = std::to_string(value.value);
            value.commandQualifier = bytes[offset + 2];
            break;
        case 50:
            value.value = static_cast<double>(readFloatLe(bytes, offset));
            value.text = std::to_string(value.value);
            value.commandQualifier = bytes[offset + 4];
            break;
        case 51:
            value.value = static_cast<double>(readUnsignedLe(bytes, offset, 4));
            value.text = std::to_string(readUnsignedLe(bytes, offset, 4));
            break;
        default:
            throw std::runtime_error("unsupported IEC ASDU typeId: " + std::to_string(typeId));
    }
    out.push_back(std::move(value));
}

IecProtectionEvent decodeProtectionEvent(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    int typeId,
    int cause,
    int commonAddress,
    int ioa,
    const iec_detail::TimeTagContext& time
) {
    IecProtectionEvent event;
    event.commonAddress = commonAddress;
    event.ioa = ioa;
    event.typeId = typeId;
    event.cause = cause;
    event.rawHex = IecCodec::toHex(bytes);
    const auto payloadSize = typePayloadSize(typeId);
    if (offset + payloadSize > bytes.size()) {
        throw std::runtime_error("IEC protection ASDU too short");
    }
    std::uint8_t descriptor = 0;
    if (typeId == 17 || typeId == 38) {
        // SEP: ES bits 0..1, EI 0x08, BL, SB, NT, IV; CP16Time2a elapsed time.
        descriptor = bytes[offset];
        event.eventState = descriptor & 0x03U;
        event.elapsedTimeMs = readLe(bytes, offset + 1, 2);
        if (event.eventState == 0 || event.eventState == 3) {
            event.quality = 0;
            event.qualityMsg = "protection event state indeterminate (ES=" + std::to_string(event.eventState) + ")";
        }
    } else {
        // SPE (18/39) or OCI (19/40), then QDP and CP16Time2a relay duration/operating time.
        event.eventState = bytes[offset] & (typeId == 18 || typeId == 39 ? 0x3FU : 0x0FU);
        descriptor = bytes[offset + 1];
        event.relayDurationMs = readLe(bytes, offset + 2, 2);
    }
    const auto names = qualityFlagNames(descriptor, false);
    if (!names.empty()) {
        event.quality = 0;
        event.qualityMsg += (event.qualityMsg.empty() ? "" : "; ") + std::string("IEC quality ") + names;
    }
    if (typeId >= 38) {
        event.timestampMs = decodeCp56Time2a(bytes, offset + payloadSize - 7, time.utc);
    } else {
        event.timestampMs = decodeCp24Time2a(bytes, offset + payloadSize - 3, time);
    }
    return event;
}

IecParameterValue decodeParameterObject(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    int typeId,
    int cause,
    int commonAddress,
    int ioa
) {
    IecParameterValue parameter;
    parameter.commonAddress = commonAddress;
    parameter.ioa = ioa;
    parameter.typeId = typeId;
    parameter.cause = cause;
    parameter.rawHex = IecCodec::toHex(bytes);
    switch (typeId) {
        case 110:
            if (offset + 3 > bytes.size()) {
                throw std::runtime_error("IEC normalized parameter too short");
            }
            parameter.value = static_cast<double>(readInt16Le(bytes, offset)) / 32768.0;
            parameter.qualifier = bytes[offset + 2];
            return parameter;
        case 111:
            if (offset + 3 > bytes.size()) {
                throw std::runtime_error("IEC scaled parameter too short");
            }
            parameter.value = static_cast<double>(readInt16Le(bytes, offset));
            parameter.qualifier = bytes[offset + 2];
            return parameter;
        case 112:
            if (offset + 5 > bytes.size()) {
                throw std::runtime_error("IEC float parameter too short");
            }
            parameter.value = static_cast<double>(readFloatLe(bytes, offset));
            parameter.qualifier = bytes[offset + 4];
            return parameter;
        case 113:
            if (offset + 1 > bytes.size()) {
                throw std::runtime_error("IEC parameter activation too short");
            }
            parameter.qualifier = bytes[offset];
            return parameter;
        default:
            throw std::runtime_error("unsupported IEC parameter typeId: " + std::to_string(typeId));
    }
}

IecFileSegment decodeFileObject(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    int typeId,
    int cause,
    int commonAddress,
    int ioa
) {
    IecFileSegment segment;
    segment.commonAddress = commonAddress;
    segment.ioa = ioa;
    segment.cause = cause;
    segment.rawHex = IecCodec::toHex(bytes);
    switch (typeId) {
        case 120:
            if (offset + 5 > bytes.size()) {
                throw std::runtime_error("IEC file ready ASDU too short");
            }
            segment.nameOfFile = readLe(bytes, offset, 2);
            segment.nameOfSection = 0;
            segment.qualifier = bytes[offset + 4];
            return segment;
        case 121:
            if (offset + 6 > bytes.size()) {
                throw std::runtime_error("IEC section ready ASDU too short");
            }
            segment.nameOfFile = readLe(bytes, offset, 2);
            segment.nameOfSection = bytes[offset + 2];
            segment.qualifier = bytes[offset + 5];
            return segment;
        case 122:
        case 124:
            if (offset + 4 > bytes.size()) {
                throw std::runtime_error("IEC file command ASDU too short");
            }
            segment.nameOfFile = readLe(bytes, offset, 2);
            segment.nameOfSection = bytes[offset + 2];
            segment.qualifier = bytes[offset + 3];
            return segment;
        case 123:
            if (offset + 4 > bytes.size()) {
                throw std::runtime_error("IEC last section ASDU too short");
            }
            segment.nameOfFile = readLe(bytes, offset, 2);
            segment.nameOfSection = bytes[offset + 2];
            segment.lastSectionOrSegment = bytes[offset + 3];
            return segment;
        case 125:
            if (offset + 5 > bytes.size()) {
                throw std::runtime_error("IEC file segment ASDU too short");
            }
            segment.nameOfFile = readLe(bytes, offset, 2);
            segment.nameOfSection = bytes[offset + 2];
            segment.qualifier = bytes[offset + 3];
            {
                const auto length = static_cast<std::size_t>(bytes[offset + 4]);
                const auto dataOffset = offset + 5;
                if (dataOffset + length > bytes.size()) {
                    throw std::runtime_error("IEC file segment data out of range");
                }
                segment.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(dataOffset),
                    bytes.begin() + static_cast<std::ptrdiff_t>(dataOffset + length));
            }
            return segment;
        case 126:
            if (offset + 4 > bytes.size()) {
                throw std::runtime_error("IEC file directory ASDU too short");
            }
            segment.nameOfFile = readLe(bytes, offset, 2);
            segment.qualifier = bytes[offset + 2];
            return segment;
        default:
            throw std::runtime_error("unsupported IEC file typeId: " + std::to_string(typeId));
    }
}

// Type families: the untimed type a point is configured with also matches the CP24/CP56
// variants the station uses for spontaneous transmission (and 21 for normalized values).
int iec104TypeFamily(int typeId) {
    switch (typeId) {
        case 1: case 2: case 30: return 1;
        case 3: case 4: case 31: return 3;
        case 5: case 6: case 32: return 5;
        case 7: case 8: case 33: return 7;
        case 9: case 10: case 21: case 34: return 9;
        case 11: case 12: case 35: return 11;
        case 13: case 14: case 36: return 13;
        case 15: case 16: case 37: return 15;
        case 17: case 38: return 17;
        case 18: case 39: return 18;
        case 19: case 40: return 19;
        default: return typeId;
    }
}

bool typeMatches(int configuredType, const IecDataValue& value) {
    if (value.functionType >= 0) {
        // IEC 103: ASDU 1 and 2 both carry the DPI of a FUN/INF; ASDU 44 points accept them too.
        const bool valueIsMessage = value.typeId == 1 || value.typeId == 2;
        if (configuredType == 44) {
            return valueIsMessage || value.typeId == 44;
        }
        if (configuredType == 1 || configuredType == 2) {
            return valueIsMessage;
        }
        return configuredType == value.typeId;
    }
    return iec104TypeFamily(configuredType) == iec104TypeFamily(value.typeId);
}

}  // namespace

std::vector<std::uint8_t> IecCodec::buildIec104StartDtAct() {
    return {0x68, 0x04, 0x07, 0x00, 0x00, 0x00};
}

std::vector<std::uint8_t> IecCodec::buildIec104StopDtAct() {
    return {0x68, 0x04, 0x13, 0x00, 0x00, 0x00};
}

std::vector<std::uint8_t> IecCodec::buildIec104TestFrAct() {
    return {0x68, 0x04, 0x43, 0x00, 0x00, 0x00};
}

std::vector<std::uint8_t> IecCodec::buildIec104SFrame(std::uint16_t receiveSequence) {
    const auto recv = static_cast<std::uint16_t>(receiveSequence << 1);
    return {
        0x68,
        0x04,
        0x01,
        0x00,
        static_cast<std::uint8_t>(recv & 0xFF),
        static_cast<std::uint8_t>((recv >> 8) & 0xFF)
    };
}

std::vector<std::uint8_t> IecCodec::buildIec104InterrogationCommand(
    const IecProtocolConfig& config,
    std::uint16_t sendSequence,
    std::uint16_t receiveSequence
) {
    return buildIec104IFrame(buildIec101InterrogationAsdu(config), sendSequence, receiveSequence);
}

std::vector<std::uint8_t> IecCodec::buildIec104CounterInterrogationCommand(
    const IecProtocolConfig& config,
    std::uint16_t sendSequence,
    std::uint16_t receiveSequence
) {
    return buildIec104IFrame(buildIec101CounterInterrogationAsdu(config), sendSequence, receiveSequence);
}

std::vector<std::uint8_t> IecCodec::buildIec101InterrogationAsdu(const IecProtocolConfig& config) {
    std::vector<std::uint8_t> asdu;
    appendAsduHeader(asdu, 100, kIecCauseActivation, config.commonAddress, config);
    appendLe(asdu, 0, config.ioaSize);
    asdu.push_back(static_cast<std::uint8_t>(config.interrogationQualifier));
    return asdu;
}

std::vector<std::uint8_t> IecCodec::buildIec101CounterInterrogationAsdu(const IecProtocolConfig& config) {
    std::vector<std::uint8_t> asdu;
    appendAsduHeader(asdu, 101, kIecCauseActivation, config.commonAddress, config);
    appendLe(asdu, 0, config.ioaSize);
    asdu.push_back(static_cast<std::uint8_t>(config.counterInterrogationQualifier));
    return asdu;
}

std::vector<std::uint8_t> IecCodec::buildIec104ClockSyncCommand(
    const IecProtocolConfig& config,
    std::uint16_t sendSequence,
    std::uint16_t receiveSequence,
    std::int64_t unixTimeMs
) {
    std::vector<std::uint8_t> asdu;
    appendAsduHeader(asdu, 103, kIecCauseActivation, config.commonAddress, config);
    appendLe(asdu, 0, config.ioaSize);
    appendCp56Time2a(asdu, unixTimeMs, iec_detail::timeTagsUtc(config));
    return buildIec104IFrame(asdu, sendSequence, receiveSequence);
}

int IecCodec::commandTypeOf(const PointDefinition& point) {
    if (point.write.iec.typeId > 0) {
        return point.write.iec.typeId;
    }
    const auto dataType = lowercase(point.write.dataType.empty() ? point.read.dataType : point.write.dataType);
    if (dataType == "single_command" || dataType == "single_point" || dataType == "bool" || dataType == "bit") {
        return 45;
    }
    if (dataType == "double_command" || dataType == "double_point") {
        return 46;
    }
    if (dataType == "regulating_step_command") {
        return 47;
    }
    if (dataType == "setpoint_normalized") {
        return 48;
    }
    if (dataType == "setpoint_float" || dataType == "float" || dataType == "float32") {
        return 50;
    }
    if (dataType == "bitstring_command" || dataType == "bitstring32") {
        return 51;
    }
    return 49;
}

bool IecCodec::isCommandType(int typeId) {
    return (typeId >= 45 && typeId <= 51) || (typeId >= 58 && typeId <= 64);
}

bool IecCodec::commandSupportsSelect(int typeId) {
    const int untimed = untimedCommandType(typeId);
    return untimed >= 45 && untimed <= 50;
}

std::vector<std::uint8_t> IecCodec::buildIec104ControlCommand(
    const IecProtocolConfig& config,
    const PointDefinition& point,
    double requestedValue,
    std::uint16_t sendSequence,
    std::uint16_t receiveSequence,
    bool select,
    bool cancel,
    std::int64_t unixTimeMs
) {
    if (!point.write.enable) {
        throw std::invalid_argument("IEC point write is disabled");
    }
    const int typeId = commandTypeOf(point);
    if (!isCommandType(typeId)) {
        throw std::invalid_argument("unsupported IEC104 write typeId: " + std::to_string(typeId));
    }
    const int untimed = untimedCommandType(typeId);
    if (select && !commandSupportsSelect(typeId)) {
        throw std::invalid_argument("IEC104 command type " + std::to_string(typeId) + " has no select/execute");
    }
    const int ioa = commandIoaOf(point);
    if (ioa < 0) {
        throw std::invalid_argument("IEC write point missing IOA");
    }
    const double scale = std::abs(point.write.scale) > 1e-12 ? point.write.scale : 1.0;
    const double rawValue = (requestedValue - point.write.offset) / scale;

    std::vector<std::uint8_t> asdu;
    appendAsduHeader(
        asdu,
        typeId,
        cancel ? kIecCauseDeactivation : kIecCauseActivation,
        commandCommonAddressOf(config, point),
        config);
    appendLe(asdu, static_cast<std::uint32_t>(ioa), config.ioaSize);

    switch (untimed) {
        case 45: {
            const bool on = std::abs(requestedValue) > 1e-9;
            asdu.push_back(commandQualifierByte(point, untimed, select, on ? 0x01 : 0x00));
            break;
        }
        case 46:
        case 47: {
            // DCS / RCS: 1 = OFF/lower, 2 = ON/higher; 0 and 3 are not permitted.
            const int state = static_cast<int>(std::round(requestedValue));
            if ((state != 1 && state != 2) || std::abs(requestedValue - state) > 1e-9) {
                throw std::invalid_argument(std::string("IEC ") +
                    (untimed == 46 ? "double command DCS" : "regulating step RCS") + " must be 1 or 2");
            }
            asdu.push_back(commandQualifierByte(point, untimed, select, static_cast<std::uint8_t>(state)));
            break;
        }
        case 48:
            appendLe(asdu, static_cast<std::uint32_t>(static_cast<std::uint16_t>(quantizeNormalized(rawValue))), 2);
            asdu.push_back(commandQualifierByte(point, untimed, select, 0));
            break;
        case 49:
            appendLe(asdu, static_cast<std::uint32_t>(static_cast<std::uint16_t>(quantizeScaled(rawValue))), 2);
            asdu.push_back(commandQualifierByte(point, untimed, select, 0));
            break;
        case 50: {
            const auto rawFloat = static_cast<float>(rawValue);
            std::uint32_t raw = 0;
            std::memcpy(&raw, &rawFloat, sizeof(rawFloat));
            appendLe(asdu, raw, 4);
            asdu.push_back(commandQualifierByte(point, untimed, select, 0));
            break;
        }
        case 51:
            appendLe(asdu, quantizeBitstring(requestedValue), 4);
            break;
        default:
            throw std::invalid_argument("unsupported IEC104 write typeId: " + std::to_string(typeId));
    }
    if (typeId != untimed) {
        appendCp56Time2a(asdu, unixTimeMs > 0 ? unixTimeMs : iec_detail::systemNowMs(), iec_detail::timeTagsUtc(config));
    }
    return buildIec104IFrame(asdu, sendSequence, receiveSequence);
}

double IecCodec::expectedControlEchoValue(const PointDefinition& point, double requestedValue) {
    const int typeId = commandTypeOf(point);
    const double scale = std::abs(point.write.scale) > 1e-12 ? point.write.scale : 1.0;
    const double rawValue = (requestedValue - point.write.offset) / scale;
    switch (untimedCommandType(typeId)) {
        case 45:
            return std::abs(requestedValue) > 1e-9 ? 1.0 : 0.0;
        case 46:
        case 47:
            return static_cast<double>(static_cast<int>(std::round(requestedValue)) & 0x03);
        case 48:
            return static_cast<double>(quantizeNormalized(rawValue)) / 32768.0;
        case 49:
            return static_cast<double>(quantizeScaled(rawValue));
        case 50:
            return static_cast<double>(static_cast<float>(rawValue));
        case 51:
            return static_cast<double>(quantizeBitstring(requestedValue));
        default:
            throw std::invalid_argument("unsupported IEC104 write typeId: " + std::to_string(typeId));
    }
}

std::vector<std::uint8_t> IecCodec::buildIec104ParameterCommand(
    const IecProtocolConfig& config,
    int ioa,
    int typeId,
    double value,
    std::uint8_t qualifier,
    std::uint16_t sendSequence,
    std::uint16_t receiveSequence,
    int cause
) {
    if (typeId < 110 || typeId > 112) {
        throw std::invalid_argument("IEC104 parameter typeId must be 110, 111 or 112");
    }
    std::vector<std::uint8_t> asdu;
    appendAsduHeader(asdu, typeId, cause, config.commonAddress, config);
    appendLe(asdu, static_cast<std::uint32_t>(ioa), config.ioaSize);
    switch (typeId) {
        case 110:
            appendLe(asdu, static_cast<std::uint32_t>(static_cast<std::uint16_t>(quantizeNormalized(value))), 2);
            asdu.push_back(qualifier);
            break;
        case 111:
            appendLe(asdu, static_cast<std::uint32_t>(static_cast<std::uint16_t>(quantizeScaled(value))), 2);
            asdu.push_back(qualifier);
            break;
        case 112: {
            const auto rawFloat = static_cast<float>(value);
            std::uint32_t raw = 0;
            std::memcpy(&raw, &rawFloat, sizeof(rawFloat));
            appendLe(asdu, raw, 4);
            asdu.push_back(qualifier);
            break;
        }
        default:
            break;
    }
    return buildIec104IFrame(asdu, sendSequence, receiveSequence);
}

std::vector<std::uint8_t> IecCodec::buildIec104ParameterActivationCommand(
    const IecProtocolConfig& config,
    int ioa,
    std::uint8_t qualifier,
    std::uint16_t sendSequence,
    std::uint16_t receiveSequence
) {
    std::vector<std::uint8_t> asdu;
    appendAsduHeader(asdu, 113, kIecCauseActivation, config.commonAddress, config);
    appendLe(asdu, static_cast<std::uint32_t>(ioa), config.ioaSize);
    asdu.push_back(qualifier);
    return buildIec104IFrame(asdu, sendSequence, receiveSequence);
}

std::vector<std::uint8_t> IecCodec::buildIec104FileCallCommand(
    const IecProtocolConfig& config,
    int ioa,
    int nameOfFile,
    int nameOfSection,
    std::uint8_t qualifier,
    std::uint16_t sendSequence,
    std::uint16_t receiveSequence
) {
    std::vector<std::uint8_t> asdu;
    appendAsduHeader(asdu, 122, 13, config.commonAddress, config);
    appendLe(asdu, static_cast<std::uint32_t>(ioa), config.ioaSize);
    appendLe(asdu, static_cast<std::uint32_t>(nameOfFile), 2);
    asdu.push_back(static_cast<std::uint8_t>(nameOfSection & 0xFF));
    asdu.push_back(qualifier);
    return buildIec104IFrame(asdu, sendSequence, receiveSequence);
}

std::vector<std::uint8_t> IecCodec::buildIec104FileAckCommand(
    const IecProtocolConfig& config,
    int ioa,
    int nameOfFile,
    int nameOfSection,
    std::uint8_t qualifier,
    std::uint16_t sendSequence,
    std::uint16_t receiveSequence
) {
    std::vector<std::uint8_t> asdu;
    appendAsduHeader(asdu, 124, 13, config.commonAddress, config);
    appendLe(asdu, static_cast<std::uint32_t>(ioa), config.ioaSize);
    appendLe(asdu, static_cast<std::uint32_t>(nameOfFile), 2);
    asdu.push_back(static_cast<std::uint8_t>(nameOfSection & 0xFF));
    asdu.push_back(qualifier);
    return buildIec104IFrame(asdu, sendSequence, receiveSequence);
}

bool IecCodec::isIec104Frame(const std::vector<std::uint8_t>& frame) {
    return frame.size() >= 6 && frame[0] == 0x68 && frame[1] >= 4 && frame[1] <= 253 &&
        frame[1] == frame.size() - 2;
}

bool IecCodec::isIec104IFrame(const std::vector<std::uint8_t>& frame) {
    return isIec104Frame(frame) && (frame[2] & 0x01U) == 0;
}

bool IecCodec::isIec104SFrame(const std::vector<std::uint8_t>& frame) {
    return isIec104Frame(frame) && frame.size() == 6 && (frame[2] & 0x03U) == 0x01;
}

bool IecCodec::isIec104UFrame(const std::vector<std::uint8_t>& frame) {
    return isIec104Frame(frame) && frame.size() == 6 && (frame[2] & 0x03U) == 0x03;
}

bool IecCodec::isIec104StartDtCon(const std::vector<std::uint8_t>& frame) {
    return frame == std::vector<std::uint8_t>{0x68, 0x04, 0x0B, 0x00, 0x00, 0x00};
}

bool IecCodec::isIec104StopDtCon(const std::vector<std::uint8_t>& frame) {
    return frame == std::vector<std::uint8_t>{0x68, 0x04, 0x23, 0x00, 0x00, 0x00};
}

bool IecCodec::isIec104TestFrAct(const std::vector<std::uint8_t>& frame) {
    return frame == std::vector<std::uint8_t>{0x68, 0x04, 0x43, 0x00, 0x00, 0x00};
}

bool IecCodec::isIec104TestFrCon(const std::vector<std::uint8_t>& frame) {
    return frame == std::vector<std::uint8_t>{0x68, 0x04, 0x83, 0x00, 0x00, 0x00};
}

std::uint16_t IecCodec::iec104SendSequence(const std::vector<std::uint8_t>& frame) {
    if (!isIec104IFrame(frame)) {
        return 0;
    }
    return static_cast<std::uint16_t>((readLe(frame, 2, 2) >> 1) & 0x7FFF);
}

std::uint16_t IecCodec::iec104ReceiveSequence(const std::vector<std::uint8_t>& frame) {
    if (!isIec104IFrame(frame) && !isIec104SFrame(frame)) {
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

IecAsdu IecCodec::decodeAsdu(const std::vector<std::uint8_t>& asdu, const IecProtocolConfig& config, std::int64_t nowMs) {
    if (asdu.size() < static_cast<std::size_t>(2 + config.cotSize + config.caSize)) {
        throw std::runtime_error("IEC ASDU too short");
    }
    const auto time = iec_detail::timeTagContext(config, nowMs);
    IecAsdu result;
    result.typeId = asdu[0];
    const auto vsq = asdu[1];
    const auto count = static_cast<int>(vsq & 0x7FU);
    const bool sequential = (vsq & 0x80U) != 0;
    std::size_t offset = 2;
    const int rawCause = readLe(asdu, offset, config.cotSize);
    result.cause = rawCause & 0x3F;
    result.negative = (rawCause & 0x40) != 0;
    result.test = (rawCause & 0x80) != 0;
    offset += static_cast<std::size_t>(config.cotSize);
    result.commonAddress = readLe(asdu, offset, config.caSize);
    offset += static_cast<std::size_t>(config.caSize);

    if (result.typeId == 100 || result.typeId == 101) {
        // QOI / QCC of a mirrored system command.
        const auto qualifierOffset = offset + static_cast<std::size_t>(config.ioaSize);
        if (qualifierOffset < asdu.size()) {
            result.commandQualifier = asdu[qualifierOffset];
        }
        return result;
    }
    const auto payloadSize = typePayloadSize(result.typeId);
    const bool stepPosition = result.typeId == 5 || result.typeId == 6 || result.typeId == 32;
    if (isParameterType(result.typeId)) {
        for (int i = 0; i < count; ++i) {
            if (offset + static_cast<std::size_t>(config.ioaSize) > asdu.size()) {
                break;
            }
            const int ioa = readLe(asdu, offset, config.ioaSize);
            offset += static_cast<std::size_t>(config.ioaSize);
            const auto begin = offset;
            result.parameters.push_back(decodeParameterObject(asdu, offset, result.typeId, result.cause, result.commonAddress, ioa));
            result.parameters.back().negative = result.negative;
            offset = begin + (result.typeId == 112 ? 5 : result.typeId == 113 ? 1 : 3);
            if (offset > asdu.size()) {
                break;
            }
        }
        return result;
    }
    if (isFileType(result.typeId)) {
        for (int i = 0; i < count; ++i) {
            if (offset + static_cast<std::size_t>(config.ioaSize) > asdu.size()) {
                break;
            }
            const int ioa = readLe(asdu, offset, config.ioaSize);
            offset += static_cast<std::size_t>(config.ioaSize);
            const auto begin = offset;
            auto segment = decodeFileObject(asdu, offset, result.typeId, result.cause, result.commonAddress, ioa);
            result.fileSegments.push_back(std::move(segment));
            if (result.typeId == 125) {
                offset = begin + 5 + result.fileSegments.back().data.size();
            } else if (result.typeId == 121) {
                offset = begin + 6;
            } else if (result.typeId == 120) {
                offset = begin + 5;
            } else if (result.typeId == 122 || result.typeId == 124 || result.typeId == 123 || result.typeId == 126) {
                offset = begin + 4;
            }
            if (offset > asdu.size()) {
                break;
            }
        }
        return result;
    }
    if (payloadSize == 0 || count <= 0) {
        return result;
    }
    const auto decodeObject = [&](std::size_t objectOffset, int ioa) {
        if (isProtectionType(result.typeId)) {
            result.protectionEvents.push_back(decodeProtectionEvent(
                asdu, objectOffset, result.typeId, result.cause, result.commonAddress, ioa, time));
        } else {
            decodeInformationObject(
                result.values, asdu, objectOffset, result.typeId, result.cause, result.commonAddress, ioa, time);
        }
    };
    if (sequential) {
        const int baseIoa = readLe(asdu, offset, config.ioaSize);
        offset += static_cast<std::size_t>(config.ioaSize);
        for (int i = 0; i < count; ++i) {
            if (offset + payloadSize > asdu.size()) {
                if (stepPosition) throw std::runtime_error("IEC step-position object is truncated");
                break;
            }
            decodeObject(offset, baseIoa + i);
            offset += payloadSize;
        }
    } else {
        for (int i = 0; i < count; ++i) {
            if (offset + static_cast<std::size_t>(config.ioaSize) + payloadSize > asdu.size()) {
                if (stepPosition) throw std::runtime_error("IEC step-position object is truncated");
                break;
            }
            const int ioa = readLe(asdu, offset, config.ioaSize);
            offset += static_cast<std::size_t>(config.ioaSize);
            decodeObject(offset, ioa);
            offset += payloadSize;
        }
    }
    for (auto& value : result.values) {
        value.negative = result.negative;
        value.test = result.test;
    }
    for (auto& event : result.protectionEvents) {
        event.test = result.test;
    }
    return result;
}

IecDataValue IecCodec::protectionEventValue(const IecProtectionEvent& event) {
    IecDataValue value;
    value.commonAddress = event.commonAddress;
    value.ioa = event.ioa;
    value.typeId = event.typeId;
    value.cause = event.cause;
    value.value = static_cast<double>(event.eventState);
    value.text = std::to_string(event.eventState);
    value.doublePoint = event.typeId == 17 || event.typeId == 38;
    value.quality = event.quality;
    value.qualityMsg = event.qualityMsg;
    value.timestampMs = event.timestampMs;
    value.test = event.test;
    value.rawHex = event.rawHex;
    return value;
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
    if (spec.typeId > 0 && !typeMatches(spec.typeId, value)) {
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
    if (spec.elementIndex != value.elementIndex) {
        return false;
    }
    return true;
}

bool IecCodec::holdsPreviousValue(const PointDefinition& point, const IecDataValue& value) {
    if (!value.doublePoint || !isBooleanDataType(pointDataType(point))) {
        return false;
    }
    const int dpi = static_cast<int>(value.value) & 0x03;
    return dpi == 0 || dpi == 3;
}

DecodedValue IecCodec::decodePointValue(const PointDefinition& point, const IecDataValue& value) {
    DecodedValue decoded;
    const auto dataType = pointDataType(point);
    double raw = value.value;
    std::string text = value.text;
    if (isBooleanDataType(dataType)) {
        if (value.doublePoint) {
            // Mapping adopted by the code: DPI 1 -> 0 (off), 2 -> 1 (on); DPI 0/3 is held by the
            // collector (holdsPreviousValue). IEC 101/104 define these meanings; for IEC 103 /
            // AM5SE change frames the firmware meaning is unverified (see decodeIec103Data).
            const int dpi = static_cast<int>(value.value) & 0x03;
            raw = dpi == 2 ? 1.0 : 0.0;
            text = dpi == 2 ? "on" : dpi == 1 ? "off" : "no on/off mapping";
        } else {
            raw = value.value > 0.0 ? value.value : 0.0;
        }
    } else if (dataType == "double_point") {
        raw = value.value > 0.0 ? value.value : 0.0;
    }
    decoded.value = raw * point.read.scale + point.read.offset;
    decoded.text = text.empty() ? std::to_string(decoded.value) : text;
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
