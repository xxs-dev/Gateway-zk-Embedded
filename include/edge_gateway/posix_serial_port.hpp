#pragma once

#ifndef _WIN32

#include <termios.h>
#include <string>

#include "edge_gateway/serial_port.hpp"

namespace edge_gateway {

class PosixSerialPort : public ISerialPort {
public:
    explicit PosixSerialPort(SerialPortOptions options);
    ~PosixSerialPort() override;

    PosixSerialPort(const PosixSerialPort&) = delete;
    PosixSerialPort& operator=(const PosixSerialPort&) = delete;

    void open() override;
    void close() override;
    bool isOpen() const override;
    void write(const std::vector<std::uint8_t>& bytes) override;
    std::vector<std::uint8_t> read(std::size_t maxBytes, int timeoutMs) override;

private:
    static speed_t baudToTermios(int baudRate);

    SerialPortOptions options_;
    int fd_ = -1;
};

}  // namespace edge_gateway

#endif
