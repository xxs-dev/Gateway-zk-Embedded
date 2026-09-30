#include "edge_gateway/can_signal_codec.hpp"
#include "edge_gateway/can_driver_service.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/virtual_can_datagram.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireNear(double actual, double expected, double tolerance, const char* message) {
    if (std::fabs(actual - expected) > tolerance) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    using namespace edge_gateway;

    ReadSpec littleEndian;
    littleEndian.dataType = "uint16";
    littleEndian.scale = 0.5;
    littleEndian.can.frameId = "0x18FF50E5";
    littleEndian.can.extended = true;
    littleEndian.can.byteOffset = 1;
    littleEndian.can.bitOffset = 0;
    littleEndian.can.bitLength = 16;
    littleEndian.can.endian = "little";

    const std::vector<std::uint8_t> frame{0x00, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00};
    const auto decoded = CanSignalCodec::decode(frame, littleEndian);
    requireNear(decoded.value, 0x1234 * 0.5, 0.0001, "uint16 little endian decode failed");
    require(decoded.rawHex == "3412", "raw hex should include selected bytes");

    ReadSpec bitSignal;
    bitSignal.dataType = "bool";
    bitSignal.can.byteOffset = 0;
    bitSignal.can.bitOffset = 3;
    bitSignal.can.bitLength = 1;
    bitSignal.can.bitOrder = "lsb0";
    requireNear(CanSignalCodec::decode(std::vector<std::uint8_t>{0x08}, bitSignal).value, 1.0, 0.0001, "lsb bit decode failed");

    WriteSpec writeSpec;
    writeSpec.enable = true;
    writeSpec.dataType = "uint16";
    writeSpec.scale = 0.5;
    writeSpec.can.frameId = "0x123";
    writeSpec.can.extended = false;
    writeSpec.can.dlc = 8;
    writeSpec.can.byteOffset = 2;
    writeSpec.can.bitOffset = 0;
    writeSpec.can.bitLength = 16;
    writeSpec.can.endian = "big";
    const auto encoded = CanSignalCodec::encode(100.0, writeSpec);
    require(encoded.frameId == 0x123, "frame id parse failed");
    require(!encoded.extended, "standard frame should not be extended");
    require(encoded.payload.size() == 8, "encoded dlc should be 8");
    require(encoded.payload[2] == 0x00 && encoded.payload[3] == 0xC8, "uint16 big endian encode failed");

    // Other signals of the same frame are preserved when a base payload is given.
    const std::vector<std::uint8_t> shadow{0xAA, 0x55, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44};
    const auto merged = CanSignalCodec::encode(100.0, writeSpec, shadow);
    require(merged.payload[0] == 0xAA && merged.payload[1] == 0x55 && merged.payload[7] == 0x44,
        "CAN write must preserve other signals from the base payload");
    require(merged.payload[2] == 0x00 && merged.payload[3] == 0xC8, "CAN merged write encode failed");

    bool rangeRejected = false;
    try {
        (void)CanSignalCodec::encode(70000.0, writeSpec);  // raw 140000 > 0xFFFF
    } catch (const std::out_of_range&) {
        rangeRejected = true;
    }
    require(rangeRejected, "CAN write outside the signal bit range must be rejected, not truncated");

    // A uint16 signal flagged signed on the read side keeps accepting negative
    // (two's complement) setpoints.
    auto signedWrite = writeSpec;
    signedWrite.scale = 1.0;
    const auto negativeSetpoint = CanSignalCodec::encode(-100.0, signedWrite, std::vector<std::uint8_t>(), true);
    require(negativeSetpoint.payload[2] == 0xFF && negativeSetpoint.payload[3] == 0x9C,
        "signed CAN write must encode two's complement");

    ReadSpec floatSignal;
    floatSignal.dataType = "float32";
    floatSignal.can.byteOffset = 0;
    floatSignal.can.bitLength = 32;
    bool nanRejected = false;
    try {
        (void)CanSignalCodec::decode(std::vector<std::uint8_t>{0x00, 0x00, 0xC0, 0x7F}, floatSignal);
    } catch (const std::runtime_error&) {
        nanRejected = true;
    }
    require(nanRejected, "CAN float32 NaN must not be published as a value");

    VirtualCanFrame virtualFrame;
    virtualFrame.frameId = 0x18FF50E5U;
    virtualFrame.extended = true;
    virtualFrame.payload = {0x12, 0x34, 0x56, 0x78};
    const auto datagram = encodeVirtualCanDatagram(virtualFrame);
    VirtualCanFrame decodedFrame;
    require(decodeVirtualCanDatagram(datagram, decodedFrame), "virtual CAN datagram decode failed");
    require(decodedFrame.frameId == virtualFrame.frameId, "virtual CAN frame id mismatch");
    require(decodedFrame.extended, "virtual CAN extended flag mismatch");
    require(decodedFrame.payload == virtualFrame.payload, "virtual CAN payload mismatch");
    auto malformedDatagram = datagram;
    malformedDatagram[6] = static_cast<std::uint8_t>(malformedDatagram[6] + 1U);
    require(!decodeVirtualCanDatagram(malformedDatagram, decodedFrame),
            "virtual CAN malformed payload length must be rejected");

    DeviceConfig config;
    config.machineCode = "CAN_INIT_TEST";
    config.protocol.type = "can_socketcan";
    config.memoryStore.sharedMemoryName = "can_signal_codec_test_online_init";
    config.memoryStore.maxLatestPoints = 16;
    config.memoryStore.maxPendingWrites = 4;
    config.memoryStore.maxPersistentSamples = 4;
    config.memoryStore.sqlitePath.clear();
    LogicalDeviceConfig logicalDevice;
    logicalDevice.meterCode = "CAN_DEVICE";
    logicalDevice.deviceName = "CAN online initialization test";
    logicalDevice.onlineTimeoutMs = 1000;
    PointDefinition onlinePoint;
    onlinePoint.index = 310000;
    onlinePoint.pointCode = "device_online";
    onlinePoint.name = "设备在线状态";
    onlinePoint.enabled = true;
    onlinePoint.read.enable = true;
    onlinePoint.read.dataType = "device_online";
    onlinePoint.read.cachePolicy.ttlMs = 60000;
    logicalDevice.points.push_back(onlinePoint);
    config.meters.push_back(logicalDevice);

    MemoryPointStore::cleanupOrphanedSegment(config.memoryStore.sharedMemoryName);
    {
        MemoryPointStore store(config.memoryStore);
        CanDriverService service(config, store);
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        const auto initialOnline = store.getLatestByIndex(310000, nowMs);
        require(static_cast<bool>(initialOnline), "CAN online point must exist after service initialization");
        requireNear(initialOnline->value, 0.0, 0.0001, "CAN online point must initialize as offline");
        require(initialOnline->quality == 1, "CAN initial offline point must have good quality");

        const auto heartbeatTs = nowMs + 1001;
        service.updateOnlineStatus(heartbeatTs);
        const auto refreshedOnline = store.getLatestByIndex(310000, heartbeatTs);
        require(static_cast<bool>(refreshedOnline), "CAN online point must remain available while state is stable");
        requireNear(refreshedOnline->value, 0.0, 0.0001, "CAN stable offline heartbeat value mismatch");
        require(refreshedOnline->ts == heartbeatTs, "CAN online point heartbeat must refresh its timestamp");
    }
    MemoryPointStore::cleanupOrphanedSegment(config.memoryStore.sharedMemoryName);

    config.memoryStore.sharedMemoryName = "can_signal_codec_test_stale_generation";
    config.meters[0].points.clear();
    PointDefinition controlPoint;
    controlPoint.index = 310001;
    controlPoint.pointCode = "active_power_setpoint";
    controlPoint.name = "有功功率设定";
    controlPoint.enabled = true;
    controlPoint.write = writeSpec;
    config.meters[0].points.push_back(controlPoint);
#ifdef _WIN32
    const std::string ownershipFile = std::tmpnam(nullptr);
#else
    const std::string ownershipFile = "/tmp/can-signal-codec-power-owner.json";
#endif
    config.mqttDriver.powerControlOwnershipFile = ownershipFile;
    config.mqttDriver.priorityControlLeaseFile = ownershipFile + ".priority";
    std::remove(ownershipFile.c_str());
    std::remove((ownershipFile + ".lock").c_str());
    std::remove(config.mqttDriver.priorityControlLeaseFile.c_str());
    MemoryPointStore::cleanupOrphanedSegment(config.memoryStore.sharedMemoryName);
    {
        MemoryPointStore store(config.memoryStore);
        PowerControlOwnership remote(ownershipFile, "mqtt-forwarder");
        const auto takeover = remote.acquireOrRenew(
            "pcs-power", "third-party", {310001}, "CAN_REMOTE", 1000, 15000
        );
        require(takeover.accepted && takeover.generation != 0, "CAN takeover setup failed");
        require(
            remote.recordReceipt("third-party", "CAN_REMOTE", takeover.generation, true),
            "CAN takeover receipt setup failed"
        );

        CanDriverService service(config, store);
        PendingWriteCommand directLocal;
        directLocal.cmdId = "CAN_DIRECT_LOCAL";
        directLocal.index = 310001;
        directLocal.value = 5.0;
        directLocal.source = "compute-engine";
        directLocal.ts = 1000;
        directLocal.acceptedAt = 1000;
        store.submitWriteCommand(directLocal);
        require(
            service.processWritebackOnce(1000) == 0,
            "generation-zero CAN write must not bypass active ownership"
        );
        const auto directLocalResult = store.getWritebackResult(directLocal.cmdId);
        require(
            directLocalResult && !directLocalResult->success &&
                directLocalResult->stage == "control-rejected",
            "direct CAN write must be rejected before field I/O"
        );

        PendingWriteCommand command;
        command.cmdId = "CAN_REMOTE";
        command.index = 310001;
        command.value = 10.0;
        command.source = "mqtt-forwarder";
        command.ts = 1000;
        command.acceptedAt = 1000;
        command.controlGeneration = takeover.generation;
        store.submitWriteCommand(command);
        require(
            remote.releaseAndAdvance("third-party") != 0,
            "CAN takeover release must advance generation"
        );

        require(service.processWritebackOnce(1001) == 0, "stale CAN control must not execute");
        const auto result = store.getWritebackResult("CAN_REMOTE");
        require(result && !result->success, "stale CAN control must report failure");
        require(result->stage == "control-rejected", "stale CAN control must fail at ownership check");

        PendingWriteCommand zeroGenerationRemote;
        zeroGenerationRemote.cmdId = "CAN_REMOTE_ZERO_GENERATION";
        zeroGenerationRemote.index = 310001;
        zeroGenerationRemote.value = 11.0;
        zeroGenerationRemote.source = "mqtt-forwarder";
        zeroGenerationRemote.ts = 1002;
        zeroGenerationRemote.acceptedAt = 1002;
        store.submitWriteCommand(zeroGenerationRemote);
        require(
            service.processWritebackOnce(1002) == 0,
            "released zero-generation CAN control must not execute"
        );
        const auto zeroGenerationResult = store.getWritebackResult(zeroGenerationRemote.cmdId);
        require(
            zeroGenerationResult && !zeroGenerationResult->success &&
                zeroGenerationResult->stage == "control-rejected",
            "released zero-generation CAN control must fail ownership validation"
        );
    }
    MemoryPointStore::cleanupOrphanedSegment(config.memoryStore.sharedMemoryName);
    std::remove(ownershipFile.c_str());
    std::remove((ownershipFile + ".lock").c_str());
    std::remove(config.mqttDriver.priorityControlLeaseFile.c_str());

    return 0;
}
