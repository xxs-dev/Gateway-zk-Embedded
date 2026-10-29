#pragma once
#include "edge_gateway/iec103_soe.hpp"
#include <memory>

namespace edge_gateway {
// Dedicated device-event history; it is not the sampled-value alarm detector.
class SoeStore {
public:
    explicit SoeStore(const std::string& path, bool readOnly = false);
    ~SoeStore();
    void append(const std::vector<Iec103SoeEvent>& events);
    std::vector<Iec103SoeEvent> read(const std::string& machine, std::int64_t beforeId = 0, int limit = 50);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace edge_gateway
