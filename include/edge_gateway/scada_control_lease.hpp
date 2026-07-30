#pragma once

#include <cstdint>
#include <string>

namespace edge_gateway {

class ScadaControlLease {
public:
    static bool isFresh(
        const std::string& projectDirectory,
        const std::string& leaseFile,
        const std::string& machineCode,
        std::int64_t nowMs,
        std::string* message = nullptr
    );
};

}  // namespace edge_gateway
