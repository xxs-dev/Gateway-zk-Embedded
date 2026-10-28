// DL/T 645 codec regressions: signed current, strict BCD, block offsets, unknown data types,
// strict addresses, receive resynchronisation and the C=12H follow-up request frame.
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "dlt645_test_support.hpp"
#include "edge_gateway/dlt645_codec.hpp"

using dlt645_test::readReply;
using dlt645_test::require;
using edge_gateway::Dlt645Codec;
using edge_gateway::PointDefinition;

namespace {

PointDefinition point(const std::string& di, const std::string& type, int bytes, double scale, int offset = 0) {
    PointDefinition p;
    p.pointCode = "p";
    p.read.dlt645Di = di;
    p.read.dataType = type;
    p.read.dlt645ByteCount = bytes;
    p.read.dlt645Offset = offset;
    p.read.scale = scale;
    return p;
}

double decode(const PointDefinition& p, const std::vector<std::uint8_t>& payload) {
    return Dlt645Codec::decodeReadResponse(readReply("1", p.read.dlt645Di, payload), p).value;
}

bool throwsWith(const std::function<void()>& action, const std::string& text) {
    try {
        action();
    } catch (const std::exception& ex) {
        return std::string(ex.what()).find(text) != std::string::npos;
    }
    return false;
}

void signedCurrent() {
    // XXX.XXX A, bit 7 of the MSB is the direction: -25.500 A = 82 55 00 (MSB first).
    const auto current = point("02020100", "dlt645_bcd_signed", 3, 0.001);
    require(std::abs(decode(current, {0x00, 0x55, 0x82}) + 25.5) < 1e-9, "-25.5 A must decode as -25.5");
    require(std::abs(decode(current, {0x00, 0x55, 0x02}) - 25.5) < 1e-9, "+25.5 A must decode as 25.5");
    require(std::abs(decode(current, {0x00, 0x55, 0x92}) + 125.5) < 1e-9, "-125.5 A must decode as -125.5");
}

void strictBcd() {
    const auto current = point("02020100", "dlt645_bcd", 3, 0.001);
    const auto voltage = point("02010100", "dlt645_bcd", 2, 0.1);
    for (const auto& payload : std::vector<std::vector<std::uint8_t>>{{0x00, 0x00, 0xC1}, {0x00, 0x00, 0xA1}}) {
        require(throwsWith([&] { decode(current, payload); }, "invalid DLT645 BCD digit"),
                "A..F nibbles in unsigned BCD must be rejected, not read as sign/zero");
    }
    require(throwsWith([&] { decode(voltage, {0x00, 0xF9}); }, "invalid DLT645 BCD digit"), "F nibble must be rejected");
    require(throwsWith([&] { decode(voltage, {0xFF, 0xFF}); }, "invalid DLT645 BCD digit"), "FFFF filler must be rejected");
    require(throwsWith([&] { decode(voltage, {0x1A, 0x22}); }, "invalid DLT645 BCD digit"), "low nibble A must be rejected");
    require(std::abs(decode(voltage, {0x05, 0x22}) - 220.5) < 1e-9, "valid BCD must still decode");
    // 8/9 are digits in unsigned BCD (only *_signed treats bit 7 as sign).
    require(std::abs(decode(current, {0x34, 0x12, 0x80}) - 801.234) < 1e-9, "digit 8 stays a digit in unsigned BCD");
}

void blockOffsets() {
    // 0201FF00: Ua, Ub, Uc (XXX.X each) in one reply.
    const std::vector<std::uint8_t> block = {0x01, 0x22, 0x12, 0x22, 0x23, 0x22};
    require(std::abs(decode(point("0201FF00", "dlt645_bcd", 2, 0.1, 0), block) - 220.1) < 1e-9, "Ua at offset 0");
    require(std::abs(decode(point("0201FF00", "dlt645_bcd", 2, 0.1, 2), block) - 221.2) < 1e-9, "Ub at offset 2");
    require(std::abs(decode(point("0201FF00", "dlt645_bcd", 2, 0.1, 4), block) - 222.3) < 1e-9, "Uc at offset 4");
    require(throwsWith([&] { decode(point("0201FF00", "dlt645_bcd", 2, 0.1, 6), block); }, "shorter"),
            "offset beyond the block must fail");
}

void unknownDataTypesAreRejected() {
    for (const auto* type : {"dlt645_uint32", "dlt645_status_bits", ""}) {
        require(throwsWith([&] { decode(point("04000501", type, 1, 1.0), {0x10}); }, "unsupported DLT645 read dataType"),
                std::string("unknown data type must not decode as BCD: ") + type);
    }
}

void strictAddresses() {
    for (const auto* bad : {"00000000002O", "AAAAAAAAAAAA", "12345678901A", "0000-0000-0012", "", "1234567890123", " 20"}) {
        require(throwsWith([&] { Dlt645Codec::normalizeAddress(bad); }, "DLT645 address"),
                std::string("address with non-digits must be rejected: ") + bad);
    }
    require(Dlt645Codec::normalizeAddress("20") == "000000000020", "short address is left padded");
    require(Dlt645Codec::normalizeAddress("000000000021") == "000000000021", "12 digits kept");
}

void resynchronisation() {
    const auto good = readReply("1", "02010100", {0x01, 0x22});
    std::vector<std::uint8_t> frame;

    // Truncated frame (header + 2 bytes) followed by a complete frame.
    std::vector<std::uint8_t> buffer(good.begin(), good.begin() + 12);
    require(!Dlt645Codec::takeFrame(buffer, frame), "truncated frame alone is incomplete");
    buffer.insert(buffer.end(), good.begin(), good.end());
    require(Dlt645Codec::takeFrame(buffer, frame) && frame == good, "complete frame behind a truncated one must be found");
    require(buffer.empty(), "consumed bytes must leave the buffer");

    // Garbage including a stray 68H, then the frame.
    buffer = {0x00, 0x68, 0x12, 0xFE, 0xFE};
    buffer.insert(buffer.end(), good.begin(), good.end());
    require(Dlt645Codec::takeFrame(buffer, frame) && frame == good, "garbage before the frame must be skipped");

    // Checksum error then a good frame.
    auto corrupt = good;
    corrupt[corrupt.size() - 2] ^= 0x01;
    buffer = corrupt;
    buffer.insert(buffer.end(), good.begin(), good.end());
    require(Dlt645Codec::takeFrame(buffer, frame) && frame == good, "corrupt frame must be skipped");

    // A truncated header announcing a long frame must not hide the real reply.
    buffer = {0x68, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0x91, 0xC8};
    buffer.insert(buffer.end(), good.begin(), good.end());
    require(Dlt645Codec::takeFrame(buffer, frame) && frame == good, "bogus length must not block resync");
    require(std::abs(decode(point("02010100", "dlt645_bcd", 2, 0.1), {0x01, 0x22}) - 220.1) < 1e-9, "decode");
}

void followUpFrame() {
    const auto frame = Dlt645Codec::buildReadFollowUpFrame("20", "04001504", 2);
    std::vector<std::uint8_t> expected = {0x68, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0x12, 0x05,
                                          0x37, 0x48, 0x33, 0x37, 0x35};
    expected.push_back(dlt645_test::checksum(expected));
    expected.push_back(0x16);
    require(frame == expected, "C=12H follow-up frame must carry DI and the sequence number");
}

void readFrameUnchanged() {
    // 68 + address + 68 + 11H + 04H + DI(+33H) + CS + 16H (verified layout, must stay).
    const auto frame = Dlt645Codec::buildReadFrame("20", "02010100");
    std::vector<std::uint8_t> expected = {0x68, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0x11, 0x04,
                                          0x33, 0x34, 0x34, 0x35};
    expected.push_back(dlt645_test::checksum(expected));
    expected.push_back(0x16);
    require(frame == expected, "C=11H read frame layout changed");
}

}  // namespace

int main() {
    try {
        signedCurrent();
        strictBcd();
        blockOffsets();
        unknownDataTypesAreRejected();
        strictAddresses();
        resynchronisation();
        followUpFrame();
        readFrameUnchanged();
        std::cout << "dlt645_codec_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "dlt645_codec_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
