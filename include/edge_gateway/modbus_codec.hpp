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

    // Register payload width in bytes of a Modbus register data type (bit/bool and 16-bit types: 2,
    // 32-bit: 4, 64-bit: 8); 0 for types that are not Modbus register types.
    static int registerTypeByteWidth(const std::string& dataType);
    // Identity byte order for the type width ("AB", "ABCD", "ABCDEFGH"); empty for unknown types.
    static std::string canonicalByteOrder(const std::string& dataType);
    // True when byteOrder uses each of the first byteOrder.size() letters exactly once.
    static bool isByteOrderPermutation(const std::string& byteOrder);
    // Sets or clears bit 0..15 of a register as the device holds it. The bit index refers to the
    // register after byteOrder ("AB" or "BA"), the same view the read side decodes.
    static std::uint16_t setRegisterBit(std::uint16_t deviceRegister, int bit, bool value,
                                        const std::string& byteOrder);

private:
    static std::vector<std::uint8_t> flattenRegisters(const std::vector<std::uint16_t>& registers);
    static std::vector<std::uint8_t> reorderBytes(
        const std::vector<std::uint8_t>& bytes,
        const std::string& byteOrder
    );
    static std::string bytesToHex(const std::vector<std::uint8_t>& bytes);
};

}  // namespace edge_gateway
