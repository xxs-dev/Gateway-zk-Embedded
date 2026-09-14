#include "control_dedup_test_support.hpp"
#include "edge_gateway/gateway_daemon.hpp"
#include "edge_gateway/point_store_router.hpp"
#include "edge_gateway/dlt645_command_executor.hpp"
#include "edge_gateway/dio_command_executor.hpp"
#include "edge_gateway/iec_command_executor.hpp"
#include <algorithm>
#include <iostream>
using namespace edge_gateway;
using dedup_test::require;

class NoCollector : public ICollector {
public:
    CollectCycleResult collectOnce(std::int64_t, bool) override { return {}; }
    void publishDeviceOnlineStatus(bool, std::int64_t) const override {}
};
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
    CommandResult writeByPoint(const PointDefinition&, double, const std::string& id,
                               const std::string&, const std::string&, std::int64_t) override {
        ++writes; CommandResult result; result.cmdId = id; result.success = true; return result;
    }
};
class DltPort : public ISerialPort {
public:
    int writes = 0; bool opened = false; std::vector<std::uint8_t> response;
    void open() override { opened = true; }
    void close() override { opened = false; }
    bool isOpen() const override { return opened; }
    void write(const std::vector<std::uint8_t>& bytes) override {
        ++writes;
        const auto start = std::find(bytes.begin(), bytes.end(), 0x68);
        require(start != bytes.end() && std::distance(start, bytes.end()) >= 12, "DLT frame");
        response = {0x68}; response.insert(response.end(), start + 1, start + 7);
        response.insert(response.end(), {0x68, 0x94, 0});
        unsigned sum = 0; for (auto byte : response) sum += byte;
        response.push_back(static_cast<std::uint8_t>(sum)); response.push_back(0x16);
    }
    std::vector<std::uint8_t> read(std::size_t size, int) override {
        size = std::min(size, response.size());
        std::vector<std::uint8_t> bytes(response.begin(), response.begin() + size);
        response.erase(response.begin(), response.begin() + size); return bytes;
    }
};

void test(const std::string& protocol) {
    dedup_test::Directory dir;
    DeviceConfig config; config.machineCode = "GW"; config.meterCode = "METER";
    config.memoryStore.sharedMemoryName = dir.path.filename().string();
    config.memoryStore.sqlitePath.clear(); config.memoryStore.controlDedupPath = dir.file("ledger.db");
    config.mqttDriver.priorityControlLeaseFile = dir.file("priority.json");
    config.mqttDriver.powerControlOwnershipFile = dir.file("owner.json");
    config.protocol.type = protocol; config.address = "000000000020";
    config.protocol.dlt645.write.enabled = true; config.protocol.dlt645.write.password = "02000000";
    PointDefinition point; point.index = 1001; point.pointCode = "control"; point.enabled = true;
    point.write.enable = true; point.write.dataType = "digital_output";
    point.read.gpio = 17;
    point.write.dlt645.di = "06010101"; point.write.dlt645.dataType = "dlt645_scheduled_control";
    point.write.dlt645.byteCount = 2; point.write.dlt645.unit = 2; config.points.push_back(point);
    auto gpio = std::make_shared<Gpio>(); auto iec = std::make_shared<IecPeer>();
    auto serial = std::make_shared<DltPort>(); SerialPortOptions options; options.device = "fake";
    auto dlt = std::make_shared<Dlt645Client>(serial, options);
    {
        MemoryPointStore store(config.memoryStore);
        PointStoreRouter router; router.addStore(config.memoryStore.sharedMemoryName, store);
        router.addRoutesFromDeviceConfigs({config}, config.memoryStore.sharedMemoryName);
        const auto construct = [&] {
            return std::make_unique<GatewayDaemon>(config, store,
                [](const DeviceConfig&, MemoryPointStore&) { return std::make_unique<NoCollector>(); },
                [&](const DeviceConfig& c, MemoryPointStore& s) -> std::unique_ptr<ICommandExecutor> {
                    if (protocol == "local_dio") return std::make_unique<DioCommandExecutor>(c, s, gpio);
                    if (protocol == "dlt645_2007") return std::make_unique<Dlt645CommandExecutor>(c, s, dlt);
                    return std::make_unique<IecCommandExecutor>(c, s, iec);
                }, GatewayDaemon::ServiceStartStop{});
        };
        auto gateway = construct();
        PendingWriteCommand c{"protocol-id", 1001, 1, "mqtt", 100, 100}; c.durableControl = true;
        require(router.submitWriteCommand(c).accepted && router.submitWriteCommand(c).accepted, "protocol ingress");
        gateway->processWritebackOnce(101);
        const auto result = store.getWritebackResult(c.cmdId);
        require(result && result->success, protocol + " actual executor failed: " + (result ? result->message : "missing"));
        gateway.reset(); gateway = construct(); store.submitWriteCommand(c); gateway->processWritebackOnce(102);
        require(gpio->writes + serial->writes + iec->writes == 1, protocol + " duplicate/restart hardware count");
    }
    MemoryPointStore::cleanupOrphanedSegment(config.memoryStore.sharedMemoryName);
    std::cout << protocol << ": real Gateway + real executor + fake hardware count=1 passed\n";
}
int main() {
    try { for (const auto* protocol : {"local_dio", "dlt645_2007", "iec104"}) test(protocol); return 0; }
    catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}
