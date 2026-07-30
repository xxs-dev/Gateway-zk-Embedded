#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

struct IecDataValue {
    int commonAddress = 0;
    int ioa = 0;
    int typeId = 0;
    int cause = 0;
    int functionType = -1;
    int informationNumber = -1;
    double value = 0.0;
    std::string text;
    std::string rawHex;
};

struct IecParameterValue {
    int commonAddress = 0;
    int ioa = 0;
    int typeId = 0;
    int cause = 0;
    int qualifier = 0;
    double value = 0.0;
    std::string rawHex;
};

struct IecProtectionEvent {
    int commonAddress = 0;
    int ioa = 0;
    int typeId = 0;
    int cause = 0;
    int eventState = 0;
    int elapsedTimeMs = 0;
    int relayDurationMs = 0;
    std::string rawHex;
};

struct IecFileSegment {
    int commonAddress = 0;
    int ioa = 0;
    int cause = 0;
    int nameOfFile = 0;
    int nameOfSection = 0;
    int qualifier = 0;
    int lastSectionOrSegment = 0;
    std::vector<std::uint8_t> data;
    std::string rawHex;
};

struct Iec103DisturbanceRecord {
    int fan = 0;
    int state = 0;
    std::int64_t timestampMs = 0;
    std::string rawTimeHex;
};

struct Iec103ComtradeChunk {
    int fileType = 0;
    int packetNumber = 0;
    bool lastPacket = false;
    std::vector<std::uint8_t> data;
    std::string rawHex;
};

struct Iec103ComtradeFiles {
    int fan = 0;
    std::string cfgPath;
    std::string datPath;
    std::size_t cfgBytes = 0;
    std::size_t datBytes = 0;
};

class Iec103ComtradeAssembler {
public:
    explicit Iec103ComtradeAssembler(std::size_t maxBytes);

    void add(const Iec103ComtradeChunk& chunk);
    bool complete() const;
    int fileType() const;
    const std::vector<std::uint8_t>& bytes() const;

private:
    std::size_t maxBytes_;
    int fileType_ = 0;
    int nextPacketNumber_ = 0;
    bool complete_ = false;
    std::vector<std::vector<std::uint8_t>> packets_;
    std::vector<std::uint8_t> bytes_;
};

struct IecAsdu {
    int typeId = 0;
    int cause = 0;
    int commonAddress = 0;
    std::vector<IecDataValue> values;
    std::vector<IecParameterValue> parameters;
    std::vector<IecProtectionEvent> protectionEvents;
    std::vector<IecFileSegment> fileSegments;
};

class IecCodec {
public:
    static std::vector<std::uint8_t> buildIec104StartDtAct();
    static std::vector<std::uint8_t> buildIec104StopDtAct();
    static std::vector<std::uint8_t> buildIec104TestFrAct();
    static std::vector<std::uint8_t> buildIec104SFrame(std::uint16_t receiveSequence);
    static std::vector<std::uint8_t> buildIec104InterrogationCommand(
        const IecProtocolConfig& config,
        std::uint16_t sendSequence,
        std::uint16_t receiveSequence
    );
    static std::vector<std::uint8_t> buildIec104ClockSyncCommand(
        const IecProtocolConfig& config,
        std::uint16_t sendSequence,
        std::uint16_t receiveSequence,
        std::int64_t unixTimeMs
    );
    static std::vector<std::uint8_t> buildIec104ControlCommand(
        const IecProtocolConfig& config,
        const PointDefinition& point,
        double requestedValue,
        std::uint16_t sendSequence,
        std::uint16_t receiveSequence,
        bool select,
        bool cancel = false
    );
    static std::vector<std::uint8_t> buildIec104ParameterCommand(
        const IecProtocolConfig& config,
        int ioa,
        int typeId,
        double value,
        std::uint8_t qualifier,
        std::uint16_t sendSequence,
        std::uint16_t receiveSequence,
        int cause = 6
    );
    static std::vector<std::uint8_t> buildIec104ParameterActivationCommand(
        const IecProtocolConfig& config,
        int ioa,
        std::uint8_t qualifier,
        std::uint16_t sendSequence,
        std::uint16_t receiveSequence
    );
    static std::vector<std::uint8_t> buildIec104FileCallCommand(
        const IecProtocolConfig& config,
        int ioa,
        int nameOfFile,
        int nameOfSection,
        std::uint8_t qualifier,
        std::uint16_t sendSequence,
        std::uint16_t receiveSequence
    );
    static std::vector<std::uint8_t> buildIec104FileAckCommand(
        const IecProtocolConfig& config,
        int ioa,
        int nameOfFile,
        int nameOfSection,
        std::uint8_t qualifier,
        std::uint16_t sendSequence,
        std::uint16_t receiveSequence
    );
    static bool isIec104Frame(const std::vector<std::uint8_t>& frame);
    static bool isIec104IFrame(const std::vector<std::uint8_t>& frame);
    static bool isIec104SFrame(const std::vector<std::uint8_t>& frame);
    static bool isIec104UFrame(const std::vector<std::uint8_t>& frame);
    static bool isIec104StartDtCon(const std::vector<std::uint8_t>& frame);
    static bool isIec104StopDtCon(const std::vector<std::uint8_t>& frame);
    static bool isIec104TestFrAct(const std::vector<std::uint8_t>& frame);
    static bool isIec104TestFrCon(const std::vector<std::uint8_t>& frame);
    static std::uint16_t iec104SendSequence(const std::vector<std::uint8_t>& frame);
    static std::uint16_t iec104ReceiveSequence(const std::vector<std::uint8_t>& frame);
    static std::vector<std::uint8_t> iec104AsduPayload(const std::vector<std::uint8_t>& frame);

    static std::vector<std::uint8_t> buildIec101InterrogationFrame(const IecProtocolConfig& config);
    static std::vector<std::uint8_t> buildIec103GeneralInterrogationFrame(const IecProtocolConfig& config);
    static std::vector<std::uint8_t> buildFt12FixedFrame(
        std::uint8_t control,
        int linkAddress,
        int linkAddressSize
    );
    static bool isFt12FixedFrame(const std::vector<std::uint8_t>& frame, int linkAddressSize);
    static std::uint8_t ft12Control(const std::vector<std::uint8_t>& frame);
    static bool ft12AccessDemand(const std::vector<std::uint8_t>& frame);
    static std::vector<std::uint8_t> buildIec103ClassRequestFrame(
        const IecProtocolConfig& config,
        int dataClass,
        bool frameCountBit
    );
    static std::vector<std::uint8_t> buildIec103ResetCommunicationFrame(const IecProtocolConfig& config);
    static std::vector<std::uint8_t> buildIec103RecordingDirectoryFrame(
        const IecProtocolConfig& config,
        bool frameCountBit
    );
    static std::vector<Iec103DisturbanceRecord> decodeIec103RecordingDirectory(
        const std::vector<std::uint8_t>& userData,
        const IecProtocolConfig& config
    );
    static std::vector<std::uint8_t> buildIec103ComtradeFileCallFrame(
        const IecProtocolConfig& config,
        int fan,
        int fileType,
        bool frameCountBit
    );
    static Iec103ComtradeChunk decodeIec103ComtradeChunk(
        const std::vector<std::uint8_t>& userData,
        const IecProtocolConfig& config
    );
    static std::vector<std::uint8_t> buildAm5seUdpDiscoveryPacket(
        std::int64_t unixTimeMs,
        const std::string& stationName
    );
    static bool isFt12VariableFrame(const std::vector<std::uint8_t>& frame);
    static std::vector<std::uint8_t> ft12UserData(const std::vector<std::uint8_t>& frame, int linkAddressSize);

    static IecAsdu decodeAsdu(const std::vector<std::uint8_t>& asdu, const IecProtocolConfig& config);
    static std::vector<IecDataValue> decodeIec103Data(
        const std::vector<std::uint8_t>& userData,
        const IecProtocolConfig& config
    );

    static bool pointMatches(const PointDefinition& point, const IecDataValue& value);
    static DecodedValue decodePointValue(const PointDefinition& point, const IecDataValue& value);
    static std::string toHex(const std::vector<std::uint8_t>& bytes);
};

}  // namespace edge_gateway
