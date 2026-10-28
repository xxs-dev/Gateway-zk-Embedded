// DL/T 645 command executor: write gates, trip/close with position check before and read-back after
// execution, outcomeUnknown on lost replies, no retries.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dlt645_test_support.hpp"
#include "edge_gateway/dlt645_client.hpp"
#include "edge_gateway/dlt645_codec.hpp"
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

// COMM104 breakers have no position DI (07020100 answers D1): the position comes from a DIO
// auxiliary contact (another driver) through the shared point store (write.dlt645.verifyIndex).
class AuxContactPort : public MeterPort {
public:
    std::atomic<bool> closed{true};
    void write(const std::vector<std::uint8_t>& bytes) override {
        MeterPort::write(bytes);
        closed = meter.breakerClosed();
    }
};

const std::uint32_t kAuxIndex = 999;

PointDefinition auxContactPoint() {
    PointDefinition point;
    point.index = kAuxIndex;
    point.pointCode = "di_load_breaker_closed";
    point.enabled = true;
    point.read.enable = true;
    point.read.dataType = "digital_input";
    return point;
}

void putAux(MemoryPointStore& store, double value) {
    PointValue v;
    v.index = kAuxIndex;
    v.machineCode = "COMM_TEST";
    v.meterCode = "DIO";
    v.pointCode = "di_load_breaker_closed";
    v.value = value;
    v.ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    v.expireAt = v.ts + 60000;
    store.putLatest(v);
}

DeviceConfig auxBreakerConfig() {
    auto config = breakerConfig();
    config.points.erase(config.points.begin());  // no same-meter position point at all
    for (auto& point : config.points) {
        point.write.dlt645.verifyPointCode.clear();
        point.write.dlt645.verifyIndex = kAuxIndex;
    }
    return config;
}

void readBackFromAnotherDevice() {
    MemoryPointStore::cleanupOrphanedSegment(kStore);
    MemoryPointStore store(kStore);
    store.registerPoint("COMM_TEST", "DIO", auxContactPoint());
    auto port = std::make_shared<AuxContactPort>();
    auto client = std::make_shared<Dlt645Client>(port, options());
    Dlt645CommandExecutor executor(auxBreakerConfig(), store, client);

    // No DIO value yet: the position is unknown, nothing is switched blind.
    const auto blind = executor.executeByIndex("aux-blind", 200001, 0, 1000);
    require(!blind.success && !blind.outcomeUnknown && writes(*port, "06010101") == 0,
            "no auxiliary contact value: command not sent: " + blind.message);

    // The DIO collector keeps sampling the contact while the command runs.
    std::atomic<bool> running{true};
    std::thread dio([&] {
        while (running) {
            putAux(store, port->closed ? 1.0 : 0.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto trip = executor.executeByIndex("aux-trip", 200001, 0, 1001);
    const auto close = executor.executeByIndex("aux-close", 200002, 0, 1002);
    const auto again = executor.executeByIndex("aux-close-2", 200002, 0, 1003);
    running = false;
    dio.join();
    require(trip.success && trip.verifyAttempted && trip.verifyPassed && !trip.outcomeUnknown,
            "trip confirmed by the DIO auxiliary contact: " + trip.message);
    require(close.success && close.verifyPassed, "close confirmed by the DIO auxiliary contact: " + close.message);
    require(again.success && again.verifyPassed && writes(*port, "06010201") == 1,
            "contact already closed: success without sending: " + again.message);
    require(port->count("07020100") == 0, "the meter itself is never asked for its position");

    // DIO stops publishing after the command: the contact value is old, the outcome is unknown.
    putAux(store, 1.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto frozen = executor.executeByIndex("aux-frozen", 200001, 0, 1004);
    require(!frozen.success && frozen.outcomeUnknown && writes(*port, "06010101") == 2,
            "no contact value collected after the command: outcome unknown: " + frozen.message);
}

// DL/T 645-2007 C=1CH remote control on a meter whose relay state is status word 3 bit 4.
PointDefinition remotePoint(std::uint32_t index, const std::string& code, const std::string& type, double expected) {
    PointDefinition point;
    point.index = index;
    point.pointCode = code;
    point.enabled = true;
    point.write.enable = true;
    point.write.allowedValues = {1.0};
    point.write.dataType = "dlt645_remote_control";
    point.write.dlt645.dataType = "dlt645_remote_control";
    point.write.dlt645.controlType = type;
    point.write.dlt645.validitySeconds = 600;
    if (type == "trip" || type == "close") {
        point.write.verifyAfterWrite = true;
        point.write.verifyByRead = true;
        point.write.verifyDelayMs = 0;
        point.write.dlt645.verifyPointCode = "relay_open";
        point.write.dlt645.verifyValue = expected;
        point.write.dlt645.verifyTimeoutMs = 300;
    }
    return point;
}

DeviceConfig meterWithRelay() {
    DeviceConfig config;
    config.machineCode = "GW";
    config.meterCode = "METER_7";
    config.address = "000000000007";
    config.protocol.type = "dlt645_2007";
    config.protocol.dlt645.write.enabled = true;
    config.protocol.dlt645.write.password = dlt645_test::fixturePassword();
    config.protocol.dlt645.write.operatorCode = "01020304";
    PointDefinition relay;
    relay.index = 700000;
    relay.pointCode = "relay_open";
    relay.enabled = true;
    relay.read.enable = true;
    relay.read.dataType = "dlt645_bitfield_le";
    relay.read.dlt645Di = "04000503";
    relay.read.dlt645ByteCount = 2;
    relay.read.bit = 4;
    config.points.push_back(relay);
    config.points.push_back(remotePoint(700001, "remote_trip", "trip", 1.0));
    config.points.push_back(remotePoint(700002, "remote_close", "close", 0.0));
    config.points.push_back(remotePoint(700003, "keep_power", "keep_power", 0.0));
    return config;
}

void remoteControlUsesC1CAndReadBack() {
    MemoryPointStore::cleanupOrphanedSegment(kStore);
    MemoryPointStore store(kStore);
    auto port = std::make_shared<MeterPort>();
    auto client = std::make_shared<Dlt645Client>(port, options());
    Dlt645CommandExecutor executor(meterWithRelay(), store, client);

    const auto before = std::time(nullptr);
    const auto trip = executor.executeByIndex("rc-trip", 700001, 1, 1000);
    require(trip.success && trip.verifyAttempted && trip.verifyPassed && !port->meter.breakerClosed(),
            "remote trip confirmed by the relay bit: " + trip.message);
    require(port->countControl(0x1C) == 1 && port->meter.lastRemoteControl() == 0x1A, "one C=1CH frame, N1=1AH");
    // N3..N8: validity deadline ss mm hh DD MM YY = now + validitySeconds (local time).
    const auto& deadline = port->meter.lastRemoteControlDeadline();
    const auto bcd = [](int value) { return static_cast<std::uint8_t>(((value / 10) << 4) | (value % 10)); };
    bool deadlineOk = false;
    for (int lateSeconds = 0; lateSeconds <= 2 && !deadlineOk; ++lateSeconds) {
        const auto expected = Dlt645Codec::localTime((static_cast<std::int64_t>(before) + 600 + lateSeconds) * 1000);
        deadlineOk = deadline.size() == 6 && deadline[0] == bcd(expected.tm_sec) && deadline[1] == bcd(expected.tm_min) &&
            deadline[2] == bcd(expected.tm_hour) && deadline[3] == bcd(expected.tm_mday) &&
            deadline[4] == bcd(expected.tm_mon + 1) && deadline[5] == bcd((expected.tm_year + 1900) % 100);
    }
    require(deadlineOk, "validity deadline (ss mm hh DD MM YY) must be ten minutes ahead");

    const auto again = executor.executeByIndex("rc-trip-2", 700001, 1, 1001);
    require(again.success && port->countControl(0x1C) == 1, "already open: nothing sent");
    const auto close = executor.executeByIndex("rc-close", 700002, 1, 1002);
    require(close.success && close.verifyPassed && port->meter.breakerClosed() && port->meter.lastRemoteControl() == 0x1C,
            "remote close (N1=1CH) confirmed: " + close.message);
    const auto keep = executor.executeByIndex("rc-keep", 700003, 1, 1003);
    require(keep.success && !keep.verifyAttempted && port->meter.lastRemoteControl() == 0x3A, "keep power (3AH) without read-back");
    require(!executor.executeByIndex("rc-bad", 700003, 0, 1004).success, "remote control is triggered with 1 only");

    port->acknowledgeWrites = false;
    const auto lost = executor.executeByIndex("rc-lost", 700001, 1, 1005);
    require(!lost.success && lost.outcomeUnknown, "lost 9CH: outcome unknown: " + lost.message);
}

void remoteControlRejectionIsReported() {
    MemoryPointStore::cleanupOrphanedSegment(kStore);
    MemoryPointStore store(kStore);
    auto port = std::make_shared<dlt645_test::ScriptPort>();
    port->onWrite = [](const std::vector<std::uint8_t>& request) {
        if (dlt645_test::requestControl(request) == 0x11) {
            return std::vector<std::vector<std::uint8_t>>{dlt645_test::readReply("7", "04000503", {0x00, 0x00})};
        }
        return std::vector<std::vector<std::uint8_t>>{dlt645_test::replyFrame("7", 0xDC, {0x04})};
    };
    auto client = std::make_shared<Dlt645Client>(port, options());
    Dlt645CommandExecutor executor(meterWithRelay(), store, client);
    const auto result = executor.executeByIndex("rc-denied", 700001, 1, 1000);
    require(!result.success && !result.outcomeUnknown &&
            result.message.find("remote control rejected ERR=0x04 (password error/unauthorized)") != std::string::npos,
            "DCH must be a definite rejection with the decoded error word: " + result.message);
}

void dlt1997WriteHasNoOperatorCode() {
    MemoryPointStore::cleanupOrphanedSegment(kStore);
    MemoryPointStore store(kStore);
    auto port = std::make_shared<MeterPort>();
    auto client = std::make_shared<Dlt645Client>(port, options(), Dlt645Version::V1997);
    DeviceConfig config;
    config.meterCode = "OLD_METER";
    config.address = "000000000003";
    config.protocol.type = "dlt645_1997";
    config.protocol.dlt645.write.enabled = true;
    config.protocol.dlt645.write.password = dlt645_test::fixturePassword();
    PointDefinition setting;
    setting.index = 300001;
    setting.pointCode = "setting";
    setting.enabled = true;
    setting.write.enable = true;
    setting.write.dlt645.di = "C117";
    setting.write.dlt645.dataType = "dlt645_bcd";
    setting.write.dlt645.byteCount = 1;
    config.points.push_back(setting);
    Dlt645CommandExecutor executor(config, store, client);
    const auto result = executor.executeByIndex("w97", 300001, 15, 1000);
    require(result.success, "1997 write answered with 84H: " + result.message);
    require(port->count("C117", 0x04) == 1, "C=04H with the 2-byte DI");
}

}  // namespace

int main() {
    try {
        tripAndCloseAreConfirmedByReadBack();
        gatesRejectWithoutSending();
        lostReplyIsOutcomeUnknown();
        readBackMismatchFails();
        readBackFromAnotherDevice();
        remoteControlUsesC1CAndReadBack();
        remoteControlRejectionIsReported();
        dlt1997WriteHasNoOperatorCode();
        MemoryPointStore::cleanupOrphanedSegment(kStore);
        std::cout << "dlt645_command_executor_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        MemoryPointStore::cleanupOrphanedSegment(kStore);
        std::cerr << "dlt645_command_executor_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
