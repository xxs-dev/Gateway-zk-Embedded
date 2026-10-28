#pragma once

// Shared fixtures for the DL/T 645 regression tests. No real or default meter password appears in
// test data: fixturePassword() is generated per run (level 02, non-zero secret).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "edge_gateway/dlt645_codec.hpp"
#include "edge_gateway/dlt645_mock_serial_port.hpp"
#include "edge_gateway/serial_port.hpp"

namespace dlt645_test {

inline void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

inline std::string fixturePassword() {
    static const std::string value = [] {
        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> digit(1, 9);
        std::string text = "02";
        for (int i = 0; i < 6; ++i) text.push_back(static_cast<char>('0' + digit(rng)));
        return text;
    }();
    return value;
}

inline std::uint8_t checksum(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t sum = 0;
    for (const auto byte : bytes) sum += byte;
    return static_cast<std::uint8_t>(sum & 0xFFU);
}

// 68 A0..A5 68 C L DATA(+33) CS 16; data is given without the 33H offset.
inline std::vector<std::uint8_t> replyFrame(
    const std::string& address,
    std::uint8_t control,
    const std::vector<std::uint8_t>& data
) {
    const auto normalized = edge_gateway::Dlt645Codec::normalizeAddress(address);
    std::vector<std::uint8_t> frame = {0x68};
    for (int i = 10; i >= 0; i -= 2) {
        frame.push_back(static_cast<std::uint8_t>(((normalized[static_cast<std::size_t>(i)] - '0') << 4) |
                                                  (normalized[static_cast<std::size_t>(i + 1)] - '0')));
    }
    frame.push_back(0x68);
    frame.push_back(control);
    frame.push_back(static_cast<std::uint8_t>(data.size()));
    for (const auto byte : data) frame.push_back(static_cast<std::uint8_t>(byte + 0x33U));
    frame.push_back(checksum(frame));
    frame.push_back(0x16);
    return frame;
}

inline std::vector<std::uint8_t> readReply(
    const std::string& address,
    const std::string& dataId,
    const std::vector<std::uint8_t>& payload,
    std::uint8_t control = 0x91
) {
    auto data = edge_gateway::Dlt645Codec::parseDataId(dataId);
    data.insert(data.end(), payload.begin(), payload.end());
    return replyFrame(address, control, data);
}

inline std::vector<std::uint8_t>::const_iterator frameStart(const std::vector<std::uint8_t>& wire) {
    const auto start = std::find(wire.begin(), wire.end(), static_cast<std::uint8_t>(0x68));
    require(start != wire.end() && std::distance(start, wire.end()) >= 12, "not a DL/T 645 frame");
    return start;
}

inline std::uint8_t requestControl(const std::vector<std::uint8_t>& wire) {
    return *(frameStart(wire) + 8);
}

inline std::string requestDataId(const std::vector<std::uint8_t>& wire) {
    const auto start = frameStart(wire);
    static const char* hex = "0123456789ABCDEF";
    std::string text;
    for (int i = 13; i >= 10; --i) {
        const auto byte = static_cast<std::uint8_t>(*(start + i) - 0x33U);
        text.push_back(hex[byte >> 4U]);
        text.push_back(hex[byte & 0x0FU]);
    }
    return text;
}

inline std::string requestAddress(const std::vector<std::uint8_t>& wire) {
    const auto start = frameStart(wire);
    static const char* hex = "0123456789ABCDEF";
    std::string text;
    for (int i = 6; i >= 1; --i) {
        const auto byte = *(start + i);
        text.push_back(hex[byte >> 4U]);
        text.push_back(hex[byte & 0x0FU]);
    }
    return text;
}

// Port whose answers are scripted per request; read() hands out queued chunks.
class ScriptPort : public edge_gateway::ISerialPort {
public:
    std::function<std::vector<std::vector<std::uint8_t>>(const std::vector<std::uint8_t>&)> onWrite;
    std::vector<std::vector<std::uint8_t>> writes;
    std::vector<std::chrono::steady_clock::time_point> writeTimes;
    std::chrono::steady_clock::time_point lastDelivery;
    std::deque<std::vector<std::uint8_t>> rx;
    bool opened = false;

    void open() override { opened = true; }
    void close() override { opened = false; }
    bool isOpen() const override { return opened; }
    void write(const std::vector<std::uint8_t>& bytes) override {
        writes.push_back(bytes);
        writeTimes.push_back(std::chrono::steady_clock::now());
        if (onWrite) {
            for (auto& chunk : onWrite(bytes)) rx.push_back(chunk);
        }
    }
    std::vector<std::uint8_t> read(std::size_t maxBytes, int timeoutMs) override {
        if (rx.empty()) {
            if (timeoutMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return {};
        }
        auto chunk = rx.front();
        rx.pop_front();
        if (chunk.size() > maxBytes) {
            rx.push_front(std::vector<std::uint8_t>(chunk.begin() + static_cast<std::ptrdiff_t>(maxBytes), chunk.end()));
            chunk.resize(maxBytes);
        }
        lastDelivery = std::chrono::steady_clock::now();
        return chunk;
    }
};

// Simulated meters (Dlt645MockSerialPort) that can be switched off; logs the requested DIs.
class MeterPort : public edge_gateway::ISerialPort {
public:
    edge_gateway::Dlt645MockSerialPort meter;
    bool alive = true;
    bool acknowledgeWrites = true;
    std::vector<std::string> requests;  // "address:DI:control"
    std::vector<std::int64_t> requestTimes;
    const std::int64_t* clock = nullptr;

    void open() override { meter.open(); }
    void close() override { meter.close(); }
    bool isOpen() const override { return meter.isOpen(); }
    void write(const std::vector<std::uint8_t>& bytes) override {
        const auto control = requestControl(bytes);
        requests.push_back(requestAddress(bytes) + ":" + requestDataId(bytes) + ":" + std::to_string(control));
        requestTimes.push_back(clock != nullptr ? *clock : 0);
        if (!alive || (control == 0x14 && !acknowledgeWrites)) return;
        meter.write(bytes);
    }
    std::vector<std::uint8_t> read(std::size_t maxBytes, int timeoutMs) override {
        return meter.read(maxBytes, timeoutMs);
    }
    std::size_t count(const std::string& dataId, int control = 0x11) const {
        std::size_t total = 0;
        const auto suffix = ":" + dataId + ":" + std::to_string(control);
        for (const auto& request : requests) {
            if (request.size() > suffix.size() &&
                request.compare(request.size() - suffix.size(), suffix.size(), suffix) == 0) {
                ++total;
            }
        }
        return total;
    }
};

}  // namespace dlt645_test
