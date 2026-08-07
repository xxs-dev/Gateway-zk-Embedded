#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace gateway_test_lab {

enum class FaultMode {
    None,
    Timeout,
    Disconnect,
    Exception,
    WriteTimeout,
    WriteVerifyFailed
};

struct ValueKey {
    int slave = 0;
    std::string area;
    int address = 0;

    bool operator<(const ValueKey& other) const;
};

struct ControlState {
    std::string scenario = "normal";
    std::vector<int> slaves{1, 2, 3};
    std::map<int, FaultMode> faults;
    std::map<ValueKey, std::uint16_t> values;
};

std::string faultModeName(FaultMode mode);
FaultMode parseFaultMode(const std::string& text);

class StateFile {
public:
    static ControlState load(const std::string& path);
    static void save(const std::string& path, const ControlState& state);
};

struct SimulatorOptions {
    std::string bindAddress = "127.0.0.1";
    std::uint16_t port = 15020;
    std::string stateFile;
    int faultDelayMs = 1500;
};

struct SimulatorStats {
    std::uint64_t acceptedConnections = 0;
    std::uint64_t requests = 0;
    std::uint64_t responses = 0;
    std::uint64_t reads = 0;
    std::uint64_t writes = 0;
    std::uint64_t injectedFaults = 0;
    std::uint64_t protocolErrors = 0;
};

class ModbusTcpSimulator {
public:
    explicit ModbusTcpSimulator(SimulatorOptions options);
    ~ModbusTcpSimulator();

    ModbusTcpSimulator(const ModbusTcpSimulator&) = delete;
    ModbusTcpSimulator& operator=(const ModbusTcpSimulator&) = delete;

    void start();
    void stop();
    bool running() const;
    std::uint16_t port() const;
    SimulatorStats stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway_test_lab
