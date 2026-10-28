#pragma once

#include <memory>
#include <string>
#include "edge_gateway/models.hpp"

namespace edge_gateway {

struct EmsControlModeState {
    bool remote = true;
    std::int64_t denyAcceptedThrough = 0;
};

// Transport origin is trusted metadata, never a user-supplied source string.
// A missing state means remote (factory default); unreadable/corrupt state fails closed.
class EmsControlMode {
public:
    explicit EmsControlMode(std::string path = "/opt/modbus-gateway/data/ems-control-mode.json");
    EmsControlModeState read() const;
    void admitPlatform(std::int64_t acceptedAt) const;
    void setRemote(bool remote, std::int64_t nowMs) const;
    // Retain this token until physical dispatch completes. Switching mode takes
    // the exclusive lock and cannot acknowledge while a platform write is in flight.
    std::shared_ptr<void> dispatchPermit(const PendingWriteCommand& command) const;
private:
    std::string path_;
};

} // namespace edge_gateway
