// Mixed-version check tool for the shared pending-write queue (SHM v11).
//
// The same source is compiled against an old release tree and against the current tree, and the
// binaries are run in turn on one shared memory segment: old producer + new consumer, new producer +
// old consumer (rollback), commands left in the queue across an upgrade or a rollback. The consumer is
// the real GatewayDaemon writeback path with a recording executor (no device I/O).
// tools/shm_pending_write_mix_check.sh drives the scenarios; see doc/部署运维/共享待写队列升级规程.md.
//
// Build: the current tree defines PWMIX_STAMPED_QUEUE (CMake target shm_pending_write_mix); an old
// tree is built with: g++ -std=c++17 -O2 -I<old>/include -I<old>/src tools/shm_pending_write_mix.cpp
//   <old>/build/libedge_gateway.a -lpthread -ldl -lrt
//
// Commands (all print one line per fact, "key=value" pairs, for the check script):
//   produce <shm> <cmdId> <index> <wallMs> [stampAgoMs]   queue one write (stampAgoMs: current tree only)
//   consume <shm> <wallMs> <maxAgeMs> <ownIndex>[,...]     one GatewayDaemon writeback pass
//   result  <shm> <cmdId>                                   stage of the writeback result
//   pending <shm>                                           queued commands as this build decodes them
//   raw     <shm> [gap <u32>] [occupied <n>]                 slot bytes as stored (optionally overwrite)
//   version <shm>                                           segment header version
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edge_gateway/common/collector_base.hpp"
#include "edge_gateway/common/command_executor_interface.hpp"
#include "edge_gateway/gateway_daemon.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "memory_point_store_layout.hpp"

using namespace edge_gateway;

namespace {

#ifdef PWMIX_STAMPED_QUEUE
const char* const kBuild = "current";
#else
const char* const kBuild = "old";
#endif

MemoryStoreConfig storeConfig(const std::string& shm) {
    MemoryStoreConfig config;
    config.sharedMemoryName = shm;
    config.sqlitePath.clear();
    config.maxLatestPoints = 64;
    config.maxPendingWrites = 64;
    config.maxPersistentSamples = 8;
    return config;
}

std::vector<std::uint32_t> parseIndexes(const std::string& text) {
    std::vector<std::uint32_t> indexes;
    std::stringstream in(text);
    std::string item;
    while (std::getline(in, item, ',')) {
        if (!item.empty()) indexes.push_back(static_cast<std::uint32_t>(std::stoul(item)));
    }
    return indexes;
}

class IdleCollector : public ICollector {
public:
    CollectCycleResult collectOnce(std::int64_t, bool) override { return CollectCycleResult(); }
    void publishDeviceOnlineStatus(bool, std::int64_t) const override {}
};

// Records every command that reaches the physical-write step.
class RecordingExecutor : public ICommandExecutor {
public:
    CommandResult executePending(const PendingWriteCommand& command, std::int64_t nowMs,
                                 const BeforePhysicalWrite& beforeWrite) const override {
        if (beforeWrite) beforeWrite();
        std::cout << "EXEC cmdId=" << command.cmdId << " index=" << command.index << " value=" << command.value
                  << std::endl;
        CommandResult result;
        result.cmdId = command.cmdId;
        result.index = command.index;
        result.requestedValue = command.value;
        result.ts = nowMs;
        result.success = true;
        result.message = "recorded";
        return result;
    }
    CommandResult executeByIndex(const std::string& cmdId, std::uint32_t index, double value,
                                 std::int64_t nowMs) const override {
        PendingWriteCommand command;
        command.cmdId = cmdId;
        command.index = index;
        command.value = value;
        return executePending(command, nowMs, BeforePhysicalWrite());
    }
};

int produce(const std::vector<std::string>& args) {
    if (args.size() < 4) throw std::invalid_argument("produce <shm> <cmdId> <index> <wallMs> [stampAgoMs]");
    MemoryPointStore store(storeConfig(args[0]));
    PendingWriteCommand command;
    command.cmdId = args[1];
    command.index = static_cast<std::uint32_t>(std::stoul(args[2]));
    command.value = 7;
    command.source = "pwmix";
    command.ts = std::stoll(args[3]);
    command.acceptedAt = command.ts;
    if (args.size() > 4) {
#ifdef PWMIX_STAMPED_QUEUE
        const auto stamp = pendingWriteMonotonicNowMs() - static_cast<std::uint32_t>(std::stoll(args[4]));
        command.acceptedMonotonicMs = stamp == 0 ? 1u : stamp;
#else
        throw std::invalid_argument("stampAgoMs needs the current tree");
#endif
    }
    store.submitWriteCommand(command);
    std::cout << "QUEUED build=" << kBuild << " cmdId=" << command.cmdId << std::endl;
    return 0;
}

int consume(const std::vector<std::string>& args) {
    if (args.size() < 4) throw std::invalid_argument("consume <shm> <wallMs> <maxAgeMs> <ownIndex>[,...]");
    DeviceConfig config;
    config.machineCode = "GW";
    config.meterCode = "PWMIX";
    config.protocol.type = "modbus_rtu";
    config.protocol.slave = 1;
    config.memoryStore = storeConfig(args[0]);
    config.memoryStore.pendingWriteMaxAgeMs = std::stoll(args[2]);
    const std::string runDir = "/tmp/pwmix-" + args[0];
    (void)::mkdir(runDir.c_str(), 0700);
    config.memoryStore.controlDedupPath = runDir + "/control_dedup.db";
    config.memoryStore.emsControlModeFile = runDir + "/ems-control-mode.json";
    config.mqttDriver.priorityControlLeaseFile = runDir + "/priority.json";
    config.mqttDriver.powerControlOwnershipFile = runDir + "/owner.json";
    for (const auto index : parseIndexes(args[3])) {
        PointDefinition point;
        point.index = index;
        point.pointCode = "P" + std::to_string(index);
        point.name = point.pointCode;
        point.enabled = true;
        point.address = static_cast<int>(index);
        point.write.enable = true;
        point.write.function = 6;
        point.write.length = 1;
        point.write.dataType = "uint16";
        config.points.push_back(point);
    }
    MemoryPointStore store(config.memoryStore);
    // A driver drains only the indexes its collectors registered; the rest stay queued for the others.
    store.registerPoints(config.machineCode, config.meterCode, config.points);
    GatewayDaemon daemon(
        config, store,
        [](const DeviceConfig&, MemoryPointStore&) { return std::unique_ptr<ICollector>(new IdleCollector()); },
        [](const DeviceConfig&, MemoryPointStore&) { return std::unique_ptr<ICommandExecutor>(new RecordingExecutor()); },
        {});
    const auto processed = daemon.processWritebackOnce(std::stoll(args[1]));
    std::cout << "CONSUMED build=" << kBuild << " processed=" << processed << std::endl;
    return 0;
}

int result(const std::vector<std::string>& args) {
    if (args.size() < 2) throw std::invalid_argument("result <shm> <cmdId>");
    MemoryPointStore store(args[0], MemoryStoreOpenMode::OpenExisting);
    const auto record = store.getWritebackResult(args[1]);
    if (!record) {
        std::cout << "RESULT cmdId=" << args[1] << " stage=none" << std::endl;
        return 0;
    }
    std::cout << "RESULT cmdId=" << args[1] << " stage=" << record->stage << " success=" << (record->success ? 1 : 0)
              << std::endl;
    return 0;
}

int pending(const std::vector<std::string>& args) {
    if (args.empty()) throw std::invalid_argument("pending <shm>");
    MemoryPointStore store(args[0], MemoryStoreOpenMode::OpenExisting);
    for (const auto& command : store.peekPendingWriteCommands()) {
        std::cout << "PENDING build=" << kBuild << " cmdId=" << command.cmdId << " index=" << command.index;
#ifdef PWMIX_STAMPED_QUEUE
        std::cout << " stamp=" << command.acceptedMonotonicMs;
#endif
        std::cout << std::endl;
    }
    return 0;
}

struct Mapping {
    memory_layout::SharedStoreLayout* layout = nullptr;
    std::size_t size = 0;
    ~Mapping() { if (layout) ::munmap(layout, size); }
};

void mapSegment(const std::string& shm, bool writable, Mapping& mapping) {
    const int fd = ::shm_open(("/" + shm).c_str(), writable ? O_RDWR : O_RDONLY, 0);
    if (fd < 0) throw std::runtime_error("shm_open failed: " + std::string(std::strerror(errno)));
    struct stat st {};
    if (::fstat(fd, &st) != 0 || static_cast<std::size_t>(st.st_size) < sizeof(memory_layout::SharedStoreLayout)) {
        ::close(fd);
        throw std::runtime_error("segment is not a v11 point store");
    }
    void* base = ::mmap(nullptr, sizeof(memory_layout::SharedStoreLayout), writable ? PROT_READ | PROT_WRITE : PROT_READ,
                        MAP_SHARED, fd, 0);
    ::close(fd);
    if (base == MAP_FAILED) throw std::runtime_error("mmap failed");
    mapping.layout = static_cast<memory_layout::SharedStoreLayout*>(base);
    mapping.size = sizeof(memory_layout::SharedStoreLayout);
}

// Byte view of the pending ring, the same in every v11 build: the 4 bytes after index (offset 12) were
// alignment padding before the monotonic stamp; occupied is the byte at offset 136.
int raw(const std::vector<std::string>& args) {
    if (args.empty()) throw std::invalid_argument("raw <shm> [gap <u32>] [occupied <n>]");
    using Slot = memory_layout::SharedPendingWriteSlot;
    const std::size_t gapOffset = offsetof(Slot, index) + sizeof(std::uint32_t);
    static_assert(offsetof(Slot, value) == 16 && offsetof(Slot, index) == 8, "v11 pending slot layout");
    static_assert(offsetof(Slot, occupied) == 136, "v11 pending slot layout");
    bool setGap = false;
    bool setOccupied = false;
    std::uint32_t gap = 0;
    unsigned occupied = 0;
    for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
        if (args[i] == "gap") { setGap = true; gap = static_cast<std::uint32_t>(std::stoul(args[i + 1], nullptr, 0)); }
        if (args[i] == "occupied") { setOccupied = true; occupied = static_cast<unsigned>(std::stoul(args[i + 1])); }
    }
    Mapping mapping;
    mapSegment(args[0], setGap || setOccupied, mapping);
    auto& header = mapping.layout->header;
    for (auto head = header.pendingWriteHead; head != header.pendingWriteTail;
         head = (head + 1) % memory_layout::kMaxPendingWriteSlots) {
        auto* bytes = reinterpret_cast<unsigned char*>(&mapping.layout->pendingWrites[head]);
        if (setGap) std::memcpy(bytes + gapOffset, &gap, sizeof(gap));
        if (setOccupied) bytes[offsetof(Slot, occupied)] = static_cast<unsigned char>(occupied);
        std::uint32_t storedGap = 0;
        std::memcpy(&storedGap, bytes + gapOffset, sizeof(storedGap));
        char cmdId[memory_layout::kCmdIdSize + 1] = {};
        std::memcpy(cmdId, bytes + offsetof(Slot, cmdId), memory_layout::kCmdIdSize);
        std::cout << "RAW slot=" << head << " cmdId=" << cmdId << " occupied=" << unsigned(bytes[offsetof(Slot, occupied)])
                  << " gap=" << storedGap << std::endl;
    }
    return 0;
}

int version(const std::vector<std::string>& args) {
    if (args.empty()) throw std::invalid_argument("version <shm>");
    Mapping mapping;
    mapSegment(args[0], false, mapping);
    std::cout << "VERSION " << mapping.layout->header.version << std::endl;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: shm_pending_write_mix produce|consume|result|pending|raw|version ..." << std::endl;
        return 2;
    }
    const std::string command = argv[1];
    const std::vector<std::string> args(argv + 2, argv + argc);
    try {
        if (command == "produce") return produce(args);
        if (command == "consume") return consume(args);
        if (command == "result") return result(args);
        if (command == "pending") return pending(args);
        if (command == "raw") return raw(args);
        if (command == "version") return version(args);
#ifdef PWMIX_STAMPED_QUEUE
        if (command == "now") {
            std::cout << "MONOTONIC " << pendingWriteMonotonicNowMs() << std::endl;
            return 0;
        }
#endif
    } catch (const std::exception& error) {
        std::cout << "ERROR build=" << kBuild << " " << error.what() << std::endl;
        return 1;
    }
    std::cerr << "unknown command " << command << std::endl;
    return 2;
}
