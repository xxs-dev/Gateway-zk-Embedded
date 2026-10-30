#include "edge_gateway/device_config_validation.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "edge_gateway/modbus_codec.hpp"
#include "edge_gateway/read_task_planner.hpp"

namespace edge_gateway {

namespace {

// Assumed device turnaround (request received -> reply starts) for the load estimate; real devices
// are often slower, so the estimate is a lower bound of the bus time.
constexpr double kAssumedTurnaroundMs = 20.0;

[[noreturn]] void reject(const DeviceConfig& config, const PointDefinition& point, const std::string& message) {
    throw std::invalid_argument(pointLabel(config, point) + ": " + message);
}

bool isUnsignedOrBitType(const std::string& dataType) {
    return dataType == "uint16" || dataType == "uint32" || dataType == "uint64" ||
        dataType == "bit" || dataType == "bool" || dataType == "uint32_base10000";
}

bool isBitType(const std::string& dataType) {
    return dataType == "bit" || dataType == "bool";
}

void validateReadSpec(const DeviceConfig& config, const PointDefinition& point) {
    const auto& read = point.read;
    if (read.function < 1 || read.function > 4) {
        reject(config, point, "read.function " + std::to_string(read.function) + " is not 1, 2, 3 or 4");
    }
    if (read.length < 1) {
        reject(config, point, "read.length must be at least 1");
    }
    if (point.address < 0 || point.address > 0xFFFF || read.length - 1 > 0xFFFF - point.address) {
        reject(config, point, "address " + std::to_string(point.address) + " with read.length " +
            std::to_string(read.length) + " is outside 0..65535");
    }
    const auto maxRequest = std::max(1, config.collect.maxRequestRegisters);
    if (read.function >= 3 && read.length > maxRequest) {
        reject(config, point, "read.length " + std::to_string(read.length) +
            " exceeds collect.maxRequestRegisters " + std::to_string(maxRequest));
    }
    if (read.signedFlag && isUnsignedOrBitType(read.dataType)) {
        reject(config, point, "read.signed=true contradicts read.dataType " + read.dataType +
            "; the data type alone decides signedness (use int16/int32/int64)");
    }
    if (read.function <= 2) {
        if (read.length != 1) {
            reject(config, point, "coil/discrete input points read one bit (read.length 1)");
        }
        return;
    }
    const auto width = read.dataType == "uint32_base10000" ? 4
        : ModbusCodec::registerTypeByteWidth(read.dataType);
    if (width == 0) {
        reject(config, point, "unsupported read.dataType \"" + read.dataType + "\" for function " +
            std::to_string(read.function));
    }
    if (read.length * 2 != width) {
        reject(config, point, "read.length " + std::to_string(read.length) + " does not match read.dataType " +
            read.dataType + " (" + std::to_string(width / 2) + " registers)");
    }
    if (read.byteOrder.size() != static_cast<std::size_t>(width) ||
        !ModbusCodec::isByteOrderPermutation(read.byteOrder)) {
        reject(config, point, "read.byteOrder \"" + read.byteOrder + "\" is not an order of the " +
            std::to_string(width) + " bytes of " + read.dataType);
    }
    if (read.dataType == "bit" && (read.bit < 0 || read.bit > 15)) {
        reject(config, point, "read.dataType bit needs read.bit 0..15");
    }
    if (read.dataType == "bool" && read.bit > 15) {
        reject(config, point, "read.bit must be 0..15");
    }
}

void validateWriteSpec(const DeviceConfig& config, const PointDefinition& point) {
    const auto& write = point.write;
    const auto address = write.address >= 0 ? write.address : point.address;
    if (address < 0 || address > 0xFFFF) {
        reject(config, point, "write address " + std::to_string(address) + " is outside 0..65535");
    }
    if (write.function != 5 && write.function != 6 && write.function != 16) {
        reject(config, point, "write.function " + std::to_string(write.function) + " is not 5, 6 or 16");
    }
    const auto width = ModbusCodec::registerTypeByteWidth(write.dataType);
    if (width == 0) {
        reject(config, point, "unsupported write.dataType \"" + write.dataType + "\"");
    }
    if (write.byteOrder.size() != static_cast<std::size_t>(width) ||
        !ModbusCodec::isByteOrderPermutation(write.byteOrder)) {
        reject(config, point, "write.byteOrder \"" + write.byteOrder + "\" is not an order of the " +
            std::to_string(width) + " bytes of " + write.dataType);
    }
    if (std::abs(write.scale) < 1e-15) {
        reject(config, point, "write.scale must not be zero");
    }
    if (write.minValue && write.maxValue && *write.minValue > *write.maxValue) {
        reject(config, point, "write.min is greater than write.max");
    }
    if (write.function == 5 && !isBitType(write.dataType)) {
        reject(config, point, "function 5 writes a coil; write.dataType must be bit or bool");
    }
    if (write.function == 6 && width != 2) {
        reject(config, point, "function 6 writes one register but write.dataType " + write.dataType +
            " needs " + std::to_string(width / 2) + "; use function 16");
    }
    if (write.function == 6 && isBitType(write.dataType)) {
        if (point.read.bit < 0 || point.read.bit > 15) {
            reject(config, point, "a register bit write (function 6, bit/bool) needs read.bit 0..15");
        }
        if (point.read.byteOrder != "AB" && point.read.byteOrder != "BA") {
            reject(config, point, "a register bit write needs read.byteOrder AB or BA");
        }
    }
    if (write.function == 16 && isBitType(write.dataType)) {
        reject(config, point, "function 16 cannot write a single bit; use function 5 (coil) or 6 (register bit)");
    }
    // The same holding register read and written with different scaling silently writes a different
    // value than the one displayed (and verifyAfterWrite can never pass).
    const auto& read = point.read;
    if (read.enable && read.function == 3 && address == point.address && read.dataType == write.dataType &&
        (std::abs(read.scale - write.scale) > 1e-12 || std::abs(read.offset - write.offset) > 1e-12)) {
        reject(config, point, "write.scale/offset (" + std::to_string(write.scale) + "/" +
            std::to_string(write.offset) + ") differ from read.scale/offset (" + std::to_string(read.scale) +
            "/" + std::to_string(read.offset) + ") of the same register");
    }
}

struct MeterView {
    std::string meterCode;
    int slave = 0;
    const std::vector<PointDefinition>* points = nullptr;
};

std::vector<MeterView> meterViews(const DeviceConfig& config) {
    std::vector<MeterView> views;
    if (config.meters.empty()) {
        views.push_back(MeterView{config.meterCode, config.protocol.slave, &config.points});
        return views;
    }
    for (const auto& meter : config.meters) {
        if (meter.enabled) {
            views.push_back(MeterView{meter.meterCode, meter.slave, &meter.points});
        }
    }
    return views;
}

// Lower bound of the bus time the configured intervals need on one RTU line. Exceeding the line
// means every point refreshes slower than configured (they are still marked stale honestly).
void estimateRtuLoad(const DeviceConfig& config) {
    const auto& transport = config.protocol.transport;
    if (transport.baudRate <= 0) {
        return;
    }
    const auto bitsPerChar = 1 + std::max(5, transport.dataBits) + (transport.parity == "N" ? 0 : 1) +
        std::max(1, transport.stopBits);
    const auto charMs = 1000.0 * bitsPerChar / transport.baudRate;
    const auto silenceMs = transport.baudRate > 19200 ? 1.75 : 3.5 * charMs;
    double busMsPerSecond = 0.0;
    std::size_t requestsPerSecond = 0;
    std::string worstSlave;
    double worstSlaveRatio = 0.0;
    for (const auto& meter : meterViews(config)) {
        std::vector<PointDefinition> readable;
        for (const auto& point : *meter.points) {
            if (point.enabled && point.read.enable && point.read.dataType != "device_online") {
                readable.push_back(point);
            }
        }
        if (readable.empty()) {
            continue;
        }
        const auto tasks = ReadTaskPlanner::build(
            readable, std::max(1, config.collect.maxBatchRegisters), config.collect.batchOptimize,
            config.collect.maxGapRegisters);
        double slaveRequestsPerSecond = 0.0;
        for (const auto& task : tasks) {
            int intervalMs = 0;
            for (const auto& taskPoint : task.points) {
                const auto pointInterval = taskPoint.definition.read.intervalMs > 0
                    ? taskPoint.definition.read.intervalMs : config.collect.defaultIntervalMs;
                intervalMs = intervalMs == 0 ? pointInterval : std::min(intervalMs, pointInterval);
            }
            const auto rate = 1000.0 / std::max(1, intervalMs);
            const auto responseBytes = task.function <= 2 ? 5 + (task.count + 7) / 8 : 5 + task.count * 2;
            const auto transactionMs = (8 + responseBytes) * charMs + 2 * silenceMs + kAssumedTurnaroundMs;
            busMsPerSecond += rate * transactionMs;
            slaveRequestsPerSecond += rate;
        }
        requestsPerSecond += static_cast<std::size_t>(std::lround(slaveRequestsPerSecond));
        // Requests to one slave are at least frameIntervalMs apart.
        if (transport.frameIntervalMs > 0) {
            const auto ratio = slaveRequestsPerSecond * transport.frameIntervalMs / 1000.0;
            if (ratio > worstSlaveRatio) {
                worstSlaveRatio = ratio;
                worstSlave = meter.meterCode + " (slave " + std::to_string(meter.slave) + ")";
            }
        }
    }
    const auto load = busMsPerSecond / 1000.0;
    if (load > 1.0) {
        std::ostringstream message;
        message.setf(std::ios::fixed);
        message.precision(0);
        message << "port " << transport.serialPort << ": configured intervals need about " << load * 100.0
                << "% of the line (" << requestsPerSecond << " requests/s at " << transport.baudRate
                << " baud, assuming " << kAssumedTurnaroundMs << " ms device turnaround); points will refresh"
                << " about ";
        message.precision(1);
        message << load << "x slower than configured. Raise intervalMs, read larger blocks or split the line";
        configWarning(message.str());
    }
    if (worstSlaveRatio > 1.0) {
        std::ostringstream message;
        message.setf(std::ios::fixed);
        message.precision(1);
        message << "port " << transport.serialPort << ": " << worstSlave << " needs " << worstSlaveRatio
                << "x the requests transport.frameIntervalMs=" << transport.frameIntervalMs
                << " allows; its points will refresh slower than configured";
        configWarning(message.str());
    }
}

}  // namespace

void validateModbusDeviceConfig(const DeviceConfig& config) {
    for (const auto* point : configPoints(config)) {
        if (!point->enabled) {
            continue;
        }
        if (point->read.enable && point->read.dataType != "device_online") {
            validateReadSpec(config, *point);
        }
        if (point->write.enable) {
            validateWriteSpec(config, *point);
        }
    }
    if (config.collect.maxGapRegisters > 0 && !config.collect.adaptiveSplitOnFailure) {
        configWarning(config.meterCode + ": collect.maxGapRegisters reads unconfigured registers; without"
                      " adaptiveSplitOnFailure a device that rejects them fails the whole block");
    }
    if (config.protocol.type == "modbus_rtu") {
        static const int kSupportedBaud[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
        const auto baud = config.protocol.transport.baudRate;
        if (std::find(std::begin(kSupportedBaud), std::end(kSupportedBaud), baud) == std::end(kSupportedBaud)) {
            throw std::invalid_argument("transport.baudRate " + std::to_string(baud) + " is not supported");
        }
        if (config.protocol.transport.timeoutMs <= 0) {
            throw std::invalid_argument("transport.timeoutMs must be positive");
        }
        estimateRtuLoad(config);
    }
}

}  // namespace edge_gateway
