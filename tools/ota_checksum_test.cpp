#include "edge_gateway/ota_service.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace edge_gateway {
struct OtaChecksumTestAccess {
    static void verifyChecksum(const OtaService& service, const OtaRequest& request, const std::string& path) {
        service.verifyChecksum(request, path);
    }
};
}

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void requireRejected(const edge_gateway::OtaService& service, const edge_gateway::OtaRequest& request,
                     const std::string& expected) {
    std::string error;
    require(!service.validateRequest(request, &error), "ota request should be rejected");
    if (error != expected) throw std::runtime_error("unexpected ota validation error: " + error);
}

struct ChecksumDirectory {
    std::string path;
    ChecksumDirectory() {
        char name[] = "/tmp/ota-checksum-test-XXXXXX";
        const auto* created = mkdtemp(name);
        require(created != nullptr, "cannot create isolated checksum fixture");
        path = created;
    }
    ~ChecksumDirectory() {
        std::remove((path + "/artifact.tar.gz").c_str());
        rmdir(path.c_str());
    }
};

void checksumTests() {
    using namespace edge_gateway;
    ChecksumDirectory directory;
    const auto artifactPath = directory.path + "/artifact.tar.gz";
    { std::ofstream artifact(artifactPath, std::ios::binary); artifact << "abc"; }
    OtaConfig config;
    config.enabled = true;
    config.checksumRequired = true;
    // A regular file blocks downloads even if a broken preflight accepts the request.
    config.downloadDir = artifactPath + "/downloads";
    config.stagingDir = artifactPath;
    config.backupDir = directory.path + "/backup";
    config.applyScript.clear();
    config.rollbackScript.clear();
    config.autoReboot = false;
    config.minFreeBytes = 0;
    config.upgradeTimeoutSec = 5;
    OtaService service(config);
    auto optionalConfig = config;
    optionalConfig.checksumRequired = false;
    OtaService optional(optionalConfig);
    OtaRequest request;
    request.jobId = "CHECKSUM_TEST";
    request.version = "checksum-test";
    request.artifactUrl = artifactPath;
    request.sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    int failures = 0;
    const auto check = [&](const char* name, const std::function<void()>& test) {
        try {
            test();
            std::cout << "ota checksum " << name << " passed\n";
        } catch (const std::exception& ex) {
            ++failures;
            std::cerr << "ota checksum " << name << " failed: " << ex.what() << "\n";
        }
    };
    const auto expectError = [](const std::function<void()>& operation, const std::string& expected) {
        std::string error;
        try { operation(); } catch (const std::exception& ex) { error = ex.what(); }
        if (error != expected) throw std::runtime_error("expected '" + expected + "', got '" + error + "'");
    };
    auto missing = request;
    missing.sha256.clear();
    check("required-missing-validation", [&] {
        requireRejected(service, missing, "ota sha256 is required");
    });
    check("required-missing-before-download", [&] {
        OtaReply reply;
        OtaStatus status;
        bool published = false;
        expectError([&] {
            service.execute(missing, "GW_TEST", 1000, &reply, &status,
                [&](const OtaStatus&) { published = true; });
        }, "ota sha256 is required");
        require(!reply.accepted && status.stage.empty() && !published,
            "missing checksum must fail before acceptance or status publication");
        require(access(config.downloadDir.c_str(), F_OK) != 0,
            "missing checksum must fail before creating downloads");
    });
    check("required-missing-verification", [&] {
        expectError([&] { OtaChecksumTestAccess::verifyChecksum(service, missing, artifactPath); },
            "ota sha256 is required");
    });
    check("invalid-format", [&] {
        for (const auto& hash : {std::string(63, 'a'), std::string(65, 'a'), std::string(64, 'g'),
                                std::string(64, ' ')}) {
            auto invalid = request;
            invalid.sha256 = hash;
            requireRejected(service, invalid, "invalid ota sha256");
            requireRejected(optional, invalid, "invalid ota sha256");
        }
    });
    check("matching-lowercase", [&] {
        require(service.validateRequest(request), "valid checksum request rejected");
        OtaChecksumTestAccess::verifyChecksum(service, request, artifactPath);
    });
    check("matching-mixed-case", [&] {
        auto mixed = request;
        mixed.sha256 = "BA7816bf8F01CFEA414140de5dae2223B00361A396177A9CB410FF61F20015AD";
        require(service.validateRequest(mixed), "valid mixed-case checksum request rejected");
        OtaChecksumTestAccess::verifyChecksum(service, mixed, artifactPath);
    });
    auto mismatch = request;
    mismatch.sha256 = std::string(64, '0');
    check("mismatch", [&] {
        require(service.validateRequest(mismatch), "well-formed checksum request rejected");
        expectError([&] { OtaChecksumTestAccess::verifyChecksum(service, mismatch, artifactPath); },
            "artifact sha256 mismatch");
    });
    check("optional-compatibility", [&] {
        require(optional.validateRequest(missing), "optional empty checksum rejected");
        require(optional.validateRequest(request), "optional valid checksum rejected");
        require(optional.validateRequest(mismatch), "optional well-formed checksum rejected");
        const auto absent = directory.path + "/does-not-exist";
        OtaChecksumTestAccess::verifyChecksum(optional, missing, absent);
        OtaChecksumTestAccess::verifyChecksum(optional, request, absent);
        OtaChecksumTestAccess::verifyChecksum(optional, mismatch, absent);
    });
    require(failures == 0, "ota checksum regression failures (no apply scripts executed)");
}

}  // namespace

int main() {
    try {
        checksumTests();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
}
