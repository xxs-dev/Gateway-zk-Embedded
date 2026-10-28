#pragma once

#include <string>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

// DL/T 645 point rules shared by the loader, the codec and the executor
// (implemented in src/validation/dlt645_validation.cpp).

// read.dataType values the codec decodes; anything else is rejected at load time.
bool isDlt645ReadDataType(const std::string& dataType);
// write.dlt645.dataType values the codec encodes.
bool isDlt645WriteDataType(const std::string& dataType);
// The effective write encoding of a point (write.dlt645.dataType, else write.dataType).
std::string dlt645WriteDataType(const WriteSpec& spec);
// Trip/close style commands (dlt645_scheduled_control): they switch a breaker or relay and must
// be confirmed by reading the position back.
bool isDlt645SwitchingControl(const WriteSpec& spec);
// A data identifier with an FF byte addresses a data block (e.g. 0201FF00 = Ua,Ub,Uc).
bool isDlt645BlockDataId(const std::string& dataIdHex);
// PA P0 P1 P2 whose secret part P0..P2 is 000000 (factory default at any level, e.g. 00000000).
bool isDlt645DefaultPassword(const std::string& passwordHex);

// Checks one meter's point list. Throws std::invalid_argument for definitions that cannot work or
// would decode wrong values; owner names the meter or template in messages.
void validateDlt645PointList(const std::vector<PointDefinition>& points, const std::string& owner);

}  // namespace edge_gateway
