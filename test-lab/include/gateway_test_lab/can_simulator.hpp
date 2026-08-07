#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace gateway_test_lab {

struct CanSimulatorOptions {
    std::string transportMode = "socketcan";
    std::string interfaceName = "gatewaytest0";
    std::string udpBindAddress = "127.0.0.1";
    int udpListenPort = 19012;
    std::string udpPeerAddress = "127.0.0.1";
    int udpPeerPort = 19011;
    std::string stateFile;
    int periodMs = 100;
};

struct CanSimulatorStats {
    std::uint64_t transmittedFrames = 0;
    std::uint64_t receivedFrames = 0;
    std::uint64_t injectedFaults = 0;
    std::uint64_t protocolErrors = 0;
};

class CanSimulator {
public:
    explicit CanSimulator(CanSimulatorOptions options);
    ~CanSimulator();

    CanSimulator(const CanSimulator&) = delete;
    CanSimulator& operator=(const CanSimulator&) = delete;

    void start();
    void stop();
    bool running() const;
    CanSimulatorStats stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gateway_test_lab
