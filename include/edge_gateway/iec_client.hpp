#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "edge_gateway/iec_codec.hpp"
#include "edge_gateway/serial_port.hpp"

namespace edge_gateway {

class IecClient {
public:
    virtual ~IecClient() = default;

    virtual std::vector<IecDataValue> poll() = 0;
};

class IecTcpClient : public IecClient {
public:
    IecTcpClient(std::string protocolType, TcpTransportConfig tcp, IecProtocolConfig iec);
    ~IecTcpClient() override;

    std::vector<IecDataValue> poll() override;

private:
    void ensureConnected();
    void disconnect();
    void configureSocketTimeouts() const;
    void sendAll(const std::vector<std::uint8_t>& bytes);
    std::vector<std::uint8_t> readSome(int timeoutMs);
    std::vector<std::vector<std::uint8_t>> drainIec104Frames(int timeoutMs, int maxFrames);
    std::vector<std::vector<std::uint8_t>> drainFt12Frames(int timeoutMs, int maxFrames);

    std::string protocolType_;
    TcpTransportConfig tcp_;
    IecProtocolConfig iec_;
    std::intptr_t socket_ = -1;
    std::uint16_t sendSequence_ = 0;
    std::uint16_t receiveSequence_ = 0;
    bool iec104Started_ = false;
    std::vector<std::uint8_t> rxBuffer_;
};

class IecSerialClient : public IecClient {
public:
    IecSerialClient(std::string protocolType, std::shared_ptr<ISerialPort> serialPort, SerialPortOptions serial, IecProtocolConfig iec);

    std::vector<IecDataValue> poll() override;

private:
    void ensurePortOpen();
    void sendAll(const std::vector<std::uint8_t>& bytes);
    std::vector<std::vector<std::uint8_t>> drainFt12Frames(int timeoutMs, int maxFrames);

    std::string protocolType_;
    std::shared_ptr<ISerialPort> serialPort_;
    SerialPortOptions serial_;
    IecProtocolConfig iec_;
    std::vector<std::uint8_t> rxBuffer_;
};

}  // namespace edge_gateway
