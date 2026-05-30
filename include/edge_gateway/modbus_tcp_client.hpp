#pragma once

#include <cstdint>
#include <cstddef>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {

class ModbusTcpClient : public IModbusClient {
public:
    explicit ModbusTcpClient(TcpTransportConfig config, int maxRequestRegisters = 125);
    ~ModbusTcpClient() override;

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
        const std::vector<std::uint8_t>& pdu
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

    void ensureConnected();
    void disconnect();
    void configureSocketTimeouts() const;
    std::vector<std::uint8_t> readExact(std::size_t size);
    void sendAll(const std::vector<std::uint8_t>& bytes);

    TcpTransportConfig config_;
    int maxRequestRegisters_ = 125;
    std::uint16_t transactionId_ = 0;
    std::intptr_t socket_ = -1;
    std::mutex transactionMutex_;
    std::condition_variable transactionCv_;
    int activeTransactions_ = 0;
    int pendingPriorityWrites_ = 0;
    static thread_local int priorityContextDepth_;
};

}  // namespace edge_gateway
