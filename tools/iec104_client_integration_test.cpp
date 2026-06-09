#include "edge_gateway/iec_client.hpp"
#include "edge_gateway/iec_codec.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using edge_gateway::IecCodec;
using edge_gateway::IecProtocolConfig;
using edge_gateway::PointDefinition;
using edge_gateway::TcpTransportConfig;

void requireTrue(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireNear(double actual, double expected, const std::string& message) {
    if (std::fabs(actual - expected) > 1e-6) {
        throw std::runtime_error(message + " actual=" + std::to_string(actual));
    }
}

void closeFd(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

void sendAll(int fd, const std::vector<std::uint8_t>& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto rc = send(fd, bytes.data() + sent, bytes.size() - sent, 0);
        if (rc <= 0) {
            throw std::runtime_error("test server send failed");
        }
        sent += static_cast<std::size_t>(rc);
    }
}

std::vector<std::uint8_t> readFrame(int fd, int timeoutMs = 3000) {
    timeval timeout;
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    std::uint8_t header[2] = {0, 0};
    const auto h0 = recv(fd, header, 2, MSG_WAITALL);
    if (h0 != 2 || header[0] != 0x68) {
        return {};
    }
    std::vector<std::uint8_t> frame;
    frame.push_back(header[0]);
    frame.push_back(header[1]);
    const auto remaining = static_cast<std::size_t>(header[1]);
    frame.resize(2 + remaining);
    const auto rc = recv(fd, frame.data() + 2, remaining, MSG_WAITALL);
    if (rc != static_cast<ssize_t>(remaining)) {
        return {};
    }
    return frame;
}

std::vector<std::uint8_t> buildSinglePointFrame(
    IecProtocolConfig config,
    std::uint16_t sendSequence,
    std::uint16_t receiveSequence,
    int cause,
    int ioa,
    bool value
) {
    std::vector<std::uint8_t> asdu = {
        1,
        1,
        static_cast<std::uint8_t>(cause & 0x3F),
        0,
        static_cast<std::uint8_t>(config.commonAddress & 0xFF),
        static_cast<std::uint8_t>((config.commonAddress >> 8) & 0xFF),
        static_cast<std::uint8_t>(ioa & 0xFF),
        static_cast<std::uint8_t>((ioa >> 8) & 0xFF),
        static_cast<std::uint8_t>((ioa >> 16) & 0xFF),
        static_cast<std::uint8_t>(value ? 1 : 0)
    };
    std::vector<std::uint8_t> frame;
    frame.push_back(0x68);
    frame.push_back(static_cast<std::uint8_t>(4 + asdu.size()));
    const auto send = static_cast<std::uint16_t>(sendSequence << 1);
    const auto recv = static_cast<std::uint16_t>(receiveSequence << 1);
    frame.push_back(static_cast<std::uint8_t>(send & 0xFF));
    frame.push_back(static_cast<std::uint8_t>((send >> 8) & 0xFF));
    frame.push_back(static_cast<std::uint8_t>(recv & 0xFF));
    frame.push_back(static_cast<std::uint8_t>((recv >> 8) & 0xFF));
    frame.insert(frame.end(), asdu.begin(), asdu.end());
    return frame;
}

class TestIec104Server {
public:
    explicit TestIec104Server(IecProtocolConfig config) : config_(config) {
        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) {
            throw std::runtime_error("socket failed");
        }
        int one = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(0);
        if (bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            throw std::runtime_error("bind failed");
        }
        socklen_t len = sizeof(addr);
        if (getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            throw std::runtime_error("getsockname failed");
        }
        port_ = ntohs(addr.sin_port);
        if (listen(listenFd_, 1) != 0) {
            throw std::runtime_error("listen failed");
        }
    }

    ~TestIec104Server() {
        stop();
    }

    int port() const {
        return port_;
    }

    void start() {
        running_.store(true);
        thread_ = std::thread(&TestIec104Server::run, this);
    }

    void stop() {
        running_.store(false);
        closeFd(clientFd_);
        clientFd_ = -1;
        closeFd(listenFd_);
        listenFd_ = -1;
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    bool sawSelect() const {
        return sawSelect_.load();
    }

    bool sawExecute() const {
        return sawExecute_.load();
    }

    bool sawClockSync() const {
        return sawClockSync_.load();
    }

private:
    void run() {
        clientFd_ = accept(listenFd_, nullptr, nullptr);
        if (clientFd_ < 0) {
            return;
        }
        while (running_.load()) {
            const auto frame = readFrame(clientFd_, 1000);
            if (frame.empty()) {
                continue;
            }
            if (frame == std::vector<std::uint8_t>{0x68, 0x04, 0x07, 0x00, 0x00, 0x00}) {
                sendAll(clientFd_, {0x68, 0x04, 0x0B, 0x00, 0x00, 0x00});
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                sendAll(clientFd_, buildSinglePointFrame(config_, serverSendSeq_++, 1, 3, 1001, true));
                continue;
            }
            if (frame == std::vector<std::uint8_t>{0x68, 0x04, 0x43, 0x00, 0x00, 0x00}) {
                sendAll(clientFd_, {0x68, 0x04, 0x83, 0x00, 0x00, 0x00});
                continue;
            }
            if (!IecCodec::isIec104IFrame(frame)) {
                continue;
            }
            const auto asdu = IecCodec::iec104AsduPayload(frame);
            if (asdu.empty()) {
                continue;
            }
            if (asdu[0] == 45 && asdu.size() >= 10) {
                sendSFrame(IecCodec::iec104SendSequence(frame) + 1);
                const bool select = (asdu.back() & 0x80U) != 0;
                if (select) {
                    sawSelect_.store(true);
                } else {
                    sawExecute_.store(true);
                }
                auto confirm = asdu;
                confirm[2] = 7;
                sendIFrame(confirm);
                if (!select) {
                    auto done = asdu;
                    done[2] = 10;
                    sendIFrame(done);
                }
            } else if (asdu[0] == 100) {
                sendSFrame(IecCodec::iec104SendSequence(frame) + 1);
                sendAll(clientFd_, buildSinglePointFrame(config_, serverSendSeq_++, IecCodec::iec104SendSequence(frame) + 1, 20, 1001, true));
            } else if (asdu[0] == 103) {
                sendSFrame(IecCodec::iec104SendSequence(frame) + 1);
                sawClockSync_.store(true);
                auto confirm = asdu;
                confirm[2] = 7;
                sendIFrame(confirm);
            }
        }
    }

    void sendIFrame(const std::vector<std::uint8_t>& asdu) {
        std::vector<std::uint8_t> frame;
        frame.push_back(0x68);
        frame.push_back(static_cast<std::uint8_t>(4 + asdu.size()));
        const auto sendSeq = static_cast<std::uint16_t>(serverSendSeq_++ << 1);
        frame.push_back(static_cast<std::uint8_t>(sendSeq & 0xFF));
        frame.push_back(static_cast<std::uint8_t>((sendSeq >> 8) & 0xFF));
        frame.push_back(0x02);
        frame.push_back(0x00);
        frame.insert(frame.end(), asdu.begin(), asdu.end());
        sendAll(clientFd_, frame);
    }

    void sendSFrame(std::uint16_t receiveSequence) {
        sendAll(clientFd_, IecCodec::buildIec104SFrame(receiveSequence));
    }

    IecProtocolConfig config_;
    int listenFd_ = -1;
    int clientFd_ = -1;
    int port_ = 0;
    std::uint16_t serverSendSeq_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> sawSelect_{false};
    std::atomic<bool> sawExecute_{false};
    std::atomic<bool> sawClockSync_{false};
    std::thread thread_;
};

}  // namespace

int main() {
    using namespace edge_gateway;

    IecProtocolConfig iec;
    iec.commonAddress = 1;
    iec.cotSize = 2;
    iec.caSize = 2;
    iec.ioaSize = 3;
    iec.backgroundReceive = true;
    iec.pollOnCollect = false;
    iec.idleReadTimeoutMs = 20;
    iec.pollTimeoutMs = 500;
    iec.t0Ms = 3000;
    iec.t1Ms = 3000;
    iec.t2Ms = 200;
    iec.t3Ms = 1000;
    iec.wAck = 1;

    TestIec104Server server(iec);
    server.start();

    TcpTransportConfig tcp;
    tcp.host = "127.0.0.1";
    tcp.port = server.port();
    tcp.timeoutMs = 200;

    {
        IecTcpClient client("iec104", tcp, iec);
        PointDefinition signal;
        signal.index = 1;
        signal.pointCode = "signal";
        signal.address = 1001;
        signal.read.enable = true;
        signal.read.dataType = "single_point";
        signal.read.iec.ioa = 1001;
        signal.read.iec.typeId = 1;
        signal.read.iec.commonAddress = 1;

        const auto values = client.poll();
        requireTrue(!values.empty(), "spontaneous value should be received");
        requireTrue(IecCodec::pointMatches(signal, values.front()), "spontaneous value should match point");
        requireNear(values.front().value, 1.0, "spontaneous value");

        PointDefinition command;
        command.index = 2;
        command.pointCode = "remote_close";
        command.address = 3001;
        command.write.enable = true;
        command.write.dataType = "single_command";
        command.write.allowedValues = {0.0, 1.0};
        command.write.iec.ioa = 3001;
        command.write.iec.typeId = 45;
        command.write.iec.commonAddress = 1;
        command.write.iec.selectBeforeExecute = true;
        command.write.iec.waitActivationTermination = true;
        command.write.iec.timeoutMs = 3000;

        const auto result = client.writeByPoint(command, 1.0, "CMD_TEST", "MACHINE", "METER", 1234);
        requireTrue(result.success, "IEC104 command should succeed: " + result.message);
        client.synchronizeClock(1234567890);
    }
    requireTrue(server.sawSelect(), "server should receive select command");
    requireTrue(server.sawExecute(), "server should receive execute command");
    requireTrue(server.sawClockSync(), "server should receive clock sync command");

    server.stop();
    std::cout << "iec104 client integration test passed" << std::endl;
    return 0;
}
