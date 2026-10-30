// driver-review: can-dio (round 5). One control result per cmdId for SocketCAN and local DO writes,
// through the real drivers (CanDriverService over the udp_test transport, GatewayDaemon with the DIO
// executor over an in-memory GPIO port): the first MQTT reply, the shared-memory receipt, the control
// ledger record, a same-cmdId replay (nothing re-executed), the late final reply after the MQTT
// wait-timeout reply, and a ledger replay after a restart all agree, for each kind of result:
//   submitted   reached the transport/output only (kernel queue, DO latch without contact feedback):
//               outcome succeeded, stage writeback-submitted, verifyPassed=false
//   confirmed   a feedback signal/contact sent after the write equals it: writeback-succeeded, verifyPassed
//   mismatch    the feedback shows another value: writeback-mismatch, outcome unknown
//   unknown     no new good feedback value in verifyTimeoutMs: writeback-timeout, outcome unknown
//   failed      refused before anything was sent: writeback-failed
// Simulation only: it proves the gateway's bookkeeping, not what a real device does.
#include "control_dedup_test_support.hpp"
#include "edge_gateway/can_driver_service.hpp"
#include "edge_gateway/control_dedup_store.hpp"
#include "edge_gateway/dio_command_executor.hpp"
#include "edge_gateway/gateway_daemon.hpp"
#include "edge_gateway/mqtt_driver_service.hpp"
#include "edge_gateway/point_store_router.hpp"
#include "edge_gateway/virtual_can_datagram.hpp"
#include "edge_gateway/writeback_outcome.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace edge_gateway;
using dedup_test::require;

namespace {

std::int64_t wallNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// None: no answer. Invalid: the device answers with its "not available" raw value (bad quality).
enum class Feedback { None, Agrees, Opposite, Invalid };

struct Kind {
    std::string name;  // cmdId prefix
    std::uint32_t index;
    double value;
    bool success;
    std::string stage;
    std::string outcome;
    bool verifyPassed;
    std::string text;  // part of the executor message
    Feedback feedback;
};

class Publisher : public IMqttDriverPublisher {
public:
    std::vector<MqttIncomingMessage> incoming;
    std::vector<MqttCommandReply> replies;
    void publishFullSnapshot(const std::string&, const std::vector<StoredPointValue>&, const std::string&) override {}
    void publishAlarm(const std::string&, std::uint32_t, const StoredPointValue&, const std::string&, bool) override {}
    void publishOnDemand(const std::string&, const std::vector<StoredPointValue>&, const std::string&) override {}
    void publishChangeEvent(const std::string&, const StoredPointValue&) override {}
    void publishCommandReply(const std::string&, const MqttCommandReply& reply) override { replies.push_back(reply); }
    void publishOtaReply(const std::string&, const OtaReply&) override {}
    void publishOtaStatus(const std::string&, const OtaStatus&) override {}
    void publishJsonMessage(const std::string&, const std::string&) override {}
    std::vector<MqttIncomingMessage> pollIncoming(int) override {
        auto result = incoming;
        incoming.clear();
        return result;
    }
};

// Shared memory, MQTT service and a ledger directory; the driver is the subclass.
struct Harness {
    DeviceConfig config;
    std::string meterCode;
    std::string shm;
    std::unique_ptr<MemoryPointStore> store;
    PointStoreRouter router;
    std::shared_ptr<Publisher> publisher = std::make_shared<Publisher>();
    std::unique_ptr<MqttDriverService> service;
    int resultWaitMs = 0;

    virtual ~Harness() = default;
    virtual void execute(const Kind& kind) = 0;  // one driver writeback pass
    virtual int deviceWrites() const = 0;

    void base(const std::string& directory, const std::string& name) {
        shm = name;
        config.machineCode = "GW";
        config.memoryStore.sharedMemoryName = name;
        config.memoryStore.maxLatestPoints = 32;
        config.memoryStore.maxPendingWrites = 64;
        config.memoryStore.maxPersistentSamples = 8;
        config.memoryStore.writebackBatchSize = 16;
        config.memoryStore.sqlitePath.clear();
        config.memoryStore.controlDedupPath = directory + "/ledger.db";
        config.memoryStore.emsControlModeFile = directory + "/ems-mode.json";
        config.mqttDriver.priorityControlLeaseFile = directory + "/priority.json";
        config.mqttDriver.powerControlOwnershipFile = directory + "/owner.json";
        MemoryPointStore::cleanupOrphanedSegment(name);
        store.reset(new MemoryPointStore(config.memoryStore));
        router.addStore(name, *store);
        router.addRoutesFromDeviceConfigs({config}, name);
    }
    void startMqtt() {
        MqttConfig mqtt;
        mqtt.enabled = true;
        mqtt.clientId = "GW";
        mqtt.topicMachineCode = "GW";
        mqtt.commandReplyTopic = "edge/command/reply";
        MqttDriverConfig driver;
        driver.enabled = true;
        driver.sharedMemoryName = shm;
        driver.emsControlModeFile = config.memoryStore.emsControlModeFile;
        driver.priorityControlLeaseFile = config.mqttDriver.priorityControlLeaseFile;
        driver.powerControlOwnershipFile = config.mqttDriver.powerControlOwnershipFile;
        driver.scanIntervalMs = 100;
        driver.fullUploadIntervalMs = 0;
        driver.publishFullOnStart = false;
        driver.commandRateMaxPerWindow = 0;
        service.reset(new MqttDriverService(mqtt, driver, {config}, router, publisher));
        resultWaitMs = effectiveControlResultWaitMs(driver, {config});
    }
    void stop() {
        service.reset();
    }
    std::vector<MqttCommandReply> send(const std::string& cmdId, const Kind& kind, std::int64_t now) {
        MqttIncomingMessage message;
        message.type = MqttIncomingType::CommandRequest;
        std::ostringstream payload;
        payload << "{\"cmdId\":\"" << cmdId << "\",\"machineCode\":\"GW\",\"index\":" << kind.index
                << ",\"value\":" << kind.value << "}";
        message.payload = payload.str();
        publisher->incoming.push_back(message);
        return scan(now);
    }
    std::vector<MqttCommandReply> scan(std::int64_t now) {
        const auto before = publisher->replies.size();
        service->runScanOnce(now);
        return std::vector<MqttCommandReply>(publisher->replies.begin() + static_cast<std::ptrdiff_t>(before),
                                             publisher->replies.end());
    }
    Optional<WritebackResultRecord> ledger(const std::string& cmdId) const {
        return ControlDedupStore(config.memoryStore.controlDedupPath).result("GW", meterCode, cmdId);
    }
};

// ---- SocketCAN (udp_test transport): the test plays the device and answers with a status frame.
int boundUdpSocket(int& port) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(fd >= 0 && ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "udp bind");
    socklen_t length = sizeof(address);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
    port = ntohs(address.sin_port);
    return fd;
}

constexpr std::uint32_t kStatusFrame = 0x18FF10E5;

struct CanHarness : Harness {
    int peer = -1;
    int peerPort = 0;
    int driverPort = 0;
    int sender = -1;
    std::unique_ptr<CanDriverService> can;
    int frames = 0;

    CanHarness(const std::string& directory, const std::string& name) {
        peer = boundUdpSocket(peerPort);
        const int probe = boundUdpSocket(driverPort);
        ::close(probe);
        sender = ::socket(AF_INET, SOCK_DGRAM, 0);
        meterCode = "CANDEV";
        config.protocol.type = "can_socketcan";
        config.protocol.can.transportMode = "udp_test";
        config.protocol.can.manageInterface = false;
        config.protocol.can.udpBindAddress = "127.0.0.1";
        config.protocol.can.udpListenPort = driverPort;
        config.protocol.can.udpPeerAddress = "127.0.0.1";
        config.protocol.can.udpPeerPort = peerPort;
        config.protocol.can.frameStateFile = directory + "/frame-state.json";
        config.collect.receiveWaitMs = 10;
        LogicalDeviceConfig meter;
        meter.meterCode = meterCode;
        meter.onlineTimeoutMs = 60000;
        meter.onlineFrameIds = {"0x18FF10E5"};
        const auto writePoint = [](std::uint32_t index, const std::string& code, const std::string& type,
                                   const std::string& frame) {
            PointDefinition point;
            point.index = index;
            point.pointCode = code;
            point.enabled = true;
            point.write.enable = true;
            point.write.dataType = type;
            point.write.can.frameId = frame;
            point.write.can.dlc = 8;
            point.write.can.bitLength = type == "bool" ? 1 : 8;
            return point;
        };
        meter.points.push_back(writePoint(320101, "relay_cmd", "bool", "0x321"));       // no feedback
        auto start = writePoint(320102, "start_cmd", "bool", "0x322");
        start.write.verifyIndex = 320201;  // run_state, sent by the device
        start.write.verifyTimeoutMs = 300;
        meter.points.push_back(start);
        meter.points.push_back(writePoint(320103, "limit_sp", "uint8", "0x323"));
        auto mode = writePoint(320104, "mode_cmd", "uint8", "0x324");
        mode.write.verifyIndex = 320202;  // mode_state; 0xFF = not available
        mode.write.verifyTimeoutMs = 300;
        meter.points.push_back(mode);
        PointDefinition state;
        state.index = 320201;
        state.pointCode = "run_state";
        state.enabled = true;
        state.read.enable = true;
        state.read.dataType = "bool";
        state.read.can.frameId = "0x18FF10E5";
        state.read.can.extended = true;
        state.read.can.bitLength = 1;
        state.read.can.receiveTimeoutMs = 0;
        state.read.cachePolicy.ttlMs = 600000;
        meter.points.push_back(state);
        PointDefinition modeState = state;
        modeState.index = 320202;
        modeState.pointCode = "mode_state";
        modeState.read.dataType = "uint8";
        modeState.read.can.byteOffset = 1;
        modeState.read.can.bitLength = 8;
        modeState.read.can.invalidRawValues = {0xFF};
        meter.points.push_back(modeState);
        config.meters.push_back(meter);
        base(directory, name);
        can.reset(new CanDriverService(config, *store));
        startMqtt();
    }
    ~CanHarness() override {
        stop();
        can.reset();
        store.reset();
        MemoryPointStore::cleanupOrphanedSegment(shm);
        ::close(peer);
        ::close(sender);
    }
    bool receiveSent() {
        std::uint8_t buffer[128];
        VirtualCanFrame frame;
        const auto size = ::recv(peer, buffer, sizeof(buffer), MSG_DONTWAIT);
        return size > 0 && decodeVirtualCanDatagram(buffer, static_cast<std::size_t>(size), frame) && !frame.error;
    }
    void status(std::size_t byte, std::uint8_t value) {
        VirtualCanFrame frame;
        frame.frameId = kStatusFrame;
        frame.extended = true;
        frame.payload = {0, 0, 0, 0, 0, 0, 0, 0};
        frame.payload[byte] = value;
        const auto bytes = encodeVirtualCanDatagram(frame);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(driverPort));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::sendto(sender, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    }
    // The writeback thread waits for the feedback while the receive side runs, as in the driver.
    void execute(const Kind& kind) override {
        std::atomic<bool> finished(false);
        std::thread writer([&] {
            can->processWritebackOnce(wallNow());
            finished = true;
        });
        bool answered = false;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!finished.load() && std::chrono::steady_clock::now() < until) {
            if (receiveSent()) {
                ++frames;
                if (kind.feedback != Feedback::None && !answered) {
                    // A device answers after the send (values of the send's millisecond do not count).
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                    const std::size_t byte = kind.index == 320104 ? 1 : 0;
                    const auto raw = kind.feedback == Feedback::Invalid ? 0xFF
                        : kind.feedback == Feedback::Agrees ? kind.value : 1.0 - kind.value;
                    status(byte, static_cast<std::uint8_t>(raw));
                    answered = true;
                }
            }
            can->processReceiveOnce(5);
        }
        writer.join();
        while (receiveSent()) ++frames;
    }
    int deviceWrites() const override { return frames; }
};

// ---- local DO over an in-memory GPIO port; DI 21 is the auxiliary contact of DO 11.
class FakeGpio : public IGpioPort {
public:
    Feedback contact = Feedback::Agrees;
    int writes = 0;
    void exportGpio(int) override {}
    void setDirection(int gpio, const std::string& direction) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (direction != "in") outputs_.insert(gpio);
    }
    bool readValue(int gpio) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (gpio == 21) {
            const bool output = level_[11];
            return contact == Feedback::Agrees ? output : !output;
        }
        return level_[gpio];
    }
    void writeValue(int gpio, bool high) override {
        std::lock_guard<std::mutex> lock(mutex_);
        level_[gpio] = high;
        ++writes;
    }
    void setOutputDirection(int gpio, bool high) override {
        std::lock_guard<std::mutex> lock(mutex_);
        outputs_.insert(gpio);
        level_[gpio] = high;
        ++writes;
    }
    bool ensureOutput(int gpio, bool high) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (outputs_.count(gpio)) return false;
        outputs_.insert(gpio);
        level_[gpio] = high;
        return true;
    }
private:
    std::mutex mutex_;
    std::map<int, bool> level_;
    std::set<int> outputs_;
};

class IdleCollector : public ICollector {
public:
    CollectCycleResult collectOnce(std::int64_t, bool) override { return CollectCycleResult(); }
    void publishDeviceOnlineStatus(bool, std::int64_t) const override {}
};

struct DioHarness : Harness {
    std::shared_ptr<FakeGpio> gpio = std::make_shared<FakeGpio>();
    std::unique_ptr<GatewayDaemon> daemon;

    DioHarness(const std::string& directory, const std::string& name) {
        meterCode = "DIO";
        config.meterCode = meterCode;
        config.protocol.type = "local_dio";
        const auto output = [](std::uint32_t index, const std::string& code, int pin) {
            PointDefinition point;
            point.index = index;
            point.pointCode = code;
            point.enabled = true;
            point.read.enable = true;
            point.read.dataType = "digital_output";
            point.read.gpio = pin;
            point.read.activeHigh = true;
            point.write.enable = true;
            point.write.dataType = "digital_output";
            point.write.allowedValues = {0, 1};
            point.write.verifyAfterWrite = true;  // latch read-back: still only the output state
            point.write.verifyByRead = true;
            point.write.verifyDelayMs = 0;
            return point;
        };
        config.points.push_back(output(330101, "do_plain", 10));
        auto confirmed = output(330102, "do_feedback", 11);
        confirmed.write.verifyIndex = 330201;
        confirmed.write.verifyTimeoutMs = 200;
        config.points.push_back(confirmed);
        auto remote = output(330103, "do_remote_feedback", 12);
        remote.write.verifyIndex = 339999;  // another device's DI, never collected here
        remote.write.verifyTimeoutMs = 200;
        config.points.push_back(remote);
        PointDefinition contact;
        contact.index = 330201;
        contact.pointCode = "di_aux";
        contact.enabled = true;
        contact.read.enable = true;
        contact.read.dataType = "digital_input";
        contact.read.gpio = 21;
        contact.read.activeHigh = true;
        config.points.push_back(contact);
        base(directory, name);
        const auto port = gpio;
        daemon.reset(new GatewayDaemon(config, *store,
            [](const DeviceConfig&, MemoryPointStore&) { return std::unique_ptr<ICollector>(new IdleCollector()); },
            [port](const DeviceConfig& runtime, MemoryPointStore& runtimeStore) {
                return std::unique_ptr<ICommandExecutor>(new DioCommandExecutor(runtime, runtimeStore, port));
            },
            {}));
        startMqtt();
    }
    ~DioHarness() override {
        stop();
        daemon.reset();
        store.reset();
        MemoryPointStore::cleanupOrphanedSegment(shm);
    }
    void execute(const Kind& kind) override {
        gpio->contact = kind.feedback == Feedback::Opposite ? Feedback::Opposite : Feedback::Agrees;
        daemon->processWritebackOnce(wallNow());
    }
    int deviceWrites() const override { return gpio->writes; }
};

const std::vector<Kind>& canKinds() {
    static const std::vector<Kind> kinds = {
        {"CANSUB", 320101, 1, true, "writeback-submitted", "succeeded", false, "queued by the kernel", Feedback::None},
        {"CANOK", 320102, 1, true, "writeback-succeeded", "succeeded", true, "=1 confirms", Feedback::Agrees},
        {"CANMIS", 320102, 1, false, "writeback-mismatch", "unknown", false, "feedback mismatch", Feedback::Opposite},
        {"CANUNK", 320102, 1, false, "writeback-timeout", "unknown", false, "no new value", Feedback::None},
        {"CANBAD", 320104, 3, false, "writeback-timeout", "unknown", false, "stale or bad quality", Feedback::Invalid},
        {"CANFAIL", 320103, 300, false, "writeback-failed", "failed", false, "", Feedback::None},
    };
    return kinds;
}

const std::vector<Kind>& dioKinds() {
    static const std::vector<Kind> kinds = {
        {"DOSUB", 330101, 1, true, "writeback-submitted", "succeeded", false, "未配置反馈，触点动作未确认", Feedback::None},
        {"DOOK", 330102, 1, true, "writeback-succeeded", "succeeded", true, "动作已确认", Feedback::Agrees},
        {"DOMIS", 330102, 1, false, "writeback-mismatch", "unknown", false, "反馈不一致", Feedback::Opposite},
        {"DOUNK", 330103, 1, false, "writeback-timeout", "unknown", false, "结果未知", Feedback::None},
    };
    return kinds;
}

std::string describe(const MqttCommandReply& reply) {
    return "success=" + std::to_string(reply.success) + " outcome=" + reply.outcome + " stage=" + reply.stage +
        " final=" + std::to_string(reply.isFinal) + " verifyPassed=" + std::to_string(reply.verifyPassed) +
        " message=" + reply.message;
}

void requireReply(const MqttCommandReply& reply, const Kind& kind, const std::string& what) {
    require(reply.success == kind.success && reply.outcome == kind.outcome && reply.stage == kind.stage &&
            reply.verifyPassed == kind.verifyPassed && reply.isFinal &&
            reply.message.find(kind.text) != std::string::npos && reply.message.size() <= 127,
            what + " " + kind.name + ": " + describe(reply));
    // Nothing short of a confirmed feedback may read like a confirmed action.
    if (!kind.verifyPassed) {
        require(reply.message.find("confirms") == std::string::npos &&
                reply.message.find("动作已确认") == std::string::npos, what + " claims a confirmation: " + reply.message);
    }
}

void requireRecord(const Optional<WritebackResultRecord>& record, const Kind& kind, const std::string& message,
                   const std::string& what) {
    require(record && record->success == kind.success && writebackOutcome(*record) == kind.outcome &&
            record->stage == kind.stage && record->message == message && record->verifyPassed == kind.verifyPassed &&
            record->outcomeUnknown == (kind.outcome == "unknown"),
            what + " " + kind.name + (record ? ": stage=" + record->stage + " message=" + record->message : ": missing"));
}

std::vector<MqttCommandReply> single(const std::vector<MqttCommandReply>& replies, const std::string& what) {
    require(replies.size() == 1, what + ": expected one reply, got " + std::to_string(replies.size()));
    return replies;
}

constexpr std::int64_t kT0 = 1770000045000LL;

// First reply, ledger, shared memory, replay without execution, late final reply.
std::map<std::string, std::string> firstReplayAndLate(Harness& gateway, const std::vector<Kind>& kinds) {
    std::map<std::string, std::string> messages;
    for (const auto& kind : kinds) {
        const auto id = kind.name + "_FIRST";
        require(gateway.send(id, kind, kT0).empty(), "accepted command waits for its result");
        gateway.execute(kind);
        const auto first = single(gateway.scan(kT0 + 20), "first " + kind.name).front();
        requireReply(first, kind, "first reply");
        requireRecord(gateway.ledger(id), kind, first.message, "ledger record");
        requireRecord(gateway.store->getWritebackResult(id, kind.index), kind, first.message, "shared-memory receipt");
        messages[id] = first.message;

        const auto writes = gateway.deviceWrites();
        const auto replay = single(gateway.send(id, kind, kT0 + 30), "replay " + kind.name).front();
        requireReply(replay, kind, "replay");
        require(replay.message == first.message, "replay keeps the message of " + kind.name);
        gateway.execute(kind);
        require(gateway.deviceWrites() == writes, "replay of " + kind.name + " must not write again");

        const auto late = kind.name + "_LATE";
        gateway.send(late, kind, kT0);
        const auto timeout = single(gateway.scan(kT0 + gateway.resultWaitMs), "timeout " + kind.name).front();
        require(!timeout.success && timeout.outcome == "unknown" && !timeout.isFinal &&
                timeout.stage == "writeback-timeout", "wait timeout is a non-final unknown for " + kind.name);
        gateway.execute(kind);
        const auto final = single(gateway.scan(kT0 + gateway.resultWaitMs + 20), "late " + kind.name).front();
        requireReply(final, kind, "late final reply");
        requireRecord(gateway.ledger(late), kind, final.message, "late ledger record");
        messages[late] = final.message;
        require(gateway.scan(kT0 + 4LL * gateway.resultWaitMs).empty(), "nothing after the final reply");
    }
    return messages;
}

// After a restart (new shared memory, same ledger): replays come from the ledger, nothing is written.
void restartReplay(Harness& gateway, const std::vector<Kind>& kinds, const std::map<std::string, std::string>& messages) {
    for (const auto& kind : kinds) {
        for (const auto* suffix : {"_FIRST", "_LATE"}) {
            const auto id = kind.name + suffix;
            const auto reply = single(gateway.send(id, kind, kT0 + 1000), "restart replay " + id).front();
            requireReply(reply, kind, "replay after restart");
            require(reply.message == messages.at(id), "restart replay keeps the message of " + id);
            requireRecord(gateway.ledger(id), kind, reply.message, "ledger after restart");
            gateway.execute(kind);
        }
    }
    require(gateway.deviceWrites() == 0, "nothing is written again after a restart");
}

void run(const std::string& label, const std::vector<Kind>& kinds,
         const std::function<std::unique_ptr<Harness>(const std::string&, const std::string&)>& make) {
    dedup_test::Directory dir;
    std::map<std::string, std::string> messages;
    {
        auto gateway = make(dir.path.string(), label + "_a_" + std::to_string(getpid()));
        messages = firstReplayAndLate(*gateway, kinds);
    }
    auto restarted = make(dir.path.string(), label + "_b_" + std::to_string(getpid()));
    restartReplay(*restarted, kinds, messages);
}

// A non-durable command (e.g. from the compute engine, no ledger) takes the daemon's plain success
// path: a DO without contact feedback is still only submitted.
void nonDurableDoIsSubmitted() {
    dedup_test::Directory dir;
    DioHarness gateway(dir.path.string(), "dio_outcome_plain_" + std::to_string(getpid()));
    PendingWriteCommand command;
    command.cmdId = "PLAIN_DO";
    command.index = 330101;
    command.value = 1;
    command.source = "compute-engine";
    command.ts = command.acceptedAt = wallNow();
    gateway.store->submitWriteCommand(command);
    gateway.daemon->processWritebackOnce(wallNow());
    const auto receipt = gateway.store->getWritebackResult("PLAIN_DO", 330101);
    require(receipt && receipt->success && receipt->stage == "writeback-submitted" && !receipt->verifyAttempted &&
            !receipt->verifyPassed && std::string(writebackOutcome(*receipt)) == "succeeded" &&
            receipt->message.find("未配置反馈") != std::string::npos,
            "plain DO receipt is submitted only: " + (receipt ? receipt->stage + " " + receipt->message : "missing"));
}

}  // namespace

int main() {
    try {
        run("can_outcome", canKinds(), [](const std::string& directory, const std::string& name) {
            return std::unique_ptr<Harness>(new CanHarness(directory, name));
        });
        run("dio_outcome", dioKinds(), [](const std::string& directory, const std::string& name) {
            return std::unique_ptr<Harness>(new DioHarness(directory, name));
        });
        nonDurableDoIsSubmitted();
        std::cout << "can_dio_control_outcome_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "can_dio_control_outcome_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
