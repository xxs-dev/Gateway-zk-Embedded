#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "edge_gateway/dlt645_client.hpp"
#include "edge_gateway/dlt645_command_executor.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/serial_port.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::uint8_t checksum(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t sum = 0;
    for (const auto byte : bytes) {
        sum += byte;
    }
    return static_cast<std::uint8_t>(sum & 0xFFU);
}

class AckSerialPort : public edge_gateway::ISerialPort {
public:
    void open() override {
        open_ = true;
    }

    void close() override {
        open_ = false;
    }

    bool isOpen() const override {
        return open_;
    }

    void write(const std::vector<std::uint8_t>& bytes) override {
        require(open_, "write on closed serial port");
        writes_.push_back(bytes);
        const auto start = std::find(bytes.begin(), bytes.end(), static_cast<std::uint8_t>(0x68));
        require(start != bytes.end() && std::distance(start, bytes.end()) >= 12, "invalid request frame");
        response_ = {0x68};
        response_.insert(response_.end(), start + 1, start + 7);
        response_.push_back(0x68);
        response_.push_back(0x94);
        response_.push_back(0x00);
        response_.push_back(checksum(response_));
        response_.push_back(0x16);
    }

    std::vector<std::uint8_t> read(std::size_t maxBytes, int timeoutMs) override {
        (void)timeoutMs;
        if (response_.empty()) {
            return {};
        }
        const auto count = std::min(maxBytes, response_.size());
        std::vector<std::uint8_t> result(response_.begin(), response_.begin() + static_cast<std::ptrdiff_t>(count));
        response_.erase(response_.begin(), response_.begin() + static_cast<std::ptrdiff_t>(count));
        return result;
    }

    const std::vector<std::vector<std::uint8_t>>& writes() const {
        return writes_;
    }

private:
    bool open_ = false;
    std::vector<std::uint8_t> response_;
    std::vector<std::vector<std::uint8_t>> writes_;
};

edge_gateway::DeviceConfig breakerConfig() {
    edge_gateway::DeviceConfig config;
    config.machineCode = "COMM_TEST";
    config.meterCode = "BREAKER_20";
    config.address = "000000000020";
    config.protocol.type = "dlt645_2007";
    config.protocol.dlt645.write.enabled = true;
    config.protocol.dlt645.write.password = "02000000";
    config.protocol.dlt645.write.operatorCode = "01020304";

    edge_gateway::PointDefinition trip;
    trip.index = 200001;
    trip.pointCode = "breaker_remote_trip";
    trip.name = "远程跳闸";
    trip.category = "command";
    trip.enabled = true;
    trip.write.enable = true;
    trip.write.minValue = 0.0;
    trip.write.maxValue = 99.0;
    trip.write.step = 1.0;
    trip.write.dlt645.di = "06010101";
    trip.write.dlt645.dataType = "dlt645_scheduled_control";
    trip.write.dlt645.byteCount = 2;
    trip.write.dlt645.unit = 2;
    config.points.push_back(trip);

    auto testTrip = trip;
    testTrip.index = 200002;
    testTrip.pointCode = "breaker_test_trip";
    testTrip.enabled = false;
    testTrip.write.dlt645.di = "06010301";
    config.points.push_back(testTrip);
    return config;
}

void verifyWriteExecutionAndSafetyGates() {
    edge_gateway::SerialPortOptions options;
    options.device = "test";
    options.timeoutMs = 50;
    options.wakeupBytes = 4;
    auto serial = std::make_shared<AckSerialPort>();
    auto client = std::make_shared<edge_gateway::Dlt645Client>(serial, options);
    {
        edge_gateway::MemoryPointStore store("dlt645_command_executor_test_store");

        auto config = breakerConfig();
        edge_gateway::Dlt645CommandExecutor executor(config, store, client);
        const auto result = executor.executeByIndex("cmd-1", 200001, 0, 1000);
        require(result.success, "enabled DLT645 command should succeed on 94H response");
        require(!result.verifyAttempted && !result.verifyPassed, "protocol acknowledgement must not claim physical verification");
        require(serial->writes().size() == 1, "successful command should emit one serial frame");
        const auto frameStart = std::find(serial->writes().front().begin(), serial->writes().front().end(), 0x68);
        require(frameStart != serial->writes().front().end() && *(frameStart + 8) == 0x14, "command must use C=14H");

        const auto outOfRange = executor.executeByIndex("cmd-2", 200001, 100, 1001);
        require(!outOfRange.success, "out-of-range scheduled delay must be rejected");
        require(serial->writes().size() == 1, "rejected value must not emit a serial frame");

        const auto disabledPoint = executor.executeByIndex("cmd-3", 200002, 0, 1002);
        require(!disabledPoint.success, "disabled test-trip point must be rejected");
        require(serial->writes().size() == 1, "disabled point must not emit a serial frame");

        config.protocol.dlt645.write.enabled = false;
        edge_gateway::Dlt645CommandExecutor disabledExecutor(config, store, client);
        const auto disabledProtocol = disabledExecutor.executeByIndex("cmd-4", 200001, 0, 1003);
        require(!disabledProtocol.success, "protocol-level write gate must reject commands");
        require(serial->writes().size() == 1, "disabled protocol must not emit a serial frame");
    }
#ifndef _WIN32
    require(
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment("dlt645_command_executor_test_store"),
        "test shared memory should be cleaned"
    );
#endif
}

}  // namespace

int main() {
    try {
        verifyWriteExecutionAndSafetyGates();
        std::cout << "dlt645_command_executor_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "dlt645_command_executor_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
