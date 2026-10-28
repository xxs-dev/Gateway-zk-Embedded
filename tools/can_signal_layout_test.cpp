// CAN signal layout and load-time validation (driver review, CAN items 1, 2, 6, 7).
//
// Reference vectors: DBC signals decoded/encoded with cantools 45.1.1 (python, strict=False):
//   payloads P0 = A5 3C 96 0F 81 7E C3 5A, P1 = 12 34 56 78 9A BC DE F0, P2 = 01 00 00 00 00 00 00 00
//   SG_ M12  : 3|12@0+   -> 0x53C 0x234 0x100        encode 0x123 -> 01 23 00 ..
//   SG_ M4   : 3|4@0+    -> 0x5   0x2   0x1          encode 0x9   -> 09 00 ..
//   SG_ M16  : 7|16@0+   -> 0xA53C 0x1234 0x0100
//   SG_ M10S : 21|10@0-  -> 0x160 0x167 0            encode -3    -> 00 00 3F D0 ..
//   SG_ I12  : 4|12@1+   -> 0x3CA 0x341 0            encode 0x3CA -> A0 3C ..
//   SG_ M20  : 13|20@0+  -> 0xF2583 0xD159E 0        encode 0xABCDE -> 00 2A F3 78 ..
//   SG_ M64  : 7|64@0+   -> 0xA53C960F817EC35A ...
//   SG_ M1   : 14|1@0+   -> 0 0 0                    encode 1     -> 00 40 ..
//   SG_ M12B : 51|12@0+  -> 0x35A 0xEF0 0            encode 0xBEE -> .. 0B EE
// Mapping to the gateway layout (doc/协议与驱动/CAN接口接入设计.md):
//   Intel    start S -> byteOffset = S / 8, bitOffset = S % 8
//   Motorola start S -> byteOffset = S / 8, bitOffset = 7 - S % 8   (S is the MSB, as in DBC)
// Hand check M12 on P0: MSB = byte0 bit3; bits byte0[3..0] = 0x5, byte1[7..0] = 0x3C -> 0x53C.
// Hand check M10S encode -3: 10-bit two's complement 0x3FD; byte2[5..0] = 111111 (0x3F),
//   byte3[7..4] = 1101 (0xD0).
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "edge_gateway/can_signal_codec.hpp"
#include "edge_gateway/config_loader.hpp"

namespace {

using edge_gateway::CanSignalCodec;
using Bytes = std::vector<std::uint8_t>;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << std::endl;
    }
}

std::string hex(const Bytes& bytes) {
    std::ostringstream out;
    for (const auto byte : bytes) {
        out << std::hex << (byte < 16 ? "0" : "") << static_cast<int>(byte);
    }
    return out.str();
}

const Bytes kP0{0xA5, 0x3C, 0x96, 0x0F, 0x81, 0x7E, 0xC3, 0x5A};
const Bytes kP1{0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
const Bytes kP2{0x01, 0, 0, 0, 0, 0, 0, 0};

edge_gateway::ReadSpec motorola(int dbcStart, int length, const std::string& type = "uint64") {
    edge_gateway::ReadSpec spec;
    spec.dataType = type;
    spec.can.frameId = "0x100";
    spec.can.endian = "big";
    spec.can.byteOffset = dbcStart / 8;
    spec.can.bitOffset = 7 - dbcStart % 8;
    spec.can.bitLength = length;
    return spec;
}

edge_gateway::ReadSpec intel(int dbcStart, int length, const std::string& type = "uint64") {
    auto spec = motorola(0, length, type);
    spec.can.endian = "little";
    spec.can.byteOffset = dbcStart / 8;
    spec.can.bitOffset = dbcStart % 8;
    return spec;
}

edge_gateway::WriteSpec writeOf(const edge_gateway::ReadSpec& read) {
    edge_gateway::WriteSpec spec;
    spec.enable = true;
    spec.dataType = read.dataType;
    spec.can = read.can;
    spec.can.dlc = 8;
    return spec;
}

double decode(const Bytes& payload, const edge_gateway::ReadSpec& spec) {
    return CanSignalCodec::decode(payload, spec).value;
}

void dbcReferenceVectors() {
    struct Vector {
        const char* name;
        edge_gateway::ReadSpec spec;
        double expected[3];
    };
    const std::vector<Vector> vectors = {
        {"M12", motorola(3, 12), {0x53C, 0x234, 0x100}},
        {"M4", motorola(3, 4), {0x5, 0x2, 0x1}},
        {"M16", motorola(7, 16), {0xA53C, 0x1234, 0x0100}},
        {"M10S", motorola(21, 10, "int16"), {0x160, 0x167, 0}},
        {"I12", intel(4, 12), {0x3CA, 0x341, 0}},
        {"M20", motorola(13, 20), {0xF2583, 0xD159E, 0}},
        {"M1", motorola(14, 1), {0, 0, 0}},
        {"M12B", motorola(51, 12), {0x35A, 0xEF0, 0}},
    };
    const Bytes* payloads[3] = {&kP0, &kP1, &kP2};
    for (const auto& vector : vectors) {
        for (int i = 0; i < 3; ++i) {
            const auto actual = decode(*payloads[i], vector.spec);
            check(actual == vector.expected[i], std::string(vector.name) + " decode P" + std::to_string(i) +
                  " = " + std::to_string(actual) + ", DBC/cantools " + std::to_string(vector.expected[i]));
        }
    }
    // 64-bit Motorola is the plain big-endian byte order.
    auto m64 = motorola(7, 64);
    check(CanSignalCodec::decode(kP0, m64).rawHex == "A53C960F817EC35A", "M64 selects all 8 bytes");
    check(decode(kP1, m64) == static_cast<double>(0x123456789ABCDEF0ULL), "M64 decode P1");

    struct Encoded {
        const char* name;
        edge_gateway::ReadSpec spec;
        double value;
        Bytes expected;
    };
    const std::vector<Encoded> encodes = {
        {"M12", motorola(3, 12), 0x123, {0x01, 0x23, 0, 0, 0, 0, 0, 0}},
        {"M4", motorola(3, 4), 0x9, {0x09, 0, 0, 0, 0, 0, 0, 0}},
        {"M10S", motorola(21, 10, "int16"), -3, {0, 0, 0x3F, 0xD0, 0, 0, 0, 0}},
        {"M20", motorola(13, 20, "uint32"), 0xABCDE, {0, 0x2A, 0xF3, 0x78, 0, 0, 0, 0}},
        {"M1", motorola(14, 1, "bool"), 1, {0, 0x40, 0, 0, 0, 0, 0, 0}},
        {"M12B", motorola(51, 12, "uint16"), 0xBEE, {0, 0, 0, 0, 0, 0, 0x0B, 0xEE}},
        {"I12", intel(4, 12, "uint16"), 0x3CA, {0xA0, 0x3C, 0, 0, 0, 0, 0, 0}},
    };
    for (const auto& item : encodes) {
        const auto frame = CanSignalCodec::encode(item.value, writeOf(item.spec));
        check(frame.payload == item.expected, std::string(item.name) + " encode = " + hex(frame.payload) +
              ", DBC/cantools " + hex(item.expected));
        check(decode(frame.payload, item.spec) == item.value, std::string(item.name) + " round trip");
    }

    // Review case: a 4-bit Motorola value in the low nibble of byte 0 (DBC start bit 3) is 1 for
    // payload 01, never the bit-reversed 8 of the old big+lsb0 path.
    check(decode(kP2, motorola(3, 4)) == 1.0, "Motorola nibble 01 decodes as 1");
    // A Motorola write touches only its own bits.
    Bytes base(8, 0xFF);
    CanSignalCodec::encodeInto(base, 0, writeOf(motorola(3, 12, "uint16")));
    check(base == Bytes({0xF0, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}), "Motorola encode keeps other bits");
}

void intelRegression() {
    // Verified-correct Intel behaviour stays: cross-byte, 64-bit, sign extension.
    check(decode(Bytes({0xA5, 0x3C}), intel(4, 12)) == 0x3CA, "Intel 12 bit across bytes");
    check(decode(Bytes({0x05, 0xF0}), intel(4, 12, "int16")) == -256, "Intel signed 12 bit");
    auto flagged = intel(0, 4, "uint8");
    flagged.signedFlag = true;
    check(decode(Bytes({0x0F}), flagged) == -1, "read.signed on uint8");
    check(decode(Bytes(8, 0xFF), intel(0, 64)) == 18446744073709551615.0, "Intel uint64");
    check(decode(Bytes({0, 0, 0, 0, 0, 0, 0, 0x80}), intel(0, 64, "int64")) == -9223372036854775808.0, "Intel int64 min");
}

// COMM202600103 正华 detector: byte-aligned 16-bit big-endian signals (byteOffset n, bitOffset 0)
// must still read bytes n, n+1 as MSB, LSB, and the Intel bit flags must not move.
void zhenghuaDetectorRegression(const std::string& root) {
    const auto path = root +
        "/products/ems/stationary/1.0/projects/COMM202600103/runtime/devices/device_can0_zhenghua_detector.json";
    std::ifstream input(path.c_str(), std::ios::binary);
    std::stringstream text;
    text << input.rdbuf();
    const auto config = edge_gateway::ConfigLoader::loadFromText(text.str());
    const Bytes frame{0x12, 0x34, 0xA5, 0x5A, 0xC3, 0x3C, 0x81, 0x18};
    int bigSignals = 0;
    int checked = 0;
    for (const auto& meter : config.meters) {
        for (const auto& point : meter.points) {
            const auto& read = point.read;
            if (!read.enable || read.can.frameId.empty()) continue;
            const auto b = static_cast<std::size_t>(read.can.byteOffset);
            double expected = 0;
            if (read.can.endian == "big") {
                check(read.can.bitOffset == 0 && read.can.bitLength == 16, point.pointCode + " is byte aligned");
                expected = frame[b] * 256.0 + frame[b + 1];
                ++bigSignals;
            } else if (read.can.bitLength == 8) {
                expected = frame[b];
            } else {
                expected = (frame[b] >> read.can.bitOffset) & 1U;
            }
            expected = expected * read.scale + read.offset;
            check(decode(frame, read) == expected, point.pointCode + " value changed");
            ++checked;
        }
    }
    check(bigSignals == 4 && checked == 34, "103 detector signals covered: big=" + std::to_string(bigSignals) +
          " total=" + std::to_string(checked));
    check(decode(frame, motorola(7, 16, "uint16")) == 0x1234, "aligned 16-bit big endian = bytes 0,1");
}

void frameIdsAndTypes() {
    check(CanSignalCodec::parseFrameId("0x18FF50E5", true) == 0x18FF50E5U, "extended id");
    check(CanSignalCodec::parseFrameId("0X7ff", false) == 0x7FFU, "standard id upper bound");
    for (const char* bad : {"123", "18FF50E5", " 0x123", "0x", "0x1G", "0x123 ", "0x200000000"}) {
        bool rejected = false;
        try { (void)CanSignalCodec::parseFrameId(bad, true); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, std::string("frameId \"") + bad + "\" must be rejected");
    }
    bool rejected = false;
    try { (void)CanSignalCodec::parseFrameId("0x18FF50E5", false); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "standard frame above 0x7FF must be rejected");
    for (const char* type : {"int", "double", "float", "short", "byte", "bit", ""}) {
        check(!CanSignalCodec::isSignalDataType(type), std::string("dataType \"") + type + "\" is not a CAN signal type");
        auto spec = intel(0, 16, type);
        rejected = false;
        try { (void)decode(kP0, spec); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, std::string("decode with dataType \"") + type + "\" must fail");
    }
    for (const char* endian : {"motorola", "BE", "Big", "intel", ""}) {
        auto spec = intel(0, 16, "uint16");
        spec.can.endian = endian;
        rejected = false;
        try { (void)decode(kP0, spec); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, std::string("endian \"") + endian + "\" must be rejected");
    }
    auto msb0 = motorola(7, 16, "uint16");
    msb0.can.bitOrder = "msb0";
    rejected = false;
    try { (void)decode(kP0, msb0); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "bitOrder msb0 must be rejected (one layout definition)");
}

void invalidRawValuesAndTemplates() {
    auto rpm = intel(0, 16, "uint16");
    rpm.scale = 0.125;
    rpm.can.invalidRawValues = {0xFFFF, 0xFE00};
    bool flagged = false;
    try { (void)decode(Bytes({0xFF, 0xFF}), rpm); } catch (const edge_gateway::CanInvalidRawValue&) { flagged = true; }
    check(flagged, "J1939 0xFFFF must be reported as invalid, not 8191.875 rpm");
    check(decode(Bytes({0x10, 0x27}), rpm) == 1250.0, "valid rpm decodes");

    edge_gateway::CanSignalSpec can;
    can.dlc = 8;
    can.payloadTemplate = "00112233445566aA";
    check(CanSignalCodec::templatePayload(can) == Bytes({0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0xAA}), "template");
    for (const char* bad : {"0011", "00 11 22 33 44 55 66 77", "001122334455667G"}) {
        can.payloadTemplate = bad;
        bool rejected = false;
        try { (void)CanSignalCodec::templatePayload(can); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, std::string("payloadTemplate \"") + bad + "\" must be rejected");
    }
}

std::string canConfig(const std::string& protocolExtra, const std::string& meterExtra, const std::string& points) {
    return std::string(R"({"schemaVersion":"1.0.0","machineCode":"GW","protocol":{"type":"can_socketcan","can":{"interfaceName":"can0")") +
        protocolExtra + R"(}},"memoryStore":{"sharedMemoryName":"can_layout_test"},"meters":[{"meterCode":"DEV1","deviceName":"d")" +
        meterExtra + R"(,"points":[)" + points + "]}]}";
}

std::string readPoint(const std::string& dataType, const std::string& can) {
    return R"({"index":1,"pointCode":"sig","enabled":true,"read":{"enable":true,"dataType":")" + dataType +
        R"(","can":{)" + can + "}}}";
}

std::string writePoint(int index, const std::string& code, const std::string& dataType, const std::string& can) {
    return R"({"index":)" + std::to_string(index) + R"(,"pointCode":")" + code +
        R"(","enabled":true,"write":{"enable":true,"dataType":")" + dataType + R"(","can":{)" + can + "}}}";
}

void expectRejected(const std::string& json, const std::string& needle, const std::string& what) {
    try {
        (void)edge_gateway::ConfigLoader::loadFromText(json);
        check(false, what + ": config was accepted");
    } catch (const std::invalid_argument& ex) {
        check(std::string(ex.what()).find(needle) != std::string::npos,
              what + ": message \"" + ex.what() + "\" lacks \"" + needle + "\"");
        check(std::string(ex.what()).find("sig") != std::string::npos || needle.find("meter") != std::string::npos ||
              std::string(ex.what()).find("DEV1") != std::string::npos, what + ": message names the point");
    }
}

void loadTimeValidation() {
    const std::string good = R"("frameId":"0x18FF50E5","extended":true,"byteOffset":0,"bitLength":16,"endian":"big")";
    (void)edge_gateway::ConfigLoader::loadFromText(canConfig("", "", readPoint("uint16", good)));
    expectRejected(canConfig("", "", readPoint("uint16", R"("frameId":"18FF50E5","extended":true)")),
                   "read.can.frameId", "frameId without 0x");
    expectRejected(canConfig("", "", readPoint("uint16", R"("frameId":"123")")), "read.can.frameId", "decimal frameId");
    expectRejected(canConfig("", "", readPoint("uint16", R"("frameId":"0x18FF50E5","extended":false)")),
                   "0x7FF", "standard frame id above 0x7FF");
    expectRejected(canConfig("", "", readPoint("uint16", R"("frameId":"0x123","endian":"motorola")")),
                   "endian", "endian motorola");
    expectRejected(canConfig("", "", readPoint("int", R"("frameId":"0x123")")), "read.dataType", "dataType int");
    expectRejected(canConfig("", "", readPoint("double", R"("frameId":"0x123")")), "read.dataType", "dataType double");
    expectRejected(canConfig("", "", readPoint("float32", R"("frameId":"0x123","bitLength":16)")),
                   "32 bits", "float32 with 16 bits");
    expectRejected(canConfig("", "", readPoint("uint16", R"("frameId":"0x123","byteOffset":7)")),
                   "read.can", "signal beyond 8 bytes");
    expectRejected(canConfig("", "", readPoint("uint16", R"("frameId":"0x123","bitOrder":"msb0")")),
                   "bitOrder", "bitOrder msb0");
    expectRejected(canConfig("", "", readPoint("uint8", R"("frameId":"0x123","invalidRawValues":[256])")),
                   "invalidRawValues", "invalid raw value wider than the signal");
    expectRejected(canConfig("", R"(,"onlineFrameIds":["18FF50E5"])", readPoint("uint16", good)),
                   "meter DEV1", "onlineFrameIds without 0x");
    // Writes: frame consistency, FD per frame, templates.
    const auto w1 = writePoint(10, "run", "bool", R"("frameId":"0x321","dlc":8,"bitLength":1)");
    const auto w2 = writePoint(11, "sig", "uint8", R"("frameId":"0x321","dlc":8,"byteOffset":1,"payloadTemplate":"0000000000000000")");
    (void)edge_gateway::ConfigLoader::loadFromText(canConfig("", "", w1 + "," + w2));
    expectRejected(canConfig("", "", w1 + "," +
                   writePoint(11, "sig", "uint8", R"("frameId":"0x321","dlc":6,"byteOffset":1)")),
                   "dlc", "same frame with another dlc");
    expectRejected(canConfig("", "", writePoint(11, "sig", "uint8", R"("frameId":"0x321","dlc":12,"fd":true)")),
                   "fdEnabled", "FD frame without fdEnabled");
    (void)edge_gateway::ConfigLoader::loadFromText(canConfig(R"(,"fdEnabled":true)", "",
        writePoint(11, "sig", "uint8", R"("frameId":"0x321","dlc":12,"fd":true,"byteOffset":11)")));
    expectRejected(canConfig(R"(,"fdEnabled":true)", "", writePoint(11, "sig", "uint8", R"("frameId":"0x321","dlc":12)")),
                   "write.can.dlc", "classic frame longer than 8 bytes");
    expectRejected(canConfig("", "", writePoint(11, "sig", "uint8", R"("frameId":"0x321","payloadTemplate":"00")")),
                   "payloadTemplate", "short payloadTemplate");
    expectRejected(canConfig("", "", writePoint(11, "sig", "", R"("frameId":"0x321")")), "write.dataType", "write without dataType");
    // Pulse signals (round 2): integer/bool only, idle value encodable, never active in the template.
    (void)edge_gateway::ConfigLoader::loadFromText(canConfig("", "",
        writePoint(10, "reset", "bool", R"("frameId":"0x321","bitLength":1,"pulse":true)") + "," +
        writePoint(11, "sig", "uint8", R"("frameId":"0x321","byteOffset":1,"payloadTemplate":"0000000000000000")")));
    expectRejected(canConfig("", "", writePoint(11, "sig", "float32", R"("frameId":"0x321","pulse":true)")),
                   "write.can.pulse", "float pulse");
    expectRejected(canConfig("", "", writePoint(11, "sig", "uint8", R"("frameId":"0x321","pulse":true,"pulseIdleValue":300)")),
                   "pulseIdleValue", "pulse idle value out of range");
    expectRejected(canConfig("", "",
        writePoint(11, "sig", "bool", R"("frameId":"0x321","bitLength":1,"pulse":true)") + "," +
        writePoint(12, "other", "uint8", R"("frameId":"0x321","byteOffset":1,"payloadTemplate":"0100000000000000")")),
                   "pulse", "template holding the pulse active");
    expectRejected(canConfig("", "", R"({"index":1,"pointCode":"sig","enabled":true,"read":{"enable":true,"dataType":"interface_online"},"write":{"enable":true,"dataType":"bool","can":{"frameId":"0x1"}}})"),
                   "read-only", "writable online point");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: can_signal_layout_test <repo root>" << std::endl;
        return 2;
    }
    try {
        dbcReferenceVectors();
        intelRegression();
        zhenghuaDetectorRegression(argv[1]);
        frameIdsAndTypes();
        invalidRawValuesAndTemplates();
        loadTimeValidation();
    } catch (const std::exception& ex) {
        std::cerr << "can_signal_layout_test exception: " << ex.what() << std::endl;
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "can_signal_layout_test passed" << std::endl;
    return 0;
}
