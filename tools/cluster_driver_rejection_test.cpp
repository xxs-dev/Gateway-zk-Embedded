#include "control_dedup_test_support.hpp"
#include "edge_gateway/gateway_daemon.hpp"
#include "edge_gateway/can_driver_service.hpp"
#include "edge_gateway/dio_command_executor.hpp"
#include "edge_gateway/iec_command_executor.hpp"
#include "edge_gateway/dlt645_command_executor.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <algorithm>
#include <iostream>
using namespace edge_gateway;
using dedup_test::require;
namespace {
class Gpio : public IGpioPort {
public:
    int writes = 0;
    void exportGpio(int) override {}
    void setDirection(int, const std::string&) override {}
    bool readValue(int) override { return true; }
    void writeValue(int, bool) override { ++writes; }
};
class IecPeer : public IecClient {
public:
    int writes = 0;
    std::vector<IecDataValue> poll() override { return {}; }
    CommandResult writeByPoint(const PointDefinition&, double, const std::string&,
                               const std::string&, const std::string&, std::int64_t) override {
        ++writes; CommandResult r; r.success = true; return r;
    }
};
class SerialPeer : public ISerialPort {
public:
    int writes = 0;
    bool opened = false;
    std::vector<std::uint8_t> response;
    void open() override { opened = true; }
    void close() override { opened = false; }
    bool isOpen() const override { return opened; }
    void write(const std::vector<std::uint8_t>& bytes) override {
        ++writes;
        const auto start = std::find(bytes.begin(), bytes.end(), 0x68);
        require(start != bytes.end() && std::distance(start, bytes.end()) >= 12, "DLT frame");
        response = {0x68}; response.insert(response.end(), start + 1, start + 7);
        response.insert(response.end(), {0x68, 0x94, 0});
        unsigned sum = 0; for (auto b : response) sum += b;
        response.push_back(static_cast<std::uint8_t>(sum)); response.push_back(0x16);
    }
    std::vector<std::uint8_t> read(std::size_t n, int) override {
        n = std::min(n, response.size());
        std::vector<std::uint8_t> result(response.begin(), response.begin() + n);
        response.erase(response.begin(), response.begin() + n); return result;
    }
};
struct Fixture {
    dedup_test::Directory dir;
    DeviceConfig config;
    PendingWriteCommand command;
    std::unique_ptr<MemoryPointStore> store;
    Fixture(const std::string& protocol) {
        config.machineCode = "test"; config.meterCode = "meter"; config.address = "000000000020";
        config.protocol.type = protocol;
        config.memoryStore.sharedMemoryName = dir.path.filename().string();
        config.memoryStore.sqlitePath.clear();
        config.memoryStore.controlDedupPath = dir.file("dedup.db");
        config.mqttDriver.priorityControlLeaseFile = dir.file("priority.json");
        config.mqttDriver.powerControlOwnershipFile = dir.file("owner.json");
        config.emsCluster.enabled = config.emsCluster.controlEnabled = true;
        config.emsCluster.controlTargetIndexes = {1234};
        config.emsCluster.virtualSharedMemoryName = "distinct_authority";
        PointDefinition p; p.index = 1234; p.pointCode = "power";
        p.write.enable = true; p.write.dataType = "digital_output"; p.read.gpio = 17;
        p.write.verifyAfterWrite = false;
        p.write.dlt645.di = "06010101"; p.write.dlt645.dataType = "dlt645_scheduled_control";
        p.write.dlt645.byteCount = 2; p.write.dlt645.unit = 2;
        p.write.can.frameId = "0x123"; p.write.can.extended = false;
        p.write.can.dlc = 8; p.write.can.bitLength = 16;
        config.points = {p};
        config.protocol.dlt645.write.enabled = true; config.protocol.dlt645.write.password = "02000000";
        command.cmdId = "missing-context"; command.index = 1234; command.value = 1;
        command.source = "graph-ems";
        store.reset(new MemoryPointStore(config.memoryStore));
    }
    ~Fixture() { store.reset(); shm_unlink(("/" + config.memoryStore.sharedMemoryName).c_str()); }
};
void executorCase(const std::string& protocol) {
    Fixture f(protocol);
    auto gpio = std::make_shared<Gpio>();
    auto iec = std::make_shared<IecPeer>();
    auto serial = std::make_shared<SerialPeer>();
    auto dlt = std::make_shared<Dlt645Client>(serial, SerialPortOptions{});
    std::unique_ptr<ICommandExecutor> executor;
    if (protocol == "local_dio") executor.reset(new DioCommandExecutor(f.config, *f.store, gpio));
    else if (protocol == "dlt645_2007") executor.reset(new Dlt645CommandExecutor(f.config, *f.store, dlt));
    else executor.reset(new IecCommandExecutor(f.config, *f.store, iec));
    for (int tagged = 0; tagged < 2; ++tagged) {
        if (tagged) f.command.clusterAuthorization = ClusterWriteAuthorization{};
        bool rejected = false;
        try { rejected = !executor->executePending(f.command, 1).success; }
        catch (const std::exception&) { rejected = true; }
        require(rejected && gpio->writes + iec->writes + serial->writes == 0,
                protocol + " cluster context reached unsupported hardware");
    }
    f.command.clusterAuthorization = NullOpt; f.command.source = "mqtt-forwarder";
    require(executor->executePending(f.command, 1).success, protocol + " ordinary command rejected");
    require(gpio->writes + iec->writes + serial->writes == 1, protocol + " ordinary write missing");
    std::cout << protocol << " cluster=0 ordinary=1 passed\n";
}
void canCase() {
    Fixture f("can_socketcan");
    const int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    require(receiver >= 0, "UDP socket");
    struct Close { int fd; ~Close() { close(fd); } } closeReceiver{receiver};
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(receiver, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "UDP bind");
    socklen_t size = sizeof(addr); getsockname(receiver, reinterpret_cast<sockaddr*>(&addr), &size);
    f.config.protocol.can.transportMode = "udp_test"; f.config.protocol.can.manageInterface = false;
    f.config.protocol.can.udpBindAddress = "127.0.0.1";
    const int probe = socket(AF_INET, SOCK_DGRAM, 0);
    require(probe >= 0, "UDP probe");
    sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(probe, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0, "UDP probe bind");
    getsockname(probe, reinterpret_cast<sockaddr*>(&local), &size);
    f.config.protocol.can.udpListenPort = ntohs(local.sin_port);
    close(probe);
    f.config.protocol.can.udpPeerAddress = "127.0.0.1"; f.config.protocol.can.udpPeerPort = ntohs(addr.sin_port);
    f.config.points[0].write.dataType = "uint16";
    CanDriverService driver(f.config, *f.store);
    for (int durable = 0; durable < 2; ++durable) {
        f.command.durableControl = durable != 0;
        f.command.cmdId = durable ? "durable" : "plain";
        f.store->submitWriteCommand(f.command);
        driver.processWritebackOnce(1);
        char bytes[100];
        require(recv(receiver, bytes, sizeof(bytes), MSG_DONTWAIT) < 0, "cluster CAN emitted datagram");
        const auto receipt = f.store->getWritebackResult(f.command.cmdId);
        require(receipt && !receipt->success, "CAN cluster receipt");
    }
    f.command.source = "mqtt-forwarder"; f.command.cmdId = "ordinary";
    f.store->submitWriteCommand(f.command); driver.processWritebackOnce(1);
    char bytes[100];
    require(recv(receiver, bytes, sizeof(bytes), MSG_DONTWAIT) > 0, "ordinary CAN datagram missing");
    std::cout << "CAN cluster=0 ordinary=1 passed\n";
}
}
int main() {
    int failures = 0;
    for (const auto& protocol : {"local_dio", "iec104", "dlt645_2007", "can"}) {
        try { if (std::string(protocol) == "can") canCase(); else executorCase(protocol); }
        catch (const std::exception& e) { std::cerr << e.what() << '\n'; ++failures; }
    }
    return failures ? 1 : 0;
}
