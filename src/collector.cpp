#include "edge_gateway/collector.hpp"

#include <algorithm>
#include <utility>
#include <stdexcept>

#include "edge_gateway/dlt645_codec.hpp"
#include "edge_gateway/modbus_codec.hpp"
#include "edge_gateway/read_task_planner.hpp"

namespace edge_gateway {

Collector::Collector(
    DeviceConfig config,
    MemoryPointStore& store,
    std::shared_ptr<IModbusClient> modbusClient,
    std::shared_ptr<Dlt645Client> dlt645Client,
    std::shared_ptr<IMqttPublisher> mqttPublisher
) : config_(std::move(config)),
    store_(store),
    modbusClient_(std::move(modbusClient)),
    dlt645Client_(std::move(dlt645Client)),
    mqttPublisher_(std::move(mqttPublisher)) {
    if (config_.protocol.type == "dlt645_2007") {
        if (!dlt645Client_) {
            throw std::invalid_argument("dlt645Client is required");
        }
    } else if (!modbusClient_) {
        throw std::invalid_argument("modbusClient is required");
    }
    for (const auto& point : config_.points) {
        if (point.enabled) {
            store_.registerPoint(config_.machineCode, config_.meterCode, point);
        }
    }
}

CollectCycleResult Collector::collectOnce(std::int64_t nowMs) {
    CollectCycleResult result;
    if (config_.protocol.type == "dlt645_2007") {
        const auto points = duePoints(nowMs);
        for (const auto& point : points) {
            PointValue value;
            try {
                value = collectDlt645Point(point, nowMs);
            } catch (const std::exception& ex) {
                value = buildFailedPointValue(point, ex.what(), nowMs);
            }
            if (point.read.cachePolicy.storeLatest) {
                store_.putLatest(value);
            }
            result.values.push_back(std::move(value));
        }
        if (!points.empty()) {
            publishDeviceOnlineStatus(true, nowMs);
        }
        if (mqttPublisher_ && !result.values.empty()) {
            mqttPublisher_->publishTelemetry(config_.machineCode, result.values);
        }
        return result;
    }

    const auto points = duePoints(nowMs);
    result.executedTasks = ReadTaskPlanner::build(points, config_.collect.maxBatchRegisters);

    for (const auto& task : result.executedTasks) {
        const auto block = executeReadTask(task);
        if (block.size() < static_cast<std::size_t>(task.count)) {
            throw std::runtime_error("modbus block length is shorter than planned task");
        }
        for (const auto& taskPoint : task.points) {
            const auto& point = taskPoint.definition;
            const auto begin = block.begin() + taskPoint.offset;
            const auto end = begin + point.read.length;
            std::vector<std::uint16_t> slice(begin, end);

            const auto decoded = ModbusCodec::decodeReadValue(slice, point);
            auto value = buildPointValue(point, decoded, nowMs);
            if (point.read.cachePolicy.storeLatest) {
                store_.putLatest(value);
            }
            result.values.push_back(std::move(value));
        }
    }

    if (!result.executedTasks.empty()) {
        publishDeviceOnlineStatus(true, nowMs);
    }

    if (mqttPublisher_ && !result.values.empty()) {
        mqttPublisher_->publishTelemetry(config_.machineCode, result.values);
    }
    return result;
}

PointValue Collector::collectDlt645Point(const PointDefinition& point, std::int64_t nowMs) const {
    if (config_.address.empty()) {
        throw std::runtime_error("DLT645 meter address is empty");
    }
    if (config_.protocol.type != "dlt645_2007") {
        throw std::runtime_error("collectDlt645Point called for non-DLT645 protocol");
    }
    if (point.read.dlt645Di.empty()) {
        throw std::runtime_error("DLT645 point missing read.dlt645.di: " + point.pointCode);
    }
    try {
        const auto response = dlt645Client_->readData(config_.address, point.read.dlt645Di);
        const auto decoded = Dlt645Codec::decodeReadResponse(response, point);
    return buildPointValue(point, decoded, nowMs);
    } catch (const std::exception& ex) {
        throw std::runtime_error(
            std::string("DLT645 read failed address=") + config_.address +
            " di=" + point.read.dlt645Di +
            " pointCode=" + point.pointCode +
            " error=" + ex.what()
        );
    }
}

PointValue Collector::buildFailedPointValue(
    const PointDefinition& point,
    const std::string& message,
    std::int64_t nowMs
) const {
    PointValue value;
    value.index = point.index;
    value.machineCode = config_.machineCode;
    value.meterCode = config_.meterCode;
    value.pointCode = point.pointCode;
    value.pointName = point.name;
    value.category = point.category;
    value.unit = point.read.unit;
    value.value = 0.0;
    value.text = message;
    value.rawHex.clear();
    value.quality = 0;
    value.qualityMsg = message;
    value.ts = nowMs;
    value.expireAt = nowMs + point.read.cachePolicy.ttlMs;
    value.stale = false;
    value.function = point.read.function;
    value.address = point.address;
    value.length = point.read.length;
    value.isStore = false;
    value.persistIntervalSec = point.persistIntervalSec;
    return value;
}

std::vector<PointDefinition> Collector::duePoints(std::int64_t nowMs) {
    std::vector<PointDefinition> points;
    points.reserve(config_.points.size());
    for (const auto& point : config_.points) {
        if (!point.enabled || !point.read.enable || point.read.dataType == "device_online") {
            continue;
        }
        const auto intervalMs = effectiveIntervalMs(point);
        const auto last = lastReadMs_.find(point.index);
        if (last != lastReadMs_.end() && nowMs - last->second < intervalMs) {
            continue;
        }
        lastReadMs_[point.index] = nowMs;
        points.push_back(point);
    }
    return points;
}

int Collector::effectiveIntervalMs(const PointDefinition& point) const {
    if (point.read.intervalMs > 0) {
        return std::max(100, point.read.intervalMs);
    }
    return std::max(100, config_.collect.defaultIntervalMs);
}

void Collector::publishDeviceOnlineStatus(bool online, std::int64_t nowMs) const {
    for (const auto& point : config_.points) {
        if (!point.enabled || !point.read.enable || point.read.dataType != "device_online") {
            continue;
        }
        auto value = buildDeviceOnlineValue(point, online, nowMs);
        if (point.read.cachePolicy.storeLatest) {
            store_.putLatest(value);
        }
    }
}

std::vector<std::uint16_t> Collector::executeReadTask(const ReadTask& task) const {
    switch (task.function) {
        case 3:
            return modbusClient_->readHoldingRegisters(config_.protocol.slave, task.start, task.count);
        case 4:
            return modbusClient_->readInputRegisters(config_.protocol.slave, task.start, task.count);
        default:
            throw std::invalid_argument("unsupported read function: " + std::to_string(task.function));
    }
}

PointValue Collector::buildPointValue(
    const PointDefinition& point,
    const DecodedValue& decoded,
    std::int64_t nowMs
) const {
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
    value.ts = nowMs;
    value.expireAt = nowMs + point.read.cachePolicy.ttlMs;
    value.function = point.read.function;
    value.address = point.address;
    value.length = point.read.length;
    value.isStore = point.isStore;
    value.persistIntervalSec = point.persistIntervalSec;
    return value;
}

PointValue Collector::buildDeviceOnlineValue(
    const PointDefinition& point,
    bool online,
    std::int64_t nowMs
) const {
    PointValue value;
    value.index = point.index;
    value.machineCode = config_.machineCode;
    value.meterCode = config_.meterCode;
    value.pointCode = point.pointCode;
    value.pointName = point.name;
    value.category = point.category;
    value.unit = point.read.unit;
    value.value = online ? 1.0 : 0.0;
    value.text = online ? "online" : "offline";
    value.rawHex.clear();
    value.quality = 1;
    value.qualityMsg = "ok";
    value.ts = nowMs;
    value.expireAt = nowMs + point.read.cachePolicy.ttlMs;
    value.function = 0;
    value.address = point.address;
    value.length = 0;
    value.isStore = point.isStore;
    value.persistIntervalSec = point.persistIntervalSec;
    return value;
}

}  // namespace edge_gateway
