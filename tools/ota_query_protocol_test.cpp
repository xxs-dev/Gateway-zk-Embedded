#include "edge_gateway/ota_query_protocol.hpp"

#include <iostream>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

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

void testFullProtocol() {
    namespace full = ota_full_query_protocol;
    const std::string sha(64, 'a'), nonce(64, 'b');
    OtaFullQueryChallenge challenge{"QUERY_JOB", "GW_QUERY", nonce, "full", "2.0.0", sha, 13,
        1700000000000, 1700000060000, "AA=="};
    const auto descriptor = std::string(full::requestFormat) + "\njobId=QUERY_JOB\nmachineCode=GW_QUERY\nnonce=" + nonce +
        "\npackageType=full\nversion=2.0.0\nartifactSha256=" + sha +
        "\nartifactSize=13\nissuedAtMs=1700000000000\nexpiresAtMs=1700000060000\n";
    require(full::challengeDescriptor(challenge) == descriptor);
    const auto wire = std::string("{\"format\":\"") + full::requestFormat + "\",\"jobId\":\"QUERY_JOB\",\"machineCode\":\"GW_QUERY\",\"nonce\":\"" + nonce +
        "\",\"packageType\":\"full\",\"version\":\"2.0.0\",\"artifactSha256\":\"" + sha +
        "\",\"artifactSize\":\"13\",\"issuedAtMs\":\"1700000000000\",\"expiresAtMs\":\"1700000060000\",\"signature\":\"AA==\"}";
    require(full::challengeDescriptor(full::parseChallenge(wire)) == descriptor);
    const auto replace = [&](const std::string& from, const std::string& to) {
        auto value = wire; const auto at = value.find(from); require(at != std::string::npos);
        value.replace(at, from.size(), to); return value;
    };
    const auto rejectWire = [&](const std::string& value) {
        bool invalid = false;
        try { (void)full::parseChallenge(value); } catch (const std::exception&) { invalid = true; }
        require(invalid);
    };
    for (const auto& value : {std::string("013"), std::string("1.0"), std::string("1e1"), std::string("-1"),
                             std::string("9223372036854775808"), std::string("18446744073709551615"), std::string("0")})
        rejectWire(replace("\"13\"", "\"" + value + "\""));
    rejectWire(replace("\"13\"", "13"));
    rejectWire(replace("\"full\"", "\"config\""));
    rejectWire(replace("\"signature\":\"AA==\"", "\"signature\":\"AA==\",\"candidateSha256\":\"" + sha + "\""));
    rejectWire(replace("\"jobId\":\"QUERY_JOB\"", "\"jobId\":\"QUERY_JOB\",\"jobId\":\"QUERY_JOB\""));
    rejectWire(replace("\"nonce\":\"" + nonce + "\",", ""));
    rejectWire(replace(full::requestFormat, "edge-original-job-query-request-v1"));
    rejectWire(replace("AA==", "AB=="));
    rejectWire(replace("QUERY_JOB", std::string("\xc0\xaf", 2)));
    rejectWire(wire + "{}");
    rejectWire(wire + std::string(8193, ' '));
    require(full::decimal("9223372036854775807") == std::numeric_limits<std::int64_t>::max());
    auto over = challenge; over.artifactSize = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
    rejected([&] { full::validate(over); });
    over = challenge; over.expiresAtMs = over.issuedAtMs + 300001;
    rejected([&] { full::validate(over); });
    for (const std::string signature : {"AA=", "AA==\n", "=AAA", "AAA=", "AAAA"}) {
        if (signature == "AAA=" || signature == "AAAA") full::requireCanonicalSignature(signature);
        else rejected([&] { full::requireCanonicalSignature(signature); });
    }
    OtaFullQueryObservation observation;
    observation.jobId = challenge.jobId; observation.machineCode = challenge.machineCode;
    observation.nonce = nonce; observation.packageType = "full"; observation.outcome = "UNKNOWN";
    observation.version = challenge.version; observation.artifactSha256 = sha; observation.artifactSize = 13;
    observation.signature = "AA==";
    const auto unknown = full::observationDescriptor(observation);
    require(unknown.find("\nversion=2.0.0\nartifactSha256=" + sha + "\nartifactSize=13\n") != std::string::npos);
    require(json::JsonParser(full::encodeObservation(observation), 4, 32).parse().asObject().values.size() == 17);
    observation.occurrenceId = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    rejected([&] { full::validate(observation); });
    observation.outcome = "HEALTHY_APPLIED"; observation.candidateSha256 = std::string(64, 'c');
    observation.parentSha256 = std::string(64, 'd'); observation.intentSha256 = std::string(64, 'e');
    observation.healthSha256 = std::string(64, 'f'); observation.healthTs = 1700000001000;
    observation.transaction = "OTA_b2ff4873e361550ecbba12a333ec6e75";
    require(full::transaction(observation.jobId, observation.occurrenceId) == observation.transaction);
    full::validate(observation);
    observation.outcome = "FAILED_ROLLED_BACK"; full::validate(observation);
    auto wrong = observation; wrong.transaction = "OTA_" + std::string(32, 'a');
    rejected([&] { full::validate(wrong); });
    wrong = observation; wrong.parentSha256.clear(); rejected([&] { full::validate(wrong); });
    wrong = observation; wrong.packageType = "config"; rejected([&] { full::validate(wrong); });
    wrong = observation; wrong.outcome = "completed"; rejected([&] { full::validate(wrong); });
    observation.outcome = "REJECTED_UNCHANGED";
    rejected([&] { full::validate(observation); });
    observation.candidateSha256.clear(); observation.parentSha256.clear(); observation.transaction.clear();
    full::validate(observation);
    std::cout << "full original-job protocol/11-field parse/17-field encode/negative vectors PASS (no crypto)\n";
}

std::string boundedVector(const std::filesystem::path& path, std::size_t limit) {
    std::ifstream input(path, std::ios::binary);
    require(input.good());
    std::string value;
    char chunk[1024];
    while (input.read(chunk, sizeof(chunk)) || input.gcount()) {
        require(value.size() + static_cast<std::size_t>(input.gcount()) <= limit);
        value.append(chunk, static_cast<std::size_t>(input.gcount()));
    }
    require(input.eof());
    return value;
}

std::string vectorText(const json::JsonValue& value, const char* key) {
    const auto* field = value.find(key);
    require(field && field->isString());
    return field->asString();
}

std::map<std::string, std::string> observationFields(const std::string& wire) {
    const auto root = json::JsonParser(wire, 4, 32).parse();
    require(root.isObject() && root.asObject().values.size() == 17);
    std::map<std::string, std::string> fields;
    for (const auto& field : root.asObject().values) {
        require(field.value && field.value->isString());
        require(fields.emplace(field.key, field.value->asString()).second);
    }
    return fields;
}

// Test-only input adapter: Edge produces observations, it has no response parser.
// Every value comes from the sealed Java wire; validation/descriptor/encode are production code.
OtaFullQueryObservation observationFromJava(const std::string& wire) {
    const auto fields = observationFields(wire);
    require(fields.at("format") == ota_full_query_protocol::responseFormat);
    OtaFullQueryObservation value;
    value.jobId = fields.at("jobId"); value.machineCode = fields.at("machineCode");
    value.nonce = fields.at("nonce"); value.packageType = fields.at("packageType");
    value.outcome = fields.at("outcome"); value.occurrenceId = fields.at("occurrenceId");
    value.version = fields.at("version"); value.artifactSha256 = fields.at("artifactSha256");
    value.artifactSize = static_cast<std::uint64_t>(ota_full_query_protocol::decimal(fields.at("artifactSize")));
    value.candidateSha256 = fields.at("candidateSha256"); value.transaction = fields.at("transaction");
    value.parentSha256 = fields.at("parentSha256"); value.intentSha256 = fields.at("intentSha256");
    value.healthSha256 = fields.at("healthSha256"); value.healthTs = ota_full_query_protocol::decimal(fields.at("healthTs"));
    value.signature = fields.at("signature");
    ota_full_query_protocol::validate(value);
    ota_full_query_protocol::requireCanonicalSignature(value.signature);
    return value;
}

void testJavaWireVectors(const std::filesystem::path& vectors, const std::filesystem::path& output) {
    namespace full = ota_full_query_protocol;
    const auto rawIndex = boundedVector(vectors / "INDEX.json", 128 * 1024);
    const auto index = json::JsonParser(rawIndex, 8, 2048).parse();
    require(vectorText(index, "commit") == "41bf609591e01a287d766a1749788b8bb0699fc6");
    require(vectorText(index, "signature") == "AA==");
    const auto* cases = index.find("cases"); require(cases && cases->isArray());
    require(!std::filesystem::exists(output) && std::filesystem::create_directory(output));
    const auto filename = [](const std::string& name) {
        require(ota_query_protocol::matches(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-", 1, 128));
        require(name != "." && name != ".." && name.find("..") == std::string::npos);
        return name;
    };
    const auto writeVector = [&](const std::string& name, const std::string& raw) {
        std::ofstream stream(output / filename(name), std::ios::binary);
        stream.write(raw.data(), static_cast<std::streamsize>(raw.size())); stream.close();
        require(stream.good());
    };
    std::vector<std::string> emitted;
    unsigned requests = 0, responseChecks = 0, descriptors = 0, rejectedSchema = 0, clocks = 0;
    for (const auto& caseValue : cases->asArray().values) {
        require(caseValue && caseValue->isObject());
        const auto& entry = *caseValue;
        const auto name = filename(vectorText(entry, "name"));
        const auto* wireMetadata = entry.find("wire"); require(wireMetadata);
        const auto path = wireMetadata->isString() ? wireMetadata->asString() : vectorText(*wireMetadata, "path");
        const auto wire = boundedVector(vectors / filename(path), full::maxPayloadBytes);
        if (wireMetadata->isObject())
            require(ota_query_detail::sha256Hex(wire) == vectorText(*wireMetadata, "sha256"));
        const bool request = entry.find("parseChallenge") != nullptr;
        const auto expected = vectorText(entry, request ? "parseChallenge" : "parseObservation");
        require(expected == "ACCEPT_SCHEMA" || expected == "REJECT_SCHEMA");
        bool accepted = false;
        std::string descriptor;
        OtaFullQueryObservation response;
        try {
            if (request) {
                descriptor = full::challengeDescriptor(full::parseChallenge(wire));
            } else {
                response = observationFromJava(wire);
                descriptor = full::observationDescriptor(response);
            }
            accepted = true;
        } catch (const std::invalid_argument&) {
        } catch (const std::out_of_range&) {
        } catch (const std::runtime_error&) {
            // File reads/hashes are outside this block: missing input is never a schema PASS.
        }
        if (accepted != (expected == "ACCEPT_SCHEMA"))
            throw std::runtime_error("Java/C++ schema difference: " + name);
        if (request) ++requests; else ++responseChecks;
        if (!accepted) ++rejectedSchema;
        if (const auto* descriptorMetadata = entry.find("descriptor")) {
            require(accepted);
            const auto expectedDescriptor = boundedVector(vectors / filename(vectorText(*descriptorMetadata, "path")), 8192);
            require(ota_query_detail::sha256Hex(expectedDescriptor) == vectorText(*descriptorMetadata, "sha256"));
            if (descriptor != expectedDescriptor) throw std::runtime_error("Java/C++ descriptor bytes differ: " + name);
            ++descriptors;
        }
        const bool clock = name.rfind("clock-", 0) == 0;
        if (clock) ++clocks;
        if (!request && accepted) {
            require(response.transaction == vectorText(entry, "transaction"));
            const auto encoded = full::encodeObservation(response);
            require(observationFields(encoded) == observationFields(wire));
            writeVector(name + ".wire.json", encoded);
            writeVector(name + ".descriptor.txt", descriptor);
            emitted.push_back("{\"name\":\"" + name + "\",\"wire\":{\"path\":\"" + name +
                ".wire.json\",\"sha256\":\"" + ota_query_detail::sha256Hex(encoded) + "\",\"bytes\":" +
                std::to_string(encoded.size()) + "},\"descriptor\":{\"path\":\"" + name +
                ".descriptor.txt\",\"sha256\":\"" + ota_query_detail::sha256Hex(descriptor) + "\",\"bytes\":" +
                std::to_string(descriptor.size()) + "},\"transaction\":\"" + response.transaction + "\"}");
        }
        std::cout << "{\"case\":\"" << name << "\",\"schema\":\"" << expected
            << "\",\"matched\":true,\"responseInputAdapter\":" << (request ? "false" : "true")
            << ",\"handlerClock\":\"" << (clock ? "NOT_EXECUTED_AT_HANDLER" : "NOT_APPLICABLE")
            << "\",\"authenticityVerified\":false}\n";
    }
    const auto* tx = index.find("transactionInput"); require(tx && tx->isObject());
    require(full::transaction(vectorText(*tx, "jobId"), vectorText(*tx, "occurrenceId")) == vectorText(index, "transaction"));
    require(requests == 17 && responseChecks == 12 && rejectedSchema == 14 && descriptors == 10 && clocks == 5 && emitted.size() == 6);
    std::string result = "{\"producer\":\"C++ PRODUCTION encodeObservation/observationDescriptor\",\"sourceJavaIndexSha256\":\"" +
        ota_query_detail::sha256Hex(rawIndex) + "\",\"schemaMatches\":29,\"requestSchemaMatches\":17,\"responseAdapterMatches\":12,";
    result += "\"descriptorBytesMatches\":10,\"handlerClockNotExecuted\":5,\"signature\":\"AA==\",\"authenticityVerified\":false,\"caseCount\":6,\"cases\":[";
    for (std::size_t i = 0; i < emitted.size(); ++i) result += (i ? "," : "") + emitted[i];
    result += "]}\n";
    writeVector("INDEX.json", result);
    std::cout << "Java raw bytes -> C++ full schema/descriptors/transaction -> 6 production-encoded responses PASS; NO CRYPTO; clocks NOT_EXECUTED_AT_HANDLER\n";
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--java-wire-vectors") {
        require(argc == 4);
        testJavaWireVectors(argv[2], argv[3]); return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--full") { testFullProtocol(); return 0; }
    require(argc == 1);
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
