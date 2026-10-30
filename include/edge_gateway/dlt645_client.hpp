#pragma once

#include <cstdint>
#include <chrono>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "edge_gateway/dlt645_version.hpp"
#include "edge_gateway/serial_port.hpp"

namespace edge_gateway {

// The meter answered with an exception reply (read D1H/C1H, follow-up D2H/C2H): the request was
// understood and refused.
class Dlt645ErrorResponse : public std::runtime_error {
public:
    Dlt645ErrorResponse(std::uint8_t errorCode, const std::string& message)
        : std::runtime_error(message), errorCode_(errorCode) {}
    std::uint8_t errorCode() const { return errorCode_; }

private:
    std::uint8_t errorCode_;
};

// A command (write, remote control) went out on the bus but no conclusive reply came back
// (timeout, corrupt or unexpected reply, echo mismatch). The meter may have executed it.
class Dlt645OutcomeUnknownError : public std::runtime_error {
public:
    explicit Dlt645OutcomeUnknownError(const std::string& message) : std::runtime_error(message) {}
};

// One client per physical bus and DL/T 645 edition: all meters on the port share its transaction
// lock and quiet time, so broadcasts and diagnostics never cut into a running transaction.
class Dlt645Client {
public:
    Dlt645Client(
        std::shared_ptr<ISerialPort> serialPort,
        SerialPortOptions options,
        Dlt645Version version = Dlt645Version::V2007
    );

    Dlt645Version version() const { return version_; }

    // Data of one DI: the bytes after the DI with 33H removed. Follow-up frames announced by the
    // reply are fetched (2007 C=12H + sequence, 1997 C=02H) and appended. Throws
    // Dlt645ErrorResponse on an exception reply.
    std::vector<std::uint8_t> readData(const std::string& meterAddress, const std::string& dataIdHex);

    // Write data (2007 C=14H, 1997 C=04H without operator code). Throws a plain error when the
    // request was not sent or was rejected, Dlt645OutcomeUnknownError when it was sent without a
    // conclusive reply. Never retried.
    void writeData(
        const std::string& meterAddress,
        const std::string& dataIdHex,
        const std::string& passwordHex,
        const std::string& operatorCodeHex,
        const std::vector<std::uint8_t>& payload
    );

    // DL/T 645-2007 C=1CH remote control (plain password, no ESAM); reply 9CH, exception DCH.
    // Same outcome rules as writeData.
    void remoteControl(
        const std::string& meterAddress,
        const std::string& passwordHex,
        const std::string& operatorCodeHex,
        std::uint8_t controlTypeCode,
        const std::tm& validUntil
    );

    // C=08H broadcast time sync (no reply). Waits for the running transaction and the quiet time,
    // then reads wallClockMs so the meters get the time of sending. The quiet time follows it.
    // Throws a plain error when nothing was sent, Dlt645OutcomeUnknownError when the frame may
    // have reached the wire (local echo missing or different).
    void broadcastTime(const std::function<std::int64_t()>& wallClockMs);

    // Diagnostic: address of the only meter answering the wildcard pattern (see
    // Dlt645Codec::buildAddressQueryFrame). Listens for the whole timeout and throws when several
    // meters answer or replies collide.
    std::string queryAddress(const std::string& pattern);

private:
    using Accept = std::function<bool(const std::vector<std::uint8_t>&)>;

    void sendFrame(const std::vector<std::uint8_t>& frame, const std::string& label, bool sensitive,
                   std::vector<std::uint8_t>& received, bool& requestSent);
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
    void sendCommand(
        const std::string& normalizedAddress,
        const std::string& label,
        const std::vector<std::uint8_t>& frame,
        std::uint8_t requestControl
    );
    void ensurePortOpen();
    void waitForBusQuietTime();
    void markBusQuiet();
    void consumeLocalEcho(
        const std::vector<std::uint8_t>& wireFrame,
        std::vector<std::uint8_t>& received
    );

    std::shared_ptr<ISerialPort> serialPort_;
    SerialPortOptions options_;
    Dlt645Version version_;
    // End of the last bus transaction (reply received or timed out); frameIntervalMs of silence
    // follows it before the next request to any meter on the bus.
    std::chrono::steady_clock::time_point busQuietSince_;
    bool busUsed_ = false;
    std::mutex transactionMutex_;
};

}  // namespace edge_gateway
