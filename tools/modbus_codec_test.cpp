#include "edge_gateway/modbus_codec.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireNear(double actual, double expected, const char* message) {
    if (std::fabs(actual - expected) > 0.000001) {
        throw std::runtime_error(message);
    }
}

edge_gateway::PointDefinition readablePoint(const char* dataType) {
    edge_gateway::PointDefinition point;
    point.read.enable = true;
    point.read.function = 3;
    point.read.length = 1;
    point.read.dataType = dataType;
    point.read.byteOrder = "AB";
    point.read.scale = 1.0;
    return point;
}

edge_gateway::PointDefinition writablePoint(const char* dataType, const char* byteOrder) {
    edge_gateway::PointDefinition point;
    point.write.enable = true;
    point.write.function = 16;
    point.write.dataType = dataType;
    point.write.byteOrder = byteOrder;
    return point;
}

bool encodeRejected(const edge_gateway::PointDefinition& point, double value) {
    try {
        (void)edge_gateway::ModbusCodec::encodeWriteValue(value, point);
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

// driver-review: a 32-bit write whose byteOrder names fewer bytes than the type used to write only the
// high word (70000 -> one register 0x0001). Encoding now has the same length contract as decoding.
void verifyWriteByteOrderMustCoverType() {
    using edge_gateway::ModbusCodec;
    require(encodeRejected(writablePoint("uint32", "AB"), 70000.0), "uint32 with a 2-byte order must be rejected");
    require(encodeRejected(writablePoint("float32", "BA"), 1.5), "float32 with a 2-byte order must be rejected");
    require(encodeRejected(writablePoint("uint16", "ABCD"), 1.0), "uint16 with a 4-byte order must be rejected");
    require(encodeRejected(writablePoint("uint32", "AABB"), 1.0), "a byte order with repeated letters must be rejected");
    const auto abcd = ModbusCodec::encodeWriteValue(70000.0, writablePoint("uint32", "ABCD"));
    require(abcd.size() == 2 && abcd[0] == 0x0001 && abcd[1] == 0x1170, "uint32 ABCD must write both registers");
    const auto cdab = ModbusCodec::encodeWriteValue(70000.0, writablePoint("uint32", "CDAB"));
    require(cdab.size() == 2 && cdab[0] == 0x1170 && cdab[1] == 0x0001, "uint32 CDAB must put the low word first");
    require(ModbusCodec::canonicalByteOrder("int32") == "ABCD" && ModbusCodec::canonicalByteOrder("uint16") == "AB" &&
            ModbusCodec::canonicalByteOrder("float64") == "ABCDEFGH" && ModbusCodec::canonicalByteOrder("dlt645_bcd").empty(),
            "canonical byte orders must follow the type width");
}

void verify64BitTypes() {
    using edge_gateway::ModbusCodec;
    for (const char* order : {"ABCDEFGH", "GHEFCDAB", "HGFEDCBA"}) {
        for (const char* type : {"uint64", "int64", "float64"}) {
            const double value = std::string(type) == "int64" ? -1234567890123.0
                : std::string(type) == "float64" ? 3.25e12 : 9876543210987.0;
            const auto registers = ModbusCodec::encodeWriteValue(value, writablePoint(type, order));
            require(registers.size() == 4, "64-bit values use four registers");
            auto point = readablePoint(type);
            point.read.length = 4;
            point.read.byteOrder = order;
            requireNear(ModbusCodec::decodeReadValue(registers, point).value, value, "64-bit value must round-trip");
        }
    }
    auto point = writablePoint("uint64", "ABCDEFGH");
    require(!ModbusCodec::validateWriteValue(1e17, point).ok, "uint64 writes beyond exact double precision must be rejected");
    require(!ModbusCodec::validateWriteValue(-1.0, point).ok, "negative uint64 must be rejected");
}

// driver-review: FC06 read-modify-write of a register bit honors the register's byte order.
void verifyRegisterBitHonorsByteOrder() {
    using edge_gateway::ModbusCodec;
    require(ModbusCodec::setRegisterBit(0x0000, 3, true, "AB") == 0x0008, "AB bit 3 is 0x0008");
    // BA: the decoded word is the byte-swapped register, so bit 3 lives in the high device byte.
    require(ModbusCodec::setRegisterBit(0x0000, 3, true, "BA") == 0x0800, "BA bit 3 must land in the swapped byte");
    require(ModbusCodec::setRegisterBit(0xFFFF, 3, false, "BA") == 0xF7FF, "BA clear must keep the other bits");
    auto point = readablePoint("bit");
    point.read.byteOrder = "BA";
    point.read.bit = 3;
    requireNear(ModbusCodec::decodeReadValue({0x0800}, point).value, 1.0, "the written BA bit must read back as set");
}

}  // namespace

int main() {
    using edge_gateway::ModbusCodec;

    const auto booleanPoint = readablePoint("bool");
    requireNear(ModbusCodec::decodeReadValue({0}, booleanPoint).value, 0.0,
                "zero bool register must decode to zero");
    requireNear(ModbusCodec::decodeReadValue({16408}, booleanPoint).value, 1.0,
                "legacy non-zero bool register 16408 must decode to one");
    requireNear(ModbusCodec::decodeReadValue({28703}, booleanPoint).value, 1.0,
                "legacy non-zero bool register 28703 must decode to one");
    require(ModbusCodec::decodeReadValue({28703}, booleanPoint).rawHex == "701F",
            "bool decode must preserve the source register in rawHex");

    auto bitPoint = readablePoint("bit");
    bitPoint.read.bit = 3;
    requireNear(ModbusCodec::decodeReadValue({8}, bitPoint).value, 1.0,
                "register bit decoding must remain unchanged");
    bitPoint.read.dataType = "bool";
    requireNear(ModbusCodec::decodeReadValue({8}, bitPoint).value, 1.0,
                "bool with explicit bit must preserve bit decoding");
    requireNear(ModbusCodec::decodeReadValue({16}, bitPoint).value, 0.0,
                "bool with explicit bit must not decode the whole register");
    auto coilPoint = readablePoint("bool");
    for (int function : {1, 2}) {
        coilPoint.read.function = function;
        requireNear(ModbusCodec::decodeReadValue({1}, coilPoint).value, 1.0,
                    "bool coil/discrete input must remain supported");
        requireNear(ModbusCodec::decodeReadValue({0}, coilPoint).value, 0.0,
                    "zero bool coil/discrete input must remain false");
    }
    bitPoint.read.bit = 16;
    bool invalidBitRejected = false;
    try { ModbusCodec::decodeReadValue({8}, bitPoint); }
    catch (const std::invalid_argument&) { invalidBitRejected = true; }
    require(invalidBitRejected, "out-of-range bool bit must be rejected");

    edge_gateway::PointDefinition int16Write;
    int16Write.write.enable = true;
    int16Write.write.dataType = "int16";
    require(ModbusCodec::validateWriteValue(32767.0, int16Write).ok, "int16 max must be writable");
    require(!ModbusCodec::validateWriteValue(40000.0, int16Write).ok, "int16 overflow must be rejected, not wrapped");
    auto uint16Write = int16Write;
    uint16Write.write.dataType = "uint16";
    require(!ModbusCodec::validateWriteValue(-1.0, uint16Write).ok, "negative uint16 must be rejected");
    uint16Write.write.scale = 0.1;
    require(!ModbusCodec::validateWriteValue(7000.0, uint16Write).ok, "scaled raw overflow must be rejected");
    verifyWriteByteOrderMustCoverType();
    verify64BitTypes();
    verifyRegisterBitHonorsByteOrder();
    return 0;
}
