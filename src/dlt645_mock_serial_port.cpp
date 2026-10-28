#include "edge_gateway/dlt645_mock_serial_port.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

#include "edge_gateway/dlt645_codec.hpp"

namespace edge_gateway {

namespace {

std::vector<std::uint8_t> bcd(long long value, std::size_t byteCount) {
    const bool negative = value < 0;
    unsigned long long magnitude = static_cast<unsigned long long>(negative ? -value : value);
    std::vector<std::uint8_t> bytes(byteCount, 0);
    for (std::size_t i = 0; i < byteCount; ++i) {
        const auto low = static_cast<std::uint8_t>(magnitude % 10U);
        magnitude /= 10U;
        const auto high = static_cast<std::uint8_t>(magnitude % 10U);
        magnitude /= 10U;
        bytes[i] = static_cast<std::uint8_t>((high << 4U) | low);
    }
    if (negative && !bytes.empty()) {
        bytes.back() = static_cast<std::uint8_t>(bytes.back() | 0x80U);  // DL/T 645 sign bit
    }
    return bytes;
}

void append(std::vector<std::uint8_t>& target, const std::vector<std::uint8_t>& bytes) {
    target.insert(target.end(), bytes.begin(), bytes.end());
}

int hexByteAt(const std::string& dataId, std::size_t index) {
    return static_cast<int>(std::strtol(dataId.substr(index * 2U, 2U).c_str(), nullptr, 16));
}

// One phase/total item of an instantaneous variable (DI3=02) or empty when unsupported.
std::vector<std::uint8_t> variableItem(int di2, int di1) {
    // Voltage XXX.X V, current XXX.XXX A (signed), P/Q/S XX.XXXX (signed), PF X.XXX (signed),
    // phase angle XXX.X degrees (A/B/C only).
    static const long long voltage[] = {0, 2205, 2213, 2198};
    static const long long current[] = {0, 12345, 11345, -10345};
    static const long long active[] = {15234, 5000, 6000, 4234};
    static const long long reactive[] = {6234, 2000, 2234, 2000};
    static const long long apparent[] = {16460, 5390, 6400, 4670};
    static const long long powerFactor[] = {925, 928, 937, 906};
    static const long long angle[] = {0, 231, 220, 245};
    switch (di2) {
        case 0x01: return di1 >= 1 && di1 <= 3 ? bcd(voltage[di1], 2) : std::vector<std::uint8_t>();
        case 0x02: return di1 >= 1 && di1 <= 3 ? bcd(current[di1], 3) : std::vector<std::uint8_t>();
        case 0x03: return di1 <= 3 ? bcd(active[di1], 3) : std::vector<std::uint8_t>();
        case 0x04: return di1 <= 3 ? bcd(reactive[di1], 3) : std::vector<std::uint8_t>();
        case 0x05: return di1 <= 3 ? bcd(apparent[di1], 3) : std::vector<std::uint8_t>();
        case 0x06: return di1 <= 3 ? bcd(powerFactor[di1], 2) : std::vector<std::uint8_t>();
        case 0x07: return di1 >= 1 && di1 <= 3 ? bcd(angle[di1], 2) : std::vector<std::uint8_t>();
        default: return {};
    }
}

// Energy XXXXXX.XX kWh/kvarh; DI2 03/04 (combined reactive 1/2) are signed.
std::vector<std::uint8_t> energyItem(int di2, int tariff, int settlement) {
    static const long long base[] = {0, 1234567, 223456, 345678, -44567};
    if (di2 < 1 || di2 > 4 || tariff > 4 || settlement > 12) {
        return {};
    }
    const auto magnitude = std::llabs(base[di2]) + tariff * 1000LL + settlement * 10LL;
    return bcd(base[di2] < 0 ? -magnitude : magnitude, 4);
}

std::vector<std::uint8_t> reply(
    const std::vector<std::uint8_t>& request,
    std::uint8_t control,
    const std::vector<std::uint8_t>& data
) {
    std::vector<std::uint8_t> frame(request.begin(), request.begin() + 8);
    frame.push_back(control);
    frame.push_back(static_cast<std::uint8_t>(data.size()));
    for (const auto byte : data) {
        frame.push_back(static_cast<std::uint8_t>(byte + 0x33U));
    }
    std::uint32_t sum = 0;
    for (const auto byte : frame) {
        sum += byte;
    }
    frame.push_back(static_cast<std::uint8_t>(sum & 0xFFU));
    frame.push_back(0x16);
    return frame;
}

}  // namespace

std::vector<std::uint8_t> Dlt645MockSerialPort::dataFor(const std::string& dataIdHex) const {
    if (dataIdHex.size() != 8) {
        return {};
    }
    const auto di3 = hexByteAt(dataIdHex, 0);
    const auto di2 = hexByteAt(dataIdHex, 1);
    const auto di1 = hexByteAt(dataIdHex, 2);
    const auto di0 = hexByteAt(dataIdHex, 3);
    std::vector<std::uint8_t> data;
    if (di3 == 0x00) {
        if (di1 == 0xFF) {  // block: total + tariffs 1..4
            for (int tariff = 0; tariff <= 4; ++tariff) append(data, energyItem(di2, tariff, di0));
            return data;
        }
        return energyItem(di2, di1, di0);
    }
    if (di3 == 0x02 && di2 >= 0x01 && di2 <= 0x07) {
        if (di0 != 0x00) return {};
        if (di1 == 0xFF) {
            for (int item = di2 == 0x01 || di2 == 0x02 || di2 == 0x07 ? 1 : 0; item <= 3; ++item) {
                append(data, variableItem(di2, item));
            }
            return data;
        }
        return variableItem(di2, di1);
    }
    if (dataIdHex == "02800002") return bcd(5000, 2);        // 50.00 Hz
    if (dataIdHex == "02900000") return {0x01};               // residual current phase (binary)
    if (dataIdHex == "02900001") return bcd(12, 2);           // residual current mA
    if (dataIdHex.compare(0, 6, "020F04") == 0 && di0 <= 7) return bcd(285, 2);  // 28.5 C
    if (dataIdHex == "03810000") return bcd(3, 2);
    if (dataIdHex == "03810101") return bcd(1234, 2);
    if (dataIdHex == "04000101") return {0x03, 0x28, 0x10, 0x26};  // WW DD MM YY
    if (dataIdHex == "04000102") return {0x00, 0x30, 0x10};        // ss mm hh
    if (dataIdHex == "04000503") {
        return {static_cast<std::uint8_t>(breakerClosed_ ? 0x00 : 0x10), 0x00};  // bit4: relay open
    }
    if (dataIdHex.compare(0, 7, "0400050") == 0 && di0 >= 1 && di0 <= 7) return {0x00, 0x00};
    if (dataIdHex == "04001504") return std::vector<std::uint8_t>(12, 0x00);
    if (dataIdHex == "07020100") return {static_cast<std::uint8_t>(breakerClosed_ ? 0x01 : 0x00)};
    return {};
}

void Dlt645MockSerialPort::write(const std::vector<std::uint8_t>& bytes) {
    if (!open_) {
        throw std::runtime_error("serial port is not open");
    }
    ++requests_;
    if (localEcho_) {
        pending_.insert(pending_.end(), bytes.begin(), bytes.end());
    }
    std::vector<std::uint8_t> buffer(bytes);
    std::vector<std::uint8_t> request;
    if (!Dlt645Codec::takeFrame(buffer, request)) {
        return;  // not a DL/T 645 frame: a real meter stays silent
    }
    const auto control = request[8];
    std::vector<std::uint8_t> data(request.begin() + 10, request.end() - 2);
    for (auto& byte : data) {
        byte = static_cast<std::uint8_t>(byte - 0x33U);
    }
    if (data.size() < 4U) {
        return;
    }
    static const char* hex = "0123456789ABCDEF";
    std::string dataId;
    for (int i = 3; i >= 0; --i) {
        dataId.push_back(hex[(data[static_cast<std::size_t>(i)] >> 4U) & 0x0FU]);
        dataId.push_back(hex[data[static_cast<std::size_t>(i)] & 0x0FU]);
    }
    std::vector<std::uint8_t> response;
    if (control == 0x11) {
        const auto payload = dataFor(dataId);
        if (payload.empty()) {
            response = reply(request, 0xD1, {0x02});  // ERR bit1: no requested data
        } else {
            std::vector<std::uint8_t> answer(data.begin(), data.begin() + 4);
            append(answer, payload);
            response = reply(request, 0x91, answer);
        }
    } else if (control == 0x14) {
        // DI(4) + PA P0 P1 P2(4) + C0..C3(4) + payload
        const bool immediate = data.size() >= 14U && data[12] == 0x00;
        if (dataId == "06010101" && immediate) breakerClosed_ = false;
        if (dataId == "06010201" && immediate) breakerClosed_ = true;
        response = reply(request, 0x94, {});
    } else {
        response = reply(request, static_cast<std::uint8_t>(control | 0xC0U), {0x02});
    }
    pending_.insert(pending_.end(), response.begin(), response.end());
}

std::vector<std::uint8_t> Dlt645MockSerialPort::read(std::size_t maxBytes, int timeoutMs) {
    (void)timeoutMs;
    if (!open_) {
        throw std::runtime_error("serial port is not open");
    }
    const auto count = std::min(maxBytes, pending_.size());
    std::vector<std::uint8_t> result(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(count));
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(count));
    return result;
}

}  // namespace edge_gateway
