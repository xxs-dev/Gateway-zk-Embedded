#include "edge_gateway/command_executor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

#include "edge_gateway/modbus_codec.hpp"
#include "edge_gateway/modbus_error.hpp"

namespace edge_gateway {

namespace {

// The request reached the device side but no conclusive reply came back (timeout, broken reply,
// lost connection, or an acknowledged write whose verification read failed). The device may have
// executed it: report CommandResult.outcomeUnknown and never retry under a new cmdId.
class WriteOutcomeUnknown : public std::runtime_error {
public:
    explicit WriteOutcomeUnknown(const std::string& message) : std::runtime_error(message) {}
};

template <typename Write>
void physicalWrite(Write write) {
    try {
        write();
    } catch (const std::exception& ex) {
        if (isWriteOutcomeUnknown(ex)) {
            throw WriteOutcomeUnknown(std::string("write outcome unknown: ") + ex.what());
        }
        throw;
    }
}

bool isFloatType(const std::string& dataType) {
    return dataType == "float32" || dataType == "float64";
}

// Half a quantization step of the read decode: the readback cannot be closer to the written value.
double readbackTolerance(const PointDefinition& point, double expected) {
    const auto relative = 1e-9 * std::max(1.0, std::abs(expected));
    if (point.read.dataType == "float32") {
        return std::abs(expected) * 2.4e-7 + relative;
    }
    if (point.read.dataType == "float64") {
        return relative;
    }
    return 0.5 * std::abs(point.read.scale) + relative;
}

bool isBitWrite(const PointDefinition& point) {
    return point.write.dataType == "bit" || point.write.dataType == "bool";
}

int writeAddressOf(const PointDefinition& point) {
    return point.write.address >= 0 ? point.write.address : point.address;
}

class ModbusPriorityWriteGuard {
public:
    explicit ModbusPriorityWriteGuard(const std::shared_ptr<IModbusClient>& client) : client_(client) {
        if (client_) {
            client_->beginPriorityWrite();
        }
    }

    ~ModbusPriorityWriteGuard() {
        if (client_) {
            client_->endPriorityWrite();
        }
    }

    ModbusPriorityWriteGuard(const ModbusPriorityWriteGuard&) = delete;
    ModbusPriorityWriteGuard& operator=(const ModbusPriorityWriteGuard&) = delete;

private:
    std::shared_ptr<IModbusClient> client_;
};

}  // namespace

CommandExecutor::CommandExecutor(
    DeviceConfig config,
    MemoryPointStore& store,
    std::shared_ptr<IModbusClient> modbusClient,
    std::shared_ptr<IMqttPublisher> mqttPublisher,
    std::shared_ptr<IGpioPort> gpioPort
) : config_(std::move(config)),
    store_(store),
    modbusClient_(std::move(modbusClient)),
    mqttPublisher_(std::move(mqttPublisher)),
    gpioPort_(std::move(gpioPort)) {
    if (config_.protocol.type == "local_dio") {
        if (!gpioPort_) {
            throw std::invalid_argument("gpioPort is required");
        }
    } else if (config_.protocol.type != "dlt645_2007" && !modbusClient_) {
        throw std::invalid_argument("modbusClient is required");
    }
    store_.registerPoints(config_.machineCode, config_.meterCode, config_.points);
}

CommandResult CommandExecutor::execute(const CommandRequest& request, std::int64_t nowMs) const {
    return execute(request, nowMs, BeforePhysicalWrite());
}

CommandResult CommandExecutor::executePending(const PendingWriteCommand& command, std::int64_t nowMs,
                                            const BeforePhysicalWrite& beforeWrite) const {
    BeforePhysicalWrite check = beforeWrite;
    if (clusterAuthorizationRequired(config_.emsCluster, command)) {
        auto guard = std::make_shared<ClusterWriteGuard>(config_.emsCluster);
        check = [guard, command, beforeWrite] {
            guard->check(command);
            if (beforeWrite) beforeWrite();
        };
    }
    const auto& point = findPointByIndex(command.index);
    return execute(CommandRequest{command.cmdId, config_.machineCode, config_.meterCode,
                                  point.pointCode, command.value}, nowMs, check);
}

CommandResult CommandExecutor::execute(const CommandRequest& request, std::int64_t nowMs,
                                       const BeforePhysicalWrite& beforeWrite) const {
    CommandResult result;
    result.cmdId = request.cmdId;
    result.machineCode = request.machineCode;
    result.meterCode = request.meterCode;
    result.pointCode = request.pointCode;
    result.ts = nowMs;
    result.requestedValue = request.value;

    try {
        if (beforeWrite && config_.protocol.type != "modbus_tcp" && config_.protocol.type != "modbus_rtu")
            throw std::runtime_error("cluster physical write requires supported Modbus transport");
        if (config_.protocol.type == "local_dio") {
            const auto& point = findPoint(request.pointCode);
            result.index = point.index;
            result.verifyAttempted = point.write.verifyAfterWrite && point.write.verifyByRead;
            dispatchLocalDioWrite(point, request.value, nowMs);
            result.verifyPassed = result.verifyAttempted;
            result.success = true;
            result.message = "ok";
            if (mqttPublisher_) {
                try {
                    mqttPublisher_->publishCommandResult(result);
                } catch (const std::exception& ex) {
                    std::cerr << "legacy command result publish failed"
                              << " cmdId=" << result.cmdId
                              << " error=" << ex.what()
                              << std::endl;
                } catch (...) {
                    std::cerr << "legacy command result publish failed"
                              << " cmdId=" << result.cmdId
                              << " error=unknown"
                              << std::endl;
                }
            }
            return result;
        }
        if (config_.protocol.type == "dlt645_2007") {
            throw std::invalid_argument("unsupported write for dlt645_2007");
        }
        const auto& point = findPoint(request.pointCode);
        result.index = point.index;
        const auto validation = ModbusCodec::validateWriteValue(request.value, point);
        if (!validation.ok) {
            throw std::invalid_argument(validation.message);
        }

        const auto encoded = ModbusCodec::encodeWriteValue(request.value, point);
        ModbusPriorityWriteGuard priority(modbusClient_);
        dispatchWrite(point, encoded, beforeWrite);

        if (point.write.verifyAfterWrite && point.write.verifyByRead) {
            result.verifyAttempted = true;
            verifyWrite(request, point, encoded, nowMs);
            result.verifyPassed = true;
        }

        result.success = true;
        result.message = "ok";
    } catch (const WriteOutcomeUnknown& ex) {
        result.success = false;
        result.outcomeUnknown = true;
        result.message = ex.what();
    } catch (const std::exception& ex) {
        result.success = false;
        result.message = ex.what();
    }

    if (mqttPublisher_) {
        try {
            mqttPublisher_->publishCommandResult(result);
        } catch (const std::exception& ex) {
            std::cerr << "legacy command result publish failed"
                      << " cmdId=" << result.cmdId
                      << " error=" << ex.what()
                      << std::endl;
        } catch (...) {
            std::cerr << "legacy command result publish failed"
                      << " cmdId=" << result.cmdId
                      << " error=unknown"
                      << std::endl;
        }
    }
    return result;
}

CommandResult CommandExecutor::executeByIndex(
    const std::string& cmdId,
    std::uint32_t index,
    double value,
    std::int64_t nowMs
) const {
    const auto& point = findPointByIndex(index);
    return execute(
        CommandRequest{cmdId, config_.machineCode, config_.meterCode, point.pointCode, value},
        nowMs
    );
}

const PointDefinition& CommandExecutor::findPoint(const std::string& pointCode) const {
    for (const auto& point : config_.points) {
        if (point.pointCode == pointCode) {
            return point;
        }
    }
    throw std::invalid_argument("point not found: " + pointCode);
}

const PointDefinition& CommandExecutor::findPointByIndex(std::uint32_t index) const {
    for (const auto& point : config_.points) {
        if (point.index == index) {
            return point;
        }
    }
    throw std::invalid_argument("point index not found: " + std::to_string(index));
}

void CommandExecutor::dispatchWrite(
    const PointDefinition& point,
    const std::vector<std::uint16_t>& encoded,
    const BeforePhysicalWrite& beforeWrite
) const {
    const auto slave = config_.protocol.slave;
    const auto address = writeAddressOf(point);
    switch (point.write.function) {
        case 5:
            if (encoded.size() != 1) {
                throw std::invalid_argument("function 5 requires one coil value");
            }
            physicalWrite([&] { modbusClient_->writeSingleCoil(slave, address, encoded.front() != 0, beforeWrite); });
            return;
        case 6: {
            if (encoded.size() != 1) {
                throw std::invalid_argument("function 6 requires one register");
            }
            // Read-modify-write of one bit happens before the write; a failure there is a plain failure.
            const auto value = isBitWrite(point) ? mergeBitWrite(point, encoded.front() != 0) : encoded.front();
            physicalWrite([&] { modbusClient_->writeSingleRegister(slave, address, value, beforeWrite); });
            return;
        }
        case 16:
            physicalWrite([&] { modbusClient_->writeMultipleRegisters(slave, address, encoded, beforeWrite); });
            return;
        default:
            throw std::invalid_argument("unsupported write function: " + std::to_string(point.write.function));
    }
}

void CommandExecutor::dispatchLocalDioWrite(
    const PointDefinition& point,
    double value,
    std::int64_t nowMs
) const {
    if (!point.write.enable) {
        throw std::invalid_argument("point write is disabled");
    }
    if (point.read.dataType != "digital_output" && point.write.dataType != "digital_output") {
        throw std::invalid_argument("local_dio write requires digital_output point");
    }
    if (point.read.gpio < 0) {
        throw std::invalid_argument("local_dio point missing read.gpio");
    }
    if (std::abs(value - 0.0) > 1e-9 && std::abs(value - 1.0) > 1e-9) {
        throw std::invalid_argument("local_dio write value must be 0 or 1");
    }
    if (point.write.minValue && value < *point.write.minValue) {
        throw std::invalid_argument("value below min");
    }
    if (point.write.maxValue && value > *point.write.maxValue) {
        throw std::invalid_argument("value above max");
    }
    if (!point.write.allowedValues.empty()) {
        const auto match = std::find_if(
            point.write.allowedValues.begin(),
            point.write.allowedValues.end(),
            [value](double candidate) {
                return std::abs(candidate - value) <= 1e-9;
            }
        );
        if (match == point.write.allowedValues.end()) {
            throw std::invalid_argument("value is not in allowedValues");
        }
    }

    const auto logicalHigh = value > 0.0;
    const auto gpioHigh = point.read.activeHigh ? logicalHigh : !logicalHigh;
    gpioPort_->exportGpio(point.read.gpio);
    gpioPort_->setDirection(point.read.gpio, "out");
    gpioPort_->writeValue(point.read.gpio, gpioHigh);

    if (point.write.verifyAfterWrite && point.write.verifyByRead) {
        if (point.write.verifyDelayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(point.write.verifyDelayMs));
        }
        const auto actualGpioHigh = gpioPort_->readValue(point.read.gpio);
        const auto actualValue = actualGpioHigh == point.read.activeHigh ? 1.0 : 0.0;
        if (std::abs(actualValue - value) > 1e-9) {
            throw std::runtime_error("verify failed");
        }
    }

    PointValue latest;
    latest.index = point.index;
    latest.machineCode = config_.machineCode;
    latest.meterCode = config_.meterCode;
    latest.pointCode = point.pointCode;
    latest.pointName = point.name;
    latest.category = point.category;
    latest.unit = point.read.unit;
    latest.value = value;
    latest.text = value > 0.0 ? "1" : "0";
    latest.rawHex = gpioHigh ? "01" : "00";
    latest.quality = 1;
    latest.qualityMsg = "ok";
    latest.ts = nowMs;
    latest.expireAt = nowMs + point.read.cachePolicy.ttlMs;
    latest.function = 0;
    latest.address = point.address;
    latest.length = 1;
    latest.isStore = point.isStore;
    latest.persistOnChange = point.persistOnChange;
    latest.persistIntervalSec = point.persistIntervalSec;
    store_.putLatest(latest);
}

std::uint16_t CommandExecutor::mergeBitWrite(const PointDefinition& point, bool value) const {
    if (point.read.bit < 0 || point.read.bit > 15) {
        throw std::invalid_argument("register bit write requires read.bit between 0 and 15");
    }

    const auto registers = modbusClient_->readHoldingRegisters(config_.protocol.slave, writeAddressOf(point), 1);
    if (registers.empty()) {
        throw std::runtime_error("register bit write readback returned no data");
    }
    // The bit index is defined on the register as the read side decodes it (read.byteOrder).
    return ModbusCodec::setRegisterBit(registers.front(), point.read.bit, value, point.read.byteOrder);
}

void CommandExecutor::verifyWrite(
    const CommandRequest& request,
    const PointDefinition& point,
    const std::vector<std::uint16_t>& encoded,
    std::int64_t nowMs
) const {
    if (!point.read.enable) {
        throw std::invalid_argument("verify requires read capability");
    }

    if (point.write.verifyDelayMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(point.write.verifyDelayMs));
    }

    std::vector<std::uint16_t> registers;
    try {
        switch (point.read.function) {
            case 1:
                registers = modbusClient_->readCoils(config_.protocol.slave, point.address, point.read.length);
                break;
            case 2:
                registers = modbusClient_->readDiscreteInputs(config_.protocol.slave, point.address, point.read.length);
                break;
            case 3:
                registers = modbusClient_->readHoldingRegisters(config_.protocol.slave, point.address, point.read.length);
                break;
            case 4:
                registers = modbusClient_->readInputRegisters(config_.protocol.slave, point.address, point.read.length);
                break;
            default:
                throw std::invalid_argument("unsupported verify read function");
        }
    } catch (const std::invalid_argument&) {
        throw;
    } catch (const std::exception& ex) {
        // The device acknowledged the write; only the confirmation is missing.
        throw WriteOutcomeUnknown(std::string("write acknowledged, verify read failed: ") + ex.what());
    }

    const auto decoded = ModbusCodec::decodeReadValue(registers, point);
    // Compare against what was actually written (after scaling, rounding and float32 encoding), not the
    // requested value: 230.7 written as float32 reads back as 230.699997.
    bool matches = false;
    double expected = request.value;
    if (isBitWrite(point)) {
        matches = (decoded.value != 0.0) == (request.value != 0.0);
    } else {
        const bool sameRegisters = (point.read.function == 3 || point.read.function == 4) &&
            point.address == writeAddressOf(point) &&
            registers.size() == encoded.size() &&
            point.read.dataType == point.write.dataType &&
            point.read.byteOrder == point.write.byteOrder;
        if (sameRegisters && std::equal(registers.begin(), registers.end(), encoded.begin())) {
            matches = true;
        } else {
            PointDefinition written = point;
            written.read.dataType = point.write.dataType;
            written.read.length = static_cast<int>(encoded.size());
            written.read.byteOrder = point.write.byteOrder;
            written.read.scale = point.write.scale;
            written.read.offset = point.write.offset;
            written.read.bit = -1;
            expected = ModbusCodec::decodeReadValue(encoded, written).value;
            matches = std::abs(decoded.value - expected) <= readbackTolerance(point, expected) ||
                (isFloatType(point.read.dataType) && decoded.value == expected);
        }
    }
    if (!matches) {
        throw std::runtime_error("verify failed: wrote " + std::to_string(expected) +
                                 ", read back " + std::to_string(decoded.value));
    }

    PointValue value;
    value.index = point.index;
    value.machineCode = config_.machineCode;
    value.meterCode = config_.meterCode;
    value.pointCode = point.pointCode;
    value.pointName = point.name;
    value.category = point.category;
    value.unit = point.read.unit;
    value.value = decoded.value;
    value.text = decoded.text;
    value.rawHex = decoded.rawHex;
    value.ts = nowMs + point.write.verifyDelayMs;
    value.expireAt = value.ts + point.read.cachePolicy.ttlMs;
    value.function = point.read.function;
    value.address = point.address;
    value.length = point.read.length;
    value.isStore = point.isStore;
    value.persistOnChange = point.persistOnChange;
    value.persistIntervalSec = point.persistIntervalSec;

    if (point.read.cachePolicy.storeLatest) {
        store_.putLatest(value);
    }
}

}  // namespace edge_gateway
