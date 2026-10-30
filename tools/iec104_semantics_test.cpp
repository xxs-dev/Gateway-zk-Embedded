// IEC 104 controlling-station semantics against a scripted controlled station on loopback:
// GI/CI scheduling with ACT_CON/ACT_TERM tracking (finding 4), N(S) supervision (5),
// counter interrogation (10), command confirmation filtering, negative COT 44-47, deselect and
// outcome-unknown (9), STARTDT timeout, APCI length and clock-sync confirmation (11).
#include "edge_gateway/iec_client.hpp"
#include "edge_gateway/iec_codec.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using namespace edge_gateway;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

class Station104 {
public:
    using Handler = std::function<void(const std::vector<std::uint8_t>&, Station104&)>;

    Handler onAsdu;
    std::function<void(Station104&)> afterStartDt;
    bool answerStartDt = true;
    std::atomic<int> connections{0};
    std::atomic<int> closedByMaster{0};

    Station104() {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(listenFd_, 4) != 0) {
            throw std::runtime_error("station listen failed");
        }
        socklen_t length = sizeof(address);
        getsockname(listenFd_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
    }

    ~Station104() {
        stop();
    }

    void start() {
        running_ = true;
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        if (listenFd_ >= 0) {
            ::close(listenFd_);
            listenFd_ = -1;
        }
    }

    int port() const { return port_; }

    // Called from handlers (station thread).
    void sendAsdu(const std::vector<std::uint8_t>& asdu) {
        sendWithSequence(asdu, sendSequence_++);
    }

    void sendWithSequence(const std::vector<std::uint8_t>& asdu, std::uint16_t sendSequence) {
        std::vector<std::uint8_t> frame = {0x68, static_cast<std::uint8_t>(4 + asdu.size()),
            static_cast<std::uint8_t>((sendSequence << 1) & 0xFF), static_cast<std::uint8_t>((sendSequence << 1) >> 8),
            static_cast<std::uint8_t>((receiveSequence_ << 1) & 0xFF), static_cast<std::uint8_t>((receiveSequence_ << 1) >> 8)};
        frame.insert(frame.end(), asdu.begin(), asdu.end());
        sendRaw(frame);
    }

    void sendRaw(const std::vector<std::uint8_t>& bytes) {
        if (clientFd_ >= 0) {
            ::send(clientFd_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        }
    }

    std::vector<std::vector<std::uint8_t>> received() {
        std::lock_guard<std::mutex> lock(mutex_);
        return received_;
    }

    int countType(std::uint8_t typeId) {
        std::lock_guard<std::mutex> lock(mutex_);
        int count = 0;
        for (const auto& asdu : received_) {
            if (!asdu.empty() && asdu[0] == typeId) ++count;
        }
        return count;
    }

private:
    static bool readable(int fd, int timeoutMs) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        timeval timeout{0, timeoutMs * 1000};
        return select(fd + 1, &set, nullptr, nullptr, &timeout) > 0;
    }

    bool readExact(std::uint8_t* data, std::size_t size) {
        std::size_t got = 0;
        while (got < size && running_) {
            if (!readable(clientFd_, 20)) continue;
            const auto count = ::recv(clientFd_, data + got, size - got, 0);
            if (count <= 0) return false;
            got += static_cast<std::size_t>(count);
        }
        return got == size;
    }

    void run() {
        while (running_) {
            if (!readable(listenFd_, 20)) continue;
            clientFd_ = ::accept(listenFd_, nullptr, nullptr);
            if (clientFd_ < 0) continue;
            ++connections;
            sendSequence_ = 0;
            receiveSequence_ = 0;
            while (running_) {
                std::uint8_t header[2];
                if (!readExact(header, 2)) {
                    if (running_) ++closedByMaster;
                    break;
                }
                std::vector<std::uint8_t> frame(header, header + 2);
                frame.resize(2 + header[1]);
                if (!readExact(frame.data() + 2, header[1])) {
                    if (running_) ++closedByMaster;
                    break;
                }
                handle(frame);
            }
            ::close(clientFd_);
            clientFd_ = -1;
        }
    }

    void handle(const std::vector<std::uint8_t>& frame) {
        if (frame[2] == 0x07) {
            if (answerStartDt) {
                sendRaw({0x68, 0x04, 0x0B, 0x00, 0x00, 0x00});
                if (afterStartDt) afterStartDt(*this);
            }
            return;
        }
        if (frame[2] == 0x43) {
            sendRaw({0x68, 0x04, 0x83, 0x00, 0x00, 0x00});
            return;
        }
        if ((frame[2] & 0x01) != 0) {
            return;
        }
        receiveSequence_ = static_cast<std::uint16_t>((IecCodec::iec104SendSequence(frame) + 1) & 0x7FFF);
        sendRaw(IecCodec::buildIec104SFrame(receiveSequence_));
        const auto asdu = IecCodec::iec104AsduPayload(frame);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            received_.push_back(asdu);
        }
        if (onAsdu) onAsdu(asdu, *this);
    }

    int listenFd_ = -1;
    int clientFd_ = -1;
    int port_ = 0;
    std::uint16_t sendSequence_ = 0;
    std::uint16_t receiveSequence_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::vector<std::vector<std::uint8_t>> received_;
};

std::vector<std::uint8_t> withCause(std::vector<std::uint8_t> asdu, int cause) {
    asdu[2] = static_cast<std::uint8_t>(cause);
    return asdu;
}

// Standard GI / CI responses: ACT_CON, data, ACT_TERM.
void answerInterrogations(const std::vector<std::uint8_t>& asdu, Station104& station) {
    if (asdu[0] == 100) {
        station.sendAsdu(withCause(asdu, 7));
        station.sendAsdu({1, 1, 20, 0, 1, 0, 0xE9, 0x03, 0x00, 0x01});
        station.sendAsdu(withCause(asdu, 10));
    } else if (asdu[0] == 101) {
        station.sendAsdu(withCause(asdu, 7));
        station.sendAsdu({15, 1, 37, 0, 1, 0, 0x01, 0x10, 0x00, 0x2A, 0x00, 0x00, 0x00, 0x00});
        station.sendAsdu(withCause(asdu, 10));
    }
}

IecProtocolConfig baseConfig() {
    IecProtocolConfig config;
    config.commonAddress = 1;
    config.backgroundReceive = true;
    config.idleReadTimeoutMs = 10;
    config.pollTimeoutMs = 50;
    config.t0Ms = 2000;
    config.t1Ms = 1500;
    config.t2Ms = 200;
    config.t3Ms = 5000;
    config.wAck = 1;
    return config;
}

TcpTransportConfig tcpFor(const Station104& station) {
    TcpTransportConfig tcp;
    tcp.host = "127.0.0.1";
    tcp.port = station.port();
    tcp.timeoutMs = 200;
    return tcp;
}

std::vector<IecDataValue> pollFor(Iec104Client& client, int durationMs) {
    std::vector<IecDataValue> values;
    const auto deadline = steadyMs() + durationMs;
    while (steadyMs() < deadline) {
        const auto polled = client.poll();
        values.insert(values.end(), polled.begin(), polled.end());
    }
    return values;
}

bool hasValue(const std::vector<IecDataValue>& values, int ioa, double value) {
    for (const auto& candidate : values) {
        if (candidate.ioa == ioa && candidate.value == value) return true;
    }
    return false;
}

// Finding 4 + 10: one GI after STARTDT, CI once the GI terminated, no GI per poll.
void verifyInterrogationScheduling() {
    Station104 station;
    station.onAsdu = answerInterrogations;
    station.start();
    auto config = baseConfig();
    config.interrogationIntervalMs = 400;
    config.counterInterrogationIntervalMs = 0;
    Iec104Client client(tcpFor(station), config);
    const auto values = pollFor(client, 300);
    require(station.countType(100) == 1, "exactly one GI within the period, got " + std::to_string(station.countType(100)));
    require(station.countType(101) == 1, "one counter interrogation after the GI terminated");
    require(hasValue(values, 1001, 1.0), "GI data delivered");
    require(hasValue(values, 4097, 42.0), "integrated total delivered by the counter interrogation");
    (void)pollFor(client, 250);
    require(station.countType(100) == 2, "terminated GI repeats after interrogationIntervalMs");
    require(station.countType(101) == 1, "counterInterrogationIntervalMs=0: CI only after connection");
}

// counterInterrogation=false: a substation that does not support C_CI_NA_1 is never sent one.
void verifyCounterInterrogationCanBeDisabled() {
    Station104 station;
    station.onAsdu = answerInterrogations;
    station.start();
    auto config = baseConfig();
    config.interrogationIntervalMs = 100;
    config.counterInterrogation = false;
    config.counterInterrogationIntervalMs = 100;
    Iec104Client client(tcpFor(station), config);
    (void)pollFor(client, 400);
    require(station.countType(100) >= 2, "GI keeps running with counter interrogation disabled");
    require(station.countType(101) == 0, "counterInterrogation=false: no counter interrogation is sent");
}

// Finding 4: a GI without ACT_TERM is not repeated, even when its period elapses.
void verifyUnterminatedInterrogationBlocks() {
    Station104 station;
    station.onAsdu = [](const std::vector<std::uint8_t>& asdu, Station104& self) {
        if (asdu[0] == 100) self.sendAsdu(withCause(asdu, 7));  // ACT_CON, never ACT_TERM
    };
    station.start();
    auto config = baseConfig();
    config.interrogationIntervalMs = 100;
    config.counterInterrogation = false;
    Iec104Client client(tcpFor(station), config);
    (void)pollFor(client, 400);
    require(station.countType(100) == 1, "unterminated GI must not be resent");
}

// Finding 5: N(S) != V(R) closes the connection (gap and duplicate).
void verifySequenceSupervision(std::uint16_t secondSequence, const std::string& what) {
    Station104 station;
    station.afterStartDt = [secondSequence](Station104& self) {
        self.sendWithSequence({1, 1, 3, 0, 1, 0, 0xE9, 0x03, 0x00, 0x01}, 0);
        self.sendWithSequence({1, 1, 3, 0, 1, 0, 0xEA, 0x03, 0x00, 0x01}, secondSequence);
    };
    station.start();
    auto config = baseConfig();
    config.pollOnCollect = false;
    config.counterInterrogation = false;
    config.backgroundReceive = false;
    Iec104Client client(tcpFor(station), config);
    bool failed = false;
    try {
        (void)pollFor(client, 200);
    } catch (const std::runtime_error& ex) {
        failed = std::string(ex.what()).find("N(S)") != std::string::npos;
    }
    require(failed, what + ": N(S) error must be reported");
    for (int i = 0; i < 50 && station.closedByMaster.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(station.closedByMaster.load() == 1, what + ": connection must be closed");
}

// Finding 11: STARTDT without confirmation closes the connection.
void verifyStartDtTimeout() {
    Station104 station;
    station.answerStartDt = false;
    station.start();
    auto config = baseConfig();
    config.t1Ms = 200;
    Iec104Client client(tcpFor(station), config);
    bool failed = false;
    try {
        (void)client.poll();
    } catch (const std::runtime_error& ex) {
        failed = std::string(ex.what()).find("STARTDT") != std::string::npos;
    }
    require(failed, "STARTDT timeout reported");
    for (int i = 0; i < 50 && station.closedByMaster.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(station.closedByMaster.load() == 1, "STARTDT timeout closes the connection");
}

// Finding 11: an APCI length above 253 is a framing error.
void verifyApduLengthLimit() {
    Station104 station;
    station.afterStartDt = [](Station104& self) {
        std::vector<std::uint8_t> oversized(2 + 254, 0);
        oversized[0] = 0x68;
        oversized[1] = 254;
        self.sendRaw(oversized);
    };
    station.start();
    auto config = baseConfig();
    config.pollOnCollect = false;
    config.counterInterrogation = false;
    config.backgroundReceive = false;
    Iec104Client client(tcpFor(station), config);
    bool failed = false;
    try {
        (void)pollFor(client, 200);
    } catch (const std::runtime_error& ex) {
        failed = std::string(ex.what()).find("APCI") != std::string::npos;
    }
    require(failed, "oversized APDU rejected");
}

PointDefinition commandPoint(int ioa) {
    PointDefinition point;
    point.index = 7;
    point.pointCode = "breaker_cmd";
    point.read.enable = true;
    point.read.dataType = "single_point";
    point.read.iec.ioa = ioa;
    point.read.iec.typeId = 1;  // the read side type must not filter command confirmations
    point.write.enable = true;
    point.write.dataType = "single_command";
    point.write.iec.ioa = ioa;
    point.write.iec.timeoutMs = 300;
    return point;
}

// Command behaviour by IOA: 3001 confirm+terminate, 3002 COT 47, 3003 select ok / execute
// negative, 3004 no reply to execute, 3005 no reply at all.
void answerCommands(const std::vector<std::uint8_t>& asdu, Station104& station) {
    if (asdu[0] != 45 || asdu[2] != 6) return;
    const int ioa = asdu[6] | (asdu[7] << 8) | (asdu[8] << 16);
    const bool select = (asdu.back() & 0x80) != 0;
    if (ioa == 3001) {
        station.sendAsdu(withCause(asdu, 7));
        if (!select) station.sendAsdu(withCause(asdu, 10));
    } else if (ioa == 3002) {
        station.sendAsdu(withCause(asdu, 0x40 | 47));
    } else if (ioa == 3003) {
        station.sendAsdu(withCause(asdu, select ? 7 : 0x47));
    } else if (ioa == 3004 && select) {
        station.sendAsdu(withCause(asdu, 7));
    }
}

bool sawDeselect(Station104& station, int ioa) {
    for (const auto& asdu : station.received()) {
        if (asdu[0] == 45 && asdu[2] == 8 && (asdu.back() & 0x80) != 0 &&
            (asdu[6] | (asdu[7] << 8) | (asdu[8] << 16)) == ioa) {
            return true;
        }
    }
    return false;
}

// Finding 9.
void verifyCommands() {
    Station104 station;
    station.onAsdu = answerCommands;
    station.start();
    auto config = baseConfig();
    config.pollOnCollect = false;
    config.counterInterrogation = false;
    Iec104Client client(tcpFor(station), config);
    (void)client.poll();

    auto direct = commandPoint(3001);
    direct.write.iec.waitActivationTermination = true;
    const auto ok = client.writeByPoint(direct, 1.0, "C1", "M", "D", 1);
    require(ok.success, "command without write.iec.typeId is confirmed by its C_SC echo: " + ok.message);

    const auto unknownIoa = client.writeByPoint(commandPoint(3002), 1.0, "C2", "M", "D", 1);
    require(!unknownIoa.success && !unknownIoa.outcomeUnknown &&
        unknownIoa.message.find("COT 47") != std::string::npos, "COT 47 is a definite failure: " + unknownIoa.message);

    auto sbo = commandPoint(3003);
    sbo.write.iec.selectBeforeExecute = true;
    const auto rejected = client.writeByPoint(sbo, 1.0, "C3", "M", "D", 1);
    require(!rejected.success && !rejected.outcomeUnknown, "negative execute is a definite failure");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(sawDeselect(station, 3003), "failed execute after a select sends the deselect");

    auto silentExecute = commandPoint(3004);
    silentExecute.write.iec.selectBeforeExecute = true;
    const auto unknown = client.writeByPoint(silentExecute, 1.0, "C4", "M", "D", 1);
    require(!unknown.success && unknown.outcomeUnknown, "execute without confirmation: outcome unknown");

    auto silentSelect = commandPoint(3005);
    silentSelect.write.iec.selectBeforeExecute = true;
    const auto selectTimeout = client.writeByPoint(silentSelect, 1.0, "C5", "M", "D", 1);
    require(!selectTimeout.success && !selectTimeout.outcomeUnknown, "select timeout never executed anything");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(sawDeselect(station, 3005), "select timeout sends the deselect");
    int executes = 0;
    for (const auto& asdu : station.received()) {
        const int ioa = asdu[6] | (asdu[7] << 8) | (asdu[8] << 16);
        if (asdu[0] == 45 && asdu[2] == 6 && ioa == 3005 && (asdu.back() & 0x80) == 0) ++executes;
    }
    require(executes == 0, "no execute after a select timeout");
}

// Finding 11: clock synchronisation waits for its ACT_CON.
void verifyClockSync() {
    Station104 station;
    std::atomic<int> mode{0};
    station.onAsdu = [&mode](const std::vector<std::uint8_t>& asdu, Station104& self) {
        if (asdu[0] != 103) return;
        if (mode == 0) self.sendAsdu(withCause(asdu, 7));
        if (mode == 1) self.sendAsdu(withCause(asdu, 0x47));
    };
    station.start();
    auto config = baseConfig();
    config.pollOnCollect = false;
    config.counterInterrogation = false;
    config.t1Ms = 300;
    Iec104Client client(tcpFor(station), config);
    client.synchronizeClock(1785315600000LL);
    mode = 1;
    bool rejected = false;
    try {
        client.synchronizeClock(1785315600000LL);
    } catch (const std::runtime_error& ex) {
        rejected = std::string(ex.what()).find("rejected") != std::string::npos;
    }
    require(rejected, "negative clock sync confirmation reported");
    mode = 2;
    bool timedOut = false;
    try {
        client.synchronizeClock(1785315600000LL);
    } catch (const std::runtime_error& ex) {
        timedOut = std::string(ex.what()).find("timeout") != std::string::npos;
    }
    require(timedOut, "missing clock sync confirmation reported");
}

}  // namespace

int main() {
    try {
        verifyInterrogationScheduling();
        verifyCounterInterrogationCanBeDisabled();
        verifyUnterminatedInterrogationBlocks();
        verifySequenceSupervision(7, "N(S) gap");
        verifySequenceSupervision(0, "duplicate N(S)");
        verifyStartDtTimeout();
        verifyApduLengthLimit();
        verifyCommands();
        verifyClockSync();
        std::cout << "IEC104 semantics tests passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "IEC104 semantics test failed: " << ex.what() << std::endl;
        return 1;
    }
}
