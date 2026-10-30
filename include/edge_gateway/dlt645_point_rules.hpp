#pragma once

#include <string>
#include <vector>

#include "edge_gateway/dlt645_version.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {

// DL/T 645 point rules shared by the loader, the codec and the executor
// (implemented in src/validation/dlt645_validation.cpp).

// read.dataType values the codec decodes; anything else is rejected at load time.
bool isDlt645ReadDataType(const std::string& dataType);
// write.dlt645.dataType values the codec encodes for this edition (dlt645_scheduled_control and
// dlt645_remote_control are DL/T 645-2007 only).
bool isDlt645WriteDataType(const std::string& dataType, Dlt645Version version);
// The effective write encoding of a point (write.dlt645.dataType, else write.dataType).
std::string dlt645WriteDataType(const WriteSpec& spec);
// Trip/close commands (dlt645_scheduled_control; dlt645_remote_control trip/close): they switch a
// breaker or relay and must be confirmed by reading the position back.
bool isDlt645SwitchingControl(const WriteSpec& spec);
// A data identifier addressing a data block: 2007 with an FF byte (0201FF00 = Ua,Ub,Uc), 1997 with a
// trailing F (901F, B61F).
bool isDlt645BlockDataId(const std::string& dataIdHex);

// Checks one meter's point list. Throws std::invalid_argument for definitions that cannot work or
// would decode wrong values; owner names the meter or template in messages. A template may leave
// the read-back target of trip/close points open (it comes from meters[].positionReadBackIndex).
void validateDlt645PointList(const std::vector<PointDefinition>& points, const std::string& owner,
                             Dlt645Version version, bool templateList = false);

// The effective points of config.meters[meterIndex] of a DL/T 645 device config: its own points,
// else the standard template with auto indexes dlt645.autoIndexBase + meterIndex*10000 + position
// (the template must match protocol.type); trip/close points without a read-back target get
// meters[].positionReadBackIndex. The single expansion used by the driver, the point store
// router, the MQTT service and the local displays.
std::vector<PointDefinition> expandDlt645MeterPoints(
    const DeviceConfig& config,
    std::size_t meterIndex,
    const std::vector<PointDefinition>& standardPoints
);

}  // namespace edge_gateway
