#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "edge_gateway/dlt645_client.hpp"
#include "edge_gateway/dlt645_codec.hpp"
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

std::vector<std::uint8_t> readResponse(const std::string& meterAddress, const std::string& dataIdHex) {
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
    frame.push_back(0x06);
    for (const auto byte : dataId) {
        frame.push_back(static_cast<std::uint8_t>(byte + 0x33));
    }
    frame.push_back(0x33);
    frame.push_back(0x33);
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
        const auto address = writes_.size() == 2 ? "000000000002" : "000000000001";
        chunks_.push_back(readResponse(address, "00010000"));
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

private:
    bool open_ = false;
    std::vector<std::vector<std::uint8_t>> writes_;
    std::vector<std::vector<std::uint8_t>> chunks_;
    std::vector<std::chrono::steady_clock::time_point> writeTimes_;
};

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

}  // namespace

int main() {
    try {
        verifyFrameIntervalIsScopedToMeterAddress();
        std::cout << "dlt645_client_interval_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "dlt645_client_interval_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
