#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "edge_gateway/serial_port.hpp"

namespace edge_gateway {

// Simulated DL/T 645-2007 meters for `Dlt645Driver --mock`: every address answers with the
// standard data formats of the shipped templates (0205 apparent power, 0206 power factor, 0207
// phase angle, signed current/power/reactive energy, FF block reads), D1H/ERR=02 for unknown DIs,
// and 94H for C=14H writes (06010101 trips, 06010201 closes the simulated breaker, reported by
// 07020100 and bit 4 of status word 04000503). FE wake-up bytes are accepted; with localEcho the
// request is looped back first like an echoing RS-485 adapter.
class Dlt645MockSerialPort : public ISerialPort {
public:
    explicit Dlt645MockSerialPort(bool localEcho = false) : localEcho_(localEcho) {}

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

    // Data of a DI as the simulated meter sends it (33H not yet added); empty = not supported.
    std::vector<std::uint8_t> dataFor(const std::string& dataIdHex) const;

private:
    bool localEcho_ = false;
    bool open_ = false;
    bool breakerClosed_ = true;
    std::size_t requests_ = 0;
    std::vector<std::uint8_t> pending_;
};

}  // namespace edge_gateway
