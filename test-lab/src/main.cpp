#include "gateway_test_lab/modbus_simulator.hpp"
#include "gateway_test_lab/can_simulator.hpp"
#include "gateway_test_lab/iec104_simulator.hpp"
#include "gateway_test_lab/serial_simulator.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 8
#include <experimental/filesystem>
namespace test_lab_fs = std::experimental::filesystem;
#else
#include <filesystem>
namespace test_lab_fs = std::filesystem;
#endif
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

std::atomic<bool> gRunning{true};

void handleSignal(int) {
    gRunning.store(false);
}

std::string optionValue(int argc, char* argv[], const std::string& name, const std::string& fallback = {}) {
    for (int i = 2; i + 1 < argc; ++i) {
        if (argv[i] == name) return argv[i + 1];
    }
    return fallback;
}

bool hasOption(int argc, char* argv[], const std::string& name) {
    for (int i = 2; i < argc; ++i) {
        if (argv[i] == name) return true;
    }
    return false;
}

int intOption(int argc, char* argv[], const std::string& name, int fallback) {
    const auto value = optionValue(argc, argv, name);
    if (value.empty()) return fallback;
    std::size_t used = 0;
    const auto parsed = std::stoi(value, &used);
    if (used != value.size()) throw std::runtime_error("invalid value for " + name + ": " + value);
    return parsed;
}

std::vector<int> parseSlaves(const std::string& value) {
    std::vector<int> slaves;
    std::istringstream input(value);
    std::string item;
    while (std::getline(input, item, ',')) {
        if (item.empty()) continue;
        const auto slave = std::stoi(item);
        if (slave < 1 || slave > 247) throw std::runtime_error("slave must be between 1 and 247");
        slaves.push_back(slave);
    }
    if (slaves.empty()) throw std::runtime_error("at least one slave is required");
    return slaves;
}

std::string requiredOption(int argc, char* argv[], const std::string& name) {
    const auto value = optionValue(argc, argv, name);
    if (value.empty()) throw std::runtime_error(name + " is required");
    return value;
}

void printState(const gateway_test_lab::ControlState& state) {
    std::cout << "scenario=" << state.scenario << '\n';
    std::cout << "slaves=";
    for (std::size_t i = 0; i < state.slaves.size(); ++i) {
        if (i > 0) std::cout << ',';
        std::cout << state.slaves[i];
    }
    std::cout << '\n';
    for (const auto& fault : state.faults) {
        if (fault.second != gateway_test_lab::FaultMode::None) {
            std::cout << "fault." << fault.first << '='
                      << gateway_test_lab::faultModeName(fault.second) << '\n';
        }
    }
    for (const auto& value : state.values) {
        std::cout << "value." << value.first.slave << '.' << value.first.area << '.'
                  << value.first.address << '=' << value.second << '\n';
    }
}

void writeStatus(
    const std::string& path,
    const gateway_test_lab::ModbusTcpSimulator& simulator,
    const std::string& stateFile
) {
    if (path.empty()) return;
    const auto stats = simulator.stats();
    const auto temporary = path + ".tmp";
    const test_lab_fs::path target(path);
    if (!target.parent_path().empty()) test_lab_fs::create_directories(target.parent_path());
    {
        std::ofstream output(temporary, std::ios::trunc);
        output << "{\"running\":true,\"port\":" << simulator.port()
               << ",\"stateFile\":\"" << stateFile
               << "\",\"acceptedConnections\":" << stats.acceptedConnections
               << ",\"requests\":" << stats.requests
               << ",\"responses\":" << stats.responses
               << ",\"reads\":" << stats.reads
               << ",\"writes\":" << stats.writes
               << ",\"injectedFaults\":" << stats.injectedFaults
               << ",\"protocolErrors\":" << stats.protocolErrors << "}\n";
    }
    std::error_code error;
    test_lab_fs::remove(target, error);
    error.clear();
    test_lab_fs::rename(temporary, target, error);
}

void writeSerialStatus(
    const std::string& path,
    const gateway_test_lab::SerialProtocolSimulator& simulator,
    const std::string& stateFile,
    gateway_test_lab::SerialProtocol protocol
) {
    if (path.empty()) return;
    const auto stats = simulator.stats();
    const auto temporary = path + ".tmp";
    const test_lab_fs::path target(path);
    if (!target.parent_path().empty()) test_lab_fs::create_directories(target.parent_path());
    {
        std::ofstream output(temporary, std::ios::trunc);
        output << "{\"running\":true,\"protocol\":\""
               << gateway_test_lab::serialProtocolName(protocol)
               << "\",\"peerDevice\":\"" << simulator.peerDevice()
               << "\",\"stateFile\":\"" << stateFile
               << "\",\"requests\":" << stats.requests
               << ",\"responses\":" << stats.responses
               << ",\"reads\":" << stats.reads
               << ",\"writes\":" << stats.writes
               << ",\"injectedFaults\":" << stats.injectedFaults
               << ",\"protocolErrors\":" << stats.protocolErrors << "}\n";
    }
    std::error_code error;
    test_lab_fs::remove(target, error);
    error.clear();
    test_lab_fs::rename(temporary, target, error);
}

void writeCanStatus(
    const std::string& path,
    const gateway_test_lab::CanSimulator& simulator,
    const std::string& stateFile,
    const gateway_test_lab::CanSimulatorOptions& options
) {
    if (path.empty()) return;
    const auto stats = simulator.stats();
    const auto temporary = path + ".tmp";
    const test_lab_fs::path target(path);
    if (!target.parent_path().empty()) test_lab_fs::create_directories(target.parent_path());
    {
        std::ofstream output(temporary, std::ios::trunc);
        output << "{\"running\":true,\"protocol\":\"can\",\"transportMode\":\""
               << options.transportMode << "\",\"interface\":\"" << options.interfaceName
               << "\",\"udpListenPort\":" << options.udpListenPort
               << ",\"udpPeerPort\":" << options.udpPeerPort
               << ",\"stateFile\":\"" << stateFile
               << "\",\"transmittedFrames\":" << stats.transmittedFrames
               << ",\"receivedFrames\":" << stats.receivedFrames
               << ",\"injectedFaults\":" << stats.injectedFaults
               << ",\"protocolErrors\":" << stats.protocolErrors << "}\n";
    }
    std::error_code error;
    test_lab_fs::remove(target, error);
    error.clear();
    test_lab_fs::rename(temporary, target, error);
}

void writeIec104Status(
    const std::string& path,
    const gateway_test_lab::Iec104Simulator& simulator,
    const std::string& stateFile
) {
    if (path.empty()) return;
    const auto stats = simulator.stats();
    const auto temporary = path + ".tmp";
    const test_lab_fs::path target(path);
    if (!target.parent_path().empty()) test_lab_fs::create_directories(target.parent_path());
    {
        std::ofstream output(temporary, std::ios::trunc);
        output << "{\"running\":true,\"protocol\":\"iec104\",\"port\":" << simulator.port()
               << ",\"stateFile\":\"" << stateFile
               << "\",\"acceptedConnections\":" << stats.acceptedConnections
               << ",\"receivedFrames\":" << stats.receivedFrames
               << ",\"transmittedFrames\":" << stats.transmittedFrames
               << ",\"commands\":" << stats.commands
               << ",\"injectedFaults\":" << stats.injectedFaults
               << ",\"protocolErrors\":" << stats.protocolErrors << "}\n";
    }
    std::error_code error;
    test_lab_fs::remove(target, error);
    error.clear();
    test_lab_fs::rename(temporary, target, error);
}

int serve(int argc, char* argv[]) {
    gateway_test_lab::SimulatorOptions options;
    options.bindAddress = optionValue(argc, argv, "--bind", "127.0.0.1");
    const auto port = intOption(argc, argv, "--port", 15020);
    if (port < 0 || port > 65535) throw std::runtime_error("port must be between 0 and 65535");
    options.port = static_cast<std::uint16_t>(port);
    options.stateFile = requiredOption(argc, argv, "--state-file");
    options.faultDelayMs = intOption(argc, argv, "--fault-delay-ms", 1500);
    const auto statusFile = optionValue(argc, argv, "--status-file");

    gateway_test_lab::ModbusTcpSimulator simulator(options);
    simulator.start();
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::cout << "gateway test-lab Modbus TCP simulator listening on "
              << options.bindAddress << ':' << simulator.port() << std::endl;
    while (gRunning.load()) {
        writeStatus(statusFile, simulator, options.stateFile);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    simulator.stop();
    if (!statusFile.empty()) {
        std::error_code error;
        test_lab_fs::remove(statusFile, error);
    }
    return 0;
}

int serveSerial(int argc, char* argv[]) {
    gateway_test_lab::SerialSimulatorOptions options;
    options.protocol = gateway_test_lab::parseSerialProtocol(requiredOption(argc, argv, "--protocol"));
    options.device = optionValue(argc, argv, "--serial-device", "auto");
    options.baudRate = intOption(argc, argv, "--baud-rate",
        options.protocol == gateway_test_lab::SerialProtocol::Dlt645 ? 2400 : 9600);
    options.dataBits = intOption(argc, argv, "--data-bits", 8);
    options.stopBits = intOption(argc, argv, "--stop-bits", 1);
    options.parity = optionValue(argc, argv, "--parity",
        options.protocol == gateway_test_lab::SerialProtocol::Dlt645 ? "E" : "N");
    options.stateFile = requiredOption(argc, argv, "--state-file");
    options.faultDelayMs = intOption(argc, argv, "--fault-delay-ms", 1500);
    const auto statusFile = optionValue(argc, argv, "--status-file");

    gateway_test_lab::SerialProtocolSimulator simulator(options);
    simulator.start();
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::cout << "gateway test-lab " << gateway_test_lab::serialProtocolName(options.protocol)
              << " simulator ready, peer device " << simulator.peerDevice() << std::endl;
    while (gRunning.load()) {
        writeSerialStatus(statusFile, simulator, options.stateFile, options.protocol);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    simulator.stop();
    if (!statusFile.empty()) {
        std::error_code error;
        test_lab_fs::remove(statusFile, error);
    }
    return 0;
}

int serveCan(int argc, char* argv[]) {
    gateway_test_lab::CanSimulatorOptions options;
    options.transportMode = optionValue(argc, argv, "--transport", "socketcan");
    options.interfaceName = optionValue(argc, argv, "--interface", "gatewaytest0");
    options.udpBindAddress = optionValue(argc, argv, "--udp-bind", "127.0.0.1");
    options.udpListenPort = intOption(argc, argv, "--udp-listen-port", 19012);
    options.udpPeerAddress = optionValue(argc, argv, "--udp-peer", "127.0.0.1");
    options.udpPeerPort = intOption(argc, argv, "--udp-peer-port", 19011);
    options.stateFile = requiredOption(argc, argv, "--state-file");
    options.periodMs = intOption(argc, argv, "--period-ms", 100);
    const auto statusFile = optionValue(argc, argv, "--status-file");
    gateway_test_lab::CanSimulator simulator(options);
    simulator.start();
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::cout << "gateway test-lab CAN simulator ready transport=" << options.transportMode;
    if (options.transportMode == "udp_test") {
        std::cout << " listen=" << options.udpBindAddress << ':' << options.udpListenPort
                  << " peer=" << options.udpPeerAddress << ':' << options.udpPeerPort;
    } else {
        std::cout << " interface=" << options.interfaceName;
    }
    std::cout << std::endl;
    while (gRunning.load()) {
        writeCanStatus(statusFile, simulator, options.stateFile, options);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    simulator.stop();
    if (!statusFile.empty()) {
        std::error_code error;
        test_lab_fs::remove(statusFile, error);
    }
    return 0;
}

int serveIec104(int argc, char* argv[]) {
    gateway_test_lab::Iec104SimulatorOptions options;
    options.bindAddress = optionValue(argc, argv, "--bind", "127.0.0.1");
    const auto port = intOption(argc, argv, "--port", 12404);
    if (port < 0 || port > 65535) throw std::runtime_error("port must be between 0 and 65535");
    options.port = static_cast<std::uint16_t>(port);
    options.stateFile = requiredOption(argc, argv, "--state-file");
    options.commonAddress = intOption(argc, argv, "--common-address", 1);
    options.periodMs = intOption(argc, argv, "--period-ms", 200);
    const auto statusFile = optionValue(argc, argv, "--status-file");
    gateway_test_lab::Iec104Simulator simulator(options);
    simulator.start();
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::cout << "gateway test-lab IEC104 simulator listening on "
              << options.bindAddress << ':' << simulator.port() << std::endl;
    while (gRunning.load()) {
        writeIec104Status(statusFile, simulator, options.stateFile);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    simulator.stop();
    if (!statusFile.empty()) {
        std::error_code error;
        test_lab_fs::remove(statusFile, error);
    }
    return 0;
}

gateway_test_lab::ControlState loadOrDefault(const std::string& stateFile) {
    if (!test_lab_fs::exists(stateFile)) return {};
    return gateway_test_lab::StateFile::load(stateFile);
}

int updateState(int argc, char* argv[], const std::string& command) {
    const auto stateFile = requiredOption(argc, argv, "--state-file");
    auto state = command == "init" ? gateway_test_lab::ControlState{} : loadOrDefault(stateFile);
    if (command == "init") {
        state.scenario = optionValue(argc, argv, "--scenario", "normal");
        state.slaves = parseSlaves(optionValue(argc, argv, "--slaves", "1,2,3"));
        state.faults.clear();
        state.values.clear();
    } else if (command == "scenario") {
        state.scenario = requiredOption(argc, argv, "--name");
    } else if (command == "fault") {
        const auto slave = intOption(argc, argv, "--slave", 0);
        if (slave < 1 || slave > 247) throw std::runtime_error("--slave must be between 1 and 247");
        state.faults[slave] = gateway_test_lab::parseFaultMode(requiredOption(argc, argv, "--mode"));
    } else if (command == "recover") {
        const auto slave = intOption(argc, argv, "--slave", 0);
        if (slave < 1 || slave > 247) throw std::runtime_error("--slave must be between 1 and 247");
        state.faults.erase(slave);
    } else if (command == "set-value") {
        gateway_test_lab::ValueKey key;
        key.slave = intOption(argc, argv, "--slave", 0);
        key.area = requiredOption(argc, argv, "--area");
        key.address = intOption(argc, argv, "--address", -1);
        const auto value = intOption(argc, argv, "--value", -1);
        if (key.slave < 1 || key.slave > 247 || key.address < 0 || key.address > 65535 ||
            value < 0 || value > 65535) {
            throw std::runtime_error("invalid set-value slave, address, or value");
        }
        state.values[key] = static_cast<std::uint16_t>(value);
    } else if (command == "clear-value") {
        gateway_test_lab::ValueKey key;
        key.slave = intOption(argc, argv, "--slave", 0);
        key.area = requiredOption(argc, argv, "--area");
        key.address = intOption(argc, argv, "--address", -1);
        state.values.erase(key);
    } else {
        throw std::runtime_error("unsupported state command: " + command);
    }
    gateway_test_lab::StateFile::save(stateFile, state);
    printState(gateway_test_lab::StateFile::load(stateFile));
    return 0;
}

void printUsage() {
    std::cout
        << "Usage:\n"
        << "  gateway-test-lab-sim serve --state-file FILE [--bind IP] [--port PORT]\n"
        << "  gateway-test-lab-sim serve-serial --protocol modbus-rtu|dlt645 --state-file FILE\n"
        << "      [--serial-device auto|DEVICE] [--baud-rate N] [--parity N|E|O]\n"
        << "  gateway-test-lab-sim serve-can --state-file FILE [--transport socketcan|udp_test]\n"
        << "      [--interface NAME] [--udp-bind IP] [--udp-listen-port N]\n"
        << "      [--udp-peer IP] [--udp-peer-port N] [--period-ms N]\n"
        << "  gateway-test-lab-sim serve-iec104 --state-file FILE [--bind IP] [--port PORT]\n"
        << "  gateway-test-lab-sim init --state-file FILE [--scenario NAME] [--slaves 1,2,3]\n"
        << "  gateway-test-lab-sim scenario --state-file FILE --name normal|ramp|random|boundary\n"
        << "  gateway-test-lab-sim fault --state-file FILE --slave N --mode MODE\n"
        << "  gateway-test-lab-sim recover --state-file FILE --slave N\n"
        << "  gateway-test-lab-sim set-value --state-file FILE --slave N --area AREA --address N --value N\n"
        << "  gateway-test-lab-sim clear-value --state-file FILE --slave N --area AREA --address N\n"
        << "  gateway-test-lab-sim show --state-file FILE\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc < 2 || hasOption(argc, argv, "--help")) {
            printUsage();
            return argc < 2 ? 2 : 0;
        }
        const std::string command = argv[1];
        if (command == "serve") return serve(argc, argv);
        if (command == "serve-serial") return serveSerial(argc, argv);
        if (command == "serve-can") return serveCan(argc, argv);
        if (command == "serve-iec104") return serveIec104(argc, argv);
        if (command == "show") {
            printState(gateway_test_lab::StateFile::load(requiredOption(argc, argv, "--state-file")));
            return 0;
        }
        if (command == "init" || command == "scenario" || command == "fault" ||
            command == "recover" || command == "set-value" || command == "clear-value") {
            return updateState(argc, argv, command);
        }
        printUsage();
        return 2;
    } catch (const std::exception& ex) {
        std::cerr << "gateway-test-lab-sim: " << ex.what() << std::endl;
        return 1;
    }
}
