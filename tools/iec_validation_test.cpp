// Load-time validation of IEC 101/103/104 device configs (finding 12): definitions that cannot
// work or would silently produce wrong values are rejected with the field named.
#include "edge_gateway/config_loader.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string config(const std::string& type, const std::string& iec, const std::string& points,
    const std::string& transport = "\"tcp\":{\"host\":\"127.0.0.1\",\"port\":2404}") {
    return "{\"schemaVersion\":\"1.0.0\",\"meterCode\":\"IEC_TEST\",\"protocol\":{\"type\":\"" + type + "\"," +
        transport + ",\"iec\":{" + iec + "}},\"points\":[" + points + "],\"meters\":[]}";
}

std::string readPoint(const std::string& iec, const std::string& dataType = "float32") {
    return "{\"index\":1,\"pointCode\":\"p1\",\"name\":\"p1\",\"enabled\":true,"
        "\"read\":{\"enable\":true,\"dataType\":\"" + dataType + "\",\"iec\":{" + iec + "}}}";
}

std::string writePoint(const std::string& write) {
    return "{\"index\":2,\"pointCode\":\"c1\",\"name\":\"c1\",\"enabled\":true,\"read\":{\"enable\":false},"
        "\"write\":{\"enable\":true," + write + "}}";
}

const std::string kSerial = "\"transport\":{\"serialPort\":\"/dev/ttyS1\",\"baudRate\":9600}";

void expectReject(const std::string& json, const std::string& fragment, const std::string& what) {
    try {
        (void)edge_gateway::ConfigLoader::loadFromText(json);
    } catch (const std::invalid_argument& ex) {
        require(std::string(ex.what()).find(fragment) != std::string::npos,
            what + ": message must name '" + fragment + "', got: " + ex.what());
        return;
    }
    throw std::runtime_error(what + ": config must be rejected");
}

void expectAccept(const std::string& json, const std::string& what) {
    try {
        (void)edge_gateway::ConfigLoader::loadFromText(json);
    } catch (const std::exception& ex) {
        throw std::runtime_error(what + ": config must load: " + ex.what());
    }
}

}  // namespace

int main() {
    try {
        const std::string mode104 = "\"transportMode\":\"tcp\"";
        const std::string mode101 = "\"transportMode\":\"serial\"";
        const std::string point104 = readPoint("\"ioa\":1001,\"typeId\":1", "single_point");
        const std::string point103 = readPoint("\"functionType\":160,\"informationNumber\":148,\"typeId\":9,\"elementIndex\":2");

        expectAccept(config("iec104", mode104, point104 + "," + readPoint("\"ioa\":2001,\"typeId\":36")), "valid iec104");
        expectAccept(config("iec101", mode101, point104, kSerial), "valid iec101");
        expectAccept(config("iec103", mode101, point103, kSerial), "valid iec103");
        expectAccept(config("iec104", mode104, writePoint(
            "\"dataType\":\"double_command\",\"allowedValues\":[1,2],\"iec\":{\"ioa\":3001,\"selectBeforeExecute\":true}")),
            "valid double command");

        // Unbalanced transmission only.
        expectReject(config("iec101", mode101 + ",\"balanced\":true", point104, kSerial), "balanced", "iec101 balanced");
        expectReject(config("iec103", mode101 + ",\"balanced\":true", point103, kSerial), "balanced", "iec103 balanced");
        expectReject(config("iec101", "\"transportMode\":\"tcp\"", point104), "serial", "iec101 over tcp");

        // Field sizes and addresses.
        expectReject(config("iec104", mode104 + ",\"cotSize\":1", point104), "cotSize=2", "iec104 COT size");
        expectReject(config("iec101", mode101 + ",\"ioaSize\":4", point104, kSerial), "ioaSize", "iec101 IOA size");
        expectReject(config("iec101", mode101 + ",\"linkAddressSize\":0", point104, kSerial), "linkAddressSize",
            "unbalanced iec101 needs a link address");
        expectReject(config("iec103", mode101 + ",\"linkAddressSize\":2", point103, kSerial), "linkAddressSize",
            "iec103 link address is one octet");
        expectReject(config("iec101", mode101 + ",\"linkAddress\":255", point104, kSerial), "linkAddress",
            "broadcast link address");
        expectReject(config("iec104", mode104 + ",\"commonAddress\":70000", point104), "commonAddress",
            "common address overflow");
        expectReject(config("iec104", mode104 + ",\"interrogationQualifier\":5", point104), "interrogationQualifier",
            "QOI out of range");
        expectReject(config("iec104", mode104, readPoint("\"ioa\":16777216,\"typeId\":13")), "IOA", "IOA overflow");

        // Monitoring points.
        expectReject(config("iec104", mode104, readPoint("\"ioa\":1001,\"typeId\":99")), "typeId=99", "unknown typeId");
        expectReject(config("iec104", mode104, readPoint("\"typeId\":13")), "read.iec.ioa", "point without IOA");
        expectReject(config("iec104", mode104, readPoint("\"ioa\":1001,\"typeId\":13,\"elementIndex\":3")),
            "elementIndex", "elementIndex on a non-packed type");
        expectReject(config("iec103", mode101, readPoint("\"typeId\":9"), kSerial), "functionType", "103 point without FUN/INF");
        expectReject(config("iec103", mode101, readPoint("\"functionType\":1,\"informationNumber\":2,\"typeId\":13"), kSerial),
            "typeId=13", "104 type on a 103 point");

        // Command points.
        expectReject(config("iec101", mode101, writePoint("\"dataType\":\"single_command\",\"iec\":{\"ioa\":3001}"), kSerial),
            "iec104 only", "commands on iec101");
        expectReject(config("iec104", mode104, writePoint("\"iec\":{\"ioa\":3001,\"typeId\":52}")), "typeId=52",
            "unsupported command type");
        expectReject(config("iec104", mode104, writePoint(
            "\"dataType\":\"double_command\",\"allowedValues\":[0,1],\"iec\":{\"ioa\":3001}")), "only 1 and 2",
            "double command values 0/3");
        expectReject(config("iec104", mode104, writePoint(
            "\"iec\":{\"ioa\":3001,\"typeId\":51,\"selectBeforeExecute\":true}")), "select", "bitstring SBO");
        expectReject(config("iec104", mode104, writePoint(
            "\"dataType\":\"single_command\",\"iec\":{\"ioa\":3001,\"qualifier\":3}")), "QU", "SCO qualifier bits");

        std::cout << "IEC validation tests passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "IEC validation test failed: " << ex.what() << std::endl;
        return 1;
    }
}
