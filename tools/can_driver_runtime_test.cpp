// CanDriverService behaviour over the udp_test transport (driver review, CAN items 2-5, 7, 8):
// per-signal error isolation, decode failures keep the old value without history, invalid raw
// values, frame mirror + batch merge, per-frame CAN FD, bus-off / interface_online, steady-clock
// timeouts, and reception that keeps running while another driver holds a priority lease.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "edge_gateway/can_driver_service.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/priority_control_lease.hpp"
#include "edge_gateway/virtual_can_datagram.hpp"

namespace {

using namespace edge_gateway;
using Bytes = std::vector<std::uint8_t>;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << std::endl;
    }
}

std::int64_t wallNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

int boundUdpSocket(int& port) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        throw std::runtime_error("udp bind failed");
    }
    socklen_t length = sizeof(address);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
    port = ntohs(address.sin_port);
    return fd;
}

int freePort() {
    int port = 0;
    const int fd = boundUdpSocket(port);
    ::close(fd);
    return port;
}

struct Bus {
    int driverPort = 0;
    int peerPort = 0;
    int peer = -1;  // receives what the driver sends
    int sender = -1;

    Bus() {
        peer = boundUdpSocket(peerPort);
        driverPort = freePort();
        sender = ::socket(AF_INET, SOCK_DGRAM, 0);
        timeval timeout{0, 200000};
        ::setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }
    ~Bus() {
        ::close(peer);
        ::close(sender);
    }

    void send(const VirtualCanFrame& frame) const {
        const auto bytes = encodeVirtualCanDatagram(frame);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(driverPort));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::sendto(sender, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    }
    void data(std::uint32_t id, bool extended, const Bytes& payload) const {
        VirtualCanFrame frame;
        frame.frameId = id;
        frame.extended = extended;
        frame.payload = payload;
        send(frame);
    }
    // SocketCAN error frame: class bits in the id, 8 data bytes (linux/can/error.h).
    void error(std::uint32_t errorClass, std::uint8_t controllerStatus = 0) const {
        VirtualCanFrame frame;
        frame.error = true;
        frame.frameId = errorClass;
        frame.payload = Bytes(8, 0);
        frame.payload[1] = controllerStatus;
        send(frame);
    }
    bool receive(VirtualCanFrame& frame) const {
        std::uint8_t buffer[128];
        const auto size = ::recv(peer, buffer, sizeof(buffer), 0);
        return size > 0 && decodeVirtualCanDatagram(buffer, static_cast<std::size_t>(size), frame);
    }
};

PointDefinition readPoint(std::uint32_t index, const std::string& code, const std::string& type,
                          const std::string& frameId, int byteOffset) {
    PointDefinition point;
    point.index = index;
    point.pointCode = code;
    point.name = code;
    point.enabled = true;
    point.read.enable = true;
    point.read.dataType = type;
    point.read.can.frameId = frameId;
    point.read.can.extended = true;
    point.read.can.byteOffset = byteOffset;
    point.read.can.receiveTimeoutMs = 1000;
    point.read.cachePolicy.ttlMs = 600000;
    return point;
}

PointDefinition writePoint(std::uint32_t index, const std::string& code, const std::string& type,
                           const std::string& frameId, int byteOffset, int bitLength) {
    PointDefinition point;
    point.index = index;
    point.pointCode = code;
    point.name = code;
    point.enabled = true;
    point.write.enable = true;
    point.write.dataType = type;
    point.write.can.frameId = frameId;
    point.write.can.dlc = 8;
    point.write.can.byteOffset = byteOffset;
    point.write.can.bitLength = bitLength;
    return point;
}

PointDefinition statusPoint(std::uint32_t index, const std::string& code, const std::string& type) {
    PointDefinition point;
    point.index = index;
    point.pointCode = code;
    point.enabled = true;
    point.read.enable = true;
    point.read.dataType = type;
    point.read.cachePolicy.ttlMs = 600000;
    return point;
}

const std::uint32_t kSpeed = 310101;
const std::uint32_t kTemp = 310102;
const std::uint32_t kRpm = 310103;
const std::uint32_t kBroken = 310104;
const std::uint32_t kDeviceOnline = 310000;
const std::uint32_t kInterfaceOnline = 310001;

DeviceConfig makeConfig(const Bus& bus, const std::string& name, const std::string& directory) {
    DeviceConfig config;
    config.machineCode = "CANRT";
    config.protocol.type = "can_socketcan";
    config.protocol.can.transportMode = "udp_test";
    config.protocol.can.manageInterface = false;
    config.protocol.can.fdEnabled = true;
    config.protocol.can.udpBindAddress = "127.0.0.1";
    config.protocol.can.udpListenPort = bus.driverPort;
    config.protocol.can.udpPeerAddress = "127.0.0.1";
    config.protocol.can.udpPeerPort = bus.peerPort;
    config.collect.receiveWaitMs = 20;
    config.collect.interfaceCheckIntervalMs = 50;
    config.memoryStore.sharedMemoryName = name;
    config.memoryStore.sqlitePath.clear();
    config.memoryStore.maxLatestPoints = 64;
    config.memoryStore.maxPendingWrites = 32;
    config.memoryStore.maxPersistentSamples = 64;
    config.memoryStore.writebackBatchSize = 32;
    config.mqttDriver.priorityControlLeaseFile = directory + "/priority.json";
    config.mqttDriver.powerControlOwnershipFile = directory + "/owner.json";
    config.protocol.can.frameStateFile = directory + "/frame-state-" + name + ".json";
    LogicalDeviceConfig meter;
    meter.meterCode = "DEV1";
    meter.deviceName = "dev";
    meter.onlineTimeoutMs = 1000;
    meter.onlineFrameIds = {"0x18FF50E5"};
    meter.points.push_back(statusPoint(kDeviceOnline, "device_online", "device_online"));
    meter.points.push_back(statusPoint(kInterfaceOnline, "can_interface_online", "interface_online"));
    meter.points.push_back(readPoint(kSpeed, "speed", "uint16", "0x18FF50E5", 0));
    auto temp = readPoint(kTemp, "temp", "uint16", "0x18FF50E5", 6);
    temp.isStore = true;
    temp.persistOnChange = true;
    meter.points.push_back(temp);
    auto rpm = readPoint(kRpm, "rpm", "uint16", "0x18FF50E5", 2);
    rpm.read.can.invalidRawValues = {0xFFFF};
    meter.points.push_back(rpm);
    // Misconfigured signal (float32 needs 32 bits): fails on every frame without affecting the others.
    auto broken = readPoint(kBroken, "broken", "float32", "0x18FF50E5", 4);
    broken.read.can.bitLength = 16;
    meter.points.push_back(broken);
    auto run = writePoint(310201, "run_cmd", "bool", "0x321", 0, 1);
    run.write.can.payloadTemplate = "00000000000000AA";
    meter.points.push_back(run);
    meter.points.push_back(writePoint(310202, "power_sp", "uint8", "0x321", 1, 8));
    auto fd = writePoint(310203, "fd_sp", "uint8", "0x322", 11, 8);
    fd.write.can.fd = true;
    fd.write.can.dlc = 12;
    meter.points.push_back(fd);
    // Frame 0x323: a one-shot reset bit next to a setpoint.
    auto reset = writePoint(310204, "reset_cmd", "bool", "0x323", 0, 1);
    reset.write.can.pulse = true;
    meter.points.push_back(reset);
    meter.points.push_back(writePoint(310205, "limit_sp", "uint8", "0x323", 1, 8));
    config.meters.push_back(meter);
    return config;
}

Optional<StoredPointValue> latest(MemoryPointStore& store, std::uint32_t index) {
    return store.getLatestByIndex(index, wallNow());
}

void pump(CanDriverService& service, int frames) {
    for (int i = 0; i < frames; ++i) {
        service.processReceiveOnce(500);
    }
}

std::size_t persistedFor(MemoryPointStore& store, std::uint32_t index, double value) {
    std::size_t count = 0;
    for (const auto& sample : store.peekPersistentSamples(64)) {
        if (sample.index == index && sample.value == value) ++count;
    }
    return count;
}

void submit(MemoryPointStore& store, const std::string& cmdId, std::uint32_t index, double value) {
    PendingWriteCommand command;
    command.cmdId = cmdId;
    command.index = index;
    command.value = value;
    command.source = "compute-engine";
    command.ts = command.acceptedAt = wallNow();
    store.submitWriteCommand(command);
}

void receivePathAndWrites(const std::string& directory) {
    Bus bus;
    const auto name = "can_runtime_test_" + std::to_string(getpid());
    MemoryPointStore::cleanupOrphanedSegment(name);
    {
        auto config = makeConfig(bus, name, directory);
        MemoryPointStore store(config.memoryStore);
        CanDriverService service(config, store);
        check(latest(store, kInterfaceOnline) && latest(store, kInterfaceOnline)->value == 0.0,
              "interface_online starts at 0");
        service.checkInterfaceOnce(wallNow(), CanDriverService::steadyNowMs());
        check(latest(store, kInterfaceOnline)->value == 1.0, "interface_online=1 once the socket is open");

        // Item 2/4/7: full frame; the broken signal fails alone, the invalid rpm is bad quality.
        bus.data(0x18FF50E5, true, {0x10, 0x27, 0x00, 0x01, 0, 0, 0x34, 0x12});
        pump(service, 1);
        check(latest(store, kSpeed)->value == 10000.0 && latest(store, kSpeed)->quality == 1, "speed decoded");
        check(latest(store, kTemp)->value == 0x1234 && latest(store, kTemp)->quality == 1, "temp decoded");
        check(latest(store, kRpm)->value == 256.0, "rpm decoded");
        const auto broken = latest(store, kBroken);
        check(broken && broken->quality == 0, "a misconfigured signal is bad quality, not a stopped bus");
        check(latest(store, kDeviceOnline)->value == 1.0, "device online after its frame");
        check(persistedFor(store, kTemp, 0x1234) == 1, "temp persisted once");

        bus.data(0x18FF50E5, true, {0x20, 0x4E, 0xFF, 0xFF});  // DLC 4: temp missing, rpm = 0xFFFF
        pump(service, 1);
        check(latest(store, kSpeed)->value == 20000.0 && latest(store, kSpeed)->quality == 1,
              "speed still decodes from the short frame");
        const auto temp = latest(store, kTemp);
        check(temp && temp->quality == 0 && temp->value == 0x1234, "short frame keeps temp's old value, bad quality");
        check(persistedFor(store, kTemp, 0.0) == 0, "decode failure never reaches the history database");
        const auto rpm = latest(store, kRpm);
        check(rpm && rpm->quality == 0 && rpm->value == 256.0, "0xFFFF (not available) keeps the old rpm, bad quality");

        // Item 5: frame mirror. run_cmd=1 then power_sp=55 keeps run_cmd; the template byte stays.
        VirtualCanFrame sent;
        submit(store, "W1", 310201, 1);
        service.processWritebackOnce(wallNow());
        check(bus.receive(sent) && sent.frameId == 0x321 && !sent.fd &&
              sent.payload == Bytes({0x01, 0, 0, 0, 0, 0, 0, 0xAA}), "run_cmd=1 frame from the template");
        submit(store, "W2", 310202, 55);
        service.processWritebackOnce(wallNow());
        check(bus.receive(sent) && sent.payload == Bytes({0x01, 55, 0, 0, 0, 0, 0, 0xAA}),
              "power_sp=55 keeps run_cmd=1");
        // One batch, two signals of one frame: a single frame carries both.
        submit(store, "W3", 310201, 0);
        submit(store, "W4", 310202, 66);
        service.processWritebackOnce(wallNow());
        check(bus.receive(sent) && sent.payload == Bytes({0x00, 66, 0, 0, 0, 0, 0, 0xAA}), "batch merged into one frame");
        check(!bus.receive(sent), "no second frame for the merged batch");
        check(store.getWritebackResult("W3")->success && store.getWritebackResult("W4")->success, "both merged writes succeed");
        // The same signal twice in a batch is sent twice, in order (a pulse is never swallowed).
        submit(store, "W5", 310201, 1);
        submit(store, "W6", 310201, 0);
        service.processWritebackOnce(wallNow());
        check(bus.receive(sent) && sent.payload[0] == 1, "pulse first edge sent");
        check(bus.receive(sent) && sent.payload[0] == 0 && sent.payload[1] == 66, "pulse second edge sent");
        // Item 7: FD per frame.
        submit(store, "W7", 310203, 9);
        service.processWritebackOnce(wallNow());
        check(bus.receive(sent) && sent.frameId == 0x322 && sent.fd && sent.payload.size() == 12 && sent.payload[11] == 9,
              "fd:true frame sent as CAN FD");
        submit(store, "W8", 310202, 300);  // out of range for uint8
        service.processWritebackOnce(wallNow());
        check(!store.getWritebackResult("W8")->success && !bus.receive(sent), "out-of-range write rejected unsent");

        // Item 3: bus-off -> interface offline, device offline, signals stale with their last value.
        bus.error(0x040);  // CAN_ERR_BUSOFF
        pump(service, 1);
        check(!service.interfaceOnline() && latest(store, kInterfaceOnline)->value == 0.0, "bus-off: interface_online=0");
        check(latest(store, kDeviceOnline)->value == 0.0, "bus-off: device offline");
        // A write during bus-off is refused, not queued to go out whenever the bus recovers.
        submit(store, "W9", 310202, 77);
        service.processWritebackOnce(wallNow());
        const auto refused = store.getWritebackResult("W9");
        check(refused && !refused->success && refused->stage == "writeback-failed" &&
              refused->message.find("offline") != std::string::npos, "write during bus-off refused");
        check(!bus.receive(sent), "nothing sent during bus-off");
        const auto stale = latest(store, kSpeed);
        check(stale && stale->quality == 0 && stale->value == 20000.0, "bus-off: speed bad quality, value kept");
        bus.error(0x004, 0x10);  // controller error-passive: reported, no state change while bus-off
        pump(service, 1);
        bus.error(0x100);  // CAN_ERR_RESTARTED
        pump(service, 1);
        check(service.interfaceOnline() && latest(store, kInterfaceOnline)->value == 1.0, "restart: interface_online=1");
        check(latest(store, kDeviceOnline)->value == 0.0, "restart: device stays offline until it sends");
        bus.data(0x18FF50E5, true, {0x30, 0x75, 0, 1, 0, 0, 0x34, 0x12});
        pump(service, 1);
        check(latest(store, kDeviceOnline)->value == 1.0 && latest(store, kSpeed)->value == 30000.0 &&
              latest(store, kSpeed)->quality == 1, "frames after restart bring the device back");

        // Item 8: timeouts run on the steady clock; a wall-clock step back changes nothing.
        const auto mono = CanDriverService::steadyNowMs();
        service.updateOnlineStatus(wallNow() - 3600000, mono + 100);
        check(latest(store, kDeviceOnline)->value == 1.0, "wall clock step back keeps the device online");
        service.updateOnlineStatus(wallNow(), mono + 1500);
        check(latest(store, kDeviceOnline)->value == 0.0, "1.5 s steady time without frames: device offline");
        check(latest(store, kSpeed)->quality == 0 && latest(store, kSpeed)->value == 30000.0,
              "receive timeout: bad quality, value kept");
    }
    MemoryPointStore::cleanupOrphanedSegment(name);
}

void typoFrameIdFailsAtStartup(const std::string& directory) {
    Bus bus;
    const auto name = "can_runtime_typo_" + std::to_string(getpid());
    MemoryPointStore::cleanupOrphanedSegment(name);
    {
        auto config = makeConfig(bus, name, directory);
        config.meters[0].points[2].read.can.frameId = "18FF50E5";  // missing 0x
        MemoryPointStore store(config.memoryStore);
        bool rejected = false;
        try {
            CanDriverService service(config, store);
        } catch (const std::invalid_argument& ex) {
            rejected = std::string(ex.what()).find("speed") != std::string::npos;
        }
        check(rejected, "a frameId without 0x is rejected at start, naming the point");
    }
    MemoryPointStore::cleanupOrphanedSegment(name);
}

// Item 7: another driver's priority lease must not pause CAN reception.
void receptionIgnoresPriorityLease(const std::string& directory) {
    Bus bus;
    const auto name = "can_runtime_lease_" + std::to_string(getpid());
    MemoryPointStore::cleanupOrphanedSegment(name);
    {
        auto config = makeConfig(bus, name, directory);
        PriorityControlLease other(config.mqttDriver.priorityControlLeaseFile, "mqtt-driver");
        other.acquire("PRIO", "PCS", 1, wallNow(), 30000);
        MemoryPointStore store(config.memoryStore);
        CanDriverService service(config, store);
        service.start();
        bool updated = false;
        for (int i = 0; i < 40 && !updated; ++i) {
            bus.data(0x18FF50E5, true, {0x01, 0x00, 0, 1, 0, 0, 0x34, 0x12});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const auto speed = latest(store, kSpeed);
            updated = speed && speed->value == 1.0;
        }
        service.stop();
        other.release("PRIO");
        check(updated, "frames are decoded while another driver holds a priority lease");
    }
    MemoryPointStore::cleanupOrphanedSegment(name);
}

VirtualCanFrame writeAndReceive(Bus& bus, MemoryPointStore& store, CanDriverService& service,
                                const std::vector<std::pair<std::uint32_t, double>>& writes, const std::string& tag) {
    int n = 0;
    for (const auto& write : writes) {
        submit(store, tag + std::to_string(n++), write.first, write.second);
    }
    service.processWritebackOnce(wallNow());
    VirtualCanFrame frame;
    if (!bus.receive(frame)) {
        check(false, tag + ": no frame sent");
    }
    return frame;
}

// Round 2, item 1: a pulse bit goes out in one frame only.
void pulseSignals(const std::string& directory) {
    Bus bus;
    const auto name = "can_runtime_pulse_" + std::to_string(getpid());
    MemoryPointStore::cleanupOrphanedSegment(name);
    {
        auto config = makeConfig(bus, name, directory);
        MemoryPointStore store(config.memoryStore);
        CanDriverService service(config, store);
        auto frame = writeAndReceive(bus, store, service, {{310204, 1}}, "P1");
        check(frame.frameId == 0x323 && frame.payload[0] == 1, "reset pulse sent");
        frame = writeAndReceive(bus, store, service, {{310205, 55}}, "P2");
        check(frame.payload[0] == 0 && frame.payload[1] == 55, "the next write of the frame does not repeat the pulse");
        frame = writeAndReceive(bus, store, service, {{310204, 1}, {310205, 60}}, "P3");
        check(frame.payload[0] == 1 && frame.payload[1] == 60, "pulse merged with a setpoint in one frame");
        check(!bus.receive(frame), "merged batch is one frame");
        frame = writeAndReceive(bus, store, service, {{310205, 61}}, "P4");
        check(frame.payload[0] == 0 && frame.payload[1] == 61, "pulse back to idle after the merged frame");
    }
    MemoryPointStore::cleanupOrphanedSegment(name);
}

// Round 2, item 2: the frame mirrors survive a restart; a changed definition starts from the template.
void frameStatePersistence(const std::string& directory) {
    Bus bus;
    const auto name = "can_runtime_state_" + std::to_string(getpid());
    MemoryPointStore::cleanupOrphanedSegment(name);
    auto config = makeConfig(bus, name, directory);
    const auto& stateFile = config.protocol.can.frameStateFile;
    std::remove(stateFile.c_str());
    {
        MemoryPointStore store(config.memoryStore);
        {
            CanDriverService service(config, store);
            writeAndReceive(bus, store, service, {{310201, 1}}, "S1");      // run_cmd = 1
            writeAndReceive(bus, store, service, {{310204, 1}}, "S2");      // reset pulse
        }
        std::ifstream saved(stateFile.c_str());
        check(saved.good(), "frame state written after a successful send");
        {
            CanDriverService restarted(config, store);
            auto frame = writeAndReceive(bus, store, restarted, {{310202, 10}}, "S3");
            check(frame.payload == Bytes({0x01, 10, 0, 0, 0, 0, 0, 0xAA}),
                  "after a restart a setpoint write keeps run_cmd=1 (not the template's 0)");
            frame = writeAndReceive(bus, store, restarted, {{310205, 5}}, "S4");
            check(frame.payload[0] == 0 && frame.payload[1] == 5, "a pulse is never restored active");
        }
        // The layout of frame 0x321 changes (power_sp scale): its saved state no longer applies.
        auto changed = config;
        changed.meters[0].points[7].write.scale = 0.5;
        {
            CanDriverService restarted(changed, store);
            auto frame = writeAndReceive(bus, store, restarted, {{310202, 10}}, "S5");
            check(frame.payload == Bytes({0x00, 20, 0, 0, 0, 0, 0, 0xAA}),
                  "a changed frame definition discards the saved state and starts from the template");
        }
        // Missing and corrupt files: start from the template, no failure.
        std::remove(stateFile.c_str());
        {
            CanDriverService restarted(config, store);
            auto frame = writeAndReceive(bus, store, restarted, {{310202, 11}}, "S6");
            check(frame.payload == Bytes({0x00, 11, 0, 0, 0, 0, 0, 0xAA}), "no state file: template");
        }
        {
            std::ofstream corrupt(stateFile.c_str(), std::ios::trunc);
            corrupt << "{\"version\":1,\"interfaceName\":\"can0\",\"frames\":[{\"frameId\":";
        }
        {
            CanDriverService restarted(config, store);
            auto frame = writeAndReceive(bus, store, restarted, {{310202, 12}}, "S7");
            check(frame.payload == Bytes({0x00, 12, 0, 0, 0, 0, 0, 0xAA}), "corrupt state file: template");
        }
        // A state that cannot be saved is removed, so a restart cannot apply an outdated one.
        auto unsaved = config;
        unsaved.protocol.can.frameStateFile = directory + "/missing-dir/state.json";
        {
            CanDriverService service(unsaved, store);
            auto frame = writeAndReceive(bus, store, service, {{310201, 1}}, "S8");
            check(frame.payload[0] == 1 && store.getWritebackResult("S80")->success,
                  "a failed state save does not fail the write");
        }
    }
    MemoryPointStore::cleanupOrphanedSegment(name);
}

}  // namespace

int main() {
    char pattern[] = "/tmp/can_runtime_test_XXXXXX";
    const char* directory = ::mkdtemp(pattern);
    if (directory == nullptr) {
        std::cerr << "mkdtemp failed" << std::endl;
        return 1;
    }
    try {
        receivePathAndWrites(directory);
        typoFrameIdFailsAtStartup(directory);
        receptionIgnoresPriorityLease(directory);
        pulseSignals(directory);
        frameStatePersistence(directory);
    } catch (const std::exception& ex) {
        std::cerr << "can_driver_runtime_test exception: " << ex.what() << std::endl;
        ++failures;
    }
    if (std::system((std::string("rm -rf ") + directory).c_str()) != 0) {
        std::cerr << "cleanup of " << directory << " failed" << std::endl;
    }
    if (failures != 0) {
        std::cerr << failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "can_driver_runtime_test passed" << std::endl;
    return 0;
}
