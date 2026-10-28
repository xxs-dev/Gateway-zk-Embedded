#pragma once

// Persisted CAN transmit-frame mirrors (protocol.can.frameStateFile). Internal to the CAN driver.

#include <cstdint>
#include <string>
#include <vector>

namespace edge_gateway {
namespace can_frame_state {

struct Entry {
    std::uint32_t frameId = 0;
    bool extended = false;
    bool fd = false;
    bool remoteRequest = false;
    // Fingerprint of the frame definition (dlc, template, every write signal's layout and scaling):
    // a state saved for another definition is never applied.
    std::string layout;
    std::vector<std::uint8_t> payload;
};

std::string layoutHash(const std::string& signature);
std::string serialize(const std::string& interfaceName, const std::vector<Entry>& entries);
// Throws std::runtime_error on malformed content or a file written for another interface.
std::vector<Entry> parse(const std::string& text, const std::string& interfaceName);
// Writes path via a temporary file, fsync and rename, so a crash leaves the old or the new file.
void writeAtomically(const std::string& path, const std::string& content);
// Returns false when the file does not exist; throws when it exists but cannot be read.
bool readFile(const std::string& path, std::string& content);

}  // namespace can_frame_state
}  // namespace edge_gateway
