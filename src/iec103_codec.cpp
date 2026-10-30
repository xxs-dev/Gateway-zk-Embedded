// IEC 60870-5-103 application layer: monitoring ASDUs, general interrogation and the AM5SE
// disturbance-recording (COMTRADE) ASDUs. FT1.2 link framing lives in iec_ft12_link.cpp.
#include "edge_gateway/iec_codec.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "iec_codec_detail.hpp"

namespace edge_gateway {

using iec_detail::appendCp56Time2a;
using iec_detail::appendLe;
using iec_detail::decodeCp32Time2a;
using iec_detail::decodeCp56Time2a;
using iec_detail::markBad;
using iec_detail::readFloatLe;
using iec_detail::readInt16Le;
using iec_detail::readInt32Le;
using iec_detail::readLe;
using iec_detail::readUnsignedLe;

namespace {

// Fixed IEC 103 ASDU header: TYPE, VSQ, COT, CA (one octet each), then FUN, INF.
constexpr std::size_t kHeaderSize = 4;
constexpr int kIec103CauseTestMode = 7;

double decodeGenericValue(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    int dataType,
    int dataSize
) {
    if (dataSize <= 0 || dataSize > 8 || offset + static_cast<std::size_t>(dataSize) > bytes.size()) {
        throw std::runtime_error("IEC103 generic value is truncated");
    }
    if (dataType == 7 && dataSize == 4) {
        return static_cast<double>(readFloatLe(bytes, offset));
    }
    if (dataType == 3) {
        if (dataSize == 1) {
            return static_cast<double>(static_cast<std::int8_t>(bytes[offset]));
        }
        if (dataSize == 2) {
            return static_cast<double>(readInt16Le(bytes, offset));
        }
        if (dataSize == 4) {
            return static_cast<double>(readInt32Le(bytes, offset));
        }
    }
    return static_cast<double>(readUnsignedLe(bytes, offset, std::min(dataSize, 4)));
}

IecDataValue baseValue(const std::vector<std::uint8_t>& asdu, int functionType, int informationNumber) {
    IecDataValue value;
    value.typeId = asdu[0];
    value.cause = asdu[2];
    value.test = asdu[2] == kIec103CauseTestMode;
    value.commonAddress = asdu[3];
    value.functionType = functionType;
    value.informationNumber = informationNumber;
    value.ioa = functionType * 256 + informationNumber;
    value.rawHex = IecCodec::toHex(asdu);
    return value;
}

std::string hexOctets(unsigned value, int octets) {
    static const char digits[] = "0123456789ABCDEF";
    std::string text = "0x";
    for (int nibble = octets * 2 - 1; nibble >= 0; --nibble) {
        text += digits[(value >> (nibble * 4)) & 0x0FU];
    }
    return text;
}

// COT spelled out in diagnostics, so that a general-interrogation answer (COT 9) is never read
// as a spontaneous change (COT 1).
std::string causeLabel(int cause) {
    switch (cause) {
        case 1: return "1(spontaneous)";
        case 2: return "2(cyclic)";
        case kIec103CauseTestMode: return "7(test)";
        case 9: return "9(gi)";
        case 11: return "11(local-op)";
        case 12: return "12(remote-op)";
        default: return std::to_string(cause);
    }
}

std::string evidenceHeader(const IecDataValue& value) {
    return "asdu=" + std::to_string(value.typeId) + " cot=" + causeLabel(value.cause) +
        " ca=" + std::to_string(value.commonAddress) + " fun=" + std::to_string(value.functionType) +
        " inf=" + std::to_string(value.informationNumber);
}

// ASDU 1 / 2: FUN, INF, DPI, (RET, FAN,) CP32Time2a, SIN. value holds DPI 0..3 and rawDpiOctet
// the whole octet as received; the collector maps it per IEC 60870-5-103 (1 -> off/0,
// 2 -> on/1; 0/3 and octets with spare bits 3..8 set -> bad quality, previous value held).
// For AM5SE this is the mapping the code adopts, NOT a verified property of the firmware: the
// actual meaning of DPI in its change frames still has to be confirmed by the vendor, or by
// comparing raw frames (GATEWAY_IEC103_DEBUG / channel trace) with the real breaker position on
// site. value.evidence keeps ASDU/COT/CA/FUN/INF and the raw octet for that comparison.
void decodeTimeTaggedMessage(
    std::vector<IecDataValue>& out,
    const std::vector<std::uint8_t>& asdu,
    const iec_detail::TimeTagContext& time
) {
    const bool relativeTime = asdu[0] == 2;
    const std::size_t timeOffset = kHeaderSize + 3 + (relativeTime ? 4 : 0);
    if (asdu.size() < timeOffset + 4 + 1) {
        throw std::runtime_error("IEC103 ASDU" + std::to_string(asdu[0]) + " is truncated");
    }
    auto value = baseValue(asdu, asdu[kHeaderSize], asdu[kHeaderSize + 1]);
    const int octet = asdu[kHeaderSize + 2];
    const int dpi = octet & 0x03;
    value.rawDpiOctet = octet;
    value.value = static_cast<double>(dpi);
    value.text = std::to_string(dpi);
    value.doublePoint = true;
    value.timestampMs = decodeCp32Time2a(asdu, timeOffset, time);
    value.evidence = evidenceHeader(value) + " dpiOctet=" + hexOctets(static_cast<unsigned>(octet), 1);
    if ((octet & ~0x03) != 0) {
        markBad(value, "IEC103 DPI octet " + hexOctets(static_cast<unsigned>(octet), 1) +
            " has spare bits 3..8 set; not an on/off value");
    } else if (dpi == 0 || dpi == 3) {
        markBad(value, "IEC103 DPI=" + std::to_string(dpi) + " has no on/off mapping");
    }
    out.push_back(std::move(value));
}

// ASDU 3 / 9: MEA = 13-bit two's complement in bits 3..15 (normalized to +-1), ER 0x02, OV 0x01.
void decodeMeasurands(std::vector<IecDataValue>& out, const std::vector<std::uint8_t>& asdu) {
    const std::size_t first = kHeaderSize + 2;
    if (asdu.size() < first + 2) {
        throw std::runtime_error("IEC103 ASDU" + std::to_string(asdu[0]) + " has no measurand");
    }
    std::size_t count = (asdu.size() - first) / 2;
    if (asdu[0] == 9) {
        const auto declared = static_cast<std::size_t>(asdu[1] & 0x7FU);
        if (declared == 0 || first + declared * 2 > asdu.size()) {
            throw std::runtime_error("IEC103 ASDU9 measurand count does not match its length");
        }
        count = declared;
    } else {
        count = std::min<std::size_t>(count, 4);
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto offset = first + i * 2;
        const auto raw = static_cast<std::uint16_t>(readUnsignedLe(asdu, offset, 2));
        auto value = baseValue(asdu, asdu[kHeaderSize], asdu[kHeaderSize + 1]);
        value.elementIndex = static_cast<int>(i);
        std::int16_t signedRaw = 0;
        const auto masked = static_cast<std::uint16_t>(raw & 0xFFF8U);
        std::memcpy(&signedRaw, &masked, sizeof(signedRaw));
        value.value = static_cast<double>(signedRaw / 8) / 4096.0;
        value.text = std::to_string(value.value);
        if ((raw & 0x02U) != 0) {
            markBad(value, "IEC103 MEA ER (invalid)");
        }
        if ((raw & 0x01U) != 0) {
            markBad(value, "IEC103 MEA OV (overflow)");
        }
        out.push_back(std::move(value));
    }
}

// ASDU 4: SCL (fault location, R32.23) + RET + FAN + CP32Time2a.
void decodeFaultLocation(
    std::vector<IecDataValue>& out,
    const std::vector<std::uint8_t>& asdu,
    const iec_detail::TimeTagContext& time
) {
    const std::size_t sclOffset = kHeaderSize + 2;
    if (asdu.size() < sclOffset + 4 + 4 + 4) {
        throw std::runtime_error("IEC103 ASDU4 is truncated");
    }
    auto value = baseValue(asdu, asdu[kHeaderSize], asdu[kHeaderSize + 1]);
    value.value = static_cast<double>(readFloatLe(asdu, sclOffset));
    value.text = std::to_string(value.value);
    value.timestampMs = decodeCp32Time2a(asdu, sclOffset + 8, time);
    if (!std::isfinite(value.value)) {
        markBad(value, "IEC103 SCL is not finite");
    }
    out.push_back(std::move(value));
}

// ASDU 44 (AM5SE signal blocks): FUN, base INF, per block 16 state bits + 16 change bits + QDS.
// The field verification only saw ASDU 44 as the GI answer (COT 9); its state bits were never
// compared with the real switch positions. value.evidence names block, bit, the status/change
// words and the QDS octet so that GI snapshots stay distinguishable from ASDU 1/2 change frames.
void decodeSignalBlocks(std::vector<IecDataValue>& out, const std::vector<std::uint8_t>& asdu) {
    if (asdu.size() < kHeaderSize + 2) {
        return;
    }
    const int count = std::max(1, static_cast<int>(asdu[1] & 0x7FU));
    const int functionType = asdu[kHeaderSize];
    const int baseInformationNumber = asdu[kHeaderSize + 1];
    std::size_t offset = kHeaderSize + 2;
    for (int block = 0; block < count; ++block) {
        if (offset + 5 > asdu.size()) {
            throw std::runtime_error("IEC103 ASDU44 information element is truncated");
        }
        const auto states = static_cast<std::uint16_t>(readUnsignedLe(asdu, offset, 2));
        const auto changes = readUnsignedLe(asdu, offset + 2, 2);
        const auto qds = static_cast<unsigned>(asdu[offset + 4]);
        for (int bit = 0; bit < 16; ++bit) {
            auto value = baseValue(asdu, functionType, baseInformationNumber + block * 16 + bit);
            value.value = (states & (static_cast<std::uint16_t>(1U) << bit)) != 0 ? 1.0 : 0.0;
            value.text = value.value > 0.0 ? "1" : "0";
            value.evidence = evidenceHeader(value) + " block=" + std::to_string(block) +
                " bitIndex=" + std::to_string(bit) + " status=" + hexOctets(states, 2) +
                " change=" + hexOctets(changes, 2) + " qds=" + hexOctets(qds, 1);
            out.push_back(std::move(value));
        }
        offset += 5;
    }
}

// ASDU 10 generic data (AM5SE measurands): group 0 entries map to INF 139 + entry.
void decodeGenericData(
    std::vector<IecDataValue>& out,
    const std::vector<std::uint8_t>& asdu,
    const IecProtocolConfig& config
) {
    std::size_t offset = kHeaderSize;
    if (offset + 4 > asdu.size()) {
        return;
    }
    offset += 2;  // Generic classification FUN/INF (normally FE/F1).
    offset += 1;  // Return information identifier.
    const int genericCount = static_cast<int>(asdu[offset++] & 0x3FU);
    for (int i = 0; i < genericCount; ++i) {
        if (offset + 6 > asdu.size()) {
            throw std::runtime_error("IEC103 ASDU10 generic identifier is truncated");
        }
        const int group = asdu[offset++];
        const int entry = asdu[offset++];
        offset += 1;  // KOD.
        const int dataType = asdu[offset++];
        const int dataSize = asdu[offset++];
        const int number = asdu[offset++] & 0x7F;
        const auto payloadBytes = static_cast<std::size_t>(dataSize) * static_cast<std::size_t>(number);
        if (number <= 0 || offset + payloadBytes > asdu.size()) {
            throw std::runtime_error("IEC103 ASDU10 generic value is truncated");
        }
        if (group == 0 && entry > 0) {
            auto value = baseValue(asdu, config.deviceFunctionType, 139 + entry);
            value.value = decodeGenericValue(asdu, offset, dataType, dataSize);
            value.text = std::to_string(value.value);
            out.push_back(std::move(value));
        }
        offset += payloadBytes;
    }
}

}  // namespace

Iec103ComtradeAssembler::Iec103ComtradeAssembler(std::size_t maxBytes)
    : maxBytes_(std::max<std::size_t>(1U, maxBytes)) {
}

void Iec103ComtradeAssembler::add(const Iec103ComtradeChunk& chunk) {
    if (chunk.fileType != 0x51 && chunk.fileType != 0x52) {
        throw std::invalid_argument("IEC103 COMTRADE file type must be 0x51 or 0x52");
    }
    if (fileType_ == 0) {
        fileType_ = chunk.fileType;
    } else if (fileType_ != chunk.fileType) {
        throw std::runtime_error("IEC103 COMTRADE file type changed during transfer");
    }
    if (chunk.packetNumber < 0) {
        throw std::runtime_error("IEC103 COMTRADE packet number is negative");
    }
    if (chunk.packetNumber < nextPacketNumber_) {
        const auto index = static_cast<std::size_t>(chunk.packetNumber);
        if (index >= packets_.size() || packets_[index] != chunk.data) {
            throw std::runtime_error("IEC103 COMTRADE conflicting duplicate packet");
        }
        return;
    }
    if (complete_) {
        throw std::runtime_error("IEC103 COMTRADE packet received after final packet");
    }
    if (chunk.packetNumber != nextPacketNumber_) {
        throw std::runtime_error(
            "IEC103 COMTRADE packet gap: expected " + std::to_string(nextPacketNumber_) +
            " but received " + std::to_string(chunk.packetNumber));
    }
    if (chunk.data.size() > maxBytes_ - std::min(maxBytes_, bytes_.size())) {
        throw std::runtime_error("IEC103 COMTRADE file exceeds configured size limit");
    }
    packets_.push_back(chunk.data);
    bytes_.insert(bytes_.end(), chunk.data.begin(), chunk.data.end());
    ++nextPacketNumber_;
    complete_ = chunk.lastPacket;
}

bool Iec103ComtradeAssembler::complete() const {
    return complete_;
}

int Iec103ComtradeAssembler::fileType() const {
    return fileType_;
}

const std::vector<std::uint8_t>& Iec103ComtradeAssembler::bytes() const {
    return bytes_;
}

std::vector<std::uint8_t> IecCodec::buildIec103GeneralInterrogationAsdu(const IecProtocolConfig& config) {
    return {
        7, 0x81, 9,
        static_cast<std::uint8_t>(config.commonAddress & 0xFF),
        0xFF, 0x00,
        static_cast<std::uint8_t>(config.returnInformationIdentifier & 0xFF)
    };
}

std::vector<std::uint8_t> IecCodec::buildIec103RecordingDirectoryAsdu(const IecProtocolConfig& config) {
    return {
        0x18, 0x81, 0x1F,
        static_cast<std::uint8_t>(config.commonAddress & 0xFF),
        0xFF, 0x00, 0x18, 0x01, 0x00, 0x00, 0x00
    };
}

std::vector<std::uint8_t> IecCodec::buildIec103ComtradeFileCallAsdu(
    const IecProtocolConfig& config,
    int fan,
    int fileType
) {
    if (fan < 0 || fan > 0xFFFF) {
        throw std::invalid_argument("IEC103 disturbance FAN must be between 0 and 65535");
    }
    if (fileType != 0x51 && fileType != 0x52) {
        throw std::invalid_argument("IEC103 COMTRADE file type must be 0x51 or 0x52");
    }
    std::vector<std::uint8_t> asdu = {
        0x44, 0x81, 0x00,
        static_cast<std::uint8_t>(config.commonAddress & 0xFF),
        0xFF, static_cast<std::uint8_t>(fileType)
    };
    appendLe(asdu, static_cast<std::uint32_t>(fan), 2);
    return asdu;
}

std::vector<Iec103DisturbanceRecord> IecCodec::decodeIec103RecordingDirectory(
    const std::vector<std::uint8_t>& asdu,
    const IecProtocolConfig& config
) {
    if (asdu.size() < 6 || asdu[0] != 0x17) {
        return {};
    }
    const int count = static_cast<int>(asdu[1] & 0x7FU);
    if ((asdu[2] & 0x3F) != 0x1F) {
        return {};
    }
    std::size_t offset = 6;
    std::vector<Iec103DisturbanceRecord> records;
    records.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        if (offset + 10 > asdu.size()) {
            throw std::runtime_error("IEC103 ASDU23 directory entry is truncated");
        }
        Iec103DisturbanceRecord record;
        record.fan = readLe(asdu, offset, 2);
        record.state = asdu[offset + 2];
        record.timestampMs = decodeCp56Time2a(asdu, offset + 3, iec_detail::timeTagsUtc(config));
        record.rawTimeHex = toHex(std::vector<std::uint8_t>(
            asdu.begin() + static_cast<std::ptrdiff_t>(offset + 3),
            asdu.begin() + static_cast<std::ptrdiff_t>(offset + 10)));
        records.push_back(std::move(record));
        offset += 10;
    }
    return records;
}

Iec103ComtradeChunk IecCodec::decodeIec103ComtradeChunk(const std::vector<std::uint8_t>& asdu) {
    if (asdu.size() < 8 || asdu[0] != 0x50) {
        throw std::runtime_error("IEC103 response is not an ASDU80 COMTRADE packet");
    }
    const int cause = asdu[2] & 0x3F;
    if (cause != 0x14) {
        throw std::runtime_error("IEC103 ASDU80 has unexpected cause " + std::to_string(cause));
    }
    const auto sequenceLow = asdu[6];
    const auto sequenceHigh = asdu[7];
    Iec103ComtradeChunk chunk;
    chunk.fileType = asdu[5];
    chunk.packetNumber = static_cast<int>(sequenceLow) | (static_cast<int>(sequenceHigh & 0x7FU) << 8);
    chunk.lastPacket = (sequenceHigh & 0x80U) != 0;
    chunk.data.assign(asdu.begin() + 8, asdu.end());
    if (chunk.data.size() > 240U) {
        throw std::runtime_error("IEC103 ASDU80 payload exceeds 240 bytes");
    }
    chunk.rawHex = toHex(asdu);
    return chunk;
}

std::vector<std::uint8_t> IecCodec::buildAm5seUdpDiscoveryPacket(
    const IecProtocolConfig& config,
    std::int64_t unixTimeMs
) {
    std::vector<std::uint8_t> packet(41, 0);
    packet[0] = 0xFF;
    packet[1] = 0x01;
    std::vector<std::uint8_t> time;
    appendCp56Time2a(time, unixTimeMs, iec_detail::timeTagsUtc(config));
    std::copy(time.begin(), time.end(), packet.begin() + 2);
    const auto stationBytes = std::min<std::size_t>(16U, config.stationName.size());
    std::copy_n(config.stationName.begin(), stationBytes, packet.begin() + 9);
    return packet;
}

std::vector<IecDataValue> IecCodec::decodeIec103Data(
    const std::vector<std::uint8_t>& asdu,
    const IecProtocolConfig& config,
    std::int64_t nowMs
) {
    std::vector<IecDataValue> values;
    if (asdu.size() < kHeaderSize + 2) {
        return values;
    }
    const auto time = iec_detail::timeTagContext(config, nowMs);
    switch (asdu[0]) {
        case 1:
        case 2:
            decodeTimeTaggedMessage(values, asdu, time);
            break;
        case 3:
        case 9:
            decodeMeasurands(values, asdu);
            break;
        case 4:
            decodeFaultLocation(values, asdu, time);
            break;
        case 10:
            decodeGenericData(values, asdu, config);
            break;
        case 44:
            decodeSignalBlocks(values, asdu);
            break;
        default:
            // 5 identification, 6 time sync, 8 GI termination, 23/80 recording: no point values.
            break;
    }
    return values;
}

}  // namespace edge_gateway
