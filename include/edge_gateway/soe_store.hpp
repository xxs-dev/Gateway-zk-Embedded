#pragma once
#include "edge_gateway/iec103_soe.hpp"
#include "edge_gateway/iec_codec.hpp"
#include <memory>

namespace edge_gateway {
// Dedicated device-event history; it is not the sampled-value alarm detector.
class SoeStore {
public:
    explicit SoeStore(const std::string& path, bool readOnly = false);
    ~SoeStore();
    void append(const std::vector<Iec103SoeEvent>& events);
    void recordDirectory(const DeviceConfig& config, const std::vector<Iec103DisturbanceRecord>& records,
                         bool complete, bool success, std::int64_t checkedMs);
    void recordDownload(const DeviceConfig& config, const Iec103DisturbanceRecord& record,
                        const Iec103ComtradeFiles& files, std::int64_t downloadedMs);
    std::vector<Iec103SoeEvent> read(const std::string& machine, std::int64_t beforeId = 0, int limit = 50);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace edge_gateway
