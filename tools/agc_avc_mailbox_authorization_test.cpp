// AGC/AVC dispatch mailbox authorization (round 5). Authorization comes from the ingress that received a
// request and the server-side agcAvc.commandMailbox policy; the payload "source" is audit only.
// Covers forged sources, unauthorized ingresses, missing identity, the normal authorized paths, EMS
// local mode, every other write path that could reach a mailbox index, and the consumer's origin check.
#include "control_dedup_test_support.hpp"
#include "edge_gateway/agc_avc_command_mailbox.hpp"
#include "edge_gateway/ems_control_mode.hpp"
#include "edge_gateway/mqtt_driver_service.hpp"
#include "edge_gateway/point_store_router.hpp"

#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "../system_monitor_direct_maintenance.cpp"

using namespace edge_gateway;
using dedup_test::require;

namespace {

constexpr std::uint32_t kTarget = 720010;
constexpr std::uint32_t kSequence = 720011;
constexpr std::uint32_t kUnlisted = 720012;  // a command point of the device that agcAvc does not reference
constexpr std::uint32_t kStatus = 720050;

class Publisher : public IMqttDriverPublisher {
public:
    std::vector<MqttIncomingMessage> incoming;
    std::vector<MqttCommandReply> replies;
    std::vector<std::string> status;
    void publishFullSnapshot(const std::string&, const std::vector<StoredPointValue>&, const std::string&) override {}
    void publishAlarm(const std::string&, std::uint32_t, const StoredPointValue&, const std::string&, bool) override {}
    void publishOnDemand(const std::string&, const std::vector<StoredPointValue>&, const std::string&) override {}
    void publishChangeEvent(const std::string&, const StoredPointValue&) override {}
    void publishCommandReply(const std::string&, const MqttCommandReply& reply) override { replies.push_back(reply); }
    void publishOtaReply(const std::string&, const OtaReply&) override {}
    void publishOtaStatus(const std::string&, const OtaStatus&) override {}
    void publishJsonMessage(const std::string&, const std::string& payload) override { status.push_back(payload); }
    std::vector<MqttIncomingMessage> pollIncoming(int) override {
        auto result = incoming;
        incoming.clear();
        return result;
    }
};

DeviceConfig mailboxDevice(const std::string& sharedMemoryName) {
    DeviceConfig config;
    config.machineCode = "GW";
    config.protocol.type = "agc_avc_virtual";
    config.memoryStore.sharedMemoryName = sharedMemoryName;
    config.memoryStore.maxLatestPoints = 16;
    config.memoryStore.sqlitePath.clear();
    LogicalDeviceConfig meter;
    meter.meterCode = "AGC_AVC_CORE";
    for (const auto index : {kTarget, kSequence, kUnlisted, kStatus}) {
        PointDefinition point;
        point.index = index;
        point.pointCode = "P" + std::to_string(index);
        point.category = index == kStatus ? "status" : "command";
        point.enabled = true;
        meter.points.push_back(point);
    }
    config.meters.push_back(meter);
    return config;
}

void policyMatrix() {
    AgcAvcCommandMailboxPolicy none;
    AgcAvcCommandMailboxPolicy platform;
    platform.platformDispatch = true;
    AgcAvcCommandMailboxPolicy maintenance;
    maintenance.maintenanceShadowTest = true;
    const auto platformIngress = LatestValueOrigin::PlatformDispatch;
    const auto shadowIngress = LatestValueOrigin::MaintenanceShadowTest;
    require(!agcAvcMailboxRefusal(true, true, false, none, platformIngress).empty(), "platform dispatch is off by default");
    require(agcAvcMailboxRefusal(true, true, false, platform, platformIngress).empty(), "platform dispatch in shadow mode");
    require(agcAvcMailboxRefusal(true, false, true, platform, platformIngress).empty(), "platform dispatch in submit mode");
    require(!agcAvcMailboxRefusal(false, true, false, platform, platformIngress).empty(), "disabled AGC/AVC refuses");
    require(!agcAvcMailboxRefusal(true, true, false, platform, shadowIngress).empty(),
            "the platform policy does not authorize maintenance injection");
    require(!agcAvcMailboxRefusal(true, true, false, none, shadowIngress).empty(), "maintenance injection is off by default");
    require(agcAvcMailboxRefusal(true, true, false, maintenance, shadowIngress).empty(), "maintenance shadow injection");
    require(!agcAvcMailboxRefusal(true, false, true, maintenance, shadowIngress).empty(),
            "maintenance injection never reaches an executable mailbox (submitWrites)");
    require(!agcAvcMailboxRefusal(true, true, true, maintenance, shadowIngress).empty(),
            "maintenance injection needs submitWrites=false even in shadow mode");
    require(!agcAvcMailboxRefusal(true, true, false, maintenance, platformIngress).empty(),
            "the maintenance policy does not authorize platform dispatch");
    require(!agcAvcMailboxRefusal(true, true, false, AgcAvcCommandMailboxPolicy{true, true}, LatestValueOrigin::Ordinary).empty(),
            "an ordinary writer is never a mailbox ingress");
    AgcAvcCommandMailboxRuntime unconfigured;
    require(!unconfigured.refusal(platformIngress).empty(), "an unconfigured mailbox refuses every ingress");
}

struct MqttFixture {
    dedup_test::Directory dir;
    DeviceConfig device;
    std::unique_ptr<MemoryPointStore> store;
    PointStoreRouter router;
    std::shared_ptr<Publisher> publisher = std::make_shared<Publisher>();
    std::unique_ptr<MqttDriverService> service;
    MqttDriverConfig driver;

    MqttFixture() : device(mailboxDevice("agc_mailbox_auth_" + std::to_string(getpid()))) {
        device.memoryStore.controlDedupPath = dir.file("control_dedup.db");
        MemoryPointStore::cleanupOrphanedSegment(device.memoryStore.sharedMemoryName);
        store.reset(new MemoryPointStore(device.memoryStore));
        router.addStore(device.memoryStore.sharedMemoryName, *store);
        router.addRoutesFromDeviceConfigs({device}, device.memoryStore.sharedMemoryName);
        MqttConfig mqtt;
        mqtt.enabled = true;
        mqtt.clientId = "GW";
        mqtt.topicMachineCode = "GW";
        mqtt.commandReplyTopic = "edge/command/reply";
        mqtt.statusTopic = "edge/status";
        driver.enabled = true;
        driver.sharedMemoryName = device.memoryStore.sharedMemoryName;
        driver.emsControlModeFile = dir.file("ems-mode.json");
        driver.priorityControlLeaseFile = dir.file("priority.json");
        driver.powerControlOwnershipFile = dir.file("owner.json");
        driver.fullUploadIntervalMs = 0;
        driver.allowRemoteHighPriority = true;  // the mailbox must refuse high priority on its own
        service.reset(new MqttDriverService(mqtt, driver, {device}, router, publisher));
    }
    ~MqttFixture() {
        service.reset();
        store.reset();
        MemoryPointStore::cleanupOrphanedSegment(device.memoryStore.sharedMemoryName);
    }
    void policy(bool platformDispatch, bool maintenanceShadowTest, bool shadowMode, bool submitWrites) {
        AgcAvcCommandMailboxRuntime runtime;
        runtime.configured = true;
        runtime.enabled = true;
        runtime.shadowMode = shadowMode;
        runtime.submitWrites = submitWrites;
        runtime.policy.platformDispatch = platformDispatch;
        runtime.policy.maintenanceShadowTest = maintenanceShadowTest;
        runtime.sequenceIndex = kSequence;
        runtime.commandIndexes = {kTarget, kSequence};
        service->setAgcAvcCommandMailboxRuntime(runtime);
    }
    const MqttCommandReply& send(const std::string& fields, std::int64_t now, bool retained = false) {
        MqttIncomingMessage message;
        message.type = MqttIncomingType::CommandRequest;
        message.payload = "{\"machineCode\":\"GW\"," + fields + "}";
        message.retained = retained;
        publisher->incoming.push_back(message);
        const auto before = publisher->replies.size();
        service->runScanOnce(now);
        require(publisher->replies.size() == before + 1, "every mailbox request gets exactly one reply: " + fields);
        return publisher->replies.back();
    }
    Optional<StoredPointValue> latest(std::uint32_t index) const { return store->getLatestByIndex(index, 1770000099000LL); }
};

void requireRefused(const MqttFixture& f, const MqttCommandReply& reply, const std::string& stage, const std::string& what) {
    require(!reply.success && reply.stage == stage && reply.outcome == "failed",
            what + ": got stage=" + reply.stage + " message=" + reply.message);
    require(!f.latest(kTarget), what + ": nothing may reach the mailbox");
}

void mqttIngress() {
    const std::int64_t now = 1770000090000LL;
    {
        MqttFixture f;
        f.policy(false, true, true, false);
        // Forged dispatch identity: the old substring check let any publisher in by naming itself.
        requireRefused(f, f.send(R"("cmdId":"F1","index":720010,"value":5,"source":"agc-avc-dispatch")", now),
                       "control-rejected", "forged agc-avc-dispatch source without platform policy");
        // Forged maintenance identity over MQTT: MQTT is the platform ingress, never maintenance.
        requireRefused(f, f.send(R"("cmdId":"F2","index":720010,"value":5,"source":"gateway-desktop-agc-avc-shadow-test")", now),
                       "control-rejected", "forged shadow-test source over MQTT");
        requireRefused(f, f.send(R"("cmdId":"F3","index":720010,"value":5,"source":"scada-local")", now),
                       "control-rejected", "forged local source over MQTT");
    }
    {
        MqttFixture f;
        f.policy(true, false, false, true);
        // Missing identity is fine: the ingress is the identity. Source stays audit text only.
        const auto& accepted = f.send(R"("cmdId":"OK1","index":720010,"value":7)", now);
        require(accepted.success && accepted.stage == "mailbox-committed", "authorized platform dispatch commits");
        auto value = f.latest(kTarget);
        require(value && value->value == 7.0 && value->origin == LatestValueOrigin::PlatformDispatch,
                "the commit is tagged platform-dispatch");
        f.send(R"("cmdId":"OK2","index":720010,"value":8,"source":"system-monitor-direct-maintenance")", now + 10);
        value = f.latest(kTarget);
        require(value && value->value == 8.0 && value->origin == LatestValueOrigin::PlatformDispatch,
                "a maintenance-looking source over MQTT is still platform dispatch");
        bool audited = false;
        for (const auto& payload : f.publisher->status) {
            audited = audited || (payload.find("\"event\":\"mailbox-committed\"") != std::string::npos &&
                                  payload.find("\"ingress\":\"platform-dispatch\"") != std::string::npos &&
                                  payload.find("system-monitor-direct-maintenance") != std::string::npos);
        }
        require(audited, "the payload source is kept as audit text with the ingress");
        // Escalation attempts on an authorized ingress.
        const auto& high = f.send(R"("cmdId":"HP","index":720010,"value":9,"highPriority":true)", now + 20);
        require(!high.success && high.stage == "control-rejected", "high priority is never granted through the mailbox");
        const auto& status = f.send(R"("cmdId":"ST","index":720050,"value":1)", now + 30);
        require(!status.success && !f.latest(kStatus), "a non-command point of the virtual device is not writable");
        const auto& unlisted = f.send(R"("cmdId":"UL","index":720012,"value":1)", now + 35);
        require(!unlisted.success && unlisted.stage == "control-rejected" && !f.latest(kUnlisted),
                "a command point that agcAvc does not reference is not a writable mailbox index");
        const auto& sequence = f.send(R"("cmdId":"SQ","index":720011,"value":1)", now + 40);
        require(sequence.success && f.latest(kSequence)->origin == LatestValueOrigin::PlatformDispatch,
                "every configured mailbox index is writable by the authorized ingress");
        const auto& retained = f.send(R"("cmdId":"RT","index":720010,"value":10)", now + 50, true);
        require(!retained.success && f.latest(kTarget)->value == 8.0, "retained commands never reach the mailbox");
        // EMS local mode: an otherwise authorized platform request must not reach the mailbox.
        EmsControlMode(f.driver.emsControlModeFile).setRemote(false, now + 60);
        const auto& local = f.send(R"("cmdId":"LM","index":720010,"value":11,"source":"local-display-scada")", now + 70);
        require(!local.success && local.stage == "control-rejected" && f.latest(kTarget)->value == 8.0,
                "EMS local mode refuses platform dispatch into the mailbox");
    }
    {
        MqttFixture f;
        AgcAvcCommandMailboxRuntime unconfigured;
        f.service->setAgcAvcCommandMailboxRuntime(unconfigured);
        requireRefused(f, f.send(R"("cmdId":"NC","index":720010,"value":5,"source":"agc-avc-dispatch")", now),
                       "control-rejected", "a missing agc-avc-service.json leaves the mailbox closed");
    }
}

// Every other path that could reach a mailbox index: device writes, plain latest updates, ordinary
// writers of the same slot.
void otherWritePaths() {
    dedup_test::Directory dir;
    auto device = mailboxDevice("agc_mailbox_paths_" + std::to_string(getpid()));
    device.memoryStore.controlDedupPath = dir.file("control_dedup.db");
    MemoryPointStore::cleanupOrphanedSegment(device.memoryStore.sharedMemoryName);
    {
        MemoryPointStore store(device.memoryStore);
        PointStoreRouter router;
        router.addStore(device.memoryStore.sharedMemoryName, store);
        router.addRoutesFromDeviceConfigs({device}, device.memoryStore.sharedMemoryName);
        PendingWriteCommand command;
        command.cmdId = "PATH";
        command.index = kTarget;
        command.value = 3;
        command.source = "agc-avc-dispatch";
        command.ts = 1770000090000LL;
        command.acceptedAt = command.ts;
        require(!router.submitWriteCommand(command).accepted, "device write path (SCADA/EMS/pointctl --app-config) refuses mailbox points");
        require(!router.submitWriteCommands({command}).accepted, "grouped device write path refuses mailbox points");
        require(!router.submitCommandMailbox(command, LatestValueOrigin::Ordinary).accepted, "mailbox commit needs an ingress");
        PointValue value;
        value.index = kTarget;
        value.value = 3;
        value.ts = command.ts;
        value.expireAt = command.ts + 60000;
        value.origin = LatestValueOrigin::PlatformDispatch;  // a caller cannot smuggle an origin through
        require(!router.putLatestByIndex(value).accepted, "latest update path (compute/graph/legacy EMS) refuses mailbox points");
        require(!router.putLatestByLocation(device.memoryStore.sharedMemoryName, value).accepted,
                "located latest update path refuses mailbox points");
        require(!store.getLatestByIndex(kTarget, command.ts), "no refused path wrote the mailbox");
        require(router.submitCommandMailbox(command, LatestValueOrigin::PlatformDispatch).accepted, "authorized commit");
        // A writer that bypasses the router (e.g. a collector misconfigured onto the same index) stores
        // an ordinary value: the tag never survives it, so the consumer ignores that value.
        PointValue overwrite;
        overwrite.index = kTarget;
        overwrite.machineCode = "GW";
        overwrite.meterCode = "AGC_AVC_CORE";
        overwrite.pointCode = "P720010";
        overwrite.value = 99;
        overwrite.ts = command.ts + 1;
        overwrite.expireAt = command.ts + 60000;
        store.putLatest(overwrite);
        const auto stored = store.getLatestByIndex(kTarget, command.ts + 2);
        require(stored && stored->value == 99 && stored->origin == LatestValueOrigin::Ordinary,
                "an ordinary write clears the mailbox origin");
        AgcAvcConfig consumer;
        consumer.enabled = true;
        consumer.commandMailbox.platformDispatch = true;
        require(!agcAvcMailboxAccepts(consumer, stored->origin), "the consumer refuses the overwritten value");
    }
    MemoryPointStore::cleanupOrphanedSegment(device.memoryStore.sharedMemoryName);
}

// Direct maintenance (authenticated batch control): shadow-test injection by policy and mode only.
void directMaintenance() {
    for (int scenario = 0; scenario < 4; ++scenario) {
        dedup_test::Directory dir;
        const auto shm = "agc_mailbox_direct_" + std::to_string(getpid()) + "_" + std::to_string(scenario);
        MemoryPointStore::cleanupOrphanedSegment(shm);
        edge_gateway::filesystem::create_directories(dir.path / "devices");
        const auto identityFile = dir.file("device_identity.json");
        const auto deviceFile = (dir.path / "devices" / "device_agc_avc_virtual.json").string();
        const auto appFile = dir.file("mqtt-service.json");
        const bool policyOn = scenario != 0;
        const bool submit = scenario == 2;
        {
            std::ofstream(identityFile.c_str()) << R"({"machineCode":"GW"})";
            std::ofstream(deviceFile.c_str())
                << R"({"protocol":{"type":"agc_avc_virtual"},"memoryStore":{"sharedMemoryName":")" << shm
                << R"(","maxLatestPoints":16,"sqlitePath":"","controlDedupPath":")" << dir.file("control_dedup.db")
                << R"("},"meters":[{"meterCode":"AGC_AVC_CORE","points":[)"
                << R"({"index":720010,"pointCode":"agc_dispatch_p","category":"command","enabled":true,"read":{"enable":false,"dataType":"float64"},"write":{"enable":false}},)"
                << R"({"index":720011,"pointCode":"agc_command_sequence","category":"command","enabled":true,"read":{"enable":false,"dataType":"uint64"},"write":{"enable":false}}]}]})";
            std::ofstream(dir.file("agc-avc-service.json").c_str())
                << R"({"runtimeMode":"agc_avc","identityConfigFile":")" << identityFile
                << R"(","deviceConfigFiles":[")" << deviceFile << R"("],"agcAvc":{"enabled":true,"shadowMode":)"
                << (submit ? "false" : "true") << R"(,"submitWrites":)" << (submit ? "true" : "false")
                << R"(,"commandMailbox":{"maintenanceShadowTest":)" << (policyOn ? "true" : "false")
                << R"(},"outputSharedMemoryName":")" << shm << R"(","sharedMemoryNames":[")" << shm
                << R"("],"agc":{"target":{"index":720010},"commandSequence":{"index":720011}}}})";
            std::ofstream(appFile.c_str())
                << R"({"identityConfigFile":")" << identityFile << R"(","deviceConfigFiles":[],"mqtt":{"clientId":"GW"},)"
                << R"("mqttDriver":{"sharedMemoryName":")" << shm << R"(","priorityControlLeaseFile":")" << dir.file("priority.json")
                << R"(","powerControlOwnershipFile":")" << dir.file("owner.json") << R"("},"cameraService":{"sharedMemoryName":""}})";
        }
        SystemMonitorDirectMaintenanceConfig config;
        config.appConfigFile = appFile;
        config.identityConfigFile = identityFile;
        config.scadaUpperComputerSafetyEnabled = false;
        // scenario 0: policy off; 1: policy on, shadow; 2: policy on, submitWrites; 3: high priority.
        const std::string source = scenario == 0 ? "gateway-desktop-agc-avc-shadow-test" : "bench-operator";
        const std::string body = std::string(R"({"machineCode":"GW","commands":[{"cmdId":"DM)") +
            std::to_string(scenario) + R"(","index":720010,"value":12,"source":")" + source + "\"" +
            (scenario == 3 ? R"(,"highPriority":true)" : "") + "}]}";
        const auto reply = batchControlJson(config, body);
        Optional<StoredPointValue> value;
        {
            MemoryPointStore store(shm, MemoryStoreOpenMode::CreateOrOpen);
            value = store.getLatestByIndex(kTarget, nowMs());
        }
        if (scenario == 1) {
            require(reply.find("\"stage\":\"mailbox-committed\"") != std::string::npos && value && value->value == 12 &&
                    value->origin == LatestValueOrigin::MaintenanceShadowTest,
                    "authorized maintenance shadow injection commits, tagged maintenance-shadow-test: " + reply);
        } else {
            require(reply.find("\"stage\":\"control-rejected\"") != std::string::npos &&
                    reply.find("\"success\":false") != std::string::npos && !value,
                    "direct maintenance scenario " + std::to_string(scenario) + " must be control-rejected: " + reply);
        }
        MemoryPointStore::cleanupOrphanedSegment(shm);
    }
}

}  // namespace

int main() {
    try {
        policyMatrix();
        mqttIngress();
        otherWritePaths();
        directMaintenance();
        std::cout << "agc_avc_mailbox_authorization_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "agc_avc_mailbox_authorization_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
