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
    // Position inside a multi-element information object: measurand index of IEC 103
    // ASDU 3/9, status bit of IEC 101/104 type 20 (M_PS_NA_1). 0 for single elements.
    int elementIndex = 0;
    double value = 0.0;
    std::string text;
    std::string rawHex;
    // COT P/N bit (0x40): negative confirmation from the controlled station.
    bool negative = false;
    // COT T bit (0x80): test transmission. Never updates production points.
    bool test = false;
    // VTI T bit on step-position information objects (types 5/6/32).
    bool transient = false;
    // Double-point information (IEC 101/104 DIQ types 3/4/31, IEC 103 ASDU 1/2 DPI): value holds
    // the raw DPI 0..3. IEC 101/104: 1 = OFF, 2 = ON, 0/3 = intermediate/indeterminate. The same
    // mapping is applied to IEC 103 ASDU 1/2; for AM5SE it is the code's adopted mapping, not a
    // verified firmware property (to be confirmed by the vendor or against raw frames on site).
    bool doublePoint = false;
    // 1 = good; 0 = the device flagged the value unusable (IV/NT/OV/BL/SB, DPI 0/3,
    // counter IV/CY, IEC 103 MEA OV/ER). qualityMsg names the flags.
    int quality = 1;
    std::string qualityMsg;
    // Device time tag (CP56Time2a / CP24Time2a / IEC 103 CP32Time2a) in Unix ms, interpreted in
    // protocol.iec.timestampZone. 0 when the object carries no time tag or the tag is invalid.
    std::int64_t timestampMs = 0;
    // Command echo only: raw qualifier byte (SCO/DCO/RCO/QOS) incl. S/E bit.
    int commandQualifier = -1;
    // IEC 103 ASDU 1/2 only: the DPI octet exactly as received; -1 for every other object.
    // IEC 60870-5-103 uses bits 1..2 (1 = OFF, 2 = ON, 0/3 not used) and leaves bits 3..8
    // spare, so only 0x01 / 0x02 are on/off candidates; any other octet (DPI 0/3 or spare bits
    // set) is bad quality and the collector keeps the previous value (holdsPreviousValue).
    int rawDpiOctet = -1;
    // IEC 103 status evidence recorded by the decoder (ASDU 1/2 DPI, ASDU 44 status bit): the
    // message and raw fields that produced this value, e.g.
    // "asdu=1 cot=1(spontaneous) ca=1 fun=1 inf=100 dpiOctet=0x02". Empty for other objects.
    // Kept for diagnostics only (iec103StatusDiagnostic); never used to interpret the value.
    std::string evidence;
};

struct IecParameterValue {
    int commonAddress = 0;
    int ioa = 0;
    int typeId = 0;
    int cause = 0;
    int qualifier = 0;
    double value = 0.0;
    std::string rawHex;
    bool negative = false;
};

struct IecProtectionEvent {
    int commonAddress = 0;
    int ioa = 0;
    int typeId = 0;
    int cause = 0;
    // 17/38: ES of SEP (0..3); 18/39: SPE start-event bits; 19/40: OCI output-circuit bits.
    int eventState = 0;
    // 17/38: CP16Time2a elapsed time.
    int elapsedTimeMs = 0;
    // 18/39: relay duration time; 19/40: relay operating time (CP16Time2a).
    int relayDurationMs = 0;
    int quality = 1;
    std::string qualityMsg;
    std::int64_t timestampMs = 0;
    bool test = false;
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
    bool negative = false;
    bool test = false;
    int commonAddress = 0;
    // Qualifier of system commands (QOI of 100, QCC of 101); -1 otherwise.
    int commandQualifier = -1;
    std::vector<IecDataValue> values;
    std::vector<IecParameterValue> parameters;
    std::vector<IecProtectionEvent> protectionEvents;
    std::vector<IecFileSegment> fileSegments;
};

// IEC 101/104 transmission causes used by the master.
enum IecCause {
    kIecCauseActivation = 6,
    kIecCauseDeactivation = 8,
    kIecCauseActivationConfirmation = 7,
    kIecCauseActivationTermination = 10,
    kIecCauseUnknownType = 44,
    kIecCauseUnknownCause = 45,
    kIecCauseUnknownCommonAddress = 46,
    kIecCauseUnknownObjectAddress = 47
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
    static std::vector<std::uint8_t> buildIec104CounterInterrogationCommand(
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
    // select=true sets S/E; cancel=true sends COT 8 (deactivation). select+cancel is the
    // deselect that releases a selection after a failed select-before-execute sequence.
    static std::vector<std::uint8_t> buildIec104ControlCommand(
        const IecProtocolConfig& config,
        const PointDefinition& point,
        double requestedValue,
        std::uint16_t sendSequence,
        std::uint16_t receiveSequence,
        bool select,
        bool cancel = false,
        std::int64_t unixTimeMs = 0
    );
    // Command ASDU type a write point sends: write.iec.typeId, else derived from dataType.
    static int commandTypeOf(const PointDefinition& point);
    static bool isCommandType(int typeId);
    // Command types that carry the S/E (select/execute) bit.
    static bool commandSupportsSelect(int typeId);
    // Value the controlled station is expected to echo in the ACT_CON/ACT_TERM
    // of a command built by buildIec104ControlCommand (same raw quantisation).
    static double expectedControlEchoValue(const PointDefinition& point, double requestedValue);
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
    // APCI length octet must be 4..253 (IEC 60870-5-104 5.1).
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

    // IEC 101 / 103 application data units (without FT1.2 link framing).
    static std::vector<std::uint8_t> buildIec101InterrogationAsdu(const IecProtocolConfig& config);
    static std::vector<std::uint8_t> buildIec101CounterInterrogationAsdu(const IecProtocolConfig& config);
    // ASDU 7, COT 9 (general interrogation), SCN = returnInformationIdentifier.
    static std::vector<std::uint8_t> buildIec103GeneralInterrogationAsdu(const IecProtocolConfig& config);
    static std::vector<std::uint8_t> buildIec103RecordingDirectoryAsdu(const IecProtocolConfig& config);
    static std::vector<std::uint8_t> buildIec103ComtradeFileCallAsdu(
        const IecProtocolConfig& config,
        int fan,
        int fileType
    );
    static std::vector<Iec103DisturbanceRecord> decodeIec103RecordingDirectory(
        const std::vector<std::uint8_t>& asdu,
        const IecProtocolConfig& config
    );
    static Iec103ComtradeChunk decodeIec103ComtradeChunk(const std::vector<std::uint8_t>& asdu);
    // Online/clock broadcast: CP56Time2a of unixTimeMs (timestampZone) + stationName.
    static std::vector<std::uint8_t> buildAm5seUdpDiscoveryPacket(
        const IecProtocolConfig& config,
        std::int64_t unixTimeMs
    );

    // nowMs resolves time tags without a date (CP24Time2a, CP32Time2a); 0 = system clock.
    static IecAsdu decodeAsdu(
        const std::vector<std::uint8_t>& asdu,
        const IecProtocolConfig& config,
        std::int64_t nowMs = 0
    );
    // IEC 103 ASDU (type, VSQ, COT, CA, ...); 1-octet COT and CA as fixed by IEC 60870-5-103.
    static std::vector<IecDataValue> decodeIec103Data(
        const std::vector<std::uint8_t>& asdu,
        const IecProtocolConfig& config,
        std::int64_t nowMs = 0
    );

    // Protection event as a point value (eventState); ES of types 17/38 is double-point like.
    static IecDataValue protectionEventValue(const IecProtectionEvent& event);

    static bool pointMatches(const PointDefinition& point, const IecDataValue& value);
    // True when value cannot be represented by point: a double point with DPI 0/3 (no on/off
    // mapping) on a boolean point, or an IEC 103 DPI octet with spare bits set (on any point).
    // The collector then keeps the previous value and publishes it with bad quality.
    static bool holdsPreviousValue(const PointDefinition& point, const IecDataValue& value);
    static DecodedValue decodePointValue(const PointDefinition& point, const IecDataValue& value);
    // One-line diagnostic for IEC 103 status values (ASDU 1/2 DPI, ASDU 44 status bit): source
    // message and raw fields (value.evidence), the value actually stored (storedValue, or the
    // held previous value when held), quality and the mapping the code applied, always marked
    // verified=false: no vendor document or field record has confirmed the AM5SE semantics.
    // Empty for values without IEC 103 status evidence.
    static std::string iec103StatusDiagnostic(
        const PointDefinition& point,
        const IecDataValue& value,
        double storedValue,
        int quality,
        bool held
    );
    // GATEWAY_IEC103_DEBUG=1 (default off): raw IEC 103 change frames and status diagnostics are
    // logged to standard error (journal) for field comparison.
    static bool iec103DebugEnabled();
    static std::string toHex(const std::vector<std::uint8_t>& bytes);
};

}  // namespace edge_gateway
