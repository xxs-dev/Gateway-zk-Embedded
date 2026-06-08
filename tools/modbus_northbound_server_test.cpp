#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/modbus_northbound_server.hpp"
#include "edge_gateway/modbus_tcp_client.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

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

edge_gateway::PointDefinition buildPoint() {
    edge_gateway::PointDefinition point;
    point.index = 720001;
    point.pointCode = "NB_TEST_VOLTAGE";
    point.name = "Northbound Test Voltage";
    point.enabled = true;
    point.read.enable = true;
    point.read.length = 1;
    point.read.dataType = "uint16";
    point.read.scale = 0.1;
    point.read.byteOrder = "AB";
    point.read.cachePolicy.ttlMs = 600000;
    point.northbound.enabled = true;
    point.northbound.unitId = 1;
    point.northbound.readFunction = 3;
    point.northbound.address = 100;
    point.northbound.length = 1;
    point.northbound.dataType = "uint16";
    point.northbound.scale = 0.1;
    point.northbound.byteOrder = "AB";
    return point;
}

edge_gateway::DeviceConfig buildConfig(const std::string& storeName) {
    edge_gateway::DeviceConfig config;
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
    config.northboundServer.maxClients = 2;
    config.points = {buildPoint()};
    return config;
}

void verifyMappedHoldingRegisterRead() {
    const std::string storeName = "gateway_modbus_northbound_server_test";
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
    const auto config = buildConfig(storeName);
    edge_gateway::MemoryPointStore store(config.memoryStore);
    store.registerPoint(config.machineCode, config.meterCode, config.points.front());

    edge_gateway::PointValue value;
    value.index = config.points.front().index;
    value.machineCode = config.machineCode;
    value.meterCode = config.meterCode;
    value.pointCode = config.points.front().pointCode;
    value.value = 23.5;
    value.quality = 1;
    value.ts = nowMs();
    value.expireAt = value.ts + 600000;
    store.putLatest(value);

    edge_gateway::ModbusNorthboundServer server(config, store);
    server.start();
    for (int i = 0; i < 50 && server.boundPort() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    require(server.boundPort() > 0, "server should bind an ephemeral port");

    edge_gateway::TcpTransportConfig tcp;
    tcp.host = "127.0.0.1";
    tcp.port = server.boundPort();
    tcp.timeoutMs = 1000;
    edge_gateway::ModbusTcpClient client(tcp, 125);
    const auto registers = client.readHoldingRegisters(1, 100, 1);
    require(registers.size() == 1, "mapped read should return one register");
    require(registers.front() == 235, "mapped register should use northbound scale");
    server.stop();
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
}

}  // namespace

int main() {
    try {
        verifyMappedHoldingRegisterRead();
        std::cout << "modbus_northbound_server_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "modbus_northbound_server_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
