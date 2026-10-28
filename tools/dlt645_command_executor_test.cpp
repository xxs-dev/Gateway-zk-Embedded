// DL/T 645 command executor: write gates, trip/close with position check before and read-back after
// execution, outcomeUnknown on lost replies, no retries.
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "dlt645_test_support.hpp"
#include "edge_gateway/dlt645_client.hpp"
#include "edge_gateway/dlt645_command_executor.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/serial_port.hpp"

using dlt645_test::MeterPort;
using dlt645_test::require;
using namespace edge_gateway;

namespace {

const char* kStore = "dlt645_command_executor_test_store";

PointDefinition switchPoint(std::uint32_t index, const std::string& code, const std::string& di, double expected) {
    PointDefinition point;
    point.index = index;
    point.pointCode = code;
    point.name = code;
    point.category = "command";
    point.enabled = true;
    point.write.enable = true;
    point.write.minValue = 0.0;
    point.write.maxValue = 99.0;
    point.write.step = 1.0;
    point.write.verifyAfterWrite = true;
    point.write.verifyByRead = true;
    point.write.verifyDelayMs = 0;
    point.write.dlt645.di = di;
    point.write.dlt645.dataType = "dlt645_scheduled_control";
    point.write.dlt645.byteCount = 2;
    point.write.dlt645.unit = 2;
    point.write.dlt645.verifyPointCode = "breaker_switch_state";
    point.write.dlt645.verifyValue = expected;
    point.write.dlt645.verifyTimeoutMs = 300;
    return point;
}

DeviceConfig breakerConfig() {
    DeviceConfig config;
    config.machineCode = "COMM_TEST";
    config.meterCode = "BREAKER_20";
    config.address = "000000000020";
    config.protocol.type = "dlt645_2007";
    config.protocol.dlt645.write.enabled = true;
    config.protocol.dlt645.write.password = dlt645_test::fixturePassword();
    config.protocol.dlt645.write.operatorCode = "01020304";

    PointDefinition state;
    state.index = 200000;
    state.pointCode = "breaker_switch_state";
    state.enabled = true;
    state.read.enable = true;
    state.read.dataType = "dlt645_bcd";
    state.read.dlt645Di = "07020100";
    state.read.dlt645ByteCount = 1;
    config.points.push_back(state);
    config.points.push_back(switchPoint(200001, "breaker_remote_trip", "06010101", 0.0));
    config.points.push_back(switchPoint(200002, "breaker_remote_close", "06010201", 1.0));

    auto testTrip = switchPoint(200003, "breaker_test_trip", "06010301", 0.0);
    testTrip.enabled = false;
    config.points.push_back(testTrip);

    PointDefinition cancel;
    cancel.index = 200004;
    cancel.pointCode = "breaker_cancel_trip";
    cancel.enabled = true;
    cancel.write.enable = true;
    cancel.write.allowedValues = {1.0};
    cancel.write.dlt645.di = "06010102";
    cancel.write.dlt645.dataType = "dlt645_none";
    config.points.push_back(cancel);
    return config;
}

SerialPortOptions options() {
    SerialPortOptions o;
    o.device = "test";
    o.timeoutMs = 20;
    o.wakeupBytes = 4;
    return o;
}

std::size_t writes(const MeterPort& port, const std::string& di) {
    return port.count(di, 0x14);
}

void tripAndCloseAreConfirmedByReadBack() {
    MemoryPointStore::cleanupOrphanedSegment(kStore);
    MemoryPointStore store(kStore);
    auto port = std::make_shared<MeterPort>();
    auto client = std::make_shared<Dlt645Client>(port, options());
    Dlt645CommandExecutor executor(breakerConfig(), store, client);

    const auto trip = executor.executeByIndex("cmd-trip", 200001, 0, 1000);
    require(trip.success && trip.verifyAttempted && trip.verifyPassed && !trip.outcomeUnknown,
            "trip must be confirmed by the position read-back: " + trip.message);
    require(writes(*port, "06010101") == 1 && !port->meter.breakerClosed(), "one C=14H trip frame");
    require(port->count("07020100") >= 2, "position read before (check) and after (confirm) the command");

    const auto again = executor.executeByIndex("cmd-trip-2", 200001, 0, 1001);
    require(again.success && again.verifyPassed && writes(*port, "06010101") == 1,
            "breaker already open: success without sending another trip");

    const auto close = executor.executeByIndex("cmd-close", 200002, 0, 1002);
    require(close.success && close.verifyPassed && port->meter.breakerClosed(), "close confirmed: " + close.message);

    const auto delayed = executor.executeByIndex("cmd-delayed", 200001, 5, 1003);
    require(delayed.success && !delayed.verifyAttempted && port->meter.breakerClosed(),
            "delayed trip is accepted but cannot be verified yet");

    const auto cancel = executor.executeByIndex("cmd-cancel", 200004, 1, 1004);
    require(cancel.success && !cancel.verifyAttempted, "cancel command has no read-back");
}

void gatesRejectWithoutSending() {
    MemoryPointStore::cleanupOrphanedSegment(kStore);
    MemoryPointStore store(kStore);
    auto port = std::make_shared<MeterPort>();
    auto client = std::make_shared<Dlt645Client>(port, options());
    auto config = breakerConfig();
    Dlt645CommandExecutor executor(config, store, client);
    require(!executor.executeByIndex("c1", 200001, 100, 1000).success, "out-of-range delay rejected");
    require(!executor.executeByIndex("c2", 200003, 0, 1000).success, "disabled test-trip point rejected");
    config.protocol.dlt645.write.enabled = false;
    Dlt645CommandExecutor disabled(config, store, client);
    require(!disabled.executeByIndex("c3", 200001, 0, 1000).success, "protocol-level write gate");
    require(port->requests.empty(), "rejected commands must not reach the bus");

    port->alive = false;  // position cannot be read: nothing is switched blind
    const auto blind = executor.executeByIndex("c4", 200001, 0, 1000);
    require(!blind.success && !blind.outcomeUnknown && writes(*port, "06010101") == 0,
            "unreadable position: command not sent: " + blind.message);
}

void lostReplyIsOutcomeUnknown() {
    MemoryPointStore::cleanupOrphanedSegment(kStore);
    MemoryPointStore store(kStore);
    auto port = std::make_shared<MeterPort>();
    port->acknowledgeWrites = false;  // reads answered, the write reply is lost
    auto client = std::make_shared<Dlt645Client>(port, options());
    Dlt645CommandExecutor executor(breakerConfig(), store, client);
    const auto result = executor.executeByIndex("cmd-lost", 200001, 0, 1000);
    require(!result.success && result.outcomeUnknown, "lost 94H reply must report outcomeUnknown: " + result.message);
    require(writes(*port, "06010101") == 1, "the write is not retried");
}

void readBackMismatchFails() {
    MemoryPointStore::cleanupOrphanedSegment(kStore);
    MemoryPointStore store(kStore);
    auto port = std::make_shared<MeterPort>();
    auto client = std::make_shared<Dlt645Client>(port, options());
    auto config = breakerConfig();
    // Acknowledged command whose effect never shows: test-trip DI does not move the simulated breaker.
    config.points[1].write.dlt645.di = "06010301";
    Dlt645CommandExecutor executor(config, store, client);
    const auto result = executor.executeByIndex("cmd-stuck", 200001, 0, 1000);
    require(!result.success && result.verifyAttempted && !result.verifyPassed && !result.outcomeUnknown,
            "position unchanged after 94H must fail verification: " + result.message);
    require(writes(*port, "06010301") == 1, "one command, no retry");
}

}  // namespace

int main() {
    try {
        tripAndCloseAreConfirmedByReadBack();
        gatesRejectWithoutSending();
        lostReplyIsOutcomeUnknown();
        readBackMismatchFails();
        MemoryPointStore::cleanupOrphanedSegment(kStore);
        std::cout << "dlt645_command_executor_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        MemoryPointStore::cleanupOrphanedSegment(kStore);
        std::cerr << "dlt645_command_executor_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
