#include "edge_gateway/am5se_iec103_client.hpp"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

void requireTrue(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void closeSocket(SocketHandle socketHandle) {
#ifdef _WIN32
    closesocket(socketHandle);
#else
    close(socketHandle);
#endif
}

std::string temporaryDirectory(int port) {
    const char* base = std::getenv(
#ifdef _WIN32
        "TEMP"
#else
        "TMPDIR"
#endif
    );
    if (base == nullptr || *base == '\0') {
#ifdef _WIN32
        base = ".";
#else
        base = "/tmp";
#endif
    }
    std::string path(base);
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path.push_back('/');
    }
    path += "am5se-iec103-test-" + std::to_string(port);
#ifdef _WIN32
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0775);
#endif
    return path;
}

bool fileExists(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    return input.good();
}

void removeTestOutput(const std::string& directory) {
    std::remove((directory + "/FAN00015.CFG").c_str());
    std::remove((directory + "/FAN00015.DAT").c_str());
#ifdef _WIN32
    _rmdir(directory.c_str());
#else
    rmdir(directory.c_str());
#endif
}

void sendAll(SocketHandle socketHandle, const std::vector<std::uint8_t>& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
#ifdef _WIN32
        const auto result = send(socketHandle, reinterpret_cast<const char*>(bytes.data() + sent),
            static_cast<int>(bytes.size() - sent), 0);
#else
        const auto result = send(socketHandle, bytes.data() + sent, bytes.size() - sent, 0);
#endif
        if (result <= 0) {
            throw std::runtime_error("mock send failed");
        }
        sent += static_cast<std::size_t>(result);
    }
}

std::vector<std::uint8_t> receiveExact(SocketHandle socketHandle, std::size_t size) {
    std::vector<std::uint8_t> result(size);
    std::size_t received = 0;
    while (received < size) {
#ifdef _WIN32
        const auto count = recv(socketHandle, reinterpret_cast<char*>(result.data() + received),
            static_cast<int>(size - received), 0);
#else
        const auto count = recv(socketHandle, result.data() + received, size - received, 0);
#endif
        if (count <= 0) {
            throw std::runtime_error("mock receive failed");
        }
        received += static_cast<std::size_t>(count);
    }
    return result;
}

std::vector<std::uint8_t> receiveFrame(SocketHandle socketHandle) {
    auto first = receiveExact(socketHandle, 1);
    if (first[0] == 0x10) {
        auto rest = receiveExact(socketHandle, 4);
        first.insert(first.end(), rest.begin(), rest.end());
        return first;
    }
    if (first[0] != 0x68) {
        throw std::runtime_error("mock received invalid frame start");
    }
    auto header = receiveExact(socketHandle, 3);
    first.insert(first.end(), header.begin(), header.end());
    auto rest = receiveExact(socketHandle, static_cast<std::size_t>(first[1]) + 2U);
    first.insert(first.end(), rest.begin(), rest.end());
    return first;
}

std::vector<std::uint8_t> variableFrame(const std::vector<std::uint8_t>& userData) {
    std::vector<std::uint8_t> frame = {
        0x68,
        static_cast<std::uint8_t>(userData.size()),
        static_cast<std::uint8_t>(userData.size()),
        0x68
    };
    frame.insert(frame.end(), userData.begin(), userData.end());
    std::uint32_t checksum = 0;
    for (const auto byte : userData) {
        checksum += byte;
    }
    frame.push_back(static_cast<std::uint8_t>(checksum & 0xFF));
    frame.push_back(0x16);
    return frame;
}

int availablePort() {
    const auto socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == kInvalidSocket) {
        throw std::runtime_error("cannot allocate test socket");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(socketHandle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        closeSocket(socketHandle);
        throw std::runtime_error("cannot bind test socket");
    }
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    getsockname(socketHandle, reinterpret_cast<sockaddr*>(&address), &length);
    const auto port = ntohs(address.sin_port);
    closeSocket(socketHandle);
    return port;
}

void mockDevice(int port) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(static_cast<std::uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &destination.sin_addr);
    if (connect(socketHandle, reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) != 0) {
        closeSocket(socketHandle);
        throw std::runtime_error("mock reverse TCP connect failed");
    }

    const std::vector<std::uint8_t> acknowledge = {0x10, 0x20, 0x01, 0x21, 0x16};
    receiveFrame(socketHandle);  // Reset communication.
    sendAll(socketHandle, acknowledge);
    receiveFrame(socketHandle);  // General interrogation.
    sendAll(socketHandle, acknowledge);
    receiveFrame(socketHandle);  // Class 1.
    sendAll(socketHandle, variableFrame({
        0x08, 0x01, 0x2C, 0x01, 0x09, 0x01, 0x01, 0x64,
        0x01, 0x00, 0x00, 0x00, 0x00, 0xE9
    }));

    receiveFrame(socketHandle);  // Directory ASDU24.
    sendAll(socketHandle, acknowledge);
    receiveFrame(socketHandle);  // Class 1 directory response.
    sendAll(socketHandle, variableFrame({
        0x08, 0x01, 0x17, 0x01, 0x1F, 0x01, 0xFF, 0x00,
        0x0F, 0x00, 0x01, 0x00, 0x00, 0x01, 0x02, 0x01, 0x01, 0x1A
    }));

    receiveFrame(socketHandle);  // CFG ASDU68.
    sendAll(socketHandle, acknowledge);
    receiveFrame(socketHandle);  // CFG class 1 response.
    sendAll(socketHandle, variableFrame({
        0x08, 0x01, 0x50, 0x81, 0x14, 0x01, 0x01, 0x51,
        0x00, 0x80, 'C', 'F', 'G'
    }));

    receiveFrame(socketHandle);  // DAT ASDU68.
    sendAll(socketHandle, acknowledge);
    receiveFrame(socketHandle);  // DAT class 1 response.
    sendAll(socketHandle, variableFrame({
        0x08, 0x01, 0x50, 0x81, 0x14, 0x01, 0x01, 0x52,
        0x00, 0x80, 0x01, 0x02, 0x03
    }));
    closeSocket(socketHandle);
}

}  // namespace

int main() {
    using namespace edge_gateway;

#ifdef _WIN32
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    const int port = availablePort();
    std::exception_ptr mockError;
    std::thread device([&] {
        try {
            mockDevice(port);
        } catch (...) {
            mockError = std::current_exception();
        }
    });

    const auto output = temporaryDirectory(port);
    removeTestOutput(output);
    try {
        IecProtocolConfig config;
        config.transportMode = "am5se_passive_tcp";
        config.listenAddress = "127.0.0.1";
        config.listenPort = port;
        config.udpBindAddress = "127.0.0.1";
        config.udpBroadcastAddress = "127.0.0.1";
        config.udpPort = port + 1;
        config.linkAddress = 1;
        config.commonAddress = 1;
        config.linkAddressSize = 1;
        config.deviceFunctionType = 1;
        config.interrogationCot = 9;
        config.pollTimeoutMs = 1000;
        config.t0Ms = 3000;
        config.maxPollFrames = 8;
        config.recordingMaxFileBytes = 1024;

        Am5seIec103Client client(config);
        const auto values = client.poll();
        requireTrue(values.size() == 16, "passive TCP general interrogation values");
        requireTrue(values.front().informationNumber == 100 && values.front().value == 1.0,
            "passive TCP ASDU44 value");

        const auto records = client.listDisturbanceRecords(1000);
        requireTrue(records.size() == 1 && records.front().fan == 15, "passive TCP recording directory");

        const auto files = client.pullComtradeRecording(15, output, 1000);
        requireTrue(files.cfgBytes == 3 && files.datBytes == 3, "passive TCP COMTRADE sizes");
        requireTrue(fileExists(files.cfgPath) && fileExists(files.datPath),
            "passive TCP COMTRADE files");
    } catch (...) {
        if (device.joinable()) {
            device.join();
        }
        removeTestOutput(output);
        throw;
    }
    device.join();
    if (mockError) {
        std::rethrow_exception(mockError);
    }
    removeTestOutput(output);
    std::cout << "AM5SE IEC103 passive TCP integration test passed" << std::endl;
    return EXIT_SUCCESS;
}
