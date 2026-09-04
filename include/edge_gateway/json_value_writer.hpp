#pragma once

#include <cmath>
#include <ostream>

namespace edge_gateway {

inline void appendJsonNumber(std::ostream& out, double value) {
    if (std::isfinite(value)) {
        out << value;
    } else {
        out << "null";
    }
}

}  // namespace edge_gateway
