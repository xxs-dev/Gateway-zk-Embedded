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
    return 0;
}
