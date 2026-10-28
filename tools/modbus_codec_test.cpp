#include "edge_gateway/modbus_codec.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>
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
    return 0;
}
