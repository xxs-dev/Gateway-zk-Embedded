#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "edge_gateway/serial_port.hpp"

namespace edge_gateway {

class Dlt645Client {
public:
    Dlt645Client(std::shared_ptr<ISerialPort> serialPort, SerialPortOptions options);

    std::vector<std::uint8_t> readData(const std::string& meterAddress, const std::string& dataIdHex);

private:
    void ensurePortOpen();

    std::shared_ptr<ISerialPort> serialPort_;
    SerialPortOptions options_;
};

}  // namespace edge_gateway
