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

struct IecAsdu {
    int typeId = 0;
    int cause = 0;
    int commonAddress = 0;
    std::vector<IecDataValue> values;
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
