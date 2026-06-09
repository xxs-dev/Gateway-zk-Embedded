#pragma once

#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "edge_gateway/iec_codec.hpp"
#include "edge_gateway/serial_port.hpp"

namespace edge_gateway {

class IecClient {
public:
    virtual ~IecClient() = default;

    virtual std::vector<IecDataValue> poll() = 0;
    virtual std::vector<IecDataValue> drainBufferedValues() {
        return {};
    }
    virtual CommandResult writeByPoint(
        const PointDefinition& point,
        double value,
        const std::string& cmdId,
        const std::string& machineCode,
        const std::string& meterCode,
        std::int64_t nowMs
    );
    virtual void synchronizeClock(std::int64_t nowMs);
};

class IecTcpClient : public IecClient {
public:
    IecTcpClient(std::string protocolType, TcpTransportConfig tcp, IecProtocolConfig iec);
    ~IecTcpClient() override;

    std::vector<IecDataValue> poll() override;
    std::vector<IecDataValue> drainBufferedValues() override;
    CommandResult writeByPoint(
        const PointDefinition& point,
        double value,
        const std::string& cmdId,
        const std::string& machineCode,
        const std::string& meterCode,
        std::int64_t nowMs
    ) override;
    void synchronizeClock(std::int64_t nowMs) override;

private:
    void ensureConnected();
    void ensureIec104Started();
    void startReceiveLoop();
    void stopReceiveLoop();
    void receiveLoop();
    void disconnect();
    void configureSocketTimeouts() const;
    std::uint16_t nextSendSequence();
    std::uint16_t currentReceiveSequence() const;
    void sendIec104IFrame(const std::vector<std::uint8_t>& bytes);
    void sendAll(const std::vector<std::uint8_t>& bytes);
    std::vector<std::uint8_t> readSome(int timeoutMs);
    void handleIec104Frame(const std::vector<std::uint8_t>& frame);
    void bufferValues(const std::vector<IecDataValue>& values);
    std::vector<IecDataValue> drainBufferedValuesLocked();
    bool waitForIec104Start(int timeoutMs);
    bool waitForControlConfirmation(const PointDefinition& point, double value, int expectedCause, int timeoutMs);
    std::vector<std::vector<std::uint8_t>> drainIec104Frames(int timeoutMs, int maxFrames);
    std::vector<std::vector<std::uint8_t>> drainFt12Frames(int timeoutMs, int maxFrames);

    std::string protocolType_;
    TcpTransportConfig tcp_;
    IecProtocolConfig iec_;
    std::intptr_t socket_ = -1;
    std::uint16_t sendSequence_ = 0;
    std::uint16_t receiveSequence_ = 0;
    std::uint16_t remoteReceiveSequence_ = 0;
    bool iec104Started_ = false;
    std::vector<std::uint8_t> rxBuffer_;
    std::atomic<bool> receiveRunning_{false};
    std::thread receiveThread_;
    mutable std::mutex stateMutex_;
    std::mutex sendMutex_;
    std::condition_variable stateChanged_;
    std::vector<IecDataValue> bufferedValues_;
    std::vector<IecDataValue> controlValues_;
    std::int64_t lastReceiveMs_ = 0;
    std::int64_t lastSendMs_ = 0;
    std::int64_t lastAckMs_ = 0;
    std::uint16_t unacknowledgedReceived_ = 0;
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
