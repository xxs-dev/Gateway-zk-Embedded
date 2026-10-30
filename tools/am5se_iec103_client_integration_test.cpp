// AM5SE IEC 103 network client against a scripted FT1.2 secondary on loopback TCP, in both
// connection modes: GI, recording directory and COMTRADE transfer, ASDU 1 DPI changes, late
// replies, events during a recording transfer and GI after a link reset.
#include "edge_gateway/am5se_iec103_client.hpp"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "iec_ft12_test_station.hpp"

namespace {

using namespace edge_gateway;
using ft12_test::Ft12TestStation;
using ft12_test::TcpStation;

void requireTrue(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string temporaryDirectory(const std::string& suffix) {
    const char* base = std::getenv("TMPDIR");
    std::string path = base != nullptr && *base != '\0' ? base : "/tmp";
    if (path.back() != '/') path.push_back('/');
    path += "am5se-iec103-test-" + std::to_string(getpid()) + "-" + suffix;
    mkdir(path.c_str(), 0775);
    return path;
}

bool fileExists(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    return input.good();
}

void removeTestOutput(const std::string& directory) {
    std::remove((directory + "/FAN00015.CFG").c_str());
    std::remove((directory + "/FAN00015.DAT").c_str());
    rmdir(directory.c_str());
}

int availablePort() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        throw std::runtime_error("cannot bind test socket");
    }
    socklen_t length = sizeof(address);
    getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
    close(fd);
    return ntohs(address.sin_port);
}

std::vector<std::uint8_t> asdu44(std::uint16_t states) {
    return {0x2C, 0x01, 0x09, 0x01, 0x01, 0x64,
        static_cast<std::uint8_t>(states & 0xFF), static_cast<std::uint8_t>(states >> 8), 0x00, 0x00, 0x00, 0xE9};
}

// ASDU 1 (time-tagged message): FUN 1, INF 100, DPI, CP32Time2a, SIN.
std::vector<std::uint8_t> asdu1(int dpi) {
    return {0x01, 0x81, 0x01, 0x01, 0x01, 0x64, static_cast<std::uint8_t>(dpi), 0x10, 0x27, 0x05, 0x0A, 0x00};
}

std::vector<std::uint8_t> comtradePacket(int fileType, int packet, bool last, std::vector<std::uint8_t> data) {
    std::vector<std::uint8_t> asdu = {0x50, 0x81, 0x14, 0x01, 0x01, static_cast<std::uint8_t>(fileType),
        static_cast<std::uint8_t>(packet & 0xFF),
        static_cast<std::uint8_t>(((packet >> 8) & 0x7F) | (last ? 0x80 : 0x00))};
    asdu.insert(asdu.end(), data.begin(), data.end());
    return asdu;
}

struct Am5seBehaviour {
    int datPackets = 1;
    std::vector<std::vector<std::uint8_t>> eventsDuringDat;

    void operator()(const std::vector<std::uint8_t>& asdu, Ft12TestStation& station) const {
        if (asdu.empty()) return;
        if (asdu[0] == 0x07) {
            requireTrue(asdu[2] == 9, "AM5SE GI must use COT 9");
            station.class1.push_back(asdu44(0x0001));
            station.class1.push_back({0x08, 0x81, 0x0A, 0x01, 0xFF, 0x00, 0xE9});
        } else if (asdu[0] == 0x18) {
            station.class1.push_back({0x17, 0x01, 0x1F, 0x01, 0xFF, 0x00,
                0x0F, 0x00, 0x01, 0x00, 0x00, 0x01, 0x02, 0x01, 0x01, 0x1A});
        } else if (asdu[0] == 0x44 && asdu[5] == 0x51) {
            station.class1.push_back(comtradePacket(0x51, 0, true, {'C', 'F', 'G'}));
        } else if (asdu[0] == 0x44 && asdu[5] == 0x52) {
            for (const auto& event : eventsDuringDat) {
                station.class1.push_back(event);
            }
            for (int packet = 0; packet < datPackets; ++packet) {
                station.class1.push_back(comtradePacket(0x52, packet, packet == datPackets - 1,
                    {static_cast<std::uint8_t>(packet), 0x02, 0x03}));
            }
        }
    }
};

IecProtocolConfig baseConfig() {
    IecProtocolConfig config;
    config.linkAddress = 1;
    config.commonAddress = 1;
    config.linkAddressSize = 1;
    config.deviceFunctionType = 1;
    config.pollTimeoutMs = 100;
    config.t0Ms = 3000;
    config.maxPollFrames = 8;
    config.recordingMaxFileBytes = 1024;
    return config;
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

void verifyRoundTrip(Am5seIec103Client& client, Ft12TestStation& station, const std::string& output, std::size_t datBytes) {
    const auto values = client.poll();
    requireTrue(values.size() == 16, "general interrogation values");
    requireTrue(values.front().informationNumber == 100 && values.front().value == 1.0, "ASDU44 value");
    requireTrue(station.countUserData(0x07) == 1, "one GI after the link reset");

    const auto records = client.listDisturbanceRecords(2000);
    requireTrue(records.size() == 1 && records.front().fan == 15, "recording directory");

    const auto files = client.pullComtradeRecording(15, output, 3000);
    requireTrue(files.cfgBytes == 3 && files.datBytes == datBytes, "COMTRADE sizes");
    requireTrue(fileExists(files.cfgPath) && fileExists(files.datPath), "COMTRADE files");
}

void verifyPassiveMode() {
    Ft12TestStation station;
    Am5seBehaviour behaviour;
    behaviour.datPackets = 12;  // more class 1 requests than maxPollFrames
    behaviour.eventsDuringDat = {asdu1(2), asdu1(1)};
    station.onUserData = behaviour;
    const int port = availablePort();
    TcpStation device(station);
    device.connectTo(port);
    const auto output = temporaryDirectory("passive");
    removeTestOutput(output);
    try {
        auto config = baseConfig();
        config.transportMode = "am5se_passive_tcp";
        config.listenAddress = "127.0.0.1";
        config.listenPort = port;
        config.udpBindAddress = "127.0.0.1";
        config.udpBroadcastAddress = "127.0.0.1";
        config.udpPort = port + 1;
        Am5seIec103Client client(config);
        std::vector<Iec103SoeEvent> soe;
        client.setSoeSink([&](const std::vector<Iec103SoeEvent>& events) {
            soe.insert(soe.end(), events.begin(), events.end());
        });
        verifyRoundTrip(client, station, output, 36);
        requireTrue(soe.size() == 2 && soe[0].dpi == 2 && soe[1].dpi == 1 &&
            soe[0].eventId != soe[1].eventId, "recording SOE action/return retained in order");
    } catch (...) {
        device.stop();
        removeTestOutput(output);
        throw;
    }
    device.stop();
    removeTestOutput(output);
}

struct ActiveFixture {
    Ft12TestStation station;
    TcpStation device{station};
    std::unique_ptr<Am5seIec103Client> client;

    explicit ActiveFixture(Am5seBehaviour behaviour, IecProtocolConfig config = baseConfig()) {
        station.onUserData = behaviour;
        const int port = device.listen();
        config.transportMode = "am5se_active_tcp";
        TcpTransportConfig tcp;
        tcp.host = "127.0.0.1";
        tcp.port = port;
        tcp.connectTimeoutMs = 1000;
        client.reset(new Am5seIec103Client(config, tcp));
    }

    ~ActiveFixture() {
        client.reset();
        device.stop();
    }
};

void verifyActiveMode() {
    ActiveFixture fixture{Am5seBehaviour()};
    const auto output = temporaryDirectory("active");
    removeTestOutput(output);
    try {
        verifyRoundTrip(*fixture.client, fixture.station, output, 3);
    } catch (...) {
        removeTestOutput(output);
        throw;
    }
    removeTestOutput(output);
}

// Finding 3: the code's adopted mapping of ASDU 1 DPI on a boolean point (1 -> 0, 2 -> 1, 0/3 bad
// quality). It tests the code, not the AM5SE firmware semantics, which are still unverified.
void verifyDoublePointChanges() {
    ActiveFixture fixture{Am5seBehaviour()};
    (void)fixture.client->poll();
    PointDefinition breaker;
    breaker.read.enable = true;
    breaker.read.dataType = "bool";
    breaker.read.iec.functionType = 1;
    breaker.read.iec.informationNumber = 100;
    breaker.read.iec.typeId = 44;
    const int dpis[] = {2, 1, 3};
    const double expected[] = {1.0, 0.0, 0.0};
    for (int i = 0; i < 3; ++i) {
        fixture.station.pushClass1(asdu1(dpis[i]));
        const auto values = fixture.client->poll();
        requireTrue(values.size() == 1 && values[0].typeId == 1 && values[0].doublePoint, "ASDU1 change received");
        requireTrue(IecCodec::pointMatches(breaker, values[0]), "ASDU1 matches the ASDU44 signal point");
        requireTrue(IecCodec::decodePointValue(breaker, values[0]).value == expected[i],
            "DPI " + std::to_string(dpis[i]) + " mapped to " + std::to_string(expected[i]));
        requireTrue((dpis[i] == 3) == IecCodec::holdsPreviousValue(breaker, values[0]) &&
            (dpis[i] == 3) == (values[0].quality == 0), "DPI 3 holds the previous value with bad quality");
    }
}

// Finding 6: one late reply must not make every later poll return the previous answer.
void verifyLateReplyRecovers() {
    ActiveFixture fixture{Am5seBehaviour()};
    (void)fixture.client->poll();
    {
        std::lock_guard<std::mutex> lock(fixture.station.mutex());
        fixture.station.class2.push_back(asdu44(0x0002));
        fixture.station.class2.push_back(asdu44(0x0004));
        fixture.station.class2.push_back(asdu44(0x0008));
        fixture.station.replyDelaysMs = {150, 150};  // > pollTimeoutMs (100 ms)
    }
    requireTrue(onBits(fixture.client->poll()) == "INF101 ", "poll 1 returns A");
    requireTrue(onBits(fixture.client->poll()) == "INF102 ", "poll 2 returns B");
    requireTrue(onBits(fixture.client->poll()) == "INF103 ", "poll 3 returns C");
}

// Finding 7: class 1 events during a COMTRADE transfer are delivered, and poll() does not
// wait for the transfer.
void verifyEventsDuringRecording() {
    Am5seBehaviour behaviour;
    behaviour.datPackets = 10;
    behaviour.eventsDuringDat = {asdu1(2)};
    ActiveFixture fixture{behaviour};
    (void)fixture.client->poll();
    {
        std::lock_guard<std::mutex> lock(fixture.station.mutex());
        for (int i = 0; i < 40; ++i) fixture.station.replyDelaysMs.push_back(25);
    }
    const auto output = temporaryDirectory("events");
    removeTestOutput(output);
    auto transfer = std::async(std::launch::async, [&] {
        return fixture.client->pullComtradeRecording(15, output, 5000);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto pollStarted = ft12_test::steadyMs();
    (void)fixture.client->poll();
    requireTrue(ft12_test::steadyMs() - pollStarted < 80, "poll must not wait for the recording transfer");
    const auto files = transfer.get();
    removeTestOutput(output);
    requireTrue(files.datBytes == 30, "DAT transferred around the event");
    const auto events = fixture.client->drainBufferedValues();
    requireTrue(events.size() == 1 && events[0].typeId == 1 && events[0].value == 2.0,
        "the trip event received during the transfer must be delivered");
}

// Finding 7: after a link timeout the reset link starts over with a general interrogation.
void verifyGiAfterLinkReset() {
    ActiveFixture fixture{Am5seBehaviour()};
    (void)fixture.client->poll();
    {
        std::lock_guard<std::mutex> lock(fixture.station.mutex());
        fixture.station.dropRequests = 3;
    }
    bool failed = false;
    try {
        (void)fixture.client->poll();
    } catch (const std::exception&) {
        failed = true;
    }
    requireTrue(failed, "three lost replies fail the poll");
    const auto values = fixture.client->poll();
    requireTrue(fixture.station.countUserData(0x07) == 2, "GI repeated after the link reset");
    requireTrue(values.size() == 16, "GI data after the link reset");
}

void verifySoePersistenceBackpressure() {
    ActiveFixture fixture{Am5seBehaviour()};
    std::vector<Iec103SoeEvent> stored;
    bool fail = true;
    std::string pendingId;
    fixture.client->setSoeSink([&](const std::vector<Iec103SoeEvent>& events) {
        if (fail) {
            if (pendingId.empty()) pendingId = events.at(0).eventId;
            requireTrue(events.at(0).eventId == pendingId, "retry keeps SOE identity");
            throw std::runtime_error("injected storage failure");
        }
        stored.insert(stored.end(), events.begin(), events.end());
    });
    (void)fixture.client->poll();
    requireTrue(stored.empty(), "GI is not spontaneous SOE");
    fixture.station.pushClass1(asdu1(2));
    fixture.station.pushClass1(asdu1(1));
    for (int i = 0; i < 2; ++i) {
        const auto before = fixture.station.requestCount();
        bool failed = false;
        try { fixture.client->poll(); }
        catch (const std::runtime_error& e) { failed = std::string(e.what()) == "injected storage failure"; }
        requireTrue(failed, "storage failure is surfaced");
        if (i == 1) requireTrue(fixture.station.requestCount() == before,
            "no request or FCB advancement until pending SOE is committed");
    }
    fail = false;
    fixture.client->poll();
    requireTrue(stored.size() == 2 && stored[0].eventId == pendingId &&
        stored[0].dpi == 2 && stored[1].dpi == 1, "pending and next SOE committed once in order");
}

void verifyRecordingAssociation(int scenario) {
    auto config = baseConfig();
    config.soe.enabled = true;
    config.soe.recordingLinkEnabled = true;
    config.soe.recordingDirectoryTimeoutMs = 150;
    config.soe.utcOffsetMinutes = 0;
    if (scenario == 2) config.maxPollFrames = 1;
    ActiveFixture fixture{Am5seBehaviour(), config};
    fixture.client->poll();
    fixture.client->poll(); // finish GI even with a one-frame poll budget
    int directoryCalls = 0, snapshots = 0, downloads = 0;
    bool complete = false, success = true;
    std::vector<Iec103SoeEvent> soe;
    fixture.client->setSoeSink([&](const std::vector<Iec103SoeEvent>& events) {
        soe.insert(soe.end(), events.begin(), events.end());
    });
    {
        std::lock_guard<std::mutex> lock(fixture.station.mutex());
        fixture.station.onUserData = [&](const std::vector<std::uint8_t>& asdu, Ft12TestStation& station) {
            if (asdu[0] != 24) { Am5seBehaviour()(asdu, station); return; }
            ++directoryCalls;
            if (scenario == 3) { station.dropRequests = 10; return; }
            if (scenario == 4) station.address = 2;
            std::vector<std::uint8_t> directory = {23,1,31,1,255,0,15,0,6,
                0x88,0x13,0,0,static_cast<std::uint8_t>(scenario == 1 && directoryCalls == 3 ? 30 : 29),10,26};
            if (scenario == 5) directory.push_back(0);
            if (scenario == 6) directory[3] = 2;
            if (scenario == 7) directory[2] |= 0x40;
            if (scenario < 2) station.class1.push_back(asdu1(2));
            station.class1.push_back(directory);
            if (scenario == 2) station.class1.push_back(directory);
        };
    }
    fixture.client->setSoeRecordingSinks(
        [&](const std::vector<Iec103DisturbanceRecord>&, bool done, bool ok, std::int64_t) {
            ++snapshots; complete = done; success = ok;
        },
        [&](const Iec103DisturbanceRecord& r, const Iec103ComtradeFiles& files, std::int64_t) {
            requireTrue(r.fan == 15 && fileExists(files.cfgPath) && fileExists(files.datPath),
                "association callback only after complete files");
            ++downloads;
        });
    const auto start = ft12_test::steadyMs();
    bool failed = false;
    try { fixture.client->poll(); } catch (const std::exception&) { failed = true; }
    if (scenario >= 3) {
        requireTrue(failed && snapshots == 1 && !success && !complete,
            "invalid/timeout directory cannot certify an association");
        if (scenario == 3) requireTrue(ft12_test::steadyMs() - start < 450,
            "total directory deadline also bounds retries");
        return;
    }
    requireTrue(!failed && snapshots == 1 && success && complete == (scenario != 2),
        "automatic catalogue records completeness");
    if (scenario == 2) return;
    fixture.client->poll();
    requireTrue(snapshots == 1, "automatic refresh throttled");
    const auto output = temporaryDirectory("association-" + std::to_string(scenario));
    const auto files = fixture.client->pullComtradeRecording(15, output, 1000);
    std::remove(files.cfgPath.c_str());
    std::remove(files.datPath.c_str());
    rmdir(output.c_str());
    requireTrue(snapshots == 3 && downloads == (scenario == 0 ? 1 : 0),
        "before/after directory identity prevents recycled FAN association");
    requireTrue(soe.size() == 3, "SOE during each catalogue retained");
    requireTrue(files.cfgPath.find("FAN00015_") != std::string::npos, "unique recording file name");
}

}  // namespace

int main() {
    try {
        verifyPassiveMode();
        verifyActiveMode();
        verifyDoublePointChanges();
        verifyLateReplyRecovers();
        verifyEventsDuringRecording();
        verifyGiAfterLinkReset();
        verifySoePersistenceBackpressure();
        for (int scenario = 0; scenario < 8; ++scenario) verifyRecordingAssociation(scenario);
    } catch (const std::exception& ex) {
        std::cerr << "AM5SE IEC103 integration test failed: " << ex.what() << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "AM5SE IEC103 passive and active TCP integration tests passed" << std::endl;
    return EXIT_SUCCESS;
}
