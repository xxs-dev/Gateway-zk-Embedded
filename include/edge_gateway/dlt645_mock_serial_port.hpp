#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "edge_gateway/serial_port.hpp"

namespace edge_gateway {

// Simulated DL/T 645 meters for `Dlt645Driver --mock` and tests, answering in the standard data
// formats of the shipped templates:
//  - 2007: C=11H reads (0205 apparent power, 0206 power factor, 0207 phase angle, signed
//    current/power/reactive energy, FF blocks, breaker extension DIs), C=14H writes (06010101
//    trips, 06010201 closes the simulated breaker), C=1CH remote control (1A trips, 1C closes),
//    C=13H read address; the position shows in 07020100 and bit 4 of status word 04000503;
//  - 1997: C=01H reads (9010.. energy, B61x voltage XXX, B62x current XX.XX, B63x power XX.XXXX,
//    B64x reactive XX.XX, B65x power factor X.XXX, C032 meter number, blocks ending in F),
//    C=04H writes;
//  - C=08H broadcast time is recorded, never answered; unknown DIs get the exception reply ERR=02.
// With an address list only those meters answer; a wildcard (AA) request matching several of them
// produces colliding replies. FE wake-up bytes are accepted; with localEcho the request is looped
// back first like an echoing RS-485 adapter.
class Dlt645MockSerialPort : public ISerialPort {
public:
    explicit Dlt645MockSerialPort(bool localEcho = false, std::vector<std::string> addresses = {});

    void open() override { open_ = true; }
    void close() override {
        open_ = false;
        pending_.clear();
    }
    bool isOpen() const override { return open_; }
    void write(const std::vector<std::uint8_t>& bytes) override;
    std::vector<std::uint8_t> read(std::size_t maxBytes, int timeoutMs) override;

    bool breakerClosed() const { return breakerClosed_; }
    std::size_t requestCount() const { return requests_; }
    std::size_t broadcastCount() const { return broadcasts_; }
    // ss mm hh DD MM YY of the last broadcast time sync (33H removed).
    const std::vector<std::uint8_t>& lastBroadcastTime() const { return lastBroadcastTime_; }
    // Last remote control command: N1 and the validity deadline (33H removed).
    std::uint8_t lastRemoteControl() const { return lastRemoteControl_; }
    const std::vector<std::uint8_t>& lastRemoteControlDeadline() const { return lastRemoteControlDeadline_; }

    // Data of a DI as the simulated meter sends it (33H not yet added); empty = not supported.
    // 8 hex digits: DL/T 645-2007; 4 hex digits: DL/T 645-1997.
    std::vector<std::uint8_t> dataFor(const std::string& dataIdHex) const;

private:
    std::vector<std::string> matchingMeters(const std::vector<std::uint8_t>& request) const;
    std::vector<std::uint8_t> answer(const std::vector<std::uint8_t>& request, const std::string& address);

    bool localEcho_ = false;
    std::vector<std::string> addresses_;
    bool open_ = false;
    bool breakerClosed_ = true;
    std::size_t requests_ = 0;
    std::size_t broadcasts_ = 0;
    std::vector<std::uint8_t> lastBroadcastTime_;
    std::uint8_t lastRemoteControl_ = 0;
    std::vector<std::uint8_t> lastRemoteControlDeadline_;
    std::vector<std::uint8_t> pending_;
};

}  // namespace edge_gateway
