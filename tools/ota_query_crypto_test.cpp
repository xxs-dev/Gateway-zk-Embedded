#include "edge_gateway/ota_query_crypto.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char** argv) {
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
    const bool linkedPublicAccepted = edge_gateway::ota_query_crypto::verifyChallenge(challenge, publicLink);
    unlink(publicLink.c_str());
    if (linkedPublicAccepted) throw std::runtime_error("symlinked public key was accepted");
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
