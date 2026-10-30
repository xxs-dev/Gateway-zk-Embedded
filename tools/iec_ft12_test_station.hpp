#pragma once

// Scripted IEC 60870-5-2 FT1.2 secondary station (unbalanced) for driver regression tests.
// SerialStation plugs it behind ISerialPort in-process; TcpStation serves it on loopback TCP
// (station as server, or connecting to the master for the AM5SE reverse mode).
// Test-only code: never contacts a real device.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "edge_gateway/iec_ft12_link.hpp"

namespace ft12_test {

using edge_gateway::Ft12Frame;
using edge_gateway::Ft12FrameParser;
using edge_gateway::buildFt12FixedFrame;
using edge_gateway::buildFt12VariableFrame;

inline std::int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

class Ft12TestStation {
public:
    using Handler = std::function<void(const std::vector<std::uint8_t>&, Ft12TestStation&)>;

    // Configure before or between exchanges (guarded by mutex()).
    int address = 1;
    bool e5Only = false;
    int dropRequests = 0;
    int wrongAddressReplies = 0;
    int dfcReplies = 0;
    std::deque<int> replyDelaysMs;
    std::deque<std::vector<std::uint8_t>> class1;
    std::deque<std::vector<std::uint8_t>> class2;
    Handler onUserData;

    // Observations.
    std::vector<Ft12Frame> requests;
    std::vector<std::vector<std::uint8_t>> userData;
    int repeats = 0;
    int resets = 0;

    std::mutex& mutex() { return mutex_; }

    void receive(const std::vector<std::uint8_t>& bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        parser_.append(bytes);
        Ft12Frame frame;
        while (parser_.next(frame)) {
            handle(frame);
        }
    }

    // Reply bytes whose (simulated) processing time has passed.
    std::vector<std::uint8_t> takeDue(std::int64_t nowMs) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::uint8_t> out;
        while (!pending_.empty() && pending_.front().first <= nowMs) {
            out.insert(out.end(), pending_.front().second.begin(), pending_.front().second.end());
            pending_.pop_front();
        }
        return out;
    }

    int countUserData(std::uint8_t typeId) {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(std::count_if(userData.begin(), userData.end(), [typeId](const std::vector<std::uint8_t>& asdu) {
            return !asdu.empty() && asdu[0] == typeId;
        }));
    }

    std::size_t requestCount() {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests.size();
    }

    void pushClass1(std::vector<std::uint8_t> asdu) {
        std::lock_guard<std::mutex> lock(mutex_);
        class1.push_back(std::move(asdu));
    }

    void pushClass2(std::vector<std::uint8_t> asdu) {
        std::lock_guard<std::mutex> lock(mutex_);
        class2.push_back(std::move(asdu));
    }

private:
    std::uint8_t secondaryControl(int function) {
        std::uint8_t control = static_cast<std::uint8_t>(function);
        if (!class1.empty()) control |= 0x20;
        if (dfcReplies > 0) {
            --dfcReplies;
            control |= 0x10;
        }
        return control;
    }

    void reply(const std::vector<std::uint8_t>& bytes) {
        const int delay = replyDelaysMs.empty() ? 0 : replyDelaysMs.front();
        if (!replyDelaysMs.empty()) replyDelaysMs.pop_front();
        busyUntil_ = std::max(busyUntil_, steadyMs()) + delay;
        pending_.emplace_back(busyUntil_, bytes);
    }

    void handle(const Ft12Frame& frame) {
        requests.push_back(frame);
        if (dropRequests > 0) {
            --dropRequests;
            return;
        }
        const int function = frame.control & 0x0F;
        const bool fcv = (frame.control & 0x10) != 0;
        const int fcb = (frame.control & 0x20) != 0 ? 1 : 0;
        if (wrongAddressReplies > 0) {
            --wrongAddressReplies;
            reply(buildFt12FixedFrame(0x00, address + 1, 1));
        }
        if (function == 0 || function == 7) {
            ++resets;
            expectedFcb_ = 1;
            lastResponse_ = e5Only ? std::vector<std::uint8_t>{0xE5} : buildFt12FixedFrame(secondaryControl(0), address, 1);
            reply(lastResponse_);
            return;
        }
        if (function == 9) {
            reply(buildFt12FixedFrame(secondaryControl(11), address, 1));
            return;
        }
        if (fcv) {
            if (expectedFcb_ >= 0 && fcb != expectedFcb_) {
                // Repetition (same FCB): resend the buffered reply without processing again.
                ++repeats;
                reply(lastResponse_);
                return;
            }
            expectedFcb_ = fcb ^ 1;
        }
        std::vector<std::uint8_t> response;
        if (function == 3) {
            userData.push_back(frame.asdu);
            if (onUserData) onUserData(frame.asdu, *this);
            response = e5Only ? std::vector<std::uint8_t>{0xE5} : buildFt12FixedFrame(secondaryControl(0), address, 1);
        } else if (function == 10 || function == 11) {
            auto& queue = function == 10 ? class1 : class2;
            if (queue.empty()) {
                response = e5Only ? std::vector<std::uint8_t>{0xE5} : buildFt12FixedFrame(secondaryControl(9), address, 1);
            } else {
                const auto asdu = queue.front();
                queue.pop_front();
                response = buildFt12VariableFrame(secondaryControl(8), address, 1, asdu);
            }
        } else {
            response = buildFt12FixedFrame(secondaryControl(15), address, 1);
        }
        lastResponse_ = response;
        reply(response);
    }

    std::mutex mutex_;
    Ft12FrameParser parser_{1};
    std::deque<std::pair<std::int64_t, std::vector<std::uint8_t>>> pending_;
    std::int64_t busyUntil_ = 0;
    int expectedFcb_ = -1;
    std::vector<std::uint8_t> lastResponse_;
};

// In-process serial line to the station.
class SerialStation final : public edge_gateway::ISerialPort {
public:
    Ft12TestStation station;

    void open() override { open_ = true; }
    void close() override { open_ = false; }
    bool isOpen() const override { return open_; }
    void write(const std::vector<std::uint8_t>& bytes) override { station.receive(bytes); }
    std::vector<std::uint8_t> read(std::size_t, int timeoutMs) override {
        const auto deadline = steadyMs() + std::max(0, timeoutMs);
        while (true) {
            auto out = station.takeDue(steadyMs());
            if (!out.empty() || steadyMs() >= deadline) {
                return out;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

private:
    bool open_ = false;
};

// Station on loopback TCP: listen() for the master to connect (AM5SE TCP server mode), or
// connectTo() a master listener (AM5SE reverse connection). Serves connections until stop().
class TcpStation {
public:
    explicit TcpStation(Ft12TestStation& station) : station_(station) {}
    ~TcpStation() { stop(); }

    int listen() {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(listenFd_, 4) != 0) {
            throw std::runtime_error("test station cannot listen");
        }
        socklen_t length = sizeof(address);
        getsockname(listenFd_, reinterpret_cast<sockaddr*>(&address), &length);
        running_ = true;
        thread_ = std::thread([this] {
            while (running_) {
                if (!readable(listenFd_, 20)) continue;
                const int fd = ::accept(listenFd_, nullptr, nullptr);
                if (fd >= 0) serve(fd);
            }
        });
        return ntohs(address.sin_port);
    }

    void connectTo(int port) {
        running_ = true;
        thread_ = std::thread([this, port] {
            while (running_) {
                const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_port = htons(static_cast<std::uint16_t>(port));
                inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
                if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
                    serve(fd);
                } else {
                    ::close(fd);
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }
        });
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        if (listenFd_ >= 0) {
            ::close(listenFd_);
            listenFd_ = -1;
        }
    }

    int connections() const { return connections_.load(); }

private:
    static bool readable(int fd, int timeoutMs) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        timeval timeout{0, timeoutMs * 1000};
        return select(fd + 1, &set, nullptr, nullptr, &timeout) > 0;
    }

    void serve(int fd) {
        ++connections_;
        while (running_) {
            const auto due = station_.takeDue(steadyMs());
            if (!due.empty() && ::send(fd, due.data(), due.size(), MSG_NOSIGNAL) <= 0) {
                break;
            }
            if (!readable(fd, 2)) continue;
            std::vector<std::uint8_t> buffer(1024);
            const auto received = ::recv(fd, buffer.data(), buffer.size(), 0);
            if (received <= 0) {
                break;
            }
            buffer.resize(static_cast<std::size_t>(received));
            station_.receive(buffer);
        }
        ::close(fd);
    }

    Ft12TestStation& station_;
    int listenFd_ = -1;
    std::atomic<bool> running_{false};
    std::atomic<int> connections_{0};
    std::thread thread_;
};

}  // namespace ft12_test
