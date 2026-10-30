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

// DL/T 645-1997 items (no sign bits in 1997): energy XXXXXX.XX, voltage XXX V, current XX.XX A,
// active power XX.XXXX kW, reactive power XX.XX kvar, power factor X.XXX. index 0 = total.
std::vector<std::uint8_t> item1997(int group, int item) {
    static const long long voltage[] = {0, 220, 221, 219};
    static const long long current[] = {0, 1234, 1134, 1034};
    static const long long active[] = {15234, 5000, 6000, 4234};
    static const long long reactive[] = {62, 20, 22, 20};
    static const long long powerFactor[] = {925, 928, 937, 906};
    switch (group) {
        case 0xB61: return item >= 1 && item <= 3 ? bcd(voltage[item], 2) : std::vector<std::uint8_t>();
        case 0xB62: return item >= 1 && item <= 3 ? bcd(current[item], 2) : std::vector<std::uint8_t>();
        case 0xB63: return item <= 3 ? bcd(active[item], 3) : std::vector<std::uint8_t>();
        case 0xB64: return item <= 3 ? bcd(reactive[item], 2) : std::vector<std::uint8_t>();
        case 0xB65: return item <= 3 ? bcd(powerFactor[item], 2) : std::vector<std::uint8_t>();
        default: break;
    }
    static const long long energy[] = {1234567, 223456, 345678, 44567};  // 901x, 902x, 911x, 912x
    const int kind = group == 0x901 ? 0 : group == 0x902 ? 1 : group == 0x911 ? 2 : group == 0x912 ? 3 : -1;
    if (kind >= 0 && item <= 4) {
        return bcd(energy[kind] + item * 1000LL, 4);
    }
    return {};
}

std::vector<std::uint8_t> addressBytes(const std::string& address) {
    // 12 digits A5..A0 -> A0..A5
    std::vector<std::uint8_t> bytes;
    for (int i = 10; i >= 0; i -= 2) {
        bytes.push_back(static_cast<std::uint8_t>(((address[static_cast<std::size_t>(i)] - '0') << 4) |
                                                  (address[static_cast<std::size_t>(i + 1)] - '0')));
    }
    return bytes;
}

std::vector<std::uint8_t> reply(const std::string& address, std::uint8_t control, const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> frame = {0x68};
    append(frame, addressBytes(address));
    frame.push_back(0x68);
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

std::string dataIdText(const std::vector<std::uint8_t>& data, std::size_t length) {
    static const char* hex = "0123456789ABCDEF";
    std::string text;
    for (std::size_t i = length; i-- > 0;) {
        text.push_back(hex[(data[i] >> 4U) & 0x0FU]);
        text.push_back(hex[data[i] & 0x0FU]);
    }
    return text;
}

}  // namespace

Dlt645MockSerialPort::Dlt645MockSerialPort(bool localEcho, std::vector<std::string> addresses)
    : localEcho_(localEcho) {
    for (const auto& address : addresses) {
        addresses_.push_back(Dlt645Codec::normalizeAddress(address));
    }
}

std::vector<std::uint8_t> Dlt645MockSerialPort::dataFor(const std::string& dataIdHex) const {
    std::vector<std::uint8_t> data;
    if (dataIdHex.size() == 4) {
        const auto group = static_cast<int>(std::strtol(dataIdHex.substr(0, 3).c_str(), nullptr, 16));
        const auto last = dataIdHex[3];
        if (last == 'F' || last == 'f') {  // block: total (energy/power) or phases, in order
            const int first = group == 0xB61 || group == 0xB62 ? 1 : 0;
            for (int item = first; item <= (group >= 0xB61 ? 3 : 4); ++item) append(data, item1997(group, item));
            return data;
        }
        if (dataIdHex == "C010") return {0x03, 0x28, 0x10, 0x26};  // WW DD MM YY
        if (dataIdHex == "C011") return {0x00, 0x30, 0x10};        // ss mm hh
        return item1997(group, last - '0');
    }
    if (dataIdHex.size() != 8) {
        return {};
    }
    const auto di3 = hexByteAt(dataIdHex, 0);
    const auto di2 = hexByteAt(dataIdHex, 1);
    const auto di1 = hexByteAt(dataIdHex, 2);
    const auto di0 = hexByteAt(dataIdHex, 3);
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

std::vector<std::string> Dlt645MockSerialPort::matchingMeters(const std::vector<std::uint8_t>& request) const {
    const auto requested = Dlt645Codec::frameAddress(request);  // A5..A0, AA = wildcard byte
    if (addresses_.empty()) {
        // Every address answers; a wildcard request is answered by meter 000000000001.
        std::string address;
        for (std::size_t i = 0; i < 12; i += 2) {
            address += requested.compare(i, 2, "AA") == 0 ? std::string("00") : requested.substr(i, 2);
        }
        if (address == "000000000000") address = "000000000001";
        return {address};
    }
    std::vector<std::string> matches;
    for (const auto& address : addresses_) {
        bool match = true;
        for (std::size_t i = 0; i < 12 && match; i += 2) {
            match = requested.compare(i, 2, "AA") == 0 || requested.compare(i, 2, address, i, 2) == 0;
        }
        if (match) matches.push_back(address);
    }
    return matches;
}

std::vector<std::uint8_t> Dlt645MockSerialPort::answer(const std::vector<std::uint8_t>& request, const std::string& address) {
    const auto control = request[8];
    std::vector<std::uint8_t> data(request.begin() + 10, request.end() - 2);
    for (auto& byte : data) {
        byte = static_cast<std::uint8_t>(byte - 0x33U);
    }
    const bool v1997 = control == 0x01 || control == 0x02 || control == 0x04;
    const std::size_t diLength = v1997 ? 2U : 4U;
    const auto exception = static_cast<std::uint8_t>(control | 0xC0U);
    if (control == 0x13) {  // 2007 read address
        return reply(address, 0x93, addressBytes(address));
    }
    if (control == 0x1C) {  // 2007 remote control: PA..P2 C0..C3 N1 N2 N3..N8
        if (data.size() != 16U) return reply(address, exception, {0x01});
        lastRemoteControl_ = data[8];
        lastRemoteControlDeadline_.assign(data.begin() + 10, data.end());
        if (data[8] == 0x1A) breakerClosed_ = false;
        if (data[8] == 0x1C) breakerClosed_ = true;
        return reply(address, 0x9C, {});
    }
    if (data.size() < diLength) {
        return reply(address, exception, {0x01});
    }
    const auto dataId = dataIdText(data, diLength);
    if (control == 0x11 || control == 0x01) {
        auto payload = dataId == "04000401" || dataId == "C032" ? addressBytes(address) : dataFor(dataId);
        if (payload.empty()) {
            return reply(address, exception, {0x02});  // 2007: no requested data; 1997: wrong DI
        }
        std::vector<std::uint8_t> normal(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(diLength));
        append(normal, payload);
        return reply(address, static_cast<std::uint8_t>(control | 0x80U), normal);
    }
    if (control == 0x14 || control == 0x04) {
        // 2007: DI(4) PA..P2(4) C0..C3(4) payload; 1997: DI(2) PA..P2(4) payload
        const std::size_t payloadAt = control == 0x14 ? 12U : 6U;
        const bool immediate = data.size() >= payloadAt + 2U && data[payloadAt] == 0x00;
        if (dataId == "06010101" && immediate) breakerClosed_ = false;
        if (dataId == "06010201" && immediate) breakerClosed_ = true;
        return reply(address, static_cast<std::uint8_t>(control | 0x80U), {});
    }
    return reply(address, exception, {0x02});
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
    if (request[8] == Dlt645Codec::kBroadcastTime) {
        ++broadcasts_;
        lastBroadcastTime_.assign(request.begin() + 10, request.end() - 2);
        for (auto& byte : lastBroadcastTime_) byte = static_cast<std::uint8_t>(byte - 0x33U);
        return;  // broadcast: no reply
    }
    const auto meters = matchingMeters(request);
    if (meters.empty()) {
        return;
    }
    std::vector<std::uint8_t> response = answer(request, meters.front());
    for (std::size_t i = 1; i < meters.size(); ++i) {
        // Several meters answering at once: their bits collide on the bus.
        const auto other = answer(request, meters[i]);
        response.resize(std::max(response.size(), other.size()), 0x00);
        for (std::size_t b = 0; b < other.size(); ++b) {
            response[b] = static_cast<std::uint8_t>(response[b] | other[b]);
        }
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
