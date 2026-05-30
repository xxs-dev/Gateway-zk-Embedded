#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "edge_gateway/modbus_tcp_client.hpp"
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

}  // namespace

int main() {
    try {
        verifyReadCountLimitIsEnforcedBeforeConnect();
        verifyBitReadCountLimitIsEnforcedBeforeConnect();
        verifyWriteCountLimitIsEnforcedBeforeConnect();
        verifyPriorityWriteHooksAreBalanced();
        std::cout << "modbus_tcp_client_limit_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "modbus_tcp_client_limit_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
