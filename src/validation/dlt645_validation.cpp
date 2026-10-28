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

namespace edge_gateway {

namespace {

const char* const kReadDataTypes[] = {
    "dlt645_bcd", "dlt645_bcd_signed", "dlt645_uint_le", "dlt645_bitfield_le",
    "dlt645_hex", "dlt645_ascii", "dlt645_datetime", "device_online"
};
const char* const kWriteDataTypes[] = {
    "dlt645_scheduled_control", "dlt645_none", "dlt645_fixed_hex", "dlt645_bcd", "dlt645_uint_le"
};

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

void validateRead(const PointDefinition& point, const Findings& findings) {
    const bool fatal = point.enabled && point.read.enable;
    const auto& read = point.read;
    if (!isDlt645ReadDataType(read.dataType)) {
        findings.report(point, fatal, "read.dataType '" + read.dataType +
            "' is not a DL/T 645 type (allowed: " + joined(kReadDataTypes) + ")");
        return;
    }
    if (read.dataType == "device_online") return;
    if (!isHex(read.dlt645Di, 8)) {
        findings.report(point, fatal, "read.dlt645.di '" + read.dlt645Di + "' must be 8 hex digits (DI3..DI0)");
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
    const Findings& findings
) {
    const bool fatal = point.enabled;
    const auto& write = point.write;
    if (!point.enabled) {
        configWarning("point " + point.pointCode + " has write.enable=true but is disabled; it is not writable");
    }
    if (!isHex(write.dlt645.di, 8)) {
        findings.report(point, fatal, "write.dlt645.di '" + write.dlt645.di + "' must be 8 hex digits");
    }
    const auto dataType = dlt645WriteDataType(write);
    if (!isDlt645WriteDataType(dataType)) {
        findings.report(point, fatal, "write.dlt645.dataType '" + dataType +
            "' is not a DL/T 645 write type (allowed: " + joined(kWriteDataTypes) + ")");
        return;
    }
    const auto byteCount = write.dlt645.byteCount;
    if (dataType == "dlt645_scheduled_control") {
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
    if (isDlt645SwitchingControl(write) && (!verifyByRead || write.dlt645.verifyPointCode.empty())) {
        findings.report(point, fatal, "switching command needs a position read-back: set write.verifyAfterWrite, "
            "write.verifyByRead and write.dlt645.verifyPointCode/verifyValue");
        return;
    }
    if (write.verifyAfterWrite && !write.verifyByRead) {
        findings.report(point, fatal, "DL/T 645 can only verify by reading back; set write.verifyByRead");
    }
    if (!verifyByRead) return;
    if (write.dlt645.verifyPointCode.empty()) {
        findings.report(point, fatal, "write.verifyByRead needs write.dlt645.verifyPointCode");
        return;
    }
    if (isDlt645SwitchingControl(write) && !write.dlt645.verifyValue) {
        findings.report(point, fatal, "switching command needs write.dlt645.verifyValue (expected position)");
    }
    if (write.dlt645.verifyTimeoutMs < 100 || write.dlt645.verifyTimeoutMs > 60000) {
        findings.report(point, fatal, "write.dlt645.verifyTimeoutMs must be 100..60000");
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

bool isDlt645WriteDataType(const std::string& dataType) {
    return contains(kWriteDataTypes, dataType);
}

std::string dlt645WriteDataType(const WriteSpec& spec) {
    return spec.dlt645.dataType.empty() ? spec.dataType : spec.dlt645.dataType;
}

bool isDlt645SwitchingControl(const WriteSpec& spec) {
    return dlt645WriteDataType(spec) == "dlt645_scheduled_control";
}

bool isDlt645BlockDataId(const std::string& dataIdHex) {
    if (dataIdHex.size() != 8) return false;
    for (std::size_t i = 0; i < 8; i += 2) {
        if ((dataIdHex[i] == 'F' || dataIdHex[i] == 'f') && (dataIdHex[i + 1] == 'F' || dataIdHex[i + 1] == 'f')) {
            return true;
        }
    }
    return false;
}

bool isDlt645DefaultPassword(const std::string& passwordHex) {
    return passwordHex.size() == 8 && passwordHex.compare(2, 6, "000000") == 0;
}

void validateDlt645PointList(const std::vector<PointDefinition>& points, const std::string& owner) {
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
            validateRead(point, findings);
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
            validateWrite(point, points, findings);
        }
    }
}

void validateDlt645DeviceConfig(const DeviceConfig& config) {
    const auto& transport = config.protocol.transport;
    if (transport.wakeupBytes < 0 || transport.wakeupBytes > 16) {
        throw std::invalid_argument("protocol.transport.wakeupBytes must be 0..16");
    }
    const auto& write = config.protocol.dlt645.write;
    if (!write.password.empty() && isDlt645DefaultPassword(write.password)) {
        throw std::invalid_argument(
            "dlt645.write.password is a factory default (P0P1P2=000000); provision the site password"
        );
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
        validateDlt645PointList(points, owner);
        for (const auto& point : points) {
            anyWritablePoint = anyWritablePoint || (point.enabled && point.write.enable);
        }
    };

    std::set<std::string> addresses;
    for (const auto& meter : config.meters) {
        if (!meter.enabled) continue;
        const auto owner = "meter " + meter.meterCode;
        const auto normalized = checkAddress(owner, meter.address);
        if (!addresses.insert(normalized).second) {
            throw std::invalid_argument(owner + " address " + normalized + " is used by another meter on this bus");
        }
        inspect(meter.points, owner);
    }
    if (!config.points.empty()) {
        if (!config.address.empty()) checkAddress("device " + config.meterCode, config.address);
        inspect(config.points, "device " + config.meterCode);
    }
    if (anyWritablePoint && !write.enabled) {
        configWarning("DL/T 645 points have write.enable=true but dlt645.write.enabled=false; writes are rejected");
    }
}

}  // namespace edge_gateway
