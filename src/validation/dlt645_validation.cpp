#include "edge_gateway/device_config_validation.hpp"
#include "edge_gateway/dlt645_codec.hpp"
#include "edge_gateway/dlt645_point_rules.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace edge_gateway {

namespace {

const char* const kReadDataTypes[] = {
    "dlt645_bcd", "dlt645_bcd_signed", "dlt645_uint_le", "dlt645_bitfield_le",
    "dlt645_hex", "dlt645_ascii", "dlt645_datetime", "device_online"
};
const char* const kWriteDataTypes[] = {
    "dlt645_scheduled_control", "dlt645_remote_control", "dlt645_none", "dlt645_fixed_hex", "dlt645_bcd",
    "dlt645_uint_le"
};
// DL/T 645-2007 only: the vendor trip/close DIs (0601xxxx) and the C=1CH remote control.
const char* const k2007OnlyWriteDataTypes[] = {"dlt645_scheduled_control", "dlt645_remote_control"};
const char* const kRemoteControlTypes[] = {
    "trip", "close_permit", "close", "alarm", "alarm_release", "keep_power", "keep_power_release"
};

constexpr std::uint64_t kAutoIndexBlock = 10000;

template <std::size_t N>
bool contains(const char* const (&values)[N], const std::string& value) {
    for (const auto* candidate : values) {
        if (value == candidate) return true;
    }
    return false;
}

template <std::size_t N>
std::string joined(const char* const (&values)[N]) {
    std::string text;
    for (const auto* candidate : values) {
        if (!text.empty()) text += ", ";
        text += candidate;
    }
    return text;
}

bool isHex(const std::string& text, std::size_t length) {
    if (text.size() != length) return false;
    for (const auto ch : text) {
        if (std::isxdigit(static_cast<unsigned char>(ch)) == 0) return false;
    }
    return true;
}

// Errors on enabled points stop the load; the same mistakes on disabled points are only reported,
// they become errors when the point is enabled.
class Findings {
public:
    explicit Findings(std::string owner) : owner_(std::move(owner)) {}

    void report(const PointDefinition& point, bool fatal, const std::string& message) const {
        const auto text = owner_ + " point " + point.pointCode + " (index " + std::to_string(point.index) +
            "): " + message;
        if (fatal) throw std::invalid_argument(text);
        configWarning(text + " [point disabled]");
    }

private:
    std::string owner_;
};

bool usesBit(const std::string& dataType) {
    return dataType == "dlt645_uint_le" || dataType == "dlt645_bitfield_le";
}

void validateRead(const PointDefinition& point, Dlt645Version version, const Findings& findings) {
    const bool fatal = point.enabled && point.read.enable;
    const auto& read = point.read;
    if (!isDlt645ReadDataType(read.dataType)) {
        findings.report(point, fatal, "read.dataType '" + read.dataType +
            "' is not a DL/T 645 type (allowed: " + joined(kReadDataTypes) + ")");
        return;
    }
    if (read.dataType == "device_online") return;
    if (!isHex(read.dlt645Di, dlt645DataIdHexLength(version))) {
        findings.report(point, fatal, "read.dlt645.di '" + read.dlt645Di + "' must be " +
            (version == Dlt645Version::V1997 ? "4 hex digits (DI1 DI0) for dlt645_1997" : "8 hex digits (DI3..DI0) for dlt645_2007"));
        return;
    }
    if (read.dlt645ByteCount < 0 || read.dlt645ByteCount > 200) {
        findings.report(point, fatal, "read.dlt645.byteCount must be 0..200");
    }
    if (read.dlt645Offset < 0 || read.dlt645Offset > 200) {
        findings.report(point, fatal, "read.dlt645.offset must be 0..200 (byte offset inside the DI data)");
    }
    if (read.dlt645Offset > 0 && read.dlt645ByteCount <= 0) {
        findings.report(point, fatal, "read.dlt645.offset needs read.dlt645.byteCount > 0");
    }
    if (isDlt645BlockDataId(read.dlt645Di) && read.dlt645ByteCount <= 0) {
        findings.report(point, fatal, "block DI " + read.dlt645Di +
            " needs read.dlt645.byteCount and read.dlt645.offset for each value");
    }
    const bool numeric = read.dataType == "dlt645_bcd" || read.dataType == "dlt645_bcd_signed" ||
        read.dataType == "dlt645_uint_le";
    if (numeric && read.dlt645ByteCount > 8) {
        findings.report(point, fatal, "read.dlt645.byteCount above 8 cannot be decoded as a number");
    }
    if (read.bit >= 0 && !usesBit(read.dataType)) {
        findings.report(point, fatal, "read.bit is only decoded for dlt645_uint_le / dlt645_bitfield_le");
    }
    if (read.bit >= 0 && read.dlt645ByteCount > 0 && read.bit >= read.dlt645ByteCount * 8) {
        findings.report(point, fatal, "read.bit exceeds read.dlt645.byteCount");
    }
    if (!std::isfinite(read.scale) || std::abs(read.scale) < 1e-15) {
        findings.report(point, fatal, "read.scale must be a finite non-zero number");
    }
}

void validateWrite(
    const PointDefinition& point,
    const std::vector<PointDefinition>& points,
    Dlt645Version version,
    const Findings& findings,
    bool templateList
) {
    const bool fatal = point.enabled;
    const auto& write = point.write;
    if (!point.enabled) {
        configWarning("point " + point.pointCode + " has write.enable=true but is disabled; it is not writable");
    }
    const auto dataType = dlt645WriteDataType(write);
    if (!contains(kWriteDataTypes, dataType)) {
        findings.report(point, fatal, "write.dlt645.dataType '" + dataType +
            "' is not a DL/T 645 write type (allowed: " + joined(kWriteDataTypes) + ")");
        return;
    }
    if (version == Dlt645Version::V1997 && contains(k2007OnlyWriteDataTypes, dataType)) {
        findings.report(point, fatal, dataType + " is DL/T 645-2007 only (protocol.type is dlt645_1997)");
        return;
    }
    const auto byteCount = write.dlt645.byteCount;
    if (dataType == "dlt645_remote_control") {
        // C=1CH has no DI: N1 comes from controlType.
        if (!write.dlt645.di.empty() || byteCount != 0) {
            findings.report(point, fatal, "dlt645_remote_control uses no write.dlt645.di / byteCount");
        }
        if (!contains(kRemoteControlTypes, write.dlt645.controlType)) {
            findings.report(point, fatal, "write.dlt645.controlType '" + write.dlt645.controlType +
                "' must be one of " + joined(kRemoteControlTypes));
        }
        if (write.dlt645.validitySeconds < 10 || write.dlt645.validitySeconds > 86400) {
            findings.report(point, fatal, "write.dlt645.validitySeconds must be 10..86400");
        }
    } else if (!isHex(write.dlt645.di, dlt645DataIdHexLength(version))) {
        findings.report(point, fatal, "write.dlt645.di '" + write.dlt645.di + "' must be " +
            std::to_string(dlt645DataIdHexLength(version)) + " hex digits for " + dlt645ProtocolName(version));
    }
    if (dataType == "dlt645_remote_control") {
        // checked above
    } else if (dataType == "dlt645_scheduled_control") {
        if (byteCount != 2) findings.report(point, fatal, "dlt645_scheduled_control needs write.dlt645.byteCount=2");
        if (write.dlt645.unit != 2 && write.dlt645.unit != 3) {
            findings.report(point, fatal, "dlt645_scheduled_control write.dlt645.unit must be 2 (min) or 3 (h)");
        }
        if (!write.maxValue || *write.maxValue > 99 || (write.minValue && *write.minValue < 0)) {
            findings.report(point, fatal, "dlt645_scheduled_control needs write.min>=0 and write.max<=99");
        }
    } else if (dataType == "dlt645_none") {
        if (byteCount != 0) findings.report(point, fatal, "dlt645_none needs write.dlt645.byteCount=0");
    } else if (dataType == "dlt645_fixed_hex") {
        if (byteCount < 0 || !isHex(write.dlt645.fixedDataHex, static_cast<std::size_t>(std::max(0, byteCount)) * 2U)) {
            findings.report(point, fatal, "write.dlt645.fixedDataHex must hold byteCount bytes of hex");
        }
    } else if (byteCount <= 0 || byteCount > 8) {
        findings.report(point, fatal, dataType + " needs write.dlt645.byteCount 1..8");
    }

    const bool verifyByRead = write.verifyAfterWrite && write.verifyByRead;
    const bool hasPointCode = !write.dlt645.verifyPointCode.empty();
    const bool hasIndex = write.dlt645.verifyIndex != 0;
    if (templateList && verifyByRead && !hasPointCode && !hasIndex) {
        // Target supplied per meter (meters[].positionReadBackIndex); the rest is checked here.
        if (!write.dlt645.verifyValue) {
            findings.report(point, fatal, "read-back needs write.dlt645.verifyValue (expected position)");
        }
        if (write.dlt645.verifyTimeoutMs < 100 || write.dlt645.verifyTimeoutMs > 60000) {
            findings.report(point, fatal, "write.dlt645.verifyTimeoutMs must be 100..60000");
        }
        return;
    }
    if (isDlt645SwitchingControl(write) && (!verifyByRead || (!hasPointCode && !hasIndex))) {
        findings.report(point, fatal, "switching command needs a position read-back: set write.verifyAfterWrite, "
            "write.verifyByRead, write.dlt645.verifyValue and write.dlt645.verifyPointCode (same meter) or "
            "write.dlt645.verifyIndex (another device, e.g. a DIO auxiliary contact)");
        return;
    }
    if (write.verifyAfterWrite && !write.verifyByRead) {
        findings.report(point, fatal, "DL/T 645 can only verify by reading back; set write.verifyByRead");
    }
    if (!verifyByRead) return;
    if (hasPointCode == hasIndex) {
        findings.report(point, fatal, "write.verifyByRead needs exactly one of write.dlt645.verifyPointCode and "
            "write.dlt645.verifyIndex");
        return;
    }
    if ((isDlt645SwitchingControl(write) || hasIndex) && !write.dlt645.verifyValue) {
        findings.report(point, fatal, "read-back needs write.dlt645.verifyValue (expected position)");
    }
    if (write.dlt645.verifyTimeoutMs < 100 || write.dlt645.verifyTimeoutMs > 60000) {
        findings.report(point, fatal, "write.dlt645.verifyTimeoutMs must be 100..60000");
    }
    if (hasIndex) {
        // Another device's point: it lives in another configuration file, so only the index itself
        // can be checked here; the executor refuses the command when the point is absent or stale.
        if (write.dlt645.verifyIndex == point.index) {
            findings.report(point, fatal, "write.dlt645.verifyIndex must name another point");
        }
        return;
    }
    const PointDefinition* target = nullptr;
    for (const auto& candidate : points) {
        if (candidate.pointCode == write.dlt645.verifyPointCode) {
            target = &candidate;
            break;
        }
    }
    if (target == nullptr || !target->enabled || !target->read.enable ||
        target->read.dataType == "device_online" || target->read.dlt645Di.empty()) {
        findings.report(point, fatal, "write.dlt645.verifyPointCode '" + write.dlt645.verifyPointCode +
            "' must name an enabled DL/T 645 read point of the same meter");
    }
}

}  // namespace

bool isDlt645ReadDataType(const std::string& dataType) {
    return contains(kReadDataTypes, dataType);
}

bool isDlt645WriteDataType(const std::string& dataType, Dlt645Version version) {
    return contains(kWriteDataTypes, dataType) &&
        (version == Dlt645Version::V2007 || !contains(k2007OnlyWriteDataTypes, dataType));
}

std::string dlt645WriteDataType(const WriteSpec& spec) {
    return spec.dlt645.dataType.empty() ? spec.dataType : spec.dlt645.dataType;
}

bool isDlt645SwitchingControl(const WriteSpec& spec) {
    const auto dataType = dlt645WriteDataType(spec);
    return dataType == "dlt645_scheduled_control" ||
        (dataType == "dlt645_remote_control" && (spec.dlt645.controlType == "trip" || spec.dlt645.controlType == "close"));
}

bool isDlt645BlockDataId(const std::string& dataIdHex) {
    const auto isF = [](char ch) { return ch == 'F' || ch == 'f'; };
    if (dataIdHex.size() == 4) {
        // 1997: a trailing F (e.g. 901F, B61F) or an FF byte addresses a block.
        return isF(dataIdHex[3]) || (isF(dataIdHex[0]) && isF(dataIdHex[1]));
    }
    if (dataIdHex.size() != 8) return false;
    for (std::size_t i = 0; i < 8; i += 2) {
        if (isF(dataIdHex[i]) && isF(dataIdHex[i + 1])) {
            return true;
        }
    }
    return false;
}

void validateDlt645PointList(const std::vector<PointDefinition>& points, const std::string& owner,
                             Dlt645Version version, bool templateList) {
    const Findings findings(owner);
    // Several points decoding exactly the same bytes of one DI: on a block DI that is the
    // "Ua/Ub/Uc all show Ua" mistake (missing offsets).
    std::map<std::tuple<std::string, int, int, int, std::string>, const PointDefinition*> decodes;
    std::set<std::string> codes;
    for (const auto& point : points) {
        if (!point.pointCode.empty() && !codes.insert(point.pointCode).second) {
            findings.report(point, true, "duplicate pointCode in the same meter");
        }
        const bool readDefined = point.read.enable || !point.read.dlt645Di.empty();
        if (readDefined) {
            validateRead(point, version, findings);
            if (point.enabled && point.read.enable && point.read.dataType != "device_online") {
                const auto key = std::make_tuple(point.read.dlt645Di, point.read.dlt645Offset,
                    point.read.dlt645ByteCount, point.read.bit, point.read.dataType);
                const auto inserted = decodes.emplace(key, &point);
                if (!inserted.second) {
                    const auto message = "decodes the same bytes of DI " + point.read.dlt645Di + " as point " +
                        inserted.first->second->pointCode + "; set read.dlt645.offset";
                    if (isDlt645BlockDataId(point.read.dlt645Di)) findings.report(point, true, message);
                    configWarning(owner + " point " + point.pointCode + ": " + message);
                }
            }
        }
        if (point.write.enable) {
            validateWrite(point, points, version, findings, templateList);
        }
    }
}

void validateDlt645DeviceConfig(const DeviceConfig& config) {
    const auto version = dlt645VersionOf(config.protocol.type);
    const auto& transport = config.protocol.transport;
    if (transport.wakeupBytes < 0 || transport.wakeupBytes > 16) {
        throw std::invalid_argument("protocol.transport.wakeupBytes must be 0..16");
    }
    // Password/operator code format (8 hex digits, PA P0 P1 P2 / C0..C3) is checked by the parser.
    // The vendor's level-0 factory code 00000000 is a valid site password and is not refused.
    const auto& write = config.protocol.dlt645.write;
    if (version == Dlt645Version::V1997 && !write.operatorCode.empty()) {
        configWarning("dlt645.write.operatorCode is not used by DL/T 645-1997 writes");
    }
    const auto& timeSync = config.protocol.dlt645.timeSync;
    if (timeSync.enabled && (timeSync.intervalMs < 3600000 || timeSync.intervalMs > 604800000)) {
        // Meters accept one broadcast time sync per day; more often only adds bus traffic.
        throw std::invalid_argument("dlt645.timeSync.intervalMs must be 3600000..604800000 (1 h .. 7 d)");
    }
    const auto checkAddress = [](const std::string& owner, const std::string& address) {
        std::string normalized;
        try {
            normalized = Dlt645Codec::normalizeAddress(address);
        } catch (const std::exception& ex) {
            throw std::invalid_argument(owner + " address '" + address + "': " + ex.what());
        }
        if (address.size() != 12) {
            configWarning(owner + " address '" + address + "' is padded to " + normalized);
        }
        return normalized;
    };

    bool anyWritablePoint = false;
    const auto inspect = [&](const std::vector<PointDefinition>& points, const std::string& owner) {
        validateDlt645PointList(points, owner, version);
        for (const auto& point : points) {
            anyWritablePoint = anyWritablePoint || (point.enabled && point.write.enable);
        }
    };

    // Auto-expanded meters (no points[]) own autoIndexBase + meter*10000 .. +9999.
    const std::uint64_t base = config.protocol.dlt645.autoIndexBase;
    std::vector<std::pair<std::uint64_t, std::string>> autoRanges;
    for (std::size_t i = 0; i < config.meters.size(); ++i) {
        if (!config.meters[i].points.empty()) continue;
        const auto first = base + static_cast<std::uint64_t>(i) * kAutoIndexBlock;
        if (first + kAutoIndexBlock - 1U > 0xFFFFFFFFULL) {
            throw std::invalid_argument("dlt645.autoIndexBase " + std::to_string(base) + ": meter " +
                config.meters[i].meterCode + " would get indexes beyond 4294967295");
        }
        autoRanges.emplace_back(first, config.meters[i].meterCode);
    }
    const auto checkExplicitIndexes = [&](const std::vector<PointDefinition>& points, const std::string& owner) {
        for (const auto& point : points) {
            for (const auto& range : autoRanges) {
                if (point.index >= range.first && point.index < range.first + kAutoIndexBlock) {
                    throw std::invalid_argument(owner + " point " + point.pointCode + " index " +
                        std::to_string(point.index) + " lies in the auto-index block of meter " + range.second +
                        " (" + std::to_string(range.first) + ".." + std::to_string(range.first + kAutoIndexBlock - 1U) +
                        "); change dlt645.autoIndexBase or the index");
                }
            }
        }
    };

    std::set<std::string> addresses;
    for (const auto& meter : config.meters) {
        const auto owner = "meter " + meter.meterCode;
        checkExplicitIndexes(meter.points, owner);
        if (!meter.enabled) continue;
        const auto normalized = checkAddress(owner, meter.address);
        if (!addresses.insert(normalized).second) {
            throw std::invalid_argument(owner + " address " + normalized + " is used by another meter on this bus");
        }
        inspect(meter.points, owner);
    }
    if (!config.points.empty()) {
        checkExplicitIndexes(config.points, "device " + config.meterCode);
        if (!config.address.empty()) checkAddress("device " + config.meterCode, config.address);
        inspect(config.points, "device " + config.meterCode);
    }
    if (anyWritablePoint && !write.enabled) {
        configWarning("DL/T 645 points have write.enable=true but dlt645.write.enabled=false; writes are rejected");
    }
}

std::vector<PointDefinition> expandDlt645MeterPoints(
    const DeviceConfig& config,
    std::size_t meterIndex,
    const std::vector<PointDefinition>& standardPoints
) {
    const auto version = dlt645VersionOf(config.protocol.type);
    const auto& meter = config.meters.at(meterIndex);
    auto points = meter.points.empty() ? standardPoints : meter.points;
    if (meter.points.empty()) {
        for (const auto& point : points) {
            if (!point.read.dlt645Di.empty() && point.read.dlt645Di.size() != dlt645DataIdHexLength(version)) {
                throw std::invalid_argument("meter " + meter.meterCode + ": standard points file " +
                    config.protocol.standardPointsFile + " does not match protocol.type " + config.protocol.type);
            }
        }
        if (points.size() > kAutoIndexBlock) {
            throw std::invalid_argument("DL/T 645 standard template has more than 10000 points");
        }
        const auto indexBase = static_cast<std::uint64_t>(config.protocol.dlt645.autoIndexBase) +
            static_cast<std::uint64_t>(meterIndex) * kAutoIndexBlock;
        if (indexBase + kAutoIndexBlock - 1U > 0xFFFFFFFFULL) {
            throw std::invalid_argument("dlt645.autoIndexBase leaves no index block for meter " + meter.meterCode);
        }
        for (std::size_t i = 0; i < points.size(); ++i) {
            points[i].index = static_cast<std::uint32_t>(indexBase + i);
        }
    }
    if (meter.positionReadBackIndex != 0) {
        for (auto& point : points) {
            auto& verify = point.write.dlt645;
            if (point.write.verifyAfterWrite && point.write.verifyByRead && isDlt645SwitchingControl(point.write) &&
                verify.verifyPointCode.empty() && verify.verifyIndex == 0) {
                verify.verifyIndex = meter.positionReadBackIndex;
            }
        }
    }
    return points;
}

}  // namespace edge_gateway
