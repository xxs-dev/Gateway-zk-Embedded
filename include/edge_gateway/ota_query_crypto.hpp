#pragma once

#include "edge_gateway/ota_query_protocol.hpp"

namespace edge_gateway::ota_query_crypto {

bool verifyChallenge(const OtaQueryChallenge& challenge, const std::string& publicKeyPath);
std::string signObservation(const OtaQueryObservation& observation,
                            const std::string& privateKeyPath);

}  // namespace edge_gateway::ota_query_crypto
