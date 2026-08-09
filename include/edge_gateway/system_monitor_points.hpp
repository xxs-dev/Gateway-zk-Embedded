#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

class MemoryPointStore;
class PointStoreRouter;

namespace system_monitor_points {

constexpr char kSharedMemoryName[] = "gateway_point_store_system_monitor";
constexpr char kMeterCode[] = "SYSTEM_CELLULAR";

constexpr std::uint32_t kCellularEnabled = 920000001U;
constexpr std::uint32_t kCellularPresent = 920000002U;
constexpr std::uint32_t kCellularConnected = 920000003U;
constexpr std::uint32_t kCellularUsingRoute = 920000004U;
constexpr std::uint32_t kCellularSignalPercent = 920000005U;
constexpr std::uint32_t kCellularRxTotalMiB = 920000006U;
constexpr std::uint32_t kCellularTxTotalMiB = 920000007U;
constexpr std::uint32_t kCellularRxRateKiBps = 920000008U;
constexpr std::uint32_t kCellularTxRateKiBps = 920000009U;

const std::vector<PointDefinition>& definitions();
void registerStorePoints(MemoryPointStore& store, const std::string& machineCode);
void addRoutes(
    PointStoreRouter& router,
    const std::string& machineCode,
    const std::string& sharedMemoryName = kSharedMemoryName
);

}  // namespace system_monitor_points
}  // namespace edge_gateway
