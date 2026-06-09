#include "edge_gateway/iec_codec.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
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
        case 2: return 4;   // M_SP_TA_1
        case 3: return 1;   // M_DP_NA_1
        case 4: return 4;   // M_DP_TA_1
        case 5: return 3;   // M_ST_NA_1
        case 6: return 6;   // M_ST_TA_1
        case 7: return 5;   // M_BO_NA_1
        case 8: return 8;   // M_BO_TA_1
        case 9: return 3;   // M_ME_NA_1
        case 10: return 6;  // M_ME_TA_1
        case 11: return 3;  // M_ME_NB_1
        case 12: return 6;  // M_ME_TB_1
        case 13: return 5;  // M_ME_NC_1
        case 14: return 8;  // M_ME_TC_1
        case 15: return 5;  // M_IT_NA_1
        case 16: return 8;  // M_IT_TA_1
        case 20: return 1;  // M_PS_NA_1
        case 21: return 2;  // M_ME_ND_1
        case 30: return 8;  // M_SP_TB_1
        case 31: return 8;  // M_DP_TB_1
        case 32: return 10; // M_ST_TB_1
        case 33: return 12; // M_BO_TB_1
        case 34: return 10; // M_ME_TD_1
        case 35: return 10; // M_ME_TE_1
        case 36: return 12; // M_ME_TF_1
        case 37: return 12; // M_IT_TB_1
        case 45: return 1;  // C_SC_NA_1
        case 46: return 1;  // C_DC_NA_1
        case 48: return 3;  // C_SE_NA_1
        case 49: return 3;  // C_SE_NB_1
        case 50: return 5;  // C_SE_NC_1
        default:
            return 0;
    }
}

void appendCot(std::vector<std::uint8_t>& bytes, int cause, const IecProtocolConfig& config) {
    if (config.cotSize <= 1) {
        appendLe(bytes, cause & 0x3F, 1);
        return;
    }
    appendLe(bytes, (cause & 0x3F) | ((config.originatorAddress & 0xFF) << 8), config.cotSize);
}

bool hasCp56Time(int typeId) {
    return typeId >= 30 && typeId <= 40;
}

bool hasCp24Time(int typeId) {
    return (typeId >= 2 && typeId <= 16 && (typeId % 2) == 0);
}

std::size_t valuePayloadSize(int typeId) {
    auto size = typePayloadSize(typeId);
    if (hasCp56Time(typeId)) {
        size -= 7;
    } else if (hasCp24Time(typeId)) {
        size -= 3;
    }
    return size;
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

void appendCp56Time2a(std::vector<std::uint8_t>& bytes, std::int64_t unixTimeMs) {
    using namespace std::chrono;
    const auto millis = milliseconds(unixTimeMs);
    const auto secondsPoint = system_clock::time_point(millis);
    const std::time_t seconds = system_clock::to_time_t(secondsPoint);
#ifdef _WIN32
    std::tm tmValue;
    localtime_s(&tmValue, &seconds);
    const std::tm* tm = &tmValue;
#else
    std::tm tmValue;
    localtime_r(&seconds, &tmValue);
    const std::tm* tm = &tmValue;
#endif
    const auto msOfMinute = static_cast<int>((unixTimeMs % 60000 + 60000) % 60000);
    appendLe(bytes, msOfMinute, 2);
    bytes.push_back(static_cast<std::uint8_t>(tm->tm_min & 0x3F));
    bytes.push_back(static_cast<std::uint8_t>(tm->tm_hour & 0x1F));
    bytes.push_back(static_cast<std::uint8_t>(((tm->tm_wday == 0 ? 7 : tm->tm_wday) << 5) | (tm->tm_mday & 0x1F)));
    bytes.push_back(static_cast<std::uint8_t>((tm->tm_mon + 1) & 0x0F));
    bytes.push_back(static_cast<std::uint8_t>((tm->tm_year % 100) & 0x7F));
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

int commandTypeOf(const PointDefinition& point) {
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
    if (dataType == "setpoint_normalized") {
        return 48;
    }
    if (dataType == "setpoint_float" || dataType == "float" || dataType == "float32") {
        return 50;
    }
    return 49;
}

std::uint8_t buildQualifier(const PointDefinition& point, bool select, std::uint8_t lowBits) {
    std::uint8_t qualifier = static_cast<std::uint8_t>(point.write.iec.qualifier & 0x7CU);
    qualifier = static_cast<std::uint8_t>(qualifier | lowBits);
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
        case 2:
        case 30:
            value.value = (bytes[payloadOffset] & 0x01U) != 0 ? 1.0 : 0.0;
            value.text = value.value > 0.0 ? "on" : "off";
            break;
        case 3:
        case 4:
        case 31:
            value.value = static_cast<double>(bytes[payloadOffset] & 0x03U);
            value.text = std::to_string(static_cast<int>(value.value));
            break;
        case 5:
        case 6:
        case 32:
            value.value = static_cast<double>(readInt16Le(bytes, payloadOffset));
            value.text = std::to_string(value.value);
            break;
        case 7:
        case 8:
        case 33:
            value.value = static_cast<double>(readInt32Le(bytes, payloadOffset));
            value.text = std::to_string(value.value);
            break;
        case 9:
        case 10:
        case 34:
            value.value = static_cast<double>(readInt16Le(bytes, payloadOffset)) / 32768.0;
            value.text = std::to_string(value.value);
            break;
        case 11:
        case 12:
        case 35:
            value.value = static_cast<double>(readInt16Le(bytes, payloadOffset));
            value.text = std::to_string(value.value);
            break;
        case 13:
        case 14:
        case 36:
            value.value = static_cast<double>(readFloatLe(bytes, payloadOffset));
            value.text = std::to_string(value.value);
            break;
        case 15:
        case 16:
        case 37:
            value.value = static_cast<double>(readInt32Le(bytes, payloadOffset));
            value.text = std::to_string(value.value);
            break;
        case 20:
            value.value = static_cast<double>(bytes[payloadOffset]);
            value.text = std::to_string(static_cast<int>(value.value));
            break;
        case 21:
            value.value = static_cast<double>(readInt16Le(bytes, payloadOffset)) / 32768.0;
            value.text = std::to_string(value.value);
            break;
        case 45:
            value.value = (bytes[payloadOffset] & 0x01U) != 0 ? 1.0 : 0.0;
            value.text = value.value > 0.0 ? "on" : "off";
            break;
        case 46:
            value.value = static_cast<double>(bytes[payloadOffset] & 0x03U);
            value.text = std::to_string(static_cast<int>(value.value));
            break;
        case 48:
            value.value = static_cast<double>(readInt16Le(bytes, payloadOffset)) / 32768.0;
            value.text = std::to_string(value.value);
            break;
        case 49:
            value.value = static_cast<double>(readInt16Le(bytes, payloadOffset));
            value.text = std::to_string(value.value);
            break;
        case 50:
            value.value = static_cast<double>(readFloatLe(bytes, payloadOffset));
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
    std::vector<std::uint8_t> asdu;
    asdu.push_back(100);
    asdu.push_back(1);
    appendCot(asdu, config.interrogationCot, config);
    appendLe(asdu, config.commonAddress, config.caSize);
    appendLe(asdu, 0, config.ioaSize);
    asdu.push_back(static_cast<std::uint8_t>(config.interrogationQualifier));

    return buildIec104IFrame(asdu, sendSequence, receiveSequence);
}

std::vector<std::uint8_t> IecCodec::buildIec104ClockSyncCommand(
    const IecProtocolConfig& config,
    std::uint16_t sendSequence,
    std::uint16_t receiveSequence,
    std::int64_t unixTimeMs
) {
    std::vector<std::uint8_t> asdu;
    asdu.push_back(103);
    asdu.push_back(1);
    appendCot(asdu, 6, config);
    appendLe(asdu, config.commonAddress, config.caSize);
    appendLe(asdu, 0, config.ioaSize);
    appendCp56Time2a(asdu, unixTimeMs);
    return buildIec104IFrame(asdu, sendSequence, receiveSequence);
}

std::vector<std::uint8_t> IecCodec::buildIec104ControlCommand(
    const IecProtocolConfig& config,
    const PointDefinition& point,
    double requestedValue,
    std::uint16_t sendSequence,
    std::uint16_t receiveSequence,
    bool select,
    bool cancel
) {
    if (!point.write.enable) {
        throw std::invalid_argument("IEC point write is disabled");
    }
    const int typeId = commandTypeOf(point);
    const int ioa = commandIoaOf(point);
    if (ioa < 0) {
        throw std::invalid_argument("IEC write point missing IOA");
    }
    const double scale = std::abs(point.write.scale) > 1e-12 ? point.write.scale : 1.0;
    const double rawValue = (requestedValue - point.write.offset) / scale;

    std::vector<std::uint8_t> asdu;
    asdu.push_back(static_cast<std::uint8_t>(typeId));
    asdu.push_back(1);
    appendCot(asdu, cancel ? 8 : 6, config);
    appendLe(asdu, commandCommonAddressOf(config, point), config.caSize);
    appendLe(asdu, ioa, config.ioaSize);

    switch (typeId) {
        case 45: {
            const bool on = std::abs(requestedValue) > 1e-9;
            asdu.push_back(buildQualifier(point, select, on ? 0x01 : 0x00));
            break;
        }
        case 46: {
            int state = static_cast<int>(std::round(requestedValue));
            if (state < 0 || state > 3) {
                throw std::invalid_argument("IEC double command value must be 0..3");
            }
            asdu.push_back(buildQualifier(point, select, static_cast<std::uint8_t>(state & 0x03)));
            break;
        }
        case 48: {
            int raw = static_cast<int>(std::round(rawValue * 32768.0));
            raw = std::max(-32768, std::min(32767, raw));
            appendLe(asdu, raw, 2);
            asdu.push_back(buildQualifier(point, select, 0));
            break;
        }
        case 49: {
            int raw = static_cast<int>(std::round(rawValue));
            raw = std::max(-32768, std::min(32767, raw));
            appendLe(asdu, raw, 2);
            asdu.push_back(buildQualifier(point, select, 0));
            break;
        }
        case 50: {
            const auto rawFloat = static_cast<float>(rawValue);
            std::uint32_t raw = 0;
            std::memcpy(&raw, &rawFloat, sizeof(rawFloat));
            appendLe(asdu, static_cast<int>(raw), 4);
            asdu.push_back(buildQualifier(point, select, 0));
            break;
        }
        default:
            throw std::invalid_argument("unsupported IEC104 write typeId: " + std::to_string(typeId));
    }
    return buildIec104IFrame(asdu, sendSequence, receiveSequence);
}

bool IecCodec::isIec104Frame(const std::vector<std::uint8_t>& frame) {
    return frame.size() >= 6 && frame[0] == 0x68 && frame[1] == frame.size() - 2;
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

std::vector<std::uint8_t> IecCodec::buildIec101InterrogationFrame(const IecProtocolConfig& config) {
    std::vector<std::uint8_t> userData;
    userData.push_back(0x73);
    appendLe(userData, config.linkAddress, config.linkAddressSize);
    userData.push_back(100);
    userData.push_back(1);
    appendCot(userData, config.interrogationCot, config);
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
