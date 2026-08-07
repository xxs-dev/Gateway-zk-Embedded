#include "gateway_test_lab/serial_simulator.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>

#include "edge_gateway/dlt645_client.hpp"
#include "edge_gateway/dlt645_codec.hpp"
#include "edge_gateway/modbus_rtu_client.hpp"
#include "edge_gateway/posix_serial_port.hpp"
#endif

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

#ifndef _WIN32
std::string statePath(const std::string& suffix) {
    return "/tmp/gateway-test-lab-serial-" + std::to_string(static_cast<long long>(getpid())) + suffix;
}

edge_gateway::SerialPortOptions serialOptions(
    const std::string& device,
    int baudRate,
    const std::string& parity,
    int timeoutMs = 300
) {
    edge_gateway::SerialPortOptions options;
    options.device = device;
    options.baudRate = baudRate;
    options.dataBits = 8;
    options.stopBits = 1;
    options.parity = parity;
    options.timeoutMs = timeoutMs;
    options.frameIntervalMs = 0;
    options.readRetryCount = 0;
    return options;
}

void testModbusRtu() {
    const auto stateFile = statePath("-modbus.conf");
    gateway_test_lab::ControlState state;
    gateway_test_lab::StateFile::save(stateFile, state);

    gateway_test_lab::SerialSimulatorOptions simulatorOptions;
    simulatorOptions.protocol = gateway_test_lab::SerialProtocol::ModbusRtu;
    simulatorOptions.stateFile = stateFile;
    simulatorOptions.faultDelayMs = 200;
    gateway_test_lab::SerialProtocolSimulator simulator(simulatorOptions);
    simulator.start();

    const auto options = serialOptions(simulator.peerDevice(), 9600, "N");
    auto port = std::make_shared<edge_gateway::PosixSerialPort>(options);
    edge_gateway::ModbusRtuClient client(port, options);

    const auto initial = client.readHoldingRegisters(1, 1, 1);
    require(initial.size() == 1U && initial[0] >= 2200U && initial[0] <= 2220U,
            "Modbus RTU simulator returned an unexpected value");
    client.writeSingleRegister(1, 10, 4321);
    const auto written = client.readHoldingRegisters(1, 10, 1);
    require(written.size() == 1U && written[0] == 4321U,
            "Modbus RTU simulator did not retain a written register");

    state.faults[2] = gateway_test_lab::FaultMode::Exception;
    gateway_test_lab::StateFile::save(stateFile, state);
    std::this_thread::sleep_for(std::chrono::milliseconds(75));
    bool rejected = false;
    try {
        (void)client.readHoldingRegisters(2, 1, 1);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "Modbus RTU exception fault was not observed by the real client");
    state.faults.clear();
    gateway_test_lab::StateFile::save(stateFile, state);
    std::this_thread::sleep_for(std::chrono::milliseconds(75));
    require(client.readHoldingRegisters(2, 1, 1).size() == 1U,
            "Modbus RTU client did not recover after fault removal");

    const auto stats = simulator.stats();
    require(stats.requests >= 5U && stats.responses >= 5U,
            "Modbus RTU simulator statistics were not updated");
    simulator.stop();
    std::remove(stateFile.c_str());
}

void testDlt645() {
    const auto stateFile = statePath("-dlt645.conf");
    gateway_test_lab::ControlState state;
    gateway_test_lab::StateFile::save(stateFile, state);

    gateway_test_lab::SerialSimulatorOptions simulatorOptions;
    simulatorOptions.protocol = gateway_test_lab::SerialProtocol::Dlt645;
    simulatorOptions.baudRate = 2400;
    simulatorOptions.parity = "E";
    simulatorOptions.stateFile = stateFile;
    gateway_test_lab::SerialProtocolSimulator simulator(simulatorOptions);
    simulator.start();

    const auto options = serialOptions(simulator.peerDevice(), 2400, "E");
    auto port = std::make_shared<edge_gateway::PosixSerialPort>(options);
    edge_gateway::Dlt645Client client(port, options);

    edge_gateway::PointDefinition point;
    point.read.dlt645Di = "02010100";
    point.read.dlt645ByteCount = 2;
    point.read.dataType = "dlt645_bcd";
    point.read.scale = 0.1;
    const auto response = client.readData("1", point.read.dlt645Di);
    const auto decoded = edge_gateway::Dlt645Codec::decodeReadResponse(response, point);
    require(decoded.value >= 220.0 && decoded.value <= 222.0,
            "DL/T 645 simulator returned an unexpected phase voltage");

    client.writeData("1", point.read.dlt645Di, "00000000", "00000000", {0x34, 0x12});
    const auto afterWrite = edge_gateway::Dlt645Codec::decodeReadResponse(
        client.readData("1", point.read.dlt645Di), point
    );
    require(std::abs(afterWrite.value - 123.4) < 0.001,
            "DL/T 645 simulator did not retain a written payload");

    state.faults[2] = gateway_test_lab::FaultMode::Exception;
    gateway_test_lab::StateFile::save(stateFile, state);
    bool rejected = false;
    try {
        const auto faultResponse = client.readData("2", point.read.dlt645Di);
        (void)edge_gateway::Dlt645Codec::decodeReadResponse(faultResponse, point);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "DL/T 645 exception fault was not observed by the real client");
    state.faults.clear();
    gateway_test_lab::StateFile::save(stateFile, state);
    require(!client.readData("2", point.read.dlt645Di).empty(),
            "DL/T 645 client did not recover after fault removal");

    const auto stats = simulator.stats();
    require(stats.requests >= 5U && stats.responses >= 5U,
            "DL/T 645 simulator statistics were not updated");
    simulator.stop();
    std::remove(stateFile.c_str());
}
#endif

}  // namespace

int main() {
    try {
#ifndef _WIN32
        testModbusRtu();
        testDlt645();
#endif
        std::cout << "gateway_test_lab_serial_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "gateway_test_lab_serial_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
