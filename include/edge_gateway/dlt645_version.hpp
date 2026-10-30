#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

namespace edge_gateway {

// protocol.type selects the DL/T 645 edition: "dlt645_2007" or "dlt645_1997". Same frame
// (68 A0..A5 68 C L DATA(+33H) CS 16); 1997 uses 2-byte data identifiers and other control codes.
enum class Dlt645Version { V2007, V1997 };

inline bool isDlt645Protocol(const std::string& protocolType) {
    return protocolType == "dlt645_2007" || protocolType == "dlt645_1997";
}

inline Dlt645Version dlt645VersionOf(const std::string& protocolType) {
    if (protocolType == "dlt645_2007") return Dlt645Version::V2007;
    if (protocolType == "dlt645_1997") return Dlt645Version::V1997;
    throw std::invalid_argument("not a DL/T 645 protocol.type: " + protocolType);
}

inline const char* dlt645ProtocolName(Dlt645Version version) {
    return version == Dlt645Version::V1997 ? "dlt645_1997" : "dlt645_2007";
}

// Data identifier length in hex digits: DI3..DI0 (2007) or DI1 DI0 (1997).
inline std::size_t dlt645DataIdHexLength(Dlt645Version version) {
    return version == Dlt645Version::V1997 ? 4U : 8U;
}

}  // namespace edge_gateway
