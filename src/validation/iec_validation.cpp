#include "edge_gateway/device_config_validation.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "edge_gateway/iec_codec.hpp"

namespace edge_gateway {

namespace {

void reject(const std::string& message) {
    throw std::invalid_argument(message);
}

std::int64_t maxForOctets(int octets) {
    return (static_cast<std::int64_t>(1) << (8 * octets)) - 1;
}

bool isIec103(const std::string& type) {
    return type == "iec103" || type == "iec103_tcp" || type == "iec103_serial";
}

// Monitoring ASDUs the IEC 101/104 decoder supports.
bool supportedIec104MonitorType(int typeId) {
    return (typeId >= 1 && typeId <= 21) || (typeId >= 30 && typeId <= 40);
}

bool supportedIec103MonitorType(int typeId) {
    return typeId == 1 || typeId == 2 || typeId == 3 || typeId == 4 || typeId == 9 || typeId == 10 || typeId == 44;
}

int untimedCommand(int typeId) {
    if (typeId >= 58 && typeId <= 63) return typeId - 13;
    if (typeId == 64) return 51;
    return typeId;
}

void validateProtocol(const DeviceConfig& config) {
    const auto& type = config.protocol.type;
    const auto& iec = config.protocol.iec;
    const auto& mode = iec.transportMode;
    const bool am5se = mode == "am5se_passive_tcp" || mode == "am5se_active_tcp";
    if (type != "iec104" && type != "iec101" && !isIec103(type)) {
        reject("unsupported IEC protocol.type=" + type + " (iec104, iec101, iec103, iec103_tcp, iec103_serial)");
    }
    if (type == "iec104" && mode != "tcp") {
        reject("protocol.type=iec104 requires protocol.iec.transportMode=tcp");
    }
    if (type == "iec101" && mode != "serial") {
        reject("protocol.type=iec101 is implemented on a serial line only (protocol.iec.transportMode=serial)");
    }
    if (type == "iec103_tcp" && mode != "tcp") {
        reject("protocol.type=iec103_tcp requires protocol.iec.transportMode=tcp");
    }
    if (type == "iec103_serial" && mode != "serial") {
        reject("protocol.type=iec103_serial requires protocol.iec.transportMode=serial");
    }
    if (type == "iec103" && mode != "serial" && mode != "tcp" && !am5se) {
        reject("protocol.iec.transportMode=" + mode + " is not valid for iec103 (serial, tcp, am5se_passive_tcp, am5se_active_tcp)");
    }
    if (am5se && type != "iec103") {
        reject("protocol.iec.transportMode=" + mode + " requires protocol.type=iec103");
    }
    if (iec.balanced) {
        reject("protocol.iec.balanced=true: only unbalanced FT1.2 transmission is implemented");
    }
    if (iec.timestampZone != "local" && iec.timestampZone != "utc") {
        reject("protocol.iec.timestampZone=\"" + iec.timestampZone + "\" must be \"local\" or \"utc\"");
    }
    if (mode == "am5se_active_tcp" && (config.protocol.tcp.host.empty() || config.protocol.tcp.port <= 0)) {
        reject("protocol.iec.transportMode=am5se_active_tcp requires protocol.tcp.host and protocol.tcp.port");
    }

    if (type == "iec104") {
        if (iec.cotSize != 2 || iec.caSize != 2 || iec.ioaSize != 3) {
            reject("IEC 60870-5-104 requires protocol.iec.cotSize=2, caSize=2, ioaSize=3 (got " +
                std::to_string(iec.cotSize) + "/" + std::to_string(iec.caSize) + "/" + std::to_string(iec.ioaSize) + ")");
        }
    }
    if (type == "iec101") {
        if (iec.cotSize < 1 || iec.cotSize > 2) reject("protocol.iec.cotSize must be 1 or 2 for iec101");
        if (iec.caSize < 1 || iec.caSize > 2) reject("protocol.iec.caSize must be 1 or 2 for iec101");
        if (iec.ioaSize < 1 || iec.ioaSize > 3) reject("protocol.iec.ioaSize must be 1..3 for iec101");
        if (iec.linkAddressSize < 1 || iec.linkAddressSize > 2) {
            reject("protocol.iec.linkAddressSize must be 1 or 2 for unbalanced iec101");
        }
    }
    if (isIec103(type) && iec.linkAddressSize != 1) {
        reject("protocol.iec.linkAddressSize must be 1 for IEC 60870-5-103");
    }
    if (type != "iec104") {
        const auto maxLink = maxForOctets(iec.linkAddressSize);
        if (iec.linkAddress < 0 || iec.linkAddress >= maxLink) {
            reject("protocol.iec.linkAddress=" + std::to_string(iec.linkAddress) +
                " must be 0.." + std::to_string(maxLink - 1) + " (the broadcast address gets no reply)");
        }
    }
    const int caSize = isIec103(type) ? 1 : iec.caSize;
    if (iec.commonAddress < 0 || iec.commonAddress > maxForOctets(caSize)) {
        reject("protocol.iec.commonAddress=" + std::to_string(iec.commonAddress) + " does not fit " +
            std::to_string(caSize) + " octet(s)");
    }
    if (iec.originatorAddress < 0 || iec.originatorAddress > 255) {
        reject("protocol.iec.originatorAddress must be 0..255");
    }
    if (!isIec103(type)) {
        if (iec.interrogationQualifier < 20 || iec.interrogationQualifier > 36) {
            reject("protocol.iec.interrogationQualifier must be 20 (station) or 21..36 (group)");
        }
        const int request = iec.counterInterrogationQualifier & 0x3F;
        if (iec.counterInterrogationQualifier < 0 || iec.counterInterrogationQualifier > 255 ||
            request < 1 || request > 5) {
            reject("protocol.iec.counterInterrogationQualifier must carry RQT 1..5 (bits 0..5) and FRZ in bits 6..7");
        }
    }
    if (iec.pollOnCollect && iec.interrogationIntervalMs > 0 && iec.interrogationIntervalMs < 60000) {
        configWarning("protocol.iec.interrogationIntervalMs=" + std::to_string(iec.interrogationIntervalMs) +
            " repeats the general interrogation more than once a minute; spontaneous data already keeps points current");
    }
}

void validateReadPoint(const DeviceConfig& config, const PointDefinition& point) {
    const auto label = pointLabel(config, point);
    const auto& spec = point.read.iec;
    const auto& iec = config.protocol.iec;
    if (spec.cause < 0 || spec.cause > 63) {
        reject(label + ": read.iec.cause must be 0..63");
    }
    if (isIec103(config.protocol.type)) {
        if (spec.functionType < 0 || spec.functionType > 255 || spec.informationNumber < 0 || spec.informationNumber > 255) {
            reject(label + ": IEC 103 points need read.iec.functionType and read.iec.informationNumber (0..255)");
        }
        if (spec.typeId > 0 && !supportedIec103MonitorType(spec.typeId)) {
            reject(label + ": read.iec.typeId=" + std::to_string(spec.typeId) +
                " is not a supported IEC 103 ASDU (1, 2, 3, 4, 9, 10, 44)");
        }
        const bool measurands = spec.typeId == 3 || spec.typeId == 9;
        if (spec.elementIndex < 0 || spec.elementIndex > 15 || (spec.elementIndex > 0 && !measurands)) {
            reject(label + ": read.iec.elementIndex selects a measurand of ASDU 3 (0..3) or ASDU 9 (0..15)");
        }
        if (spec.commonAddress < 0 || spec.commonAddress > 255) {
            reject(label + ": read.iec.commonAddress must be 0..255 for IEC 103");
        }
        return;
    }
    const int ioa = spec.ioa >= 0 ? spec.ioa : (point.address > 0 ? point.address : -1);
    if (ioa < 0) {
        reject(label + ": read.iec.ioa (or address) is required; without it the point matches every object");
    }
    if (ioa > maxForOctets(iec.ioaSize)) {
        reject(label + ": IOA " + std::to_string(ioa) + " does not fit protocol.iec.ioaSize=" + std::to_string(iec.ioaSize));
    }
    if (spec.typeId > 0 && !supportedIec104MonitorType(spec.typeId)) {
        reject(label + ": read.iec.typeId=" + std::to_string(spec.typeId) +
            " is not a supported monitoring type (1..21, 30..40)");
    }
    if (spec.elementIndex < 0 || spec.elementIndex > 15 || (spec.elementIndex > 0 && spec.typeId != 20)) {
        reject(label + ": read.iec.elementIndex selects a status bit (0..15) of typeId 20 only");
    }
    if (spec.commonAddress < 0 || spec.commonAddress > maxForOctets(iec.caSize)) {
        reject(label + ": read.iec.commonAddress does not fit protocol.iec.caSize");
    }
}

void validateWritePoint(const DeviceConfig& config, const PointDefinition& point) {
    const auto label = pointLabel(config, point);
    const auto& iec = config.protocol.iec;
    const auto& write = point.write.iec;
    if (config.protocol.type != "iec104") {
        reject(label + ": write.enable=true but IEC commands are implemented for iec104 only");
    }
    const int typeId = IecCodec::commandTypeOf(point);
    if (!IecCodec::isCommandType(typeId)) {
        reject(label + ": write.iec.typeId=" + std::to_string(typeId) +
            " is not a supported command type (45..51, 58..64)");
    }
    if (write.selectBeforeExecute && !IecCodec::commandSupportsSelect(typeId)) {
        reject(label + ": write.iec.selectBeforeExecute=true but command type " + std::to_string(typeId) +
            " has no select/execute");
    }
    const int ioa = write.ioa >= 0 ? write.ioa
        : point.read.iec.ioa >= 0 ? point.read.iec.ioa
        : point.write.address >= 0 ? point.write.address
        : point.address;
    if (ioa < 0 || ioa > maxForOctets(iec.ioaSize)) {
        reject(label + ": command IOA " + std::to_string(ioa) + " does not fit protocol.iec.ioaSize");
    }
    if (ioa == 0) {
        configWarning(label + ": command IOA resolves to 0 (station address); set write.iec.ioa");
    }
    if (write.commonAddress < 0 || write.commonAddress > maxForOctets(iec.caSize)) {
        reject(label + ": write.iec.commonAddress does not fit protocol.iec.caSize");
    }
    const int untimed = untimedCommand(typeId);
    if (untimed >= 45 && untimed <= 47) {
        if ((write.qualifier & ~0x7C) != 0) {
            reject(label + ": write.iec.qualifier for command type " + std::to_string(typeId) +
                " may only set QU bits 2..6 (mask 0x7C)");
        }
    } else if (untimed >= 48 && untimed <= 50) {
        if (write.qualifier < 0 || write.qualifier > 127) {
            reject(label + ": write.iec.qualifier (QL) must be 0..127");
        }
    } else if (write.qualifier != 0) {
        reject(label + ": bitstring commands carry no qualifier; write.iec.qualifier must be 0");
    }
    for (const auto value : point.write.allowedValues) {
        bool valid = true;
        if (untimed == 45) {
            valid = value == 0.0 || value == 1.0;
        } else if (untimed == 46 || untimed == 47) {
            valid = value == 1.0 || value == 2.0;
        } else if (untimed == 51) {
            valid = value >= 0.0 && value <= 4294967295.0 && std::floor(value) == value;
        }
        if (!valid) {
            reject(label + ": write.allowedValues contains " + std::to_string(value) + ", not valid for command type " +
                std::to_string(typeId) + (untimed == 46 || untimed == 47 ? " (only 1 and 2 are permitted)" : ""));
        }
    }
}

}  // namespace

void validateIecDeviceConfig(const DeviceConfig& config) {
    validateProtocol(config);
    for (const auto* point : configPoints(config)) {
        if (!point->enabled) {
            continue;
        }
        if (point->read.enable && point->read.dataType != "device_online") {
            validateReadPoint(config, *point);
        }
        if (point->write.enable) {
            validateWritePoint(config, *point);
        }
    }
}

}  // namespace edge_gateway
