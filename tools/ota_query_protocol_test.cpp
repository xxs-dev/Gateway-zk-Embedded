#include "edge_gateway/ota_query_protocol.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

using namespace edge_gateway;

void require(bool condition) {
    if (!condition) throw std::runtime_error("original-job query protocol mismatch");
}

template <typename F>
void rejected(F&& action) {
    bool invalid = false;
    try { action(); }
    catch (const std::invalid_argument&) { invalid = true; }
    require(invalid);
}

int main() {
    const std::string digest(64, 'a');
    OtaQueryChallenge challenge{"JOB_1", "GW_TEST", std::string(64, 'b'), "2.0.0",
        digest, 13, std::string(64, 'c'), 1700000000000, 1700000060000, ""};
    require(ota_query_protocol::challengeDescriptor(challenge) ==
        "edge-original-job-query-request-v1\njobId=JOB_1\nmachineCode=GW_TEST\nnonce=" +
        std::string(64, 'b') + "\nversion=2.0.0\nartifactSha256=" + digest +
        "\nartifactSize=13\ncandidateSha256=" + std::string(64, 'c') +
        "\nissuedAtMs=1700000000000\nexpiresAtMs=1700000060000\n");
    OtaQueryObservation observation{"JOB_1", "GW_TEST", std::string(64, 'b'),
        "HEALTHY_APPLIED", "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee", "2.0.0",
        digest, 13, std::string(64, 'c'), "OTA_" + std::string(32, 'd'),
        std::string(64, 'e'), std::string(64, 'f'), 1700000001000, ""};
    require(ota_query_protocol::observationDescriptor(observation) ==
        "edge-original-job-query-response-v1\njobId=JOB_1\nmachineCode=GW_TEST\nnonce=" +
        std::string(64, 'b') + "\noutcome=HEALTHY_APPLIED\noccurrenceId=" +
        "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee\nversion=2.0.0\nartifactSha256=" +
        digest + "\nartifactSize=13\ncandidateSha256=" + std::string(64, 'c') +
        "\ntransaction=OTA_" + std::string(32, 'd') + "\nintentSha256=" +
        std::string(64, 'e') + "\nhealthSha256=" + std::string(64, 'f') +
        "\nhealthTs=1700000001000\n");
    observation.transaction = "OTA_" + std::string(32, 'O');
    rejected([&] { ota_query_protocol::observationDescriptor(observation); });
    observation.transaction.clear();
    rejected([&] { ota_query_protocol::observationDescriptor(observation); });
    observation.transaction = "OTA_" + std::string(32, 'd');
    observation.candidateSha256.clear();
    rejected([&] { ota_query_protocol::observationDescriptor(observation); });
    observation.outcome = "REJECTED_UNCHANGED";
    observation.transaction.clear();
    ota_query_protocol::observationDescriptor(observation);
    observation.artifactSize = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
    rejected([&] { ota_query_protocol::observationDescriptor(observation); });
    challenge.artifactSize = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
    rejected([&] { ota_query_protocol::challengeDescriptor(challenge); });
    std::cout << "original-job query canonical protocol PASS\n";
}
