#include <algorithm>
#include <chrono>
#include <cmath>
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
#include "dlt645_test_support.hpp"

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
            // Answer from the addressed meter unless a foreign answer is forced.
            const auto address = foreignResponseAddress_.empty()
                ? requestAddress(frameStart)
                : foreignResponseAddress_;
            chunks_.push_back(readResponse(address, readDataId_.empty() ? requestDataId(frameStart) : readDataId_,
                                           readPayload_));
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

    void answerFromAddress(const std::string& address) {
        foreignResponseAddress_ = address;
    }

    static std::string requestDataId(std::vector<std::uint8_t>::const_iterator frameStart) {
        static constexpr char kHex[] = "0123456789ABCDEF";
        std::string dataId;
        for (int i = 13; i >= 10; --i) {
            const auto byte = static_cast<std::uint8_t>(*(frameStart + i) - 0x33U);
            dataId.push_back(kHex[(byte >> 4) & 0x0F]);
            dataId.push_back(kHex[byte & 0x0F]);
        }
        return dataId;
    }

    static std::string requestAddress(std::vector<std::uint8_t>::const_iterator frameStart) {
        static constexpr char kHex[] = "0123456789ABCDEF";
        std::string address;
        for (int i = 6; i >= 1; --i) {
            const auto byte = *(frameStart + i);
            address.push_back(kHex[(byte >> 4) & 0x0F]);
            address.push_back(kHex[byte & 0x0F]);
        }
        return address;
    }

private:
    bool open_ = false;
    std::vector<std::vector<std::uint8_t>> writes_;
    std::vector<std::vector<std::uint8_t>> chunks_;
    std::vector<std::chrono::steady_clock::time_point> writeTimes_;
    bool rejectWrites_ = false;
    bool respondToReads_ = true;
    std::string readDataId_;
    std::vector<std::uint8_t> readPayload_ = {0x00, 0x00};
    std::string foreignResponseAddress_;
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

std::vector<std::uint8_t> requestDataIdBytes(const std::vector<std::uint8_t>& frame) {
    const auto start = std::find(frame.begin(), frame.end(), static_cast<std::uint8_t>(0x68));
    require(start != frame.end() && std::distance(start, frame.end()) >= 14, "invalid DLT645 read request");
    return std::vector<std::uint8_t>(start + 10, start + 14);
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
        auto config = dlt645CollectorConfig(storeName);
        config.collect.offlineFailureThreshold = 1;
        config.collect.failureBadQualityThreshold = 1;
        edge_gateway::Dlt645Collector collector(config, store, client);

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

void verifyCollectorSpreadsRetriesAcrossCyclesAndStopsAfterTimeout() {
    const std::string storeName = "dlt645_collector_round_budget_test_store";
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
    {
        edge_gateway::MemoryPointStore store(storeName);
        edge_gateway::SerialPortOptions options;
        options.device = "test";
        options.timeoutMs = 1;
        auto serial = std::make_shared<ImmediateDlt645SerialPort>();
        serial->respondToReads(false);
        auto client = std::make_shared<edge_gateway::Dlt645Client>(serial, options);
        auto config = dlt645CollectorConfig(storeName);
        auto secondDi = dlt645BitPoint(645003, "BREAKER_OTHER_DI", 0);
        secondDi.read.dlt645Di = "04001505";
        config.points.push_back(secondDi);
        config.protocol.transport.readRetryCount = 1;
        config.collect.maxTasksPerMeterPerCycle = 3;
        config.collect.slaveFailureBackoffThreshold = 100;
        config.collect.offlineFailureThreshold = 1;
        edge_gateway::Dlt645Collector collector(config, store, client);

        for (const auto nowMs : {1000LL, 1100LL, 1200LL}) {
            bool failed = false;
            try {
                (void)collector.collectOnce(nowMs);
            } catch (const std::runtime_error&) {
                failed = true;
            }
            require(failed, "offline DLT645 cycle should report its failed recovery probe");
            require(
                serial->writes().size() == static_cast<std::size_t>((nowMs - 1000) / 100 + 1),
                "offline DLT645 device must consume only one request in each collection cycle"
            );
        }

        require(
            requestDataIdBytes(serial->writes()[0]) == requestDataIdBytes(serial->writes()[1]),
            "configured DLT645 retry should retry the same DI in the next cycle"
        );
        require(
            requestDataIdBytes(serial->writes()[1]) != requestDataIdBytes(serial->writes()[2]),
            "DLT645 cursor should advance after cross-cycle retries are exhausted"
        );
    }
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
}

// frameIntervalMs is bus silence after the previous reply, whichever meter is addressed next.
void verifyFrameIntervalIsBusWide() {
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
    for (std::size_t i = 1; i < 3; ++i) {
        const auto gapMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            serial->writeTimes()[i] - serial->writeTimes()[i - 1]
        ).count();
        require(gapMs >= 195, "every request on the bus must wait frameIntervalMs after the previous reply");
    }
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

void verifySignedBcdDecoding() {
    edge_gateway::PointDefinition point;
    point.read.dlt645Di = "02030000";
    point.read.dlt645ByteCount = 3;
    point.read.scale = 0.0001;
    point.read.dataType = "dlt645_bcd_signed";
    // -0.1234 kW: bit7 of the MSB is the sign (DL/T 645-2007).
    const auto negative = edge_gateway::Dlt645Codec::decodeReadResponse(
        readResponse("20", point.read.dlt645Di, {0x34, 0x12, 0x80}), point);
    require(std::abs(negative.value + 0.1234) < 1e-9, "DLT645 signed BCD negative decode mismatch");
    const auto positive = edge_gateway::Dlt645Codec::decodeReadResponse(
        readResponse("20", point.read.dlt645Di, {0x34, 0x12, 0x05}), point);
    require(std::abs(positive.value - 5.1234) < 1e-9, "DLT645 signed BCD positive decode mismatch");

    // Existing unsigned dlt645_bcd behaviour is unchanged: MSB digit 8 is a digit.
    point.read.dataType = "dlt645_bcd";
    const auto unsignedValue = edge_gateway::Dlt645Codec::decodeReadResponse(
        readResponse("20", point.read.dlt645Di, {0x34, 0x12, 0x80}), point);
    require(std::abs(unsignedValue.value - 80.1234) < 1e-9, "DLT645 unsigned BCD decode must stay unchanged");
}

void verifyForeignMeterResponseIsRejected() {
    edge_gateway::SerialPortOptions options;
    options.device = "test";
    options.timeoutMs = 50;
    auto serial = std::make_shared<ImmediateDlt645SerialPort>();
    serial->answerFromAddress("000000000021");
    edge_gateway::Dlt645Client client(serial, options);
    bool rejected = false;
    try {
        (void)client.readData("20", "02010100");
    } catch (const std::runtime_error& ex) {
        rejected = std::string(ex.what()).find("address mismatch") != std::string::npos;
    }
    require(rejected, "DLT645 read response from another meter address must be rejected");
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

    const auto password = dlt645_test::fixturePassword();
    const auto frame = edge_gateway::Dlt645Codec::buildWriteFrame(
        "20",
        "06010101",
        password,
        "01020304",
        immediate
    );
    // DI0..DI3, PA P0 P1 P2 (in configured order), C0..C3, data; every data byte +33H.
    std::vector<std::uint8_t> expected = {
        0x68, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0x14, 0x0E,
        0x34, 0x34, 0x34, 0x39
    };
    for (std::size_t i = 0; i < 8; i += 2) {
        expected.push_back(static_cast<std::uint8_t>(std::stoi(password.substr(i, 2), nullptr, 16) + 0x33));
    }
    expected.insert(expected.end(), {0x34, 0x35, 0x36, 0x37, 0x33, 0x35});
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
    const auto password = dlt645_test::fixturePassword();
    std::string encodedPassword;
    for (std::size_t i = 0; i < 8; i += 2) {
        static constexpr char kHex[] = "0123456789ABCDEF";
        const auto byte = static_cast<std::uint8_t>(std::stoi(password.substr(i, 2), nullptr, 16) + 0x33);
        encodedPassword.push_back(kHex[byte >> 4]);
        encodedPassword.push_back(kHex[byte & 0x0F]);
    }
    std::ostringstream debug;
    auto* previous = std::cerr.rdbuf(debug.rdbuf());
    client.writeData("20", "06010101", password, "01020304", {0x00, 0x02});
    std::cerr.rdbuf(previous);
#ifdef _WIN32
    _putenv_s("GATEWAY_DLT645_DEBUG", "0");
#else
    setenv("GATEWAY_DLT645_DEBUG", "0", 1);
#endif
    require(debug.str().find("hex=<redacted>") != std::string::npos, "DLT645 write debug frame must be redacted");
    require(debug.str().find(password) == std::string::npos, "DLT645 password must not appear in debug output");
    require(debug.str().find(encodedPassword) == std::string::npos, "encoded DLT645 password must not appear in debug output");

    serial->rejectWrites(true);
    bool rejected = false;
    try {
        client.writeData("20", "06010101", password, "01020304", {0x00, 0x02});
    } catch (const std::runtime_error& ex) {
        rejected = std::string(ex.what()).find("ERR=0x04") != std::string::npos;
    }
    require(rejected, "DLT645 D4H exception response should expose the ERR code");
}

}  // namespace

int main() {
    try {
        verifyFrameIntervalIsBusWide();
        verifyConfiguredWakeupPreambleIsSent();
        verifyBinaryAndBitfieldDecoding();
        verifySignedBcdDecoding();
        verifyForeignMeterResponseIsRejected();
        verifyWriteFrameAndPayloadEncoding();
        verifyWriteResponseAndPasswordRedaction();
        verifyCollectorCachesSuccessfulReadPerDataId();
        verifyCollectorCachesFailedReadPerDataId();
        verifyCollectorSpreadsRetriesAcrossCyclesAndStopsAfterTimeout();
        std::cout << "dlt645_client_interval_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "dlt645_client_interval_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
