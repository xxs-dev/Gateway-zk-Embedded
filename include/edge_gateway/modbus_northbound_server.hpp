#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {

class ModbusNorthboundServer {
public:
    ModbusNorthboundServer(DeviceConfig config, MemoryPointStore& store);
    ~ModbusNorthboundServer();

    ModbusNorthboundServer(const ModbusNorthboundServer&) = delete;
    ModbusNorthboundServer& operator=(const ModbusNorthboundServer&) = delete;

    void start();
    void stop();
    bool isRunning() const;
    int boundPort() const;

private:
    struct Mapping {
        PointDefinition point;
        ModbusNorthboundMappingConfig config;
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
    void handleClient(std::intptr_t clientSocket, std::string clientAddress);
    std::vector<std::uint8_t> handleRequest(
        std::uint8_t unitId,
        const std::vector<std::uint8_t>& pdu
    );
    std::vector<std::uint8_t> handleRegisterRead(
        std::uint8_t unitId,
        std::uint8_t function,
        int start,
        int count
    );
    std::vector<std::uint8_t> handleBitRead(
        std::uint8_t unitId,
        std::uint8_t function,
        int start,
        int count
    );
    std::vector<std::uint16_t> encodeRegistersForMapping(const Mapping& mapping, std::int64_t nowMs) const;
    std::vector<std::uint16_t> zeroRegistersForMapping(const Mapping& mapping) const;
    std::uint16_t encodeBitForMapping(const Mapping& mapping, std::int64_t nowMs) const;
    void sendAll(std::intptr_t socket, const std::vector<std::uint8_t>& bytes) const;
    bool readExact(std::intptr_t socket, std::vector<std::uint8_t>& bytes, std::size_t size) const;
    bool isClientAllowed(const std::string& clientAddress) const;
    void closeListenSocket();

    DeviceConfig config_;
    MemoryPointStore& store_;
    std::unordered_map<AddressKey, Mapping, AddressKeyHash, AddressKeyEqual> addressMappings_;
    std::atomic<bool> running_{false};
    std::atomic<int> activeClients_{0};
    std::intptr_t listenSocket_ = -1;
    mutable std::mutex clientsMutex_;
    std::vector<std::thread> clientThreads_;
    std::thread acceptThread_;
    std::atomic<int> boundPort_{0};
};

}  // namespace edge_gateway
