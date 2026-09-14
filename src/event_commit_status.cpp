#include "edge_gateway/event_commit_sink.hpp"
#include <sstream>

namespace edge_gateway {
std::string eventCommitStatusJson(const EventCommitStatus& status) {
    const char* phase = "invalid";
    switch (status.phase) {
    case EventCommitPhase::Starting: phase = "STARTING"; break;
    case EventCommitPhase::Normal: phase = "NORMAL"; break;
    case EventCommitPhase::GapDrain: phase = "GAP_DRAIN"; break;
    case EventCommitPhase::Rebase: phase = "REBASE"; break;
    case EventCommitPhase::Fenced: phase = "FENCED"; break;
    case EventCommitPhase::Stopped: phase = "STOPPED"; break;
    }
    std::ostringstream out;
    out << "{\"schemaVersion\":1,\"phase\":\"" << phase << "\",\"queueItems\":" << status.queueItems
        << ",\"queueBytes\":" << status.queueBytes << ",\"acceptedBatches\":\"" << status.accepted
        << "\",\"committedBatches\":\"" << status.committed << "\",\"rejectedBatches\":\"" << status.rejected
        << "\",\"gapEpisodes\":\"" << status.gaps << "\",\"unknown\":" << (status.unknown ? "true" : "false")
        << ",\"rejectedInputSamples\":\"" << status.rejectedInputSamples
        << "\",\"pendingInputSamples\":\"" << status.pendingInputSamples << "\""
        << ",\"lastCommitMonotonicMs\":\"" << status.lastCommitMonotonicMs << "\",\"error\":\"";
    const char* hex = "0123456789abcdef";
    for (const unsigned char c : status.error) {
        if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
        else if (c < 32) out << "\\u00" << hex[c >> 4] << hex[c & 15];
        else out << static_cast<char>(c);
    }
    out << "\"}";
    return out.str();
}
} // namespace edge_gateway
