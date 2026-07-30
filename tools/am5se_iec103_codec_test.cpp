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
    config.transportMode = "am5se_passive_tcp";
    config.linkAddress = 2;
    config.linkAddressSize = 1;
    config.commonAddress = 2;
    config.deviceFunctionType = 1;
    config.interrogationCot = 9;
    config.returnInformationIdentifier = 0xE9;

    requireTrue(
        IecCodec::buildIec103ResetCommunicationFrame(config) ==
            std::vector<std::uint8_t>({0x10, 0x40, 0x02, 0x42, 0x16}),
        "AM5SE reset communication frame");
    requireTrue(
        IecCodec::buildIec103ClassRequestFrame(config, 1, false) ==
            std::vector<std::uint8_t>({0x10, 0x5A, 0x02, 0x5C, 0x16}),
        "AM5SE class 1 request FCB=0");
    requireTrue(
        IecCodec::buildIec103ClassRequestFrame(config, 2, true) ==
            std::vector<std::uint8_t>({0x10, 0x7B, 0x02, 0x7D, 0x16}),
        "AM5SE class 2 request FCB=1");
    requireTrue(
        IecCodec::buildIec103GeneralInterrogationFrame(config) ==
            std::vector<std::uint8_t>({
                0x68, 0x09, 0x09, 0x68, 0x73, 0x02, 0x07, 0x81,
                0x09, 0x02, 0xFF, 0x00, 0xE9, 0xF0, 0x16
            }),
        "AM5SE general interrogation includes RII");
    requireTrue(
        IecCodec::buildIec103RecordingDirectoryFrame(config, false) ==
            std::vector<std::uint8_t>({
                0x68, 0x0D, 0x0D, 0x68, 0x53, 0x02, 0x18, 0x81,
                0x1F, 0x02, 0xFF, 0x00, 0x18, 0x01, 0x00, 0x00,
                0x00, 0x27, 0x16
            }),
        "AM5SE ASDU24 directory request");
    requireTrue(
        IecCodec::buildIec103ComtradeFileCallFrame(config, 15, 0x51, true) ==
            std::vector<std::uint8_t>({
                0x68, 0x0A, 0x0A, 0x68, 0x73, 0x02, 0x44, 0x81,
                0x00, 0x02, 0xFF, 0x51, 0x0F, 0x00, 0x9B, 0x16
            }),
        "AM5SE ASDU68 CFG request");

    const std::vector<std::uint8_t> directoryUserData = {
        0x08, 0x02, 0x17, 0x02, 0x1F, 0x02, 0xFF, 0x00,
        0x02, 0x00, 0x01, 0x1C, 0xD0, 0x0D, 0x0F, 0x12, 0x02, 0x14,
        0x03, 0x00, 0x01, 0xBE, 0x2C, 0x0E, 0x0F, 0x12, 0x02, 0x14
    };
    const auto directory = IecCodec::decodeIec103RecordingDirectory(directoryUserData, config);
    requireTrue(directory.size() == 2, "AM5SE ASDU23 directory count");
    requireTrue(directory[0].fan == 2 && directory[1].fan == 3, "AM5SE ASDU23 FAN values");
    requireTrue(directory[0].timestampMs > 0, "AM5SE ASDU23 timestamp");

    const std::vector<std::uint8_t> chunkUserData = {
        0x08, 0x02, 0x50, 0x81, 0x14, 0x02, 0x01, 0x51,
        0x04, 0x80, 'C', 'F', 'G'
    };
    const auto chunk = IecCodec::decodeIec103ComtradeChunk(chunkUserData, config);
    requireTrue(chunk.fileType == 0x51, "AM5SE ASDU80 CFG type");
    requireTrue(chunk.packetNumber == 4, "AM5SE ASDU80 packet number");
    requireTrue(chunk.lastPacket, "AM5SE ASDU80 final marker");
    requireTrue(chunk.data == std::vector<std::uint8_t>({'C', 'F', 'G'}), "AM5SE ASDU80 payload");

    Iec103ComtradeAssembler assembler(16);
    assembler.add({0x51, 0, false, {'A', 'B'}, {}});
    assembler.add({0x51, 0, false, {'A', 'B'}, {}});
    assembler.add({0x51, 1, true, {'C'}, {}});
    requireTrue(assembler.complete(), "COMTRADE assembler final state");
    requireTrue(assembler.bytes() == std::vector<std::uint8_t>({'A', 'B', 'C'}), "COMTRADE assembler bytes");
    bool gapRejected = false;
    try {
        Iec103ComtradeAssembler gapAssembler(16);
        gapAssembler.add({0x52, 1, true, {'X'}, {}});
    } catch (const std::runtime_error&) {
        gapRejected = true;
    }
    requireTrue(gapRejected, "COMTRADE assembler rejects packet gaps");

    const std::vector<std::uint8_t> yxUserData = {
        0x08, 0x02, 0x2C, 0x02, 0x09, 0x02, 0x01, 0x64,
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x50, 0x00, 0x00, 0x00, 0x00,
        0xE9
    };
    const auto yx = IecCodec::decodeIec103Data(yxUserData, config);
    requireTrue(yx.size() == 32, "AM5SE ASDU44 expands two 16-bit blocks");
    requireNear(yx[20].value, 1.0, "AM5SE ASDU44 INF120 state");
    requireNear(yx[22].value, 1.0, "AM5SE ASDU44 INF122 state");
    requireTrue(yx[20].informationNumber == 120, "AM5SE ASDU44 INF mapping");

    const std::vector<std::uint8_t> ycUserData = {
        0x08, 0x02, 0x0A, 0x81, 0x02, 0x02, 0xFE, 0xF1, 0x00, 0x02,
        0x00, 0x00, 0x01, 0x03, 0x02, 0x01, 0x18, 0x00,
        0x00, 0x01, 0x01, 0x07, 0x04, 0x01, 0x0E, 0x60, 0x2A, 0x3A
    };
    const auto yc = IecCodec::decodeIec103Data(ycUserData, config);
    requireTrue(yc.size() == 1, "AM5SE ASDU10 skips group-size entry");
    requireTrue(yc.front().functionType == 1 && yc.front().informationNumber == 140,
        "AM5SE ASDU10 maps GIN 0/1 to FUN/INF 1/140");
    requireNear(yc.front().value, 0.00065, "AM5SE ASDU10 float value");

    const auto discovery = IecCodec::buildAm5seUdpDiscoveryPacket(0, "KY");
    requireTrue(discovery.size() == 41, "AM5SE UDP discovery length");
    requireTrue(discovery[0] == 0xFF && discovery[1] == 0x01, "AM5SE UDP discovery marker");
    requireTrue(discovery[9] == 'K' && discovery[10] == 'Y', "AM5SE UDP station name");

    const auto loaded = ConfigLoader::loadFromText(R"JSON({
      "protocol": {
        "type": "iec103",
        "iec": {
          "transportMode": "am5se_passive_tcp",
          "listenAddress": "192.168.1.10",
          "listenPort": 1048,
          "udpBroadcastAddress": "192.168.1.255",
          "udpPort": 1032,
          "deviceFunctionType": 1,
          "recordingDirectory": "/tmp/recordings",
          "recordingMaxFileBytes": 1048576
        }
      },
      "points": [],
      "meters": []
    })JSON");
    requireTrue(loaded.protocol.iec.transportMode == "am5se_passive_tcp", "AM5SE config mode");
    requireTrue(loaded.protocol.iec.listenPort == 1048, "AM5SE config listen port");
    requireTrue(loaded.protocol.iec.deviceFunctionType == 1, "AM5SE config FUN");
    requireTrue(loaded.protocol.iec.recordingMaxFileBytes == 1048576U, "AM5SE config file limit");

    std::cout << "AM5SE IEC103 codec tests passed" << std::endl;
    return EXIT_SUCCESS;
}
