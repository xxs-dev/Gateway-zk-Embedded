#include "control_dedup_test_support.hpp"
#include "edge_gateway/control_dedup_store.hpp"
#include "edge_gateway/gateway_daemon.hpp"
#include "edge_gateway/can_driver_service.hpp"
#include "edge_gateway/mqtt_driver_service.hpp"
#include "edge_gateway/point_store_router.hpp"
#include "../src/memory_point_store_layout_v10.hpp"
#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <iostream>
#include <atomic>
#include "../system_monitor_direct_maintenance.cpp"

using namespace edge_gateway;
using dedup_test::require;

class Hardware : public IModbusClient {
public:
    std::atomic<int> writes{0};
    std::function<void()> afterWrite;
    std::vector<std::uint16_t> readCoils(int, int, int n) override { return std::vector<std::uint16_t>(n); }
    std::vector<std::uint16_t> readDiscreteInputs(int s, int a, int n) override { return readCoils(s,a,n); }
    std::vector<std::uint16_t> readHoldingRegisters(int s, int a, int n) override { return readCoils(s,a,n); }
    std::vector<std::uint16_t> readInputRegisters(int s, int a, int n) override { return readCoils(s,a,n); }
    void writeSingleCoil(int, int, bool) override { ++writes; if (afterWrite) afterWrite(); }
    void writeSingleRegister(int, int, std::uint16_t) override { ++writes; if (afterWrite) afterWrite(); }
    void writeMultipleRegisters(int, int, const std::vector<std::uint16_t>&) override { ++writes; if (afterWrite) afterWrite(); }
};
class Publisher : public IMqttDriverPublisher {
public:
    std::vector<MqttIncomingMessage> incoming;
    std::vector<MqttCommandReply> replies;
    void publishFullSnapshot(const std::string&, const std::vector<StoredPointValue>&, const std::string&) override {}
    void publishAlarm(const std::string&, std::uint32_t, const StoredPointValue&, const std::string&, bool) override {}
    void publishOnDemand(const std::string&, const std::vector<StoredPointValue>&, const std::string&) override {}
    void publishChangeEvent(const std::string&, const StoredPointValue&) override {}
    void publishCommandReply(const std::string&, const MqttCommandReply& r) override { replies.push_back(r); }
    void publishOtaReply(const std::string&, const OtaReply&) override {}
    void publishOtaStatus(const std::string&, const OtaStatus&) override {}
    void publishJsonMessage(const std::string&, const std::string&) override {}
    std::vector<MqttIncomingMessage> pollIncoming(int) override { auto result = incoming; incoming.clear(); return result; }
};
struct Fixture {
    dedup_test::Directory dir;
    DeviceConfig config;
    std::unique_ptr<MemoryPointStore> store;
    PointStoreRouter router;
    std::shared_ptr<Hardware> hardware = std::make_shared<Hardware>();
    std::unique_ptr<GatewayDaemon> gateway;
    Fixture() {
        config.machineCode = "GW"; config.meterCode = "METER";
        config.protocol.type = "modbus_rtu";
        config.protocol.slave = 1;
        config.memoryStore.sharedMemoryName = dir.path.filename().string();
        config.memoryStore.maxLatestPoints = 16;
        config.memoryStore.maxPendingWrites = 32;
        config.memoryStore.maxPersistentSamples = 4;
        config.memoryStore.sqlitePath.clear();
        config.memoryStore.controlDedupPath = dir.file("ledger.db");
        config.mqttDriver.priorityControlLeaseFile = dir.file("priority.json");
        config.mqttDriver.powerControlOwnershipFile = dir.file("owner.json");
        for (unsigned index : {1001U, 1002U}) {
            PointDefinition p; p.index = index; p.pointCode = "P" + std::to_string(index);
            p.enabled = true; p.read.enable = false; p.write.enable = true;
            p.write.function = 6; p.write.length = 1; p.write.address = index - 1001;
            p.write.dataType = "uint16"; config.points.push_back(p);
        }
        store = std::make_unique<MemoryPointStore>(config.memoryStore);
        router.addStore(config.memoryStore.sharedMemoryName, *store);
        router.addRoutesFromDeviceConfigs({config}, config.memoryStore.sharedMemoryName);
        restart();
    }
    ~Fixture() {
        gateway.reset(); store.reset();
        MemoryPointStore::cleanupOrphanedSegment(config.memoryStore.sharedMemoryName);
    }
    void restart() { gateway.reset(); gateway = std::make_unique<GatewayDaemon>(config, *store, hardware); }
    PendingWriteCommand command(const std::string& id) {
        PendingWriteCommand c{id, 1001, 12, "mqtt", 100, 100}; c.durableControl = true; return c;
    }
    WritebackResultRecord consume(const PendingWriteCommand& c) {
        gateway->processWritebackOnce(101);
        const auto r = store->getWritebackResult(c.cmdId, c.index);
        require(bool(r), "missing actual-consumer result"); return *r;
    }
};

void legacyStoreRefusalTest(const MemoryStoreConfig& config, int version) {
    using namespace edge_gateway::memory_layout_v10;
    const auto name = config.sharedMemoryName + "_v" + std::to_string(version);
    const auto path = "/" + name;
    const int fd = shm_open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    require(fd >= 0, "create exclusive legacy fixture");
    struct Cleanup {
        int fd;
        std::string path;
        ~Cleanup() { close(fd); shm_unlink(path.c_str()); }
    } cleanup{fd, path};

    // The ABI11 runtime cannot create legacy fixtures; use the frozen on-disk layout.
    auto original = std::make_unique<SharedStoreLayout>();
    original->header.magic = kSharedStoreMagic;
    original->header.version = version;
    pthread_mutexattr_t attr{};
    require(pthread_mutexattr_init(&attr) == 0, "legacy mutex attributes");
    require(pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0, "legacy shared mutex");
    require(pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0, "legacy robust mutex");
    require(pthread_mutex_init(&original->header.mutex, &attr) == 0, "legacy mutex initialization");
    pthread_mutexattr_destroy(&attr);
    original->header.latestCount = 2;
    original->header.persistentTail = 2;
    original->header.persistentSequence = 2;
    for (std::size_t i = 0; i < 2; ++i) {
        auto& latest = original->latest[i];
        latest.index = 1001 + i;
        latest.value = i == 0 ? 77 : 0;
        latest.ts = 100;
        latest.expireAt = 10000;
        latest.occupied = 1;
        auto& history = original->persistent[i];
        history.index = latest.index;
        history.value = latest.value;
        history.ts = latest.ts;
        history.sequence = i + 1;
        history.occupied = 1;
    }
    require(ftruncate(fd, sizeof(*original)) == 0, "legacy fixture size");
    require(pwrite(fd, original.get(), sizeof(*original), 0) == static_cast<ssize_t>(sizeof(*original)),
            "legacy fixture contents");
    struct stat before{};
    require(fstat(fd, &before) == 0, "legacy fixture identity");
    const auto preserved = [&] {
        const int named = shm_open(path.c_str(), O_RDONLY, 0);
        require(named >= 0, "runtime must preserve legacy segment name");
        struct stat afterStat{};
        auto after = std::make_unique<SharedStoreLayout>();
        const auto statResult = fstat(named, &afterStat);
        const auto bytes = pread(named, after.get(), sizeof(*after), 0);
        close(named);
        require(statResult == 0 && afterStat.st_dev == before.st_dev && afterStat.st_ino == before.st_ino &&
                afterStat.st_size == before.st_size && bytes == static_cast<ssize_t>(sizeof(*after)) &&
                std::memcmp(original.get(), after.get(), sizeof(*after)) == 0,
                "runtime refusal must preserve every legacy byte, including offline zero and history");
    };
    require(!MemoryPointStore::cleanupOrphanedSegment(name), "startup cleanup must preserve old ownerless segment");
    preserved();
    for (const auto mode : {MemoryStoreOpenMode::CreateOrOpen, MemoryStoreOpenMode::OpenExisting}) {
        bool rejected = false;
        try { MemoryPointStore old(name, mode); }
        catch (const std::exception& error) {
            rejected = std::string(error.what()).find("shared memory size mismatch") != std::string::npos;
        }
        require(rejected, "ABI11 runtime must refuse legacy attach before any command can enter its queue");
        preserved();
    }
    auto oldConfig = config;
    oldConfig.sharedMemoryName = name;
    oldConfig.sharedMemoryCreateVersion = version;
    bool rejected = false;
    try { MemoryPointStore old(oldConfig); }
    catch (const std::exception& error) {
        rejected = std::string(error.what()).find("runtime requires ABI 11") != std::string::npos;
    }
    require(rejected, "explicit legacy creation must remain forbidden");
    preserved();
}

void gatewayTest() {
    Fixture f;
    auto c = f.command("same");
    require(f.router.submitWriteCommand(c).accepted, "first submit");
    require(f.router.submitWriteCommand(c).accepted, "duplicate before consume");
    f.consume(c);
    require(f.hardware->writes == 1, "two queued duplicates must perform one actual Modbus write");
    f.restart();
    const auto replay = f.router.submitWriteCommand(c);
    require(replay.writeback && replay.writeback->success, "restart should replay original result");
    c.value++;
    require(!f.router.submitWriteCommand(c).accepted && f.hardware->writes == 1, "conflict must not expose stale success");
    c.value--; c.index++;
    require(!f.router.submitWriteCommand(c).accepted, "cross-index conflict");
    c.index--;
    auto other = *f.router.routeByIndex(c.index); other.index = 1003; other.meterCode = "OTHER";
    f.router.addRoute(other); c.index = 1003;
    require(!f.router.submitWriteCommand(c).accepted, "cross-meter conflict in gateway namespace");

    c = f.command("after-write-failure");
    require(f.router.submitWriteCommand(c).accepted, "fault command submit");
    f.hardware->afterWrite = [&] { dedup_test::Database db(f.config.memoryStore.controlDedupPath); db.failFinalize(); };
    const auto failure = f.consume(c);
    require(!failure.success && failure.stage == "writeback-timeout" && f.hardware->writes == 2,
            "actual Modbus write then SQLite finalize failure must be unknown");
    f.hardware->afterWrite = {};
    { dedup_test::Database db(f.config.memoryStore.controlDedupPath); db.sql("DROP TRIGGER fail_finalize;"); }
    f.restart();
    const auto pending = f.router.submitWriteCommand(c);
    require(pending.writeback && pending.writeback->stage == "writeback-timeout" && f.hardware->writes == 2,
            "restart cannot reexecute post-write unknown");

    c = f.command("reserve-crash");
    require(f.router.submitWriteCommand(c).accepted, "reserve crash submit");
    ControlDedupStore ledger(f.config.memoryStore.controlDedupPath);
    require(ledger.claim("GW", "METER", c, 101).owner, "reserve before simulated crash");
    f.restart();
    require(f.consume(c).stage == "writeback-timeout" && f.hardware->writes == 2, "reserved-before-write cannot execute");

    c = f.command("busy-consumer");
    require(f.router.submitWriteCommand(c).accepted, "busy submit");
    {
        dedup_test::Database db(f.config.memoryStore.controlDedupPath); db.sql("BEGIN IMMEDIATE;");
        require(!f.consume(c).success && f.hardware->writes == 2, "busy claim must not write hardware");
        require(!f.router.getDurableWritebackResult(*f.router.routeByIndex(c.index), c.cmdId, c.acceptedAt),
                "ready BUSY failure in SHM is not an authoritative operation receipt");
        db.sql("ROLLBACK;");
    }
    f.store->submitWriteCommand(c);
    f.hardware->afterWrite = [&] {
        require(!f.router.getDurableWritebackResult(*f.router.routeByIndex(c.index), c.cmdId, c.acceptedAt),
                "pending must hide prior same-acceptedAt busy failure while real write is in flight");
    };
    require(f.consume(c).success, "second queued owner can complete after BUSY");
    f.hardware->afterWrite = {};
    c = f.command("forged-missing"); f.store->submitWriteCommand(c);
    require(!f.consume(c).success && f.hardware->writes == 3, "flag without registration cannot downgrade");
    c = f.command("long-source"); c.source = "scada-windows:" + std::string(80, 's');
    require(f.router.submitWriteCommand(c).accepted && f.consume(c).success, "full source ingress to 31-byte SHM must execute");
    require(f.hardware->writes == 4, "long source actual write");
    c = f.command("non-standard");
    require(f.router.submitWriteCommand(c).accepted, "non-standard exception submit");
    f.hardware->afterWrite = [] { throw 42; };
    require(f.consume(c).stage == "writeback-timeout" && f.hardware->writes == 5, "non-std write exception unknown");
    f.hardware->afterWrite = {};

    const auto count = dedup_test::Database(f.config.memoryStore.controlDedupPath).count();
    f.config.memoryStore.controlDedupPath = f.dir.file("nonexistent/internal.db"); f.restart();
    c = f.command(""); c.durableControl = false;
    f.store->submitWriteCommand(c); f.gateway->processWritebackOnce(101);
    require(f.hardware->writes == 6 && !edge_gateway::filesystem::exists(f.config.memoryStore.controlDedupPath),
            "internal writes must not touch dedup DB");
    require(dedup_test::Database(f.dir.file("ledger.db")).count() == count, "internal must not consume capacity");
    for (int version : {8, 9, 10}) legacyStoreRefusalTest(f.config.memoryStore, version);
    auto otherConfig = f.config; otherConfig.memoryStore.controlDedupPath = f.dir.file("different.db");
    bool rejected = false;
    try { PointStoreRouter router; router.addRoutesFromDeviceConfigs({f.config, otherConfig}, "unused"); }
    catch (...) { rejected = true; }
    require(rejected, "router must reject split ledger paths before adding routes");
    std::cout << "Gateway actual Modbus writes: " << f.hardware->writes << "; duplicate/crash/busy/finalize/internal/ABI passed\n";
}

void mqttTest() {
    Fixture f;
    auto publisher = std::make_shared<Publisher>();
    MqttConfig mqtt; mqtt.commandRequestTopic = "command";
    auto driver = f.config.mqttDriver; driver.fullUploadIntervalMs = 0; driver.commandRateMaxPerWindow = 0;
    MqttDriverService service(mqtt, driver, {f.config}, f.router, publisher);
    const std::string payload = R"({"cmdId":"mqtt-id","machineCode":"GW","index":1001,"value":12,"source":"compute-engine","durableControl":false})";
    publisher->incoming.push_back({MqttIncomingType::CommandRequest, mqtt.commandRequestTopic, payload}); service.runScanOnce(100);
    require(f.store->peekPendingWriteCommands().size() == 1 && f.store->peekPendingWriteCommands()[0].durableControl,
            "MQTT source/flag spoof must not bypass trusted marking");
    f.gateway->processWritebackOnce(101); service.runScanOnce(102);
    require(!publisher->replies.empty() && publisher->replies.back().success, "MQTT actual write receipt");
    publisher->incoming.push_back({MqttIncomingType::CommandRequest, mqtt.commandRequestTopic, payload}); service.runScanOnce(103);
    require(publisher->replies.size() == 2 && publisher->replies.back().success && f.hardware->writes == 1,
            "MQTT TTL must not block persisted replay");
    auto changed = payload; changed.replace(changed.find("12"), 2, "13");
    publisher->incoming.push_back({MqttIncomingType::CommandRequest, mqtt.commandRequestTopic, changed}); service.runScanOnce(104);
    require(!publisher->replies.back().success && f.hardware->writes == 1, "MQTT conflict must not return old success");
    std::cout << "MQTT real ingress receipts/replay/spoof/conflict passed\n";
}

void canTest() {
    Fixture f; f.gateway.reset();
    const int receiver = socket(AF_INET, SOCK_DGRAM, 0); require(receiver >= 0, "UDP socket");
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(receiver, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "UDP bind");
    socklen_t length = sizeof(addr); getsockname(receiver, reinterpret_cast<sockaddr*>(&addr), &length);
    auto config = f.config; config.protocol.type = "can_socketcan";
    config.protocol.can.transportMode = "udp_test"; config.protocol.can.manageInterface = false;
    config.protocol.can.udpBindAddress = "127.0.0.1";
    const int probe = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in bindAddr{}; bindAddr.sin_family = AF_INET; bindAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(probe, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == 0, "CAN port probe");
    getsockname(probe, reinterpret_cast<sockaddr*>(&bindAddr), &length);
    config.protocol.can.udpListenPort = ntohs(bindAddr.sin_port); close(probe);
    config.protocol.can.udpPeerAddress = "127.0.0.1"; config.protocol.can.udpPeerPort = ntohs(addr.sin_port);
    for (auto& p : config.points) {
        p.write.can.frameId = "0x123"; p.write.can.extended = false;
        p.write.can.dlc = 8; p.write.can.bitLength = 16;
    }
    auto c = f.command("can-id");
    require(f.router.submitWriteCommand(c).accepted && f.router.submitWriteCommand(c).accepted, "CAN duplicate submit");
    {
        CanDriverService can(config, *f.store); can.processWritebackOnce(101);
        char bytes[100]; require(recv(receiver, bytes, sizeof(bytes), MSG_DONTWAIT) > 0, "one actual CAN UDP send");
        require(recv(receiver, bytes, sizeof(bytes), MSG_DONTWAIT) < 0, "no duplicate CAN frame");
    }
    {
        CanDriverService can(config, *f.store); f.store->submitWriteCommand(c); can.processWritebackOnce(102);
        char bytes[100]; require(recv(receiver, bytes, sizeof(bytes), MSG_DONTWAIT) < 0, "CAN restart cannot redispatch");
        c.cmdId = "can-finalize-failure"; require(f.router.submitWriteCommand(c).accepted, "CAN fault submit");
        dedup_test::Database db(config.memoryStore.controlDedupPath); db.failFinalize();
        can.processWritebackOnce(103);
        require(recv(receiver, bytes, sizeof(bytes), MSG_DONTWAIT) > 0, "CAN fault window actually sent");
        const auto r = f.store->getWritebackResult(c.cmdId);
        require(r && !r->success && r->stage == "writeback-timeout", "CAN finalize failure unknown");
        f.store->submitWriteCommand(c); can.processWritebackOnce(104);
        require(recv(receiver, bytes, sizeof(bytes), MSG_DONTWAIT) < 0, "CAN pending cannot resend");
    }
    close(receiver);
    std::cout << "CAN real UDP send count=2; replay/restart/finalize failure passed\n";
}

void directTest() {
    Fixture f;
    LogicalDeviceConfig firstMeter; firstMeter.meterCode = "METER"; firstMeter.slave = 1;
    firstMeter.points = f.config.points;
    LogicalDeviceConfig secondMeter; secondMeter.meterCode = "OTHER"; secondMeter.slave = 2;
    secondMeter.points = {f.config.points.front()}; secondMeter.points.front().index = 1003;
    secondMeter.points.front().pointCode = "P1003";
    f.config.meters = {firstMeter, secondMeter}; f.config.points.clear(); f.restart();
    const auto deviceFile = f.dir.file("device.json");
    const auto appFile = f.dir.file("app.json");
    const auto identityFile = f.dir.file("device_identity.json");
    {
        std::ofstream identity(identityFile); identity << R"({"machineCode":"GW"})";
        std::ofstream device(deviceFile);
        device << R"({"machineCode":"GW","meterCode":"METER","protocol":{"type":"modbus_rtu","slave":1},"memoryStore":{"sharedMemoryName":")"
               << f.config.memoryStore.sharedMemoryName << R"(","controlDedupPath":")" << f.config.memoryStore.controlDedupPath
               << R"("},"meters":[{"meterCode":"METER","slave":1,"points":[{"index":1001,"pointCode":"P1001","enabled":true,"write":{"enable":true,"function":6,"address":0,"length":1,"dataType":"uint16"}},)"
               << R"({"index":1002,"pointCode":"P1002","enabled":true,"write":{"enable":true,"function":6,"address":1,"length":1,"dataType":"uint16"}}]},)"
               << R"({"meterCode":"OTHER","slave":2,"points":[{"index":1003,"pointCode":"P1003","enabled":true,"write":{"enable":true,"function":6,"address":0,"length":1,"dataType":"uint16"}}]}]})";
        std::ofstream app(appFile);
        app << R"({"identityConfigFile":")" << identityFile << R"(","deviceConfigFiles":[")" << deviceFile
            << R"("],"mqtt":{"clientId":"GW"},"mqttDriver":{"sharedMemoryName":")" << f.config.memoryStore.sharedMemoryName
            << R"(","controlResultWaitTimeoutMs":1000,"priorityControlLeaseFile":")" << f.config.mqttDriver.priorityControlLeaseFile
            << R"(","powerControlOwnershipFile":")" << f.config.mqttDriver.powerControlOwnershipFile
            << R"("},"cameraService":{"sharedMemoryName":""}})";
    }
    SystemMonitorDirectMaintenanceConfig config;
    config.appConfigFile = appFile; config.identityConfigFile = identityFile;
    config.scadaUpperComputerSafetyEnabled = false;
    std::atomic<bool> running{true};
    std::thread worker([&] { while (running) { f.gateway->processWritebackOnce(nowMs()); std::this_thread::sleep_for(std::chrono::milliseconds(5)); } });
    try {
        const std::string body = R"({"machineCode":"GW","commands":[{"cmdId":"direct-1","index":1001,"value":1,"source":"compute-engine","durableControl":false},{"cmdId":"direct-2","index":1002,"value":2},{"cmdId":"direct-3","index":1003,"value":3}]})";
        const auto first = batchControlJson(config, body);
        require(first.find("writeback-succeeded") != std::string::npos && f.hardware->writes == 3,
                "Direct real batch to SHM to actual Modbus writes: " + first);
        const auto repeat = batchControlJson(config, body);
        require(repeat.find("writeback-succeeded") != std::string::npos && f.hardware->writes == 3, "Direct same/different-meter batch replay");
        const auto conflict = batchControlJson(config,
            R"({"machineCode":"GW","commands":[{"cmdId":"direct-1","index":1003,"value":1,"source":"compute-engine"}]})");
        require(conflict.find("conflicts") != std::string::npos && f.hardware->writes == 3, "Direct cross-meter conflict");
        bool emptyRejected = false;
        try { parseBatchControlCommands(R"({"commands":[{"cmdId":"","index":1001,"value":1}]})"); }
        catch (...) { emptyRejected = true; }
        require(emptyRejected, "explicit empty Direct ID rejected");
        const auto implicit = parseBatchControlCommands(R"({"commands":[{"index":1001,"value":1}]})");
        require(implicit.front().cmdId.size() <= 63 && !implicit.front().cmdId.empty(), "omitted ID yields fresh bounded ID");
    } catch (...) { running = false; worker.join(); throw; }
    running = false; worker.join();
    std::cout << "HTTP Direct actual batch/source-spoof/replay/conflict/ID compatibility passed\n";
}

int main() {
    try { gatewayTest(); mqttTest(); canTest(); directTest(); return 0; }
    catch (const std::exception& ex) { std::cerr << "control_dedup_integration_test: " << ex.what() << '\n'; return 1; }
}
