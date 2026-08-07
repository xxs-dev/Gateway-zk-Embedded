#include "gateway_test_lab/modbus_simulator.hpp"

#include <chrono>
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 8
#include <experimental/filesystem>
namespace test_lab_fs = std::experimental::filesystem;
#else
#include <filesystem>
namespace test_lab_fs = std::filesystem;
#endif
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "edge_gateway/modbus_tcp_client.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string temporaryStateFile() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return (test_lab_fs::temp_directory_path() /
            ("gateway-test-lab-state-" + std::to_string(stamp) + ".conf")).string();
}

void waitForStateReload() {
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
}

}  // namespace

int main() {
    const auto stateFile = temporaryStateFile();
    try {
        gateway_test_lab::ControlState state;
        state.scenario = "normal";
        state.slaves = {1, 2};
        gateway_test_lab::StateFile::save(stateFile, state);

        gateway_test_lab::SimulatorOptions options;
        options.bindAddress = "127.0.0.1";
        options.port = 0;
        options.stateFile = stateFile;
        options.faultDelayMs = 120;
        gateway_test_lab::ModbusTcpSimulator simulator(options);
        simulator.start();

        edge_gateway::TcpTransportConfig tcp;
        tcp.host = "127.0.0.1";
        tcp.port = simulator.port();
        tcp.connectTimeoutMs = 300;
        tcp.timeoutMs = 80;
        edge_gateway::ModbusTcpClient client(tcp, 125);

        const auto normal = client.readHoldingRegisters(1, 1, 2);
        require(normal.size() == 2, "normal register read should return two values");
        require(normal[0] >= 2200 && normal[0] < 2220, "normal voltage waveform mismatch");

        client.writeSingleRegister(1, 10, 4321);
        require(client.readHoldingRegisters(1, 10, 1).front() == 4321,
                "single register write should be readable");
        client.writeSingleCoil(1, 3, true);
        require(client.readCoils(1, 3, 1).front() == 1,
                "single coil write should be readable");

        state.faults[1] = gateway_test_lab::FaultMode::WriteVerifyFailed;
        gateway_test_lab::StateFile::save(stateFile, state);
        waitForStateReload();
        client.writeSingleRegister(1, 10, 9999);
        require(client.readHoldingRegisters(1, 10, 1).front() == 4321,
                "write-verify-failed must acknowledge without changing the value");

        state.faults[1] = gateway_test_lab::FaultMode::Exception;
        gateway_test_lab::StateFile::save(stateFile, state);
        waitForStateReload();
        bool exceptionSeen = false;
        try {
            (void)client.readHoldingRegisters(1, 0, 1);
        } catch (const std::runtime_error&) {
            exceptionSeen = true;
        }
        require(exceptionSeen, "exception fault should be visible to the Modbus client");

        state.faults.erase(1);
        gateway_test_lab::StateFile::save(stateFile, state);
        waitForStateReload();
        require(client.readHoldingRegisters(1, 0, 1).size() == 1,
                "recovered slave should respond again");
        require(client.readHoldingRegisters(2, 0, 1).size() == 1,
                "healthy slave should remain available");

        const auto stats = simulator.stats();
        require(stats.reads >= 6, "simulator should count register and coil reads");
        require(stats.writes >= 3, "simulator should count writes");
        require(stats.injectedFaults >= 2, "simulator should count injected faults");
        simulator.stop();
        test_lab_fs::remove(stateFile);
        std::cout << "gateway_test_lab_modbus_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        test_lab_fs::remove(stateFile);
        std::cerr << "gateway_test_lab_modbus_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
