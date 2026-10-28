#pragma once

#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "edge_gateway/serial_port.hpp"

namespace edge_gateway {

// The meter answered with an exception reply (D1H/D2H): the request was understood and refused.
class Dlt645ErrorResponse : public std::runtime_error {
public:
    Dlt645ErrorResponse(std::uint8_t errorCode, const std::string& message)
        : std::runtime_error(message), errorCode_(errorCode) {}
    std::uint8_t errorCode() const { return errorCode_; }

private:
    std::uint8_t errorCode_;
};

// A write request went out on the bus but no conclusive reply came back (timeout, corrupt or
// unexpected reply, echo mismatch). The meter may have executed it.
class Dlt645OutcomeUnknownError : public std::runtime_error {
public:
    explicit Dlt645OutcomeUnknownError(const std::string& message) : std::runtime_error(message) {}
};

// One client per physical bus: all meters on the port share its transaction lock and quiet time.
class Dlt645Client {
public:
    Dlt645Client(std::shared_ptr<ISerialPort> serialPort, SerialPortOptions options);

    // Data of one DI: the bytes after the DI with 33H removed. Follow-up frames announced by a B1H
    // reply are fetched with C=12H and appended. Throws Dlt645ErrorResponse on a D1H/D2H reply.
    std::vector<std::uint8_t> readData(const std::string& meterAddress, const std::string& dataIdHex);

    // C=14H write. Throws a plain error when the request was not sent or was rejected (D4H), and
    // Dlt645OutcomeUnknownError when it was sent without a conclusive reply. Never retried.
    void writeData(
        const std::string& meterAddress,
        const std::string& dataIdHex,
        const std::string& passwordHex,
        const std::string& operatorCodeHex,
        const std::vector<std::uint8_t>& payload
    );

private:
    using Accept = std::function<bool(const std::vector<std::uint8_t>&)>;

    // Sends frame and returns the first reply from normalizedAddress that accept() takes. Replies
    // from other addresses, requests (echo) and unexpected control codes are skipped until the
    // response timeout. requestSent is set once bytes may have reached the wire.
    std::vector<std::uint8_t> exchange(
        const std::string& normalizedAddress,
        const std::string& dataIdHex,
        const std::vector<std::uint8_t>& frame,
        bool sensitiveRequest,
        const Accept& accept,
        bool& requestSent
    );
    void ensurePortOpen();
    void waitForBusQuietTime();
    void consumeLocalEcho(
        const std::vector<std::uint8_t>& wireFrame,
        std::vector<std::uint8_t>& received
    );

    std::shared_ptr<ISerialPort> serialPort_;
    SerialPortOptions options_;
    // End of the last bus transaction (reply received or timed out); frameIntervalMs of silence
    // follows it before the next request to any meter on the bus.
    std::chrono::steady_clock::time_point busQuietSince_;
    bool busUsed_ = false;
    std::mutex transactionMutex_;
};

}  // namespace edge_gateway
