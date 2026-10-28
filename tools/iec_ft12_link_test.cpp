// Regression tests for the shared unbalanced FT1.2 master (IEC 101, IEC 103, AM5SE):
// link start-up, FCB handling, retransmission, ACD-driven class 1 polling, general/counter
// interrogation and the "E5 is not an acquisition" rule. The secondary station is simulated
// in-process behind the ISerialPort interface; no device is contacted.
#include "edge_gateway/iec_ft12_client.hpp"
#include "edge_gateway/iec_ft12_link.hpp"
#include "iec_ft12_test_station.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace edge_gateway;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

using ft12_test::Ft12TestStation;
using ft12_test::SerialStation;

IecProtocolConfig iec101Config() {
    IecProtocolConfig config;
    config.transportMode = "serial";
    config.linkAddress = 1;
    config.linkAddressSize = 1;
    config.commonAddress = 1;
    config.cotSize = 2;
    config.caSize = 2;
    config.ioaSize = 3;
    config.pollTimeoutMs = 25;
    config.maxPollFrames = 16;
    return config;
}

IecProtocolConfig iec103Config() {
    auto config = iec101Config();
    config.cotSize = 1;
    config.caSize = 1;
    config.returnInformationIdentifier = 0xE9;
    config.deviceFunctionType = 1;
    return config;
}

// IEC 101 station: answers GI and CI with ACT_CON, data and ACT_TERM in class 1.
void answerIec101Interrogations(const std::vector<std::uint8_t>& asdu, Ft12TestStation& station) {
    if (asdu.empty()) return;
    if (asdu[0] == 100) {
        require(asdu.size() == 10 && asdu[2] == 6 && asdu[9] == 20, "IEC101 GI must be C_IC_NA_1 COT 6 QOI 20");
        station.class1.push_back({100, 1, 7, 0, 1, 0, 0, 0, 0, 20});
        station.class1.push_back({1, 1, 20, 0, 1, 0, 0xE9, 0x03, 0x00, 0x01});
        station.class1.push_back({100, 1, 10, 0, 1, 0, 0, 0, 0, 20});
    } else if (asdu[0] == 101) {
        require(asdu.back() == 5, "IEC101 CI QCC must request all counters without freeze");
        station.class1.push_back({101, 1, 7, 0, 1, 0, 0, 0, 0, 5});
        station.class1.push_back({15, 1, 37, 0, 1, 0, 0x01, 0x10, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00});
        station.class1.push_back({101, 1, 10, 0, 1, 0, 0, 0, 0, 5});
    }
}

bool hasValue(const std::vector<IecDataValue>& values, int ioa, double value) {
    return std::any_of(values.begin(), values.end(), [&](const IecDataValue& candidate) {
        return candidate.ioa == ioa && candidate.value == value;
    });
}

// Finding 1: link start-up, GI COT 6, FCB alternation, ACD -> class 1, ACT_TERM, CI.
void verifyIec101StartupAndInterrogation() {
    auto port = std::make_shared<SerialStation>();
    auto* station = &port->station;
    station->onUserData = answerIec101Interrogations;
    IecFt12Client client("iec101", port, iec101Config());

    const auto first = client.poll();
    require(station->requests.size() >= 4, "IEC101 start-up exchanges");
    require(station->requests[0].control == 0x49, "first frame is request status of link (FC 9)");
    require(station->requests[1].control == 0x40, "then reset of remote link (FC 0)");
    require(station->requests[2].variable && station->requests[2].control == 0x73,
        "GI is SEND/CONFIRM with FCB=1 after the reset");
    require(hasValue(first, 1001, 1.0), "GI data from class 1 must be returned");

    const auto second = client.poll();
    require(station->countUserData(101) == 1, "counter interrogation follows the terminated GI");
    require(hasValue(second, 4097, 100.0), "integrated total from the counter interrogation");
    require(station->countUserData(100) == 1, "a terminated GI is not repeated within its period");

    int expected = 1;
    for (const auto& request : station->requests) {
        if ((request.control & 0x10) == 0) {
            continue;
        }
        require(((request.control & 0x20) != 0 ? 1 : 0) == expected, "FCB must alternate for every new FCV=1 request");
        expected ^= 1;
    }
    require(station->repeats == 0, "no repetitions without reply loss");
}

// counterInterrogation=false: an IEC 101 substation without C_CI_NA_1 support is never sent one.
void verifyIec101CounterInterrogationDisabled() {
    auto port = std::make_shared<SerialStation>();
    auto* station = &port->station;
    station->onUserData = answerIec101Interrogations;
    auto config = iec101Config();
    config.counterInterrogation = false;
    IecFt12Client client("iec101", port, config);
    const auto first = client.poll();
    (void)client.poll();
    (void)client.poll();
    require(hasValue(first, 1001, 1.0), "GI data still collected");
    require(station->countUserData(101) == 0, "counterInterrogation=false: no counter interrogation is sent");
}

// Finding 1: a station that only answers E5 is not an acquisition (old code: online, no data).
void verifyE5OnlyIsNotSuccess() {
    auto port = std::make_shared<SerialStation>();
    auto* station = &port->station;
    station->e5Only = true;
    auto config = iec101Config();
    config.maxPollFrames = 4;
    IecFt12Client client("iec101", port, config);
    bool failed = false;
    try {
        (void)client.poll();
    } catch (const std::runtime_error& ex) {
        failed = std::string(ex.what()).find("no application data") != std::string::npos;
    }
    require(failed, "E5-only replies must fail the poll");
    require(station->countUserData(100) == 1, "the GI was sent once");
}

// Same-FCB retransmission, link reset after repeated loss, and GI again after the reset.
void verifyRetransmissionAndRecovery() {
    auto port = std::make_shared<SerialStation>();
    auto* station = &port->station;
    station->onUserData = answerIec101Interrogations;
    IecFt12Client client("iec101", port, iec101Config());
    (void)client.poll();
    (void)client.poll();

    const auto before = station->requests.size();
    station->dropRequests = 1;
    (void)client.poll();
    require(station->requests.size() >= before + 2 &&
        station->requests[before].raw == station->requests[before + 1].raw,
        "a lost reply is recovered by repeating the identical frame (same FCB)");

    station->dropRequests = 3;
    bool linkFailed = false;
    try {
        (void)client.poll();
    } catch (const Ft12LinkError&) {
        linkFailed = true;
    }
    require(linkFailed, "three unanswered attempts declare the link down");
    const auto values = client.poll();
    require(station->countUserData(100) == 2, "a reset link starts over with a general interrogation");
    require(hasValue(values, 1001, 1.0), "GI data after the link reset");
}

std::vector<std::uint8_t> asdu44(std::uint16_t states) {
    return {0x2C, 0x01, 0x14, 0x01, 0x01, 0x64,
        static_cast<std::uint8_t>(states & 0xFF), static_cast<std::uint8_t>(states >> 8), 0x00, 0x00, 0x00, 0xE9};
}

std::string onBits(const std::vector<IecDataValue>& values) {
    std::string text;
    for (const auto& value : values) {
        if (value.typeId == 44 && value.value > 0.0) {
            text += "INF" + std::to_string(value.informationNumber) + " ";
        }
    }
    return text;
}

// IEC 103 station: GI (ASDU 7, COT 9) answered by ASDU 44 and ASDU 8 termination.
void answerIec103Interrogation(const std::vector<std::uint8_t>& asdu, Ft12TestStation& station) {
    if (!asdu.empty() && asdu[0] == 7) {
        require(asdu.size() == 7 && asdu[2] == 9 && asdu[6] == 0xE9, "IEC103 GI must be ASDU 7, COT 9, SCN");
        station.class1.push_back(asdu44(0x0000));
        station.class1.push_back({8, 0x81, 10, 1, 0xFF, 0x00, 0xE9});
    }
}

// Finding 1 (IEC 103): reset CU only, GI with COT 9, ASDU 8 ends the GI, ASDU 9 in class 2.
void verifyIec103Session() {
    auto port = std::make_shared<SerialStation>();
    auto* station = &port->station;
    station->onUserData = answerIec103Interrogation;
    IecFt12Client client("iec103", port, iec103Config());
    (void)client.poll();
    require(station->requests[0].control == 0x40, "IEC103 starts with reset CU (no status request)");
    require(station->countUserData(7) == 1, "IEC103 GI sent once");

    station->class2.push_back({9, 0x83, 2, 1, 160, 148, 0x00, 0x20, 0x00, 0x40, 0x01, 0x00});
    const auto values = client.poll();
    require(values.size() == 3 && values[1].elementIndex == 1, "ASDU 9 decoded per measurand");
    require(values[0].value == 0.25 && values[1].value == 0.5 && values[2].quality == 0, "ASDU 9 MEA values and OV");
    require(station->countUserData(7) == 1, "ASDU 8 terminated the GI: no repetition");
}

// Finding 6: a reply that arrives after the response timeout must not shift every later
// poll by one (the station repeats it for the retransmission with the same FCB).
void verifyLateReplyDoesNotShiftPolls() {
    auto port = std::make_shared<SerialStation>();
    auto* station = &port->station;
    station->onUserData = answerIec103Interrogation;
    IecFt12Client client("iec103", port, iec103Config());
    (void)client.poll();

    station->class2.push_back(asdu44(0x0001));
    station->class2.push_back(asdu44(0x0002));
    station->class2.push_back(asdu44(0x0004));
    // A is answered after 40 ms (> 25 ms timeout); the repeated A for the retransmission
    // arrives 40 ms later, i.e. after the next request has been sent.
    station->replyDelaysMs = {40, 40};
    require(onBits(client.poll()) == "INF100 ", "poll 1 returns A");
    require(onBits(client.poll()) == "INF101 ", "poll 2 returns B, not the late duplicate of A");
    require(onBits(client.poll()) == "INF102 ", "poll 3 returns C");
}

// Finding 6: replies for another link address are ignored, not taken as the answer.
void verifyForeignAddressIgnored() {
    auto port = std::make_shared<SerialStation>();
    auto* station = &port->station;
    station->onUserData = answerIec103Interrogation;
    IecFt12Client client("iec103", port, iec103Config());
    (void)client.poll();
    station->class2.push_back(asdu44(0x0008));
    station->wrongAddressReplies = 1;
    const auto before = station->requests.size();
    require(onBits(client.poll()) == "INF103 ", "reply for our address accepted");
    require(station->requests.size() == before + 1, "the foreign reply did not cause a retransmission");
}

// DFC=1: no user data (GI) is sent until the station clears DFC.
void verifyDataFlowControl() {
    auto port = std::make_shared<SerialStation>();
    auto* station = &port->station;
    station->onUserData = answerIec103Interrogation;
    station->dfcReplies = 1;
    station->class2.push_back(asdu44(0x0001));
    IecFt12Client client("iec103", port, iec103Config());
    (void)client.poll();
    require(station->countUserData(7) == 0, "GI held back while DFC=1");
    (void)client.poll();
    require(station->countUserData(7) == 1, "GI sent once DFC is clear");
}

}  // namespace

int main() {
    try {
        verifyIec101StartupAndInterrogation();
        verifyIec101CounterInterrogationDisabled();
        verifyE5OnlyIsNotSuccess();
        verifyRetransmissionAndRecovery();
        verifyIec103Session();
        verifyLateReplyDoesNotShiftPolls();
        verifyForeignAddressIgnored();
        verifyDataFlowControl();
        std::cout << "IEC FT1.2 link tests passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "IEC FT1.2 link test failed: " << ex.what() << std::endl;
        return 1;
    }
}
