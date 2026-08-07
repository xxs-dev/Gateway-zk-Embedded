#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace edge_gateway {

struct VirtualCanFrame {
    std::uint32_t frameId = 0;
    bool extended = false;
    bool remoteRequest = false;
    bool fd = false;
    std::vector<std::uint8_t> payload;
};

constexpr std::size_t kVirtualCanHeaderSize = 12;
constexpr std::size_t kVirtualCanMaxPayloadSize = 64;

inline std::vector<std::uint8_t> encodeVirtualCanDatagram(const VirtualCanFrame& frame) {
    if (frame.payload.size() > kVirtualCanMaxPayloadSize) {
        return {};
    }
    std::vector<std::uint8_t> bytes(kVirtualCanHeaderSize + frame.payload.size(), 0);
    bytes[0] = 'G';
    bytes[1] = 'C';
    bytes[2] = 'A';
    bytes[3] = 'N';
    bytes[4] = 1;
    bytes[5] = static_cast<std::uint8_t>(
        (frame.extended ? 0x01U : 0U) |
        (frame.remoteRequest ? 0x02U : 0U) |
        (frame.fd ? 0x04U : 0U)
    );
    bytes[6] = static_cast<std::uint8_t>(frame.payload.size());
    bytes[8] = static_cast<std::uint8_t>((frame.frameId >> 24U) & 0xFFU);
    bytes[9] = static_cast<std::uint8_t>((frame.frameId >> 16U) & 0xFFU);
    bytes[10] = static_cast<std::uint8_t>((frame.frameId >> 8U) & 0xFFU);
    bytes[11] = static_cast<std::uint8_t>(frame.frameId & 0xFFU);
    for (std::size_t i = 0; i < frame.payload.size(); ++i) {
        bytes[kVirtualCanHeaderSize + i] = frame.payload[i];
    }
    return bytes;
}

inline bool decodeVirtualCanDatagram(
    const std::uint8_t* bytes,
    std::size_t size,
    VirtualCanFrame& frame
) {
    if (bytes == nullptr || size < kVirtualCanHeaderSize ||
        bytes[0] != 'G' || bytes[1] != 'C' || bytes[2] != 'A' || bytes[3] != 'N' ||
        bytes[4] != 1) {
        return false;
    }
    const auto payloadSize = static_cast<std::size_t>(bytes[6]);
    if (payloadSize > kVirtualCanMaxPayloadSize || size != kVirtualCanHeaderSize + payloadSize) {
        return false;
    }
    frame.frameId =
        (static_cast<std::uint32_t>(bytes[8]) << 24U) |
        (static_cast<std::uint32_t>(bytes[9]) << 16U) |
        (static_cast<std::uint32_t>(bytes[10]) << 8U) |
        static_cast<std::uint32_t>(bytes[11]);
    frame.extended = (bytes[5] & 0x01U) != 0;
    frame.remoteRequest = (bytes[5] & 0x02U) != 0;
    frame.fd = (bytes[5] & 0x04U) != 0;
    frame.payload.assign(bytes + kVirtualCanHeaderSize, bytes + size);
    return true;
}

inline bool decodeVirtualCanDatagram(
    const std::vector<std::uint8_t>& bytes,
    VirtualCanFrame& frame
) {
    return decodeVirtualCanDatagram(bytes.data(), bytes.size(), frame);
}

}  // namespace edge_gateway
