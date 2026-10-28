#include "edge_gateway/ota_query_crypto.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc == 3 && (std::string(argv[1]) == "--reject-private-key" ||
                      std::string(argv[1]) == "--reject-library")) {
        using namespace edge_gateway;
        const auto expected = std::string(argv[1]) == "--reject-private-key"
            ? ota_query_crypto::Failure::Key : ota_query_crypto::Failure::Library;
        OtaQueryObservation observation;
        observation.jobId = "JOB_1"; observation.machineCode = "GW_TEST";
        observation.nonce = std::string(64, 'b'); observation.outcome = "UNKNOWN";
        try { (void)ota_query_crypto::signObservation(observation, argv[2]); }
        catch (const ota_query_crypto::Error& error) {
            if (error.failure() != expected) throw;
            std::cout << "expected rejection classified\n";
            return 0;
        }
        throw std::runtime_error("unsafe key or unavailable library was accepted");
    }
    if (argc >= 2 && std::string(argv[1]) == "--full") {
        if (argc != 5) throw std::runtime_error("full bridge requires platform public, device private, full request signature");
        edge_gateway::OtaFullQueryChallenge full{"QUERY_JOB", "GW_QUERY", std::string(64, 'b'), "full",
            "2.0.0", std::string(64, 'a'), 13, 1700000000000, 1700000060000, ""};
        std::ifstream input(argv[4]); input >> full.signature;
        if (!edge_gateway::ota_query_crypto::verifyChallenge(full, argv[2]))
            throw std::runtime_error("signed full platform challenge was rejected");
        auto changed = full; changed.nonce = std::string(64, 'd');
        if (edge_gateway::ota_query_crypto::verifyChallenge(changed, argv[2]))
            throw std::runtime_error("tampered full challenge was accepted");
        edge_gateway::OtaQueryChallenge old{full.jobId, full.machineCode, full.nonce, full.version,
            full.artifactSha256, full.artifactSize, std::string(64, 'c'), full.issuedAtMs, full.expiresAtMs, full.signature};
        if (edge_gateway::ota_query_crypto::verifyChallenge(old, argv[2]))
            throw std::runtime_error("full signature was accepted in old query domain");
        edge_gateway::OtaFullQueryObservation reply;
        reply.jobId = full.jobId; reply.machineCode = full.machineCode; reply.nonce = full.nonce;
        reply.packageType = full.packageType; reply.version = full.version;
        reply.artifactSha256 = full.artifactSha256; reply.artifactSize = full.artifactSize;
        reply.outcome = "UNKNOWN";
        reply.signature = edge_gateway::ota_query_crypto::signObservation(reply, argv[3]);
        std::cout << edge_gateway::ota_full_query_protocol::encodeObservation(reply) << '\n';
        return 0;
    }
    if (argc != 4) throw std::runtime_error("expected platform public, device private, request signature");
    edge_gateway::OtaQueryChallenge challenge{"JOB_1", "GW_TEST", std::string(64, 'b'),
        "2.0.0", std::string(64, 'a'), 13, std::string(64, 'c'),
        1700000000000, 1700000060000, ""};
    std::ifstream input(argv[3]);
    input >> challenge.signature;
    if (!edge_gateway::ota_query_crypto::verifyChallenge(challenge, argv[1]))
        throw std::runtime_error("signed platform challenge was rejected");
    auto tampered = challenge;
    tampered.nonce = std::string(64, 'd');
    if (edge_gateway::ota_query_crypto::verifyChallenge(tampered, argv[1]))
        throw std::runtime_error("tampered platform challenge was accepted");
    const std::string publicLink = std::string(argv[1]) + ".link";
    if (symlink(argv[1], publicLink.c_str()) != 0)
        throw std::runtime_error("cannot create synthetic public-key symlink");
    bool linkedPublicRejectedAsKey = false;
    try { (void)edge_gateway::ota_query_crypto::verifyChallenge(challenge, publicLink); }
    catch (const edge_gateway::ota_query_crypto::Error& error) {
        linkedPublicRejectedAsKey = error.failure() == edge_gateway::ota_query_crypto::Failure::Key;
    }
    unlink(publicLink.c_str());
    if (!linkedPublicRejectedAsKey) throw std::runtime_error("symlinked public key was not classified");
    bool missingPublicRejectedAsKey = false;
    try { (void)edge_gateway::ota_query_crypto::verifyChallenge(challenge, publicLink); }
    catch (const edge_gateway::ota_query_crypto::Error& error) {
        missingPublicRejectedAsKey = error.failure() == edge_gateway::ota_query_crypto::Failure::Key;
    }
    if (!missingPublicRejectedAsKey) throw std::runtime_error("missing public key was not classified");
    auto malformedSignature = challenge;
    malformedSignature.signature = "not-base64";
    if (edge_gateway::ota_query_crypto::verifyChallenge(malformedSignature, argv[1]))
        throw std::runtime_error("malformed signature was accepted");
    edge_gateway::OtaQueryObservation observation{"JOB_1", "GW_TEST", std::string(64, 'b'),
        "HEALTHY_APPLIED", "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee", "2.0.0",
        std::string(64, 'a'), 13, std::string(64, 'c'), "OTA_" + std::string(32, 'd'),
        std::string(64, 'e'), std::string(64, 'f'), 1700000001000, ""};
    const auto signedObservation = edge_gateway::ota_query_crypto::signObservation(observation, argv[2]);
    if (chmod(argv[2], 0644) != 0) throw std::runtime_error("cannot change synthetic private-key mode");
    bool unsafePrivateAccepted = false;
    try { edge_gateway::ota_query_crypto::signObservation(observation, argv[2]); }
    catch (const std::exception&) { unsafePrivateAccepted = true; }
    if (chmod(argv[2], 0600) != 0 || !unsafePrivateAccepted)
        throw std::runtime_error("world-readable private key was accepted");
    const std::string privateLink = std::string(argv[2]) + ".link";
    if (symlink(argv[2], privateLink.c_str()) != 0)
        throw std::runtime_error("cannot create synthetic private-key symlink");
    bool linkedPrivateRejected = false;
    try { edge_gateway::ota_query_crypto::signObservation(observation, privateLink); }
    catch (const std::exception&) { linkedPrivateRejected = true; }
    unlink(privateLink.c_str());
    if (!linkedPrivateRejected) throw std::runtime_error("symlinked private key was accepted");
    std::cout << signedObservation << '\n';
}
