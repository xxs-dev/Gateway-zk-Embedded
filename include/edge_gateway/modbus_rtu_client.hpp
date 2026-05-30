#pragma once

#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/serial_port.hpp"

namespace edge_gateway {

class ModbusRtuClient : public IModbusClient {
public:
    ModbusRtuClient(std::shared_ptr<ISerialPort> serialPort, SerialPortOptions options);

    void beginPriorityWrite() override;
    void endPriorityWrite() override;
    void enterTransaction();
    void leaveTransaction();

    std::vector<std::uint16_t> readCoils(int slave, int start, int count) override;
    std::vector<std::uint16_t> readDiscreteInputs(int slave, int start, int count) override;
    std::vector<std::uint16_t> readHoldingRegisters(int slave, int start, int count) override;
    std::vector<std::uint16_t> readInputRegisters(int slave, int start, int count) override;

    void writeSingleCoil(int slave, int address, bool value) override;
    void writeSingleRegister(int slave, int address, std::uint16_t value) override;
    void writeMultipleRegisters(
        int slave,
        int address,
        const std::vector<std::uint16_t>& values
    ) override;

private:
    std::vector<std::uint8_t> transact(
        int slave,
        std::uint8_t function,
        const std::vector<std::uint8_t>& pdu,
        std::size_t minResponseSize
    );

    std::vector<std::uint16_t> executeRegisterRead(
        int slave,
        std::uint8_t function,
        int start,
        int count
    );
    std::vector<std::uint16_t> executeBitRead(
        int slave,
        std::uint8_t function,
        int start,
        int count
    );

    void ensurePortOpen();
    void waitForFrameInterval();
    static std::uint16_t crc16(const std::vector<std::uint8_t>& bytes);
    static void appendCrc(std::vector<std::uint8_t>& frame);
    static void validateCrc(const std::vector<std::uint8_t>& frame);
    static std::vector<std::uint16_t> decodeRegistersFromReadResponse(
        const std::vector<std::uint8_t>& response,
        std::uint8_t function,
        int expectedCount
    );
    static std::vector<std::uint16_t> decodeBitsFromReadResponse(
        const std::vector<std::uint8_t>& response,
        std::uint8_t function,
        int expectedCount
    );
    static void validateWriteEcho(
        const std::vector<std::uint8_t>& response,
        std::uint8_t function,
        int address,
        int countOrValue
    );

    std::shared_ptr<ISerialPort> serialPort_;
    SerialPortOptions options_;
    std::mutex transactionMutex_;
    std::condition_variable transactionCv_;
    int activeTransactions_ = 0;
    int pendingPriorityWrites_ = 0;
    std::chrono::steady_clock::time_point lastRequestWriteAt_{};
    static thread_local int priorityContextDepth_;
};

}  // namespace edge_gateway
