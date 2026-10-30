// MQTT transport security against a local broker stub (127.0.0.1 only):
// TLS with chain and host-name/IP verification, explicit refusal of plaintext,
// missing trust material and insecureSkipVerify, and no plaintext fallback.
// CA/server/client certificates are generated at run time with the openssl CLI
// in a temporary directory; no key material is stored in the repository.
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edge_gateway/builtin_mqtt_driver_publisher.hpp"
#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/mqtt_forwarder_service.hpp"

using edge_gateway::BuiltinMqttDriverPublisher;
using edge_gateway::MqttConfig;
using edge_gateway::MqttEventOutboxOwnership;
using edge_gateway::MqttPublisherMode;
using edge_gateway::MqttTransportPolicy;
using edge_gateway::evaluateMqttTransportPolicy;

namespace {

int failures = 0;
std::string repoRoot;
std::string workDir;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::cout << "ok   " << what << std::endl;
    } else {
        ++failures;
        std::cout << "FAIL " << what << std::endl;
    }
}

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

bool fileExists(const std::string& path) {
    struct stat info{};
    return stat(path.c_str(), &info) == 0;
}

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream output(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    output << text;
}

std::vector<std::string> readLines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream input(path.c_str());
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    return lines;
}

int countLines(const std::string& path, const std::string& needle) {
    int count = 0;
    for (const auto& line : readLines(path)) {
        if (contains(line, needle)) {
            ++count;
        }
    }
    return count;
}

struct Broker {
    pid_t pid = -1;
    int port = 0;
    std::string log;

    ~Broker() {
        stop();
    }
    void stop() {
        if (pid > 0) {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
            pid = -1;
        }
    }
    int accepted() const {
        return countLines(log, "\"event\": \"accepted\"");
    }
    int plaintextConnects() const {
        return countLines(log, "\"first\": \"10\"");
    }
};

bool startBroker(Broker& broker, const std::string& name, const std::string& mode,
    const std::string& cert = std::string(), const std::string& key = std::string(),
    const std::string& clientCa = std::string()) {
    const auto portFile = workDir + "/" + name + ".port";
    broker.log = workDir + "/" + name + ".log";
    writeFile(broker.log, "");
    const auto script = repoRoot + "/tools/mqtt_tls_test_broker.py";
    std::vector<std::string> args = {"python3", "-I", script, "--mode", mode,
        "--port-file", portFile, "--log", broker.log};
    if (!cert.empty()) {
        args.insert(args.end(), {"--cert", cert, "--key", key});
    }
    if (!clientCa.empty()) {
        args.insert(args.end(), {"--client-ca", clientCa});
    }
    broker.pid = fork();
    if (broker.pid == 0) {
        std::vector<char*> argv;
        for (auto& arg : args) {
            argv.push_back(&arg[0]);
        }
        argv.push_back(nullptr);
        execvp("python3", argv.data());
        _exit(127);
    }
    for (int attempt = 0; attempt < 200; ++attempt) {
        std::ifstream input(portFile.c_str());
        if (input >> broker.port && broker.port > 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

MqttConfig clientConfig(const std::string& broker, const std::string& caFile = std::string()) {
    MqttConfig config;
    config.enabled = true;
    config.protocolVersion = "mqtt3";
    config.broker = broker;
    config.clientId = "tls-test";
    config.connectTimeoutMs = 3000;
    config.reconnectBackoffMinMs = 0;
    config.reconnectBackoffMaxMs = 0;
    config.offlineBufferEnabled = false;
    config.tls.caFile = caFile;
    return config;
}

// Full CONNECT/CONNACK + PINGREQ/PINGRESP round trip; empty string means success.
std::string probe(const MqttConfig& config) {
    try {
        BuiltinMqttDriverPublisher publisher(config, MqttPublisherMode::TxOnly, MqttEventOutboxOwnership::External);
        publisher.probeConnection();
        return std::string();
    } catch (const std::exception& ex) {
        return ex.what();
    }
}

bool generateCertificates() {
    writeFile(workDir + "/req.cnf",
        "[req]\ndistinguished_name = dn\nprompt = no\n[dn]\nCN = placeholder\n"
        "[v3_ca]\nbasicConstraints = critical,CA:TRUE\nkeyUsage = critical,keyCertSign,cRLSign\n"
        "subjectKeyIdentifier = hash\n");
    writeFile(workDir + "/ok.ext",
        "basicConstraints = CA:FALSE\nextendedKeyUsage = serverAuth\n"
        "subjectAltName = DNS:localhost,IP:127.0.0.1\n");
    writeFile(workDir + "/wronghost.ext",
        "basicConstraints = CA:FALSE\nextendedKeyUsage = serverAuth\n"
        "subjectAltName = DNS:wrong-host.invalid\n");
    writeFile(workDir + "/client.ext",
        "basicConstraints = CA:FALSE\nextendedKeyUsage = clientAuth\n");
    writeFile(workDir + "/ca.cnf",
        "[ca]\ndefault_ca = test_ca\n[test_ca]\ndir = .\ndatabase = index.txt\n"
        "new_certs_dir = newcerts\nserial = serial\ndefault_md = sha256\npolicy = any\n"
        "unique_subject = no\n[any]\ncommonName = supplied\n"
        "[server_ext]\nbasicConstraints = CA:FALSE\nextendedKeyUsage = serverAuth\n"
        "subjectAltName = DNS:localhost,IP:127.0.0.1\n");
    const std::string ec = " -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes";
    const std::string script =
        "set -e; cd '" + workDir + "'; mkdir -p newcerts empty-dir; : > index.txt; echo 01 > serial;"
        "openssl req -x509 -config req.cnf -extensions v3_ca" + ec + " -keyout ca.key -out ca.crt -days 2 -subj /CN=mqtt-test-ca;"
        "openssl req -x509 -config req.cnf -extensions v3_ca" + ec + " -keyout other-ca.key -out other-ca.crt -days 2 -subj /CN=mqtt-other-ca;"
        "openssl req -new -config req.cnf" + ec + " -keyout server.key -out server.csr -subj /CN=localhost;"
        "openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 2 -extfile ok.ext -out server-ok.crt;"
        "openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 2 -extfile wronghost.ext -out server-wronghost.crt;"
        "openssl ca -batch -config ca.cnf -cert ca.crt -keyfile ca.key -in server.csr -out server-expired.crt"
        " -startdate 20200101000000Z -enddate 20200102000000Z -extensions server_ext -notext;"
        "openssl req -new -config req.cnf" + ec + " -keyout client.key -out client.csr -subj /CN=GW-TLS-TEST;"
        "openssl x509 -req -in client.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 2 -extfile client.ext -out client.crt;"
        "printf 'not a certificate\\n' > garbage.crt";
    const auto command = "sh -c \"" + script + "\" > '" + workDir + "/openssl.log' 2>&1";
    return std::system(command.c_str()) == 0;
}

void testPolicy() {
    using Kind = MqttTransportPolicy::Kind;
    auto config = clientConfig("");
    auto policy = evaluateMqttTransportPolicy(config);
    check(policy.kind == Kind::Refused && contains(policy.message, "not configured"),
        "empty broker is refused as not configured (no default address)");

    config = clientConfig("tcp://10.0.0.8:1883");
    policy = evaluateMqttTransportPolicy(config);
    check(policy.kind == Kind::Refused && contains(policy.message, "plaintext broker tcp://10.0.0.8:1883 refused") &&
            contains(policy.message, "allowInsecurePlaintext"),
        "tcp:// broker is refused by default with the fix in the message");
    for (const auto& broker : {std::string("mqtt://10.0.0.8"), std::string("10.0.0.8:1883"), std::string("10.0.0.8")}) {
        config = clientConfig(broker);
        policy = evaluateMqttTransportPolicy(config);
        check(policy.kind == Kind::Refused && policy.port == 1883, "plaintext broker refused: " + broker);
    }
    config = clientConfig("tcp://10.0.0.8:1883");
    config.allowInsecurePlaintext = true;
    policy = evaluateMqttTransportPolicy(config);
    check(policy.kind == Kind::InsecurePlaintext && contains(policy.message, "WARNING: INSECURE PLAINTEXT"),
        "plaintext only with explicit allowInsecurePlaintext, with a risk warning");

    config = clientConfig("ssl://broker.example.com");
    policy = evaluateMqttTransportPolicy(config);
    check(policy.kind == Kind::Tls && policy.host == "broker.example.com" && policy.port == 8883,
        "ssl:// defaults to port 8883");
    config = clientConfig("MQTTS://broker.example.com:8884");
    policy = evaluateMqttTransportPolicy(config);
    check(policy.kind == Kind::Tls && policy.port == 8884, "mqtts:// scheme is case-insensitive");
    config = clientConfig("tcp://broker.example.com:8883");
    config.tls.enabled = true;
    policy = evaluateMqttTransportPolicy(config);
    check(policy.kind == Kind::Tls, "tls.enabled=true forces TLS for a tcp:// broker");
    config = clientConfig("ssl://[::1]:8883");
    policy = evaluateMqttTransportPolicy(config);
    check(policy.kind == Kind::Tls && policy.host == "::1", "bracketed IPv6 broker");
    config = clientConfig("ssl://broker.example.com:8883");
    config.allowInsecurePlaintext = true;
    check(evaluateMqttTransportPolicy(config).kind == Kind::Tls, "allowInsecurePlaintext never weakens TLS");

    for (const auto& broker : {std::string("ws://broker.example.com"), std::string("ssl://broker.example.com:abc"),
             std::string("ssl://broker.example.com:0"), std::string("ssl://broker.example.com:70000"),
             std::string("ssl://:8883"), std::string("ssl://::1:8883")}) {
        config = clientConfig(broker);
        check(evaluateMqttTransportPolicy(config).kind == Kind::Refused, "invalid broker refused: " + broker);
    }
    config = clientConfig("ssl://user:s3cret@broker.example.com:8883");
    policy = evaluateMqttTransportPolicy(config);
    check(policy.kind == Kind::Refused && !contains(policy.message, "s3cret"),
        "credentials in broker URL are refused without being echoed");

    config = clientConfig("ssl://broker.example.com:8883");
    config.tls.insecureSkipVerify = true;
    config.allowInsecurePlaintext = true;
    policy = evaluateMqttTransportPolicy(config);
    check(policy.kind == Kind::Refused && contains(policy.message, "insecureSkipVerify"),
        "insecureSkipVerify=true is refused even with the plaintext experiment switch");
    config = clientConfig("ssl://broker.example.com:8883");
    config.tls.certFile = "/x/client.crt";
    check(evaluateMqttTransportPolicy(config).kind == Kind::Refused, "certFile without keyFile is refused");
}

void testConfigLoading() {
    const auto path = workDir + "/app.json";
    writeFile(path, R"({"mqtt":{"enabled":true,"broker":"tcp://127.0.0.1:1883","allowInsecurePlaintext":true},)"
        R"("mqttForward":{"enabled":false,"allowInsecurePlaintext":true}})");
    auto app = edge_gateway::ConfigLoader::loadAppConfigFromFile(path);
    check(app.mqtt.allowInsecurePlaintext && app.mqttForward.allowInsecurePlaintext,
        "allowInsecurePlaintext parsed for mqtt and mqttForward");
    writeFile(path, R"({"mqtt":{"enabled":true,"broker":"tcp://127.0.0.1:1883"}})");
    app = edge_gateway::ConfigLoader::loadAppConfigFromFile(path);
    check(!app.mqtt.allowInsecurePlaintext && !app.mqttForward.allowInsecurePlaintext,
        "allowInsecurePlaintext defaults to false");
    writeFile(path, R"({"mqtt":{"enabled":true,"allowInsecurePlaintext":"true"}})");
    bool rejected = false;
    try {
        edge_gateway::ConfigLoader::loadAppConfigFromFile(path);
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, "allowInsecurePlaintext must be a JSON boolean");

    edge_gateway::MqttForwardConfig forward;
    forward.enabled = true;
    forward.broker = "tcp://127.0.0.1:1883";
    forward.allowInsecurePlaintext = true;
    check(edge_gateway::MqttForwarderService::makeTxOnlyMqttConfig(forward, "GW1").allowInsecurePlaintext,
        "forwarder carries its own allowInsecurePlaintext");
    forward.allowInsecurePlaintext = false;
    check(!edge_gateway::MqttForwarderService::makeTxOnlyMqttConfig(forward, "GW1").allowInsecurePlaintext,
        "forwarder plaintext stays refused by default");
    MqttConfig primary = clientConfig("tcp://127.0.0.1:1883");
    primary.allowInsecurePlaintext = true;
    check(edge_gateway::MqttForwarderService::makePrimaryFullMqttConfig(
              primary, edge_gateway::MqttFullUploadWorkerConfig(), "GW1").allowInsecurePlaintext,
        "isolated primary full worker inherits the main transport policy");

    // Shipped factory templates load; connecting is refused until the site deploys broker and CA.
    for (const auto& name : {std::string("mqtt-service.json"), std::string("monitor-service.json"),
             std::string("camera-service.json")}) {
        const auto file = repoRoot + "/config/factory/runtime/apps/" + name;
        try {
            const auto shipped = edge_gateway::ConfigLoader::loadAppConfigFromFile(file);
            const auto policy = evaluateMqttTransportPolicy(shipped.mqtt);
            check(shipped.mqtt.tls.enabled && !shipped.mqtt.allowInsecurePlaintext &&
                    !shipped.mqtt.tls.caFile.empty() &&
                    policy.kind == MqttTransportPolicy::Kind::Refused && contains(policy.message, "not configured"),
                "factory " + name + " loads, requires TLS and refuses until a broker is deployed");
        } catch (const std::exception& ex) {
            check(false, "factory " + name + " loads: " + ex.what());
        }
    }
}

void testNetwork() {
    const auto ca = workDir + "/ca.crt";
    Broker ok, wrongHost, expired, plain, mutual;
    check(startBroker(ok, "ok", "tls", workDir + "/server-ok.crt", workDir + "/server.key"), "start TLS broker");
    check(startBroker(wrongHost, "wronghost", "tls", workDir + "/server-wronghost.crt", workDir + "/server.key"),
        "start wrong-host TLS broker");
    check(startBroker(expired, "expired", "tls", workDir + "/server-expired.crt", workDir + "/server.key"),
        "start expired-certificate TLS broker");
    check(startBroker(plain, "plain", "plain"), "start plaintext broker");
    check(startBroker(mutual, "mutual", "tls", workDir + "/server-ok.crt", workDir + "/server.key", ca),
        "start mutual-TLS broker");
    const auto port = [](const Broker& broker) { return std::to_string(broker.port); };

    auto error = probe(clientConfig("ssl://localhost:" + port(ok), ca));
    check(error.empty(), "TLS to localhost with deployed CA connects: " + error);
    check(countLines(ok.log, "\"sni\": \"localhost\"") == 1 && countLines(ok.log, "\"mqtt\": \"connect\"") == 1,
        "SNI carries the broker host name and MQTT CONNECT runs inside TLS");
    error = probe(clientConfig("ssl://127.0.0.1:" + port(ok), ca));
    check(error.empty(), "TLS to an IP broker verifies the certificate IP SAN: " + error);
    check(countLines(ok.log, "\"sni\": null") == 1, "no SNI is sent for an IP literal");

    error = probe(clientConfig("ssl://localhost:" + port(ok), workDir + "/other-ca.crt"));
    check(contains(error, "certificate verification failed") && contains(error, "unable to get local issuer certificate"),
        "wrong CA is rejected: " + error);
    error = probe(clientConfig("ssl://localhost:" + port(wrongHost), ca));
    check(contains(error, "certificate verification failed") && contains(error, "ostname mismatch"),
        "host name mismatch is rejected: " + error);
    error = probe(clientConfig("ssl://127.0.0.1:" + port(wrongHost), ca));
    check(contains(error, "certificate verification failed") && contains(error, "IP address mismatch"),
        "IP mismatch is rejected: " + error);
    error = probe(clientConfig("ssl://localhost:" + port(expired), ca));
    check(contains(error, "certificate verification failed") && contains(error, "certificate has expired"),
        "expired broker certificate is rejected: " + error);
    check(ok.plaintextConnects() == 0 && wrongHost.plaintextConnects() == 0 && expired.plaintextConnects() == 0,
        "failed TLS attempts never retry as plaintext MQTT");

    // TLS client pointed at a plaintext broker: handshake fails, three reconnects stay TLS.
    for (int attempt = 0; attempt < 3; ++attempt) {
        error = probe(clientConfig("ssl://127.0.0.1:" + port(plain), ca));
        check(contains(error, "mqtt tls handshake failed"), "TLS to plaintext port fails closed: " + error);
    }
    check(plain.accepted() == 3 && countLines(plain.log, "\"first\": \"16\"") == 3 && plain.plaintextConnects() == 0,
        "every attempt to the plaintext port was a TLS ClientHello, never a plaintext CONNECT");

    // Trust material problems are found before any socket is opened.
    int before = ok.accepted();
    error = probe(clientConfig("ssl://localhost:" + port(ok), workDir + "/missing-ca.crt"));
    check(contains(error, "caFile is missing or unreadable"), "missing caFile is reported: " + error);
    error = probe(clientConfig("ssl://localhost:" + port(ok), workDir + "/garbage.crt"));
    check(contains(error, "not a usable PEM CA certificate"), "invalid caFile is reported: " + error);
    setenv("SSL_CERT_FILE", (workDir + "/absent-bundle.pem").c_str(), 1);
    setenv("SSL_CERT_DIR", (workDir + "/absent-dir").c_str(), 1);
    error = probe(clientConfig("ssl://localhost:" + port(ok)));
    check(contains(error, "no system trust store was found"), "no caFile and no system store is refused: " + error);
    auto skip = clientConfig("ssl://localhost:" + port(ok), ca);
    skip.tls.insecureSkipVerify = true;
    error = probe(skip);
    check(contains(error, "insecureSkipVerify=true is not supported"), "insecureSkipVerify is refused: " + error);
    check(ok.accepted() == before, "no connection is opened without usable trust material");

    setenv("SSL_CERT_FILE", ca.c_str(), 1);
    setenv("SSL_CERT_DIR", (workDir + "/empty-dir").c_str(), 1);
    error = probe(clientConfig("ssl://localhost:" + port(ok)));
    check(error.empty(), "empty caFile uses an available system trust store: " + error);
    unsetenv("SSL_CERT_FILE");
    unsetenv("SSL_CERT_DIR");

    // Mutual TLS.
    auto withClient = clientConfig("ssl://localhost:" + port(mutual), ca);
    withClient.tls.certFile = workDir + "/client.crt";
    withClient.tls.keyFile = workDir + "/client.key";
    error = probe(withClient);
    check(error.empty(), "mutual TLS with client certificate connects: " + error);
    error = probe(clientConfig("ssl://localhost:" + port(mutual), ca));
    check(!error.empty(), "mutual TLS broker rejects a client without certificate: " + error);
    withClient.tls.keyFile = workDir + "/missing.key";
    error = probe(withClient);
    check(contains(error, "keyFile is missing or unreadable"), "missing client key is reported: " + error);

    // Plaintext: refused without the switch (no socket), works with it and warns.
    before = plain.accepted();
    error = probe(clientConfig("tcp://127.0.0.1:" + port(plain)));
    check(contains(error, "mqtt connection refused") && contains(error, "plaintext broker"),
        "plaintext broker is refused by default: " + error);
    try {
        BuiltinMqttDriverPublisher subscriber(clientConfig("tcp://127.0.0.1:" + port(plain)),
            MqttPublisherMode::Bidirectional, MqttEventOutboxOwnership::External);
        subscriber.pollIncoming(0);
        check(false, "subscriber connection is refused for plaintext");
    } catch (const std::exception& ex) {
        check(contains(ex.what(), "plaintext broker"), std::string("subscriber connection is refused: ") + ex.what());
    }
    check(plain.accepted() == before, "refused plaintext never opens a socket");

    auto experiment = clientConfig("tcp://127.0.0.1:" + port(plain));
    experiment.allowInsecurePlaintext = true;
    std::ostringstream captured;
    auto* previous = std::cerr.rdbuf(captured.rdbuf());
    error = probe(experiment);
    std::cerr.rdbuf(previous);
    check(error.empty(), "explicit experimental plaintext connects: " + error);
    check(plain.plaintextConnects() == 1 && countLines(plain.log, "\"mqtt\": \"connect\"") == 1,
        "experimental plaintext sends CONNECT in clear");
    const auto warning = "WARNING: INSECURE PLAINTEXT MQTT to tcp://127.0.0.1:" + port(plain);
    const auto first = captured.str().find(warning);
    check(first != std::string::npos && captured.str().find(warning, first + 1) != std::string::npos,
        "experimental plaintext prints the risk warning at start and on connect");

    // Deployed template without the CA on disk: refused before connecting, path named.
    const std::string deployedCa = "/opt/modbus-gateway/config/runtime/tls/ca.crt";
    if (!fileExists(deployedCa)) {
        const auto shipped = edge_gateway::ConfigLoader::loadAppConfigFromFile(
            repoRoot + "/config/factory/runtime/apps/mqtt-service.json");
        auto deployed = shipped.mqtt;
        deployed.broker = "ssl://localhost:" + port(ok);
        deployed.offlineBufferEnabled = false;
        before = ok.accepted();
        error = probe(deployed);
        check(contains(error, "caFile is missing or unreadable: " + deployedCa) && ok.accepted() == before,
            "template with broker but undeployed CA names the missing file: " + error);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: mqtt_tls_transport_test <repo root>" << std::endl;
        return 2;
    }
    repoRoot = argv[1];
    signal(SIGPIPE, SIG_IGN);
    testPolicy();
    if (std::system("openssl version > /dev/null 2>&1") != 0 ||
        std::system("python3 -c 'import ssl' > /dev/null 2>&1") != 0) {
        std::cout << "SKIP network part: openssl CLI or python3 ssl unavailable" << std::endl;
        return failures == 0 ? 77 : 1;
    }
    char pattern[] = "/tmp/mqtt-tls-test-XXXXXX";
    const char* tmp = mkdtemp(pattern);
    if (tmp == nullptr) {
        std::cerr << "mkdtemp failed" << std::endl;
        return 1;
    }
    workDir = tmp;
    testConfigLoading();
    if (!generateCertificates()) {
        std::cerr << "certificate generation failed, see " << workDir << "/openssl.log" << std::endl;
        return 1;
    }
    testNetwork();
    if (failures == 0) {
        if (std::system(("rm -rf '" + workDir + "'").c_str()) != 0) {
            std::cout << "could not remove " << workDir << std::endl;
        }
    } else {
        std::cout << "artifacts kept in " << workDir << std::endl;
    }
    std::cout << (failures == 0 ? "mqtt_tls_transport_test passed" : "mqtt_tls_transport_test FAILED")
              << " failures=" << failures << std::endl;
    return failures == 0 ? 0 : 1;
}
