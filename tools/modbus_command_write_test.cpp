// driver-review: Modbus write results. Verify compares what was written (float32 rounding), register bit
// writes honor byteOrder, and a request that reached the device without a conclusive reply is reported
// as outcomeUnknown instead of a plain failure.
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include "edge_gateway/command_executor.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/modbus_error.hpp"

using namespace edge_gateway;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class RegisterClient : public IModbusClient {
public:
    std::map<int, std::uint16_t> registers;
    enum class Fault { None, TimeoutAfterSend, ExceptionReply, RefusedBeforeSend, VerifyReadTimeout, IgnoreWrite };
    Fault writeFault = Fault::None;
    bool failReads = false;
    int writes = 0;

    std::vector<std::uint16_t> read(int start, int count) {
        if (failReads) throw ModbusError(ModbusFailureKind::Timeout, "modbus response timeout", true);
        std::vector<std::uint16_t> out;
        for (int i = 0; i < count; ++i) out.push_back(registers[start + i]);
        return out;
    }
    std::vector<std::uint16_t> readCoils(int, int start, int count) override { return read(start, count); }
    std::vector<std::uint16_t> readDiscreteInputs(int, int start, int count) override { return read(start, count); }
    std::vector<std::uint16_t> readHoldingRegisters(int, int start, int count) override { return read(start, count); }
    std::vector<std::uint16_t> readInputRegisters(int, int start, int count) override { return read(start, count); }
    void writeSingleCoil(int, int address, bool value) override { store(address, {static_cast<std::uint16_t>(value)}); }
    void writeSingleRegister(int, int address, std::uint16_t value) override { store(address, {value}); }
    void writeMultipleRegisters(int, int address, const std::vector<std::uint16_t>& values) override { store(address, values); }

private:
    void store(int address, const std::vector<std::uint16_t>& values) {
        ++writes;
        switch (writeFault) {
            case Fault::RefusedBeforeSend:
                throw ModbusError(ModbusFailureKind::Transport, "modbus tcp connect failed", false);
            case Fault::ExceptionReply:
                throw ModbusError(ModbusFailureKind::ProtocolException, "modbus exception code 3", true);
            case Fault::IgnoreWrite:
                return;
            default:
                break;
        }
        for (std::size_t i = 0; i < values.size(); ++i) registers[address + static_cast<int>(i)] = values[i];
        if (writeFault == Fault::TimeoutAfterSend) {
            throw ModbusError(ModbusFailureKind::Timeout, "modbus response timeout", true);
        }
        if (writeFault == Fault::VerifyReadTimeout) failReads = true;
    }
};

PointDefinition writablePoint(std::uint32_t index, const std::string& code, int address, const std::string& type) {
    PointDefinition point;
    point.index = index;
    point.pointCode = code;
    point.name = code;
    point.enabled = true;
    point.address = address;
    const int registers = type == "uint16" || type == "int16" || type == "bit" || type == "bool" ? 1 : 2;
    point.read.enable = true;
    point.read.function = 3;
    point.read.length = registers;
    point.read.dataType = type;
    point.read.byteOrder = registers == 1 ? "AB" : "ABCD";
    point.read.cachePolicy.storeLatest = true;
    point.read.cachePolicy.ttlMs = 600000;
    point.write.enable = true;
    point.write.function = registers == 1 ? 6 : 16;
    point.write.length = registers;
    point.write.dataType = type;
    point.write.byteOrder = point.read.byteOrder;
    point.write.verifyAfterWrite = true;
    point.write.verifyByRead = true;
    point.write.verifyDelayMs = 0;
    return point;
}

DeviceConfig config(const std::string& shm) {
    DeviceConfig cfg;
    cfg.machineCode = "GW";
    cfg.meterCode = "M1";
    cfg.protocol.type = "modbus_rtu";
    cfg.protocol.slave = 1;
    cfg.memoryStore.sharedMemoryName = shm;
    cfg.memoryStore.sqlitePath.clear();
    cfg.memoryStore.maxLatestPoints = 32;
    cfg.memoryStore.maxPendingWrites = 8;
    cfg.memoryStore.maxPersistentSamples = 8;
    auto power = writablePoint(1, "power_setpoint", 100, "float32");
    auto bit = writablePoint(2, "run_bit", 200, "bit");
    bit.read.bit = 3;
    bit.read.byteOrder = "BA";
    bit.write.byteOrder = "AB";
    auto scaled = writablePoint(3, "scaled_float", 300, "float32");
    scaled.read.scale = 0.1;
    scaled.write.scale = 0.1;
    auto mismatch = writablePoint(4, "limit", 400, "uint16");
    cfg.points = {power, bit, scaled, mismatch};
    return cfg;
}

CommandResult write(CommandExecutor& executor, std::uint32_t index, double value) {
    return executor.executeByIndex("CMD_" + std::to_string(index), index, value, 1000);
}

}  // namespace

int main() {
    const std::string shm = "modbus_command_write_test_" + std::to_string(getpid());
    MemoryPointStore::cleanupOrphanedSegment(shm);
    try {
        const auto cfg = config(shm);
        MemoryPointStore store(cfg.memoryStore);
        auto client = std::make_shared<RegisterClient>();
        CommandExecutor executor(cfg, store, client);

        // float32 cannot hold 1000.1 or 230.7 exactly; the device holds what was written.
        for (const double value : {1000.1, 230.7, -0.3}) {
            const auto result = write(executor, 1, value);
            require(result.success && result.verifyPassed, "float32 verify must compare against the written value: " +
                    result.message);
        }
        const auto scaled = write(executor, 3, 230.7);
        require(scaled.success && scaled.verifyPassed, "scaled float32 verify failed: " + scaled.message);

        client->registers[200] = 0x00F0;
        const auto bitResult = write(executor, 2, 1.0);
        require(bitResult.success, "BA register bit write failed: " + bitResult.message);
        require(client->registers[200] == 0x08F0, "BA register bit write must set bit 3 of the swapped byte");

        client->registers[400] = 7;
        client->writeFault = RegisterClient::Fault::IgnoreWrite;  // acknowledged, but the device keeps 7
        const auto mismatch = write(executor, 4, 9.0);
        require(!mismatch.success && !mismatch.outcomeUnknown && mismatch.message.find("verify failed") == 0,
                "a readback that differs from the written value is a definite verify failure");

        client->writeFault = RegisterClient::Fault::TimeoutAfterSend;
        auto unknown = write(executor, 4, 11.0);
        require(!unknown.success && unknown.outcomeUnknown,
                "a write that timed out after sending must be reported as outcome unknown");

        client->writeFault = RegisterClient::Fault::ExceptionReply;
        auto rejected = write(executor, 4, 12.0);
        require(!rejected.success && !rejected.outcomeUnknown,
                "an exception reply is a definite failure, not an unknown outcome");

        client->writeFault = RegisterClient::Fault::RefusedBeforeSend;
        auto refused = write(executor, 4, 13.0);
        require(!refused.success && !refused.outcomeUnknown,
                "a request that never reached the device is a definite failure");

        client->writeFault = RegisterClient::Fault::VerifyReadTimeout;
        auto unverified = write(executor, 4, 14.0);
        require(!unverified.success && unverified.outcomeUnknown,
                "an acknowledged write whose verification read failed must be outcome unknown");
        client->failReads = false;
        client->writeFault = RegisterClient::Fault::None;

        // Read-modify-write: a failing pre-read is a definite failure (nothing was written).
        client->failReads = true;
        auto preRead = write(executor, 2, 0.0);
        require(!preRead.success && !preRead.outcomeUnknown, "a failed bit pre-read must not be reported as unknown");
        client->failReads = false;
    } catch (const std::exception& ex) {
        MemoryPointStore::cleanupOrphanedSegment(shm);
        std::cerr << "modbus_command_write_test failed: " << ex.what() << std::endl;
        return 1;
    }
    MemoryPointStore::cleanupOrphanedSegment(shm);
    std::cout << "modbus_command_write_test passed" << std::endl;
    return 0;
}
