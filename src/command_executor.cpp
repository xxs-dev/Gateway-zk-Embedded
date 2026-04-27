#include "edge_gateway/command_executor.hpp"

#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>
#include <utility>

#include "edge_gateway/modbus_codec.hpp"

namespace edge_gateway {

CommandExecutor::CommandExecutor(
    DeviceConfig config,
    MemoryPointStore& store,
    std::shared_ptr<IModbusClient> modbusClient,
    std::shared_ptr<IMqttPublisher> mqttPublisher
) : config_(std::move(config)),
    store_(store),
    modbusClient_(std::move(modbusClient)),
    mqttPublisher_(std::move(mqttPublisher)) {
    if (config_.protocol.type != "dlt645_2007" && !modbusClient_) {
        throw std::invalid_argument("modbusClient is required");
    }
    for (const auto& point : config_.points) {
        if (point.enabled) {
            store_.registerPoint(config_.machineCode, config_.meterCode, point);
        }
    }
}

CommandResult CommandExecutor::execute(const CommandRequest& request, std::int64_t nowMs) const {
    CommandResult result;
    result.cmdId = request.cmdId;
    result.machineCode = request.machineCode;
    result.meterCode = request.meterCode;
    result.pointCode = request.pointCode;
    result.ts = nowMs;
    result.requestedValue = request.value;

    try {
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
        dispatchWrite(point, encoded);

        if (point.write.verifyAfterWrite && point.write.verifyByRead) {
            result.verifyAttempted = true;
            verifyWrite(request, point, nowMs);
            result.verifyPassed = true;
        }

        result.success = true;
        result.message = "ok";
    } catch (const std::exception& ex) {
        result.success = false;
        result.message = ex.what();
    }

    if (mqttPublisher_) {
        mqttPublisher_->publishCommandResult(result);
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
    const std::vector<std::uint16_t>& encoded
) const {
    switch (point.write.function) {
        case 6:
            if (encoded.size() != 1) {
                throw std::invalid_argument("function 6 requires one register");
            }
            modbusClient_->writeSingleRegister(config_.protocol.slave, point.address, encoded.front());
            return;
        case 16:
            modbusClient_->writeMultipleRegisters(config_.protocol.slave, point.address, encoded);
            return;
        default:
            throw std::invalid_argument("unsupported write function: " + std::to_string(point.write.function));
    }
}

void CommandExecutor::verifyWrite(
    const CommandRequest& request,
    const PointDefinition& point,
    std::int64_t nowMs
) const {
    if (!point.read.enable) {
        throw std::invalid_argument("verify requires read capability");
    }

    if (point.write.verifyDelayMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(point.write.verifyDelayMs));
    }

    std::vector<std::uint16_t> registers;
    switch (point.read.function) {
        case 3:
            registers = modbusClient_->readHoldingRegisters(
                config_.protocol.slave,
                point.address,
                point.read.length
            );
            break;
        case 4:
            registers = modbusClient_->readInputRegisters(
                config_.protocol.slave,
                point.address,
                point.read.length
            );
            break;
        default:
            throw std::invalid_argument("unsupported verify read function");
    }

    const auto decoded = ModbusCodec::decodeReadValue(registers, point);
    if (std::abs(decoded.value - request.value) > 1e-6) {
        throw std::runtime_error("verify failed");
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
    value.persistIntervalSec = point.persistIntervalSec;

    if (point.read.cachePolicy.storeLatest) {
        store_.putLatest(value);
    }
}

}  // namespace edge_gateway
