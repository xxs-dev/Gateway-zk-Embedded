#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace edge_gateway {

struct OtaQueryChallenge {
    std::string jobId;
    std::string machineCode;
    std::string nonce;
    std::string version;
    std::string artifactSha256;
    std::uint64_t artifactSize = 0;
    std::string candidateSha256;
    std::int64_t issuedAtMs = 0;
    std::int64_t expiresAtMs = 0;
    std::string signature;
};

struct OtaQueryObservation {
    std::string jobId;
    std::string machineCode;
    std::string nonce;
    std::string outcome;
    std::string occurrenceId;
    std::string version;
    std::string artifactSha256;
    std::uint64_t artifactSize = 0;
    std::string candidateSha256;
    std::string transaction;
    std::string intentSha256;
    std::string healthSha256;
    std::int64_t healthTs = 0;
    std::string signature;
};

namespace ota_query_protocol {

inline bool matches(const std::string& value, const std::string& allowed,
                    std::size_t minimum, std::size_t maximum) {
    if (value.size() < minimum || value.size() > maximum) return false;
    for (char c : value)
        if (allowed.find(c) == std::string::npos) return false;
    return true;
}

inline bool digest(const std::string& value) {
    return matches(value, "0123456789abcdef", 64, 64);
}

inline bool occurrence(const std::string& value) {
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (std::string("0123456789abcdef").find(value[i]) == std::string::npos) return false;
    }
    return true;
}

inline void validate(const OtaQueryChallenge& value) {
    if (!matches(value.jobId, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-", 1, 128) ||
        !matches(value.machineCode, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._:-", 1, 128) ||
        !matches(value.nonce, "0123456789abcdef", 64, 64) ||
        !matches(value.version, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-", 1, 128) ||
        !digest(value.artifactSha256) || value.artifactSize == 0 ||
        value.artifactSize > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        !digest(value.candidateSha256) || value.issuedAtMs <= 0 ||
        value.expiresAtMs <= value.issuedAtMs ||
        value.expiresAtMs - value.issuedAtMs > 300000)
        throw std::invalid_argument("invalid OTA query challenge");
}

inline void validate(const OtaQueryObservation& value) {
    if (!matches(value.jobId, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-", 1, 128) ||
        !matches(value.machineCode, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._:-", 1, 128) ||
        !matches(value.nonce, "0123456789abcdef", 64, 64) ||
        (value.outcome != "UNKNOWN" && value.outcome != "HEALTHY_APPLIED" &&
            value.outcome != "FAILED_ROLLED_BACK" && value.outcome != "REJECTED_UNCHANGED"))
        throw std::invalid_argument("invalid OTA query observation");
    if (value.outcome == "UNKNOWN") {
        if (!value.occurrenceId.empty() || !value.version.empty() || !value.artifactSha256.empty() ||
            value.artifactSize != 0 || !value.candidateSha256.empty() || !value.transaction.empty() ||
            !value.intentSha256.empty() || !value.healthSha256.empty() || value.healthTs != 0)
            throw std::invalid_argument("partial unknown OTA query observation");
        return;
    }
    if (!occurrence(value.occurrenceId) ||
        !matches(value.version, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-", 1, 128) ||
        !digest(value.artifactSha256) || value.artifactSize == 0 ||
        value.artifactSize > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        !digest(value.intentSha256) || !digest(value.healthSha256) || value.healthTs <= 0 ||
        ((value.outcome == "HEALTHY_APPLIED" || value.outcome == "FAILED_ROLLED_BACK") &&
            (!digest(value.candidateSha256) || value.transaction.size() != 36 ||
                value.transaction.substr(0, 4) != "OTA_" ||
                !matches(value.transaction.substr(4), "0123456789abcdef", 32, 32))) ||
        (value.outcome == "REJECTED_UNCHANGED" &&
            (!value.candidateSha256.empty() || !value.transaction.empty())))
        throw std::invalid_argument("invalid resolved OTA query observation");
}

inline std::string challengeDescriptor(const OtaQueryChallenge& value) {
    validate(value);
    return "edge-original-job-query-request-v1\njobId=" + value.jobId +
        "\nmachineCode=" + value.machineCode + "\nnonce=" + value.nonce +
        "\nversion=" + value.version + "\nartifactSha256=" + value.artifactSha256 +
        "\nartifactSize=" + std::to_string(value.artifactSize) +
        "\ncandidateSha256=" + value.candidateSha256 +
        "\nissuedAtMs=" + std::to_string(value.issuedAtMs) +
        "\nexpiresAtMs=" + std::to_string(value.expiresAtMs) + "\n";
}

inline std::string observationDescriptor(const OtaQueryObservation& value) {
    validate(value);
    return "edge-original-job-query-response-v1\njobId=" + value.jobId +
        "\nmachineCode=" + value.machineCode + "\nnonce=" + value.nonce +
        "\noutcome=" + value.outcome + "\noccurrenceId=" + value.occurrenceId +
        "\nversion=" + value.version + "\nartifactSha256=" + value.artifactSha256 +
        "\nartifactSize=" + std::to_string(value.artifactSize) +
        "\ncandidateSha256=" + value.candidateSha256 +
        "\ntransaction=" + value.transaction + "\nintentSha256=" + value.intentSha256 +
        "\nhealthSha256=" + value.healthSha256 + "\nhealthTs=" +
        std::to_string(value.healthTs) + "\n";
}

}  // namespace ota_query_protocol
}  // namespace edge_gateway
