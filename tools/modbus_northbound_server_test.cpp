#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/modbus_northbound_server.hpp"
#include "edge_gateway/modbus_tcp_client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using edge_gateway::DeviceConfig;
using edge_gateway::MemoryPointStore;
using edge_gateway::ModbusNorthboundServer;
using edge_gateway::PointDefinition;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

PointDefinition mappedPoint(std::uint32_t index, const std::string& code, int function, int address,
                            const std::string& dataType, const std::string& byteOrder, int length) {
    PointDefinition point;
    point.index = index;
    point.pointCode = code;
    point.enabled = true;
    point.read.enable = true;
    point.read.length = length;
    point.read.dataType = dataType;
    point.read.byteOrder = byteOrder;
    point.read.cachePolicy.ttlMs = 600000;
    point.northbound.enabled = true;
    point.northbound.unitId = 1;
    point.northbound.readFunction = function;
    point.northbound.address = address;
    point.northbound.length = length;
    point.northbound.dataType = dataType;
    point.northbound.byteOrder = byteOrder;
    return point;
}

PointDefinition buildPoint() {
    auto point = mappedPoint(720001, "NB_TEST_VOLTAGE", 3, 100, "uint16", "AB", 1);
    point.name = "Northbound Test Voltage";
    point.read.scale = 0.1;
    point.northbound.scale = 0.1;
    return point;
}

DeviceConfig buildConfig(const std::string& storeName) {
    DeviceConfig config;
    config.machineCode = "GW_TEST";
    config.meterCode = "MTR_TEST";
    config.deviceName = "Northbound Test Device";
    config.memoryStore.sharedMemoryName = storeName;
    config.memoryStore.maxLatestPoints = 32;
    config.northboundServer.enabled = true;
    config.northboundServer.mode = "mapped";
    config.northboundServer.protocol = "modbus_tcp";
    config.northboundServer.bindHost = "127.0.0.1";
    config.northboundServer.port = 0;
    config.northboundServer.requestTimeoutMs = 500;
    config.northboundServer.maxClients = 4;
    config.points = {buildPoint()};
    return config;
}

// Owns a fresh shared-memory store with every point of the config registered.
struct StoreFixture {
    explicit StoreFixture(const DeviceConfig& deviceConfig) : config(deviceConfig) {
        MemoryPointStore::cleanupOrphanedSegment(config.memoryStore.sharedMemoryName);
        store.reset(new MemoryPointStore(config.memoryStore));
        for (const auto& point : config.points) {
            store->registerPoint(config.machineCode, config.meterCode, point);
        }
    }
    ~StoreFixture() {
        store.reset();
        MemoryPointStore::cleanupOrphanedSegment(config.memoryStore.sharedMemoryName);
    }
    void put(std::uint32_t index, double value, int quality = 1, bool stale = false) {
        edge_gateway::PointValue item;
        item.index = index;
        item.machineCode = config.machineCode;
        item.meterCode = config.meterCode;
        item.value = value;
        item.quality = quality;
        item.ts = nowMs();
        // The store derives "stale" from expireAt when the value is read.
        item.expireAt = stale ? item.ts - 1 : item.ts + 600000;
        for (const auto& point : config.points) {
            if (point.index == index) item.pointCode = point.pointCode;
        }
        store->putLatest(item);
    }
    DeviceConfig config;
    std::unique_ptr<MemoryPointStore> store;
};

void waitBound(const ModbusNorthboundServer& server) {
    for (int i = 0; i < 250 && server.boundPort() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    require(server.boundPort() > 0, "server should bind an ephemeral port");
}

int connectTo(int family, const char* host, int port) {
    const int fd = socket(family, SOCK_STREAM, 0);
    require(fd >= 0, "client socket failed");
    int rc = -1;
    if (family == AF_INET) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        inet_pton(AF_INET, host, &address.sin_addr);
        rc = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    } else {
        sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(static_cast<std::uint16_t>(port));
        inet_pton(AF_INET6, host, &address.sin6_addr);
        rc = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    }
    require(rc == 0, "client connect failed");
    timeval timeout{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    return fd;
}

// Sends one read request on fd and returns the response PDU (function byte onwards), empty when
// the server closed the connection.
std::vector<std::uint8_t> exchange(int fd, std::uint8_t unit, std::uint8_t function, int start, int count) {
    static std::uint16_t transaction = 0;
    ++transaction;
    const std::uint8_t request[12] = {
        static_cast<std::uint8_t>(transaction >> 8), static_cast<std::uint8_t>(transaction), 0, 0, 0, 6, unit,
        function, static_cast<std::uint8_t>(start >> 8), static_cast<std::uint8_t>(start),
        static_cast<std::uint8_t>(count >> 8), static_cast<std::uint8_t>(count)};
    if (send(fd, request, sizeof(request), MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof(request))) return {};
    std::vector<std::uint8_t> header(7);
    std::size_t got = 0;
    while (got < header.size()) {
        const auto n = recv(fd, header.data() + got, header.size() - got, 0);
        if (n <= 0) return {};
        got += static_cast<std::size_t>(n);
    }
    require(header[0] == request[0] && header[1] == request[1], "transaction id must be echoed");
    const auto length = static_cast<std::size_t>((header[4] << 8) | header[5]);
    std::vector<std::uint8_t> pdu(length - 1);
    got = 0;
    while (got < pdu.size()) {
        const auto n = recv(fd, pdu.data() + got, pdu.size() - got, 0);
        if (n <= 0) return {};
        got += static_cast<std::size_t>(n);
    }
    return pdu;
}

std::vector<std::uint8_t> request(int port, std::uint8_t unit, std::uint8_t function, int start, int count) {
    const int fd = connectTo(AF_INET, "127.0.0.1", port);
    auto pdu = exchange(fd, unit, function, start, count);
    close(fd);
    return pdu;
}

std::uint16_t word(const std::vector<std::uint8_t>& pdu, std::size_t index) {
    return static_cast<std::uint16_t>((pdu[2 + index * 2] << 8) | pdu[3 + index * 2]);
}

bool isException(const std::vector<std::uint8_t>& pdu, std::uint8_t code) {
    return pdu.size() == 2 && (pdu[0] & 0x80) != 0 && pdu[1] == code;
}

void verifyMappedHoldingRegisterRead() {
    StoreFixture fixture(buildConfig("gateway_modbus_northbound_server_test"));
    fixture.put(720001, 23.5);
    ModbusNorthboundServer server(fixture.config, *fixture.store);
    server.start();
    waitBound(server);

    edge_gateway::TcpTransportConfig tcp;
    tcp.host = "127.0.0.1";
    tcp.port = server.boundPort();
    tcp.timeoutMs = 1000;
    edge_gateway::ModbusTcpClient client(tcp, 125);
    const auto registers = client.readHoldingRegisters(1, 100, 1);
    require(registers.size() == 1, "mapped read should return one register");
    require(registers.front() == 235, "mapped register should use northbound scale");
    server.stop();
}

void verifyUnrestrictedWildcardBindIsRefused() {
    auto config = buildConfig("gateway_modbus_northbound_server_refuse_test");
    config.northboundServer.bindHost = "0.0.0.0";
    config.northboundServer.allowedClientCidrs.clear();
    config.northboundServer.allowUnrestrictedClients = false;
    StoreFixture fixture(config);
    std::vector<std::string> events;
    ModbusNorthboundServer server(config, *fixture.store,
        [&](const std::string& event, const std::string&) { events.push_back(event); });
    server.start();
    require(!server.isRunning(), "wildcard bind without allowedClientCidrs must be refused by default");
    require(server.startError().find("allowUnrestrictedClients") != std::string::npos,
        "northbound refusal must report the config remedy");
    require(events.size() == 1 && events.front() == "northbound-refused", "refusal must be reported as a status event");
    server.stop();
}

// Regression: each register used to read the point store separately, so a 32-bit value updated by
// the collector between two registers was served half old / half new (65535 -> 65536 read as 131071).
void verifyMultiRegisterReadIsOneSnapshot() {
    auto config = buildConfig("gateway_modbus_northbound_atomic_test");
    config.points = {mappedPoint(900001, "E_CNT", 3, 100, "int32", "ABCD", 2)};
    StoreFixture fixture(config);
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        bool flip = false;
        while (!stop.load()) {
            fixture.put(900001, flip ? 65536.0 : 65535.0);
            flip = !flip;
        }
    });
    ModbusNorthboundServer server(config, *fixture.store);
    server.start();
    waitBound(server);
    const int fd = connectTo(AF_INET, "127.0.0.1", server.boundPort());
    int torn = 0;
    int ok = 0;
    for (int i = 0; i < 20000; ++i) {
        const auto pdu = exchange(fd, 1, 3, 100, 2);
        require(pdu.size() == 6 && pdu[0] == 3, "int32 read must succeed");
        const auto value = static_cast<std::int32_t>((static_cast<std::uint32_t>(word(pdu, 0)) << 16) | word(pdu, 1));
        if (value == 65535 || value == 65536) ++ok; else ++torn;
    }
    close(fd);
    stop.store(true);
    writer.join();
    server.stop();
    std::cout << "atomic int32 reads ok=" << ok << " torn=" << torn << std::endl;
    require(torn == 0, "multi-register values must never be torn across collector updates");
}

// Regression: one stale or unencodable point used to fail a whole 125-register block with 04.
void verifyInvalidValuesFollowOnInvalid() {
    auto config = buildConfig("gateway_modbus_northbound_invalid_test");
    auto good = mappedPoint(1, "GOOD", 3, 10, "uint16", "AB", 1);
    auto stale = mappedPoint(2, "STALE_U16", 3, 11, "uint16", "AB", 1);
    auto negative = mappedPoint(3, "NEG_U16", 3, 12, "uint16", "AB", 1);
    auto badFloat = mappedPoint(4, "BAD_F32", 3, 13, "float32", "CDAB", 2);
    auto missingI32 = mappedPoint(5, "MISSING_I32", 3, 15, "int32", "ABCD", 2);
    auto held = mappedPoint(6, "HELD", 3, 17, "int16", "AB", 1);
    held.northbound.onInvalid = "hold";
    auto custom = mappedPoint(7, "CUSTOM", 3, 18, "uint16", "AB", 1);
    custom.northbound.sentinel = 9999.0;
    auto coil = mappedPoint(8, "COIL", 1, 10, "bool", "AB", 1);
    auto coilBad = mappedPoint(9, "COIL_BAD", 1, 11, "bool", "AB", 1);
    config.points = {good, stale, negative, badFloat, missingI32, held, custom, coil, coilBad};
    StoreFixture fixture(config);
    fixture.put(1, 42);
    fixture.put(2, 7, 1, true);
    fixture.put(3, -5);
    fixture.put(4, 1.0, 0);
    fixture.put(6, -12);
    fixture.put(7, 70000);
    fixture.put(8, 1);
    fixture.put(9, 1, 0);
    ModbusNorthboundServer server(config, *fixture.store);
    server.start();
    waitBound(server);

    auto pdu = request(server.boundPort(), 1, 3, 10, 9);
    require(pdu.size() == 2 + 18 && pdu[0] == 3, "a block with invalid values must still be answered");
    require(word(pdu, 0) == 42, "valid value unaffected by its neighbours");
    require(word(pdu, 1) == 0xFFFF, "stale uint16 reads the uint16 sentinel");
    require(word(pdu, 2) == 0xFFFF, "unencodable negative uint16 reads the sentinel, never a wrapped value");
    require(word(pdu, 3) == 0x0000 && word(pdu, 4) == 0x7FC0, "bad-quality float32 reads NaN in the mapping byte order");
    require(word(pdu, 5) == 0x8000 && word(pdu, 6) == 0x0000, "missing int32 reads INT32_MIN");
    require(word(pdu, 7) == static_cast<std::uint16_t>(-12), "hold serves the valid value");
    require(word(pdu, 8) == 9999, "configured sentinel replaces an out-of-range value");

    fixture.put(6, 1, 0);
    pdu = request(server.boundPort(), 1, 3, 17, 1);
    require(pdu.size() == 4 && word(pdu, 0) == static_cast<std::uint16_t>(-12), "hold keeps the last valid value");

    pdu = request(server.boundPort(), 1, 1, 10, 2);
    require(pdu.size() == 3 && pdu[2] == 0x01, "bad-quality coil reads 0 while its neighbour reads 1");
    server.stop();
}

void verifyRequestShapeRules() {
    auto config = buildConfig("gateway_modbus_northbound_shape_test");
    config.points = {
        mappedPoint(1, "U16", 3, 199, "uint16", "AB", 1),
        mappedPoint(2, "I32", 3, 200, "int32", "ABCD", 2),
        mappedPoint(3, "AFTER", 3, 202, "uint16", "AB", 1),
    };
    StoreFixture fixture(config);
    fixture.put(1, 5);
    fixture.put(2, 70000);
    fixture.put(3, 6);
    ModbusNorthboundServer server(config, *fixture.store);
    server.start();
    waitBound(server);
    const auto port = server.boundPort();

    require(isException(request(port, 1, 3, 201, 1), 0x02), "reading the low half of an int32 alone is illegal (02)");
    require(isException(request(port, 1, 3, 199, 2), 0x02), "a read ending inside an int32 is illegal (02)");
    auto pdu = request(port, 1, 3, 199, 4);
    require(pdu.size() == 10 && word(pdu, 0) == 5 && word(pdu, 1) == 1 && word(pdu, 2) == 4464 && word(pdu, 3) == 6,
        "whole values in one block read normally");
    require(isException(request(port, 1, 3, 199, 5), 0x02), "an unmapped address in the block is illegal (02)");
    require(isException(request(port, 1, 3, 199, 0), 0x03), "quantity 0 is illegal (03)");
    require(isException(request(port, 1, 6, 199, 1), 0x01), "writes are refused while writesEnabled=false");

    pdu = request(port, 255, 3, 199, 1);
    require(pdu.size() == 4 && word(pdu, 0) == 5, "unit 255 addresses the only configured unit");
    pdu = request(port, 0, 3, 199, 1);
    require(pdu.size() == 4 && word(pdu, 0) == 5, "unit 0 addresses the only configured unit");
    require(isException(request(port, 7, 3, 199, 1), 0x02), "an unknown unit is still refused");
    server.stop();

    // With two units configured, 0/255 are ambiguous and must be mapped explicitly.
    config.points[2].northbound.unitId = 2;
    config.points[2].northbound.address = 199;
    config.memoryStore.sharedMemoryName += "_two_units";
    StoreFixture twoUnits(config);
    twoUnits.put(1, 5);
    twoUnits.put(3, 6);
    ModbusNorthboundServer multi(config, *twoUnits.store);
    multi.start();
    waitBound(multi);
    require(isException(request(multi.boundPort(), 255, 3, 199, 1), 0x02), "unit 255 is ambiguous with two units");
    pdu = request(multi.boundPort(), 2, 3, 199, 1);
    require(pdu.size() == 4 && word(pdu, 0) == 6, "explicit unit 2 answers");
    multi.stop();
}

// Regression: on a dual-stack "::" listener IPv4 clients arrive as ::ffff:a.b.c.d and were refused
// by every IPv4 allowlist entry.
void verifyDualStackAllowlist() {
    edge_gateway::NorthboundServerConfig allowlist;
    allowlist.allowedClientCidrs = {"192.168.1.0/24", "10.0.0.7", "fd00::5"};
    require(edge_gateway::northboundClientAllowed(allowlist, "192.168.1.20"), "IPv4 client in CIDR is admitted");
    require(edge_gateway::northboundClientAllowed(allowlist, "::ffff:192.168.1.20"), "IPv4-mapped client matches IPv4 CIDR");
    require(edge_gateway::northboundClientAllowed(allowlist, "::ffff:10.0.0.7"), "IPv4-mapped client matches IPv4 address");
    require(edge_gateway::northboundClientAllowed(allowlist, "fd00::5"), "IPv6 address entry is matched exactly");
    require(!edge_gateway::northboundClientAllowed(allowlist, "::ffff:192.168.2.20"), "IPv4-mapped client outside CIDR is refused");
    require(!edge_gateway::northboundClientAllowed(allowlist, "::1"), "IPv6 client outside the list is refused");
    allowlist.allowedClientCidrs.clear();
    require(edge_gateway::northboundClientAllowed(allowlist, "203.0.113.9"), "empty list admits every client");

    const int probe = socket(AF_INET6, SOCK_STREAM, 0);
    if (probe < 0) {
        std::cout << "dual-stack socket check skipped: no IPv6 in this environment" << std::endl;
        return;
    }
    close(probe);
    auto config = buildConfig("gateway_modbus_northbound_dualstack_test");
    config.northboundServer.bindHost = "::";
    config.northboundServer.allowedClientCidrs = {"127.0.0.0/8"};
    StoreFixture fixture(config);
    fixture.put(720001, 23.5);
    ModbusNorthboundServer server(config, *fixture.store);
    server.start();
    waitBound(server);
    auto pdu = request(server.boundPort(), 1, 3, 100, 1);
    require(pdu.size() == 4 && word(pdu, 0) == 235, "IPv4 client of a :: listener must match an IPv4 CIDR");
    const int fd = connectTo(AF_INET6, "::1", server.boundPort());
    pdu = exchange(fd, 1, 3, 100, 1);
    close(fd);
    require(pdu.empty(), "::1 is not in the IPv4 allowlist and must be disconnected");
    server.stop();
}

// Regression: a bad northbound definition used to throw out of the daemon (crashing collection).
void verifyConfigProblemsOnlyDisableNorthbound() {
    auto config = buildConfig("gateway_modbus_northbound_problem_test");
    config.northboundServer.protocol = "modbus_rtu";
    auto noAddress = mappedPoint(10, "NO_ADDR", 3, -1, "uint16", "AB", 1);
    auto shortOrder = mappedPoint(11, "F32_AB", 3, 300, "float32", "AB", 2);
    auto repeated = mappedPoint(12, "I32_AABB", 3, 310, "int32", "AABB", 2);
    auto longU16 = mappedPoint(13, "U16_LEN2", 3, 320, "uint16", "AB", 2);
    auto overlap = mappedPoint(14, "OVERLAP", 3, 301, "uint16", "AB", 1);
    auto badPolicy = mappedPoint(15, "POLICY", 3, 330, "uint16", "AB", 1);
    badPolicy.northbound.onInvalid = "exception";
    auto badSentinel = mappedPoint(16, "SENTINEL", 3, 331, "uint16", "AB", 1);
    badSentinel.northbound.sentinel = -1.0;
    config.points = {buildPoint(), noAddress, shortOrder, repeated, longU16, overlap, badPolicy, badSentinel};
    const auto problems = edge_gateway::northboundConfigProblems(config);
    const auto mentions = [&](const std::string& needle) {
        for (const auto& problem : problems) {
            if (problem.find(needle) != std::string::npos) return true;
        }
        return false;
    };
    require(mentions("protocol \"modbus_rtu\""), "unsupported protocol must be reported");
    require(mentions("NO_ADDR") && mentions("address must be configured"), "missing address must be reported");
    require(mentions("F32_AB") && mentions("has 2 letters"), "byteOrder width must be checked");
    require(mentions("I32_AABB") && mentions("repeats a letter"), "repeated byteOrder letters must be reported");
    require(mentions("U16_LEN2") && mentions("length 2"), "length must match dataType");
    require(mentions("POLICY") && mentions("onInvalid"), "unknown onInvalid must be reported");
    require(mentions("SENTINEL") && mentions("sentinel"), "unrepresentable sentinel must be reported");

    StoreFixture fixture(config);
    std::vector<std::string> events;
    // Must not throw: the collector keeps running, only the northbound server is refused.
    ModbusNorthboundServer server(config, *fixture.store,
        [&](const std::string& event, const std::string&) { events.push_back(event); });
    server.start();
    require(!server.isRunning(), "a broken northbound config must not start");
    require(server.startError().find("NO_ADDR") != std::string::npos, "refusal must name the broken point");
    require(events.size() == 1 && events.front() == "northbound-refused", "refusal must be reported as an event");

    auto overlapOnly = buildConfig("gateway_modbus_northbound_problem_test");
    overlapOnly.points = {mappedPoint(1, "I32", 3, 300, "int32", "ABCD", 2), mappedPoint(2, "U16", 3, 301, "uint16", "AB", 1)};
    require(edge_gateway::northboundConfigProblems(overlapOnly).size() == 1, "an overlap inside an int32 must be reported");
    overlapOnly.northboundServer.enabled = false;
    require(edge_gateway::northboundConfigProblems(overlapOnly).empty(), "a disabled northbound server is not validated");

    // Loading a device config with a broken northbound definition succeeds (warning only).
    const std::string text = R"({
        "machineCode":"GW_TEST","meterCode":"M1","protocol":{"type":"modbus_rtu","slave":1},
        "northboundServer":{"enabled":true,"protocol":"modbus_rtu","bindHost":"127.0.0.1","port":1502},
        "points":[{"index":1,"pointCode":"P1","read":{"enable":true,"function":3,"address":0,"length":1,"dataType":"uint16"},
                   "northbound":{"enabled":true,"address":-1}}],"meters":[]})";
    const auto loaded = edge_gateway::ConfigLoader::loadFromText(text);
    require(loaded.northboundServer.enabled && !edge_gateway::northboundConfigProblems(loaded).empty(),
        "a broken northbound definition must load (collection keeps running) and stay reported");
}

// Regression: a failed bind only printed to stderr and never retried.
void verifyBindFailureIsReportedAndRetried() {
    const int blocker = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(blocker, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(blocker, 1) == 0,
        "blocker socket failed");
    socklen_t length = sizeof(address);
    getsockname(blocker, reinterpret_cast<sockaddr*>(&address), &length);
    const int port = ntohs(address.sin_port);

    auto config = buildConfig("gateway_modbus_northbound_bind_test");
    config.northboundServer.port = port;
    config.northboundServer.bindRetryIntervalMs = 200;
    StoreFixture fixture(config);
    fixture.put(720001, 23.5);
    std::mutex mutex;
    std::vector<std::string> events;
    ModbusNorthboundServer server(config, *fixture.store,
        [&](const std::string& event, const std::string&) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(event);
        });
    server.start();
    for (int i = 0; i < 100 && server.startError().empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    require(server.isRunning() && server.startError().find("retrying") != std::string::npos,
        "bind failure must be visible and retried");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    {
        std::lock_guard<std::mutex> lock(mutex);
        require(events.size() == 1 && events.front() == "northbound-bind-failed", "bind failure must be reported once");
    }
    close(blocker);
    waitBound(server);
    const auto pdu = request(port, 1, 3, 100, 1);
    require(pdu.size() == 4 && word(pdu, 0) == 235, "server must serve after the port frees up");
    require(server.startError().empty(), "start error clears once listening");
    {
        std::lock_guard<std::mutex> lock(mutex);
        require(events.size() == 2 && events.back() == "northbound-listening", "recovery must be reported");
    }
    server.stop();
}

}  // namespace

int main() {
    try {
        verifyUnrestrictedWildcardBindIsRefused();
        verifyMappedHoldingRegisterRead();
        verifyMultiRegisterReadIsOneSnapshot();
        verifyInvalidValuesFollowOnInvalid();
        verifyRequestShapeRules();
        verifyDualStackAllowlist();
        verifyConfigProblemsOnlyDisableNorthbound();
        verifyBindFailureIsReportedAndRetried();
        std::cout << "modbus_northbound_server_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "modbus_northbound_server_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
