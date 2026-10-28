#include "edge_gateway/dlt645_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#include "edge_gateway/dlt645_codec.hpp"

namespace edge_gateway {

namespace {

constexpr int kMaxFollowUpFrames = 32;

bool dlt645DebugEnabled() {
    const char* value = std::getenv("GATEWAY_DLT645_DEBUG");
    return value != nullptr && std::string(value) != "0" && std::string(value) != "false";
}

std::string bytesToHex(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        out << std::setw(2) << static_cast<int>(byte);
    }
    return out.str();
}

void drainStaleInput(edge_gateway::ISerialPort& serialPort) {
    for (int i = 0; i < 16; ++i) {
        if (serialPort.read(256, 0).empty()) {
            return;
        }
    }
}

int remainingMs(std::chrono::steady_clock::time_point deadline) {
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()
    ).count());
}

// Data identifier echoed at the start of a normal reply's data field (33H removed).
bool dataIdMatches(const std::vector<std::uint8_t>& frame, const std::vector<std::uint8_t>& dataId) {
    if (frame.size() < 10U + dataId.size() + 2U || frame[9] < dataId.size()) {
        return false;
    }
    for (std::size_t i = 0; i < dataId.size(); ++i) {
        if (static_cast<std::uint8_t>(frame[10 + i] - 0x33U) != dataId[i]) {
            return false;
        }
    }
    return true;
}

std::uint8_t errorCodeOf(const std::vector<std::uint8_t>& frame) {
    return frame.size() >= 13U && frame[9] >= 1U ? static_cast<std::uint8_t>(frame[10] - 0x33U) : 0U;
}

std::uint8_t normalReply(std::uint8_t control) { return static_cast<std::uint8_t>(control | 0x80U); }
std::uint8_t followUpReply(std::uint8_t control) { return static_cast<std::uint8_t>(control | 0xA0U); }
std::uint8_t exceptionReply(std::uint8_t control) { return static_cast<std::uint8_t>(control | 0xC0U); }

}  // namespace

Dlt645Client::Dlt645Client(
    std::shared_ptr<ISerialPort> serialPort,
    SerialPortOptions options,
    Dlt645Version version
) : serialPort_(std::move(serialPort)),
    options_(std::move(options)),
    version_(version) {
    if (!serialPort_) {
        throw std::invalid_argument("serialPort is required");
    }
    if (options_.wakeupBytes < 0 || options_.wakeupBytes > 16) {
        throw std::invalid_argument("DLT645 wakeupBytes must be 0..16");
    }
}

std::vector<std::uint8_t> Dlt645Client::readData(const std::string& meterAddress, const std::string& dataIdHex) {
    std::lock_guard<std::mutex> lock(transactionMutex_);
    const auto normalizedAddress = Dlt645Codec::normalizeAddress(meterAddress);
    const auto dataId = Dlt645Codec::parseDataId(dataIdHex);
    const auto read = Dlt645Codec::readControl(version_);
    bool sent = false;
    const auto response = exchange(
        normalizedAddress,
        dataIdHex,
        Dlt645Codec::buildReadFrame(normalizedAddress, dataIdHex, version_),
        false,
        [&dataId, read](const std::vector<std::uint8_t>& frame) {
            // The exception reply carries no DI; a normal reply must echo the requested one.
            return frame[8] == exceptionReply(read) ||
                ((frame[8] == normalReply(read) || frame[8] == followUpReply(read)) && dataIdMatches(frame, dataId));
        },
        sent
    );
    if (response[8] == exceptionReply(read)) {
        const auto code = errorCodeOf(response);
        throw Dlt645ErrorResponse(code, "DLT645 exception response " + Dlt645Codec::describeError(code, version_));
    }
    auto data = Dlt645Codec::decodeFrameData(response, dataId, version_);
    if (response[8] != followUpReply(read)) {
        return data;
    }

    // More data follows: fetch it frame by frame (2007 with a sequence number).
    const auto followUpRequest = Dlt645Codec::followUpControl(version_);
    for (int sequence = 1; sequence <= kMaxFollowUpFrames; ++sequence) {
        const auto seq = static_cast<std::uint8_t>(sequence);
        const auto followUp = exchange(
            normalizedAddress,
            dataIdHex,
            Dlt645Codec::buildReadFollowUpFrame(normalizedAddress, dataIdHex, seq, version_),
            false,
            [&dataId, followUpRequest](const std::vector<std::uint8_t>& frame) {
                return frame[8] == exceptionReply(followUpRequest) ||
                    ((frame[8] == normalReply(followUpRequest) || frame[8] == followUpReply(followUpRequest)) &&
                     dataIdMatches(frame, dataId));
            },
            sent
        );
        if (followUp[8] == exceptionReply(followUpRequest)) {
            const auto code = errorCodeOf(followUp);
            throw Dlt645ErrorResponse(code, "DLT645 follow-up exception response " +
                Dlt645Codec::describeError(code, version_));
        }
        auto part = Dlt645Codec::decodeFrameData(followUp, dataId, version_);
        if (version_ == Dlt645Version::V2007) {
            if (part.empty() || part.back() != seq) {
                throw std::runtime_error("DLT645 follow-up frame sequence mismatch");
            }
            part.pop_back();
        }
        data.insert(data.end(), part.begin(), part.end());
        if (followUp[8] == normalReply(followUpRequest)) {
            return data;
        }
    }
    throw std::runtime_error("DLT645 follow-up frames exceed " + std::to_string(kMaxFollowUpFrames));
}

void Dlt645Client::writeData(
    const std::string& meterAddress,
    const std::string& dataIdHex,
    const std::string& passwordHex,
    const std::string& operatorCodeHex,
    const std::vector<std::uint8_t>& payload
) {
    std::lock_guard<std::mutex> lock(transactionMutex_);
    const auto normalizedAddress = Dlt645Codec::normalizeAddress(meterAddress);
    const auto frame = Dlt645Codec::buildWriteFrame(
        normalizedAddress, dataIdHex, passwordHex, operatorCodeHex, payload, version_
    );
    sendCommand(normalizedAddress, dataIdHex, frame, Dlt645Codec::writeControl(version_));
}

void Dlt645Client::remoteControl(
    const std::string& meterAddress,
    const std::string& passwordHex,
    const std::string& operatorCodeHex,
    std::uint8_t controlTypeCode,
    const std::tm& validUntil
) {
    if (version_ != Dlt645Version::V2007) {
        throw std::invalid_argument("DLT645 remote control (C=1CH) is DL/T 645-2007 only");
    }
    std::lock_guard<std::mutex> lock(transactionMutex_);
    const auto normalizedAddress = Dlt645Codec::normalizeAddress(meterAddress);
    const auto frame = Dlt645Codec::buildRemoteControlFrame(
        normalizedAddress, passwordHex, operatorCodeHex, controlTypeCode, validUntil
    );
    sendCommand(normalizedAddress, "remote-control", frame, Dlt645Codec::kRemoteControl);
}

void Dlt645Client::sendCommand(
    const std::string& normalizedAddress,
    const std::string& label,
    const std::vector<std::uint8_t>& frame,
    std::uint8_t requestControl
) {
    bool sent = false;
    std::vector<std::uint8_t> response;
    try {
        response = exchange(
            normalizedAddress,
            label,
            frame,
            true,
            [requestControl](const std::vector<std::uint8_t>& reply) {
                return reply[8] == normalReply(requestControl) || reply[8] == exceptionReply(requestControl);
            },
            sent
        );
    } catch (const std::exception& ex) {
        if (!sent) {
            throw;
        }
        throw Dlt645OutcomeUnknownError(
            std::string("DLT645 command outcome unknown (request sent, no conclusive reply): ") + ex.what()
        );
    }
    if (response[8] == exceptionReply(requestControl)) {
        // Definite refusal ("... rejected ERR=0xNN (meaning)").
        Dlt645Codec::validateCommandResponse(response, normalizedAddress, requestControl, version_);
    }
    try {
        Dlt645Codec::validateCommandResponse(response, normalizedAddress, requestControl, version_);
    } catch (const std::exception& ex) {
        throw Dlt645OutcomeUnknownError(std::string("DLT645 command outcome unknown (malformed reply): ") + ex.what());
    }
}

void Dlt645Client::broadcastTime(const std::function<std::int64_t()>& wallClockMs) {
    std::lock_guard<std::mutex> lock(transactionMutex_);
    std::vector<std::uint8_t> received;
    bool sent = false;
    try {
        waitForBusQuietTime();
        const auto frame = Dlt645Codec::buildBroadcastTimeFrame(Dlt645Codec::localTime(wallClockMs()));
        sendFrame(frame, "broadcast-time", false, received, sent);
    } catch (...) {
        markBusQuiet();
        throw;
    }
    markBusQuiet();  // no reply: the quiet time starts right after sending
}

std::string Dlt645Client::queryAddress(const std::string& pattern) {
    std::lock_guard<std::mutex> lock(transactionMutex_);
    const auto frame = Dlt645Codec::buildAddressQueryFrame(pattern, version_);
    const auto request = frame[8];
    std::vector<std::uint8_t> buffer;
    bool sent = false;
    try {
        sendFrame(frame, "address-query", false, buffer, sent);
    } catch (...) {
        markBusQuiet();
        throw;
    }
    // Listen for the whole timeout: a second answer means more than one meter matched.
    std::set<std::string> addresses;
    // Bytes that ended up in no valid frame (wake-up FE excluded) mean colliding or corrupt replies.
    const auto meaningful = [](const std::vector<std::uint8_t>& bytes) {
        return static_cast<std::size_t>(std::count_if(bytes.begin(), bytes.end(), [](std::uint8_t b) { return b != 0xFE; }));
    };
    std::size_t receivedBytes = meaningful(buffer);
    std::size_t framedBytes = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.timeoutMs);
    while (true) {
        std::vector<std::uint8_t> candidate;
        while (Dlt645Codec::takeFrame(buffer, candidate)) {
            framedBytes += candidate.size();
            // Any reply (even an exception reply) names the meter that sent it; requests (echo) do not.
            if ((candidate[8] & 0x80U) != 0 && (candidate[8] & 0x1FU) == (request & 0x1FU)) {
                addresses.insert(Dlt645Codec::frameAddress(candidate));
            }
        }
        const auto waitMs = remainingMs(deadline);
        if (waitMs <= 0) {
            break;
        }
        const auto chunk = serialPort_->read(256, waitMs);
        receivedBytes += meaningful(chunk);
        buffer.insert(buffer.end(), chunk.begin(), chunk.end());
    }
    markBusQuiet();
    const bool corrupt = receivedBytes > framedBytes;
    if (addresses.size() > 1U) {
        std::string list;
        for (const auto& address : addresses) list += (list.empty() ? "" : ", ") + address;
        throw std::runtime_error("DLT645 address query: several meters answered (" + list +
            "); disconnect the others or use a shortened address pattern");
    }
    if (addresses.empty()) {
        throw std::runtime_error(corrupt
            ? "DLT645 address query: replies collided or were corrupt (several meters on the bus?)"
            : "DLT645 address query: no meter answered");
    }
    if (corrupt) {
        throw std::runtime_error("DLT645 address query: one reply (" + *addresses.begin() +
            ") plus corrupt bytes; several meters may have answered at once");
    }
    return *addresses.begin();
}

void Dlt645Client::sendFrame(
    const std::vector<std::uint8_t>& frame,
    const std::string& label,
    bool sensitive,
    std::vector<std::uint8_t>& received,
    bool& requestSent
) {
    ensurePortOpen();
    std::vector<std::uint8_t> wireFrame;
    wireFrame.reserve(static_cast<std::size_t>(options_.wakeupBytes) + frame.size());
    wireFrame.insert(wireFrame.end(), static_cast<std::size_t>(options_.wakeupBytes), static_cast<std::uint8_t>(0xFE));
    wireFrame.insert(wireFrame.end(), frame.begin(), frame.end());
    if (dlt645DebugEnabled()) {
        std::cerr << "[dlt645] tx"
                  << " " << label
                  << " wakeupBytes=" << options_.wakeupBytes
                  << " size=" << wireFrame.size()
                  << " hex=" << (sensitive ? "<redacted>" : bytesToHex(wireFrame))
                  << std::endl;
    }
    waitForBusQuietTime();
    // Discard late answers to an earlier (timed-out) request so they cannot be
    // taken as the response to this one.
    drainStaleInput(*serialPort_);
    requestSent = true;
    serialPort_->write(wireFrame);
    if (options_.localEcho) {
        consumeLocalEcho(wireFrame, received);
    }
}

std::vector<std::uint8_t> Dlt645Client::exchange(
    const std::string& normalizedAddress,
    const std::string& dataIdHex,
    const std::vector<std::uint8_t>& frame,
    bool sensitiveRequest,
    const Accept& accept,
    bool& requestSent
) {
    // Whatever happens from here on occupied the bus: the next request waits for the quiet time.
    struct BusRelease {
        Dlt645Client& client;
        bool& active;
        ~BusRelease() {
            if (active) client.markBusQuiet();
        }
    } release{*this, requestSent};

    std::vector<std::uint8_t> buffer;
    sendFrame(frame, "address=" + normalizedAddress + " di=" + dataIdHex, sensitiveRequest, buffer, requestSent);

    bool foreignReply = false;
    bool unexpectedReply = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.timeoutMs);
    while (true) {
        std::vector<std::uint8_t> candidate;
        while (Dlt645Codec::takeFrame(buffer, candidate)) {
            if ((candidate[8] & 0x80U) == 0) {
                continue;  // a request (adapter echo or another master), not a reply
            }
            if (!Dlt645Codec::frameAddressMatches(candidate, normalizedAddress)) {
                foreignReply = true;  // late answer of another meter on the bus
                continue;
            }
            if (!accept(candidate)) {
                unexpectedReply = true;  // e.g. late answer of an earlier request to this meter
                continue;
            }
            if (dlt645DebugEnabled()) {
                std::cerr << "[dlt645] rx"
                          << " address=" << normalizedAddress
                          << " di=" << dataIdHex
                          << " size=" << candidate.size()
                          << " hex=" << bytesToHex(candidate)
                          << std::endl;
            }
            return candidate;
        }
        const auto waitMs = remainingMs(deadline);
        if (waitMs <= 0) {
            break;
        }
        const auto chunk = serialPort_->read(256, waitMs);
        buffer.insert(buffer.end(), chunk.begin(), chunk.end());
    }

    if (unexpectedReply) {
        throw std::runtime_error("DLT645 response control code or data id mismatch");
    }
    if (foreignReply) {
        throw std::runtime_error("DLT645 response address mismatch");
    }
    if (!buffer.empty()) {
        throw std::runtime_error("DLT645 incomplete or corrupt response");
    }
    throw std::runtime_error("DLT645 response timeout");
}

void Dlt645Client::consumeLocalEcho(
    const std::vector<std::uint8_t>& wireFrame,
    std::vector<std::uint8_t>& received
) {
    // The RS-485 adapter loops our own bytes back first; they must match what was sent.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.timeoutMs);
    while (received.size() < wireFrame.size()) {
        const auto waitMs = remainingMs(deadline);
        if (waitMs <= 0) {
            throw std::runtime_error("DLT645 local echo timeout (transport.localEcho=true)");
        }
        const auto chunk = serialPort_->read(wireFrame.size() - received.size(), waitMs);
        received.insert(received.end(), chunk.begin(), chunk.end());
    }
    if (!std::equal(wireFrame.begin(), wireFrame.end(), received.begin())) {
        throw std::runtime_error("DLT645 local echo mismatch (bus collision or adapter without echo)");
    }
    received.erase(received.begin(), received.begin() + static_cast<std::ptrdiff_t>(wireFrame.size()));
}

void Dlt645Client::markBusQuiet() {
    busQuietSince_ = std::chrono::steady_clock::now();
    busUsed_ = true;
}

void Dlt645Client::waitForBusQuietTime() {
    const auto intervalMs = std::max(0, options_.frameIntervalMs);
    if (intervalMs <= 0 || !busUsed_) {
        return;
    }
    const auto nextWriteAt = busQuietSince_ + std::chrono::milliseconds(intervalMs);
    if (std::chrono::steady_clock::now() < nextWriteAt) {
        std::this_thread::sleep_until(nextWriteAt);
    }
}

void Dlt645Client::ensurePortOpen() {
    if (!serialPort_->isOpen()) {
        serialPort_->open();
    }
}

}  // namespace edge_gateway
