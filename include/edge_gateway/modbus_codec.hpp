#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

class ModbusCodec {
public:
    static DecodedValue decodeReadValue(
        const std::vector<std::uint16_t>& registers,
        const PointDefinition& point
    );

    static std::vector<std::uint16_t> encodeWriteValue(
        double businessValue,
        const PointDefinition& point
    );

    static WriteValidationResult validateWriteValue(
        double businessValue,
        const PointDefinition& point
    );

private:
    static std::vector<std::uint8_t> flattenRegisters(const std::vector<std::uint16_t>& registers);
    static std::vector<std::uint8_t> reorderBytes(
        const std::vector<std::uint8_t>& bytes,
        const std::string& byteOrder
    );
    static std::string bytesToHex(const std::vector<std::uint8_t>& bytes);
};

}  // namespace edge_gateway
