#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {

class ModbusTcpClient : public IModbusClient {
public:
    explicit ModbusTcpClient(TcpTransportConfig config);
    ~ModbusTcpClient() override;

    std::vector<std::uint16_t> readHoldingRegisters(int slave, int start, int count) override;
    std::vector<std::uint16_t> readInputRegisters(int slave, int start, int count) override;

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

    void ensureConnected();
    void disconnect();
    void configureSocketTimeouts() const;
    std::vector<std::uint8_t> readExact(std::size_t size);
    void sendAll(const std::vector<std::uint8_t>& bytes);

    TcpTransportConfig config_;
    std::uint16_t transactionId_ = 0;
    std::intptr_t socket_ = -1;
};

}  // namespace edge_gateway
