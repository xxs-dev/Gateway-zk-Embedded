#pragma once

#include <functional>
#include <stdexcept>
#include "edge_gateway/models.hpp"
#include "edge_gateway/compat.hpp"

namespace edge_gateway {

constexpr std::size_t kControlCommandIdMaxBytes = 63;
inline void validateControlCommandId(const std::string& id) {
    if (id.empty() || id.size() > kControlCommandIdMaxBytes || id.find('\0') != std::string::npos ||
        id.find_first_not_of(" \t\r\n") == std::string::npos)
        throw std::invalid_argument("durable cmdId must contain 1..63 stable non-NUL bytes");
}

class ControlDedupStore {
public:
    struct Claim {
        bool owner = false;
        WritebackResultRecord result;
    };
    explicit ControlDedupStore(std::string path, std::size_t capacity = 100000);
    Optional<WritebackResultRecord> bind(const std::string& machine, const std::string& meter,
                                       const PendingWriteCommand& command);
    Claim claim(const std::string& machine, const std::string& meter,
                const PendingWriteCommand& command, std::int64_t now);
    void finish(const std::string& machine, const std::string& meter,
                const WritebackResultRecord& result);
    Optional<WritebackResultRecord> result(const std::string& machine, const std::string& meter,
                                         const std::string& id) const;
    WritebackResultRecord dispatch(const std::string& machine, const std::string& meter,
                                  const PendingWriteCommand& command, std::int64_t now,
                                  const std::function<WritebackResultRecord()>& execute);
private:
    std::string path_;
    std::size_t capacity_;
};

} // namespace edge_gateway
