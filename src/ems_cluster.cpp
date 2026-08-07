#include "edge_gateway/ems_cluster.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#else
#include <direct.h>
#include <process.h>
#endif

namespace edge_gateway {

namespace {

constexpr std::uint32_t kProtocolMagic = 0x4b454331U;
constexpr std::uint8_t kProtocolVersion = 2;
constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kAuthTagSize = 32;
constexpr std::size_t kMaxFrameSize = 64 * 1024;

std::uint64_t fnv1a64(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto ch : text) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

class Sha256 {
public:
    Sha256() { reset(); }

    void update(const std::uint8_t* data, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) {
            buffer_[bufferSize_++] = data[i];
            bitLength_ += 8;
            if (bufferSize_ == 64) {
                transform();
                bufferSize_ = 0;
            }
        }
    }

    std::array<std::uint8_t, 32> finish() {
        const auto originalBitLength = bitLength_;
        buffer_[bufferSize_++] = 0x80;
        if (bufferSize_ > 56) {
            while (bufferSize_ < 64) buffer_[bufferSize_++] = 0;
            transform();
            bufferSize_ = 0;
        }
        while (bufferSize_ < 56) buffer_[bufferSize_++] = 0;
        for (int shift = 56; shift >= 0; shift -= 8) {
            buffer_[bufferSize_++] = static_cast<std::uint8_t>((originalBitLength >> shift) & 0xffU);
        }
        transform();

        std::array<std::uint8_t, 32> result{};
        for (std::size_t i = 0; i < state_.size(); ++i) {
            result[i * 4] = static_cast<std::uint8_t>(state_[i] >> 24U);
            result[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16U);
            result[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8U);
            result[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
        }
        return result;
    }

private:
    static std::uint32_t rotateRight(std::uint32_t value, int bits) {
        return (value >> bits) | (value << (32 - bits));
    }

    void reset() {
        state_ = {{
            0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
            0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
        }};
        bufferSize_ = 0;
        bitLength_ = 0;
    }

    void transform() {
        static const std::uint32_t constants[64] = {
            0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
            0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
            0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
            0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
            0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
            0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
            0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
            0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
        };
        std::uint32_t words[64]{};
        for (int i = 0; i < 16; ++i) {
            const auto offset = static_cast<std::size_t>(i * 4);
            words[i] = (static_cast<std::uint32_t>(buffer_[offset]) << 24U) |
                (static_cast<std::uint32_t>(buffer_[offset + 1]) << 16U) |
                (static_cast<std::uint32_t>(buffer_[offset + 2]) << 8U) |
                static_cast<std::uint32_t>(buffer_[offset + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const auto s0 = rotateRight(words[i - 15], 7) ^ rotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3U);
            const auto s1 = rotateRight(words[i - 2], 17) ^ rotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10U);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        auto a = state_[0]; auto b = state_[1]; auto c = state_[2]; auto d = state_[3];
        auto e = state_[4]; auto f = state_[5]; auto g = state_[6]; auto h = state_[7];
        for (int i = 0; i < 64; ++i) {
            const auto s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
            const auto choice = (e & f) ^ (~e & g);
            const auto temp1 = h + s1 + choice + constants[i] + words[i];
            const auto s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = s0 + majority;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t bufferSize_ = 0;
    std::uint64_t bitLength_ = 0;
};

std::array<std::uint8_t, 32> hmacSha256(
    const std::string& key,
    const std::uint8_t* data,
    std::size_t size
) {
    std::array<std::uint8_t, 64> keyBlock{};
    if (key.size() > keyBlock.size()) {
        Sha256 hash;
        hash.update(reinterpret_cast<const std::uint8_t*>(key.data()), key.size());
        const auto digest = hash.finish();
        std::copy(digest.begin(), digest.end(), keyBlock.begin());
    } else {
        std::copy(key.begin(), key.end(), keyBlock.begin());
    }
    std::array<std::uint8_t, 64> innerPad{};
    std::array<std::uint8_t, 64> outerPad{};
    for (std::size_t i = 0; i < keyBlock.size(); ++i) {
        innerPad[i] = keyBlock[i] ^ 0x36U;
        outerPad[i] = keyBlock[i] ^ 0x5cU;
    }
    Sha256 inner;
    inner.update(innerPad.data(), innerPad.size());
    inner.update(data, size);
    const auto innerDigest = inner.finish();
    Sha256 outer;
    outer.update(outerPad.data(), outerPad.size());
    outer.update(innerDigest.data(), innerDigest.size());
    return outer.finish();
}

bool constantTimeEqual(const std::uint8_t* lhs, const std::uint8_t* rhs, std::size_t size) {
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < size; ++i) difference |= lhs[i] ^ rhs[i];
    return difference == 0;
}

void appendU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
    out.push_back(static_cast<std::uint8_t>(value));
}

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void appendU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void appendDouble(std::vector<std::uint8_t>& out, double value) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "double must be 64-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    appendU64(out, bits);
}

void appendString(std::vector<std::uint8_t>& out, const std::string& value) {
    if (value.size() > 1024) throw std::invalid_argument("KECP/1 string exceeds 1024 bytes");
    appendU16(out, static_cast<std::uint16_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
    std::uint8_t u8() { require(1); return data_[offset_++]; }
    std::uint16_t u16() {
        require(2);
        const auto value = static_cast<std::uint16_t>((data_[offset_] << 8U) | data_[offset_ + 1]);
        offset_ += 2;
        return value;
    }
    std::uint32_t u32() {
        require(4);
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) value = (value << 8U) | data_[offset_++];
        return value;
    }
    std::uint64_t u64() {
        require(8);
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i) value = (value << 8U) | data_[offset_++];
        return value;
    }
    double f64() {
        const auto bits = u64();
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    std::string string() {
        const auto length = u16();
        require(length);
        std::string value(reinterpret_cast<const char*>(data_ + offset_), length);
        offset_ += length;
        return value;
    }
    bool done() const { return offset_ == size_; }
private:
    void require(std::size_t count) {
        if (count > size_ - offset_) throw std::invalid_argument("truncated KECP/1 frame");
    }
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t offset_ = 0;
};

std::string directoryOf(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

void ensureDirectory(const std::string& path) {
    if (path.empty()) return;
    std::string current;
    for (std::size_t i = 0; i < path.size(); ++i) {
        current.push_back(path[i]);
        if ((path[i] != '/' && path[i] != '\\') || current.size() <= 1) continue;
#ifndef _WIN32
        mkdir(current.c_str(), 0775);
#else
        _mkdir(current.c_str());
#endif
    }
#ifndef _WIN32
    mkdir(path.c_str(), 0775);
#else
    _mkdir(path.c_str());
#endif
}

std::string readText(const std::string& path) {
    if (path.empty()) return {};
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) return {};
    std::ostringstream out;
    out << input.rdbuf();
    return out.str();
}

void writeAtomic(const std::string& path, const std::string& text) {
    if (path.empty()) return;
    ensureDirectory(directoryOf(path));
#ifndef _WIN32
    const auto pid = static_cast<long long>(getpid());
#else
    const auto pid = static_cast<long long>(_getpid());
#endif
    const auto temporary = path + ".tmp." + std::to_string(pid);
    std::ofstream output(temporary.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
    if (!output) throw std::runtime_error("failed to write cluster state: " + temporary);
    output << text;
    output.close();
    if (!output || std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        throw std::runtime_error("failed to install cluster state: " + path);
    }
}

std::string escapeJson(const std::string& value) {
    std::string out;
    for (const auto ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

std::uint64_t jsonUnsigned(const std::string& text, const std::string& name, std::uint64_t fallback = 0) {
    std::smatch match;
    const std::regex pattern("\\\"" + name + "\\\"\\s*:\\s*([0-9]+)");
    return std::regex_search(text, match, pattern) ? std::stoull(match[1].str()) : fallback;
}

std::string jsonString(const std::string& text, const std::string& name) {
    std::smatch match;
    const std::regex pattern("\\\"" + name + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
    return std::regex_search(text, match, pattern) ? match[1].str() : std::string();
}

std::string assignmentsKey(const std::vector<EmsClusterCabinetAssignment>& values) {
    std::ostringstream out;
    for (const auto& item : values) out << item.nodeId << '=' << item.cabinetNo << ';';
    return out.str();
}

bool validAssignmentSet(
    const std::vector<EmsClusterCabinetAssignment>& assignments,
    int maxMembers
) {
    if (assignments.empty() || assignments.size() > static_cast<std::size_t>(maxMembers)) return false;
    std::set<std::string> nodes;
    std::set<int> numbers;
    for (const auto& assignment : assignments) {
        if (assignment.nodeId.empty() || assignment.cabinetNo <= 0 ||
            assignment.cabinetNo > maxMembers || !nodes.insert(assignment.nodeId).second ||
            !numbers.insert(assignment.cabinetNo).second) return false;
    }
    return true;
}

bool safeInterfaceName(const std::string& value) {
    if (value.empty() || value.size() > 15) return false;
    return std::all_of(value.begin(), value.end(), [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '-' || ch == '.';
    });
}

bool validDispatchCode(std::uint16_t value) {
    switch (static_cast<EmsClusterDispatchCode>(value)) {
        case EmsClusterDispatchCode::Accepted:
        case EmsClusterDispatchCode::Clamped:
        case EmsClusterDispatchCode::ControlDisabled:
        case EmsClusterDispatchCode::NoQuorum:
        case EmsClusterDispatchCode::NotLeader:
        case EmsClusterDispatchCode::TermMismatch:
        case EmsClusterDispatchCode::MembershipMismatch:
        case EmsClusterDispatchCode::StaleSequence:
        case EmsClusterDispatchCode::Expired:
        case EmsClusterDispatchCode::CapabilityStale:
        case EmsClusterDispatchCode::NotReady:
        case EmsClusterDispatchCode::Interlocked:
        case EmsClusterDispatchCode::ManualOverride:
        case EmsClusterDispatchCode::InvalidTarget:
            return true;
    }
    return false;
}

void appendPhasePower(std::vector<std::uint8_t>& out, const EmsClusterPhasePower& value) {
    appendDouble(out, value.paKw);
    appendDouble(out, value.pbKw);
    appendDouble(out, value.pcKw);
    appendDouble(out, value.qaKvar);
    appendDouble(out, value.qbKvar);
    appendDouble(out, value.qcKvar);
}

EmsClusterPhasePower readPhasePower(ByteReader& reader) {
    EmsClusterPhasePower value;
    value.paKw = reader.f64();
    value.pbKw = reader.f64();
    value.pcKw = reader.f64();
    value.qaKvar = reader.f64();
    value.qbKvar = reader.f64();
    value.qcKvar = reader.f64();
    return value;
}

bool finitePhasePower(const EmsClusterPhasePower& value) {
    return std::isfinite(value.paKw) && std::isfinite(value.pbKw) &&
        std::isfinite(value.pcKw) && std::isfinite(value.qaKvar) &&
        std::isfinite(value.qbKvar) && std::isfinite(value.qcKvar);
}

void appendCapability(std::vector<std::uint8_t>& out, const EmsClusterCapability& value) {
    appendDouble(out, value.socPercent);
    appendDouble(out, value.ratedActivePowerKw);
    appendDouble(out, value.ratedApparentPowerKva);
    appendDouble(out, value.availableChargePowerKw);
    appendDouble(out, value.availableDischargePowerKw);
    appendDouble(out, value.availableReactivePowerKvar);
    out.push_back(value.controlEnabled ? 1U : 0U);
    out.push_back(value.ready ? 1U : 0U);
    out.push_back(value.interlocked ? 1U : 0U);
    out.push_back(value.manualOverride ? 1U : 0U);
    appendPhasePower(out, value.actual);
}

EmsClusterCapability readCapability(ByteReader& reader) {
    EmsClusterCapability value;
    value.socPercent = reader.f64();
    value.ratedActivePowerKw = reader.f64();
    value.ratedApparentPowerKva = reader.f64();
    value.availableChargePowerKw = reader.f64();
    value.availableDischargePowerKw = reader.f64();
    value.availableReactivePowerKvar = reader.f64();
    value.controlEnabled = reader.u8() != 0;
    value.ready = reader.u8() != 0;
    value.interlocked = reader.u8() != 0;
    value.manualOverride = reader.u8() != 0;
    value.actual = readPhasePower(reader);
    return value;
}

bool finiteCapability(const EmsClusterCapability& value) {
    return std::isfinite(value.socPercent) && std::isfinite(value.ratedActivePowerKw) &&
        std::isfinite(value.ratedApparentPowerKva) &&
        std::isfinite(value.availableChargePowerKw) &&
        std::isfinite(value.availableDischargePowerKw) &&
        std::isfinite(value.availableReactivePowerKvar) && finitePhasePower(value.actual);
}

}  // namespace

std::vector<std::uint8_t> EmsClusterProtocol::encode(
    const EmsClusterMessage& message,
    const EmsClusterConfig& config
) {
    std::vector<std::uint8_t> payload;
    appendU64(payload, message.clusterIdHash);
    appendU64(payload, message.configHash);
    appendU64(payload, message.term);
    appendU64(payload, message.membershipEpoch);
    appendU64(payload, message.sequence);
    appendU64(payload, message.proposalId);
    appendString(payload, message.senderNodeId);
    appendString(payload, message.senderBootId);
    appendString(payload, message.leaderNodeId);
    appendDouble(payload, message.loadScore);
    appendU32(payload, static_cast<std::uint32_t>(message.electionPriority));
    appendU16(payload, static_cast<std::uint16_t>(message.tcpPort));
    appendU16(payload, static_cast<std::uint16_t>(message.cabinetNo));
    appendU16(payload, static_cast<std::uint16_t>(message.lockedCabinetNo));
    payload.push_back(message.voteGranted ? 1U : 0U);
    payload.push_back(message.metricsComplete ? 1U : 0U);
    payload.push_back(message.computeHealthy ? 1U : 0U);
    appendU64(payload, message.dispatchSequence);
    appendU32(payload, message.dispatchTtlMs);
    appendU16(payload, static_cast<std::uint16_t>(message.dispatchCode));
    appendCapability(payload, message.capability);
    appendPhasePower(payload, message.requestedPower);
    appendPhasePower(payload, message.acceptedPower);
    appendU16(payload, static_cast<std::uint16_t>(message.assignments.size()));
    for (const auto& assignment : message.assignments) {
        appendString(payload, assignment.nodeId);
        appendU16(payload, static_cast<std::uint16_t>(assignment.cabinetNo));
    }
    if (payload.size() + kHeaderSize + kAuthTagSize > kMaxFrameSize) {
        throw std::invalid_argument("KECP/1 frame exceeds 64 KiB");
    }

    const bool authenticated = config.securityMode == "psk";
    std::vector<std::uint8_t> frame;
    appendU32(frame, kProtocolMagic);
    frame.push_back(kProtocolVersion);
    frame.push_back(static_cast<std::uint8_t>(message.type));
    appendU16(frame, authenticated ? 1U : 0U);
    appendU32(frame, static_cast<std::uint32_t>(payload.size()));
    appendU32(frame, crc32(payload.data(), payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    if (authenticated) {
        const auto tag = hmacSha256(config.psk, frame.data(), frame.size());
        frame.insert(frame.end(), tag.begin(), tag.end());
    }
    return frame;
}

EmsClusterMessage EmsClusterProtocol::decode(
    const std::uint8_t* data,
    std::size_t size,
    const EmsClusterConfig& config
) {
    if (data == nullptr || size < kHeaderSize || size > kMaxFrameSize) {
        throw std::invalid_argument("invalid KECP/1 frame size");
    }
    ByteReader header(data, kHeaderSize);
    if (header.u32() != kProtocolMagic) throw std::invalid_argument("invalid KECP/1 magic");
    if (header.u8() != kProtocolVersion) throw std::invalid_argument("unsupported KECP/1 version");
    const auto type = static_cast<EmsClusterMessageType>(header.u8());
    if (static_cast<int>(type) < static_cast<int>(EmsClusterMessageType::Discover) ||
        static_cast<int>(type) > static_cast<int>(EmsClusterMessageType::Feedback)) {
        throw std::invalid_argument("unsupported KECP/1 message type");
    }
    const auto flags = header.u16();
    const auto payloadSize = header.u32();
    const auto expectedCrc = header.u32();
    const bool authenticated = (flags & 1U) != 0;
    const auto expectedSize = kHeaderSize + payloadSize + (authenticated ? kAuthTagSize : 0U);
    if (expectedSize != size) throw std::invalid_argument("KECP/1 frame length mismatch");
    if (config.securityMode == "psk" && !authenticated) {
        throw std::invalid_argument("unauthenticated KECP/1 frame rejected in PSK mode");
    }
    if (config.securityMode == "none" && authenticated) {
        throw std::invalid_argument("authenticated KECP/1 frame rejected in none mode");
    }
    if (authenticated) {
        const auto tag = hmacSha256(config.psk, data, kHeaderSize + payloadSize);
        if (!constantTimeEqual(tag.data(), data + kHeaderSize + payloadSize, tag.size())) {
            throw std::invalid_argument("KECP/1 authentication failed");
        }
    }
    const auto* payload = data + kHeaderSize;
    if (crc32(payload, payloadSize) != expectedCrc) throw std::invalid_argument("KECP/1 CRC mismatch");
    ByteReader reader(payload, payloadSize);
    EmsClusterMessage message;
    message.type = type;
    message.clusterIdHash = reader.u64();
    message.configHash = reader.u64();
    message.term = reader.u64();
    message.membershipEpoch = reader.u64();
    message.sequence = reader.u64();
    message.proposalId = reader.u64();
    message.senderNodeId = reader.string();
    message.senderBootId = reader.string();
    message.leaderNodeId = reader.string();
    message.loadScore = reader.f64();
    message.electionPriority = static_cast<int>(reader.u32());
    message.tcpPort = reader.u16();
    message.cabinetNo = reader.u16();
    message.lockedCabinetNo = reader.u16();
    message.voteGranted = reader.u8() != 0;
    message.metricsComplete = reader.u8() != 0;
    message.computeHealthy = reader.u8() != 0;
    message.dispatchSequence = reader.u64();
    message.dispatchTtlMs = reader.u32();
    const auto dispatchCode = reader.u16();
    if (!validDispatchCode(dispatchCode)) {
        throw std::invalid_argument("KECP/1 frame contains an invalid dispatch code");
    }
    message.dispatchCode = static_cast<EmsClusterDispatchCode>(dispatchCode);
    message.capability = readCapability(reader);
    message.requestedPower = readPhasePower(reader);
    message.acceptedPower = readPhasePower(reader);
    if (!std::isfinite(message.loadScore) || !finiteCapability(message.capability) ||
        !finitePhasePower(message.requestedPower) || !finitePhasePower(message.acceptedPower)) {
        throw std::invalid_argument("KECP/1 frame contains non-finite control data");
    }
    const auto assignmentCount = reader.u16();
    if (assignmentCount > 32) throw std::invalid_argument("KECP/1 assignment count exceeds limit");
    for (std::uint16_t i = 0; i < assignmentCount; ++i) {
        EmsClusterCabinetAssignment assignment;
        assignment.nodeId = reader.string();
        assignment.cabinetNo = reader.u16();
        message.assignments.push_back(std::move(assignment));
    }
    if (!reader.done()) throw std::invalid_argument("KECP/1 payload contains trailing bytes");
    return message;
}

std::uint64_t EmsClusterProtocol::clusterIdHash(const std::string& clusterId) {
    return fnv1a64("KECP/1:cluster:" + clusterId);
}

std::uint64_t EmsClusterProtocol::configHash(const EmsClusterConfig& config) {
    std::ostringstream normalized;
    normalized << "KECP/1|" << config.clusterId << '|' << config.transport << '|'
               << config.securityMode << '|' << config.expectedMembers << '|'
               << config.maxMembers << '|' << config.minimumQuorum << '|'
               << config.heartbeatMs << '|' << config.leaderLeaseMs << '|'
               << config.controlEnabled << '|' << config.dispatchCycleMs << '|'
               << config.dispatchTtlMs << '|' << config.capabilityTtlMs << '|'
               << config.stationTargetTtlMs << '|'
               << config.zeroTargetOnLoss << '|' << config.virtualSharedMemoryName << '|'
               << config.virtualPointBaseIndex;
    return fnv1a64(normalized.str());
}

EmsClusterNode::EmsClusterNode(EmsClusterConfig config, std::string nodeId, std::string bootId)
    : config_(std::move(config)), nodeId_(std::move(nodeId)), bootId_(std::move(bootId)) {
    validateConfig(config_);
    if (nodeId_.empty()) throw std::invalid_argument("EMS cluster node requires machineCode");
    if (bootId_.empty()) throw std::invalid_argument("EMS cluster node requires bootId");
    clusterIdHash_ = EmsClusterProtocol::clusterIdHash(config_.clusterId);
    configHash_ = EmsClusterProtocol::configHash(config_);
    role_ = config_.enabled ? EmsClusterRole::Discovering : EmsClusterRole::Disabled;
    reason_ = config_.enabled ? "waiting for compatible members" : "cluster disabled";
    if (config_.enabled) {
        loadPersistentState();
        loadMembership();
    }
}

void EmsClusterNode::validateConfig(const EmsClusterConfig& config) {
    if (!config.enabled) return;
    if (config.clusterId.empty()) throw std::invalid_argument("emsCluster.clusterId is required");
    if (config.transport != "ethernet") throw std::invalid_argument("emsCluster transport must currently be ethernet");
    if (!safeInterfaceName(config.clusterInterface)) throw std::invalid_argument("invalid emsCluster.clusterInterface");
    if (config.ipMode != "autoLinkLocal" && config.ipMode != "static") {
        throw std::invalid_argument("emsCluster.ipMode must be autoLinkLocal or static");
    }
    if (config.prefixLength < 8 || config.prefixLength > 30) throw std::invalid_argument("invalid emsCluster.prefixLength");
    if (config.securityMode != "psk" && config.securityMode != "none") {
        throw std::invalid_argument("emsCluster currently supports securityMode=psk or none");
    }
    if (config.securityMode == "psk" && config.psk.size() < 16) {
        throw std::invalid_argument("emsCluster.psk must contain at least 16 characters");
    }
    if (config.expectedMembers < 2 || config.expectedMembers > 5) {
        throw std::invalid_argument("emsCluster.expectedMembers must be between 2 and 5");
    }
    if (config.maxMembers < config.expectedMembers || config.maxMembers > 5) {
        throw std::invalid_argument("emsCluster.maxMembers must be expectedMembers..5");
    }
    const auto majority = config.expectedMembers / 2 + 1;
    if (config.minimumQuorum != 0 &&
        (config.minimumQuorum < majority || config.minimumQuorum > config.expectedMembers)) {
        throw std::invalid_argument("emsCluster.minimumQuorum cannot be lower than the expected-member majority");
    }
    if (config.lockedCabinetNo < 0 || config.lockedCabinetNo > config.maxMembers) {
        throw std::invalid_argument("emsCluster.lockedCabinetNo is outside maxMembers");
    }
    if (config.discoveryPort <= 0 || config.discoveryPort > 65535 || config.tcpPort <= 0 || config.tcpPort > 65535) {
        throw std::invalid_argument("invalid emsCluster discovery/tcp port");
    }
    if (config.heartbeatMs < 100 || config.leaderLeaseMs < config.heartbeatMs * 3) {
        throw std::invalid_argument("emsCluster.leaderLeaseMs must cover at least three heartbeats");
    }
    if (config.electionTimeoutMinMs <= config.leaderLeaseMs ||
        config.electionTimeoutMaxMs < config.electionTimeoutMinMs) {
        throw std::invalid_argument("emsCluster election timeout must be greater than leader lease");
    }
    if (config.memberTimeoutMs <= config.electionTimeoutMaxMs) {
        throw std::invalid_argument("emsCluster.memberTimeoutMs must exceed electionTimeoutMaxMs");
    }
    if (config.controlEnabled && config.securityMode != "psk") {
        throw std::invalid_argument("emsCluster control requires securityMode=psk");
    }
    if (config.dispatchCycleMs < config.heartbeatMs || config.dispatchCycleMs > 10000) {
        throw std::invalid_argument("emsCluster.dispatchCycleMs must be heartbeatMs..10000");
    }
    if (config.dispatchTtlMs < config.dispatchCycleMs * 2 || config.dispatchTtlMs > 30000) {
        throw std::invalid_argument("emsCluster.dispatchTtlMs must cover at least two dispatch cycles");
    }
    if (config.capabilityTtlMs < config.dispatchCycleMs * 2 || config.capabilityTtlMs > 30000) {
        throw std::invalid_argument("emsCluster.capabilityTtlMs must cover at least two dispatch cycles");
    }
    if (config.stationTargetTtlMs < config.dispatchCycleMs * 2 || config.stationTargetTtlMs > 30000) {
        throw std::invalid_argument("emsCluster.stationTargetTtlMs must cover at least two dispatch cycles");
    }
    if (config.virtualSharedMemoryName.empty()) {
        throw std::invalid_argument("emsCluster.virtualSharedMemoryName is required");
    }
    if (config.virtualPointBaseIndex == 0 || config.virtualPointBaseIndex > 999999000U) {
        throw std::invalid_argument("emsCluster.virtualPointBaseIndex is outside the supported range");
    }
}

double EmsClusterNode::calculateLoadScore(const EmsClusterLoadSample& load) {
    const auto clamp = [](double value, double minValue, double maxValue) {
        return std::max(minValue, std::min(maxValue, value));
    };
    const auto timeout = load.computeMetricsAvailable ? clamp(load.computeTimeoutPercent, 0.0, 100.0) : 100.0;
    const auto queue = load.computeMetricsAvailable ? clamp(load.controlQueueP95Ms / 10.0, 0.0, 100.0) : 100.0;
    return clamp(
        0.35 * clamp(load.cpuPercent, 0.0, 100.0) +
        0.15 * clamp(load.memoryPercent, 0.0, 100.0) +
        0.25 * timeout +
        0.15 * queue +
        0.10 * clamp(load.packetLossPercent, 0.0, 100.0),
        0.0,
        100.0
    );
}

const char* EmsClusterNode::roleName(EmsClusterRole role) {
    switch (role) {
        case EmsClusterRole::Disabled: return "disabled";
        case EmsClusterRole::Discovering: return "discovering";
        case EmsClusterRole::Follower: return "follower";
        case EmsClusterRole::Candidate: return "candidate";
        case EmsClusterRole::Leader: return "leader";
        case EmsClusterRole::Quarantined: return "quarantined";
        case EmsClusterRole::Fault: return "fault";
    }
    return "unknown";
}

const char* EmsClusterNode::dispatchCodeName(EmsClusterDispatchCode code) {
    switch (code) {
        case EmsClusterDispatchCode::Accepted: return "accepted";
        case EmsClusterDispatchCode::Clamped: return "clamped";
        case EmsClusterDispatchCode::ControlDisabled: return "control_disabled";
        case EmsClusterDispatchCode::NoQuorum: return "no_quorum";
        case EmsClusterDispatchCode::NotLeader: return "not_leader";
        case EmsClusterDispatchCode::TermMismatch: return "term_mismatch";
        case EmsClusterDispatchCode::MembershipMismatch: return "membership_mismatch";
        case EmsClusterDispatchCode::StaleSequence: return "stale_sequence";
        case EmsClusterDispatchCode::Expired: return "expired";
        case EmsClusterDispatchCode::CapabilityStale: return "capability_stale";
        case EmsClusterDispatchCode::NotReady: return "not_ready";
        case EmsClusterDispatchCode::Interlocked: return "interlocked";
        case EmsClusterDispatchCode::ManualOverride: return "manual_override";
        case EmsClusterDispatchCode::InvalidTarget: return "invalid_target";
    }
    return "unknown";
}

void EmsClusterNode::loadPersistentState() {
    const auto text = readText(config_.consensusStateFile);
    if (text.empty()) return;
    const auto persistedClusterId = jsonString(text, "clusterId");
    if (!persistedClusterId.empty() && persistedClusterId != config_.clusterId) return;
    currentTerm_ = jsonUnsigned(text, "term", 0);
    votedFor_ = jsonString(text, "votedFor");
}

void EmsClusterNode::persistConsensusState() const {
    std::ostringstream out;
    out << "{\n  \"schemaVersion\": \"1.0\",\n  \"clusterId\": \"" << escapeJson(config_.clusterId)
        << "\",\n  \"term\": " << currentTerm_
        << ",\n  \"votedFor\": \"" << escapeJson(votedFor_) << "\"\n}\n";
    writeAtomic(config_.consensusStateFile, out.str());
}

void EmsClusterNode::loadMembership() {
    const auto text = readText(config_.membershipFile);
    if (text.empty()) return;
    const auto persistedClusterId = jsonString(text, "clusterId");
    if (!persistedClusterId.empty() && persistedClusterId != config_.clusterId) return;
    const auto persistedEpoch = jsonUnsigned(text, "membershipEpoch", 0);
    std::vector<EmsClusterCabinetAssignment> loaded;
    const std::regex pattern("\\{\\s*\\\"nodeId\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"\\s*,\\s*\\\"cabinetNo\\\"\\s*:\\s*([0-9]+)\\s*\\}");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern); it != std::sregex_iterator(); ++it) {
        loaded.push_back({(*it)[1].str(), std::stoi((*it)[2].str())});
    }
    if (persistedEpoch > 0 && !validAssignmentSet(loaded, config_.maxMembers)) {
        throw std::runtime_error("invalid persisted EMS cluster membership");
    }
    membershipEpoch_ = persistedEpoch;
    assignments_ = std::move(loaded);
}

void EmsClusterNode::persistMembership() const {
    std::ostringstream out;
    out << "{\n  \"schemaVersion\": \"1.0\",\n  \"clusterId\": \"" << escapeJson(config_.clusterId)
        << "\",\n  \"membershipEpoch\": " << membershipEpoch_ << ",\n  \"assignments\": [";
    for (std::size_t i = 0; i < assignments_.size(); ++i) {
        out << (i == 0 ? "\n" : ",\n") << "    {\"nodeId\": \"" << escapeJson(assignments_[i].nodeId)
            << "\", \"cabinetNo\": " << assignments_[i].cabinetNo << "}";
    }
    if (!assignments_.empty()) out << '\n';
    out << "  ]\n}\n";
    writeAtomic(config_.membershipFile, out.str());
}

void EmsClusterNode::resetElectionDeadline(std::int64_t nowMs) {
    const auto span = std::max(1, config_.electionTimeoutMaxMs - config_.electionTimeoutMinMs);
    const auto loadDelay = static_cast<int>(std::round(loadScore_ * static_cast<double>(span) * 0.006));
    const auto priorityDelay = std::max(0, 100 - config_.electionPriority) * span / 500;
    const auto jitter = static_cast<int>(fnv1a64(nodeId_ + std::to_string(currentTerm_ + 1)) % static_cast<std::uint64_t>(std::max(1, span / 5)));
    const auto delay = std::min(span, loadDelay + priorityDelay + jitter);
    electionDeadlineMs_ = nowMs + config_.electionTimeoutMinMs + delay;
}

void EmsClusterNode::tick(std::int64_t nowMs, const EmsClusterLoadSample& load) {
    if (!config_.enabled || role_ == EmsClusterRole::Disabled || role_ == EmsClusterRole::Quarantined || role_ == EmsClusterRole::Fault) return;
    load_ = load;
    loadScore_ = calculateLoadScore(load);
    if (electionDeadlineMs_ == 0) resetElectionDeadline(nowMs);

    if (lastDiscoveryMs_ == 0 || nowMs - lastDiscoveryMs_ >= config_.discoveryIntervalMs) {
        queue(EmsClusterMessageType::Discover, {}, true);
        lastDiscoveryMs_ = nowMs;
    }

    if (role_ == EmsClusterRole::Leader) {
        if (!load.computeHealthy) {
            queue(EmsClusterMessageType::StepDown);
            invalidateDispatch(EmsClusterDispatchCode::NotReady);
            becomeFollower(currentTerm_, {}, nowMs, "local ComputeEngine is unhealthy");
            return;
        }
        if (lastHeartbeatMs_ == 0 || nowMs - lastHeartbeatMs_ >= config_.heartbeatMs) {
            queue(EmsClusterMessageType::Heartbeat);
            lastHeartbeatMs_ = nowMs;
        }
        int acknowledgements = 1;
        for (const auto& entry : members_) {
            if (isVotingMember(entry.first) && entry.second.status.compatible &&
                nowMs - entry.second.lastAckMs <= config_.leaderLeaseMs) {
                ++acknowledgements;
            }
        }
        if (acknowledgements >= effectiveQuorum()) lastQuorumMs_ = nowMs;
        if (lastQuorumMs_ > 0 && nowMs - lastQuorumMs_ > config_.leaderLeaseMs) {
            queue(EmsClusterMessageType::StepDown);
            invalidateDispatch(EmsClusterDispatchCode::NoQuorum);
            becomeFollower(currentTerm_, {}, nowMs, "leader lost majority lease");
            return;
        }
        maybeProposeMembership(nowMs);
        maybeCommitMembership(nowMs);
        tickDispatch(nowMs);
        return;
    }

    if (!leaderNodeId_.empty() && !leaderLeaseValid(nowMs)) {
        leaderNodeId_.clear();
        role_ = EmsClusterRole::Discovering;
        reason_ = "leader lease expired";
    }
    if (leaderNodeId_.empty() && nowMs >= electionDeadlineMs_) {
        if (!isVotingMember(nodeId_)) {
            role_ = EmsClusterRole::Discovering;
            reason_ = "waiting for committed membership";
            resetElectionDeadline(nowMs);
            tickDispatch(nowMs);
            return;
        }
        if (!load.computeHealthy) {
            role_ = EmsClusterRole::Discovering;
            reason_ = "local ComputeEngine is unhealthy and cannot become leader";
            resetElectionDeadline(nowMs);
            tickDispatch(nowMs);
            return;
        }
        if (onlineCompatibleCount(nowMs) >= effectiveQuorum()) {
            startElection(nowMs);
        } else {
            role_ = EmsClusterRole::Discovering;
            reason_ = "waiting for quorum";
            resetElectionDeadline(nowMs);
        }
    }
    tickDispatch(nowMs);
}

void EmsClusterNode::refreshMember(const EmsClusterInbound& inbound, std::int64_t nowMs) {
    const auto& message = inbound.message;
    if (message.senderNodeId.empty() || message.senderNodeId == nodeId_) return;
    auto& member = members_[message.senderNodeId];
    member.status.nodeId = message.senderNodeId;
    member.status.bootId = message.senderBootId;
    member.status.address = inbound.sourceAddress;
    member.status.lastSeenMs = nowMs;
    member.status.loadScore = message.loadScore;
    member.status.electionPriority = message.electionPriority;
    member.status.cabinetNo = message.cabinetNo;
    member.status.lockedCabinetNo = message.lockedCabinetNo;
    member.status.metricsComplete = message.metricsComplete;
    member.status.computeHealthy = message.computeHealthy;
    member.status.compatible = message.configHash == configHash_;
    member.status.duplicateIdentity = false;
}

void EmsClusterNode::receive(const EmsClusterInbound& inbound, std::int64_t nowMs) {
    if (!config_.enabled || role_ == EmsClusterRole::Disabled || role_ == EmsClusterRole::Fault) return;
    const auto& message = inbound.message;
    if (message.clusterIdHash != clusterIdHash_) return;
    if (message.senderNodeId == nodeId_) {
        if (!message.senderBootId.empty() && message.senderBootId != bootId_) {
            role_ = EmsClusterRole::Quarantined;
            reason_ = "duplicate machineCode detected with a different bootId";
        }
        return;
    }
    const auto previous = members_.find(message.senderNodeId);
    if (previous != members_.end() && !previous->second.status.bootId.empty() &&
        previous->second.status.bootId != message.senderBootId &&
        nowMs - previous->second.status.lastSeenMs <= config_.memberTimeoutMs) {
        previous->second.status.duplicateIdentity = true;
        previous->second.status.compatible = false;
        reason_ = "duplicate remote machineCode detected: " + message.senderNodeId;
        return;
    }
    refreshMember(inbound, nowMs);
    auto memberIt = members_.find(message.senderNodeId);
    if (memberIt == members_.end()) return;
    auto& member = memberIt->second;
    if (!member.status.compatible) return;
    if (message.type == EmsClusterMessageType::Discover &&
        message.senderBootId == member.status.bootId && message.sequence < member.lastSequence) {
        member.lastSequence = 0;
    }
    if (message.type != EmsClusterMessageType::Discover && message.type != EmsClusterMessageType::Hello) {
        if (message.sequence <= member.lastSequence) return;
        member.lastSequence = message.sequence;
    }
    if (message.term > currentTerm_) becomeFollower(message.term, {}, nowMs, "observed a higher term");

    switch (message.type) {
        case EmsClusterMessageType::Discover:
            queue(EmsClusterMessageType::Hello, message.senderNodeId);
            break;
        case EmsClusterMessageType::Hello:
            break;
        case EmsClusterMessageType::Heartbeat:
        case EmsClusterMessageType::LeaderCommit:
            if (message.term < currentTerm_ || message.leaderNodeId != message.senderNodeId) break;
            becomeFollower(message.term, message.senderNodeId, nowMs, "leader heartbeat received");
            if (message.membershipEpoch > membershipEpoch_ && !message.assignments.empty()) {
                handleMembershipCommit(message, nowMs);
            }
            queue(EmsClusterMessageType::HeartbeatAck, message.senderNodeId);
            break;
        case EmsClusterMessageType::HeartbeatAck:
            if (role_ == EmsClusterRole::Leader && message.term == currentTerm_) member.lastAckMs = nowMs;
            break;
        case EmsClusterMessageType::VoteRequest:
            handleVoteRequest(message, nowMs);
            break;
        case EmsClusterMessageType::VoteReply:
            if (role_ == EmsClusterRole::Candidate && message.term == currentTerm_ &&
                message.voteGranted && isVotingMember(message.senderNodeId)) {
                votesGranted_.insert(message.senderNodeId);
                if (static_cast<int>(votesGranted_.size()) >= effectiveQuorum()) becomeLeader(nowMs);
            }
            break;
        case EmsClusterMessageType::StepDown:
            if (message.term == currentTerm_ && message.senderNodeId == leaderNodeId_) {
                becomeFollower(currentTerm_, {}, nowMs, "leader stepped down");
            }
            break;
        case EmsClusterMessageType::MembershipProposal:
            handleMembershipProposal(message, nowMs);
            break;
        case EmsClusterMessageType::MembershipAck:
            if (role_ == EmsClusterRole::Leader && message.term == currentTerm_ &&
                message.proposalId == pendingProposalId_ &&
                std::find_if(pendingAssignments_.begin(), pendingAssignments_.end(), [&](const auto& item) {
                    return item.nodeId == message.senderNodeId;
                }) != pendingAssignments_.end()) {
                membershipAcks_.insert(message.senderNodeId);
            }
            break;
        case EmsClusterMessageType::MembershipCommit:
            handleMembershipCommit(message, nowMs);
            break;
        case EmsClusterMessageType::CapabilityReport:
            handleCapabilityReport(message, nowMs);
            break;
        case EmsClusterMessageType::DispatchTarget:
            handleDispatchTarget(message, nowMs);
            break;
        case EmsClusterMessageType::DispatchAck:
            handleDispatchAck(message, nowMs);
            break;
        case EmsClusterMessageType::Feedback:
            handleFeedback(message, nowMs);
            break;
    }
}

void EmsClusterNode::becomeFollower(
    std::uint64_t term,
    const std::string& leader,
    std::int64_t nowMs,
    const std::string& reason
) {
    const bool termChanged = term > currentTerm_;
    const bool leaderChanged = !leader.empty() && leader != leaderNodeId_;
    if (termChanged) {
        currentTerm_ = term;
        votedFor_.clear();
        persistConsensusState();
    }
    role_ = leader.empty() ? EmsClusterRole::Discovering : EmsClusterRole::Follower;
    leaderNodeId_ = leader;
    if (!leader.empty()) lastLeaderSeenMs_ = nowMs;
    votesGranted_.clear();
    if (termChanged || leaderChanged) {
        pendingProposalId_ = 0;
        pendingAssignments_.clear();
        membershipAcks_.clear();
        pendingProposalLastSentMs_ = 0;
        invalidateDispatch(EmsClusterDispatchCode::TermMismatch);
    }
    reason_ = reason;
    resetElectionDeadline(nowMs);
}

void EmsClusterNode::startElection(std::int64_t nowMs) {
    ++currentTerm_;
    votedFor_ = nodeId_;
    persistConsensusState();
    role_ = EmsClusterRole::Candidate;
    leaderNodeId_.clear();
    votesGranted_.clear();
    votesGranted_.insert(nodeId_);
    reason_ = "requesting majority vote";
    invalidateDispatch(EmsClusterDispatchCode::NotLeader);
    resetElectionDeadline(nowMs);
    queue(EmsClusterMessageType::VoteRequest);
}

void EmsClusterNode::becomeLeader(std::int64_t nowMs) {
    role_ = EmsClusterRole::Leader;
    leaderNodeId_ = nodeId_;
    lastQuorumMs_ = nowMs;
    lastHeartbeatMs_ = 0;
    dispatchSequence_ = 0;
    invalidateDispatch(EmsClusterDispatchCode::Expired);
    reason_ = "majority vote committed leader";
    queue(EmsClusterMessageType::LeaderCommit);
}

void EmsClusterNode::handleVoteRequest(const EmsClusterMessage& message, std::int64_t nowMs) {
    bool grant = false;
    if (message.term == currentTerm_ &&
        (votedFor_.empty() || votedFor_ == message.senderNodeId) &&
        message.configHash == configHash_ && message.computeHealthy &&
        message.membershipEpoch >= membershipEpoch_ && isVotingMember(message.senderNodeId) &&
        isVotingMember(nodeId_)) {
        votedFor_ = message.senderNodeId;
        persistConsensusState();
        grant = true;
        leaderNodeId_.clear();
        role_ = EmsClusterRole::Follower;
        reason_ = "vote granted";
        resetElectionDeadline(nowMs);
    }
    auto reply = baseMessage(EmsClusterMessageType::VoteReply);
    reply.voteGranted = grant;
    outgoing_.push_back({std::move(reply), message.senderNodeId, false});
}

void EmsClusterNode::handleMembershipProposal(const EmsClusterMessage& message, std::int64_t nowMs) {
    if (message.term != currentTerm_ || message.senderNodeId != leaderNodeId_ ||
        message.membershipEpoch <= membershipEpoch_ || message.assignments.empty()) return;
    if (!validAssignmentsForLocal(message.assignments)) return;
    if (std::find_if(message.assignments.begin(), message.assignments.end(), [&](const auto& item) {
            return item.nodeId == nodeId_;
        }) == message.assignments.end()) return;
    pendingProposalId_ = message.proposalId;
    pendingMembershipEpoch_ = message.membershipEpoch;
    pendingAssignments_ = message.assignments;
    auto ack = baseMessage(EmsClusterMessageType::MembershipAck);
    ack.proposalId = pendingProposalId_;
    ack.membershipEpoch = pendingMembershipEpoch_;
    outgoing_.push_back({std::move(ack), message.senderNodeId, false});
    lastLeaderSeenMs_ = nowMs;
}

void EmsClusterNode::handleMembershipCommit(const EmsClusterMessage& message, std::int64_t nowMs) {
    if (message.term != currentTerm_ || message.senderNodeId != leaderNodeId_ ||
        message.membershipEpoch < membershipEpoch_ || message.assignments.empty() ||
        !validAssignmentsForLocal(message.assignments)) return;
    if (message.proposalId != 0 && pendingProposalId_ != 0 && message.proposalId != pendingProposalId_) return;
    assignments_ = message.assignments;
    membershipEpoch_ = message.membershipEpoch;
    persistMembership();
    pendingProposalId_ = 0;
    pendingAssignments_.clear();
    pendingProposalLastSentMs_ = 0;
    reason_ = "membership committed";
    lastLeaderSeenMs_ = nowMs;
}

void EmsClusterNode::maybeProposeMembership(std::int64_t nowMs) {
    if (pendingProposalId_ != 0) {
        if (pendingProposalLastSentMs_ == 0 ||
            nowMs - pendingProposalLastSentMs_ >= config_.heartbeatMs * 4) {
            auto proposal = baseMessage(EmsClusterMessageType::MembershipProposal);
            proposal.proposalId = pendingProposalId_;
            proposal.membershipEpoch = pendingMembershipEpoch_;
            proposal.assignments = pendingAssignments_;
            outgoing_.push_back({std::move(proposal), {}, false});
            pendingProposalLastSentMs_ = nowMs;
        }
        return;
    }
    struct Candidate { std::string nodeId; int priority; int locked; };
    std::vector<Candidate> candidates{{nodeId_, config_.electionPriority, config_.lockedCabinetNo}};
    for (const auto& entry : members_) {
        if (entry.second.status.compatible &&
            nowMs - entry.second.status.lastSeenMs <= config_.memberTimeoutMs) {
            candidates.push_back({
                entry.first,
                entry.second.status.electionPriority,
                entry.second.status.lockedCabinetNo
            });
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.priority != rhs.priority ? lhs.priority > rhs.priority : lhs.nodeId < rhs.nodeId;
    });

    auto desired = assignments_;
    std::map<int, std::string> lockedOwners;
    for (const auto& candidate : candidates) {
        if (candidate.locked == 0) continue;
        const auto inserted = lockedOwners.emplace(candidate.locked, candidate.nodeId);
        if (!inserted.second && inserted.first->second != candidate.nodeId) {
            reason_ = "duplicate locked cabinet number: " + std::to_string(candidate.locked);
            return;
        }
    }
    for (const auto& candidate : candidates) {
        if (candidate.locked == 0) continue;
        auto own = std::find_if(desired.begin(), desired.end(), [&](const auto& item) {
            return item.nodeId == candidate.nodeId;
        });
        const auto occupant = std::find_if(desired.begin(), desired.end(), [&](const auto& item) {
            return item.cabinetNo == candidate.locked;
        });
        if (occupant != desired.end() && occupant->nodeId != candidate.nodeId) {
            reason_ = "locked cabinet number conflicts with committed membership: " +
                std::to_string(candidate.locked);
            return;
        }
        if (own == desired.end()) desired.push_back({candidate.nodeId, candidate.locked});
        else own->cabinetNo = candidate.locked;
    }

    std::set<int> used;
    std::set<std::string> assignedNodes;
    for (const auto& item : desired) {
        used.insert(item.cabinetNo);
        assignedNodes.insert(item.nodeId);
    }
    for (const auto& candidate : candidates) {
        if (assignedNodes.count(candidate.nodeId) != 0) continue;
        int number = candidate.locked;
        if (number > 0 && used.count(number) != 0) {
            reason_ = "locked cabinet number conflicts with committed membership";
            return;
        }
        if (number == 0) {
            for (int value = 1; value <= config_.maxMembers; ++value) {
                if (used.count(value) == 0) { number = value; break; }
            }
        }
        if (number == 0) continue;
        desired.push_back({candidate.nodeId, number});
        assignedNodes.insert(candidate.nodeId);
        used.insert(number);
    }
    std::sort(desired.begin(), desired.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.cabinetNo < rhs.cabinetNo;
    });
    if (sameAssignments(desired, assignments_)) return;
    pendingAssignments_ = desired;
    pendingMembershipEpoch_ = membershipEpoch_ + 1;
    pendingProposalId_ = fnv1a64(
        nodeId_ + ':' + std::to_string(currentTerm_) + ':' +
        std::to_string(pendingMembershipEpoch_) + ':' + assignmentsKey(desired)
    );
    membershipAcks_.clear();
    membershipAcks_.insert(nodeId_);
    auto proposal = baseMessage(EmsClusterMessageType::MembershipProposal);
    proposal.proposalId = pendingProposalId_;
    proposal.membershipEpoch = pendingMembershipEpoch_;
    proposal.assignments = pendingAssignments_;
    outgoing_.push_back({std::move(proposal), {}, false});
    pendingProposalLastSentMs_ = nowMs;
}

void EmsClusterNode::maybeCommitMembership(std::int64_t) {
    const auto pendingMajority = static_cast<int>(pendingAssignments_.size() / 2U + 1U);
    const auto requiredAcks = std::max(effectiveQuorum(), pendingMajority);
    if (pendingProposalId_ == 0 || static_cast<int>(membershipAcks_.size()) < requiredAcks) return;
    assignments_ = pendingAssignments_;
    membershipEpoch_ = pendingMembershipEpoch_;
    persistMembership();
    auto commit = baseMessage(EmsClusterMessageType::MembershipCommit);
    commit.proposalId = pendingProposalId_;
    commit.membershipEpoch = membershipEpoch_;
    commit.assignments = assignments_;
    outgoing_.push_back({std::move(commit), {}, false});
    pendingProposalId_ = 0;
    pendingAssignments_.clear();
    membershipAcks_.clear();
    pendingProposalLastSentMs_ = 0;
    reason_ = "leader and majority committed membership";
}

void EmsClusterNode::queue(EmsClusterMessageType type, const std::string& target, bool discovery) {
    outgoing_.push_back({baseMessage(type), target, discovery});
}

EmsClusterMessage EmsClusterNode::baseMessage(EmsClusterMessageType type) const {
    EmsClusterMessage message;
    message.type = type;
    message.clusterIdHash = clusterIdHash_;
    message.configHash = configHash_;
    message.term = currentTerm_;
    message.membershipEpoch = membershipEpoch_;
    message.sequence = ++sequence_;
    message.senderNodeId = nodeId_;
    message.senderBootId = bootId_;
    message.leaderNodeId = leaderNodeId_;
    message.loadScore = loadScore_;
    message.electionPriority = config_.electionPriority;
    message.tcpPort = config_.tcpPort;
    message.cabinetNo = cabinetNoFor(nodeId_);
    message.lockedCabinetNo = config_.lockedCabinetNo;
    message.metricsComplete = load_.computeMetricsAvailable;
    message.computeHealthy = load_.computeHealthy;
    if (type == EmsClusterMessageType::Heartbeat || type == EmsClusterMessageType::LeaderCommit) {
        message.assignments = assignments_;
    }
    return message;
}

int EmsClusterNode::effectiveQuorum() const {
    const auto votingMembers = assignments_.empty()
        ? config_.expectedMembers
        : static_cast<int>(assignments_.size());
    const auto majority = votingMembers / 2 + 1;
    return std::max(config_.minimumQuorum, majority);
}

int EmsClusterNode::onlineCompatibleCount(std::int64_t nowMs) const {
    int count = isVotingMember(nodeId_) ? 1 : 0;
    for (const auto& entry : members_) {
        if (isVotingMember(entry.first) && entry.second.status.compatible &&
            nowMs - entry.second.status.lastSeenMs <= config_.memberTimeoutMs) ++count;
    }
    return count;
}

int EmsClusterNode::cabinetNoFor(const std::string& nodeId) const {
    const auto it = std::find_if(assignments_.begin(), assignments_.end(), [&](const auto& item) {
        return item.nodeId == nodeId;
    });
    return it == assignments_.end() ? 0 : it->cabinetNo;
}

bool EmsClusterNode::isVotingMember(const std::string& nodeId) const {
    return assignments_.empty() || cabinetNoFor(nodeId) > 0;
}

bool EmsClusterNode::leaderLeaseValid(std::int64_t nowMs) const {
    return !leaderNodeId_.empty() && lastLeaderSeenMs_ > 0 && nowMs - lastLeaderSeenMs_ <= config_.leaderLeaseMs;
}

bool EmsClusterNode::sameAssignments(
    const std::vector<EmsClusterCabinetAssignment>& lhs,
    const std::vector<EmsClusterCabinetAssignment>& rhs
) const {
    if (lhs.size() != rhs.size()) return false;
    auto left = lhs;
    auto right = rhs;
    const auto order = [](const auto& a, const auto& b) { return a.nodeId < b.nodeId; };
    std::sort(left.begin(), left.end(), order);
    std::sort(right.begin(), right.end(), order);
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i].nodeId != right[i].nodeId || left[i].cabinetNo != right[i].cabinetNo) return false;
    }
    return true;
}

bool EmsClusterNode::validAssignmentsForLocal(
    const std::vector<EmsClusterCabinetAssignment>& assignments
) const {
    if (!validAssignmentSet(assignments, config_.maxMembers)) return false;
    if (config_.lockedCabinetNo == 0) return true;
    const auto own = std::find_if(assignments.begin(), assignments.end(), [&](const auto& item) {
        return item.nodeId == nodeId_;
    });
    return own != assignments.end() && own->cabinetNo == config_.lockedCabinetNo;
}

std::vector<EmsClusterOutbound> EmsClusterNode::drainOutgoing() {
    auto result = std::move(outgoing_);
    outgoing_.clear();
    return result;
}

EmsClusterStatus EmsClusterNode::status(std::int64_t nowMs) const {
    EmsClusterStatus result;
    result.role = role_;
    result.nodeId = nodeId_;
    result.leaderNodeId = leaderNodeId_;
    result.term = currentTerm_;
    result.membershipEpoch = membershipEpoch_;
    result.cabinetNo = cabinetNoFor(nodeId_);
    result.onlineMembers = onlineCompatibleCount(nowMs);
    result.quorum = effectiveQuorum();
    result.quorumValid = role_ == EmsClusterRole::Leader
        ? lastQuorumMs_ > 0 && nowMs - lastQuorumMs_ <= config_.leaderLeaseMs
        : leaderLeaseValid(nowMs);
    result.metricsComplete = load_.computeMetricsAvailable;
    result.computeHealthy = load_.computeHealthy;
    result.loadScore = loadScore_;
    result.controlConfigured = config_.controlEnabled;
    result.capability = localCapability_;
    result.dispatch = activeDispatch(nowMs);
    result.controlActive = result.dispatch.valid && result.quorumValid;
    result.reason = reason_;
    for (const auto& entry : members_) {
        auto member = entry.second.status;
        member.cabinetNo = cabinetNoFor(member.nodeId);
        member.lastSeenAgeMs = member.lastSeenMs > 0 ? std::max<std::int64_t>(0, nowMs - member.lastSeenMs) : -1;
        member.online = member.compatible && member.lastSeenAgeMs >= 0 &&
            member.lastSeenAgeMs <= config_.memberTimeoutMs;
        member.capabilityFresh = entry.second.lastCapabilityMs > 0 &&
            nowMs - entry.second.lastCapabilityMs <= config_.capabilityTtlMs;
        member.capabilityAgeMs = entry.second.lastCapabilityMs > 0
            ? std::max<std::int64_t>(0, nowMs - entry.second.lastCapabilityMs)
            : -1;
        member.dispatchAgeMs = member.dispatch.receivedAtMs > 0
            ? std::max<std::int64_t>(0, nowMs - member.dispatch.receivedAtMs)
            : -1;
        if (member.dispatch.valid && member.dispatch.expireAtMs > 0 &&
            nowMs >= member.dispatch.expireAtMs) {
            member.dispatch.valid = false;
            member.dispatch.code = EmsClusterDispatchCode::Expired;
            member.dispatch.accepted = {};
        }
        result.members.push_back(std::move(member));
    }
    return result;
}

std::string emsClusterStatusJson(const EmsClusterStatus& status, std::int64_t wallNowMs) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2)
        << "{\"schemaVersion\":\"2.0\",\"ts\":" << wallNowMs
        << ",\"phase\":2,\"controlWritesEnabled\":" << (status.controlConfigured ? "true" : "false")
        << ",\"role\":\"" << EmsClusterNode::roleName(status.role) << "\""
        << ",\"nodeId\":\"" << escapeJson(status.nodeId) << "\""
        << ",\"leaderNodeId\":\"" << escapeJson(status.leaderNodeId) << "\""
        << ",\"term\":" << status.term
        << ",\"membershipEpoch\":" << status.membershipEpoch
        << ",\"cabinetNo\":" << status.cabinetNo
        << ",\"onlineMembers\":" << status.onlineMembers
        << ",\"quorum\":" << status.quorum
        << ",\"quorumValid\":" << (status.quorumValid ? "true" : "false")
        << ",\"metricsComplete\":" << (status.metricsComplete ? "true" : "false")
        << ",\"computeHealthy\":" << (status.computeHealthy ? "true" : "false")
        << ",\"loadScore\":" << status.loadScore
        << ",\"controlActive\":" << (status.controlActive ? "true" : "false")
        << ",\"dispatch\":{\"valid\":" << (status.dispatch.valid ? "true" : "false")
        << ",\"sequence\":" << status.dispatch.sequence
        << ",\"code\":\"" << EmsClusterNode::dispatchCodeName(status.dispatch.code) << "\""
        << ",\"requested\":[" << status.dispatch.requested.paKw << ','
        << status.dispatch.requested.pbKw << ',' << status.dispatch.requested.pcKw << ','
        << status.dispatch.requested.qaKvar << ',' << status.dispatch.requested.qbKvar << ','
        << status.dispatch.requested.qcKvar << ']'
        << ",\"accepted\":[" << status.dispatch.accepted.paKw << ','
        << status.dispatch.accepted.pbKw << ',' << status.dispatch.accepted.pcKw << ','
        << status.dispatch.accepted.qaKvar << ',' << status.dispatch.accepted.qbKvar << ','
        << status.dispatch.accepted.qcKvar << "]}"
        << ",\"reason\":\"" << escapeJson(status.reason) << "\",\"members\":[";
    for (std::size_t i = 0; i < status.members.size(); ++i) {
        const auto& member = status.members[i];
        out << (i == 0 ? "" : ",")
            << "{\"nodeId\":\"" << escapeJson(member.nodeId) << "\""
            << ",\"address\":\"" << escapeJson(member.address) << "\""
            << ",\"cabinetNo\":" << member.cabinetNo
            << ",\"lockedCabinetNo\":" << member.lockedCabinetNo
            << ",\"online\":" << (member.online ? "true" : "false")
            << ",\"lastSeenAgeMs\":" << member.lastSeenAgeMs
            << ",\"loadScore\":" << member.loadScore
            << ",\"compatible\":" << (member.compatible ? "true" : "false")
            << ",\"duplicateIdentity\":" << (member.duplicateIdentity ? "true" : "false")
            << ",\"metricsComplete\":" << (member.metricsComplete ? "true" : "false")
            << ",\"computeHealthy\":" << (member.computeHealthy ? "true" : "false")
            << ",\"capabilityFresh\":" << (member.capabilityFresh ? "true" : "false")
            << ",\"capabilityAgeMs\":" << member.capabilityAgeMs
            << ",\"capability\":{\"controlEnabled\":"
            << (member.capability.controlEnabled ? "true" : "false")
            << ",\"ready\":" << (member.capability.ready ? "true" : "false")
            << ",\"interlocked\":" << (member.capability.interlocked ? "true" : "false")
            << ",\"manualOverride\":" << (member.capability.manualOverride ? "true" : "false")
            << ",\"socPercent\":" << member.capability.socPercent
            << ",\"ratedActivePowerKw\":" << member.capability.ratedActivePowerKw
            << ",\"ratedApparentPowerKva\":" << member.capability.ratedApparentPowerKva
            << ",\"availableChargePowerKw\":" << member.capability.availableChargePowerKw
            << ",\"availableDischargePowerKw\":" << member.capability.availableDischargePowerKw
            << ",\"availableReactivePowerKvar\":" << member.capability.availableReactivePowerKvar
            << ",\"actual\":[" << member.capability.actual.paKw << ','
            << member.capability.actual.pbKw << ',' << member.capability.actual.pcKw << ','
            << member.capability.actual.qaKvar << ',' << member.capability.actual.qbKvar << ','
            << member.capability.actual.qcKvar << "]}"
            << ",\"dispatchAgeMs\":" << member.dispatchAgeMs
            << ",\"dispatch\":{\"valid\":" << (member.dispatch.valid ? "true" : "false")
            << ",\"sequence\":" << member.dispatch.sequence
            << ",\"code\":\"" << EmsClusterNode::dispatchCodeName(member.dispatch.code) << "\""
            << ",\"requested\":[" << member.dispatch.requested.paKw << ','
            << member.dispatch.requested.pbKw << ',' << member.dispatch.requested.pcKw << ','
            << member.dispatch.requested.qaKvar << ',' << member.dispatch.requested.qbKvar << ','
            << member.dispatch.requested.qcKvar << ']'
            << ",\"accepted\":[" << member.dispatch.accepted.paKw << ','
            << member.dispatch.accepted.pbKw << ',' << member.dispatch.accepted.pcKw << ','
            << member.dispatch.accepted.qaKvar << ',' << member.dispatch.accepted.qbKvar << ','
            << member.dispatch.accepted.qcKvar << "]}}";
    }
    out << "]}";
    return out.str();
}

}  // namespace edge_gateway
