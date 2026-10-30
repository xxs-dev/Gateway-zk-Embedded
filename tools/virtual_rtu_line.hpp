// Virtual RS-485 line for Modbus RTU tests: wire time at the configured character time plus a device
// latency; answers FC3 (register value == address) and FC6 (echo) for the slaves in `alive`.
#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "edge_gateway/serial_port.hpp"

namespace virtual_rtu {

using Clock = std::chrono::steady_clock;

inline std::uint16_t crc16(const std::vector<std::uint8_t>& bytes) {
    std::uint16_t crc = 0xFFFF;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i) {
            const bool lsb = (crc & 1U) != 0;
            crc >>= 1;
            if (lsb) crc ^= 0xA001;
        }
    }
    return crc;
}

// Answers FC3/FC6 for slaves in `alive` with register value == address, after `latency(start)` ms.
class VirtualLine : public edge_gateway::ISerialPort {
public:
    double charMs = 10.0 / 9600 * 1000;
    std::set<int> alive{1, 2};
    std::set<int> exceptionSlaves;
    std::function<int(int start)> latency = [](int) { return 5; };
    std::vector<Clock::time_point> writeStarts;
    std::vector<Clock::time_point> replyEnds;
    std::map<int, int> requestsBySlave;

    void open() override { open_ = true; }
    void close() override { open_ = false; }
    bool isOpen() const override { return open_; }
    void write(const std::vector<std::uint8_t>& frame) override {
        writeStarts.push_back(Clock::now());
        std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long long>(frame.size() * charMs * 1000)));
        if (frame.size() < 8) return;
        const int slave = frame[0];
        ++requestsBySlave[slave];
        const int function = frame[1];
        const int start = (frame[2] << 8) | frame[3];
        const int count = (frame[4] << 8) | frame[5];
        if (!alive.count(slave)) return;
        std::vector<std::uint8_t> reply{static_cast<std::uint8_t>(slave)};
        if (exceptionSlaves.count(slave)) {
            reply.push_back(static_cast<std::uint8_t>(function | 0x80));
            reply.push_back(2);
        } else if (function == 3) {
            reply.push_back(3);
            reply.push_back(static_cast<std::uint8_t>(count * 2));
            for (int i = 0; i < count; ++i) {
                reply.push_back(static_cast<std::uint8_t>((start + i) >> 8));
                reply.push_back(static_cast<std::uint8_t>((start + i) & 0xFF));
            }
        } else if (function == 6) {
            reply.assign(frame.begin(), frame.begin() + 6);
        } else {
            return;
        }
        const auto crc = crc16(reply);
        reply.push_back(static_cast<std::uint8_t>(crc & 0xFF));
        reply.push_back(static_cast<std::uint8_t>(crc >> 8));
        const auto at = Clock::now() + std::chrono::milliseconds(latency(start)) +
            std::chrono::microseconds(static_cast<long long>(reply.size() * charMs * 1000));
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back({at, reply});
    }
    std::vector<std::uint8_t> read(std::size_t, int timeoutMs) override {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!queue_.empty() && queue_.front().first <= Clock::now()) {
                    auto bytes = queue_.front().second;
                    replyEnds.push_back(queue_.front().first);
                    queue_.pop_front();
                    return bytes;
                }
            }
            if (Clock::now() >= deadline) return {};
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

private:
    bool open_ = false;
    std::mutex mutex_;
    std::deque<std::pair<Clock::time_point, std::vector<std::uint8_t>>> queue_;
};

}  // namespace virtual_rtu
