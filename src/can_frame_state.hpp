#pragma once

// CAN requested-state journal (protocol.can.frameStateFile), internal to the CAN driver.
//
// The file records, per transmit frame, the payload the gateway last *requested* on the bus (the
// last frame it queued), not the device state. Pulse signals are always stored idle. A frame whose
// send is in flight is marked pending first (write-ahead): after a crash or power loss a pending
// frame is untrusted because nobody knows whether it went out.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace edge_gateway {
namespace can_frame_state {

constexpr int kFormat = 2;

struct Entry {
    std::uint32_t frameId = 0;
    bool extended = false;
    bool fd = false;
    bool remoteRequest = false;
    int dlc = 0;
    // Fingerprint of the frame definition (dlc, template, every write signal's layout and scaling):
    // a state saved for another definition is never applied.
    std::string layout;
    // A send of this frame was in flight when the file was written.
    bool pending = false;
    // Last committed requested payload; empty when the gateway holds no trusted requested state.
    std::vector<std::uint8_t> requested;
};

std::string layoutHash(const std::string& signature);
// Whole file including the SHA-256 of its content (format, interface and every entry).
std::string serialize(const std::string& interfaceName, const std::vector<Entry>& entries);
// Throws std::runtime_error on malformed content, an unknown format, a checksum mismatch or a
// file written for another interface.
std::vector<Entry> parse(const std::string& text, const std::string& interfaceName);
// Writes path via a temporary file, fsync, rename and a directory fsync: after a crash or power loss
// the file holds either the old or the new content.
void writeAtomically(const std::string& path, const std::string& content);
// Returns false when the file does not exist; throws when it exists but cannot be read.
bool readFile(const std::string& path, std::string& content);
// Test seam: when set, called with the content before every writeAtomically(); returning true makes
// that write fail as a disk error would.
void setWriteFailureForTest(std::function<bool(const std::string& content)> fail);

}  // namespace can_frame_state
}  // namespace edge_gateway
