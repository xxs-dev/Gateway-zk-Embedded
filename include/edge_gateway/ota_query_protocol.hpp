#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#include "edge_gateway/json_value.hpp"
#include "../../src/ota_query_sha256.hpp"

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

struct OtaFullQueryChallenge {
    std::string jobId, machineCode, nonce, packageType, version, artifactSha256;
    std::uint64_t artifactSize = 0;
    std::int64_t issuedAtMs = 0, expiresAtMs = 0;
    std::string signature;
};

struct OtaFullQueryObservation {
    std::string jobId, machineCode, nonce, packageType, outcome, occurrenceId;
    std::string version, artifactSha256;
    std::uint64_t artifactSize = 0;
    std::string candidateSha256, transaction, parentSha256, intentSha256, healthSha256;
    std::int64_t healthTs = 0;
    std::string signature;
};

namespace ota_full_query_protocol {

constexpr const char* requestFormat = "edge-original-full-job-query-request-v1";
constexpr const char* responseFormat = "edge-original-full-job-query-response-v1";
constexpr std::size_t maxPayloadBytes = 8192;

inline void require(bool condition) {
    if (!condition) throw std::invalid_argument("invalid full original-job query protocol value");
}

inline void identity(const std::string& job, const std::string& machine, const std::string& nonce,
                     const std::string& type, const std::string& version,
                     const std::string& sha, std::uint64_t size) {
    using ota_query_protocol::matches;
    require(matches(job, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-", 1, 128) &&
        matches(machine, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._:-", 1, 128) &&
        ota_query_protocol::digest(nonce) && type == "full" &&
        matches(version, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-", 1, 128) &&
        ota_query_protocol::digest(sha) && size > 0 &&
        size <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
}

inline std::string transaction(const std::string& job, const std::string& occurrence) {
    require(ota_query_protocol::matches(job,
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-", 1, 128) &&
        ota_query_protocol::occurrence(occurrence));
    return "OTA_" + ota_query_detail::sha256Hex(job + occurrence).substr(0, 32);
}

inline void validate(const OtaFullQueryChallenge& value) {
    identity(value.jobId, value.machineCode, value.nonce, value.packageType,
             value.version, value.artifactSha256, value.artifactSize);
    require(value.issuedAtMs > 0 && value.expiresAtMs > value.issuedAtMs &&
        value.expiresAtMs - value.issuedAtMs <= 300000);
}

inline void validate(const OtaFullQueryObservation& value) {
    identity(value.jobId, value.machineCode, value.nonce, value.packageType,
             value.version, value.artifactSha256, value.artifactSize);
    require(value.outcome == "UNKNOWN" || value.outcome == "HEALTHY_APPLIED" ||
        value.outcome == "FAILED_ROLLED_BACK" || value.outcome == "REJECTED_UNCHANGED");
    if (value.outcome == "UNKNOWN") {
        require(value.occurrenceId.empty() && value.candidateSha256.empty() && value.transaction.empty() &&
            value.parentSha256.empty() && value.intentSha256.empty() && value.healthSha256.empty() && value.healthTs == 0);
        return;
    }
    require(ota_query_protocol::occurrence(value.occurrenceId) &&
        ota_query_protocol::digest(value.intentSha256) && ota_query_protocol::digest(value.healthSha256) && value.healthTs > 0);
    if (value.outcome == "REJECTED_UNCHANGED") {
        require(value.candidateSha256.empty() && value.transaction.empty() && value.parentSha256.empty());
    } else {
        require(ota_query_protocol::digest(value.candidateSha256) && ota_query_protocol::digest(value.parentSha256) &&
            value.transaction == transaction(value.jobId, value.occurrenceId));
    }
}

inline std::string challengeDescriptor(const OtaFullQueryChallenge& value) {
    validate(value);
    return std::string(requestFormat) + "\njobId=" + value.jobId + "\nmachineCode=" + value.machineCode +
        "\nnonce=" + value.nonce + "\npackageType=" + value.packageType + "\nversion=" + value.version +
        "\nartifactSha256=" + value.artifactSha256 + "\nartifactSize=" + std::to_string(value.artifactSize) +
        "\nissuedAtMs=" + std::to_string(value.issuedAtMs) + "\nexpiresAtMs=" + std::to_string(value.expiresAtMs) + "\n";
}

inline std::string observationDescriptor(const OtaFullQueryObservation& value) {
    validate(value);
    return std::string(responseFormat) + "\njobId=" + value.jobId + "\nmachineCode=" + value.machineCode +
        "\nnonce=" + value.nonce + "\npackageType=" + value.packageType + "\noutcome=" + value.outcome +
        "\noccurrenceId=" + value.occurrenceId + "\nversion=" + value.version + "\nartifactSha256=" + value.artifactSha256 +
        "\nartifactSize=" + std::to_string(value.artifactSize) + "\ncandidateSha256=" + value.candidateSha256 +
        "\ntransaction=" + value.transaction + "\nparentSha256=" + value.parentSha256 + "\nintentSha256=" + value.intentSha256 +
        "\nhealthSha256=" + value.healthSha256 + "\nhealthTs=" + std::to_string(value.healthTs) + "\n";
}

inline void requireCanonicalSignature(const std::string& value) {
    require(!value.empty() && value.size() <= maxPayloadBytes && value.size() % 4 == 0);
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto padding = (value.back() == '=') + (value[value.size() - 2] == '=');
    for (std::size_t i = 0; i < value.size() - padding; ++i)
        require(alphabet.find(value[i]) != std::string::npos);
    for (std::size_t i = value.size() - padding; i < value.size(); ++i) require(value[i] == '=');
    if (padding) {
        const auto last = alphabet.find(value[value.size() - padding - 1]);
        require(last != std::string::npos && (last & (padding == 2 ? 15 : 3)) == 0);
    }
}

inline std::int64_t decimal(const std::string& value) {
    require(!value.empty() && value.size() <= 19 && (value.size() == 1 || value.front() != '0'));
    std::int64_t result = 0;
    for (char c : value) {
        require(c >= '0' && c <= '9');
        require(result <= (std::numeric_limits<std::int64_t>::max() - (c - '0')) / 10);
        result = result * 10 + c - '0';
    }
    return result;
}

inline OtaFullQueryChallenge parseChallenge(const std::string& payload) {
    require(!payload.empty() && payload.size() <= maxPayloadBytes);
    const auto root = json::JsonParser(payload, 4, 32).parse();
    require(root.isObject() && root.asObject().values.size() == 11);
    const char* fields[] = {"format", "jobId", "machineCode", "nonce", "packageType", "version",
        "artifactSha256", "artifactSize", "issuedAtMs", "expiresAtMs", "signature"};
    for (const auto* field : fields) {
        std::size_t count = 0;
        for (const auto& member : root.asObject().values) {
            if (member.key == field) { require(member.value && member.value->isString()); ++count; }
        }
        require(count == 1);
    }
    const auto text = [&](const char* name) { return root.find(name)->asString(); };
    require(text("format") == requestFormat);
    OtaFullQueryChallenge value;
    value.jobId = text("jobId"); value.machineCode = text("machineCode"); value.nonce = text("nonce");
    value.packageType = text("packageType"); value.version = text("version"); value.artifactSha256 = text("artifactSha256");
    value.artifactSize = static_cast<std::uint64_t>(decimal(text("artifactSize")));
    value.issuedAtMs = decimal(text("issuedAtMs")); value.expiresAtMs = decimal(text("expiresAtMs"));
    value.signature = text("signature");
    validate(value);
    requireCanonicalSignature(value.signature);
    return value;
}

inline std::string encodeObservation(const OtaFullQueryObservation& value) {
    validate(value);
    requireCanonicalSignature(value.signature);
    // Validated identifiers/digests/decimals/Base64 need no JSON escaping.
    const auto field = [](const std::string& v) { return "\"" + v + "\""; };
    const auto encoded = std::string("{\"format\":") + field(responseFormat) + ",\"jobId\":" + field(value.jobId) +
        ",\"machineCode\":" + field(value.machineCode) + ",\"nonce\":" + field(value.nonce) +
        ",\"packageType\":" + field(value.packageType) + ",\"outcome\":" + field(value.outcome) +
        ",\"occurrenceId\":" + field(value.occurrenceId) + ",\"version\":" + field(value.version) +
        ",\"artifactSha256\":" + field(value.artifactSha256) + ",\"artifactSize\":" + field(std::to_string(value.artifactSize)) +
        ",\"candidateSha256\":" + field(value.candidateSha256) + ",\"transaction\":" + field(value.transaction) +
        ",\"parentSha256\":" + field(value.parentSha256) + ",\"intentSha256\":" + field(value.intentSha256) +
        ",\"healthSha256\":" + field(value.healthSha256) + ",\"healthTs\":" + field(std::to_string(value.healthTs)) +
        ",\"signature\":" + field(value.signature) + "}";
    require(encoded.size() <= maxPayloadBytes);
    return encoded;
}

}  // namespace ota_full_query_protocol
}  // namespace edge_gateway
