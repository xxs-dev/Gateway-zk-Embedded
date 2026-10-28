#include "edge_gateway/dlt645_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
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

std::string hexByte(std::uint8_t value) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(value);
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
    if (frame.size() < 14U || frame[9] < 4U) {
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

}  // namespace

Dlt645Client::Dlt645Client(
    std::shared_ptr<ISerialPort> serialPort,
    SerialPortOptions options
) : serialPort_(std::move(serialPort)),
    options_(std::move(options)) {
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
    bool sent = false;
    const auto response = exchange(
        normalizedAddress,
        dataIdHex,
        Dlt645Codec::buildReadFrame(normalizedAddress, dataIdHex),
        false,
        [&dataId](const std::vector<std::uint8_t>& frame) {
            // D1H carries no DI; a 91H/B1H reply must echo the requested one.
            return frame[8] == 0xD1 || ((frame[8] == 0x91 || frame[8] == 0xB1) && dataIdMatches(frame, dataId));
        },
        sent
    );
    if (response[8] == 0xD1) {
        const auto code = errorCodeOf(response);
        throw Dlt645ErrorResponse(code, "DLT645 exception response code=0x" + hexByte(code));
    }
    auto data = Dlt645Codec::decodeFrameData(response, dataId);
    if (response[8] != 0xB1) {
        return data;
    }

    // B1H: more data follows; fetch it frame by frame with C=12H and a sequence number.
    for (int sequence = 1; sequence <= kMaxFollowUpFrames; ++sequence) {
        const auto seq = static_cast<std::uint8_t>(sequence);
        const auto followUp = exchange(
            normalizedAddress,
            dataIdHex,
            Dlt645Codec::buildReadFollowUpFrame(normalizedAddress, dataIdHex, seq),
            false,
            [&dataId](const std::vector<std::uint8_t>& frame) {
                return frame[8] == 0xD2 || ((frame[8] == 0x92 || frame[8] == 0xB2) && dataIdMatches(frame, dataId));
            },
            sent
        );
        if (followUp[8] == 0xD2) {
            const auto code = errorCodeOf(followUp);
            throw Dlt645ErrorResponse(code, "DLT645 follow-up exception response code=0x" + hexByte(code));
        }
        auto part = Dlt645Codec::decodeFrameData(followUp, dataId);
        if (part.empty() || part.back() != seq) {
            throw std::runtime_error("DLT645 follow-up frame sequence mismatch");
        }
        part.pop_back();
        data.insert(data.end(), part.begin(), part.end());
        if (followUp[8] == 0x92) {
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
        normalizedAddress,
        dataIdHex,
        passwordHex,
        operatorCodeHex,
        payload
    );
    bool sent = false;
    std::vector<std::uint8_t> response;
    try {
        response = exchange(
            normalizedAddress,
            dataIdHex,
            frame,
            true,
            [](const std::vector<std::uint8_t>& reply) { return reply[8] == 0x94 || reply[8] == 0xD4; },
            sent
        );
    } catch (const std::exception& ex) {
        if (!sent) {
            throw;
        }
        throw Dlt645OutcomeUnknownError(
            std::string("DLT645 write outcome unknown (request sent, no conclusive reply): ") + ex.what()
        );
    }
    if (response[8] == 0xD4) {
        Dlt645Codec::validateWriteResponse(response, normalizedAddress);  // throws "write rejected ERR="
    }
    try {
        Dlt645Codec::validateWriteResponse(response, normalizedAddress);
    } catch (const std::exception& ex) {
        throw Dlt645OutcomeUnknownError(std::string("DLT645 write outcome unknown (malformed reply): ") + ex.what());
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
    ensurePortOpen();
    std::vector<std::uint8_t> wireFrame;
    wireFrame.reserve(static_cast<std::size_t>(options_.wakeupBytes) + frame.size());
    wireFrame.insert(
        wireFrame.end(),
        static_cast<std::size_t>(options_.wakeupBytes),
        static_cast<std::uint8_t>(0xFE)
    );
    wireFrame.insert(wireFrame.end(), frame.begin(), frame.end());
    if (dlt645DebugEnabled()) {
        std::cerr << "[dlt645] tx"
                  << " address=" << normalizedAddress
                  << " di=" << dataIdHex
                  << " wakeupBytes=" << options_.wakeupBytes
                  << " size=" << wireFrame.size()
                  << " hex=" << (sensitiveRequest ? "<redacted>" : bytesToHex(wireFrame))
                  << std::endl;
    }
    waitForBusQuietTime();
    // Discard late answers to an earlier (timed-out) request so they cannot be
    // taken as the response to this one.
    drainStaleInput(*serialPort_);

    // Whatever happens from here on occupied the bus: the next request waits for the quiet time.
    struct BusRelease {
        Dlt645Client& client;
        ~BusRelease() {
            client.busQuietSince_ = std::chrono::steady_clock::now();
            client.busUsed_ = true;
        }
    } release{*this};

    requestSent = true;
    serialPort_->write(wireFrame);

    std::vector<std::uint8_t> buffer;
    if (options_.localEcho) {
        consumeLocalEcho(wireFrame, buffer);
    }

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
