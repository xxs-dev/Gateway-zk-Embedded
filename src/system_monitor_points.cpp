#include "edge_gateway/system_monitor_points.hpp"

#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace edge_gateway {
namespace system_monitor_points {

namespace {

PointDefinition makePoint(
    std::uint32_t index,
    const std::string& pointCode,
    const std::string& name,
    const std::string& unit
) {
    PointDefinition point;
    point.index = index;
    point.pointCode = pointCode;
    point.name = name;
    point.category = "system_monitor";
    point.read.enable = true;
    point.read.dataType = "float64";
    point.read.unit = unit;
    point.read.cachePolicy.ttlMs = 30000;
    return point;
}

}  // namespace

const std::vector<PointDefinition>& definitions() {
    static const std::vector<PointDefinition> points = {
        makePoint(kCellularEnabled, "cellular_enabled", "Cellular monitoring enabled", ""),
        makePoint(kCellularPresent, "cellular_present", "Cellular modem present", ""),
        makePoint(kCellularConnected, "cellular_connected", "Cellular network connected", ""),
        makePoint(kCellularUsingRoute, "cellular_using_route", "Cellular route active", ""),
        makePoint(kCellularSignalPercent, "cellular_signal_percent", "Cellular signal", "%"),
        makePoint(kCellularRxTotalMiB, "cellular_rx_total_mib", "Cellular received since link start", "MiB"),
        makePoint(kCellularTxTotalMiB, "cellular_tx_total_mib", "Cellular transmitted since link start", "MiB"),
        makePoint(kCellularRxRateKiBps, "cellular_rx_rate_kibps", "Cellular receive rate", "KiB/s"),
        makePoint(kCellularTxRateKiBps, "cellular_tx_rate_kibps", "Cellular transmit rate", "KiB/s")
    };
    return points;
}

void registerStorePoints(MemoryPointStore& store, const std::string& machineCode) {
    store.registerPoints(machineCode, kMeterCode, definitions());
}

void addRoutes(
    PointStoreRouter& router,
    const std::string& machineCode,
    const std::string& sharedMemoryName
) {
    for (const auto& point : definitions()) {
        PointStoreRoute route;
        route.index = point.index;
        route.sourceIndex = point.index;
        route.machineCode = machineCode;
        route.meterCode = kMeterCode;
        route.pointCode = point.pointCode;
        route.protocolType = "system_monitor";
        route.interfaceCode = "system_monitor";
        route.interfaceType = "system";
        route.sharedMemoryName = sharedMemoryName;
        route.ttlMs = point.read.cachePolicy.ttlMs;
        router.addRoute(route);
    }
}

}  // namespace system_monitor_points
}  // namespace edge_gateway
