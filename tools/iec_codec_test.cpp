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

    const auto sFrame = IecCodec::buildIec104SFrame(5);
    requireTrue(sFrame == std::vector<std::uint8_t>({0x68, 0x04, 0x01, 0x00, 0x0A, 0x00}), "IEC104 S frame");
    requireTrue(IecCodec::isIec104SFrame(sFrame), "IEC104 S frame detection");
    requireTrue(IecCodec::iec104ReceiveSequence(sFrame) == 5, "IEC104 S frame receive sequence");

    PointDefinition commandPoint;
    commandPoint.index = 500001;
    commandPoint.pointCode = "remote_close";
    commandPoint.address = 3001;
    commandPoint.write.enable = true;
    commandPoint.write.dataType = "single_command";
    commandPoint.write.iec.typeId = 45;
    commandPoint.write.iec.ioa = 3001;
    commandPoint.write.iec.commonAddress = 1;
    commandPoint.write.iec.selectBeforeExecute = true;
    const auto selectFrame = IecCodec::buildIec104ControlCommand(config, commandPoint, 1.0, 2, 5, true);
    requireTrue(IecCodec::isIec104IFrame(selectFrame), "IEC104 control command I frame");
    const auto selectAsdu = IecCodec::iec104AsduPayload(selectFrame);
    requireTrue(selectAsdu.size() == 10, "IEC104 single command ASDU size");
    requireTrue(selectAsdu[0] == 45, "IEC104 single command type");
    requireTrue(selectAsdu.back() == 0x81, "IEC104 select single command SCO");

    const std::vector<std::uint8_t> timedAsdu = {
        30, 1,
        3, 0,
        1, 0,
        0xE9, 0x03, 0x00,
        0x01,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x1A
    };
    const auto timedDecoded = IecCodec::decodeAsdu(timedAsdu, config);
    requireTrue(timedDecoded.values.size() == 1, "IEC104 timed ASDU value count");
    requireTrue(timedDecoded.values.front().ioa == 1001, "IEC104 timed IOA");
    requireNear(timedDecoded.values.front().value, 1.0, "IEC104 timed single value");

    const auto parameterFrame = IecCodec::buildIec104ParameterCommand(config, 5001, 112, 12.5, 0x01, 3, 1, 6);
    requireTrue(IecCodec::isIec104IFrame(parameterFrame), "IEC104 parameter I frame");
    const auto parameterDecoded = IecCodec::decodeAsdu(IecCodec::iec104AsduPayload(parameterFrame), config);
    requireTrue(parameterDecoded.parameters.size() == 1, "IEC104 parameter decode count");
    requireTrue(parameterDecoded.parameters.front().ioa == 5001, "IEC104 parameter IOA");
    requireNear(parameterDecoded.parameters.front().value, 12.5, "IEC104 parameter float");
    requireTrue(parameterDecoded.parameters.front().qualifier == 0x01, "IEC104 parameter qualifier");

    const auto activationFrame = IecCodec::buildIec104ParameterActivationCommand(config, 5001, 0x02, 4, 1);
    const auto activationDecoded = IecCodec::decodeAsdu(IecCodec::iec104AsduPayload(activationFrame), config);
    requireTrue(activationDecoded.parameters.size() == 1, "IEC104 parameter activation count");
    requireTrue(activationDecoded.parameters.front().typeId == 113, "IEC104 parameter activation type");

    const auto fileCall = IecCodec::buildIec104FileCallCommand(config, 0, 7, 1, 0x01, 5, 1);
    const auto fileCallDecoded = IecCodec::decodeAsdu(IecCodec::iec104AsduPayload(fileCall), config);
    requireTrue(fileCallDecoded.fileSegments.size() == 1, "IEC104 file call decode count");
    requireTrue(fileCallDecoded.fileSegments.front().nameOfFile == 7, "IEC104 file call NOF");

    const std::vector<std::uint8_t> protectionAsdu = {
        38, 1,
        3, 0,
        1, 0,
        0x71, 0x17, 0x00,
        0x02,
        0xE8, 0x03,
        0, 0, 0, 0, 1, 1, 0x1A
    };
    const auto protectionDecoded = IecCodec::decodeAsdu(protectionAsdu, config);
    requireTrue(protectionDecoded.protectionEvents.size() == 1, "IEC104 protection event count");
    requireTrue(protectionDecoded.protectionEvents.front().ioa == 6001, "IEC104 protection IOA");
    requireTrue(protectionDecoded.protectionEvents.front().eventState == 2, "IEC104 protection event state");
    requireTrue(protectionDecoded.protectionEvents.front().elapsedTimeMs == 1000, "IEC104 protection elapsed time");

    const std::vector<std::uint8_t> fileSegmentAsdu = {
        125, 1,
        13, 0,
        1, 0,
        0, 0, 0,
        0x07, 0x00,
        0x01,
        0x80,
        0x03,
        'a', 'b', 'c'
    };
    const auto fileSegmentDecoded = IecCodec::decodeAsdu(fileSegmentAsdu, config);
    requireTrue(fileSegmentDecoded.fileSegments.size() == 1, "IEC104 file segment count");
    requireTrue(fileSegmentDecoded.fileSegments.front().data.size() == 3, "IEC104 file segment length");
    requireTrue(fileSegmentDecoded.fileSegments.front().data.front() == 'a', "IEC104 file segment data");

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
          "commonAddress": 1,
          "clockSyncIntervalSec": 300
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
        },
        {
          "index": 2,
          "pointCode": "iec104_remote_close",
          "name": "IEC104 remote close",
          "category": "control",
          "address": 3001,
          "enabled": true,
          "read": {
            "enable": false
          },
          "write": {
            "enable": true,
            "dataType": "single_command",
            "iec": {
              "ioa": 3001,
              "typeId": 45,
              "commonAddress": 1,
              "selectBeforeExecute": true,
              "waitActivationTermination": true,
              "timeoutMs": 1500
            }
          }
        }
      ],
      "meters": []
    })JSON");
    requireTrue(loaded.protocol.type == "iec103", "config protocol type");
    requireTrue(loaded.protocol.iec.transportMode == "tcp", "config IEC103 TCP mode");
    requireTrue(loaded.protocol.iec.backgroundReceive, "config IEC background receive default");
    requireTrue(loaded.protocol.iec.clockSyncIntervalSec == 300, "config IEC clock sync interval");
    requireTrue(!loaded.points.empty(), "config IEC points");
    requireTrue(loaded.points.front().read.iec.functionType == 160, "config IEC103 FUN");
    requireTrue(loaded.points.size() == 2, "config IEC command point count");
    requireTrue(loaded.points.back().write.iec.selectBeforeExecute, "config IEC write SBO");
    requireTrue(loaded.points.back().write.iec.waitActivationTermination, "config IEC write activation termination");

    std::cout << "iec codec tests passed" << std::endl;
    return EXIT_SUCCESS;
}
