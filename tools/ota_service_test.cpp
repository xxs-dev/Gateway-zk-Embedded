#include "edge_gateway/ota_service.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireRejected(const edge_gateway::OtaService& service, edge_gateway::OtaRequest request, const std::string& expected) {
    std::string error;
    require(!service.validateRequest(request, &error), "ota request should be rejected");
    if (error.find(expected) == std::string::npos) {
        throw std::runtime_error("unexpected ota validation error: " + error);
    }
}

}  // namespace

int main() {
    using namespace edge_gateway;

    OtaConfig config;
    config.enabled = true;
    config.downloadDir = "/tmp/gateway-ota-test";
    config.packageType = "tar.gz";
    OtaService service(config);

    OtaRequest ok;
    ok.jobId = "JOB_20260522_001";
    ok.version = "config-20260522.1";
    ok.artifactUrl = "http://127.0.0.1:8090/api/config/ota/config-packages/gateway-config-GW0001-config-1.tar.gz?apiToken=secret";
    ok.sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    std::string error;
    require(service.validateRequest(ok, &error), "valid ota request should pass");

    auto tooLarge = ok;
    tooLarge.size = config.maxArtifactBytes + 1;
    requireRejected(service, tooLarge, "ota artifact size exceeds maxArtifactBytes");

    auto badJob = ok;
    badJob.jobId = "JOB;reboot";
    requireRejected(service, badJob, "invalid ota jobId");

    auto badVersion = ok;
    badVersion.version = "../config";
    requireRejected(service, badVersion, "invalid ota version");

    auto badSha = ok;
    badSha.sha256 = "not-a-sha";
    requireRejected(service, badSha, "invalid ota sha256");

    auto badName = ok;
    badName.version.clear();
    badName.artifactUrl = "http://127.0.0.1:8090/download/../bad.tar.gz";
    requireRejected(service, badName, "invalid ota artifactUrl");

    auto badQuery = ok;
    badQuery.artifactUrl = "http://127.0.0.1:8090/api/config/ota/config-packages/gateway.tar.gz?otaToken=abc%0a";
    requireRejected(service, badQuery, "invalid ota artifactUrl");

    auto localTraversal = ok;
    localTraversal.version.clear();
    localTraversal.artifactUrl = "../gateway.tar.gz";
    requireRejected(service, localTraversal, "invalid ota artifactUrl");

    std::cout << "ota_service_test passed" << std::endl;
    return 0;
}
