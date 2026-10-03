#pragma once

#include <stdexcept>
#include "edge_gateway/ota_query_protocol.hpp"

namespace edge_gateway::ota_query_crypto {

enum class Failure { Key, Library, Operation };

class Error : public std::runtime_error {
public:
    explicit Error(Failure failure)
        : std::runtime_error("OTA query crypto unavailable"), failure_(failure) {}
    Failure failure() const { return failure_; }
private:
    Failure failure_;
};

bool verifyChallenge(const OtaQueryChallenge& challenge, const std::string& publicKeyPath);
std::string signObservation(const OtaQueryObservation& observation,
                            const std::string& privateKeyPath);

}  // namespace edge_gateway::ota_query_crypto
