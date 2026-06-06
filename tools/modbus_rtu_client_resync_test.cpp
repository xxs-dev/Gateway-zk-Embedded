#include <cstdint>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "edge_gateway/modbus_rtu_client.hpp"
#include "edge_gateway/serial_port.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::uint16_t crc16(const std::vector<std::uint8_t>& bytes) {
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

void appendCrc(std::vector<std::uint8_t>& frame) {
    const auto crc = crc16(frame);
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xFF));
}

std::vector<std::uint8_t> readResponse(
    std::uint8_t slave,
    std::uint8_t function,
    const std::vector<std::uint16_t>& registers
) {
    std::vector<std::uint8_t> frame;
    frame.reserve(5 + registers.size() * 2);
    frame.push_back(slave);
    frame.push_back(function);
    frame.push_back(static_cast<std::uint8_t>(registers.size() * 2));
    for (const auto reg : registers) {
        frame.push_back(static_cast<std::uint8_t>((reg >> 8) & 0xFF));
        frame.push_back(static_cast<std::uint8_t>(reg & 0xFF));
    }
    appendCrc(frame);
    return frame;
}

class StaleThenCurrentSerialPort : public edge_gateway::ISerialPort {
public:
    void open() override {
        open_ = true;
    }

    void close() override {
        open_ = false;
    }

    bool isOpen() const override {
        return open_;
    }

    void write(const std::vector<std::uint8_t>& bytes) override {
        require(open_, "write on closed serial port");
        writes_.push_back(bytes);
        chunks_.push_back(readResponse(217, 0x03, {0x0064}));
        chunks_.push_back(readResponse(217, 0x03, {0x0001, 0x0002, 0x0003}));
    }

    std::vector<std::uint8_t> read(std::size_t maxBytes, int timeoutMs) override {
        require(open_, "read on closed serial port");
        (void)timeoutMs;
        if (chunks_.empty()) {
            return {};
        }
        auto chunk = chunks_.front();
        chunks_.erase(chunks_.begin());
        if (chunk.size() <= maxBytes) {
            return chunk;
        }
        std::vector<std::uint8_t> head(chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(maxBytes));
        chunks_.insert(chunks_.begin(), std::vector<std::uint8_t>(
            chunk.begin() + static_cast<std::ptrdiff_t>(maxBytes),
            chunk.end()
        ));
        return head;
    }

    const std::vector<std::vector<std::uint8_t>>& writes() const {
        return writes_;
    }

private:
    bool open_ = false;
    std::vector<std::vector<std::uint8_t>> chunks_;
    std::vector<std::vector<std::uint8_t>> writes_;
};

class ShortResponseSerialPort : public edge_gateway::ISerialPort {
public:
    void open() override {
        open_ = true;
    }

    void close() override {
        open_ = false;
    }

    bool isOpen() const override {
        return open_;
    }

    void write(const std::vector<std::uint8_t>& bytes) override {
        require(open_, "write on closed serial port");
        writes_.push_back(bytes);
    }

    std::vector<std::uint8_t> read(std::size_t maxBytes, int timeoutMs) override {
        require(open_, "read on closed serial port");
        if (writes_.empty()) {
            return {};
        }
        if (!sentShort_) {
            sentShort_ = true;
            return {12, 0x03, 0x02};
        }
        if (timeoutMs == 10 && !sentLateBytes_) {
            sentLateBytes_ = true;
            cleanupDrainBytes_ = 4;
            return std::vector<std::uint8_t>(std::min<std::size_t>(maxBytes, 4), 0xFF);
        }
        return {};
    }

    std::size_t cleanupDrainBytes() const {
        return cleanupDrainBytes_;
    }

private:
    bool open_ = false;
    bool sentShort_ = false;
    bool sentLateBytes_ = false;
    std::size_t cleanupDrainBytes_ = 0;
    std::vector<std::vector<std::uint8_t>> writes_;
};

class ExceptionResponseSerialPort : public edge_gateway::ISerialPort {
public:
    void open() override {
        open_ = true;
    }

    void close() override {
        open_ = false;
    }

    bool isOpen() const override {
        return open_;
    }

    void write(const std::vector<std::uint8_t>& bytes) override {
        require(open_, "write on closed serial port");
        writes_.push_back(bytes);
        auto frame = std::vector<std::uint8_t>{12, 0x83, 0x02};
        appendCrc(frame);
        chunks_.push_back(frame);
    }

    std::vector<std::uint8_t> read(std::size_t maxBytes, int timeoutMs) override {
        require(open_, "read on closed serial port");
        (void)timeoutMs;
        if (chunks_.empty()) {
            return {};
        }
        auto chunk = chunks_.front();
        chunks_.erase(chunks_.begin());
        if (chunk.size() <= maxBytes) {
            return chunk;
        }
        std::vector<std::uint8_t> head(chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(maxBytes));
        chunks_.insert(chunks_.begin(), std::vector<std::uint8_t>(
            chunk.begin() + static_cast<std::ptrdiff_t>(maxBytes),
            chunk.end()
        ));
        return head;
    }

    const std::vector<std::vector<std::uint8_t>>& writes() const {
        return writes_;
    }

private:
    bool open_ = false;
    std::vector<std::vector<std::uint8_t>> chunks_;
    std::vector<std::vector<std::uint8_t>> writes_;
};

class EmptyThenValidSerialPort : public edge_gateway::ISerialPort {
public:
    void open() override {
        open_ = true;
    }

    void close() override {
        open_ = false;
    }

    bool isOpen() const override {
        return open_;
    }

    void write(const std::vector<std::uint8_t>& bytes) override {
        require(open_, "write on closed serial port");
        writes_.push_back(bytes);
        writeTimes_.push_back(std::chrono::steady_clock::now());
    }

    std::vector<std::uint8_t> read(std::size_t maxBytes, int timeoutMs) override {
        require(open_, "read on closed serial port");
        (void)timeoutMs;
        if (writes_.size() < 2 || sentValid_) {
            return {};
        }
        sentValid_ = true;
        auto response = readResponse(12, 0x03, {0x000A});
        if (response.size() <= maxBytes) {
            return response;
        }
        return std::vector<std::uint8_t>(
            response.begin(),
            response.begin() + static_cast<std::ptrdiff_t>(maxBytes)
        );
    }

    const std::vector<std::vector<std::uint8_t>>& writes() const {
        return writes_;
    }

    const std::vector<std::chrono::steady_clock::time_point>& writeTimes() const {
        return writeTimes_;
    }

private:
    bool open_ = false;
    bool sentValid_ = false;
    std::vector<std::vector<std::uint8_t>> writes_;
    std::vector<std::chrono::steady_clock::time_point> writeTimes_;
};

class NoResponseSerialPort : public edge_gateway::ISerialPort {
public:
    void open() override {
        open_ = true;
    }

    void close() override {
        open_ = false;
    }

    bool isOpen() const override {
        return open_;
    }

    void write(const std::vector<std::uint8_t>& bytes) override {
        require(open_, "write on closed serial port");
        writes_.push_back(bytes);
    }

    std::vector<std::uint8_t> read(std::size_t maxBytes, int timeoutMs) override {
        require(open_, "read on closed serial port");
        (void)maxBytes;
        (void)timeoutMs;
        return {};
    }

    const std::vector<std::vector<std::uint8_t>>& writes() const {
        return writes_;
    }

private:
    bool open_ = false;
    std::vector<std::vector<std::uint8_t>> writes_;
};

class ImmediateRegisterSerialPort : public edge_gateway::ISerialPort {
public:
    void open() override {
        open_ = true;
    }

    void close() override {
        open_ = false;
    }

    bool isOpen() const override {
        return open_;
    }

    void write(const std::vector<std::uint8_t>& bytes) override {
        require(open_, "write on closed serial port");
        require(bytes.size() >= 2, "request frame should include slave and function");
        writes_.push_back(bytes);
        writeTimes_.push_back(std::chrono::steady_clock::now());
        chunks_.push_back(readResponse(bytes[0], bytes[1], {static_cast<std::uint16_t>(writes_.size())}));
    }

    std::vector<std::uint8_t> read(std::size_t maxBytes, int timeoutMs) override {
        require(open_, "read on closed serial port");
        (void)timeoutMs;
        if (chunks_.empty()) {
            return {};
        }
        auto chunk = chunks_.front();
        chunks_.erase(chunks_.begin());
        if (chunk.size() <= maxBytes) {
            return chunk;
        }
        std::vector<std::uint8_t> head(chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(maxBytes));
        chunks_.insert(chunks_.begin(), std::vector<std::uint8_t>(
            chunk.begin() + static_cast<std::ptrdiff_t>(maxBytes),
            chunk.end()
        ));
        return head;
    }

    const std::vector<std::vector<std::uint8_t>>& writes() const {
        return writes_;
    }

    const std::vector<std::chrono::steady_clock::time_point>& writeTimes() const {
        return writeTimes_;
    }

private:
    bool open_ = false;
    std::vector<std::vector<std::uint8_t>> chunks_;
    std::vector<std::vector<std::uint8_t>> writes_;
    std::vector<std::chrono::steady_clock::time_point> writeTimes_;
};

void verifyWrongLengthReadResponseIsSkipped() {
    edge_gateway::SerialPortOptions options;
    options.device = "test";
    options.timeoutMs = 100;

    auto serial = std::make_shared<StaleThenCurrentSerialPort>();
    edge_gateway::ModbusRtuClient client(serial, options);

    const auto values = client.readHoldingRegisters(217, 847, 3);
    require(values.size() == 3, "expected current three-register response");
    require(values[0] == 1 && values[1] == 2 && values[2] == 3, "current response values mismatch");
    require(serial->writes().size() == 1, "expected one request write");
}

void verifyShortResponseDrainsLateBytes() {
    edge_gateway::SerialPortOptions options;
    options.device = "test";
    options.timeoutMs = 50;

    auto serial = std::make_shared<ShortResponseSerialPort>();
    edge_gateway::ModbusRtuClient client(serial, options);

    bool threw = false;
    try {
        (void)client.readHoldingRegisters(12, 831, 1);
    } catch (const std::runtime_error& ex) {
        threw = std::string(ex.what()).find("response too short") != std::string::npos;
    }
    require(threw, "short response should fail");
    require(serial->cleanupDrainBytes() > 0, "short response failure should drain late serial bytes");
}

void verifyExceptionResponseIsNotRetriedAsShortResponse() {
    edge_gateway::SerialPortOptions options;
    options.device = "test";
    options.timeoutMs = 50;

    auto serial = std::make_shared<ExceptionResponseSerialPort>();
    edge_gateway::ModbusRtuClient client(serial, options);

    bool threw = false;
    try {
        (void)client.readHoldingRegisters(12, 831, 1);
    } catch (const std::runtime_error& ex) {
        const auto message = std::string(ex.what());
        threw = message.find("modbus exception code 2") != std::string::npos;
        require(message.find("response too short") == std::string::npos, "exception frame should not look like short response");
    }
    require(threw, "exception response should report the modbus exception code");
    require(serial->writes().size() == 1, "exception response should not trigger a read retry");
}

void verifyNoResponseReadIsRetriedAndPaced() {
    edge_gateway::SerialPortOptions options;
    options.device = "test";
    options.timeoutMs = 5;
    options.frameIntervalMs = 30;
    options.readRetryCount = 1;

    auto serial = std::make_shared<EmptyThenValidSerialPort>();
    edge_gateway::ModbusRtuClient client(serial, options);

    const auto values = client.readHoldingRegisters(12, 831, 1);
    require(values.size() == 1 && values[0] == 10, "retry should return the second response value");
    require(serial->writes().size() == 2, "empty first response should trigger one retry");
    const auto intervalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        serial->writeTimes()[1] - serial->writeTimes()[0]
    ).count();
    require(intervalMs >= 25, "retry write should honor the configured frame interval");
}

void verifyFrameIntervalIsScopedToSlaveAddress() {
    edge_gateway::SerialPortOptions options;
    options.device = "test";
    options.timeoutMs = 50;
    options.frameIntervalMs = 200;
    options.readRetryCount = 0;

    auto serial = std::make_shared<ImmediateRegisterSerialPort>();
    edge_gateway::ModbusRtuClient client(serial, options);

    (void)client.readHoldingRegisters(1, 0, 1);
    (void)client.readHoldingRegisters(2, 0, 1);
    (void)client.readHoldingRegisters(1, 1, 1);

    require(serial->writes().size() == 3, "expected one write per read");
    require(serial->writes()[0][0] == 1 && serial->writes()[1][0] == 2 && serial->writes()[2][0] == 1,
        "test should write slave sequence 1,2,1");
    const auto firstToSecondMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        serial->writeTimes()[1] - serial->writeTimes()[0]
    ).count();
    const auto firstToThirdMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        serial->writeTimes()[2] - serial->writeTimes()[0]
    ).count();
    require(firstToSecondMs < 140, "different slave addresses should not be delayed by frameIntervalMs");
    require(firstToThirdMs >= 180, "same slave address should honor frameIntervalMs");
}

void verifyNoResponseFailureIsReportedAsTimeout() {
    edge_gateway::SerialPortOptions options;
    options.device = "test";
    options.timeoutMs = 5;
    options.readRetryCount = 1;

    auto serial = std::make_shared<NoResponseSerialPort>();
    edge_gateway::ModbusRtuClient client(serial, options);

    bool threw = false;
    try {
        (void)client.readHoldingRegisters(12, 831, 1);
    } catch (const std::runtime_error& ex) {
        const auto message = std::string(ex.what());
        threw = message.find("response timeout") != std::string::npos;
        require(message.find("response too short") == std::string::npos, "empty response should not look like a short frame");
    }
    require(threw, "empty responses should report response timeout");
    require(serial->writes().size() == 2, "empty read should retry before timing out");
}

void verifyMaxRequestRegistersIsEnforced() {
    edge_gateway::SerialPortOptions options;
    options.device = "test";
    options.timeoutMs = 100;
    options.maxRequestRegisters = 2;

    auto serial = std::make_shared<StaleThenCurrentSerialPort>();
    edge_gateway::ModbusRtuClient client(serial, options);

    bool threw = false;
    try {
        (void)client.readHoldingRegisters(217, 847, 3);
    } catch (const std::invalid_argument& ex) {
        threw = std::string(ex.what()).find("maxRequestRegisters") != std::string::npos;
    }
    require(threw, "read count above maxRequestRegisters should throw");
    require(serial->writes().empty(), "oversized request should not write to serial port");
}

}  // namespace

int main() {
    try {
        verifyWrongLengthReadResponseIsSkipped();
        verifyShortResponseDrainsLateBytes();
        verifyExceptionResponseIsNotRetriedAsShortResponse();
        verifyNoResponseReadIsRetriedAndPaced();
        verifyFrameIntervalIsScopedToSlaveAddress();
        verifyNoResponseFailureIsReportedAsTimeout();
        verifyMaxRequestRegistersIsEnforced();
        std::cout << "modbus_rtu_client_resync_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "modbus_rtu_client_resync_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
