#include "edge_gateway/mock_serial_port.hpp"

#include <algorithm>
#include <string>
#include <stdexcept>
#include <utility>

namespace edge_gateway {

namespace {

std::vector<std::uint8_t> encodeDlt645Bcd(std::uint64_t value, std::size_t byteCount) {
    std::vector<std::uint8_t> bytes(byteCount, 0);
    for (std::size_t i = 0; i < byteCount; ++i) {
        const auto low = static_cast<std::uint8_t>(value % 10);
        value /= 10;
        const auto high = static_cast<std::uint8_t>(value % 10);
        value /= 10;
        bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return bytes;
}

std::vector<std::uint8_t> mockDlt645Payload(const std::vector<std::uint8_t>& dataIdLe) {
    const std::string id(
        [] (const std::vector<std::uint8_t>& bytes) {
            static const char* hex = "0123456789ABCDEF";
            std::string out;
            out.reserve(bytes.size() * 2);
            for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {
                out.push_back(hex[(*it >> 4) & 0x0F]);
                out.push_back(hex[*it & 0x0F]);
            }
            return out;
        }(dataIdLe)
    );

    if (id == "00010000") return encodeDlt645Bcd(1234567, 4);
    if (id == "00020000") return encodeDlt645Bcd(2234567, 4);
    if (id == "00030000") return encodeDlt645Bcd(345678, 4);
    if (id == "00040000") return encodeDlt645Bcd(445678, 4);
    if (id == "02010100") return encodeDlt645Bcd(2205, 2);
    if (id == "02010200") return encodeDlt645Bcd(2213, 2);
    if (id == "02010300") return encodeDlt645Bcd(2198, 2);
    if (id == "02020100") return encodeDlt645Bcd(12345, 3);
    if (id == "02020200") return encodeDlt645Bcd(11345, 3);
    if (id == "02020300") return encodeDlt645Bcd(10345, 3);
    if (id == "02030000") return encodeDlt645Bcd(15234, 3);
    if (id == "02040000") return encodeDlt645Bcd(6234, 3);
    if (id == "02070000") return encodeDlt645Bcd(998, 2);
    if (id == "02800002") return encodeDlt645Bcd(5000, 2);
    if (id == "01010001") return encodeDlt645Bcd(2404201030, 5);
    if (id == "01020001") return encodeDlt645Bcd(2404201031, 5);
    if (id == "01030001") return encodeDlt645Bcd(2404201032, 5);
    if (id == "04000101") return encodeDlt645Bcd(260420, 3);
    if (id == "04000102") return encodeDlt645Bcd(260420103000, 6);
    if (id == "04000503") return encodeDlt645Bcd(1, 4);
    if (id.size() == 8 && id.substr(0, 4) == "0001") return encodeDlt645Bcd(100000 + std::stoul(id.substr(4, 4), nullptr, 16), 4);
    if (id.size() == 8 && id.substr(0, 4) == "0002") return encodeDlt645Bcd(200000 + std::stoul(id.substr(4, 4), nullptr, 16), 4);
    if (id.size() == 8 && id.substr(0, 4) == "0003") return encodeDlt645Bcd(300000 + std::stoul(id.substr(4, 4), nullptr, 16), 4);
    if (id.size() == 8 && id.substr(0, 4) == "0004") return encodeDlt645Bcd(400000 + std::stoul(id.substr(4, 4), nullptr, 16), 4);
    return encodeDlt645Bcd(1234, 4);
}

}  // namespace

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
    if (bytes.size() >= 12 && bytes.front() == 0x68 && bytes[7] == 0x68) {
        handleDlt645Read(bytes);
        return;
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

void MockSerialPort::handleDlt645Read(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 16) {
        throw std::runtime_error("DLT645 request too short");
    }
    std::vector<std::uint8_t> dataId(bytes.begin() + 10, bytes.begin() + 14);
    for (auto& byte : dataId) {
        byte = static_cast<std::uint8_t>(byte - 0x33);
    }
    const auto payload = mockDlt645Payload(dataId);

    std::vector<std::uint8_t> response(bytes.begin(), bytes.begin() + 10);
    response[8] = 0x91;
    response[9] = static_cast<std::uint8_t>(4 + payload.size());
    for (std::size_t i = 10; i < 14; ++i) {
        response.push_back(bytes[i]);
    }
    for (const auto byte : payload) {
        response.push_back(static_cast<std::uint8_t>(byte + 0x33));
    }

    std::uint32_t sum = 0;
    for (const auto byte : response) {
        sum += byte;
    }
    response.push_back(static_cast<std::uint8_t>(sum & 0xFF));
    response.push_back(0x16);
    pendingResponse_ = std::move(response);
}

}  // namespace edge_gateway
