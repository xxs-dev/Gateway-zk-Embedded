#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "edge_gateway/serial_port.hpp"

namespace edge_gateway {

class MockSerialPort : public ISerialPort {
public:
    void open() override;
    void close() override;
    bool isOpen() const override;
    void write(const std::vector<std::uint8_t>& bytes) override;
    std::vector<std::uint8_t> read(std::size_t maxBytes, int timeoutMs) override;

    void setRegister(int address, std::uint16_t value);
    std::uint16_t getRegister(int address) const;

private:
    static std::uint16_t decodeWord(const std::vector<std::uint8_t>& frame, std::size_t offset);
    static std::uint16_t crc16(const std::vector<std::uint8_t>& bytes);
    static void appendCrc(std::vector<std::uint8_t>& frame);

    void handleRead(std::uint8_t slave, std::uint8_t function, const std::vector<std::uint8_t>& bytes);
    void handleWriteSingle(
        std::uint8_t slave,
        std::uint8_t function,
        const std::vector<std::uint8_t>& bytes
    );
    void handleWriteMultiple(
        std::uint8_t slave,
        std::uint8_t function,
        const std::vector<std::uint8_t>& bytes
    );
    void handleDlt645Read(const std::vector<std::uint8_t>& bytes);

    bool open_ = false;
    std::unordered_map<int, std::uint16_t> registers_;
    std::vector<std::uint8_t> pendingResponse_;
};

}  // namespace edge_gateway
