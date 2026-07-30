#pragma once

#include <cstdint>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "edge_gateway/serial_port.hpp"

namespace edge_gateway {

class Dlt645Client {
public:
    Dlt645Client(std::shared_ptr<ISerialPort> serialPort, SerialPortOptions options);

    std::vector<std::uint8_t> readData(const std::string& meterAddress, const std::string& dataIdHex);

    void writeData(
        const std::string& meterAddress,
        const std::string& dataIdHex,
        const std::string& passwordHex,
        const std::string& operatorCodeHex,
        const std::vector<std::uint8_t>& payload
    );

private:
    std::vector<std::uint8_t> exchange(
        const std::string& normalizedAddress,
        const std::string& dataIdHex,
        const std::vector<std::uint8_t>& frame,
        bool sensitiveRequest
    );
    void ensurePortOpen();
    void waitForFrameInterval(const std::string& meterAddress);

    std::shared_ptr<ISerialPort> serialPort_;
    SerialPortOptions options_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastRequestWriteAtByMeter_;
    std::mutex transactionMutex_;
};

}  // namespace edge_gateway
