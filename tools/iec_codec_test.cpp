#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/iec_codec.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void requireTrue(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireNear(double actual, double expected, const std::string& message) {
    if (std::fabs(actual - expected) > 1e-6) {
        throw std::runtime_error(message + " actual=" + std::to_string(actual));
    }
}

}  // namespace

int main() {
    using namespace edge_gateway;

    IecProtocolConfig config;
    config.cotSize = 2;
    config.caSize = 2;
    config.ioaSize = 3;
    config.commonAddress = 1;

    const std::vector<std::uint8_t> asdu = {
        13, 1,
        3, 0,
        1, 0,
        0xD1, 0x07, 0x00,
        0x00, 0x00, 0x48, 0x41,
        0x00
    };
    const auto decoded = IecCodec::decodeAsdu(asdu, config);
    requireTrue(decoded.values.size() == 1, "IEC104 ASDU value count");
    requireTrue(decoded.values.front().ioa == 2001, "IEC104 IOA");
    requireNear(decoded.values.front().value, 12.5, "IEC104 float value");

    PointDefinition point;
    point.address = 2001;
    point.read.enable = true;
    point.read.dataType = "measured_float";
    point.read.scale = 2.0;
    point.read.offset = 1.0;
    point.read.iec.typeId = 13;
    point.read.iec.commonAddress = 1;
    requireTrue(IecCodec::pointMatches(point, decoded.values.front()), "IEC point match by address fallback");
    const auto pointValue = IecCodec::decodePointValue(point, decoded.values.front());
    requireNear(pointValue.value, 26.0, "IEC point scale offset");

    const std::vector<std::uint8_t> iec103UserData = {
        0x73, 0x01,
        9, 1, 3, 1,
        160, 1,
        0x34, 0x12, 0x00, 0x00, 0x00
    };
    config.linkAddressSize = 1;
    const auto iec103 = IecCodec::decodeIec103Data(iec103UserData, config);
    requireTrue(iec103.size() == 1, "IEC103 value count");
    requireTrue(iec103.front().functionType == 160, "IEC103 FUN");
    requireTrue(iec103.front().informationNumber == 1, "IEC103 INF");
    requireNear(iec103.front().value, 0x1234, "IEC103 measured value");

    const auto loaded = ConfigLoader::loadFromText(R"JSON({
      "schemaVersion": "1.0.0",
      "enabled": true,
      "protocol": {
        "type": "iec103",
        "tcp": {
          "host": "127.0.0.1",
          "port": 2404
        },
        "iec": {
          "transportMode": "tcp",
          "linkAddress": 1,
          "linkAddressSize": 1,
          "commonAddress": 1
        }
      },
      "points": [
        {
          "index": 1,
          "pointCode": "iec103_fun160_inf1",
          "name": "IEC103 FUN160 INF1",
          "category": "telemetry",
          "enabled": true,
          "read": {
            "enable": true,
            "dataType": "int16",
            "iec": {
              "functionType": 160,
              "informationNumber": 1,
              "typeId": 9,
              "commonAddress": 1
            }
          }
        }
      ],
      "meters": []
    })JSON");
    requireTrue(loaded.protocol.type == "iec103", "config protocol type");
    requireTrue(loaded.protocol.iec.transportMode == "tcp", "config IEC103 TCP mode");
    requireTrue(!loaded.points.empty(), "config IEC points");
    requireTrue(loaded.points.front().read.iec.functionType == 160, "config IEC103 FUN");

    std::cout << "iec codec tests passed" << std::endl;
    return EXIT_SUCCESS;
}
