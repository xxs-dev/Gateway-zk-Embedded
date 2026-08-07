#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "gateway_test_lab/modbus_simulator.hpp"

namespace gateway_test_lab {

enum class SerialProtocol {
    ModbusRtu,
    Dlt645
};

SerialProtocol parseSerialProtocol(const std::string& value);
std::string serialProtocolName(SerialProtocol protocol);

struct SerialSimulatorOptions {
    SerialProtocol protocol = SerialProtocol::ModbusRtu;
    std::string device = "auto";
    int baudRate = 9600;
    int dataBits = 8;
    int stopBits = 1;
    std::string parity = "N";
    std::string stateFile;
    int faultDelayMs = 1500;
};

struct SerialSimulatorStats {
    std::uint64_t requests = 0;
    std::uint64_t responses = 0;
    std::uint64_t reads = 0;
    std::uint64_t writes = 0;
    std::uint64_t injectedFaults = 0;
    std::uint64_t protocolErrors = 0;
};

class SerialProtocolSimulator {
public:
    explicit SerialProtocolSimulator(SerialSimulatorOptions options);
    ~SerialProtocolSimulator();

    SerialProtocolSimulator(const SerialProtocolSimulator&) = delete;
    SerialProtocolSimulator& operator=(const SerialProtocolSimulator&) = delete;

    void start();
    void stop();
    bool running() const;
    std::string peerDevice() const;
    SerialSimulatorStats stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway_test_lab
