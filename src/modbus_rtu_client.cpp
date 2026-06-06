#include "edge_gateway/modbus_rtu_client.hpp"

#include <cstdlib>
#include <chrono>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace edge_gateway {

thread_local int ModbusRtuClient::priorityContextDepth_ = 0;

namespace {

std::vector<std::uint8_t> makeWord(int value) {
    if (value < 0 || value > 0xFFFF) {
        throw std::invalid_argument("modbus word out of range");
    }
    return {
        static_cast<std::uint8_t>((value >> 8) & 0xFF),
        static_cast<std::uint8_t>(value & 0xFF)
    };
}

std::uint16_t frameCrc16(const std::vector<std::uint8_t>& bytes) {
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

bool hasValidCrc(const std::vector<std::uint8_t>& frame) {
    if (frame.size() < 4) {
        return false;
    }
    std::vector<std::uint8_t> payload(frame.begin(), frame.end() - 2);
    const auto expected = frameCrc16(payload);
    const auto actual = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(frame[frame.size() - 1] << 8) |
        frame[frame.size() - 2]
    );
    return expected == actual;
}

std::size_t expectedResponseSize(
    std::uint8_t function,
    const std::vector<std::uint8_t>& response,
    std::size_t fallbackMinSize
) {
    if (response.size() >= 2 && (response[1] & 0x80U) != 0) {
        return 5;
    }
    if ((function == 0x01 || function == 0x02 || function == 0x03 || function == 0x04) && response.size() >= 3) {
        return static_cast<std::size_t>(response[2]) + 5;
    }
    if (function == 0x05 || function == 0x06 || function == 0x10) {
        return 8;
    }
    return fallbackMinSize;
}

bool isReadFunction(std::uint8_t function) {
    return function == 0x01 || function == 0x02 || function == 0x03 || function == 0x04;
}

bool isExceptionResponse(const std::vector<std::uint8_t>& response, std::uint8_t function) {
    return response.size() == 5 &&
        response[1] == static_cast<std::uint8_t>(function | 0x80U);
}

bool isCompleteResponse(
    const std::vector<std::uint8_t>& response,
    std::uint8_t function,
    std::size_t minResponseSize
) {
    return response.size() >= minResponseSize || isExceptionResponse(response, function);
}

std::string hexBytes(const std::vector<std::uint8_t>& bytes) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        out.push_back(kHex[(byte >> 4) & 0x0F]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

bool traceFailuresEnabled() {
    const char* value = std::getenv("MODBUS_TRACE_FAILURES");
    return value != nullptr && std::string(value) == "1";
}

int wordAt(const std::vector<std::uint8_t>& pdu, std::size_t offset) {
    if (pdu.size() < offset + 2) {
        return -1;
    }
    return (static_cast<int>(pdu[offset]) << 8) | static_cast<int>(pdu[offset + 1]);
}

std::size_t drainSerialInput(ISerialPort& serialPort, int perReadTimeoutMs = 0) {
    std::size_t drained = 0;
    for (int i = 0; i < 16; ++i) {
        auto chunk = serialPort.read(256, perReadTimeoutMs);
        if (chunk.empty()) {
            return drained;
        }
        drained += chunk.size();
    }
    return drained;
}

std::size_t tryDrainSerialInput(ISerialPort& serialPort, int perReadTimeoutMs) {
    try {
        return drainSerialInput(serialPort, perReadTimeoutMs);
    } catch (...) {
        return 0;
    }
}

class ModbusTransactionScope {
public:
    explicit ModbusTransactionScope(ModbusRtuClient& client) : client_(client) {
        client_.enterTransaction();
    }

    ~ModbusTransactionScope() {
        client_.leaveTransaction();
    }

    ModbusTransactionScope(const ModbusTransactionScope&) = delete;
    ModbusTransactionScope& operator=(const ModbusTransactionScope&) = delete;

private:
    ModbusRtuClient& client_;
};

class PriorityWriteScope {
public:
    explicit PriorityWriteScope(ModbusRtuClient& client) : client_(client) {
        client_.beginPriorityWrite();
    }

    ~PriorityWriteScope() {
        client_.endPriorityWrite();
    }

    PriorityWriteScope(const PriorityWriteScope&) = delete;
    PriorityWriteScope& operator=(const PriorityWriteScope&) = delete;

private:
    ModbusRtuClient& client_;
};

bool tryExtractResponseFrame(
    std::vector<std::uint8_t>& buffer,
    std::uint8_t slave,
    std::uint8_t function,
    std::vector<std::uint8_t>& frame,
    std::size_t minResponseSize
) {
    while (buffer.size() >= 2) {
        const auto start = std::find(buffer.begin(), buffer.end(), slave);
        if (start == buffer.end()) {
            buffer.clear();
            return false;
        }
        if (start != buffer.begin()) {
            buffer.erase(buffer.begin(), start);
        }
        if (buffer.size() < 2) {
            return false;
        }

        const auto responseFunction = buffer[1];
        if (responseFunction != function && responseFunction != static_cast<std::uint8_t>(function | 0x80U)) {
            buffer.erase(buffer.begin());
            continue;
        }

        const auto expectedSize = expectedResponseSize(function, buffer, minResponseSize);
        if (buffer.size() < expectedSize) {
            return false;
        }
        if (isReadFunction(function) && (buffer[1] & 0x80U) == 0 && expectedSize != minResponseSize) {
            buffer.erase(buffer.begin());
            continue;
        }

        std::vector<std::uint8_t> candidate(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(expectedSize));
        if (hasValidCrc(candidate)) {
            frame = std::move(candidate);
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(expectedSize));
            return true;
        }
        buffer.erase(buffer.begin());
    }
    return false;
}

}  // namespace

ModbusRtuClient::ModbusRtuClient(
    std::shared_ptr<ISerialPort> serialPort,
    SerialPortOptions options
) : serialPort_(std::move(serialPort)),
    options_(std::move(options)) {
    if (!serialPort_) {
        throw std::invalid_argument("serialPort is required");
    }
}

void ModbusRtuClient::beginPriorityWrite() {
    ++priorityContextDepth_;
    if (priorityContextDepth_ == 1) {
        std::lock_guard<std::mutex> lock(transactionMutex_);
        ++pendingPriorityWrites_;
        transactionCv_.notify_all();
    }
}

void ModbusRtuClient::endPriorityWrite() {
    if (priorityContextDepth_ <= 0) {
        return;
    }
    --priorityContextDepth_;
    if (priorityContextDepth_ == 0) {
        {
            std::lock_guard<std::mutex> lock(transactionMutex_);
            if (pendingPriorityWrites_ > 0) {
                --pendingPriorityWrites_;
            }
        }
        transactionCv_.notify_all();
    }
}

void ModbusRtuClient::enterTransaction() {
    std::unique_lock<std::mutex> lock(transactionMutex_);
    const bool priorityContext = priorityContextDepth_ > 0;
    transactionCv_.wait(lock, [&] {
        if (activeTransactions_ != 0) {
            return false;
        }
        return priorityContext || pendingPriorityWrites_ == 0;
    });
    activeTransactions_ = 1;
}

void ModbusRtuClient::leaveTransaction() {
    {
        std::lock_guard<std::mutex> lock(transactionMutex_);
        activeTransactions_ = 0;
    }
    transactionCv_.notify_all();
}

void ModbusRtuClient::waitForFrameInterval(int slave) {
    const auto intervalMs = std::max(0, options_.frameIntervalMs);
    const auto lastWriteIt = lastRequestWriteAtBySlave_.find(slave);
    if (intervalMs <= 0 || lastWriteIt == lastRequestWriteAtBySlave_.end()) {
        return;
    }

    const auto nextWriteAt = lastWriteIt->second + std::chrono::milliseconds(intervalMs);
    const auto now = std::chrono::steady_clock::now();
    if (now < nextWriteAt) {
        std::this_thread::sleep_until(nextWriteAt);
    }
}

std::vector<std::uint16_t> ModbusRtuClient::readCoils(int slave, int start, int count) {
    return executeBitRead(slave, 0x01, start, count);
}

std::vector<std::uint16_t> ModbusRtuClient::readDiscreteInputs(int slave, int start, int count) {
    return executeBitRead(slave, 0x02, start, count);
}

std::vector<std::uint16_t> ModbusRtuClient::readHoldingRegisters(int slave, int start, int count) {
    return executeRegisterRead(slave, 0x03, start, count);
}

std::vector<std::uint16_t> ModbusRtuClient::readInputRegisters(int slave, int start, int count) {
    return executeRegisterRead(slave, 0x04, start, count);
}

void ModbusRtuClient::writeSingleCoil(int slave, int address, bool value) {
    PriorityWriteScope priority(*this);
    std::vector<std::uint8_t> pdu;
    const auto addr = makeWord(address);
    const auto coilValue = makeWord(value ? 0xFF00 : 0x0000);
    pdu.insert(pdu.end(), addr.begin(), addr.end());
    pdu.insert(pdu.end(), coilValue.begin(), coilValue.end());

    const auto response = transact(slave, 0x05, pdu, 8);
    validateWriteEcho(response, 0x05, address, value ? 0xFF00 : 0x0000);
}

void ModbusRtuClient::writeSingleRegister(int slave, int address, std::uint16_t value) {
    PriorityWriteScope priority(*this);
    std::vector<std::uint8_t> pdu;
    const auto addr = makeWord(address);
    pdu.insert(pdu.end(), addr.begin(), addr.end());
    pdu.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    pdu.push_back(static_cast<std::uint8_t>(value & 0xFF));

    const auto response = transact(slave, 0x06, pdu, 8);
    validateWriteEcho(response, 0x06, address, value);
}

void ModbusRtuClient::writeMultipleRegisters(
    int slave,
    int address,
    const std::vector<std::uint16_t>& values
) {
    PriorityWriteScope priority(*this);
    if (values.empty()) {
        throw std::invalid_argument("writeMultipleRegisters requires at least one value");
    }
    if (values.size() > static_cast<std::size_t>(options_.maxRequestRegisters)) {
        throw std::invalid_argument(
            "write register count exceeds maxRequestRegisters: " + std::to_string(values.size()) +
            " > " + std::to_string(options_.maxRequestRegisters)
        );
    }

    std::vector<std::uint8_t> pdu;
    const auto addr = makeWord(address);
    const auto quantity = makeWord(static_cast<int>(values.size()));
    pdu.insert(pdu.end(), addr.begin(), addr.end());
    pdu.insert(pdu.end(), quantity.begin(), quantity.end());
    pdu.push_back(static_cast<std::uint8_t>(values.size() * 2));
    for (const auto value : values) {
        pdu.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        pdu.push_back(static_cast<std::uint8_t>(value & 0xFF));
    }

    const auto response = transact(slave, 0x10, pdu, 8);
    validateWriteEcho(response, 0x10, address, static_cast<int>(values.size()));
}

std::vector<std::uint8_t> ModbusRtuClient::transact(
    int slave,
    std::uint8_t function,
    const std::vector<std::uint8_t>& pdu,
    std::size_t minResponseSize
) {
    ModbusTransactionScope transaction(*this);
    if (slave <= 0 || slave > 247) {
        throw std::invalid_argument("slave must be in range 1..247");
    }

    ensurePortOpen();

    std::vector<std::uint8_t> frame;
    frame.reserve(2 + pdu.size() + 2);
    frame.push_back(static_cast<std::uint8_t>(slave));
    frame.push_back(function);
    frame.insert(frame.end(), pdu.begin(), pdu.end());
    appendCrc(frame);

    std::vector<std::uint8_t> response;
    std::vector<std::uint8_t> lastRawReceived;
    std::size_t lastCleanupRxBytes = 0;
    int attemptsMade = 0;
    bool sawResponseBytes = false;
    const auto maxAttempts = isReadFunction(function)
        ? std::max(1, options_.readRetryCount + 1)
        : 1;
    for (int attempts = 1; attempts <= maxAttempts; ++attempts) {
        attemptsMade = attempts;
        waitForFrameInterval(slave);
        drainSerialInput(*serialPort_);
        const auto writeStartedAt = std::chrono::steady_clock::now();
        serialPort_->write(frame);
        lastRequestWriteAtBySlave_[slave] = writeStartedAt;
        std::vector<std::uint8_t> buffer;
        std::vector<std::uint8_t> rawReceived;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto now = std::chrono::steady_clock::now();
            const auto remainingMs = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count()
            );
            if (remainingMs <= 0) {
                break;
            }

            auto chunk = serialPort_->read(256, remainingMs);
            if (chunk.empty()) {
                continue;
            }

            rawReceived.insert(rawReceived.end(), chunk.begin(), chunk.end());
            buffer.insert(buffer.end(), chunk.begin(), chunk.end());
            if (tryExtractResponseFrame(
                    buffer,
                    static_cast<std::uint8_t>(slave),
                    function,
                    response,
                    minResponseSize
                )) {
                break;
            }
        }

        if (isCompleteResponse(response, function, minResponseSize)) {
            break;
        }

        if (!rawReceived.empty()) {
            lastRawReceived = rawReceived;
            sawResponseBytes = true;
        }
        lastCleanupRxBytes = tryDrainSerialInput(*serialPort_, 10);
        if (attempts >= maxAttempts) {
            break;
        }
    }

    if (!isCompleteResponse(response, function, minResponseSize)) {
        if (traceFailuresEnabled()) {
            std::cerr << "modbus trace failure"
                      << " port=" << options_.device
                      << " slave=" << slave
                      << " function=" << static_cast<int>(function)
                      << " start=" << wordAt(pdu, 0)
                      << " count=" << wordAt(pdu, 2)
                      << " expectedBytes=" << minResponseSize
                      << " rxBytes=" << lastRawReceived.size()
                      << " cleanupRxBytes=" << lastCleanupRxBytes
                      << " attempts=" << attemptsMade
                      << " tx=" << hexBytes(frame)
                      << " rx=" << hexBytes(lastRawReceived)
                      << std::endl;
        }
        if (!sawResponseBytes) {
            throw std::runtime_error("modbus response timeout");
        }
        throw std::runtime_error("modbus response too short");
    }

    validateCrc(response);
    if (response[0] != static_cast<std::uint8_t>(slave)) {
        throw std::runtime_error("modbus slave mismatch");
    }
    if (response[1] == static_cast<std::uint8_t>(function | 0x80)) {
        throw std::runtime_error("modbus exception code " + std::to_string(response[2]));
    }
    if (response[1] != function) {
        throw std::runtime_error("modbus function mismatch");
    }
    return response;
}

std::vector<std::uint16_t> ModbusRtuClient::executeRegisterRead(
    int slave,
    std::uint8_t function,
    int start,
    int count
) {
    if (count <= 0) {
        throw std::invalid_argument("read count must be positive");
    }
    if (count > options_.maxRequestRegisters) {
        throw std::invalid_argument(
            "read count exceeds maxRequestRegisters: " + std::to_string(count) +
            " > " + std::to_string(options_.maxRequestRegisters)
        );
    }

    std::vector<std::uint8_t> pdu;
    const auto startWord = makeWord(start);
    const auto countWord = makeWord(count);
    pdu.insert(pdu.end(), startWord.begin(), startWord.end());
    pdu.insert(pdu.end(), countWord.begin(), countWord.end());

    const auto response = transact(slave, function, pdu, 5 + static_cast<std::size_t>(count) * 2);
    return decodeRegistersFromReadResponse(response, function, count);
}

std::vector<std::uint16_t> ModbusRtuClient::executeBitRead(
    int slave,
    std::uint8_t function,
    int start,
    int count
) {
    if (count <= 0) {
        throw std::invalid_argument("read count must be positive");
    }
    if (count > options_.maxRequestRegisters) {
        throw std::invalid_argument(
            "bit read count exceeds maxRequestRegisters: " + std::to_string(count) +
            " > " + std::to_string(options_.maxRequestRegisters)
        );
    }

    std::vector<std::uint8_t> pdu;
    const auto startWord = makeWord(start);
    const auto countWord = makeWord(count);
    pdu.insert(pdu.end(), startWord.begin(), startWord.end());
    pdu.insert(pdu.end(), countWord.begin(), countWord.end());

    const auto byteCount = static_cast<std::size_t>((count + 7) / 8);
    const auto response = transact(slave, function, pdu, 5 + byteCount);
    return decodeBitsFromReadResponse(response, function, count);
}

void ModbusRtuClient::ensurePortOpen() {
    if (!serialPort_->isOpen()) {
        serialPort_->open();
    }
}

std::uint16_t ModbusRtuClient::crc16(const std::vector<std::uint8_t>& bytes) {
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

void ModbusRtuClient::appendCrc(std::vector<std::uint8_t>& frame) {
    const auto crc = crc16(frame);
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xFF));
}

void ModbusRtuClient::validateCrc(const std::vector<std::uint8_t>& frame) {
    if (frame.size() < 4) {
        throw std::runtime_error("frame too short for crc");
    }

    std::vector<std::uint8_t> payload(frame.begin(), frame.end() - 2);
    const auto expected = crc16(payload);
    const auto actual = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(frame[frame.size() - 1] << 8) |
        frame[frame.size() - 2]
    );
    if (expected != actual) {
        throw std::runtime_error("modbus crc mismatch");
    }
}

std::vector<std::uint16_t> ModbusRtuClient::decodeRegistersFromReadResponse(
    const std::vector<std::uint8_t>& response,
    std::uint8_t function,
    int expectedCount
) {
    if (response.size() < 5) {
        throw std::runtime_error("read response too short");
    }
    if (response[1] != function) {
        throw std::runtime_error("read function mismatch");
    }

    const auto byteCount = static_cast<std::size_t>(response[2]);
    if (byteCount != static_cast<std::size_t>(expectedCount) * 2) {
        throw std::runtime_error("unexpected byte count in read response");
    }
    if (response.size() != byteCount + 5) {
        throw std::runtime_error("unexpected response frame size");
    }

    std::vector<std::uint16_t> registers;
    registers.reserve(static_cast<std::size_t>(expectedCount));
    for (std::size_t i = 0; i < byteCount; i += 2) {
        const auto hi = static_cast<std::uint16_t>(response[3 + i]);
        const auto lo = static_cast<std::uint16_t>(response[3 + i + 1]);
        registers.push_back(static_cast<std::uint16_t>((hi << 8) | lo));
    }
    return registers;
}

std::vector<std::uint16_t> ModbusRtuClient::decodeBitsFromReadResponse(
    const std::vector<std::uint8_t>& response,
    std::uint8_t function,
    int expectedCount
) {
    if (response.size() < 5) {
        throw std::runtime_error("bit read response too short");
    }
    if (response[1] != function) {
        throw std::runtime_error("bit read function mismatch");
    }

    const auto byteCount = static_cast<std::size_t>(response[2]);
    const auto expectedByteCount = static_cast<std::size_t>((expectedCount + 7) / 8);
    if (byteCount != expectedByteCount) {
        throw std::runtime_error("unexpected byte count in bit read response");
    }
    if (response.size() != byteCount + 5) {
        throw std::runtime_error("unexpected bit response frame size");
    }

    std::vector<std::uint16_t> bits;
    bits.reserve(static_cast<std::size_t>(expectedCount));
    for (int i = 0; i < expectedCount; ++i) {
        const auto byte = response[3 + static_cast<std::size_t>(i / 8)];
        bits.push_back(static_cast<std::uint16_t>((byte >> (i % 8)) & 0x01U));
    }
    return bits;
}

void ModbusRtuClient::validateWriteEcho(
    const std::vector<std::uint8_t>& response,
    std::uint8_t function,
    int address,
    int countOrValue
) {
    if (response.size() != 8) {
        throw std::runtime_error("write response must be 8 bytes");
    }
    if (response[1] != function) {
        throw std::runtime_error("write function mismatch");
    }

    const auto echoedAddress = static_cast<int>(
        static_cast<std::uint16_t>(response[2] << 8) | response[3]
    );
    const auto echoedValue = static_cast<int>(
        static_cast<std::uint16_t>(response[4] << 8) | response[5]
    );

    if (echoedAddress != address || echoedValue != countOrValue) {
        throw std::runtime_error("write echo mismatch");
    }
}

}  // namespace edge_gateway
