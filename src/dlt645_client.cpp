#include "edge_gateway/dlt645_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "edge_gateway/dlt645_codec.hpp"

namespace edge_gateway {

namespace {

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

void trimToFrameStart(std::vector<std::uint8_t>& bytes) {
    const auto it = std::find(bytes.begin(), bytes.end(), 0x68);
    if (it == bytes.end()) {
        if (bytes.size() > 32) {
            bytes.clear();
        }
        return;
    }
    if (it != bytes.begin()) {
        bytes.erase(bytes.begin(), it);
    }
}

std::size_t expectedDlt645FrameSize(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 10) {
        return 0;
    }
    if (bytes[0] != 0x68 || bytes[7] != 0x68) {
        return 0;
    }
    return static_cast<std::size_t>(bytes[9]) + 12U;
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
}

std::vector<std::uint8_t> Dlt645Client::readData(const std::string& meterAddress, const std::string& dataIdHex) {
    ensurePortOpen();
    const auto frame = Dlt645Codec::buildReadFrame(meterAddress, dataIdHex);
    if (dlt645DebugEnabled()) {
        std::cerr << "[dlt645] tx"
                  << " address=" << meterAddress
                  << " di=" << dataIdHex
                  << " size=" << frame.size()
                  << " hex=" << bytesToHex(frame)
                  << std::endl;
    }
    serialPort_->write(frame);

    std::vector<std::uint8_t> response;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remainingMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count()
        );
        if (remainingMs <= 0) {
            break;
        }
        auto chunk = serialPort_->read(256, remainingMs);
        if (chunk.empty()) {
            continue;
        }
        response.insert(response.end(), chunk.begin(), chunk.end());
        trimToFrameStart(response);
        const auto expectedSize = expectedDlt645FrameSize(response);
        if (expectedSize > 0 && response.size() >= expectedSize && response[expectedSize - 1] == 0x16) {
            response.resize(expectedSize);
            break;
        }
    }

    if (response.empty()) {
        throw std::runtime_error("DLT645 response timeout");
    }
    if (dlt645DebugEnabled()) {
        std::cerr << "[dlt645] rx"
                  << " address=" << meterAddress
                  << " di=" << dataIdHex
                  << " size=" << response.size()
                  << " hex=" << bytesToHex(response)
                  << std::endl;
    }
    return response;
}

void Dlt645Client::ensurePortOpen() {
    if (!serialPort_->isOpen()) {
        serialPort_->open();
    }
}

}  // namespace edge_gateway
