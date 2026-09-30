#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include "edge_gateway/system_monitor_direct_maintenance.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.compare(0, prefix.size(), prefix) == 0;
}

void verifyMaintenanceCredential() {
    namespace dm = edge_gateway::system_monitor_direct_maintenance;
    const auto root = std::string("/tmp/direct-maintenance-contract-") + std::to_string(::getpid());
    edge_gateway::SystemMonitorConfig::DirectMaintenanceConfig config;
    config.credentialFile = root + "-credential.json";
    config.authStateFile = root + "-auth-state.json";
    config.listenHosts = {"127.0.0.1"};
    config.maxAuthFailures = 3;
    std::remove(config.credentialFile.c_str());
    std::remove(config.authStateFile.c_str());

    bool rejectedEmptyCidrs = false;
    try {
        dm::validateListenerConfig(config);
    } catch (const std::exception&) {
        rejectedEmptyCidrs = true;
    }
    require(rejectedEmptyCidrs, "empty allowedClientCidrs must be refused by default");
    config.allowAnyClient = true;
    dm::validateListenerConfig(config);

    const std::string legacyHash = "5c2358ee05dbd6bc6d52939f51a45c315533ad9191eecf1631fd788ec8ab76b3";
    const std::string legacyBody = std::string("{\"passwordSha256\":\"") + legacyHash + "\"}";
    require(!dm::credentialProvisioned(config), "credential should not exist yet");
    require(!dm::verifyPasswordSha256(config, legacyHash), "fixed legacy hash must be rejected by default");
    require(startsWith(dm::handleHttpRequest(config, "POST", "/api/v1/auth/bootstrap-login", "", "", legacyBody, "10.0.0.1"),
                       "HTTP/1.1 503"),
            "sensitive endpoints must be refused while no credential is provisioned");
    config.allowLegacyFixedPassword = true;
    require(dm::verifyPasswordSha256(config, legacyHash), "legacy hash should be accepted only when explicitly allowed");
    config.allowLegacyFixedPassword = false;

    std::string error;
    require(!dm::provisionCredential(config, "short", &error), "short maintenance passwords must be rejected");
    require(dm::provisionCredential(config, "Maint#2026-secret", &error), "credential provisioning failed: " + error);
    require(dm::credentialProvisioned(config), "credential should be provisioned");
    const auto good = dm::passwordSha256Hex("Maint#2026-secret");
    require(good.size() == 64, "client passwordSha256 helper is broken");
    require(dm::verifyPasswordSha256(config, good), "provisioned password was rejected");
    require(!dm::verifyPasswordSha256(config, dm::passwordSha256Hex("wrong-password")), "wrong password was accepted");
    require(!dm::verifyPasswordSha256(config, legacyHash), "fixed legacy hash must not unlock a provisioned device");

    const std::string goodBody = std::string("{\"passwordSha256\":\"") + good + "\"}";
    const auto login = dm::handleHttpRequest(config, "POST", "/api/v1/auth/bootstrap-login", "", "", goodBody, "10.0.0.2");
    require(startsWith(login, "HTTP/1.1 200") && login.find("\"sensitiveApiEnabled\":true") != std::string::npos,
            "provisioned login should succeed and enable the sensitive API");
    // Authorized request reaches the handler (which then fails on the empty test app config).
    const auto authorized = dm::handleHttpRequest(config, "POST", "/api/v1/config/delete", "", "", goodBody, "10.0.0.2");
    require(!startsWith(authorized, "HTTP/1.1 401") && !startsWith(authorized, "HTTP/1.1 403") &&
                !startsWith(authorized, "HTTP/1.1 503"),
            "valid credential did not pass the sensitive endpoint gate");

    const std::string badBody = std::string("{\"passwordSha256\":\"") + dm::passwordSha256Hex("nope-nope") + "\"}";
    for (int attempt = 0; attempt < 3; ++attempt) {
        require(startsWith(dm::handleHttpRequest(config, "POST", "/api/v1/config/delete", "", "", badBody, "10.0.0.3"),
                           "HTTP/1.1 401"),
                "wrong password should be unauthorized");
    }
    require(startsWith(dm::handleHttpRequest(config, "POST", "/api/v1/config/delete", "", "", goodBody, "10.0.0.3"),
                       "HTTP/1.1 429"),
            "client must be locked out after repeated failures");

    std::remove(config.authStateFile.c_str());
    require(startsWith(dm::handleHttpRequest(config, "POST", "/api/v1/config/delete", "", "", goodBody, "10.0.0.4"),
                       "HTTP/1.1 403"),
            "sensitive API must stay disabled while authState.sensitiveApiEnabled is false");
    std::remove(config.credentialFile.c_str());
}

}  // namespace

int main() {
    try {
        const auto json = edge_gateway::system_monitor_direct_maintenance::otaCapabilitiesJson();
        require(json.find("\"supportedPackageTypes\":[\"config\",\"full\",\"scada\"]") != std::string::npos,
                "OTA package capabilities are incomplete");
        require(json.find("\"emsLogic\":{") != std::string::npos,
                "emsLogic capability object is missing");
        require(json.find("\"editorSourceSchema\":\"2.x\"") != std::string::npos,
                "editor source schema capability is incorrect");
        require(json.find("\"runtimeSchema\":\"2.x\"") != std::string::npos,
                "runtime schema capability is incorrect");
        require(json.find("\"compilerContract\":\"GraphEmsV2/direct\"") != std::string::npos,
                "compiler contract capability is incorrect");
        require(json.find("\"executesEditorSource\":true") != std::string::npos,
                "direct V2 execution capability is incorrect");
        verifyMaintenanceCredential();
        std::cout << "system_monitor_direct_maintenance_contract_test passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "system_monitor_direct_maintenance_contract_test failed: " << ex.what() << "\n";
        return 1;
    }
}
