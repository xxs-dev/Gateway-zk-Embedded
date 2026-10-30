#include "edge_gateway/device_config_validation.hpp"

#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "edge_gateway/can_signal_codec.hpp"

namespace edge_gateway {

namespace {

bool isOnlineStatusPoint(const PointDefinition& point) {
    return point.pointCode == "device_online" || point.read.dataType == "device_online" ||
        point.read.dataType == "interface_online";
}

[[noreturn]] void reject(const std::string& label, const std::string& field, const std::string& message) {
    throw std::invalid_argument("CAN " + label + ": " + field + ": " + message);
}

void checkSignalDataType(const std::string& label, const std::string& field, const std::string& dataType) {
    if (!CanSignalCodec::isSignalDataType(dataType)) {
        reject(label, field, "unsupported \"" + dataType +
               "\" (bool, int8, uint8, int16, uint16, int32, uint32, int64, uint64, float32)");
    }
}

void checkFrameId(const std::string& label, const std::string& field, const CanSignalSpec& can) {
    try {
        (void)CanSignalCodec::parseFrameId(can.frameId, can.extended);
    } catch (const std::exception& ex) {
        reject(label, field, ex.what());
    }
}

void warnTypeWidth(const std::string& label, const std::string& field, const std::string& dataType, int bitLength) {
    const CanSignalSpec none;
    const auto width = CanSignalCodec::signalBitLength(none, dataType);
    if (dataType == "bool" && bitLength != 1) {
        configWarning("CAN " + label + ": " + field + " bool signal with bitLength " + std::to_string(bitLength) +
                      " reads 1 for any non-zero raw value");
    } else if (dataType != "bool" && bitLength > width) {
        configWarning("CAN " + label + ": " + field + " bitLength " + std::to_string(bitLength) +
                      " is wider than dataType " + dataType);
    }
}

void validateReadSignal(const std::string& label, const PointDefinition& point, bool fdEnabled) {
    const auto& read = point.read;
    checkSignalDataType(label, "read.dataType", read.dataType);
    checkFrameId(label, "read.can.frameId", read.can);
    if (read.can.dlc < 0 || read.can.dlc > 64) {
        reject(label, "read.can.dlc", "must be 0..64");
    }
    // Layout (endian, bitOrder, offsets, length, type) through the codec itself, on the longest frame
    // the interface can receive: 8 bytes, or 64 with CAN FD. The received frame length is what counts
    // at run time; read.can.dlc is informational.
    const std::size_t maxBytes = fdEnabled ? 64U : 8U;
    try {
        (void)CanSignalCodec::decode(std::vector<std::uint8_t>(maxBytes, 0), read);
    } catch (const CanInvalidRawValue&) {
        // all-zero frame matches an invalid raw value: the layout itself is fine
    } catch (const std::exception& ex) {
        reject(label, "read.can", std::string(ex.what()) +
               (fdEnabled ? "" : " (classic CAN frames carry at most 8 bytes)"));
    }
    const auto bitLength = CanSignalCodec::signalBitLength(read.can, read.dataType);
    if (read.can.dlc > 0 && read.can.byteOffset * 8 + read.can.bitOffset + bitLength > read.can.dlc * 8) {
        configWarning("CAN " + label + ": read.can signal ends beyond read.can.dlc " + std::to_string(read.can.dlc) +
                      "; frames shorter than the signal are reported as bad quality");
    }
    for (const auto raw : read.can.invalidRawValues) {
        if (bitLength < 64 && raw >= (std::uint64_t{1} << bitLength)) {
            reject(label, "read.can.invalidRawValues",
                   "value " + std::to_string(raw) + " does not fit the " + std::to_string(bitLength) + "-bit signal");
        }
    }
    if (read.can.receiveTimeoutMs < 0) {
        reject(label, "read.can.receiveTimeoutMs", "must not be negative (0 disables the timeout)");
    }
    warnTypeWidth(label, "read.can", read.dataType, bitLength);
}

struct TxFrameDefinition {
    std::string owner;
    int dlc = 0;
    bool fd = false;
    bool remoteRequest = false;
    bool templateSafe = false;
    std::string payloadTemplate;
    std::vector<std::pair<std::string, std::vector<int>>> signals;  // owner label, occupied bits
    struct Pulse {
        std::string label;
        WriteSpec write;
        bool signedSignal;
    };
    std::vector<Pulse> pulses;
};

std::vector<int> occupiedBits(const CanSignalSpec& can, int bitLength) {
    // Both orders occupy consecutive positions from byteOffset*8+bitOffset in reading order; the
    // physical bit is the same position for Intel and mirrored within the byte for Motorola.
    std::vector<int> bits;
    const auto start = can.byteOffset * 8 + can.bitOffset;
    for (int i = 0; i < bitLength; ++i) {
        const auto position = start + i;
        bits.push_back(can.endian == "big" ? (position / 8) * 8 + 7 - position % 8 : position);
    }
    return bits;
}

void validateWriteSignal(const std::string& label, const PointDefinition& point, bool fdEnabled,
                         std::map<std::uint64_t, TxFrameDefinition>& frames) {
    const auto& write = point.write;
    checkSignalDataType(label, "write.dataType", write.dataType);
    checkFrameId(label, "write.can.frameId", write.can);
    if (write.can.fd) {
        if (!fdEnabled) {
            reject(label, "write.can.fd", "CAN FD frames need protocol.can.fdEnabled=true");
        }
        if (!CanSignalCodec::isValidFdLength(write.can.dlc)) {
            reject(label, "write.can.dlc", "CAN FD length must be 0..8, 12, 16, 20, 24, 32, 48 or 64");
        }
        if (write.can.remoteRequest) {
            reject(label, "write.can.remoteRequest", "CAN FD has no remote frames");
        }
    } else if (write.can.dlc < 0 || write.can.dlc > 8) {
        reject(label, "write.can.dlc", "classic CAN frames carry 0..8 bytes (set write.can.fd=true for CAN FD)");
    }
    std::vector<std::uint8_t> payload;
    try {
        payload = CanSignalCodec::templatePayload(write.can);
        auto layout = payload;
        CanSignalCodec::encodeInto(layout, write.offset, write, true);
    } catch (const std::exception& ex) {
        reject(label, "write.can", ex.what());
    }
    if (write.can.pulse) {
        if (write.dataType == "float32") {
            reject(label, "write.can.pulse", "pulse signals must be bool or integer");
        }
        auto idle = payload;
        try {
            CanSignalCodec::encodeInto(idle, write.can.pulseIdleValue, write, point.read.signedFlag);
        } catch (const std::exception& ex) {
            reject(label, "write.can.pulseIdleValue", ex.what());
        }
    }
    if (write.verifyAfterWrite) {
        configWarning("CAN " + label + ": write.verifyAfterWrite is not supported for CAN; a successful write "
                      "means the frame was queued by the kernel");
    }

    const auto key = CanSignalCodec::frameKey(CanSignalCodec::parseFrameId(write.can.frameId, write.can.extended),
                                              write.can.extended);
    auto inserted = frames.emplace(key, TxFrameDefinition());
    auto& frame = inserted.first->second;
    if (inserted.second) {
        frame.owner = label;
        frame.dlc = write.can.dlc;
        frame.fd = write.can.fd;
        frame.remoteRequest = write.can.remoteRequest;
        frame.templateSafe = write.can.templateSafe;
    } else if (frame.dlc != write.can.dlc || frame.fd != write.can.fd || frame.remoteRequest != write.can.remoteRequest ||
               frame.templateSafe != write.can.templateSafe) {
        reject(label, "write.can", "frame " + write.can.frameId + " is also written by " + frame.owner +
               " with a different dlc/fd/remoteRequest/templateSafe; all signals of one frame must agree");
    }
    if (!write.can.payloadTemplate.empty()) {
        if (!frame.payloadTemplate.empty() && frame.payloadTemplate != write.can.payloadTemplate) {
            reject(label, "write.can.payloadTemplate", "differs from the template of the same frame (" +
                   frame.payloadTemplate + ")");
        }
        frame.payloadTemplate = write.can.payloadTemplate;
    }
    const auto bits = occupiedBits(write.can, CanSignalCodec::signalBitLength(write.can, write.dataType));
    for (const auto& other : frame.signals) {
        for (const auto bit : bits) {
            bool overlap = false;
            for (const auto used : other.second) overlap = overlap || used == bit;
            if (overlap) {
                configWarning("CAN " + label + ": write signal overlaps " + other.first + " in frame " +
                              write.can.frameId + "; the later write overwrites the shared bits");
                break;
            }
        }
    }
    frame.signals.emplace_back(label, bits);
    if (write.can.pulse) {
        frame.pulses.push_back(TxFrameDefinition::Pulse{label, write, point.read.signedFlag});
    }
}

// A template must hold every pulse signal of its frame at the idle value, or every frame would
// repeat the pulse.
void checkPulseTemplates(const std::map<std::uint64_t, TxFrameDefinition>& frames) {
    for (const auto& item : frames) {
        const auto& frame = item.second;
        if (frame.payloadTemplate.empty()) {
            continue;
        }
        for (const auto& pulse : frame.pulses) {
            auto can = pulse.write.can;
            can.payloadTemplate = frame.payloadTemplate;
            const auto payload = CanSignalCodec::templatePayload(can);
            auto idle = payload;
            CanSignalCodec::encodeInto(idle, pulse.write.can.pulseIdleValue, pulse.write, pulse.signedSignal);
            if (idle != payload) {
                reject(pulse.label, "write.can.payloadTemplate",
                       "sets this pulse signal to a value other than pulseIdleValue");
            }
        }
    }
}

}  // namespace

void validateCanDeviceConfig(const DeviceConfig& config) {
    const auto& can = config.protocol.can;
    if (can.transportMode != "socketcan" && can.transportMode != "udp_test") {
        throw std::invalid_argument("CAN protocol.can.transportMode must be socketcan (udp_test only for the test lab): " +
                                    can.transportMode);
    }
    std::map<std::uint64_t, TxFrameDefinition> frames;
    const auto validatePoints = [&](const std::vector<PointDefinition>& points) {
        for (const auto& point : points) {
            if (!point.enabled) {
                continue;
            }
            const auto label = pointLabel(config, point);
            if (isOnlineStatusPoint(point)) {
                if (point.write.enable) {
                    reject(label, "write.enable", "online status points are read-only");
                }
                continue;
            }
            if (point.read.enable) {
                validateReadSignal(label, point, can.fdEnabled);
            }
            if (point.write.enable) {
                validateWriteSignal(label, point, can.fdEnabled, frames);
            }
        }
    };
    if (config.meters.empty()) {
        validatePoints(config.points);
    }
    for (const auto& meter : config.meters) {
        if (!meter.enabled) {
            continue;
        }
        for (const auto& id : meter.onlineFrameIds) {
            try {
                (void)CanSignalCodec::parseAnyFrameId(id);
            } catch (const std::exception& ex) {
                throw std::invalid_argument("CAN meter " + meter.meterCode + ": onlineFrameIds: " + ex.what());
            }
        }
        if (meter.onlineTimeoutMs <= 0) {
            throw std::invalid_argument("CAN meter " + meter.meterCode + ": onlineTimeoutMs must be positive");
        }
        validatePoints(meter.points);
    }
    checkPulseTemplates(frames);
}

}  // namespace edge_gateway
