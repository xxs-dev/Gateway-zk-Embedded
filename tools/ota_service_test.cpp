#include "edge_gateway/ota_service.hpp"

#include <cstdlib>
#include <fstream>
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

bool containsText(const std::string& path, const std::string& expected) {
    std::ifstream input(path.c_str());
    if (!input.is_open()) {
        return false;
    }
    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return content.find(expected) != std::string::npos;
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

    OtaConfig markerConfig;
    markerConfig.enabled = true;
    markerConfig.downloadDir = "/tmp/gateway-ota-marker/downloads";
    markerConfig.stagingDir = "/tmp/gateway-ota-marker/staging";
    markerConfig.backupDir = "/tmp/gateway-ota-marker/backup";
    markerConfig.applyScript = "/tmp/gateway-ota-marker/apply-ok.sh";
    markerConfig.rollbackScript.clear();
    markerConfig.checksumRequired = false;
    markerConfig.currentVersion = "1.0.0";
    markerConfig.packageType = "tar.gz";
    markerConfig.minFreeBytes = 0;
    OtaService markerService(markerConfig);
    OtaRequest markerRequest;
    markerRequest.jobId = "JOB_MARKER_001";
    markerRequest.version = "2.0.0";
    markerRequest.artifactUrl = "/tmp/gateway-ota-marker/source.tar.gz";
    {
        std::system("rm -rf /tmp/gateway-ota-marker");
        std::system("mkdir -p /tmp/gateway-ota-marker");
        std::ofstream artifact(markerRequest.artifactUrl.c_str(), std::ios::binary | std::ios::trunc);
        artifact << "payload";
        std::ofstream script(markerConfig.applyScript.c_str(), std::ios::trunc);
        script << "#!/bin/sh\nexit 0\n";
    }
    OtaReply reply;
    OtaStatus status;
    markerService.execute(markerRequest, "GW_TEST", 1770000000000LL, &reply, &status, nullptr);
    const std::string markerPath = markerConfig.stagingDir + "/current_version.txt";
    require(containsText(markerPath, "jobId=JOB_MARKER_001"), "version marker should include jobId");
    require(containsText(markerPath, "backupDir=/tmp/gateway-ota-marker/backup/JOB_MARKER_001"), "version marker should include job backup dir");
    require(containsText(markerPath, "workDir=/tmp/gateway-ota-marker/staging/JOB_MARKER_001"), "version marker should include work dir");

    std::cout << "ota_service_test passed" << std::endl;
    return 0;
}
