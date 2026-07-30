#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "edge_gateway/dlt645_client.hpp"
#include "edge_gateway/dlt645_codec.hpp"
#include "edge_gateway/dlt645_collector.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/serial_port.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::uint8_t checksum(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t sum = 0;
    for (const auto byte : bytes) {
        sum += byte;
    }
    return static_cast<std::uint8_t>(sum & 0xFF);
}

std::vector<std::uint8_t> readResponse(
    const std::string& meterAddress,
    const std::string& dataIdHex,
    const std::vector<std::uint8_t>& payload = {0x00, 0x00}
) {
    const auto normalizedAddress = edge_gateway::Dlt645Codec::normalizeAddress(meterAddress);
    const auto dataId = edge_gateway::Dlt645Codec::parseDataId(dataIdHex);

    std::vector<std::uint8_t> frame = {0x68};
    for (int i = static_cast<int>(normalizedAddress.size()) - 2; i >= 0; i -= 2) {
        const auto high = static_cast<std::uint8_t>(normalizedAddress[static_cast<std::size_t>(i)] - '0');
        const auto low = static_cast<std::uint8_t>(normalizedAddress[static_cast<std::size_t>(i + 1)] - '0');
        frame.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    frame.push_back(0x68);
    frame.push_back(0x91);
    frame.push_back(static_cast<std::uint8_t>(4U + payload.size()));
    for (const auto byte : dataId) {
        frame.push_back(static_cast<std::uint8_t>(byte + 0x33));
    }
    for (const auto byte : payload) {
        frame.push_back(static_cast<std::uint8_t>(byte + 0x33));
    }
    frame.push_back(checksum(frame));
    frame.push_back(0x16);
    return frame;
}

std::vector<std::uint8_t> writeResponse(
    const std::vector<std::uint8_t>& request,
    bool rejected,
    std::uint8_t error = 0x04
) {
    const auto start = std::find(request.begin(), request.end(), static_cast<std::uint8_t>(0x68));
    require(start != request.end() && std::distance(start, request.end()) >= 12, "invalid DLT645 request frame");
    std::vector<std::uint8_t> frame = {0x68};
    frame.insert(frame.end(), start + 1, start + 7);
    frame.push_back(0x68);
    frame.push_back(rejected ? 0xD4 : 0x94);
    frame.push_back(rejected ? 0x01 : 0x00);
    if (rejected) {
        frame.push_back(static_cast<std::uint8_t>(error + 0x33U));
    }
    frame.push_back(checksum(frame));
    frame.push_back(0x16);
    return frame;
}

class ImmediateDlt645SerialPort : public edge_gateway::ISerialPort {
public:
    void open() override {
        open_ = true;
    }

    void close() override {
        open_ = false;
    }

    bool isOpen() const override {
        return open_;
    }

    void write(const std::vector<std::uint8_t>& bytes) override {
        require(open_, "write on closed serial port");
        writes_.push_back(bytes);
        writeTimes_.push_back(std::chrono::steady_clock::now());
        const auto frameStart = std::find(bytes.begin(), bytes.end(), static_cast<std::uint8_t>(0x68));
        require(frameStart != bytes.end() && std::distance(frameStart, bytes.end()) >= 10, "invalid DLT645 request");
        if (*(frameStart + 8) == 0x14) {
            chunks_.push_back(writeResponse(bytes, rejectWrites_));
        } else if (respondToReads_) {
            const auto address = writes_.size() == 2 ? "000000000002" : "000000000001";
            chunks_.push_back(readResponse(address, readDataId_, readPayload_));
        }
    }

    std::vector<std::uint8_t> read(std::size_t maxBytes, int timeoutMs) override {
        require(open_, "read on closed serial port");
        (void)timeoutMs;
        if (chunks_.empty()) {
            return {};
        }
        auto chunk = chunks_.front();
        chunks_.erase(chunks_.begin());
        if (chunk.size() <= maxBytes) {
            return chunk;
        }
        std::vector<std::uint8_t> head(chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(maxBytes));
        chunks_.insert(chunks_.begin(), std::vector<std::uint8_t>(
            chunk.begin() + static_cast<std::ptrdiff_t>(maxBytes),
            chunk.end()
        ));
        return head;
    }

    const std::vector<std::vector<std::uint8_t>>& writes() const {
        return writes_;
    }

    const std::vector<std::chrono::steady_clock::time_point>& writeTimes() const {
        return writeTimes_;
    }

    void rejectWrites(bool value) {
        rejectWrites_ = value;
    }

    void setReadResponse(
        const std::string& dataId,
        const std::vector<std::uint8_t>& payload
    ) {
        readDataId_ = dataId;
        readPayload_ = payload;
        respondToReads_ = true;
    }

    void respondToReads(bool value) {
        respondToReads_ = value;
    }

private:
    bool open_ = false;
    std::vector<std::vector<std::uint8_t>> writes_;
    std::vector<std::vector<std::uint8_t>> chunks_;
    std::vector<std::chrono::steady_clock::time_point> writeTimes_;
    bool rejectWrites_ = false;
    bool respondToReads_ = true;
    std::string readDataId_ = "00010000";
    std::vector<std::uint8_t> readPayload_ = {0x00, 0x00};
};

edge_gateway::PointDefinition dlt645BitPoint(
    std::uint32_t index,
    const std::string& pointCode,
    int bit
) {
    edge_gateway::PointDefinition point;
    point.index = index;
    point.pointCode = pointCode;
    point.name = pointCode;
    point.category = "alarm";
    point.enabled = true;
    point.read.enable = true;
    point.read.dataType = "dlt645_bitfield_le";
    point.read.dlt645Di = "04001504";
    point.read.dlt645ByteCount = 12;
    point.read.bit = bit;
    point.read.intervalMs = 100;
    point.read.cachePolicy.storeLatest = true;
    point.read.cachePolicy.ttlMs = 600000;
    return point;
}

edge_gateway::DeviceConfig dlt645CollectorConfig(const std::string& storeName) {
    edge_gateway::DeviceConfig config;
    config.machineCode = "GW_DLT645_CACHE_TEST";
    config.meterCode = "BREAKER_20";
    config.address = "20";
    config.memoryStore.sharedMemoryName = storeName;
    config.points = {
        dlt645BitPoint(645001, "BREAKER_EVENT_BIT_0", 0),
        dlt645BitPoint(645002, "BREAKER_EVENT_BIT_1", 1)
    };
    return config;
}

void verifyCollectorCachesSuccessfulReadPerDataId() {
    const std::string storeName = "dlt645_collector_success_cache_test_store";
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
    {
        edge_gateway::MemoryPointStore store(storeName);
        edge_gateway::SerialPortOptions options;
        options.device = "test";
        options.timeoutMs = 5;
        auto serial = std::make_shared<ImmediateDlt645SerialPort>();
        serial->setReadResponse("04001504", {0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
        auto client = std::make_shared<edge_gateway::Dlt645Client>(serial, options);
        edge_gateway::Dlt645Collector collector(dlt645CollectorConfig(storeName), store, client);

        const auto result = collector.collectOnce(1000);
        require(serial->writes().size() == 1, "same successful DLT645 DI must be read once per cycle");
        require(result.values.size() == 2, "both points sharing a successful DLT645 DI must be decoded");
        require(result.values[0].quality == 1 && result.values[0].value == 1.0,
                "first bit should decode from the shared DLT645 response");
        require(result.values[1].quality == 1 && result.values[1].value == 0.0,
                "second bit should decode from the shared DLT645 response");
    }
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
}

void verifyCollectorCachesFailedReadPerDataId() {
    const std::string storeName = "dlt645_collector_failure_cache_test_store";
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
    {
        edge_gateway::MemoryPointStore store(storeName);
        edge_gateway::SerialPortOptions options;
        options.device = "test";
        options.timeoutMs = 1;
        auto serial = std::make_shared<ImmediateDlt645SerialPort>();
        serial->respondToReads(false);
        auto client = std::make_shared<edge_gateway::Dlt645Client>(serial, options);
        edge_gateway::Dlt645Collector collector(dlt645CollectorConfig(storeName), store, client);

        bool failed = false;
        try {
            (void)collector.collectOnce(1000);
        } catch (const std::runtime_error& ex) {
            failed = std::string(ex.what()).find("DLT645 response timeout") != std::string::npos;
        }
        require(failed, "a cycle with only DLT645 timeouts must report the read failure");
        require(serial->writes().size() == 1, "same failed DLT645 DI must be read once per cycle");

        const auto first = store.getLatestByIndex(645001, 1000);
        const auto second = store.getLatestByIndex(645002, 1000);
        require(static_cast<bool>(first) && first->quality == 0,
                "first point sharing a failed DLT645 DI must be stored with bad quality");
        require(static_cast<bool>(second) && second->quality == 0,
                "second point sharing a failed DLT645 DI must be stored with bad quality");
    }
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
}

void verifyFrameIntervalIsScopedToMeterAddress() {
    edge_gateway::SerialPortOptions options;
    options.device = "test";
    options.timeoutMs = 50;
    options.frameIntervalMs = 200;

    auto serial = std::make_shared<ImmediateDlt645SerialPort>();
    edge_gateway::Dlt645Client client(serial, options);

    (void)client.readData("1", "00010000");
    (void)client.readData("2", "00010000");
    (void)client.readData("000000000001", "00010000");

    require(serial->writes().size() == 3, "expected one write per DLT645 read");
    const auto firstToSecondMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        serial->writeTimes()[1] - serial->writeTimes()[0]
    ).count();
    const auto firstToThirdMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        serial->writeTimes()[2] - serial->writeTimes()[0]
    ).count();
    require(firstToSecondMs < 140, "different DLT645 meter addresses should not be delayed by frameIntervalMs");
    require(firstToThirdMs >= 180, "same DLT645 meter address should honor frameIntervalMs");
}

void verifyConfiguredWakeupPreambleIsSent() {
    edge_gateway::SerialPortOptions options;
    options.device = "test";
    options.timeoutMs = 50;
    options.wakeupBytes = 4;

    auto serial = std::make_shared<ImmediateDlt645SerialPort>();
    edge_gateway::Dlt645Client client(serial, options);
    (void)client.readData("20", "02010100");

    require(serial->writes().size() == 1, "expected one DLT645 write");
    const auto& wireFrame = serial->writes().front();
    require(wireFrame.size() == 20, "expected four wakeup bytes plus sixteen-byte read frame");
    require(
        wireFrame[0] == 0xFE && wireFrame[1] == 0xFE &&
        wireFrame[2] == 0xFE && wireFrame[3] == 0xFE,
        "configured DLT645 wakeup bytes must be FE"
    );
    require(wireFrame[4] == 0x68, "DLT645 frame must follow wakeup preamble");
}

void verifyBinaryAndBitfieldDecoding() {
    edge_gateway::PointDefinition point;
    point.read.dlt645Di = "04000501";
    point.read.dlt645ByteCount = 2;
    point.read.dataType = "dlt645_uint_le";
    const auto wordResponse = readResponse("20", point.read.dlt645Di, {0x34, 0x12});
    const auto word = edge_gateway::Dlt645Codec::decodeReadResponse(wordResponse, point);
    require(word.value == 4660.0, "DLT645 little-endian status word decode mismatch");

    point.read.dlt645Di = "04001504";
    point.read.dlt645ByteCount = 12;
    point.read.dataType = "dlt645_bitfield_le";
    point.read.bit = 71;
    std::vector<std::uint8_t> status(12, 0);
    status[8] = 0x80;
    const auto bitResponse = readResponse("20", point.read.dlt645Di, status);
    const auto bit = edge_gateway::Dlt645Codec::decodeReadResponse(bitResponse, point);
    require(bit.value == 1.0, "DLT645 96-bit event status extraction mismatch");

    point.read.bit = 72;
    const auto clearBit = edge_gateway::Dlt645Codec::decodeReadResponse(bitResponse, point);
    require(clearBit.value == 0.0, "DLT645 clear event bit extraction mismatch");
}

void verifyWriteFrameAndPayloadEncoding() {
    edge_gateway::WriteSpec scheduled;
    scheduled.dataType = "dlt645_scheduled_control";
    scheduled.dlt645.dataType = "dlt645_scheduled_control";
    scheduled.dlt645.byteCount = 2;
    scheduled.dlt645.unit = 2;
    const auto immediate = edge_gateway::Dlt645Codec::encodeWritePayload(0, scheduled);
    require(immediate == std::vector<std::uint8_t>({0x00, 0x02}), "scheduled immediate payload mismatch");
    const auto delayed = edge_gateway::Dlt645Codec::encodeWritePayload(25, scheduled);
    require(delayed == std::vector<std::uint8_t>({0x25, 0x02}), "scheduled minute payload mismatch");

    edge_gateway::WriteSpec bcd;
    bcd.dataType = "dlt645_bcd";
    bcd.scale = 0.1;
    bcd.dlt645.dataType = "dlt645_bcd";
    bcd.dlt645.byteCount = 2;
    require(
        edge_gateway::Dlt645Codec::encodeWritePayload(123.4, bcd) ==
            std::vector<std::uint8_t>({0x34, 0x12}),
        "DLT645 BCD write payload mismatch"
    );

    const auto frame = edge_gateway::Dlt645Codec::buildWriteFrame(
        "20",
        "06010101",
        "02000000",
        "01020304",
        immediate
    );
    std::vector<std::uint8_t> expected = {
        0x68, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0x14, 0x0E,
        0x34, 0x34, 0x34, 0x39,
        0x35, 0x33, 0x33, 0x33,
        0x34, 0x35, 0x36, 0x37,
        0x33, 0x35
    };
    expected.push_back(checksum(expected));
    expected.push_back(0x16);
    require(frame == expected, "DLT645 C=14H write frame mismatch");
}

void verifyWriteResponseAndPasswordRedaction() {
    edge_gateway::SerialPortOptions options;
    options.device = "test";
    options.timeoutMs = 50;
    options.wakeupBytes = 4;
    auto serial = std::make_shared<ImmediateDlt645SerialPort>();
    edge_gateway::Dlt645Client client(serial, options);

#ifdef _WIN32
    _putenv_s("GATEWAY_DLT645_DEBUG", "1");
#else
    setenv("GATEWAY_DLT645_DEBUG", "1", 1);
#endif
    std::ostringstream debug;
    auto* previous = std::cerr.rdbuf(debug.rdbuf());
    client.writeData("20", "06010101", "02000000", "01020304", {0x00, 0x02});
    std::cerr.rdbuf(previous);
#ifdef _WIN32
    _putenv_s("GATEWAY_DLT645_DEBUG", "0");
#else
    setenv("GATEWAY_DLT645_DEBUG", "0", 1);
#endif
    require(debug.str().find("hex=<redacted>") != std::string::npos, "DLT645 write debug frame must be redacted");
    require(debug.str().find("02000000") == std::string::npos, "DLT645 password must not appear in debug output");
    require(debug.str().find("35333333") == std::string::npos, "encoded DLT645 password must not appear in debug output");

    serial->rejectWrites(true);
    bool rejected = false;
    try {
        client.writeData("20", "06010101", "02000000", "01020304", {0x00, 0x02});
    } catch (const std::runtime_error& ex) {
        rejected = std::string(ex.what()).find("ERR=0x04") != std::string::npos;
    }
    require(rejected, "DLT645 D4H exception response should expose the ERR code");
}

}  // namespace

int main() {
    try {
        verifyFrameIntervalIsScopedToMeterAddress();
        verifyConfiguredWakeupPreambleIsSent();
        verifyBinaryAndBitfieldDecoding();
        verifyWriteFrameAndPayloadEncoding();
        verifyWriteResponseAndPasswordRedaction();
        verifyCollectorCachesSuccessfulReadPerDataId();
        verifyCollectorCachesFailedReadPerDataId();
        std::cout << "dlt645_client_interval_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "dlt645_client_interval_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
