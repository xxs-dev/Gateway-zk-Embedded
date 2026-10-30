// DL/T 645 protocol extensions: 1997 edition frames and client, C=1CH remote control frame,
// C=08H broadcast time sync (frame, bus lock/quiet time, scheduling rules, explicit not-sent
// reporting with status file and rate-limited log, clock trust) and the C=13H /
// wildcard address query diagnostic.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dlt645_test_support.hpp"
#include "edge_gateway/dlt645_client.hpp"
#include "edge_gateway/dlt645_codec.hpp"
#include "edge_gateway/dlt645_mock_serial_port.hpp"
#include "edge_gateway/dlt645_time_sync.hpp"
#include "edge_gateway/filesystem_compat.hpp"
#include "edge_gateway/json_value.hpp"

#ifndef _WIN32
#include <sys/timex.h>
#endif

using dlt645_test::checksum;
using dlt645_test::require;
using dlt645_test::ScriptPort;
using namespace edge_gateway;
using Chunks = std::vector<std::vector<std::uint8_t>>;

namespace {

std::vector<std::uint8_t> withChecksum(std::vector<std::uint8_t> frame) {
    frame.push_back(checksum(frame));
    frame.push_back(0x16);
    return frame;
}

bool throwsWith(const std::function<void()>& action, const std::string& text) {
    try {
        action();
    } catch (const std::exception& ex) {
        if (std::string(ex.what()).find(text) != std::string::npos) return true;
        std::cerr << "unexpected error text: " << ex.what() << std::endl;
    }
    return false;
}

SerialPortOptions options(int timeoutMs = 20) {
    SerialPortOptions o;
    o.device = "test";
    o.timeoutMs = timeoutMs;
    return o;
}

// Concatenation of byte runs (frame prefix, password on the wire, rest).
std::vector<std::uint8_t> join(std::initializer_list<std::vector<std::uint8_t>> parts) {
    std::vector<std::uint8_t> out;
    for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
}

// The per-run fixture password as sent: PA P0 P1 P2 in string order, each +33H.
std::vector<std::uint8_t> passwordOnWire() {
    const auto hex = dlt645_test::fixturePassword();
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        bytes.push_back(static_cast<std::uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16) + 0x33));
    }
    return bytes;
}

std::tm sampleTime() {
    std::tm time{};
    time.tm_year = 2026 - 1900;
    time.tm_mon = 9;  // October
    time.tm_mday = 28;
    time.tm_hour = 14;
    time.tm_min = 5;
    time.tm_sec = 9;
    return time;
}

void frames1997() {
    // Read 9010 (forward active energy): C=01H, L=02H, DI0 DI1 = 10 90 (+33H).
    require(Dlt645Codec::buildReadFrame("1", "9010", Dlt645Version::V1997) ==
                withChecksum({0x68, 0x01, 0, 0, 0, 0, 0, 0x68, 0x01, 0x02, 0x43, 0xC3}),
            "1997 read frame");
    require(Dlt645Codec::buildReadFollowUpFrame("1", "901F", 1, Dlt645Version::V1997) ==
                withChecksum({0x68, 0x01, 0, 0, 0, 0, 0, 0x68, 0x02, 0x02, 0x52, 0xC3}),
            "1997 follow-up frame carries the DI only");
    // Write C=04H: DI(2) + PA P0 P1 P2 + data; no operator code.
    require(Dlt645Codec::buildWriteFrame("1", "C117", dlt645_test::fixturePassword(), "", {0x15}, Dlt645Version::V1997) ==
                withChecksum(join({{0x68, 0x01, 0, 0, 0, 0, 0, 0x68, 0x04, 0x07, 0x4A, 0xF4},
                                   passwordOnWire(), {0x48}})),
            "1997 write frame");
    require(throwsWith([] { Dlt645Codec::buildReadFrame("1", "02010100", Dlt645Version::V1997); }, "does not fit"),
            "a 2007 DI must not be sent as 1997");
    require(throwsWith([] { Dlt645Codec::buildReadFrame("1", "9010", Dlt645Version::V2007); }, "does not fit"),
            "a 1997 DI must not be sent as 2007");
    // Exception reply C1H with the 1997 error word.
    const auto exception = dlt645_test::replyFrame("1", 0xC1, {0x02});
    require(throwsWith([&] { Dlt645Codec::decodeFrameData(exception, Dlt645Codec::parseDataId("9010"), Dlt645Version::V1997); },
                       "ERR=0x02 (wrong data identifier)"), "1997 error word meaning");
}

void remoteControlAndBroadcastFrames() {
    const auto deadline = sampleTime();
    // 1CH: PA..P2 C0..C3 N1 N2 N3..N8, L=16.
    require(Dlt645Codec::buildRemoteControlFrame("20", dlt645_test::fixturePassword(), "01020304", 0x1A, deadline) ==
                withChecksum(join({{0x68, 0x20, 0, 0, 0, 0, 0, 0x68, 0x1C, 0x10},
                                   passwordOnWire(),
                                   {0x34, 0x35, 0x36, 0x37, 0x4D, 0x33, 0x3C, 0x38, 0x47, 0x5B, 0x43, 0x59}})),
            "C=1CH remote control frame");
    require(Dlt645Codec::remoteControlCode("close_permit") == 0x1B && Dlt645Codec::remoteControlCode("keep_power_release") == 0x3B,
            "N1 codes");
    require(throwsWith([] { Dlt645Codec::remoteControlCode("open"); }, "unknown"), "unknown control type");
    // 08H to 999999999999, ss mm hh DD MM YY.
    require(Dlt645Codec::buildBroadcastTimeFrame(deadline) ==
                withChecksum({0x68, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x68, 0x08, 0x06,
                              0x3C, 0x38, 0x47, 0x5B, 0x43, 0x59}),
            "C=08H broadcast time frame");
}

void addressPatterns() {
    require(Dlt645Codec::encodeAddressPattern("AAAAAAAAAAAA") == std::vector<std::uint8_t>(6, 0xAA), "all wildcard");
    require(Dlt645Codec::encodeAddressPattern("AAAAAAAA0012") ==
                std::vector<std::uint8_t>({0x12, 0x00, 0xAA, 0xAA, 0xAA, 0xAA}),
            "shortened address: known low bytes, AA high bytes");
    for (const auto* bad : {"0012AAAAAAAA", "AA00AA000012", "AAAAAAAA001", "AAAAAAAA00B2"}) {
        require(throwsWith([&] { Dlt645Codec::encodeAddressPattern(bad); }, "address pattern"),
                std::string("invalid pattern accepted: ") + bad);
    }
    const auto query = Dlt645Codec::buildAddressQueryFrame("AAAAAAAAAAAA", Dlt645Version::V2007);
    require(query[8] == 0x13 && query[9] == 0x00, "2007 wildcard query is C=13H without data");
    require(Dlt645Codec::buildAddressQueryFrame("AAAAAAAA0012", Dlt645Version::V2007)[8] == 0x11,
            "shortened 2007 pattern reads 04000401");
    require(dlt645_test::requestDataId(Dlt645Codec::buildAddressQueryFrame("AAAAAAAAAAAA", Dlt645Version::V1997)) == "C032",
            "1997 query reads C032");
}

void client1997() {
    auto port = std::make_shared<Dlt645MockSerialPort>(false, std::vector<std::string>{"000000000003"});
    Dlt645Client client(port, options(), Dlt645Version::V1997);
    const auto voltage = client.readData("3", "B611");
    require(voltage == std::vector<std::uint8_t>({0x20, 0x02}), "1997 B611 = 220 V (XXX)");
    const auto block = client.readData("3", "B62F");
    require(block.size() == 6, "1997 current block A/B/C");
    require(throwsWith([&] { client.readData("3", "D999"); }, "ERR=0x02 (wrong data identifier)"), "1997 C1H");

    // A1H announces follow-up data: C=02H with the DI, answered with 82H.
    auto script = std::make_shared<ScriptPort>();
    script->onWrite = [](const std::vector<std::uint8_t>& request) {
        if (dlt645_test::requestControl(request) == 0x01) {
            return Chunks{dlt645_test::readReply("3", "901F", {0x67, 0x45}, 0xA1)};
        }
        return Chunks{dlt645_test::readReply("3", "901F", {0x23, 0x01}, 0x82)};
    };
    Dlt645Client followUp(script, options(), Dlt645Version::V1997);
    require(followUp.readData("3", "901F") == std::vector<std::uint8_t>({0x67, 0x45, 0x23, 0x01}),
            "1997 follow-up data appended (no sequence byte)");
    require(dlt645_test::requestControl(script->writes[1]) == 0x02, "follow-up request uses C=02H");
}

void broadcastSharesTheBus() {
    auto port = std::make_shared<ScriptPort>();
    port->onWrite = [](const std::vector<std::uint8_t>& request) {
        if (dlt645_test::requestControl(request) == 0x08) return Chunks{};  // broadcasts are never answered
        return Chunks{dlt645_test::readReply("1", "02010100", {0x01, 0x22})};
    };
    auto o = options(200);
    o.frameIntervalMs = 100;
    auto client = std::make_shared<Dlt645Client>(port, o);
    client->readData("1", "02010100");
    const auto replyAt = port->lastDelivery;
    std::int64_t clockReadAt = 0;
    client->broadcastTime([&clockReadAt] {
        clockReadAt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return static_cast<std::int64_t>(1793700000000LL);
    });
    require(port->writes.size() == 2 && dlt645_test::requestControl(port->writes[1]) == 0x08, "broadcast sent");
    const auto quietMs = std::chrono::duration_cast<std::chrono::milliseconds>(port->writeTimes[1] - replyAt).count();
    require(quietMs >= 95, "broadcast waits for the bus quiet time after the previous reply");
    const auto sentMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        port->writeTimes[1].time_since_epoch()).count();
    require(sentMs - clockReadAt < 20, "the time is read after the bus became free, right before sending");
    client->readData("1", "02010100");
    const auto afterMs = std::chrono::duration_cast<std::chrono::milliseconds>(port->writeTimes[2] - port->writeTimes[1]).count();
    require(afterMs >= 95, "the next request also keeps the quiet time after a broadcast");

    // A broadcast never cuts into a running transaction: it waits for the client lock.
    auto slow = std::make_shared<ScriptPort>();
    std::atomic<bool> inTransaction(false);
    slow->onWrite = [&inTransaction](const std::vector<std::uint8_t>& request) {
        if (dlt645_test::requestControl(request) == 0x08) {
            require(!inTransaction.load(), "broadcast written during a transaction");
            return Chunks{};
        }
        inTransaction = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        inTransaction = false;
        return Chunks{dlt645_test::readReply("1", "02010100", {0x01, 0x22})};
    };
    auto shared = std::make_shared<Dlt645Client>(slow, options(200));
    std::thread reader([shared] { shared->readData("1", "02010100"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    shared->broadcastTime([] { return static_cast<std::int64_t>(1793700000000LL); });
    reader.join();
}

Dlt645ClockState clockState(bool trusted, const std::string& reason = "clock_unsynchronized") {
    Dlt645ClockState state;
    state.trusted = trusted;
    state.kernelRead = true;
    state.kernelStatus = trusted ? 0x2001 : 0x0041;
    state.maxErrorUs = trusted ? 25000 : 16000000;
    if (!trusted) {
        state.reason = reason;
        state.message = reason == "clock_before_2024"
            ? "未发送：系统时钟未同步（系统时间早于 2024 年，RTC 未设置）"
            : "未发送：系统时钟未同步（内核 STA_UNSYNC 置位，maxerror=16000000 us）";
    }
    return state;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

// The status file is valid JSON and repeats what status() holds.
json::JsonValue statusFile(const std::string& path) {
    return json::JsonParser(readFile(path)).parse();
}

std::string field(const json::JsonValue& value, const std::string& key) {
    const auto* item = value.find(key);
    require(item != nullptr, "status file misses " + key);
    return item->asString();
}

bool startsWith(const std::string& text, const std::string& prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

std::size_t countLines(const std::vector<std::string>& lines, const std::string& text) {
    return static_cast<std::size_t>(std::count_if(lines.begin(), lines.end(),
        [&text](const std::string& line) { return line.find(text) != std::string::npos; }));
}

std::string scratchStatusPath() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return (filesystem::temp_directory_path() / ("dlt645-time-sync-test-" + std::to_string(stamp)) / "status.json").string();
}

void timeSyncRules() {
    auto port = std::make_shared<Dlt645MockSerialPort>();
    auto client = std::make_shared<Dlt645Client>(port, options());
    // 2026-10-28 14:05:09 local time
    std::tm noon = sampleTime();
    std::int64_t wall = static_cast<std::int64_t>(std::mktime(&noon)) * 1000;
    Dlt645ClockState clock = clockState(false);
    std::vector<std::string> logs;
    const auto path = scratchStatusPath();
    Dlt645TimeSyncConfig config;
    config.enabled = true;
    config.intervalMs = 3600000;
    Dlt645TimeSync sync(config, client, path,
        [&clock](std::int64_t) { return clock; },
        [&wall] { return wall; },
        [&logs](const std::string& line) { logs.push_back(line); });
    require(field(statusFile(path), "result") == "pending" && logs.empty(), "enabled, not yet tried");

    // Not synchronized: explicit "not sent" in status, status file and log; never success.
    require(!sync.tick(0) && port->broadcastCount() == 0, "untrusted clock: no broadcast");
    const auto& status = sync.status();
    require(status.result == "skipped" && status.reason == "clock_unsynchronized" &&
                startsWith(status.message, "未发送：系统时钟未同步") && status.lastSentWallMs == 0 &&
                status.notSentSinceLastSent == 1 && status.nextAttemptInMs == 60000,
            "unsynchronized clock reported as not sent: " + status.message);
    const auto file = statusFile(path);
    require(field(file, "result") == "skipped" && field(file, "reason") == "clock_unsynchronized" &&
                startsWith(field(file, "message"), "未发送：系统时钟未同步") &&
                file.find("clock")->find("staUnsync")->asBool() &&
                file.find("clock")->find("maxErrorUs")->asNumber() == 16000000.0,
            "status file holds the last attempt and the kernel clock fields");
    require(logs.size() == 1 && logs[0].find("result=skipped reason=clock_unsynchronized") != std::string::npos &&
                logs[0].find("未发送：系统时钟未同步") != std::string::npos,
            "log says why nothing was sent");

    require(!sync.tick(30000) && sync.status().notSentSinceLastSent == 1, "retry waits a minute");
    wall += 60000;
    require(!sync.tick(60000) && sync.status().notSentSinceLastSent == 2 && logs.size() == 1,
            "an unchanged reason is not logged every minute");
    require(statusFile(path).find("attemptTs")->asNumber() == static_cast<double>(wall),
            "the status file follows every attempt");
    wall += 3600000;
    require(!sync.tick(60000 + 3600000) && logs.size() == 2 && countLines(logs, "3 attempt(s) not sent") == 1,
            "an unchanged reason is repeated hourly with the count");
    clock = clockState(false, "clock_before_2024");
    require(!sync.tick(60000 + 3660000) && logs.size() == 3 && sync.status().reason == "clock_before_2024" &&
                startsWith(sync.status().message, "未发送：系统时钟未同步（系统时间早于 2024 年"),
            "a new reason is logged at once");

    // Trusted but inside the midnight window.
    clock = clockState(true);
    std::tm midnight = sampleTime();
    midnight.tm_hour = 23;
    midnight.tm_min = 58;
    wall = static_cast<std::int64_t>(std::mktime(&midnight)) * 1000;
    require(!sync.tick(60000 + 3720000) && port->broadcastCount() == 0 && sync.status().result == "skipped" &&
                sync.status().reason == "midnight_window" && startsWith(sync.status().message, "未发送：23:55～00:05"),
            "no broadcast 23:55..00:05, reported");
    wall = static_cast<std::int64_t>(std::mktime(&noon)) * 1000;
    require(sync.tick(60000 + 3780000) && port->broadcastCount() == 1, "trusted clock: broadcast");
    require(port->lastBroadcastTime() == std::vector<std::uint8_t>({0x09, 0x05, 0x14, 0x28, 0x10, 0x26}),
            "broadcast carries the gateway local time");
    require(sync.status().result == "sent" && sync.status().lastSentWallMs == wall &&
                sync.status().notSentSinceLastSent == 0 && sync.status().nextAttemptInMs == 3600000 &&
                sync.status().message == "已发送广播校时 2026-10-28 14:05:09（表不应答，是否采纳无法确认）",
            "sent: time of sending, no claim that meters took it: " + sync.status().message);
    require(field(statusFile(path), "result") == "sent" && countLines(logs, "result=sent") == 1, "sent is logged");
    require(!sync.tick(60000 + 3780000 + 3599000) && port->broadcastCount() == 1, "interval respected");

    Dlt645TimeSyncConfig off;
    const auto offPath = scratchStatusPath();
    std::vector<std::string> offLogs;
    Dlt645TimeSync disabled(off, client, offPath, [](std::int64_t) { return clockState(true); }, [&wall] { return wall; },
        [&offLogs](const std::string& line) { offLogs.push_back(line); });
    require(!disabled.tick(0) && port->broadcastCount() == 1 && disabled.status().result == "disabled" &&
                field(statusFile(offPath), "result") == "disabled",
            "disabled by default, and the status file says so");
    filesystem::remove_all(filesystem::path(path).parent_path());
    filesystem::remove_all(filesystem::path(offPath).parent_path());
}

class UnopenablePort : public ScriptPort {
public:
    void open() override { throw std::runtime_error("cannot open /dev/ttyS9"); }
};

void timeSyncSendFailures() {
    std::tm noon = sampleTime();
    const std::int64_t wall = static_cast<std::int64_t>(std::mktime(&noon)) * 1000;
    Dlt645TimeSyncConfig config;
    config.enabled = true;
    std::vector<std::string> logs;
    const auto trusted = [](std::int64_t) { return clockState(true); };

    // Port cannot be opened: nothing reached the wire.
    auto closed = std::make_shared<Dlt645Client>(std::make_shared<UnopenablePort>(), options());
    Dlt645TimeSync notOpened(config, closed, std::string(), trusted, [wall] { return wall; },
        [&logs](const std::string& line) { logs.push_back(line); });
    require(!notOpened.tick(0) && notOpened.status().result == "failed" && notOpened.status().reason == "send_failed" &&
                startsWith(notOpened.status().message, "未发送：串口发送失败") &&
                notOpened.status().nextAttemptInMs == 60000 && notOpened.status().lastSentWallMs == 0,
            "port error: failed, not sent: " + notOpened.status().message);

    // Written, but the adapter's echo never came back: the frame may be on the bus.
    auto silent = std::make_shared<ScriptPort>();
    auto echo = options();
    echo.localEcho = true;
    auto echoClient = std::make_shared<Dlt645Client>(silent, echo);
    Dlt645TimeSync uncertain(config, echoClient, std::string(), trusted, [wall] { return wall; },
        [&logs](const std::string& line) { logs.push_back(line); });
    require(!uncertain.tick(0) && silent->writes.size() == 1 && uncertain.status().result == "failed" &&
                uncertain.status().reason == "send_uncertain" && startsWith(uncertain.status().message, "发送结果未知") &&
                uncertain.status().lastSentWallMs == 0,
            "echo missing: outcome unknown, never reported as sent: " + uncertain.status().message);
    require(countLines(logs, "result=failed") == 2 && countLines(logs, "result=sent") == 0, "failures are logged");
}

void gatewayClockState() {
    // Real kernel: an implausible date is never trusted; otherwise the verdict is STA_UNSYNC.
    const auto old = dlt645GatewayClockState(1577836800000LL);  // 2020-01-01
    require(!old.trusted && (old.reason == "clock_before_2024" || (!old.kernelRead && old.reason == "clock_state_unavailable")) &&
                startsWith(old.message, "未发送："),
            "a 2020 clock is not trusted: " + old.message);
    const auto now = dlt645GatewayClockState(1793700000000LL);  // 2026-11
#ifndef _WIN32
    if (now.kernelRead) {
        require(now.trusted == ((now.kernelStatus & STA_UNSYNC) == 0), "verdict follows STA_UNSYNC");
        require(now.trusted || (now.reason == "clock_unsynchronized" && startsWith(now.message, "未发送：系统时钟未同步")),
                "unsynchronized kernel clock: " + now.message);
    }
#endif
    require(dlt645TimeSyncStatusPath("/opt/modbus-gateway/config/runtime/devices/device_dlt645_a.json") ==
                "/run/modbus-gateway/dlt645-time-sync-device_dlt645_a.json",
            "status file named after the device config (systemd instance)");
}

void addressQuery() {
    auto single = std::make_shared<Dlt645MockSerialPort>(false, std::vector<std::string>{"000000000021"});
    Dlt645Client client(single, options());
    require(client.queryAddress("AAAAAAAAAAAA") == "000000000021", "single meter: C=13H returns its address");

    auto two = std::make_shared<Dlt645MockSerialPort>(false, std::vector<std::string>{"000000000021", "000000000022"});
    Dlt645Client bus(two, options());
    require(throwsWith([&] { bus.queryAddress("AAAAAAAAAAAA"); }, "several meters"),
            "two meters answering the wildcard must be reported");
    require(bus.queryAddress("AAAAAAAAAA22") == "000000000022", "a shortened pattern selects one meter");
    require(throwsWith([&] { bus.queryAddress("AAAAAAAAAA99"); }, "no meter answered"), "no match: no answer");

    auto old = std::make_shared<Dlt645MockSerialPort>(false, std::vector<std::string>{"000000000003"});
    Dlt645Client client1997(old, options(), Dlt645Version::V1997);
    require(client1997.queryAddress("AAAAAAAAAAAA") == "000000000003", "1997 address via C032 to the wildcard");
}

}  // namespace

int main() {
    try {
        frames1997();
        remoteControlAndBroadcastFrames();
        addressPatterns();
        client1997();
        broadcastSharesTheBus();
        timeSyncRules();
        timeSyncSendFailures();
        gatewayClockState();
        addressQuery();
        std::cout << "dlt645_extensions_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "dlt645_extensions_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
