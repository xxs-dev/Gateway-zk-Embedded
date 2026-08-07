#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace gateway_test_lab {

struct Iec104SimulatorOptions {
    std::string bindAddress = "127.0.0.1";
    std::uint16_t port = 12404;
    std::string stateFile;
    int commonAddress = 1;
    int periodMs = 200;
};

struct Iec104SimulatorStats {
    std::uint64_t acceptedConnections = 0;
    std::uint64_t receivedFrames = 0;
    std::uint64_t transmittedFrames = 0;
    std::uint64_t commands = 0;
    std::uint64_t injectedFaults = 0;
    std::uint64_t protocolErrors = 0;
};

class Iec104Simulator {
public:
    explicit Iec104Simulator(Iec104SimulatorOptions options);
    ~Iec104Simulator();

    Iec104Simulator(const Iec104Simulator&) = delete;
    Iec104Simulator& operator=(const Iec104Simulator&) = delete;

    void start();
    void stop();
    bool running() const;
    std::uint16_t port() const;
    Iec104SimulatorStats stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway_test_lab
