#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/iec_codec.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using edge_gateway::IecCodec;
using edge_gateway::IecDataValue;
using edge_gateway::IecProtocolConfig;
using edge_gateway::PointDefinition;

void requireTrue(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireNear(double actual, double expected, const std::string& message) {
    if (std::fabs(actual - expected) > 1e-6) {
        throw std::runtime_error(message + " actual=" + std::to_string(actual));
    }
}

template <typename F>
void requireThrows(F function, const std::string& message) {
    bool threw = false;
    try {
        function();
    } catch (const std::exception&) {
        threw = true;
    }
    requireTrue(threw, message);
}

std::int64_t systemNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// CP56Time2a bytes of a local time and the Unix ms it denotes.
std::vector<std::uint8_t> cp56(int year, int month, int day, int hour, int minute, int ms, std::int64_t* unixMs) {
    std::tm tmValue{};
    tmValue.tm_year = year - 1900;
    tmValue.tm_mon = month - 1;
    tmValue.tm_mday = day;
    tmValue.tm_hour = hour;
    tmValue.tm_min = minute;
    tmValue.tm_sec = ms / 1000;
    tmValue.tm_isdst = -1;
    *unixMs = static_cast<std::int64_t>(std::mktime(&tmValue)) * 1000 + ms % 1000;
    return {
        static_cast<std::uint8_t>(ms & 0xFF), static_cast<std::uint8_t>((ms >> 8) & 0xFF),
        static_cast<std::uint8_t>(minute), static_cast<std::uint8_t>(hour),
        static_cast<std::uint8_t>(day), static_cast<std::uint8_t>(month),
        static_cast<std::uint8_t>(year % 100)
    };
}

PointDefinition readPoint(int typeId, int ioa, const std::string& dataType) {
    PointDefinition point;
    point.read.enable = true;
    point.read.dataType = dataType;
    point.read.iec.ioa = ioa;
    point.read.iec.typeId = typeId;
    return point;
}

void verifyStepPositionAsdus(const IecProtocolConfig& config) {
    const std::vector<std::uint8_t> untimed = {
        5, 0x82, 3, 0, 1, 0, 1, 0, 0,
        0xC0, 0x80, 0x3F, 0x00
    };
    const auto steps = IecCodec::decodeAsdu(untimed, config);
    requireTrue(steps.values.size() == 2, "IEC104 step payload must be two bytes per object");
    requireNear(steps.values[0].value, -64.0, "IEC104 signed seven-bit negative step");
    requireTrue(steps.values[0].transient && steps.values[0].quality == 0,
                "IEC104 VTI transient and QDS IV must decode independently");
    requireNear(steps.values[1].value, 63.0, "IEC104 positive step");
    requireTrue(!steps.values[1].transient && steps.values[1].quality == 1,
                "IEC104 good step quality must remain good");

    const std::vector<std::uint8_t> cp24 = {
        6, 1, 3, 0, 1, 0, 2, 0, 0,
        0x7F, 0x40, 0, 0, 0
    };
    const auto timedShort = IecCodec::decodeAsdu(cp24, config);
    requireTrue(timedShort.values.size() == 1, "IEC104 CP24 step payload length");
    requireNear(timedShort.values[0].value, -1.0, "IEC104 CP24 negative step");
    requireTrue(timedShort.values[0].quality == 0 && !timedShort.values[0].transient,
                "IEC104 CP24 QDS NT must propagate");

    const std::vector<std::uint8_t> cp56Step = {
        32, 1, 3, 0, 1, 0, 3, 0, 0,
        0xBF, 0x00, 0, 0, 0, 0, 1, 1, 26
    };
    const auto timedLong = IecCodec::decodeAsdu(cp56Step, config);
    requireTrue(timedLong.values.size() == 1, "IEC104 CP56 step payload length");
    requireNear(timedLong.values[0].value, 63.0, "IEC104 CP56 positive step");
    requireTrue(timedLong.values[0].transient && timedLong.values[0].quality == 1,
                "IEC104 CP56 transient must not replace good QDS");

    for (auto frame : {untimed, cp24, cp56Step}) {
        frame.pop_back();
        bool rejected = false;
        try { (void)IecCodec::decodeAsdu(frame, config); }
        catch (const std::runtime_error&) { rejected = true; }
        requireTrue(rejected, "truncated IEC104 step object must be rejected");
    }
}

// Finding 2: a point configured with the untimed type must also take the CP56 variant the
// station uses for spontaneous changes (and type 21 for normalized values).
void verifyTypeFamilies(const IecProtocolConfig& config) {
    const auto signal = readPoint(1, 1001, "single_point");
    std::int64_t expectedMs = 0;
    auto timed = std::vector<std::uint8_t>{30, 1, 3, 0, 1, 0, 0xE9, 0x03, 0x00, 0x01};
    const auto tag = cp56(2026, 10, 28, 10, 5, 10250, &expectedMs);
    timed.insert(timed.end(), tag.begin(), tag.end());
    const auto spontaneous = IecCodec::decodeAsdu(timed, config);
    requireTrue(spontaneous.values.size() == 1, "type 30 decodes one value");
    requireTrue(IecCodec::pointMatches(signal, spontaneous.values[0]), "typeId=1 point must match a type 30 change");
    // Finding 10: CP56Time2a becomes the point time.
    requireTrue(spontaneous.values[0].timestampMs == expectedMs, "type 30 CP56Time2a must be decoded");

    auto invalidTime = timed;
    invalidTime[6 + 3 + 1 + 2] |= 0x80;  // minute octet IV
    requireTrue(IecCodec::decodeAsdu(invalidTime, config).values[0].timestampMs == 0,
        "a CP56Time2a flagged IV must not be used as point time");

    const auto analog = readPoint(13, 2001, "measured_float");
    const std::vector<std::uint8_t> type36 = {
        36, 1, 3, 0, 1, 0, 0xD1, 0x07, 0x00, 0x00, 0x00, 0x48, 0x41, 0x00,
        0, 0, 5, 10, 28, 10, 26};
    requireTrue(IecCodec::pointMatches(analog, IecCodec::decodeAsdu(type36, config).values.at(0)),
        "typeId=13 point must match type 36");
    const auto normalized = readPoint(9, 3001, "float32");
    const std::vector<std::uint8_t> type21 = {21, 1, 3, 0, 1, 0, 0xB9, 0x0B, 0x00, 0x00, 0x40};
    const auto type21Value = IecCodec::decodeAsdu(type21, config).values.at(0);
    requireTrue(IecCodec::pointMatches(normalized, type21Value), "typeId=9 point must match type 21");
    requireNear(type21Value.value, 0.5, "type 21 normalized value");
    const auto doublePoint = readPoint(3, 1001, "double_point");
    requireTrue(!IecCodec::pointMatches(doublePoint, spontaneous.values[0]), "a double point must not match a single point");

    // CP24Time2a (type 2): minute/ms of the current hour, resolved against now.
    const auto now = systemNowMs();
    const auto twoMinutesAgo = now - 120000;
    const auto seconds = static_cast<std::time_t>(twoMinutesAgo / 1000);
    std::tm local{};
    localtime_r(&seconds, &local);
    const int msOfMinute = local.tm_sec * 1000 + static_cast<int>(twoMinutesAgo % 1000);
    const std::vector<std::uint8_t> type2 = {2, 1, 3, 0, 1, 0, 0xE9, 0x03, 0x00, 0x01,
        static_cast<std::uint8_t>(msOfMinute & 0xFF), static_cast<std::uint8_t>(msOfMinute >> 8),
        static_cast<std::uint8_t>(local.tm_min)};
    const auto type2Value = IecCodec::decodeAsdu(type2, config, now).values.at(0);
    requireTrue(type2Value.timestampMs == twoMinutesAgo, "CP24Time2a must resolve to the instant two minutes ago");
}

// Finding 10: type 20 objects are 5 octets (SCD + QDS); one value per status bit.
void verifyPackedSinglePoints(const IecProtocolConfig& config) {
    const std::vector<std::uint8_t> asdu = {20, 0x02, 0x14, 0x00, 0x01, 0x00,
        100, 0, 0, 0xFF, 0xFF, 0x03, 0x00, 0x00,
        200, 0, 0, 0x01, 0x00, 0x01, 0x00, 0x00};
    const auto decoded = IecCodec::decodeAsdu(asdu, config);
    requireTrue(decoded.values.size() == 32, "two type 20 objects expand to 32 status bits");
    requireTrue(decoded.values[16].ioa == 200 && decoded.values[16].elementIndex == 0, "second type 20 object IOA");
    requireNear(decoded.values[16].value, 1.0, "IOA 200 bit 0");
    requireNear(decoded.values[17].value, 0.0, "IOA 200 bit 1");
    auto bit15 = readPoint(20, 100, "bool");
    bit15.read.iec.elementIndex = 15;
    requireTrue(IecCodec::pointMatches(bit15, decoded.values[15]) && !IecCodec::pointMatches(bit15, decoded.values[14]),
        "elementIndex selects the type 20 status bit");
}

// Finding 10: OV/BL/SB/NT/IV, DPI 0/3, counter IV/CY all reach the value quality.
void verifyQualityMapping(const IecProtocolConfig& config) {
    const std::vector<std::uint8_t> flagged = {13, 1, 3, 0, 1, 0, 1, 0, 0, 0x00, 0x00, 0x48, 0x41, 0x31};
    const auto value = IecCodec::decodeAsdu(flagged, config).values.at(0);
    requireTrue(value.quality == 0, "QDS OV|BL|SB must mark the value bad");
    requireTrue(value.qualityMsg.find("OV") != std::string::npos && value.qualityMsg.find("BL") != std::string::npos &&
        value.qualityMsg.find("SB") != std::string::npos, "quality message names the flags: " + value.qualityMsg);

    const std::vector<std::uint8_t> doubles = {3, 0x03, 0x03, 0x00, 0x01, 0x00, 1, 0, 0, 0x03, 2, 0, 0, 0x00, 3, 0, 0, 0x02};
    const auto points = IecCodec::decodeAsdu(doubles, config).values;
    requireTrue(points.size() == 3 && points[0].doublePoint, "double points decode");
    requireTrue(points[0].quality == 0 && points[1].quality == 0 && points[2].quality == 1,
        "DPI 3 and 0 are bad, DPI 2 is good");

    const std::vector<std::uint8_t> counters = {15, 0x02, 0x25, 0x00, 0x01, 0x00,
        1, 0, 0, 0x10, 0x00, 0x00, 0x00, 0x80,
        2, 0, 0, 0x20, 0x00, 0x00, 0x00, 0x20};
    const auto totals = IecCodec::decodeAsdu(counters, config).values;
    requireTrue(totals.size() == 2 && totals[0].quality == 0 && totals[1].quality == 0,
        "counter IV and CY mark the reading bad");

    // Finding 11: a bitstring is unsigned.
    const std::vector<std::uint8_t> bitstring = {7, 0x01, 0x03, 0x00, 0x01, 0x00, 1, 0, 0, 0x00, 0x00, 0x00, 0x80, 0x00};
    requireNear(IecCodec::decodeAsdu(bitstring, config).values.at(0).value, 2147483648.0, "BSI bit 31 is unsigned");

    // T bit marks test transmissions.
    auto test = flagged;
    test[2] = 0x83;
    requireTrue(IecCodec::decodeAsdu(test, config).values.at(0).test, "COT T bit marks a test value");
}

// Finding 11: protection events follow SEP/SPE/OCI + QDP + CP16Time2a + time tag.
void verifyProtectionEvents(const IecProtocolConfig& config) {
    const std::vector<std::uint8_t> type38 = {
        38, 1, 3, 0, 1, 0, 0x71, 0x17, 0x00,
        0x02, 0xE8, 0x03,
        0, 0, 0, 0, 1, 1, 0x1A
    };
    const auto event38 = IecCodec::decodeAsdu(type38, config).protectionEvents.at(0);
    requireTrue(event38.ioa == 6001 && event38.eventState == 2 && event38.elapsedTimeMs == 1000 && event38.quality == 1,
        "type 38 SEP / elapsed time");

    std::int64_t expectedMs = 0;
    auto type39 = std::vector<std::uint8_t>{39, 1, 3, 0, 1, 0, 0x72, 0x17, 0x00, 0x03, 0x80, 0xF4, 0x01};
    const auto tag = cp56(2026, 3, 4, 5, 6, 7000, &expectedMs);
    type39.insert(type39.end(), tag.begin(), tag.end());
    const auto event39 = IecCodec::decodeAsdu(type39, config).protectionEvents.at(0);
    requireTrue(event39.eventState == 3 && event39.relayDurationMs == 500, "type 39 SPE and relay duration at QDP+1");
    requireTrue(event39.quality == 0 && event39.timestampMs == expectedMs, "type 39 QDP IV and CP56Time2a");

    const std::vector<std::uint8_t> type19 = {19, 1, 3, 0, 1, 0, 0x73, 0x17, 0x00, 0x05, 0x00, 0x2C, 0x01, 0, 0, 0};
    const auto event19 = IecCodec::decodeAsdu(type19, config).protectionEvents.at(0);
    requireTrue(event19.eventState == 5 && event19.relayDurationMs == 300 && event19.quality == 1,
        "type 19 OCI and relay operating time");
    const auto asValue = IecCodec::protectionEventValue(event38);
    requireTrue(asValue.doublePoint && asValue.value == 2.0, "ES of type 38 behaves like a double point");
}

void verifyCommandBuilders(const IecProtocolConfig& config) {
    PointDefinition command;
    command.pointCode = "breaker";
    command.write.enable = true;
    command.write.iec.ioa = 3001;
    command.write.iec.typeId = 46;
    // Finding 11: DCS 0 and 3 are not permitted.
    requireThrows([&] { IecCodec::buildIec104ControlCommand(config, command, 0.0, 0, 0, false); }, "DCS 0 rejected");
    requireThrows([&] { IecCodec::buildIec104ControlCommand(config, command, 3.0, 0, 0, false); }, "DCS 3 rejected");
    const auto on = IecCodec::iec104AsduPayload(IecCodec::buildIec104ControlCommand(config, command, 2.0, 0, 0, true));
    requireTrue(on.back() == 0x82, "DCS 2 with S/E");
    // Deselect: S/E=1 with COT 8.
    const auto deselect = IecCodec::iec104AsduPayload(IecCodec::buildIec104ControlCommand(config, command, 2.0, 0, 0, true, true));
    requireTrue(deselect[2] == 8 && (deselect.back() & 0x80) != 0, "deselect uses COT 8 and S/E=1");

    command.write.iec.typeId = 47;
    const auto step = IecCodec::iec104AsduPayload(IecCodec::buildIec104ControlCommand(config, command, 1.0, 0, 0, false));
    requireTrue(step[0] == 47 && step.back() == 0x01, "regulating step command RCS 1");

    // QOS keeps QL bits 0..6.
    command.write.iec.typeId = 50;
    command.write.iec.qualifier = 3;
    const auto setpoint = IecCodec::iec104AsduPayload(IecCodec::buildIec104ControlCommand(config, command, 1.5, 0, 0, false));
    requireTrue(setpoint.back() == 0x03, "setpoint QOS QL=3");

    command.write.iec.typeId = 58;
    command.write.iec.qualifier = 0;
    const auto timedCommand = IecCodec::iec104AsduPayload(IecCodec::buildIec104ControlCommand(
        config, command, 1.0, 0, 0, false, false, systemNowMs()));
    requireTrue(timedCommand[0] == 58 && timedCommand.size() == 6 + 3 + 1 + 7, "C_SC_TA_1 carries CP56Time2a");
    requireTrue(IecCodec::decodeAsdu(timedCommand, config).values.at(0).value == 1.0, "C_SC_TA_1 echo decodes");

    command.write.iec.typeId = 51;
    requireThrows([&] { IecCodec::buildIec104ControlCommand(config, command, 5.0, 0, 0, true); },
        "bitstring command has no select");
    const auto bits = IecCodec::iec104AsduPayload(IecCodec::buildIec104ControlCommand(config, command, 4294967295.0, 0, 0, false));
    requireTrue(bits.size() == 6 + 3 + 4 && bits.back() == 0xFF, "C_BO_NA_1 32-bit string");

    command.write.iec.typeId = 0;
    command.write.dataType = "single_command";
    requireTrue(IecCodec::commandTypeOf(command) == 45, "command type derived from dataType");
    command.read.iec.typeId = 1;
    requireTrue(IecCodec::commandTypeOf(command) == 45, "read typeId never decides the command type");

    // GI / CI ASDUs: COT 6 activation, QOI / QCC.
    requireTrue(IecCodec::buildIec101InterrogationAsdu(config) ==
        std::vector<std::uint8_t>({100, 1, 6, 0, 1, 0, 0, 0, 0, 20}), "C_IC_NA_1 COT 6 QOI 20");
    requireTrue(IecCodec::buildIec101CounterInterrogationAsdu(config) ==
        std::vector<std::uint8_t>({101, 1, 6, 0, 1, 0, 0, 0, 0, 5}), "C_CI_NA_1 COT 6 QCC 5");
    const auto ci = IecCodec::decodeAsdu({101, 1, 0x47, 0, 1, 0, 0, 0, 0, 5}, config);
    requireTrue(ci.typeId == 101 && ci.cause == 7 && ci.negative && ci.commandQualifier == 5, "mirrored CI decodes");

    // Finding 11: APCI length must be 4..253.
    std::vector<std::uint8_t> big(257, 0);
    big[0] = 0x68;
    big[1] = 0xFF;
    requireTrue(!IecCodec::isIec104Frame(big), "APDU length 255 rejected");
    std::vector<std::uint8_t> max(255, 0);
    max[0] = 0x68;
    max[1] = 253;
    requireTrue(IecCodec::isIec104IFrame(max), "APDU length 253 accepted");
}

// Finding 1/3: IEC 103 ASDU 1/2 DPI, ASDU 3/9 MEA, test mode.
void verifyIec103(IecProtocolConfig config) {
    config.linkAddressSize = 1;
    const std::vector<std::uint8_t> asdu9 = {9, 0x82, 2, 1, 160, 148, 0x00, 0x20, 0x02, 0xC0};
    const auto measurands = IecCodec::decodeIec103Data(asdu9, config);
    requireTrue(measurands.size() == 2, "ASDU9 VSQ gives the measurand count");
    requireTrue(measurands[0].functionType == 160 && measurands[0].informationNumber == 148, "ASDU9 FUN/INF");
    requireNear(measurands[0].value, 0.25, "MEA 13-bit normalized value");
    requireTrue(measurands[1].elementIndex == 1 && measurands[1].quality == 0, "MEA ER marks the value bad");
    requireNear(measurands[1].value, -0.5, "negative MEA");

    const std::vector<std::uint8_t> asdu3 = {3, 0x81, 2, 1, 160, 145, 0x08, 0x00, 0x01, 0x10};
    const auto asdu3Values = IecCodec::decodeIec103Data(asdu3, config);
    requireTrue(asdu3Values.size() == 2 && asdu3Values[1].quality == 0, "ASDU3 count from length, OV bad");
    requireNear(asdu3Values[0].value, 1.0 / 4096.0, "ASDU3 smallest MEA step");

    PointDefinition il2;
    il2.read.enable = true;
    il2.read.dataType = "float32";
    il2.read.iec.functionType = 160;
    il2.read.iec.informationNumber = 148;
    il2.read.iec.typeId = 9;
    il2.read.iec.elementIndex = 1;
    requireTrue(IecCodec::pointMatches(il2, measurands[1]) && !IecCodec::pointMatches(il2, measurands[0]),
        "elementIndex selects the ASDU9 measurand");

    // ASDU 1: FUN INF DPI CP32Time2a SIN (AM5SE spontaneous change).
    const auto now = systemNowMs();
    const auto seconds = static_cast<std::time_t>(now / 1000);
    std::tm local{};
    localtime_r(&seconds, &local);
    const int msOfMinute = local.tm_sec * 1000 + static_cast<int>(now % 1000);
    std::vector<std::uint8_t> asdu1 = {1, 0x81, 1, 1, 1, 100, 0x02,
        static_cast<std::uint8_t>(msOfMinute & 0xFF), static_cast<std::uint8_t>(msOfMinute >> 8),
        static_cast<std::uint8_t>(local.tm_min), static_cast<std::uint8_t>(local.tm_hour), 0x00};
    const auto on = IecCodec::decodeIec103Data(asdu1, config, now).at(0);
    requireTrue(on.doublePoint && on.value == 2.0 && on.quality == 1, "ASDU1 DPI 2 decodes as a double point");
    requireTrue(on.timestampMs == now, "ASDU1 CP32Time2a resolves to today");

    PointDefinition breaker;
    breaker.read.enable = true;
    breaker.read.dataType = "bool";
    breaker.read.iec.functionType = 1;
    breaker.read.iec.informationNumber = 100;
    breaker.read.iec.typeId = 44;
    requireTrue(IecCodec::pointMatches(breaker, on), "ASDU44 point accepts ASDU1 changes");
    requireNear(IecCodec::decodePointValue(breaker, on).value, 1.0, "adopted mapping (firmware unverified): DPI 2 -> 1");
    asdu1[6] = 0x01;
    const auto off = IecCodec::decodeIec103Data(asdu1, config, now).at(0);
    requireNear(IecCodec::decodePointValue(breaker, off).value, 0.0, "adopted mapping (firmware unverified): DPI 1 -> 0");
    requireTrue(!IecCodec::holdsPreviousValue(breaker, off), "DPI 1 is a regular value");
    asdu1[6] = 0x03;
    const auto indeterminate = IecCodec::decodeIec103Data(asdu1, config, now).at(0);
    requireTrue(indeterminate.quality == 0 && IecCodec::holdsPreviousValue(breaker, indeterminate),
        "DPI 3 keeps the previous value with bad quality");
    asdu1[6] = 0x00;
    requireTrue(IecCodec::holdsPreviousValue(breaker, IecCodec::decodeIec103Data(asdu1, config, now).at(0)),
        "DPI 0 keeps the previous value with bad quality");
    auto raw = breaker;
    raw.read.dataType = "double_point";
    requireTrue(!IecCodec::holdsPreviousValue(raw, indeterminate) &&
        IecCodec::decodePointValue(raw, indeterminate).value == 3.0, "double_point points keep the raw DPI");

    auto testMode = asdu9;
    testMode[2] = 7;
    requireTrue(IecCodec::decodeIec103Data(testMode, config).at(0).test, "IEC 103 COT 7 (test mode) is a test value");

    requireTrue(IecCodec::buildIec103GeneralInterrogationAsdu(config) ==
        std::vector<std::uint8_t>({7, 0x81, 9, 1, 0xFF, 0x00, 0xE9}), "IEC 103 GI is ASDU 7 with COT 9");
}

// Round 2: protocol.iec.timestampZone. Fixed POSIX zone strings (no tzdata needed).
class ScopedTimeZone {
public:
    explicit ScopedTimeZone(const char* zone) {
        const char* previous = std::getenv("TZ");
        hadPrevious_ = previous != nullptr;
        if (hadPrevious_) previous_ = previous;
        setenv("TZ", zone, 1);
        tzset();
    }
    ~ScopedTimeZone() {
        if (hadPrevious_) setenv("TZ", previous_.c_str(), 1); else unsetenv("TZ");
        tzset();
    }

private:
    bool hadPrevious_ = false;
    std::string previous_;
};

std::int64_t utcMs(int year, int month, int day, int hour, int minute, int second, int ms) {
    std::tm tmValue{};
    tmValue.tm_year = year - 1900;
    tmValue.tm_mon = month - 1;
    tmValue.tm_mday = day;
    tmValue.tm_hour = hour;
    tmValue.tm_min = minute;
    tmValue.tm_sec = second;
    return static_cast<std::int64_t>(timegm(&tmValue)) * 1000 + ms;
}

std::vector<std::uint8_t> type30With(std::uint8_t hourOctet, int day, int month, int minute, int msOfMinute) {
    return {30, 1, 3, 0, 1, 0, 0xE9, 0x03, 0x00, 0x01,
        static_cast<std::uint8_t>(msOfMinute & 0xFF), static_cast<std::uint8_t>(msOfMinute >> 8),
        static_cast<std::uint8_t>(minute), hourOctet, static_cast<std::uint8_t>(day), static_cast<std::uint8_t>(month), 26};
}

void verifyTimestampZones(IecProtocolConfig local) {
    auto utc = local;
    utc.timestampZone = "utc";
    {
        ScopedTimeZone zone("CST-8");  // UTC+8, no DST (gateway zone of the AM5 site)
        // The same CP56Time2a (2026-10-28 10:05:10.250) yields different point times.
        const auto frame = type30With(10, 28, 10, 5, 10250);
        requireTrue(IecCodec::decodeAsdu(frame, local).values.at(0).timestampMs == utcMs(2026, 10, 28, 2, 5, 10, 250),
            "timestampZone=local reads CP56Time2a as gateway local time");
        requireTrue(IecCodec::decodeAsdu(frame, utc).values.at(0).timestampMs == utcMs(2026, 10, 28, 10, 5, 10, 250),
            "timestampZone=utc reads CP56Time2a as UTC");
        // SU set by a station although the zone has no DST: the zone's own rule applies.
        const auto summerFlag = type30With(0x80 | 10, 28, 10, 5, 10250);
        requireTrue(IecCodec::decodeAsdu(summerFlag, local).values.at(0).timestampMs == utcMs(2026, 10, 28, 2, 5, 10, 250),
            "SU in a zone without DST is ignored");
        requireTrue(IecCodec::decodeAsdu(summerFlag, utc).values.at(0).timestampMs == utcMs(2026, 10, 28, 10, 5, 10, 250),
            "SU is ignored for UTC");

        // IEC 103 CP32Time2a (10:00:00.000), now = 2026-10-28 02:30Z (10:30 local).
        const auto now = utcMs(2026, 10, 28, 2, 30, 0, 0);
        const std::vector<std::uint8_t> asdu1 = {1, 0x81, 1, 1, 1, 100, 0x02, 0x00, 0x00, 0, 10, 0x00};
        requireTrue(IecCodec::decodeIec103Data(asdu1, local, now).at(0).timestampMs == utcMs(2026, 10, 28, 2, 0, 0, 0),
            "IEC103 CP32Time2a in local time");
        requireTrue(IecCodec::decodeIec103Data(asdu1, utc, now).at(0).timestampMs == utcMs(2026, 10, 28, 10, 0, 0, 0),
            "IEC103 CP32Time2a in UTC");

        // C_CS_NA_1 carries the configured zone.
        const auto localSync = IecCodec::iec104AsduPayload(IecCodec::buildIec104ClockSyncCommand(local, 0, 0, now));
        const auto utcSync = IecCodec::iec104AsduPayload(IecCodec::buildIec104ClockSyncCommand(utc, 0, 0, now));
        requireTrue(localSync[12] == 10 && utcSync[12] == 2, "clock sync hour follows timestampZone");
    }
    {
        ScopedTimeZone zone("IST-5:30");  // half-hour offset: CP24 hour boundaries differ
        const auto now = utcMs(2026, 10, 28, 2, 30, 0, 0);  // 08:00 IST
        const std::vector<std::uint8_t> type2 = {2, 1, 3, 0, 1, 0, 0xE9, 0x03, 0x00, 0x01, 0x00, 0x00, 20};
        requireTrue(IecCodec::decodeAsdu(type2, local, now).values.at(0).timestampMs == utcMs(2026, 10, 28, 1, 50, 0, 0),
            "CP24Time2a minute 20 of the previous local hour (07:20 IST)");
        requireTrue(IecCodec::decodeAsdu(type2, utc, now).values.at(0).timestampMs == utcMs(2026, 10, 28, 2, 20, 0, 0),
            "CP24Time2a minute 20 of the current UTC hour");
    }
    {
        // Central European time: DST ends 2026-10-25 03:00 CEST -> 02:00 CET, so 02:30 occurs twice.
        ScopedTimeZone zone("CET-1CEST,M3.5.0,M10.5.0/3");
        const auto summer = type30With(0x80 | 2, 25, 10, 30, 0);
        const auto winter = type30With(2, 25, 10, 30, 0);
        requireTrue(IecCodec::decodeAsdu(summer, local).values.at(0).timestampMs == utcMs(2026, 10, 25, 0, 30, 0, 0),
            "SU=1 selects the CEST occurrence of the repeated hour");
        requireTrue(IecCodec::decodeAsdu(winter, local).values.at(0).timestampMs == utcMs(2026, 10, 25, 1, 30, 0, 0),
            "SU=0 selects the CET occurrence of the repeated hour");
        const auto july = utcMs(2026, 7, 1, 10, 0, 0, 0);
        const auto localSync = IecCodec::iec104AsduPayload(IecCodec::buildIec104ClockSyncCommand(local, 0, 0, july));
        const auto utcSync = IecCodec::iec104AsduPayload(IecCodec::buildIec104ClockSyncCommand(utc, 0, 0, july));
        requireTrue(localSync[12] == (0x80 | 12) && utcSync[12] == 10, "SU is sent for local summer time only");
    }
}

}  // namespace

int main() {
    using namespace edge_gateway;

    IecProtocolConfig config;
    config.cotSize = 2;
    config.caSize = 2;
    config.ioaSize = 3;
    config.commonAddress = 1;
    verifyStepPositionAsdus(config);
    verifyTypeFamilies(config);
    verifyPackedSinglePoints(config);
    verifyQualityMapping(config);
    verifyProtectionEvents(config);
    verifyCommandBuilders(config);
    verifyIec103(config);
    verifyTimestampZones(config);

    const std::vector<std::uint8_t> asdu = {
        13, 1,
        3, 0,
        1, 0,
        0xD1, 0x07, 0x00,
        0x00, 0x00, 0x48, 0x41,
        0x00
    };
    const auto decoded = IecCodec::decodeAsdu(asdu, config);
    requireTrue(decoded.values.size() == 1, "IEC104 ASDU value count");
    requireTrue(decoded.values.front().ioa == 2001, "IEC104 IOA");
    requireNear(decoded.values.front().value, 12.5, "IEC104 float value");
    requireTrue(decoded.values.front().quality == 1, "IEC104 good QDS should keep good quality");
    requireTrue(decoded.values.front().timestampMs == 0, "untimed ASDU has no device time");

    auto invalidAsdu = asdu;
    invalidAsdu.back() = 0x80;
    const auto invalidDecoded = IecCodec::decodeAsdu(invalidAsdu, config);
    requireTrue(invalidDecoded.values.front().quality == 0, "IEC104 QDS IV must mark value invalid");

    const std::vector<std::uint8_t> negativeConfirm = {
        45, 1,
        0x47, 0,
        1, 0,
        0xB9, 0x0B, 0x00,
        0x01
    };
    const auto negativeDecoded = IecCodec::decodeAsdu(negativeConfirm, config);
    requireTrue(negativeDecoded.cause == 7 && negativeDecoded.negative, "IEC104 COT P/N bit must be decoded");
    requireTrue(!negativeDecoded.values.empty() && negativeDecoded.values.front().negative,
        "IEC104 negative flag must propagate to control echo values");

    PointDefinition point;
    point.address = 2001;
    point.read.enable = true;
    point.read.dataType = "measured_float";
    point.read.scale = 2.0;
    point.read.offset = 1.0;
    point.read.iec.typeId = 13;
    point.read.iec.commonAddress = 1;
    requireTrue(IecCodec::pointMatches(point, decoded.values.front()), "IEC point match by address fallback");
    const auto pointValue = IecCodec::decodePointValue(point, decoded.values.front());
    requireNear(pointValue.value, 26.0, "IEC point scale offset");

    const auto sFrame = IecCodec::buildIec104SFrame(5);
    requireTrue(sFrame == std::vector<std::uint8_t>({0x68, 0x04, 0x01, 0x00, 0x0A, 0x00}), "IEC104 S frame");
    requireTrue(IecCodec::isIec104SFrame(sFrame), "IEC104 S frame detection");
    requireTrue(IecCodec::iec104ReceiveSequence(sFrame) == 5, "IEC104 S frame receive sequence");

    PointDefinition commandPoint;
    commandPoint.index = 500001;
    commandPoint.pointCode = "remote_close";
    commandPoint.address = 3001;
    commandPoint.write.enable = true;
    commandPoint.write.dataType = "single_command";
    commandPoint.write.iec.typeId = 45;
    commandPoint.write.iec.ioa = 3001;
    commandPoint.write.iec.commonAddress = 1;
    commandPoint.write.iec.selectBeforeExecute = true;
    const auto selectFrame = IecCodec::buildIec104ControlCommand(config, commandPoint, 1.0, 2, 5, true);
    requireTrue(IecCodec::isIec104IFrame(selectFrame), "IEC104 control command I frame");
    const auto selectAsdu = IecCodec::iec104AsduPayload(selectFrame);
    requireTrue(selectAsdu.size() == 10, "IEC104 single command ASDU size");
    requireTrue(selectAsdu[0] == 45, "IEC104 single command type");
    requireTrue(selectAsdu.back() == 0x81, "IEC104 select single command SCO");

    const auto parameterFrame = IecCodec::buildIec104ParameterCommand(config, 5001, 112, 12.5, 0x01, 3, 1, 6);
    requireTrue(IecCodec::isIec104IFrame(parameterFrame), "IEC104 parameter I frame");
    const auto parameterDecoded = IecCodec::decodeAsdu(IecCodec::iec104AsduPayload(parameterFrame), config);
    requireTrue(parameterDecoded.parameters.size() == 1, "IEC104 parameter decode count");
    requireTrue(parameterDecoded.parameters.front().ioa == 5001, "IEC104 parameter IOA");
    requireNear(parameterDecoded.parameters.front().value, 12.5, "IEC104 parameter float");
    requireTrue(parameterDecoded.parameters.front().qualifier == 0x01, "IEC104 parameter qualifier");

    const auto activationFrame = IecCodec::buildIec104ParameterActivationCommand(config, 5001, 0x02, 4, 1);
    const auto activationDecoded = IecCodec::decodeAsdu(IecCodec::iec104AsduPayload(activationFrame), config);
    requireTrue(activationDecoded.parameters.size() == 1, "IEC104 parameter activation count");
    requireTrue(activationDecoded.parameters.front().typeId == 113, "IEC104 parameter activation type");

    const auto fileCall = IecCodec::buildIec104FileCallCommand(config, 0, 7, 1, 0x01, 5, 1);
    const auto fileCallDecoded = IecCodec::decodeAsdu(IecCodec::iec104AsduPayload(fileCall), config);
    requireTrue(fileCallDecoded.fileSegments.size() == 1, "IEC104 file call decode count");
    requireTrue(fileCallDecoded.fileSegments.front().nameOfFile == 7, "IEC104 file call NOF");

    const std::vector<std::uint8_t> fileSegmentAsdu = {
        125, 1,
        13, 0,
        1, 0,
        0, 0, 0,
        0x07, 0x00,
        0x01,
        0x80,
        0x03,
        'a', 'b', 'c'
    };
    const auto fileSegmentDecoded = IecCodec::decodeAsdu(fileSegmentAsdu, config);
    requireTrue(fileSegmentDecoded.fileSegments.size() == 1, "IEC104 file segment count");
    requireTrue(fileSegmentDecoded.fileSegments.front().data.size() == 3, "IEC104 file segment length");
    requireTrue(fileSegmentDecoded.fileSegments.front().data.front() == 'a', "IEC104 file segment data");

    const auto loaded = ConfigLoader::loadFromText(R"JSON({
      "schemaVersion": "1.0.0",
      "enabled": true,
      "protocol": {
        "type": "iec104",
        "tcp": {
          "host": "127.0.0.1",
          "port": 2404
        },
        "iec": {
          "transportMode": "tcp",
          "commonAddress": 1,
          "clockSyncIntervalSec": 300
        }
      },
      "points": [
        {
          "index": 1,
          "pointCode": "iec104_packed_bit3",
          "name": "IEC104 packed bit 3",
          "category": "telemetry",
          "enabled": true,
          "read": {
            "enable": true,
            "dataType": "bool",
            "iec": {
              "ioa": 100,
              "typeId": 20,
              "elementIndex": 3
            }
          }
        },
        {
          "index": 2,
          "pointCode": "iec104_remote_close",
          "name": "IEC104 remote close",
          "category": "control",
          "address": 3001,
          "enabled": true,
          "read": {
            "enable": false
          },
          "write": {
            "enable": true,
            "dataType": "single_command",
            "iec": {
              "ioa": 3001,
              "typeId": 45,
              "commonAddress": 1,
              "selectBeforeExecute": true,
              "waitActivationTermination": true,
              "timeoutMs": 1500
            }
          }
        }
      ],
      "meters": []
    })JSON");
    requireTrue(loaded.protocol.type == "iec104", "config protocol type");
    requireTrue(loaded.protocol.iec.backgroundReceive, "config IEC background receive default");
    requireTrue(loaded.protocol.iec.clockSyncIntervalSec == 300, "config IEC clock sync interval");
    requireTrue(loaded.protocol.iec.interrogationIntervalMs == 900000, "GI repeats every 15 min by default");
    requireTrue(loaded.protocol.iec.counterInterrogation &&
        loaded.protocol.iec.counterInterrogationIntervalMs == 900000, "counter interrogation default");
    requireTrue(loaded.points.front().read.iec.elementIndex == 3, "config IEC elementIndex");
    requireTrue(loaded.points.back().write.iec.selectBeforeExecute, "config IEC write SBO");
    requireTrue(loaded.points.back().write.iec.waitActivationTermination, "config IEC write activation termination");

    std::cout << "iec codec tests passed" << std::endl;
    return EXIT_SUCCESS;
}
