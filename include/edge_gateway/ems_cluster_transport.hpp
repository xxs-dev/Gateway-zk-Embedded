#pragma once

#include <memory>
#include <string>
#include <vector>

#include "edge_gateway/ems_cluster.hpp"

namespace edge_gateway {

class IClusterTransport {
public:
    virtual ~IClusterTransport() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual std::vector<EmsClusterInbound> poll(int timeoutMs) = 0;
    virtual bool send(const EmsClusterOutbound& outbound) = 0;
};

std::unique_ptr<IClusterTransport> makeEthernetClusterTransport(
    EmsClusterConfig config,
    std::string nodeId
);

}  // namespace edge_gateway
