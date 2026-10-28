// driver-review: Modbus RTU link timing over a virtual RS-485 line.
// - a reply that arrives after timeoutMs is absorbed before the bus is reused (RTU has no transaction id:
//   it used to be taken as the reply of the next request with the same slave/function/count)
// - requestReadyInMs() exposes a slave's frameIntervalMs so the collector can serve other slaves
// - above 19200 baud the inter-frame silence is the fixed 1.75 ms
// - failures after the request reached the line carry requestSent
#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "edge_gateway/modbus_error.hpp"
#include "edge_gateway/modbus_rtu_client.hpp"
#include "virtual_rtu_line.hpp"

using namespace edge_gateway;
using virtual_rtu::Clock;
using virtual_rtu::VirtualLine;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

SerialPortOptions options(int timeoutMs, int frameIntervalMs, int baud = 9600) {
    SerialPortOptions o;
    o.device = "virtual";
    o.baudRate = baud;
    o.timeoutMs = timeoutMs;
    o.frameIntervalMs = frameIntervalMs;
    o.readRetryCount = 0;
    return o;
}

void verifyLateReplyIsNotTakenByTheNextRequest() {
    auto line = std::make_shared<VirtualLine>();
    // start 0 answers after 230 ms, beyond timeoutMs 200 (the harness case of the review).
    line->latency = [](int start) { return start == 0 ? 230 : 20; };
    ModbusRtuClient client(line, options(200, 0));
    const auto first = client.readHoldingRegisters(1, 0, 3);
    require(first == std::vector<std::uint16_t>({0, 1, 2}), "the late reply belongs to the request that caused it");
    for (const int start : {100, 200, 300}) {
        const auto values = client.readHoldingRegisters(1, start, 3);
        require(values.front() == start, "read " + std::to_string(start) + " returned registers of another request");
    }
}

void verifyNoReplyIsTimeoutWithRequestSent() {
    auto line = std::make_shared<VirtualLine>();
    line->alive = {1};
    ModbusRtuClient client(line, options(50, 0));
    try {
        client.writeSingleRegister(7, 10, 99);
        require(false, "a dead slave must time out");
    } catch (const ModbusError& error) {
        require(error.kind() == ModbusFailureKind::Timeout, "no reply must be a timeout");
        require(error.requestSent(), "the request reached the line: requestSent must be set");
        require(isWriteOutcomeUnknown(error), "a timed-out write has an unknown outcome");
    }
    line->exceptionSlaves = {1};
    try {
        client.writeSingleRegister(1, 10, 99);
        require(false, "an exception reply must throw");
    } catch (const ModbusError& error) {
        require(error.kind() == ModbusFailureKind::ProtocolException, "exception reply kind");
        require(!isWriteOutcomeUnknown(error), "an exception reply is a definite refusal");
    }
}

void verifyFrameIntervalIsExposedPerSlave() {
    auto line = std::make_shared<VirtualLine>();
    ModbusRtuClient client(line, options(200, 150));
    (void)client.readHoldingRegisters(1, 0, 1);
    const auto busy = client.requestReadyInMs(1);
    require(busy > 60 && busy <= 150, "slave 1 must report its remaining frame interval, got " + std::to_string(busy));
    require(client.requestReadyInMs(2) == 0, "another slave must be ready at once");
    (void)client.readHoldingRegisters(2, 0, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(160));
    require(client.requestReadyInMs(1) == 0, "slave 1 must be ready after its frame interval");
}

void verifyFastBaudUsesFixedSilence() {
    auto line = std::make_shared<VirtualLine>();
    line->charMs = 10.0 / 115200 * 1000;
    line->latency = [](int) { return 1; };
    ModbusRtuClient client(line, options(100, 0, 115200));
    for (int i = 0; i < 4; ++i) {
        (void)client.readHoldingRegisters(1 + (i % 2), i, 1);
    }
    for (std::size_t i = 1; i < line->writeStarts.size(); ++i) {
        const auto silenceUs = std::chrono::duration_cast<std::chrono::microseconds>(
            line->writeStarts[i] - line->replyEnds[i - 1]).count();
        require(silenceUs >= 1700, "inter-frame silence above 19200 baud must be at least 1.75 ms, got " +
                std::to_string(silenceUs) + " us");
    }
}

}  // namespace

int main() {
    try {
        verifyLateReplyIsNotTakenByTheNextRequest();
        verifyNoReplyIsTimeoutWithRequestSent();
        verifyFrameIntervalIsExposedPerSlave();
        verifyFastBaudUsesFixedSilence();
    } catch (const std::exception& ex) {
        std::cerr << "modbus_rtu_link_test failed: " << ex.what() << std::endl;
        return 1;
    }
    std::cout << "modbus_rtu_link_test passed" << std::endl;
    return 0;
}
