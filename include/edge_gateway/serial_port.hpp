#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace edge_gateway {

class ISerialPort {
public:
    virtual ~ISerialPort() = default;

    virtual void open() = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual void write(const std::vector<std::uint8_t>& bytes) = 0;
    virtual std::vector<std::uint8_t> read(std::size_t maxBytes, int timeoutMs) = 0;
};

struct SerialPortOptions {
    std::string device;
    int baudRate = 9600;
    int dataBits = 8;
    int stopBits = 1;
    std::string parity = "N";
    int timeoutMs = 1000;
    int maxRequestRegisters = 125;
    int frameIntervalMs = 0;
    int readRetryCount = 1;
};

}  // namespace edge_gateway
