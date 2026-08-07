#include "gateway_test_lab/iec104_simulator.hpp"
#include "gateway_test_lab/modbus_simulator.hpp"

#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#include "edge_gateway/iec_client.hpp"
#endif

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

#ifndef _WIN32
void runTest() {
    const auto stateFile = "/tmp/gateway-test-lab-iec104-" +
        std::to_string(static_cast<long long>(getpid())) + ".conf";
    gateway_test_lab::StateFile::save(stateFile, gateway_test_lab::ControlState{});

    gateway_test_lab::Iec104SimulatorOptions simulatorOptions;
    simulatorOptions.port = 0;
    simulatorOptions.stateFile = stateFile;
    simulatorOptions.periodMs = 50;
    gateway_test_lab::Iec104Simulator simulator(simulatorOptions);
    simulator.start();

    edge_gateway::TcpTransportConfig tcp;
    tcp.host = "127.0.0.1";
    tcp.port = simulator.port();
    tcp.connectTimeoutMs = 500;
    tcp.timeoutMs = 300;
    edge_gateway::IecProtocolConfig iec;
    iec.commonAddress = 1;
    iec.backgroundReceive = true;
    iec.pollOnCollect = false;
    iec.pollTimeoutMs = 500;
    iec.idleReadTimeoutMs = 20;
    iec.t0Ms = 1000;
    iec.t1Ms = 1000;
    iec.t2Ms = 100;
    iec.t3Ms = 1000;
    iec.wAck = 1;

    {
        edge_gateway::IecTcpClient client("iec104", tcp, iec);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        auto values = client.poll();
        if (values.size() < 2U) {
            const auto moreValues = client.poll();
            values.insert(values.end(), moreValues.begin(), moreValues.end());
        }
        const auto earlyStats = simulator.stats();
        require(values.size() >= 2U,
            "IEC104 simulator did not publish periodic telemetry values=" +
            std::to_string(values.size()) + " rx=" + std::to_string(earlyStats.receivedFrames) +
            " tx=" + std::to_string(earlyStats.transmittedFrames));

        edge_gateway::PointDefinition command;
        command.index = 999001;
        command.pointCode = "SIM_IEC104_COMMAND";
        command.write.enable = true;
        command.write.dataType = "single_command";
        command.write.allowedValues = {0.0, 1.0};
        command.write.iec.ioa = 3001;
        command.write.iec.typeId = 45;
        command.write.iec.commonAddress = 1;
        command.write.iec.selectBeforeExecute = true;
        command.write.iec.waitActivationTermination = true;
        command.write.iec.timeoutMs = 1000;
        const auto result = client.writeByPoint(
            command, 1.0, "TEST_IEC104", "SIM_COMM202600999", "SIM_IEC104_01", 1000
        );
        require(result.success, "IEC104 command failed: " + result.message);
    }

    const auto stats = simulator.stats();
    require(stats.acceptedConnections == 1U, "IEC104 simulator did not accept the client");
    require(stats.transmittedFrames >= 4U, "IEC104 simulator transmitted too few frames");
    require(stats.commands >= 2U, "IEC104 select/execute commands were not observed");
    simulator.stop();
    std::remove(stateFile.c_str());
}
#endif

}  // namespace

int main() {
    try {
#ifndef _WIN32
        runTest();
#endif
        std::cout << "gateway_test_lab_iec104_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "gateway_test_lab_iec104_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
