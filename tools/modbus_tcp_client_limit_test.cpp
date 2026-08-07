#include <cstdint>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "edge_gateway/modbus_tcp_client.hpp"
#include "edge_gateway/modbus_error.hpp"
#include "edge_gateway/models.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void verifyReadCountLimitIsEnforcedBeforeConnect() {
    edge_gateway::TcpTransportConfig config;
    config.host = "127.0.0.1";
    config.port = 65000;
    config.timeoutMs = 50;

    edge_gateway::ModbusTcpClient client(config, 2);
    bool threw = false;
    try {
        (void)client.readHoldingRegisters(1, 0, 3);
    } catch (const std::invalid_argument& ex) {
        threw = std::string(ex.what()).find("maxRequestRegisters") != std::string::npos;
    }
    require(threw, "oversized TCP read should throw maxRequestRegisters before connecting");
}

void verifyBitReadCountLimitIsEnforcedBeforeConnect() {
    edge_gateway::TcpTransportConfig config;
    config.host = "127.0.0.1";
    config.port = 65000;
    config.timeoutMs = 50;

    edge_gateway::ModbusTcpClient client(config, 2);
    bool threw = false;
    try {
        (void)client.readCoils(1, 0, 3);
    } catch (const std::invalid_argument& ex) {
        threw = std::string(ex.what()).find("maxRequestRegisters") != std::string::npos;
    }
    require(threw, "oversized TCP bit read should throw maxRequestRegisters before connecting");
}

void verifyWriteCountLimitIsEnforcedBeforeConnect() {
    edge_gateway::TcpTransportConfig config;
    config.host = "127.0.0.1";
    config.port = 65000;
    config.timeoutMs = 50;

    edge_gateway::ModbusTcpClient client(config, 2);
    bool threw = false;
    try {
        client.writeMultipleRegisters(1, 0, {1, 2, 3});
    } catch (const std::invalid_argument& ex) {
        threw = std::string(ex.what()).find("maxRequestRegisters") != std::string::npos;
    }
    require(threw, "oversized TCP write should throw maxRequestRegisters before connecting");
}

void verifyPriorityWriteHooksAreBalanced() {
    edge_gateway::TcpTransportConfig config;
    config.host = "127.0.0.1";
    config.port = 65000;
    config.timeoutMs = 50;

    edge_gateway::ModbusTcpClient client(config, 2);
    client.beginPriorityWrite();
    client.endPriorityWrite();
    client.endPriorityWrite();
}

#ifndef _WIN32

int createLoopbackListener(std::uint16_t& port) {
    const auto listener = socket(AF_INET, SOCK_STREAM, 0);
    require(listener >= 0, "failed to create TCP test listener");
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    timeval acceptTimeout{};
    acceptTimeout.tv_sec = 2;
    setsockopt(listener, SOL_SOCKET, SO_RCVTIMEO, &acceptTimeout, sizeof(acceptTimeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "failed to bind TCP test listener");
    require(listen(listener, 2) == 0, "failed to listen in TCP test");
    socklen_t addressLength = sizeof(address);
    require(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressLength) == 0, "failed to read TCP test port");
    port = ntohs(address.sin_port);
    return listener;
}

std::vector<std::uint8_t> readExactFd(int fd, std::size_t count) {
    std::vector<std::uint8_t> bytes(count);
    std::size_t received = 0;
    while (received < count) {
        const auto rc = recv(fd, bytes.data() + received, count - received, 0);
        if (rc <= 0) {
            throw std::runtime_error("TCP test server recv failed");
        }
        received += static_cast<std::size_t>(rc);
    }
    return bytes;
}

void sendAllFd(int fd, const std::vector<std::uint8_t>& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto rc = send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (rc <= 0) {
            throw std::runtime_error("TCP test server send failed");
        }
        sent += static_cast<std::size_t>(rc);
    }
}

std::vector<std::uint8_t> readResponseFor(
    const std::vector<std::uint8_t>& request,
    std::uint16_t transactionId,
    std::uint16_t value
) {
    return {
        static_cast<std::uint8_t>((transactionId >> 8) & 0xFF),
        static_cast<std::uint8_t>(transactionId & 0xFF),
        0x00, 0x00,
        0x00, 0x05,
        request[6],
        0x03, 0x02,
        static_cast<std::uint8_t>((value >> 8) & 0xFF),
        static_cast<std::uint8_t>(value & 0xFF)
    };
}

void verifyTransactionMismatchDisconnectsAndRecovers() {
    std::uint16_t port = 0;
    const auto listener = createLoopbackListener(port);
    std::exception_ptr serverFailure;
    std::thread server([&] {
        try {
            const auto first = accept(listener, nullptr, nullptr);
            if (first < 0) {
                throw std::runtime_error("first TCP test accept failed");
            }
            const auto firstRequest = readExactFd(first, 12);
            const auto firstTransaction = static_cast<std::uint16_t>((firstRequest[0] << 8) | firstRequest[1]);
            sendAllFd(first, readResponseFor(firstRequest, static_cast<std::uint16_t>(firstTransaction + 1), 0x1111));
            close(first);

            const auto second = accept(listener, nullptr, nullptr);
            if (second < 0) {
                throw std::runtime_error("second TCP test accept failed");
            }
            const auto secondRequest = readExactFd(second, 12);
            const auto secondTransaction = static_cast<std::uint16_t>((secondRequest[0] << 8) | secondRequest[1]);
            sendAllFd(second, readResponseFor(secondRequest, secondTransaction, 0x1234));
            close(second);
        } catch (...) {
            serverFailure = std::current_exception();
        }
    });

    edge_gateway::TcpTransportConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.connectTimeoutMs = 500;
    config.timeoutMs = 500;
    edge_gateway::ModbusTcpClient client(config, 125);

    bool mismatchReported = false;
    try {
        (void)client.readHoldingRegisters(1, 0, 1);
    } catch (const std::runtime_error& ex) {
        mismatchReported = std::string(ex.what()).find("transaction id mismatch") != std::string::npos;
    }
    bool recovered = false;
    try {
        const auto values = client.readHoldingRegisters(1, 0, 1);
        recovered = values.size() == 1 && values.front() == 0x1234;
    } catch (...) {
        recovered = false;
    }

    server.join();
    close(listener);
    if (serverFailure) {
        std::rethrow_exception(serverFailure);
    }
    require(mismatchReported, "wrong transaction id should be reported");
    require(recovered, "client should reconnect after MBAP mismatch");
}

void verifyPeerResetDoesNotTerminateClientProcess() {
    std::uint16_t port = 0;
    const auto listener = createLoopbackListener(port);
    std::exception_ptr serverFailure;
    std::thread server([&] {
        try {
            const auto connection = accept(listener, nullptr, nullptr);
            if (connection < 0) {
                throw std::runtime_error("TCP reset test accept failed");
            }
            const auto request = readExactFd(connection, 12);
            const auto transaction = static_cast<std::uint16_t>((request[0] << 8) | request[1]);
            sendAllFd(connection, readResponseFor(request, transaction, 0x2222));
            linger resetOnClose{};
            resetOnClose.l_onoff = 1;
            resetOnClose.l_linger = 0;
            setsockopt(connection, SOL_SOCKET, SO_LINGER, &resetOnClose, sizeof(resetOnClose));
            close(connection);
        } catch (...) {
            serverFailure = std::current_exception();
        }
    });

    edge_gateway::TcpTransportConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.connectTimeoutMs = 500;
    config.timeoutMs = 500;
    edge_gateway::ModbusTcpClient client(config, 125);
    bool firstResponseValid = false;
    try {
        const auto first = client.readHoldingRegisters(1, 0, 1);
        firstResponseValid = first.size() == 1 && first.front() == 0x2222;
    } catch (...) {
        firstResponseValid = false;
    }
    server.join();
    if (serverFailure) {
        close(listener);
        std::rethrow_exception(serverFailure);
    }
    require(firstResponseValid, "reset test first response mismatch");

    bool resetReported = false;
    try {
        (void)client.readHoldingRegisters(1, 1, 1);
    } catch (const std::runtime_error&) {
        resetReported = true;
    }
    close(listener);
    require(resetReported, "peer reset should return an exception without terminating the process");
}

void verifyPartialResponseUsesOneTransactionDeadline() {
    std::uint16_t port = 0;
    const auto listener = createLoopbackListener(port);
    std::exception_ptr serverFailure;
    std::thread server([&] {
        try {
            const auto connection = accept(listener, nullptr, nullptr);
            if (connection < 0) {
                throw std::runtime_error("partial response test accept failed");
            }
            const auto request = readExactFd(connection, 12);
            const auto transaction = static_cast<std::uint16_t>((request[0] << 8) | request[1]);
            const auto response = readResponseFor(request, transaction, 0x3456);

            sendAllFd(connection, std::vector<std::uint8_t>(response.begin(), response.begin() + 8));
            std::this_thread::sleep_for(std::chrono::milliseconds(70));
            (void)send(connection, response.data() + 8, 1, MSG_NOSIGNAL);
            std::this_thread::sleep_for(std::chrono::milliseconds(70));
            (void)send(connection, response.data() + 9, response.size() - 9, MSG_NOSIGNAL);
            close(connection);
        } catch (...) {
            serverFailure = std::current_exception();
        }
    });

    edge_gateway::TcpTransportConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.connectTimeoutMs = 500;
    config.timeoutMs = 100;
    edge_gateway::ModbusTcpClient client(config, 125);

    const auto startedAt = std::chrono::steady_clock::now();
    bool deadlineReported = false;
    try {
        (void)client.readHoldingRegisters(1, 0, 1);
    } catch (const edge_gateway::ModbusError& ex) {
        deadlineReported = ex.kind() == edge_gateway::ModbusFailureKind::Timeout;
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt
    ).count();

    server.join();
    close(listener);
    if (serverFailure) {
        std::rethrow_exception(serverFailure);
    }
    require(deadlineReported, "partial TCP response should expire the transaction deadline");
    require(elapsedMs < 140, "partial TCP response must not reset the timeout after each recv");
}

#endif

}  // namespace

int main() {
    try {
        verifyReadCountLimitIsEnforcedBeforeConnect();
        verifyBitReadCountLimitIsEnforcedBeforeConnect();
        verifyWriteCountLimitIsEnforcedBeforeConnect();
        verifyPriorityWriteHooksAreBalanced();
#ifndef _WIN32
        verifyTransactionMismatchDisconnectsAndRecovers();
        verifyPeerResetDoesNotTerminateClientProcess();
        verifyPartialResponseUsesOneTransactionDeadline();
#endif
        std::cout << "modbus_tcp_client_limit_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "modbus_tcp_client_limit_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
