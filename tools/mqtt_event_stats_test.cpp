#include "edge_gateway/mqtt_event_stats.hpp"
#include "edge_gateway/mqtt_event_stats_factory.hpp"
#include "edge_gateway/event_stats_reader.hpp"
#include "edge_gateway/json_value.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef __linux__
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <set>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using namespace edge_gateway;
using Json = json::JsonValue;
using Status = EventStatsStatus;
using Selection = EventStatsSelection;
using Clock = std::chrono::steady_clock;
using Test = std::pair<std::string, std::function<void()>>;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void checkQuery(const EventStatsQuery& actual, const EventStatsQuery& expected) {
    require(actual.key == expected.key, "query key mismatch: " + actual.key);
    require(actual.context == expected.context, "query context mismatch: " + actual.context);
    require(actual.scope.targetId == expected.scope.targetId, "query target mismatch");
    require(actual.scope.selection == expected.scope.selection, "query selection mismatch");
    require(actual.scope.include == expected.scope.include, "query include mismatch");
    require(actual.scope.exclude == expected.scope.exclude, "query exclude mismatch");
}

EventStatsCacheEntry fresh(const EventStatsQuery& query, std::int64_t count = 7,
    std::int64_t units = 91) {
    EventStatsCacheEntry entry;
    entry.query = query;
    entry.status = Status::Fresh;
    entry.hasValue = true;
    entry.valid = true;
    entry.value = {count, units};
    entry.sampledAtUnixMs = 1788825600000LL;
    entry.ageMs = 42;
    return entry;
}

class FakeSource final : public IEventStatsSource {
public:
    enum class Failure { None, Standard, Nonstandard };
    explicit FakeSource(EventStatsCacheEntry value) : entry(std::move(value)) {}
    EventStatsCacheEntry snapshot(const std::string& key) const override {
        ++calls;
        requestedKey = key;
        if (failure == Failure::Standard) throw std::runtime_error(exceptionText);
        if (failure == Failure::Nonstandard) throw 17;
        return entry;
    }
    EventStatsCacheEntry entry;
    Failure failure = Failure::None;
    std::string exceptionText = "fake source failure";
    mutable int calls = 0;
    mutable std::string requestedKey;
};

EventStatsCacheEntry consume(FakeSource& source, const EventStatsQuery& query) {
    const auto before = source.calls;
    auto result = readMqttEventStats(&source, query);
    require(source.calls == before + 1, "consumer retried or skipped memory snapshot");
    require(source.requestedKey == query.key, "consumer requested wrong cache key");
    checkQuery(result.query, query);
    return result;
}

const Json& field(const Json& object, const std::string& key) {
    const auto* value = object.find(key);
    require(value != nullptr, "health field missing: " + key);
    return *value;
}

Json health(const EventStatsCacheEntry& entry) {
    const auto fields = mqttEventStatsHealthFields(entry);
    require(!fields.empty() && fields.front() == ',', "health fields lost leading comma");
    const auto encoded = std::string("{\"sentinel\":true") + fields + '}';
    auto result = json::JsonParser(encoded, 16, 4096).parse();
    require(field(result, "sentinel").asBool(), "health fields broke enclosing object");
    std::vector<std::string> keys;
    for (const auto& item : result.asObject().values) {
        for (const auto& key : keys) require(key != item.key, "duplicate health field: " + key);
        keys.push_back(item.key);
    }
    for (const auto* key : {"eventLastAckAtMs", "eventLastError", "eventOutboxHealthy", "eventForwarding"})
        require(result.find(key) == nullptr, "statistics overwrote delivery health: " + std::string(key));
    return result;
}

void checkUnavailable(const EventStatsCacheEntry& entry, Status status, bool hasValue) {
    require(!entry.valid && entry.status == status && entry.hasValue == hasValue,
        "invalid snapshot status/value flags changed");
    const auto document = health(entry);
    require(field(document, "eventPendingCount").isNull(), "invalid current count must be null, not zero/history");
    require(!field(document, "eventStatsValid").asBool(), "invalid health advertised as valid");
    require(field(document, "eventStatsHasValue").asBool() == hasValue, "last-known availability lost");
    for (const auto* key : {"eventPendingLastKnownCount", "eventStatsAgeMs", "eventStatsSampledAtMs"})
        require(field(document, key).isNull() == !hasValue, "history nullability mismatch: " + std::string(key));
    if (hasValue) {
        require(field(document, "eventPendingLastKnownCount").asNumber() == entry.value.pendingCount,
            "last-known count was not separately preserved");
        require(field(document, "eventStatsAgeMs").asNumber() == entry.ageMs, "last-known age lost");
        require(field(document, "eventStatsSampledAtMs").asNumber() == entry.sampledAtUnixMs,
            "last-known sample time lost");
    }
}

void checkRejected(const EventStatsCacheEntry& entry) {
    checkUnavailable(entry, Status::Error, false);
    require(!entry.error.empty() && entry.error.size() <= 512, "rejection diagnostic missing/unbounded");
    require(entry.backend == EventStatsBackend::Legacy && entry.identity.storeId.empty() &&
        entry.identity.configGeneration.empty(), "rejection leaked foreign identity");
    require(entry.value.pendingCount == 0 && entry.value.pendingTextUnits == 0,
        "rejection leaked an untrusted count into last-known storage");
}

void driverQueries() {
    checkQuery(mqttDriverTotalStatsQuery(),
        {"driver.total", "main-all-events", {"main", Selection::All, {}, {}}});
    checkQuery(mqttDriverBusinessStatsQuery(),
        {"driver.business", "main-business-events", {"main", Selection::Only, {"alarm", "change"}, {}}});
}

void forwarderQueries(bool waiting) {
    for (const auto* target : {"main", "third-party", "third-party-site-b"}) {
        for (int topics = 0; topics < 4; ++topics) {
            MqttForwardEventConfig config;
            config.targetId = target;
            config.alarmTopic = topics & 1 ? "test/alarm" : "";
            config.changeTopic = topics & 2 ? "test/change" : "";
            std::vector<std::string> include;
            if (waiting || (topics & 1)) include.push_back("alarm");
            if (waiting || (topics & 2)) include.push_back("change");
            const EventStatsQuery expected{waiting ? "forwarder.waiting" : "forwarder.enabled",
                waiting ? "awaiting-business-delegation" : "configured-business-topics",
                {target, Selection::Only, include, {}}};
            for (const bool enabled : {false, true}) {
                config.enabled = enabled;
                checkQuery(mqttForwarderStatsQuery(config, waiting), expected);
            }
        }
    }
}

void freshCounts() {
    const auto query = mqttDriverTotalStatsQuery();
    for (const auto count : {std::int64_t(0), std::int64_t(7), std::numeric_limits<std::int64_t>::max()}) {
        const auto units = count ? std::numeric_limits<std::int64_t>::max() : 0;
        FakeSource source(fresh(query, count, units));
        const auto entry = consume(source, query);
        require(entry.valid && entry.hasValue && entry.status == Status::Fresh, "fresh Legacy count rejected");
        require(entry.value.pendingCount == count && entry.value.pendingTextUnits == units, "int64 count changed");
        require(entry.ageMs == 42 && entry.sampledAtUnixMs == source.entry.sampledAtUnixMs,
            "consumer rewrote sample time/age");
        require(entry.error.empty(), "fresh snapshot acquired an error");
        const auto encoded = mqttEventStatsHealthFields(entry);
        require(encoded.find("\"eventPendingCount\":" + std::to_string(count) + ',') != std::string::npos,
            "health count lost int64 precision");
        const auto document = health(entry);
        require(field(document, "eventStatsStatus").asString() == "fresh", "fresh JSON status mismatch");
        require(field(document, "eventStatsValid").asBool() && field(document, "eventStatsHasValue").asBool(),
            "fresh JSON flags mismatch");
    }
}

void missingSource() {
    const auto query = mqttDriverTotalStatsQuery();
    const auto entry = readMqttEventStats(nullptr, query);
    checkQuery(entry.query, query);
    checkUnavailable(entry, Status::NeverSampled, false);
    require(!entry.error.empty(), "missing source needs a diagnostic");
}

void neverSampled() {
    EventStatsCacheEntry initial;
    initial.query = mqttDriverBusinessStatsQuery();
    FakeSource source(initial);
    const auto entry = consume(source, initial.query);
    checkUnavailable(entry, Status::NeverSampled, false);
    require(field(health(entry), "eventStatsStatus").asString() == "never-sampled", "never-sampled JSON mismatch");
}

void unavailableStatus(Status status, const std::string& name, bool lastKnown) {
    const auto query = mqttDriverBusinessStatsQuery();
    FakeSource source(fresh(query));
    source.entry.status = status;
    source.entry.valid = false;
    source.entry.hasValue = lastKnown;
    source.entry.error = status == Status::Error ? "sample failed" : "";
    const auto entry = consume(source, query);
    checkUnavailable(entry, status, lastKnown);
    require(entry.error == source.entry.error, "sample diagnostic changed");
    require(field(health(entry), "eventStatsStatus").asString() == name, "unavailable JSON status mismatch");
    if (lastKnown) require(entry.value.pendingCount == 7 && entry.value.pendingTextUnits == 91,
        "last successful statistics were overwritten");
}

void sourceException(bool standard) {
    const auto query = mqttDriverTotalStatsQuery();
    FakeSource source(fresh(query));
    source.failure = standard ? FakeSource::Failure::Standard : FakeSource::Failure::Nonstandard;
    source.exceptionText = "snapshot \"failed\"\n" + std::string(700, 'x');
    const auto entry = consume(source, query);
    checkRejected(entry);
    if (standard) require(entry.error == source.exceptionText.substr(0, 512), "exception not bounded/preserved");
}

void normalizedScope() {
    auto query = mqttDriverBusinessStatsQuery();
    query.scope.include = {"change", "alarm", "change"};
    query.scope.exclude = {"full", "diagnostic", "full"};
    FakeSource source(fresh(query));
    source.entry.query.scope.include = {"alarm", "change"};
    source.entry.query.scope.exclude = {"diagnostic", "full"};
    require(consume(source, query).valid, "equivalent normalized scope rejected");
}

void onlyEmpty() {
    MqttForwardEventConfig config;
    const auto query = mqttForwarderStatsQuery(config, false);
    require(query.scope.selection == Selection::Only && query.scope.include.empty(), "empty topics widened to All");
    FakeSource source(fresh(query, 0, 0));
    require(consume(source, query).valid, "valid Only-empty zero rejected");
    source.entry = fresh(query, 1, 1);
    checkRejected(consume(source, query));
    source.entry = fresh(query, 0, 1);
    checkRejected(consume(source, query));
}

void inconsistentFlags() {
    const auto query = mqttDriverTotalStatsQuery();
    for (const auto status : {Status::NeverSampled, Status::Fresh, Status::Error, Status::Stale, Status::Stopped}) {
        for (const bool hasValue : {false, true}) {
            for (const bool valid : {false, true}) {
                FakeSource source(fresh(query));
                source.entry.status = status;
                source.entry.hasValue = hasValue;
                source.entry.valid = valid;
                const auto entry = consume(source, query);
                const bool accepted = status == Status::Fresh && hasValue && valid;
                require(entry.valid == accepted, "status/hasValue/valid matrix accepted inconsistent current count");
                if (!accepted) {
                    checkUnavailable(entry, status == Status::Fresh ? Status::Error : status, hasValue);
                    if (status == Status::Fresh) require(!entry.error.empty(), "inconsistent Fresh has no diagnostic");
                }
            }
        }
    }
}

void freshWithError() {
    const auto query = mqttDriverTotalStatsQuery();
    for (const auto* error : {"sample failed", " ", "error \"quoted\"\n"}) {
        FakeSource source(fresh(query));
        source.entry.error = error;
        const auto entry = consume(source, query);
        checkUnavailable(entry, Status::Error, true);
        require(!entry.error.empty(), "Fresh with an error lost its diagnostic");
        require(entry.value.pendingCount == 7 && entry.value.pendingTextUnits == 91,
            "Fresh with an error lost diagnostic history");
    }
}

void independentHealthSections() {
    for (const bool totalFailed : {false, true}) {
        auto business = fresh(mqttDriverBusinessStatsQuery(), 3, 9);
        auto total = fresh(mqttDriverTotalStatsQuery(), 11, 33);
        auto& failed = totalFailed ? total : business;
        failed.status = Status::Error;
        failed.valid = false;
        failed.error = "independent sample failure";
        const auto events = mqttEventStatsHealthFields(business);
        require(events == mqttEventStatsHealthFields(business, MqttStatsHealthSection::Events),
            "default Events health fields are no longer compatible");
        const auto backlog = mqttEventStatsHealthFields(total, MqttStatsHealthSection::FullBacklog);
        require(!backlog.empty() && backlog.front() == ',', "FullBacklog lost leading comma");
        const auto encoded = std::string("{\"eventLastAckAtMs\":17,\"eventOutboxHealthy\":true,") +
            "\"eventForwarding\":true" + events + backlog + '}';
        const auto document = json::JsonParser(encoded, 16, 4096).parse();
        std::vector<std::string> keys;
        for (const auto& item : document.asObject().values) {
            for (const auto& key : keys) require(key != item.key, "duplicate Events/FullBacklog JSON key: " + key);
            keys.push_back(item.key);
        }
        const auto fragment = std::string("{\"sentinel\":true") + backlog + '}';
        const auto fullOnly = json::JsonParser(fragment, 16, 4096).parse();
        for (const auto& item : fullOnly.asObject().values) {
            if (item.key == "sentinel") continue;
            require(item.key.compare(0, 11, "fullBacklog") == 0, "FullBacklog emitted an event-prefixed field");
            const auto suffix = item.key.substr(11);
            require(document.find("event" + suffix) != nullptr, "FullBacklog field lacks Events counterpart");
        }
        require(field(document, "eventPendingCount").isNull() == !totalFailed &&
            field(document, "fullBacklogPendingCount").isNull() == totalFailed,
            "one failed statistics section invalidated the other current count");
        require(field(document, "eventPendingLastKnownCount").asNumber() == 3 &&
            field(document, "fullBacklogPendingLastKnownCount").asNumber() == 11,
            "independent last-known counts collided");
        require(field(document, "eventStatsValid").asBool() == totalFailed &&
            field(document, "fullBacklogStatsValid").asBool() == !totalFailed,
            "independent health validity collided");
        require(field(document, "eventStatsKey").asString() == "driver.business" &&
            field(document, "fullBacklogStatsKey").asString() == "driver.total",
            "health sections lost their query identities");
        require(field(document, "eventStatsStatus").asString() == (totalFailed ? "fresh" : "error") &&
            field(document, "fullBacklogStatsStatus").asString() == (totalFailed ? "error" : "fresh"),
            "health section status collision");
        require(field(document, "eventLastAckAtMs").asNumber() == 17 &&
            field(document, "eventOutboxHealthy").asBool() && field(document, "eventForwarding").asBool(),
            "statistics health changed delivery ownership or acknowledgement fields");
    }
}

void jsonEscaping() {
    auto entry = fresh(mqttDriverBusinessStatsQuery());
    std::string value = "quote\" backslash\\ ";
    for (int c = 0; c < 32; ++c) value.push_back(static_cast<char>(c));
    value += " utf8-\xe4\xb8\xad\xe6\x96\x87";
    entry.error = value;
    entry.query.key = value;
    entry.query.context = value;
    entry.query.scope.targetId = value;
    entry.query.scope.include = {value, "alarm"};
    entry.query.scope.exclude = {"change", value};
    const auto document = health(entry);
    for (const auto* key : {"eventStatsError", "eventStatsKey", "eventStatsContext", "eventStatsTargetId"})
        require(field(document, key).asString() == value, "JSON string did not round-trip: " + std::string(key));
    const auto& include = field(document, "eventStatsInclude").asArray().values;
    const auto& exclude = field(document, "eventStatsExclude").asArray().values;
    require(include.size() == 2 && include[0]->asString() == value && include[1]->asString() == "alarm",
        "JSON include list did not round-trip");
    require(exclude.size() == 2 && exclude[0]->asString() == "change" && exclude[1]->asString() == value,
        "JSON exclude list did not round-trip");
    require(field(document, "eventStatsSelection").asString() == "only", "Only health scope lost");
    const auto encoded = mqttEventStatsHealthFields(entry);
    for (const unsigned char c : encoded) require(c >= 32, "raw control character escaped JSON string");
    entry.query = mqttDriverTotalStatsQuery();
    const auto all = health(entry);
    require(field(all, "eventStatsSelection").asString() == "all" &&
        field(all, "eventStatsInclude").asArray().values.empty() &&
        field(all, "eventStatsExclude").asArray().values.empty(), "All health scope lost");
}

void boundedError(bool exception, bool utf8) {
    const auto query = mqttDriverTotalStatsQuery();
    FakeSource source(fresh(query));
    source.entry.status = Status::Error;
    source.entry.valid = false;
    source.entry.error = utf8 ? std::string(511, 'x') + "\xe4\xb8\xad" : std::string(700, 'x');
    if (exception) {
        source.failure = FakeSource::Failure::Standard;
        source.exceptionText = source.entry.error;
    }
    const auto entry = consume(source, query);
    require(!entry.valid && !entry.error.empty() && entry.error.size() <= 512, "error text must be bounded");
    // A byte limit must not turn valid source text into an invalid UTF-8 health document.
    const auto document = health(entry);
    require(field(document, "eventStatsError").asString() == entry.error, "bounded error did not round-trip");
}

void invalidUtf8Diagnostic() {
    const std::vector<std::pair<std::string, std::string>> cases{
        {"isolated-continuation", "\x80"},
        {"truncated-two-byte", "\xc2"},
        {"truncated-three-byte", "\xe4\xb8"},
        {"truncated-four-byte", "\xf0\x9f\x92"},
        {"overlong-two-byte", "\xc0\xaf"},
        {"overlong-three-byte", "\xe0\x80\xaf"},
        {"overlong-four-byte", "\xf0\x80\x80\xaf"},
        {"surrogate-high", "\xed\xa0\x80"},
        {"surrogate-low", "\xed\xbf\xbf"},
        {"above-unicode-maximum", "\xf4\x90\x80\x80"}
    };
    const auto query = mqttDriverTotalStatsQuery();
    for (const auto& item : cases) {
        for (const bool atLimit : {false, true}) {
            const auto prefix = atLimit ? std::string(511, 'x') : std::string("diagnostic-\xe4\xb8\xad-\"\n");
            const auto expected = prefix + std::string(atLimit ? 1 : item.second.size(), '?');
            for (const bool exception : {false, true}) {
                const auto label = item.first + (atLimit ? " at-limit" : " short") +
                    (exception ? " exception" : " source");
                try {
                    FakeSource source(fresh(query));
                    source.entry.status = Status::Error;
                    source.entry.valid = false;
                    source.entry.error = prefix + item.second;
                    if (exception) {
                        source.failure = FakeSource::Failure::Standard;
                        source.exceptionText = source.entry.error;
                    }
                    const auto entry = consume(source, query);
                    checkUnavailable(entry, Status::Error, !exception);
                    require(entry.error == expected && entry.error.size() <= 512,
                        "invalid bytes not replaced by bounded question marks, or valid prefix lost");
                    const auto document = health(entry);
                    require(field(document, "eventStatsError").asString() == expected &&
                        field(document, "eventStatsStatus").asString() == "error",
                        "sanitized diagnostic did not strict-parse as an error");
                    if (exception) checkRejected(entry);
                    else require(entry.value.pendingCount == 7 && entry.value.pendingTextUnits == 91,
                        "sanitizing diagnostics changed last-known statistics");
                } catch (const std::exception& error) {
                    throw std::runtime_error(label + ": " + error.what());
                }
            }
        }
        FakeSource source(fresh(query));
        source.entry.error = item.second;
        const auto entry = consume(source, query);
        checkUnavailable(entry, Status::Error, true);
        require(!entry.error.empty() && entry.error.size() <= 512,
            item.first + ": invalid UTF-8 error was erased into false freshness");
        require(field(health(entry), "eventStatsStatus").asString() == "error",
            item.first + ": invalid UTF-8 error was reported as Fresh");
    }
}

void addFakeTests(std::vector<Test>& tests) {
    tests.push_back({"query-driver-total-all-business-only", driverQueries});
    tests.push_back({"query-forwarder-waiting-target-all-topic-combinations", [] { forwarderQueries(true); }});
    tests.push_back({"query-forwarder-enabled-target-all-topic-combinations", [] { forwarderQueries(false); }});
    tests.push_back({"fake-fresh-zero-positive-int64", freshCounts});
    tests.push_back({"fake-null-source-no-valid-zero", missingSource});
    tests.push_back({"fake-never-sampled-no-valid-zero", neverSampled});
    tests.push_back({"fake-error-without-last-known", [] { unavailableStatus(Status::Error, "error", false); }});
    tests.push_back({"fake-error-preserves-last-known", [] { unavailableStatus(Status::Error, "error", true); }});
    tests.push_back({"fake-stale-preserves-last-known", [] { unavailableStatus(Status::Stale, "stale", true); }});
    tests.push_back({"fake-stopped-preserves-last-known", [] { unavailableStatus(Status::Stopped, "stopped", true); }});
    tests.push_back({"fake-stopped-before-first-sample", [] { unavailableStatus(Status::Stopped, "stopped", false); }});
    tests.push_back({"fake-standard-exception-bounded-no-history", [] { sourceException(true); }});
    tests.push_back({"fake-nonstandard-exception-no-history", [] { sourceException(false); }});
    tests.push_back({"fake-normalized-scope-preserves-requested-context", normalizedScope});
    tests.push_back({"fake-only-empty-zero-not-all", onlyEmpty});
    tests.push_back({"fake-status-hasvalue-valid-flag-matrix", inconsistentFlags});
    tests.push_back({"fake-fresh-nonempty-error-invalid-last-known-only", freshWithError});

    using Mutation = std::pair<std::string, std::function<void(EventStatsCacheEntry&)>>;
    const std::vector<Mutation> mutations{
        {"key", [](EventStatsCacheEntry& e) { e.query.key = "driver.other"; }},
        {"context", [](EventStatsCacheEntry& e) { e.query.context = "other-context"; }},
        {"target", [](EventStatsCacheEntry& e) { e.query.scope.targetId = "third-party"; }},
        {"selection", [](EventStatsCacheEntry& e) { e.query.scope.selection = Selection::All; e.query.scope.include.clear(); }},
        {"include", [](EventStatsCacheEntry& e) { e.query.scope.include = {"alarm"}; }},
        {"exclude", [](EventStatsCacheEntry& e) { e.query.scope.exclude = {"change"}; }},
        {"backend-ipc", [](EventStatsCacheEntry& e) { e.backend = EventStatsBackend::Ipc; }},
        {"identity-store", [](EventStatsCacheEntry& e) { e.identity.storeId = "foreign-store"; }},
        {"identity-generation", [](EventStatsCacheEntry& e) { e.identity.configGeneration = "foreign-generation"; }},
        {"invalid-scope-empty-target", [](EventStatsCacheEntry& e) { e.query.scope.targetId.clear(); }},
        {"invalid-scope-selection", [](EventStatsCacheEntry& e) { e.query.scope.selection = static_cast<Selection>(99); }},
        {"invalid-scope-nul-type", [](EventStatsCacheEntry& e) { e.query.scope.include = {std::string("a\0b", 3)}; }},
        {"negative-count", [](EventStatsCacheEntry& e) { e.value.pendingCount = -1; }},
        {"negative-text-units", [](EventStatsCacheEntry& e) { e.value.pendingTextUnits = -1; }},
        {"negative-age", [](EventStatsCacheEntry& e) { e.ageMs = -1; }},
        {"zero-count-nonzero-units", [](EventStatsCacheEntry& e) { e.value.pendingCount = 0; }}
    };
    for (const auto& mutation : mutations) {
        tests.push_back({"fake-reject-" + mutation.first, [mutation] {
            const auto query = mqttDriverBusinessStatsQuery();
            for (const auto status : {Status::Fresh, Status::Error, Status::Stale, Status::Stopped}) {
                FakeSource source(fresh(query));
                source.entry.status = status;
                source.entry.valid = status == Status::Fresh;
                mutation.second(source.entry);
                checkRejected(consume(source, query));
            }
        }});
    }
    tests.push_back({"health-json-quotes-all-controls-utf8-scope-arrays", jsonEscaping});
    tests.push_back({"health-events-full-backlog-independent-no-duplicate-event-prefix", independentHealthSections});
    tests.push_back({"health-json-bounded-cache-error", [] { boundedError(false, false); }});
    tests.push_back({"health-json-bounded-cache-error-utf8-boundary", [] { boundedError(false, true); }});
    tests.push_back({"health-json-bounded-exception-utf8-boundary", [] { boundedError(true, true); }});
    tests.push_back({"health-json-invalid-utf8-source-exception-no-false-fresh", invalidUtf8Diagnostic});
}

#ifdef __linux__
using Profile = MqttEventOutbox::StorageProfile;
constexpr auto kWait = std::chrono::seconds(15);
std::string testLibrary;

template <typename Predicate> void eventually(Predicate predicate, const std::string& label,
    std::chrono::milliseconds timeout = std::chrono::duration_cast<std::chrono::milliseconds>(kWait)) {
    const auto deadline = Clock::now() + timeout;
    while (!predicate()) {
        require(Clock::now() < deadline, "poll timeout: " + label);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

std::set<std::string> children(const std::string& path) {
    auto* directory = opendir(path.c_str());
    require(directory != nullptr, "cannot inspect directory: " + path);
    std::set<std::string> result;
    while (const auto* item = readdir(directory)) {
        if (std::strcmp(item->d_name, ".") && std::strcmp(item->d_name, "..")) result.insert(item->d_name);
    }
    closedir(directory);
    return result;
}

struct Directory {
    std::string path;
    Directory() {
        char pattern[] = "/tmp/mqtt-stats-XXXXXX";
        const auto* created = mkdtemp(pattern);
        require(created != nullptr, "mkdtemp failed");
        path = created;
    }
    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;
    static void removeChildren(const std::string& root) noexcept {
        // This tree is owned by mkdtemp; lstat prevents following aliases out of it.
        if (auto* directory = opendir(root.c_str())) {
            while (const auto* item = readdir(directory)) {
                if (!std::strcmp(item->d_name, ".") || !std::strcmp(item->d_name, "..")) continue;
                const auto child = root + "/" + item->d_name;
                struct stat info{};
                if (lstat(child.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
                    removeChildren(child);
                    rmdir(child.c_str());
                } else unlink(child.c_str());
            }
            closedir(directory);
        }
    }
    ~Directory() { removeChildren(path); rmdir(path.c_str()); }
    static void collect(const std::string& root, std::set<std::string>& result) {
        for (const auto& name : children(root)) {
            const auto child = root + "/" + name;
            struct stat info{};
            require(lstat(child.c_str(), &info) == 0, "cannot stat fixture entry");
            result.insert(child);
            if (S_ISDIR(info.st_mode)) collect(child, result);
        }
    }
    std::set<std::string> files() const {
        std::set<std::string> result;
        collect(path, result);
        return result;
    }
};

class CurrentDirectory {
public:
    explicit CurrentDirectory(const std::string& path) : previous_(open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC)) {
        require(previous_ >= 0, "cannot retain working directory");
        if (chdir(path.c_str()) != 0) { close(previous_); throw std::runtime_error("fixture chdir failed"); }
    }
    ~CurrentDirectory() { (void)fchdir(previous_); close(previous_); }
    CurrentDirectory(const CurrentDirectory&) = delete;
    CurrentDirectory& operator=(const CurrentDirectory&) = delete;
private:
    int previous_;
};

std::string fileBytes(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot read fixture: " + path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct Lab {
    Directory directory;
    MqttConfig config;
    std::unique_ptr<MqttEventOutbox> writer;
    Lab() {
        config.eventOutboxSqlitePath = directory.path + "/events.db";
        config.eventOutboxSqliteLibraryPath = testLibrary;
        writer = std::make_unique<MqttEventOutbox>(config.eventOutboxSqlitePath, testLibrary,
            12, 24, 100, 0, Profile::DeleteNormal);
    }
    std::int64_t add(const std::string& id, const std::string& target, const std::string& type,
        const std::string& payload = "xx") {
        MqttEventOutbox::EventMessage event;
        event.eventId = id;
        event.targetId = target;
        event.eventType = type;
        event.topic = "t";
        event.payload = payload;
        event.eventTs = 1788825600000LL;
        const auto ids = writer->enqueueBatch({event});
        require(ids.size() == 1 && ids.front() > 0, "fixture enqueue failed");
        return ids.front();
    }
};

EventStatsCacheEntry waitCount(IEventStatsSource& source, const EventStatsQuery& query,
    std::int64_t count, std::int64_t units) {
    EventStatsCacheEntry entry;
    eventually([&] {
        entry = readMqttEventStats(&source, query);
        return entry.valid && entry.value.pendingCount == count && entry.value.pendingTextUnits == units;
    }, query.key + " count=" + std::to_string(count) + " units=" + std::to_string(units));
    checkQuery(entry.query, query);
    require(entry.backend == EventStatsBackend::Legacy && entry.identity.storeId.empty() &&
        entry.identity.configGeneration.empty(), "factory returned non-Legacy identity");
    require(entry.status == Status::Fresh && entry.hasValue && entry.sampledAtUnixMs > 0 &&
        entry.ageMs >= 0 && entry.error.empty(), "factory sample metadata invalid");
    return entry;
}

std::unique_ptr<IEventStatsSource> makeSource(const MqttConfig& config, const std::vector<EventStatsQuery>& queries) {
    auto source = makeLegacyMqttEventStatsSource(config, queries);
    require(source != nullptr, "factory must return a source even when sampling unavailable");
    return source;
}

void realSampling() {
    Lab lab;
    const auto alarmId = lab.add("main-alarm", "main", "alarm");
    lab.add("main-change", "main", "change");
    lab.add("main-full", "main", "full");
    lab.add("main-unicode", "main", "diagnostic", "\xe4\xb8\xad");
    lab.add("third-alarm", "third-party", "alarm");
    const auto sentId = lab.add("already-sent", "main", "alarm");
    lab.writer->markSent(sentId, 1788825600001LL);
    const auto total = mqttDriverTotalStatsQuery();
    const auto business = mqttDriverBusinessStatsQuery();
    auto source = makeSource(lab.config, {total, business});
    waitCount(*source, total, 4, 11);
    waitCount(*source, business, 2, 6);
    lab.writer->markSent(alarmId, 1788825600002LL);
    lab.add("new-main-change", "main", "change", "xxxx");
    waitCount(*source, total, 4, 13);
    waitCount(*source, business, 2, 8);
    require(lab.writer->pendingCount("third-party") == 1, "sampling mutated a foreign target");
}

void realForwarderScopes() {
    Lab lab;
    lab.add("main-alarm", "main", "alarm");
    lab.add("main-change", "main", "change");
    lab.add("third-alarm", "third-party", "alarm");
    lab.add("third-change", "third-party", "change");
    lab.add("third-full", "third-party", "full");
    MqttForwardEventConfig config;
    const auto waiting = mqttForwarderStatsQuery(config, true);
    const auto empty = mqttForwarderStatsQuery(config, false);
    {
        auto source = makeSource(lab.config, {waiting, empty});
        waitCount(*source, waiting, 2, 6);
        waitCount(*source, empty, 0, 0);
    }
    for (const auto* target : {"main", "third-party"}) {
        for (int topics = 1; topics < 4; ++topics) {
            config.targetId = target;
            config.alarmTopic = topics & 1 ? "test/alarm" : "";
            config.changeTopic = topics & 2 ? "test/change" : "";
            const auto query = mqttForwarderStatsQuery(config, false);
            auto source = makeSource(lab.config, {query});
            const auto count = topics == 3 ? 2 : 1;
            waitCount(*source, query, count, count * 3);
        }
    }
}

void realRelativePath() {
    Lab lab;
    Lab decoy;
    lab.add("original", "main", "alarm");
    for (int i = 0; i < 5; ++i) decoy.add("decoy-" + std::to_string(i), "main", "alarm");
    CurrentDirectory cwd(lab.directory.path);
    auto config = lab.config;
    config.eventOutboxSqlitePath = "./events.db";
    const auto query = mqttDriverTotalStatsQuery();
    auto source = makeSource(config, {query});
    require(chdir(decoy.directory.path.c_str()) == 0, "cannot switch to decoy working directory");
    waitCount(*source, query, 1, 3);
    lab.add("original-later", "main", "change");
    waitCount(*source, query, 2, 6);
    require(decoy.writer->pendingCount() == 5, "relative reader touched decoy database");
}

void missingDatabases() {
    Directory directory;
    CurrentDirectory cwd(directory.path);
    const auto files = directory.files();
    const auto threads = children("/proc/self/task");
    const auto query = mqttDriverTotalStatsQuery();
    for (const auto& path : {directory.path + "/missing.db", directory.path + "/missing-parent/events.db",
            std::string("./relative-missing.db"), std::string()}) {
        MqttConfig config;
        config.eventOutboxSqlitePath = path;
        config.eventOutboxSqliteLibraryPath = testLibrary;
        auto source = makeSource(config, {query});
        checkRejected(readMqttEventStats(source.get(), query));
        source.reset();
        require(directory.files() == files, "missing database factory created a database/directory/actor lock");
        require(children("/proc/self/task") == threads, "unavailable factory left a worker thread");
    }
    Lab existing;
    existing.add("existing", "main", "alarm");
    auto config = existing.config;
    config.eventOutboxSqlitePath += std::string("\0ignored", 8);
    auto source = makeSource(config, {query});
    checkRejected(readMqttEventStats(source.get(), query));
}

void emptyDatabaseNoSchema() {
    Directory directory;
    MqttConfig config;
    config.eventOutboxSqlitePath = directory.path + "/empty.db";
    config.eventOutboxSqliteLibraryPath = testLibrary;
    { std::ofstream file(config.eventOutboxSqlitePath, std::ios::binary); require(file.good(), "cannot create empty fixture"); }
    const auto files = directory.files();
    const auto query = mqttDriverTotalStatsQuery();
    auto source = makeSource(config, {query});
    EventStatsCacheEntry entry;
    eventually([&] { entry = readMqttEventStats(source.get(), query); return entry.status == Status::Error; },
        "empty database read failure");
    checkRejected(entry);
    source.reset();
    require(directory.files() == files && fileBytes(config.eventOutboxSqlitePath).empty(),
        "read-only factory initialized an empty database or created sidecars");
}

class HeldLock {
public:
    explicit HeldLock(const std::string& path) : fd_(open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) {
        require(fd_ >= 0, "cannot create fixture actor lock");
        if (flock(fd_, LOCK_EX | LOCK_NB) != 0) { close(fd_); throw std::runtime_error("cannot hold fixture actor lock"); }
    }
    ~HeldLock() { close(fd_); }
    HeldLock(const HeldLock&) = delete;
    HeldLock& operator=(const HeldLock&) = delete;
private:
    int fd_;
};

void readOnlyNoActors() {
    Lab lab;
    lab.add("pending", "main", "alarm");
    const auto locks = lab.directory.path + "/.event-store-clients";
    const auto store = locks + "/73746174732d74657374"; // Hex encoding of stats-test.
    require(mkdir(locks.c_str(), 0700) == 0 && mkdir(store.c_str(), 0700) == 0, "cannot create private actor fixture");
    const auto actor = store + "/sender-6d61696e.lock";
    HeldLock held(actor);
    const auto files = lab.directory.files();
    const auto database = fileBytes(lab.config.eventOutboxSqlitePath);
    const auto settings = lab.writer->storageSettings();
    require(settings.journalMode == "delete" && settings.synchronous == 1, "fixture is not DELETE/NORMAL");
    const auto query = mqttDriverTotalStatsQuery();
    auto source = makeSource(lab.config, {query});
    waitCount(*source, query, 1, 3);
    const auto after = lab.writer->storageSettings();
    require(after.journalMode == "delete" && after.synchronous == 1 &&
        after.busyTimeoutMs == settings.busyTimeoutMs && after.sqliteLibraryPath == settings.sqliteLibraryPath,
        "statistics changed live writer configuration");
    require(lab.directory.files() == files && fileBytes(lab.config.eventOutboxSqlitePath) == database,
        "statistics created actors/sidecars or modified database bytes");
    const int probe = open(actor.c_str(), O_RDWR | O_CLOEXEC);
    require(probe >= 0, "actor sentinel disappeared");
    const int locked = flock(probe, LOCK_EX | LOCK_NB);
    const int error = errno;
    close(probe);
    require(locked != 0 && (error == EAGAIN || error == EWOULDBLOCK), "statistics disturbed existing actor lock");
    source.reset();
    require(lab.directory.files() == files && fileBytes(lab.config.eventOutboxSqlitePath) == database,
        "source destruction wrote SQLite or actor files");
    const auto stopped = lab.writer->storageSettings();
    require(stopped.journalMode == "delete" && stopped.synchronous == 1, "source release changed writer durability");
    require(lab.writer->pendingCount() == 1, "statistics acknowledged or removed pending data");
}

void releasesWorker() {
    Lab lab;
    lab.add("pending", "main", "alarm");
    const auto query = mqttDriverTotalStatsQuery();
    const auto baseline = children("/proc/self/task");
    for (int iteration = 0; iteration < 3; ++iteration) {
        auto source = makeSource(lab.config, {query});
        waitCount(*source, query, 1, 3);
        require(children("/proc/self/task").size() == baseline.size() + 1, "factory did not own exactly one sampling worker");
        const auto before = Clock::now();
        source.reset();
        require(Clock::now() - before < kWait, "source destructor did not finish within bounded wait");
        // Linux may briefly retain a joined task's proc entry while finishing exit bookkeeping.
        eventually([&] { return children("/proc/self/task") == baseline; },
            "source release must join its worker", std::chrono::milliseconds(250));
    }
    lab.add("writer-survives", "main", "change");
    require(lab.writer->pendingCount() == 2, "source destruction damaged the live writer");
}

void realFailureAndRecovery() {
    Lab lab;
    lab.add("pending", "main", "alarm");
    const auto query = mqttDriverTotalStatsQuery();
    auto source = makeSource(lab.config, {query});
    const auto first = waitCount(*source, query, 1, 3);
    const auto moved = lab.directory.path + "/parked.db";
    require(std::rename(lab.config.eventOutboxSqlitePath.c_str(), moved.c_str()) == 0, "cannot hide private database");
    EventStatsCacheEntry entry;
    eventually([&] { entry = readMqttEventStats(source.get(), query); return entry.status == Status::Error; },
        "factory missing-file error after successful sample");
    checkUnavailable(entry, Status::Error, true);
    require(entry.value.pendingCount == 1 && entry.value.pendingTextUnits == 3 &&
        entry.sampledAtUnixMs >= first.sampledAtUnixMs && !entry.error.empty(), "failed poll lost last success");
    struct stat info{};
    require(lstat(lab.config.eventOutboxSqlitePath.c_str(), &info) != 0 && errno == ENOENT,
        "failed poll recreated missing database");
    require(std::rename(moved.c_str(), lab.config.eventOutboxSqlitePath.c_str()) == 0, "cannot restore private database");
    lab.add("recovered", "main", "change");
    const auto recovered = waitCount(*source, query, 2, 6);
    require(recovered.sampledAtUnixMs > entry.sampledAtUnixMs, "recovery did not advance sample timestamp");
}

void invalidFactoryQueries() {
    Lab lab;
    const auto query = mqttDriverTotalStatsQuery();
    auto bad = query;
    bad.scope.targetId.clear();
    const auto files = lab.directory.files();
    const auto baseline = children("/proc/self/task");
    for (const auto& queries : std::vector<std::vector<EventStatsQuery>>{{}, {query, query}, {bad}}) {
        auto source = makeSource(lab.config, queries);
        checkRejected(readMqttEventStats(source.get(), query));
        source.reset();
        require(lab.directory.files() == files && children("/proc/self/task") == baseline,
            "invalid factory configuration created files or leaked a thread");
    }
}

void addLinuxTests(std::vector<Test>& tests) {
    tests.push_back({"linux-factory-real-main-total-business-text-units-sent-refresh", realSampling});
    tests.push_back({"linux-factory-real-forwarder-target-topic-only-empty", realForwarderScopes});
    tests.push_back({"linux-factory-relative-path-resolved-before-cwd-change", realRelativePath});
    tests.push_back({"linux-factory-missing-relative-parent-nul-path-no-create", missingDatabases});
    tests.push_back({"linux-factory-empty-database-no-schema-no-sidecars", emptyDatabaseNoSchema});
    tests.push_back({"linux-factory-readonly-no-actor-locks-delete-normal-preserved", readOnlyNoActors});
    tests.push_back({"linux-factory-source-release-joins-worker-writer-survives", releasesWorker});
    tests.push_back({"linux-factory-read-error-last-known-no-create-recovery", realFailureAndRecovery});
    tests.push_back({"linux-factory-invalid-queries-error-source-no-thread", invalidFactoryQueries});
}
#endif

bool run(const Test& test) {
#ifdef __linux__
    // Match the reader tests: each group isolates SQLite globals and has a deadlock watchdog.
    std::cout.flush();
    std::cerr.flush();
    const auto child = fork();
    require(child >= 0, "test runner fork failed");
    if (child != 0) {
        int status = 0;
        pid_t waited;
        do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
        require(waited == child, "test runner waitpid failed");
        if (WIFSIGNALED(status)) std::cerr << "FAIL " << test.first << ": signal " << WTERMSIG(status) << std::endl;
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    alarm(60);
#endif
    bool passed = false;
    try {
        test.second();
        std::cout << "PASS " << test.first << std::endl;
        passed = true;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << test.first << ": " << error.what() << std::endl;
    } catch (...) {
        std::cerr << "FAIL " << test.first << ": nonstandard exception" << std::endl;
    }
#ifdef __linux__
    _exit(passed ? 0 : 1);
#endif
    return passed;
}
} // namespace

int main() {
    std::vector<Test> tests;
    addFakeTests(tests);
    tests.push_back({"explicit-ipc-store-identity", [] {
        const auto query = mqttDriverBusinessStatsQuery();
        FakeSource source(fresh(query));
        source.entry.backend = EventStatsBackend::Ipc;
        source.entry.identity = {"local-store", "generation-1"};
        require(!readMqttEventStats(&source, query).valid, "legacy accepted an IPC sample");
        require(readMqttEventStats(&source, query, {"local-store", "generation-1"}).valid,
            "matching explicit IPC source was rejected");
        require(!readMqttEventStats(&source, query, {"other-store", "generation-1"}).valid,
            "wrong IPC store accepted");
        require(!readMqttEventStats(&source, query, {"local-store", "generation-2"}).valid,
            "wrong IPC generation accepted");
        require(!readMqttEventStats(&source, query, {"local-store", ""}).valid,
            "partial expected identity accepted");
    }});
#ifdef __linux__
    if (const auto* library = std::getenv("SQLITE_LIBRARY")) testLibrary = library;
    addLinuxTests(tests);
#else
    std::cout << "SKIP linux-factory-groups: Linux required" << std::endl;
#endif
    std::size_t failed = 0;
    for (const auto& test : tests) {
        try { failed += !run(test); }
        catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL " << test.first << " runner: " << error.what() << std::endl;
        }
    }
    std::cout << "SUMMARY groups=" << tests.size() << " passed=" << tests.size() - failed
              << " failed=" << failed << std::endl;
    return failed ? 1 : 0;
}
