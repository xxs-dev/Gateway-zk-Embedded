// Reuse the real Unix IPC runtime and commit-then-drop-reply fixture.
#define main event_store_client_suite_main
#include "event_store_client_test.cpp"
#undef main
#include "edge_gateway/event_store_management.hpp"
#include <set>

namespace {
constexpr std::int64_t managementTs = 1780000000123LL;
const std::string managementTopic = "edge/ota/status/GW_TEST";
const std::string managementPayload =
    R"({"jobId":"job-1","machineCode":"GW_TEST","stage":"accepted","progress":0,"downloadedBytes":0,"totalBytes":512,"message":"accepted","ts":1780000000123})";

EventStoreProducerOptions managementOptions(const Lab& lab) {
    EventStoreProducerOptions options;
    options.client = lab.client();
    options.client.timeoutMs = 2000;
    return options;
}

std::string managementId(const ReplyProxy::Exchange& exchange) {
    const auto request = parse(exchange.request);
    const auto& events = field(field(request, "args"), "events").asArray().values;
    require(events.size() == 1, "management submission is not one immutable event");
    return stringField(*events[0], "eventId");
}

void acrossWriters(Profile profile, bool requireAcceptance) {
    Lab lab(profile);
    std::string failure;
    for (int i = 0; i < 2; ++i) {
        EventStoreManagementWriter writer(managementOptions(lab));
        try { writer.submit("ota_status", managementTopic, managementPayload, managementTs); }
        catch (const std::exception& error) {
            const auto attempts = lab.proxy.exchanges(kAppend);
            require(i == 1, "initial management commit failed");
            failure = "writer=" + std::to_string(i) + " " + error.what() +
                " reply=" + (attempts.empty() ? "none" : attempts.back().runtimeReply);
        }
    }
    const auto requests = lab.proxy.exchanges(kAppend);
    require(requests.size() == 2, "second writer did not reach the EventStore");
    require(managementId(requests[0]) == managementId(requests[1]),
        "same business request changed eventId across writer instances");
    require(pendingCount(lab.options) == 1, "same business request created duplicate outbox rows");
    if (requireAcceptance) require(failure.empty(), failure);
}

void distinctEvents() {
    Lab lab;
    EventStoreManagementWriter writer(managementOptions(lab));
    writer.submit("ota_status", managementTopic, managementPayload, managementTs);
    writer.submit("ota_status", managementTopic + "/other", managementPayload, managementTs);
    writer.submit("ota_status", managementTopic, managementPayload + " ", managementTs);
    writer.submit("ota_status", managementTopic, managementPayload, managementTs + 1);
    // Deliberately ambiguous without length prefixes.
    writer.submit("ota_status", "ab", "c", managementTs);
    writer.submit("ota_status", "a", "bc", managementTs);
    writer.submit("ota_status", managementTopic, "{\"jobId\":\"job-2\"}", managementTs);
    writer.submit("ota_status", managementTopic, "{\"stage\":\"failed\"}", managementTs);
    writer.submit("ota_status", managementTopic, "\xe4\xb8\xad\xe6\x96\x87", 9007199254740993LL);
    const auto requests = lab.proxy.exchanges(kAppend);
    require(managementId(requests.front()) ==
        "management:v1:898c6cdf920224d701f9a23855cedc6b7380ae1f379abe7096da24234e49fa95",
        "persistent SHA256 encoding changed");
    std::set<std::string> ids;
    for (const auto& request : requests) {
        const auto id = managementId(request);
        require(id.size() == 78 && id.substr(0, 14) == "management:v1:", "unexpected stable ID encoding");
        require(ids.insert(id).second, "different business events got the same ID");
    }
    require(ids.size() == 9 && pendingCount(lab.options) == 9, "different events were lost");
    rejects([&] { writer.submit("alarm", managementTopic, managementPayload, managementTs); });
    rejects([&] { writer.submit("ota_status", "", managementPayload, managementTs); });
    rejects([&] { writer.submit("ota_status", managementTopic, managementPayload, 0); });
    rejects([&] { writer.submit("ota_status", managementTopic, std::string("a\0b", 3), managementTs); });
    require(lab.proxy.exchanges(kAppend).size() == 9, "invalid event reached IPC");
}

void unknownReceipt(Profile profile, bool recreate) {
    Lab lab(profile);
    auto options = managementOptions(lab);
    options.client.timeoutMs = 300;
    options.retryMs = 2000;
    std::unique_ptr<EventStoreManagementWriter> writer;
    lab.proxy.arm(kAppend, ReplyProxy::Fault::Truncate);
    if (recreate) {
        // Exec avoids inheriting the runtime's worker-thread state. The child
        // exits without destructors after the unknown reply, losing its RAM queue.
        const auto pid = fork();
        require(pid >= 0, "management child fork failed");
        if (pid == 0) {
            execl("/proc/self/exe", "management_test", "--unknown-child",
                options.client.socketPath.c_str(), options.client.identity.storeId.c_str(),
                options.client.identity.configGeneration.c_str(), options.client.actorId.c_str(),
                static_cast<char*>(nullptr));
            _exit(127);
        }
        int status = 0;
        pid_t waited;
        do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
        require(waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 3,
            "child did not exit after unknown management commit");
    } else {
        writer = std::make_unique<EventStoreManagementWriter>(options);
        rejects([&] { writer->submit("ota_status", managementTopic, managementPayload, managementTs); });
    }
    lab.proxy.waitForExchanges(kAppend, 1);
    const auto original = lab.proxy.exchanges(kAppend).front();
    success(parse(original.runtimeReply), Role::Producer, 1, 1);
    require(pendingCount(lab.options) == 1, "unknown fixture did not commit upstream");
    if (writer) require(!writer->drain(0), "unknown reply reported durable acceptance");
    if (recreate) {
        writer.reset();
        lab.restart();
        writer = std::make_unique<EventStoreManagementWriter>(options);
        std::string failure;
        try { writer->submit("ota_status", managementTopic, managementPayload, managementTs); }
        catch (const std::exception& error) { failure = error.what(); }
        const auto requests = lab.proxy.exchanges(kAppend);
        require(requests.size() == 2, "recreated writer did not resubmit original event");
        require(managementId(original) == managementId(requests.back()), "unknown restart changed business ID");
        require(pendingCount(lab.options) == 1, "unknown restart duplicated event");
        require(failure.empty(), "unknown restart cannot confirm prior commit: " + failure);
    } else {
        require(writer->drain(5000), "unknown append did not resolve by exact request retry");
        writer->submit("ota_status", managementTopic, managementPayload, managementTs);
        exactReplay(lab.proxy, kAppend, original.request);
        require(pendingCount(lab.options) == 1, "unknown retry duplicated event");
    }
    lab.proxy.checked();
}
} // namespace

int main(int argc, char** argv) {
    if (const auto* library = std::getenv("SQLITE_LIBRARY")) testLibrary = library;
    if (argc == 6 && std::string(argv[1]) == "--unknown-child") {
        EventStoreProducerOptions options;
        options.client.socketPath = argv[2];
        options.client.identity = {argv[3], argv[4]};
        options.client.actorId = argv[5];
        options.client.timeoutMs = 300;
        options.retryMs = 2000;
        EventStoreManagementWriter writer(options);
        try { writer.submit("ota_status", managementTopic, managementPayload, managementTs); }
        catch (const std::exception&) { _exit(3); }
        _exit(4);
    }
    int failed = 0;
    failed += !run("management-stable-id-delete", [] { acrossWriters(Profile::DeleteFull, false); });
    failed += !run("management-stable-id-wal", [] { acrossWriters(Profile::WalFull, false); });
    failed += !run("management-distinct-events", distinctEvents);
    failed += !run("management-unknown-delete", [] { unknownReceipt(Profile::DeleteFull, false); });
    failed += !run("management-unknown-wal", [] { unknownReceipt(Profile::WalFull, false); });
    // Require durable confirmation across sessions, not just duplicate-ID rejection.
    failed += !run("management-cross-writer-delete", [] { acrossWriters(Profile::DeleteFull, true); });
    failed += !run("management-cross-writer-wal", [] { acrossWriters(Profile::WalFull, true); });
    failed += !run("management-unknown-restart-delete", [] { unknownReceipt(Profile::DeleteFull, true); });
    failed += !run("management-unknown-restart-wal", [] { unknownReceipt(Profile::WalFull, true); });
    return failed ? 1 : 0;
}
