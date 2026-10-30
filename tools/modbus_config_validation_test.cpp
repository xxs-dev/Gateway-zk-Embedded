// driver-review: Modbus point definitions are checked when the config is loaded. Definitions that cannot
// work or would silently produce wrong values are rejected with the point and field named; the shipped
// configs still load (config_corpus_load).
#include <iostream>
#include <stdexcept>
#include <string>

#include "edge_gateway/config_loader.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string device(const std::string& point, const std::string& baud = "9600") {
    return std::string("{\"schemaVersion\":\"1.1.0\",\"machineCode\":\"GW\",\"meterCode\":\"M\",\"deviceName\":\"M\",") +
        "\"protocol\":{\"type\":\"modbus_rtu\",\"slave\":1,\"transport\":{\"serialPort\":\"/dev/ttyX\",\"baudRate\":" +
        baud + "}}," +
        "\"points\":[{\"index\":1,\"pointCode\":\"P1\",\"name\":\"P1\",\"address\":10," + point + "}],\"meters\":[]}";
}

edge_gateway::DeviceConfig load(const std::string& text) {
    return edge_gateway::ConfigLoader::loadFromText(text);
}

void expectRejected(const std::string& point, const std::string& fragment, const std::string& baud = "9600") {
    try {
        (void)load(device(point, baud));
    } catch (const std::invalid_argument& error) {
        const std::string message = error.what();
        require(message.find(fragment) != std::string::npos,
                "rejection should mention '" + fragment + "', got: " + message);
        require(message.find("P1") != std::string::npos || baud != "9600", "rejection should name the point: " + message);
        return;
    }
    require(false, "config should be rejected: " + point + " baud " + baud);
}

std::string withTransport(const std::string& transport) {
    return std::string("{\"protocol\":{\"type\":\"modbus_rtu\",\"slave\":1,\"transport\":") + transport +
        "},\"points\":[],\"meters\":[]}";
}

void expectTransportRejected(const std::string& transport, const std::string& fragment) {
    try {
        (void)load(withTransport(transport));
    } catch (const std::invalid_argument& error) {
        const std::string message = error.what();
        require(message.find(fragment) != std::string::npos,
                "transport rejection should mention '" + fragment + "', got: " + message);
        return;
    }
    require(false, "transport should be rejected: " + transport);
}

// transport.rs485: off by default; ranges and the RTS level pair are checked at load.
void rs485Config() {
    const auto defaults = load(withTransport("{\"serialPort\":\"/dev/ttyX\"}"));
    require(!defaults.protocol.transport.rs485.enabled, "rs485 must be disabled by default");
    const auto configured = load(withTransport(
        "{\"serialPort\":\"/dev/ttyX\",\"rs485\":{\"enabled\":true,\"rtsOnSend\":false,\"rtsAfterSend\":true,"
        "\"delayRtsBeforeSendMs\":1,\"delayRtsAfterSendMs\":100}}"));
    const auto& rs485 = configured.protocol.transport.rs485;
    require(rs485.enabled && !rs485.rtsOnSend && rs485.rtsAfterSend && rs485.delayRtsBeforeSendMs == 1 &&
                rs485.delayRtsAfterSendMs == 100,
            "rs485 fields must be loaded as configured");
    expectTransportRejected("{\"rs485\":{\"enabled\":true,\"delayRtsAfterSendMs\":101}}", "delayRtsAfterSendMs must be an integer between 0 and 100");
    expectTransportRejected("{\"rs485\":{\"delayRtsBeforeSendMs\":-1}}", "0 and 100");
    expectTransportRejected("{\"rs485\":{\"enabled\":true,\"rtsAfterSend\":true}}", "must differ");
    expectTransportRejected("{\"rs485\":{\"enable\":true}}", "transport.rs485.enable is not a known field");
    // Types: exact JSON types only; nothing is coerced.
    expectTransportRejected("{\"rs485\":true}", "transport.rs485 must be an object");
    expectTransportRejected("{\"rs485\":null}", "transport.rs485 must be an object");
    expectTransportRejected("{\"rs485\":{\"enabled\":\"true\"}}", "transport.rs485.enabled must be true or false");
    expectTransportRejected("{\"rs485\":{\"enabled\":1}}", "transport.rs485.enabled must be true or false");
    expectTransportRejected("{\"rs485\":{\"rtsOnSend\":null}}", "transport.rs485.rtsOnSend must be true or false");
    expectTransportRejected("{\"rs485\":{\"delayRtsBeforeSendMs\":1.5}}", "delayRtsBeforeSendMs must be an integer");
    expectTransportRejected("{\"rs485\":{\"delayRtsAfterSendMs\":\"2\"}}", "delayRtsAfterSendMs must be an integer");
    // Mutually exclusive: the RTS level while sending and after sending must differ.
    expectTransportRejected("{\"rs485\":{\"rtsOnSend\":false}}", "must differ");
    // Disabled with other fields is accepted (nothing is applied).
    const auto disabled = load(withTransport("{\"rs485\":{\"enabled\":false,\"rtsOnSend\":false,\"rtsAfterSend\":true}}"));
    require(!disabled.protocol.transport.rs485.enabled, "an explicitly disabled rs485 stays disabled");
    // Kernel direction control only exists on a local serial line.
    for (const auto* protocol : {"{\"type\":\"modbus_tcp\",\"tcp\":{\"host\":\"127.0.0.1\",\"port\":502},",
                                 "{\"type\":\"iec104\",\"tcp\":{\"host\":\"127.0.0.1\",\"port\":2404},",
                                 "{\"type\":\"iec103\",\"iec\":{\"transportMode\":\"tcp\"},\"tcp\":{\"host\":\"127.0.0.1\",\"port\":2404},"}) {
        const auto text = std::string("{\"protocol\":") + protocol + "\"transport\":{\"rs485\":{\"enabled\":true}}},\"points\":[],\"meters\":[]}";
        bool rejected = false;
        try {
            (void)load(text);
        } catch (const std::invalid_argument& error) {
            rejected = std::string(error.what()).find("transport.rs485.enabled needs a serial line") != std::string::npos;
            require(rejected, std::string("unexpected rejection: ") + error.what());
        }
        require(rejected, "rs485 on a network protocol must be rejected: " + text);
    }
    const auto iecSerial = load("{\"protocol\":{\"type\":\"iec101\",\"iec\":{\"transportMode\":\"serial\"},"
                                "\"transport\":{\"serialPort\":\"/dev/ttyX\",\"rs485\":{\"enabled\":true}}},\"points\":[],\"meters\":[]}");
    require(iecSerial.protocol.transport.rs485.enabled, "an IEC serial line accepts rs485");
}

}  // namespace

int main() {
    try {
        // Valid: byteOrder omitted defaults to the natural order of the type, also for writes.
        const auto ok = load(device(
            "\"read\":{\"enable\":true,\"function\":3,\"length\":2,\"dataType\":\"uint32\"},"
            "\"write\":{\"enable\":true,\"function\":16,\"dataType\":\"uint32\"}"));
        require(ok.points[0].read.byteOrder == "ABCD", "omitted read.byteOrder of uint32 should be ABCD");
        require(ok.points[0].write.byteOrder == "ABCD", "omitted write.byteOrder of uint32 should be ABCD, not AB");
        const auto wide = load(device("\"read\":{\"enable\":true,\"function\":3,\"length\":4,\"dataType\":\"float64\"}"));
        require(wide.points[0].read.byteOrder == "ABCDEFGH", "float64 should default to ABCDEFGH");

        expectRejected("\"read\":{\"enable\":true,\"function\":3,\"length\":2,\"dataType\":\"uint32\",\"byteOrder\":\"AB\"}",
                       "read.byteOrder");
        expectRejected("\"read\":{\"enable\":true,\"function\":3,\"length\":1,\"dataType\":\"uint32\"}", "read.length");
        expectRejected("\"read\":{\"enable\":true,\"function\":3,\"length\":2,\"dataType\":\"int32\",\"byteOrder\":\"AACD\"}",
                       "read.byteOrder");
        expectRejected("\"read\":{\"enable\":true,\"function\":3,\"length\":1,\"dataType\":\"uint16\",\"signed\":true}",
                       "read.signed");
        expectRejected("\"read\":{\"enable\":true,\"function\":3,\"length\":1,\"dataType\":\"word\"}", "read.dataType");
        expectRejected("\"read\":{\"enable\":true,\"function\":7,\"length\":1,\"dataType\":\"uint16\"}", "read.function");
        expectRejected("\"read\":{\"enable\":true,\"function\":3,\"length\":1,\"dataType\":\"bit\"}", "read.bit");
        expectRejected("\"write\":{\"enable\":true,\"function\":16,\"dataType\":\"uint32\",\"byteOrder\":\"AB\"}",
                       "write.byteOrder");
        expectRejected("\"write\":{\"enable\":true,\"function\":6,\"dataType\":\"int32\"}", "function 6");
        expectRejected("\"write\":{\"enable\":true,\"function\":5,\"dataType\":\"uint16\"}", "function 5");
        expectRejected("\"write\":{\"enable\":true,\"function\":6,\"dataType\":\"uint16\",\"min\":10,\"max\":1}", "write.min");
        expectRejected("\"write\":{\"enable\":true,\"function\":6,\"dataType\":\"bit\"}", "read.bit");
        expectRejected("\"read\":{\"enable\":true,\"function\":3,\"length\":1,\"dataType\":\"uint16\",\"scale\":0.1},"
                       "\"write\":{\"enable\":true,\"function\":6,\"dataType\":\"uint16\"}", "write.scale");
        expectRejected("\"read\":{\"enable\":true,\"function\":3,\"length\":1,\"dataType\":\"uint16\"}", "baudRate", "12345");
        // Same register read and written with the same scale is fine.
        (void)load(device("\"read\":{\"enable\":true,\"function\":3,\"length\":1,\"dataType\":\"uint16\",\"scale\":0.1},"
                          "\"write\":{\"enable\":true,\"function\":6,\"dataType\":\"uint16\",\"scale\":0.1}"));
        rs485Config();
        const auto decimal = load(device(R"("read":{"enable":true,"function":3,"length":2,"dataType":"uint32_base10000","invalidRawWords":[32766]})"));
        require(decimal.points[0].read.byteOrder == "ABCD", "decimal accumulator defaults to full width");
        expectRejected(R"("read":{"enable":true,"function":3,"length":1,"dataType":"uint32_base10000"})", "read.length");
        expectRejected(R"("read":{"enable":true,"function":3,"length":2,"dataType":"uint32_base10000","signed":true})", "read.signed");
        expectRejected(R"("write":{"enable":true,"function":16,"dataType":"uint32_base10000","byteOrder":"ABCD"})", "write.dataType");
    } catch (const std::exception& ex) {
        std::cerr << "modbus_config_validation_test failed: " << ex.what() << std::endl;
        return 1;
    }
    std::cout << "modbus_config_validation_test passed" << std::endl;
    return 0;
}
