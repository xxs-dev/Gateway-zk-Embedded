#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace edge_gateway {
// Shared tmpfs remains writable with ProtectSystem=strict and BindPaths=/dev/shm.
constexpr const char* channelTraceDirectory = "/dev/shm/modbus-gateway-trace";
constexpr const char* channelTracePath = "/dev/shm/modbus-gateway-trace/channel-trace-v1.bin";
struct TraceChunk { std::uint64_t seq; std::int64_t ts; bool tx; std::uint32_t originalLength; std::vector<std::uint8_t> bytes; };
struct TracePage { bool active; std::uint64_t cursor; std::uint64_t overwritten; std::int64_t expiresAt; std::string channel; std::vector<TraceChunk> chunks; };
// One bounded capture per edge. A local, same-UID, tmpfs ring; never on a collection socket.
class ChannelTrace {
public:
    explicit ChannelTrace(std::string path = channelTracePath);
    void start(const std::string& channel, const std::string& session, int durationSec);
    TracePage read(const std::string& session, std::uint64_t after);
    void stop(const std::string& session);
    void capture(const std::string& channel, bool tx, const std::uint8_t* bytes, std::size_t size) noexcept;
private:
    std::string path_;
};
void setTraceChannel(const std::string& configPath);
std::string traceChannelKey(const std::string& configPath);
void traceBytes(bool tx, const std::uint8_t* bytes, std::size_t size) noexcept;
}
