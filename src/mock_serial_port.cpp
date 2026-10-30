#include "edge_gateway/mock_serial_port.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace edge_gateway {

void MockSerialPort::open() {
    open_ = true;
}

void MockSerialPort::close() {
    open_ = false;
    pendingResponse_.clear();
}

bool MockSerialPort::isOpen() const {
    return open_;
}

void MockSerialPort::write(const std::vector<std::uint8_t>& bytes) {
    if (!open_) {
        throw std::runtime_error("serial port is not open");
    }
    if (bytes.size() < 8) {
        throw std::runtime_error("rtu request too short");
    }

    const auto slave = bytes[0];
    const auto function = bytes[1];
    pendingResponse_.clear();
    switch (function) {
        case 0x03:
        case 0x04:
            handleRead(slave, function, bytes);
            break;
        case 0x06:
            handleWriteSingle(slave, function, bytes);
            break;
        case 0x10:
            handleWriteMultiple(slave, function, bytes);
            break;
        default:
            throw std::runtime_error("unsupported simulated modbus function");
    }
}

std::vector<std::uint8_t> MockSerialPort::read(std::size_t maxBytes, int timeoutMs) {
    (void)timeoutMs;
    if (!open_) {
        throw std::runtime_error("serial port is not open");
    }

    const auto count = std::min(maxBytes, pendingResponse_.size());
    std::vector<std::uint8_t> result(pendingResponse_.begin(), pendingResponse_.begin() + count);
    pendingResponse_.erase(pendingResponse_.begin(), pendingResponse_.begin() + count);
    return result;
}

void MockSerialPort::setRegister(int address, std::uint16_t value) {
    registers_[address] = value;
}

std::uint16_t MockSerialPort::getRegister(int address) const {
    const auto it = registers_.find(address);
    if (it == registers_.end()) {
        return 0;
    }
    return it->second;
}

std::uint16_t MockSerialPort::decodeWord(const std::vector<std::uint8_t>& frame, std::size_t offset) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(frame[offset] << 8) | frame[offset + 1]
    );
}

std::uint16_t MockSerialPort::crc16(const std::vector<std::uint8_t>& bytes) {
    std::uint16_t crc = 0xFFFF;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i) {
            const bool lsb = (crc & 0x0001U) != 0;
            crc >>= 1;
            if (lsb) {
                crc ^= 0xA001U;
            }
        }
    }
    return crc;
}

void MockSerialPort::appendCrc(std::vector<std::uint8_t>& frame) {
    const auto crc = crc16(frame);
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xFF));
}

void MockSerialPort::handleRead(
    std::uint8_t slave,
    std::uint8_t function,
    const std::vector<std::uint8_t>& bytes
) {
    const auto start = decodeWord(bytes, 2);
    const auto count = decodeWord(bytes, 4);

    std::vector<std::uint8_t> response;
    response.push_back(slave);
    response.push_back(function);
    response.push_back(static_cast<std::uint8_t>(count * 2));
    for (std::uint16_t i = 0; i < count; ++i) {
        const auto value = getRegister(static_cast<int>(start + i));
        response.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        response.push_back(static_cast<std::uint8_t>(value & 0xFF));
    }
    appendCrc(response);
    pendingResponse_ = std::move(response);
}

void MockSerialPort::handleWriteSingle(
    std::uint8_t slave,
    std::uint8_t function,
    const std::vector<std::uint8_t>& bytes
) {
    const auto address = decodeWord(bytes, 2);
    const auto value = decodeWord(bytes, 4);
    registers_[static_cast<int>(address)] = value;

    pendingResponse_ = {
        slave,
        function,
        bytes[2],
        bytes[3],
        bytes[4],
        bytes[5]
    };
    appendCrc(pendingResponse_);
}

void MockSerialPort::handleWriteMultiple(
    std::uint8_t slave,
    std::uint8_t function,
    const std::vector<std::uint8_t>& bytes
) {
    const auto address = decodeWord(bytes, 2);
    const auto count = decodeWord(bytes, 4);
    const auto byteCount = bytes[6];
    if (byteCount != count * 2) {
        throw std::runtime_error("invalid simulated write multiple byte count");
    }

    for (std::uint16_t i = 0; i < count; ++i) {
        const auto value = decodeWord(bytes, 7 + i * 2);
        registers_[static_cast<int>(address + i)] = value;
    }

    pendingResponse_ = {
        slave,
        function,
        bytes[2],
        bytes[3],
        bytes[4],
        bytes[5]
    };
    appendCrc(pendingResponse_);
}

}  // namespace edge_gateway
