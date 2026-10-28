// Built with -fsanitize=thread against src/modbus_northbound_server.cpp only; the point store read is
// stubbed so the test needs no shared memory. ThreadSanitizer fails the run (exit 66) on any race.
// Regression: stop() read and closed the listen socket while the accept thread was still assigning
// it (data race, and a stop() that could block forever in accept()).
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/modbus_northbound_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

namespace edge_gateway {
Optional<StoredPointValue> MemoryPointStore::getLatestByIndex(std::uint32_t index, std::int64_t nowMs) const {
    static std::atomic<unsigned> calls{0};
    const auto call = calls.fetch_add(1);
    StoredPointValue value;
    value.index = index;
    value.value = static_cast<double>(call % 1000);
    value.quality = call % 3 == 0 ? 0 : 1;  // exercise onInvalid=hold from several client threads
    value.ts = nowMs;
    value.expireAt = nowMs + 60000;
    return value;
}
}  // namespace edge_gateway

namespace {

void client(int port, int requests) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        return;
    }
    for (int i = 0; i < requests; ++i) {
        const std::uint8_t request[12] = {0, 1, 0, 0, 0, 6, 1, 3, 0, 100, 0, 3};
        if (send(fd, request, sizeof(request), MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof(request))) break;
        std::uint8_t response[32];
        if (recv(fd, response, sizeof(response), 0) <= 0) break;
    }
    close(fd);
}

}  // namespace

int main() {
    using namespace edge_gateway;
    // Only getLatestByIndex (stubbed above) is ever called on the store.
    alignas(MemoryPointStore) static unsigned char storage[sizeof(MemoryPointStore)];
    auto& store = *reinterpret_cast<MemoryPointStore*>(storage);
    DeviceConfig config;
    config.northboundServer.enabled = true;
    config.northboundServer.bindHost = "127.0.0.1";
    config.northboundServer.port = 0;
    PointDefinition point;
    point.index = 1;
    point.pointCode = "P";
    point.enabled = true;
    point.northbound.enabled = true;
    point.northbound.address = 100;
    point.northbound.dataType = "uint16";
    point.northbound.length = 1;
    point.northbound.byteOrder = "AB";
    point.northbound.onInvalid = "hold";
    PointDefinition wide = point;
    wide.index = 2;
    wide.pointCode = "W";
    wide.northbound.address = 101;
    wide.northbound.dataType = "float32";
    wide.northbound.length = 2;
    wide.northbound.byteOrder = "CDAB";
    config.points = {point, wide};

    for (int round = 0; round < 60; ++round) {
        std::atomic<int> events{0};
        ModbusNorthboundServer server(config, store,
            [&](const std::string&, const std::string&) { events.fetch_add(1); });
        server.start();
        if (round % 3 == 0) {
            server.stop();  // stop racing the listener start-up
            continue;
        }
        for (int i = 0; i < 250 && server.boundPort() == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::vector<std::thread> clients;
        for (int k = 0; k < 3; ++k) {
            clients.emplace_back(client, server.boundPort(), 30);
        }
        if (round % 2 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        (void)server.startError();
        server.stop();
        for (auto& thread : clients) {
            thread.join();
        }
    }
    std::cout << "modbus_northbound_tsan_test passed" << std::endl;
    return 0;
}
