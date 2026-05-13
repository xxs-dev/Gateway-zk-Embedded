#include "edge_gateway/can_signal_codec.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireNear(double actual, double expected, double tolerance, const char* message) {
    if (std::fabs(actual - expected) > tolerance) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    using namespace edge_gateway;

    ReadSpec littleEndian;
    littleEndian.dataType = "uint16";
    littleEndian.scale = 0.5;
    littleEndian.can.frameId = "0x18FF50E5";
    littleEndian.can.extended = true;
    littleEndian.can.byteOffset = 1;
    littleEndian.can.bitOffset = 0;
    littleEndian.can.bitLength = 16;
    littleEndian.can.endian = "little";

    const std::vector<std::uint8_t> frame{0x00, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00};
    const auto decoded = CanSignalCodec::decode(frame, littleEndian);
    requireNear(decoded.value, 0x1234 * 0.5, 0.0001, "uint16 little endian decode failed");
    require(decoded.rawHex == "3412", "raw hex should include selected bytes");

    ReadSpec bitSignal;
    bitSignal.dataType = "bool";
    bitSignal.can.byteOffset = 0;
    bitSignal.can.bitOffset = 3;
    bitSignal.can.bitLength = 1;
    bitSignal.can.bitOrder = "lsb0";
    requireNear(CanSignalCodec::decode(std::vector<std::uint8_t>{0x08}, bitSignal).value, 1.0, 0.0001, "lsb bit decode failed");

    WriteSpec writeSpec;
    writeSpec.enable = true;
    writeSpec.dataType = "uint16";
    writeSpec.scale = 0.5;
    writeSpec.can.frameId = "0x123";
    writeSpec.can.extended = false;
    writeSpec.can.dlc = 8;
    writeSpec.can.byteOffset = 2;
    writeSpec.can.bitOffset = 0;
    writeSpec.can.bitLength = 16;
    writeSpec.can.endian = "big";
    const auto encoded = CanSignalCodec::encode(100.0, writeSpec);
    require(encoded.frameId == 0x123, "frame id parse failed");
    require(!encoded.extended, "standard frame should not be extended");
    require(encoded.payload.size() == 8, "encoded dlc should be 8");
    require(encoded.payload[2] == 0x00 && encoded.payload[3] == 0xC8, "uint16 big endian encode failed");

    return 0;
}
