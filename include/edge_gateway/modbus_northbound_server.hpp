#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {

// Problems that keep an enabled northbound server from starting (empty when it can start, or when
// northboundServer.enabled is false). Shared by load-time validation and ModbusNorthboundServer::start():
// a bad northbound definition disables only the northbound server; collection keeps running.
std::vector<std::string> northboundConfigProblems(const DeviceConfig& config);

// Client admission by northboundServer.allowedClientCidrs (empty list admits all). An IPv4 client of
// a dual-stack "::" listener (::ffff:a.b.c.d) is matched as IPv4.
bool northboundClientAllowed(const NorthboundServerConfig& server, const std::string& clientAddress);

class ModbusNorthboundServer {
public:
    // Receives northbound lifecycle events ("northbound-refused", "northbound-bind-failed",
    // "northbound-listening") with a human-readable message. Called from start() and from the
    // listener thread.
    using StatusSink = std::function<void(const std::string& event, const std::string& message)>;

    ModbusNorthboundServer(DeviceConfig config, MemoryPointStore& store, StatusSink statusSink = StatusSink());
    ~ModbusNorthboundServer();

    ModbusNorthboundServer(const ModbusNorthboundServer&) = delete;
    ModbusNorthboundServer& operator=(const ModbusNorthboundServer&) = delete;

    // Never throws for configuration problems: a refused start leaves startError() set.
    void start();
    void stop();
    // True while the listener thread runs (listening, or retrying a failed bind).
    bool isRunning() const;
    // Non-empty while the server is refused (config problem) or cannot bind; contains the remedy.
    std::string startError() const;
    int boundPort() const;

private:
    struct Mapping {
        PointDefinition point;
        ModbusNorthboundMappingConfig config;
        int span = 1;
        std::vector<std::uint16_t> invalidRegisters;
    };

    struct AddressKey {
        int unitId = 0;
        int function = 0;
        int address = 0;
    };

    struct AddressKeyHash {
        std::size_t operator()(const AddressKey& key) const;
    };

    struct AddressKeyEqual {
        bool operator()(const AddressKey& lhs, const AddressKey& rhs) const;
    };

    void buildMappings();
    void addPointMapping(const PointDefinition& point);
    void acceptLoop();
    void serveListener(std::intptr_t listenSocket);
    void handleClient(
        std::intptr_t clientSocket,
        std::string clientAddress,
        std::shared_ptr<std::atomic<bool>> done
    );
    void reapFinishedClientsLocked();
    int resolveUnit(std::uint8_t unitId) const;
    std::vector<std::uint8_t> handleRequest(
        std::uint8_t unitId,
        const std::vector<std::uint8_t>& pdu
    );
    std::vector<std::uint8_t> handleRegisterRead(
        int unitId,
        std::uint8_t function,
        int start,
        int count
    );
    std::vector<std::uint8_t> handleBitRead(
        int unitId,
        std::uint8_t function,
        int start,
        int count
    );
    // One point-store snapshot per call: every register of a multi-register value comes from the
    // same value. Invalid values (missing, bad quality, stale, not encodable) follow onInvalid.
    std::vector<std::uint16_t> readMapping(std::size_t slot, std::int64_t nowMs);
    void sendAll(std::intptr_t socket, const std::vector<std::uint8_t>& bytes) const;
    // Reads exactly size bytes before deadline; false on EOF, error, timeout or stop.
    bool readExact(
        std::intptr_t socket,
        std::vector<std::uint8_t>& bytes,
        std::size_t size,
        std::chrono::steady_clock::time_point deadline
    ) const;
    void setStartError(const std::string& message);
    void report(const std::string& event, const std::string& message) const;

    DeviceConfig config_;
    MemoryPointStore& store_;
    StatusSink statusSink_;
    std::vector<std::string> configProblems_;
    std::vector<Mapping> mappings_;
    std::unordered_map<AddressKey, std::size_t, AddressKeyHash, AddressKeyEqual> addressMappings_;
    std::vector<int> unitIds_;
    std::mutex holdMutex_;
    std::vector<std::vector<std::uint16_t>> lastGood_;
    std::atomic<bool> running_{false};
    std::atomic<int> activeClients_{0};
    mutable std::mutex clientsMutex_;
    struct ClientThread {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<ClientThread> clientThreads_;
    std::string startError_;
    std::thread acceptThread_;
    std::atomic<int> boundPort_{0};
};

}  // namespace edge_gateway
