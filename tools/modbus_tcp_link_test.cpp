// driver-review: Modbus TCP keeps the connection when a reply is merely late. The late reply carries
// the id of the abandoned transaction and is dropped; only repeated silence (or a broken stream)
// rebuilds the connection, so one silent unit behind a serial gateway does not reset the others.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "edge_gateway/modbus_error.hpp"
#include "edge_gateway/modbus_tcp_client.hpp"

using namespace edge_gateway;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool readAll(int fd, std::uint8_t* data, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
        const auto rc = ::recv(fd, data + done, size - done, 0);
        if (rc <= 0) return false;
        done += static_cast<std::size_t>(rc);
    }
    return true;
}

struct Request { std::uint16_t tid; int unit; int start; int count; };

// Serves FC3 with register value == address; `delayFor(request number, unit)` delays each reply.
class Server {
public:
    std::function<int(int requestNumber, int unit)> delayFor = [](int, int) { return 0; };
    std::atomic<int> connections{0};
    std::uint16_t port = 0;

    Server() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int reuse = 1;
        setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind failed");
        require(::listen(listener_, 4) == 0, "listen failed");
        socklen_t length = sizeof(address);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port = ntohs(address.sin_port);
        thread_ = std::thread([this] { run(); });
    }
    ~Server() {
        stop_ = true;
        ::shutdown(listener_, SHUT_RDWR);
        ::close(listener_);
        if (thread_.joinable()) thread_.join();
    }

private:
    void run() {
        int requestNumber = 0;
        while (!stop_) {
            const int fd = ::accept(listener_, nullptr, nullptr);
            if (fd < 0) return;
            ++connections;
            std::vector<std::thread> replies;
            std::uint8_t header[7];
            while (!stop_ && readAll(fd, header, sizeof(header))) {
                const int length = (header[4] << 8) | header[5];
                std::vector<std::uint8_t> pdu(static_cast<std::size_t>(length - 1));
                if (!readAll(fd, pdu.data(), pdu.size())) break;
                Request request{static_cast<std::uint16_t>((header[0] << 8) | header[1]), header[6],
                                (pdu[1] << 8) | pdu[2], (pdu[3] << 8) | pdu[4]};
                const int delay = delayFor(++requestNumber, request.unit);
                if (delay < 0) continue;  // never answer
                replies.emplace_back([fd, request, delay] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                    std::vector<std::uint8_t> reply{
                        static_cast<std::uint8_t>(request.tid >> 8), static_cast<std::uint8_t>(request.tid & 0xFF),
                        0, 0, 0, static_cast<std::uint8_t>(3 + request.count * 2),
                        static_cast<std::uint8_t>(request.unit), 3, static_cast<std::uint8_t>(request.count * 2)};
                    for (int i = 0; i < request.count; ++i) {
                        reply.push_back(static_cast<std::uint8_t>((request.start + i) >> 8));
                        reply.push_back(static_cast<std::uint8_t>((request.start + i) & 0xFF));
                    }
                    (void)::send(fd, reply.data(), reply.size(), MSG_NOSIGNAL);
                });
            }
            for (auto& reply : replies) reply.join();
            ::close(fd);
        }
    }

    int listener_ = -1;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

TcpTransportConfig config(std::uint16_t port) {
    TcpTransportConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.timeoutMs = 150;
    cfg.connectTimeoutMs = 500;
    return cfg;
}

void verifyLateReplyIsDroppedWithoutReconnect() {
    Server server;
    // First request answers after 250 ms (> 150 ms timeout), the rest at once.
    server.delayFor = [](int number, int) { return number == 1 ? 250 : 0; };
    ModbusTcpClient client(config(server.port));
    try {
        (void)client.readHoldingRegisters(1, 0, 2);
        require(false, "the slow first request must time out");
    } catch (const ModbusError& error) {
        require(error.kind() == ModbusFailureKind::Timeout && error.requestSent(), "timeout with requestSent expected");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // the late reply is now in the socket
    const auto values = client.readHoldingRegisters(1, 40, 2);
    require(values.size() == 2 && values[0] == 40, "the late reply of the abandoned request was returned instead");
    require(server.connections == 1, "a late reply must not force a reconnect");
}

void verifySilentUnitDoesNotResetSharedConnection() {
    Server server;
    // Unit 9 behind the gateway never answers; unit 1 answers.
    server.delayFor = [](int, int unit) { return unit == 9 ? -1 : 0; };
    ModbusTcpClient client(config(server.port));
    for (int round = 0; round < 4; ++round) {
        try {
            (void)client.readHoldingRegisters(9, 0, 1);
        } catch (const ModbusError&) {
        }
        const auto values = client.readHoldingRegisters(1, 10, 1);
        require(values.front() == 10, "the answering unit must keep working");
    }
    require(server.connections == 1, "timeouts of one unit interleaved with answers must not reconnect");

    // Nothing answers at all: the connection is rebuilt after repeated silence.
    server.delayFor = [](int, int) { return -1; };
    for (int i = 0; i < 4; ++i) {
        try {
            (void)client.readHoldingRegisters(1, 0, 1);
        } catch (const ModbusError&) {
        }
    }
    require(server.connections == 2, "repeated silence must rebuild the connection once");
}

}  // namespace

int main() {
    try {
        verifyLateReplyIsDroppedWithoutReconnect();
        verifySilentUnitDoesNotResetSharedConnection();
    } catch (const std::exception& ex) {
        std::cerr << "modbus_tcp_link_test failed: " << ex.what() << std::endl;
        return 1;
    }
    std::cout << "modbus_tcp_link_test passed" << std::endl;
    return 0;
}
