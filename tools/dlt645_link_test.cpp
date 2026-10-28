// DL/T 645 client regressions: RS-485 local echo, bus-wide quiet time (frameIntervalMs), receive
// resynchronisation, late/foreign replies, B1H follow-up frames, D1H errors and write outcome.
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "dlt645_test_support.hpp"
#include "edge_gateway/dlt645_client.hpp"
#include "edge_gateway/dlt645_codec.hpp"

using dlt645_test::readReply;
using dlt645_test::replyFrame;
using dlt645_test::require;
using dlt645_test::ScriptPort;
using edge_gateway::Dlt645Client;
using edge_gateway::Dlt645ErrorResponse;
using edge_gateway::Dlt645OutcomeUnknownError;
using edge_gateway::SerialPortOptions;
using Chunks = std::vector<std::vector<std::uint8_t>>;

namespace {

SerialPortOptions options(int timeoutMs = 40) {
    SerialPortOptions o;
    o.device = "test";
    o.timeoutMs = timeoutMs;
    return o;
}

std::vector<std::uint8_t> ack(const std::string& address) {
    return replyFrame(address, 0x94, {});
}

const std::vector<std::uint8_t> kVoltage = {0x01, 0x22};  // 220.1 V

void localEcho() {
    auto port = std::make_shared<ScriptPort>();
    port->onWrite = [](const std::vector<std::uint8_t>& request) {
        if (dlt645_test::requestControl(request) == 0x14) return Chunks{request, ack("1")};
        return Chunks{request, readReply("1", "02010100", kVoltage)};
    };
    auto o = options();
    o.wakeupBytes = 4;
    o.localEcho = true;
    Dlt645Client client(port, o);
    require(client.readData("1", "02010100") == kVoltage, "echoed request must be consumed before the reply");
    client.writeData("1", "06010101", dlt645_test::fixturePassword(), "01020304", {0x00, 0x02});

    // Echo that differs from what was sent (collision): read fails, write is reported unknown.
    port->onWrite = [](const std::vector<std::uint8_t>& request) {
        auto echo = request;
        echo.back() ^= 0x01;
        return Chunks{echo, ack("1")};
    };
    bool mismatch = false;
    try {
        client.readData("1", "02010100");
    } catch (const std::runtime_error& ex) {
        mismatch = std::string(ex.what()).find("local echo mismatch") != std::string::npos;
    }
    require(mismatch, "echo mismatch must be reported");
    bool unknown = false;
    try {
        client.writeData("1", "06010101", dlt645_test::fixturePassword(), "01020304", {0x00, 0x02});
    } catch (const Dlt645OutcomeUnknownError&) {
        unknown = true;
    }
    require(unknown, "write after a corrupted echo has an unknown outcome");
}

void busQuietTime() {
    auto port = std::make_shared<ScriptPort>();
    port->onWrite = [](const std::vector<std::uint8_t>& request) {
        const auto address = dlt645_test::requestAddress(request);
        return Chunks{readReply(address, "02010100", kVoltage)};
    };
    auto o = options(200);
    o.frameIntervalMs = 120;
    Dlt645Client client(port, o);
    std::vector<std::chrono::steady_clock::time_point> replyDone;
    for (const auto* address : {"20", "21", "20", "22"}) {
        client.readData(address, "02010100");
        replyDone.push_back(port->lastDelivery);
    }
    for (std::size_t i = 1; i < port->writeTimes.size(); ++i) {
        const auto quietMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            port->writeTimes[i] - replyDone[i - 1]).count();
        require(quietMs >= 115, "bus must stay quiet frameIntervalMs after the previous reply (any meter), got " +
                                std::to_string(quietMs) + " ms");
    }

    // After a timeout the quiet time also applies.
    port->onWrite = [](const std::vector<std::uint8_t>&) { return Chunks{}; };
    auto silent = options(30);
    silent.frameIntervalMs = 80;
    Dlt645Client slow(port, silent);
    try { slow.readData("20", "02010100"); } catch (const std::exception&) {}
    const auto timedOutAt = std::chrono::steady_clock::now();
    try { slow.readData("21", "02010100"); } catch (const std::exception&) {}
    const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(port->writeTimes.back() - timedOutAt).count();
    require(gap >= 75, "quiet time must follow a timeout too");
}

void resyncAndLateReplies() {
    auto port = std::make_shared<ScriptPort>();
    const auto good = readReply("1", "02010100", kVoltage);
    port->onWrite = [&good](const std::vector<std::uint8_t>&) {
        return Chunks{std::vector<std::uint8_t>(good.begin(), good.begin() + 12), good};
    };
    Dlt645Client client(port, options());
    require(client.readData("1", "02010100") == kVoltage, "client must resynchronise after a truncated frame");

    // Late reply of another meter, then a late reply of an earlier DI, then the real reply.
    port->onWrite = [](const std::vector<std::uint8_t>&) {
        return Chunks{readReply("2", "02010100", {0x99, 0x22}),
                      readReply("1", "02010200", {0x55, 0x22}),
                      readReply("1", "02010100", kVoltage)};
    };
    require(client.readData("1", "02010100") == kVoltage, "foreign and stale replies must be skipped, never accepted");

    // Only a foreign reply: still rejected with the address check.
    port->onWrite = [](const std::vector<std::uint8_t>&) { return Chunks{readReply("2", "02010100", kVoltage)}; };
    bool rejected = false;
    try {
        client.readData("1", "02010100");
    } catch (const std::runtime_error& ex) {
        rejected = std::string(ex.what()).find("address mismatch") != std::string::npos;
    }
    require(rejected, "a reply from another address must never be accepted");
}

void followUpFrames() {
    auto port = std::make_shared<ScriptPort>();
    port->onWrite = [](const std::vector<std::uint8_t>& request) {
        const auto control = dlt645_test::requestControl(request);
        if (control == 0x11) return Chunks{readReply("1", "04001504", {0x01, 0x02}, 0xB1)};
        const auto start = dlt645_test::frameStart(request);
        const auto sequence = static_cast<std::uint8_t>(*(start + 14) - 0x33U);
        if (sequence == 1) return Chunks{readReply("1", "04001504", {0x03, 0x04, 0x01}, 0xB2)};
        return Chunks{readReply("1", "04001504", {0x05, sequence}, 0x92)};
    };
    Dlt645Client client(port, options());
    const auto data = client.readData("1", "04001504");
    require(data == std::vector<std::uint8_t>({0x01, 0x02, 0x03, 0x04, 0x05}), "follow-up data must be appended");
    require(port->writes.size() == 3, "B1H must trigger C=12H follow-up requests until 92H");
    require(dlt645_test::requestControl(port->writes[1]) == 0x12 && dlt645_test::requestControl(port->writes[2]) == 0x12,
            "follow-up requests use C=12H");
}

void errorReply() {
    auto port = std::make_shared<ScriptPort>();
    port->onWrite = [](const std::vector<std::uint8_t>&) { return Chunks{replyFrame("1", 0xD1, {0x02})}; };
    Dlt645Client client(port, options());
    bool reported = false;
    try {
        client.readData("1", "02070000");
    } catch (const Dlt645ErrorResponse& ex) {
        reported = ex.errorCode() == 0x02 && std::string(ex.what()).find("code=0x02") != std::string::npos;
    }
    require(reported, "D1H must be reported with its error code");
}

void writeOutcome() {
    auto port = std::make_shared<ScriptPort>();
    port->onWrite = [](const std::vector<std::uint8_t>&) { return Chunks{}; };
    Dlt645Client client(port, options(30));
    bool unknown = false;
    try {
        client.writeData("1", "06010101", dlt645_test::fixturePassword(), "01020304", {0x00, 0x02});
    } catch (const Dlt645OutcomeUnknownError& ex) {
        unknown = std::string(ex.what()).find("timeout") != std::string::npos;
    }
    require(unknown, "write timeout must be an unknown outcome");
    require(port->writes.size() == 1, "writes are never retried");

    port->onWrite = [](const std::vector<std::uint8_t>&) { return Chunks{replyFrame("1", 0xD4, {0x04})}; };
    bool rejected = false;
    try {
        client.writeData("1", "06010101", dlt645_test::fixturePassword(), "01020304", {0x00, 0x02});
    } catch (const Dlt645OutcomeUnknownError&) {
        rejected = false;
    } catch (const std::runtime_error& ex) {
        rejected = std::string(ex.what()).find("ERR=0x04") != std::string::npos;
    }
    require(rejected, "D4H is a definite rejection, not an unknown outcome");

    // Encoding error: nothing was sent, so the outcome is known.
    bool notSent = false;
    try {
        client.writeData("1", "0601", dlt645_test::fixturePassword(), "01020304", {});
    } catch (const Dlt645OutcomeUnknownError&) {
        notSent = false;
    } catch (const std::exception&) {
        notSent = true;
    }
    require(notSent && port->writes.size() == 2, "invalid request must fail before reaching the bus");
}

}  // namespace

int main() {
    try {
        localEcho();
        busQuietTime();
        resyncAndLateReplies();
        followUpFrames();
        errorReply();
        writeOutcome();
        std::cout << "dlt645_link_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "dlt645_link_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
