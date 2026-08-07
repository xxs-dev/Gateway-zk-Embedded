#pragma once

#include <string>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

struct EmsClusterInterfaceInfo {
    bool exists = false;
    bool physical = false;
    bool linkUp = false;
    std::string name;
    std::string macAddress;
    std::vector<std::string> ipv4Addresses;
};

struct EmsClusterNetworkCheck {
    bool ready = false;
    std::string selectedAddress;
    std::string candidateAddress;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

EmsClusterInterfaceInfo inspectEmsClusterInterface(const std::string& interfaceName);
std::string deriveEmsClusterLinkLocalAddress(
    const std::string& machineCode,
    const std::string& macAddress,
    unsigned int salt = 0
);
EmsClusterNetworkCheck checkEmsClusterNetwork(
    const EmsClusterConfig& config,
    const std::string& machineCode
);
EmsClusterNetworkCheck prepareEmsClusterNetwork(
    const EmsClusterConfig& config,
    const std::string& machineCode
);

}  // namespace edge_gateway
