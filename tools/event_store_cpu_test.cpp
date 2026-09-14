#define main history_runtime_regression_main
#include "event_store_history_runtime_test.cpp"
#undef main
#include "edge_gateway/event_history_projection.hpp"
#include <atomic>
#include <ctime>

namespace {
std::atomic<unsigned long long> pathChecks{0};
unsigned long long replaceAt = 0;
std::function<void()> replaceFile;
std::exception_ptr replacementFailure;
double cpuSeconds() {
    timespec value{};
    require(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) == 0, "CPU clock");
    return value.tv_sec + value.tv_nsec / 1e9;
}
void report(const char* name, double cpu, Clock::time_point wall, unsigned long long paths) {
    std::cout << name << " cpu_ms=" << (cpuSeconds() - cpu) * 1000
              << " wall_ms=" << std::chrono::duration<double, std::milli>(Clock::now() - wall).count()
              << " realpath_calls=" << pathChecks.load() - paths << '\n';
}
void prepareHistoryWal(const std::string& path) {
    std::ofstream(path).close();
    Sql history(path);
    history.run("PRAGMA journal_mode=WAL;");
    require(history.scalar("SELECT count(*) FROM pragma_journal_mode WHERE journal_mode='wal';") == 1,
        "history WAL not enabled");
    require(history.scalar("PRAGMA synchronous;") == 2, "history FULL not enabled");
}
}

// Link with -Wl,--wrap=realpath to count the actual physical identity checks.
extern "C" char* __real_realpath(const char*, char*);
extern "C" char* __wrap_realpath(const char* path, char* resolved) {
    const auto count = ++pathChecks;
    if (replaceAt && count == replaceAt) {
        replaceAt = 0;
        try { replaceFile(); }
        catch (...) { replacementFailure = std::current_exception(); }
    }
    return __real_realpath(path, resolved);
}

namespace {
void replacementDuringPage(bool auditing, bool missing, bool wal) {
    Temp temp;
    const auto options = temp.options(Outbox::StorageProfile::WalFull);
    Outbox box(options.databasePath, "", 12, 24, 100, 0, options.profile);
    EventStoreDatabase source(box, options.identity, options.producers);
    if (wal) prepareHistoryWal(options.historyPath);
    EventHistoryProjection projection(source, options.historyPath);
    std::vector<EventStoreJournalRow> rows(64);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        rows[i].id = i + 1;
        rows[i].local.kind = "change";
    }
    if (!auditing) {
        rows.front().local.kind = "alarm";
        rows.front().local.event.eventId = "rollback-alarm";
    }
    if (missing) { rows.back().local.kind = "alarm"; rows.back().local.event.eventId = "missing"; }
    // The fifth realpath is the history-file check at pre-COMMIT / audit return.
    // Source replacement here must abort the open transaction / verified prefix.
    const auto startingPaths = pathChecks.load();
    bool sawUncommittedAlarm = false;
    replacementFailure = {};
    replaceFile = [&] {
        if (!auditing) {
            // A separate reader cannot observe the alarm before COMMIT.
            Sql history(options.historyPath);
            sawUncommittedAlarm = history.scalar("SELECT COUNT(*) FROM alarm_events;") == 0;
        }
        require(::rename(options.databasePath.c_str(), (options.databasePath + ".saved").c_str()) == 0, "replace source");
        std::ofstream(options.databasePath) << "replacement";
    };
    replaceAt = startingPaths + 5;
    rejects([&] {
        if (auditing) projection.auditRows(rows, 0);
        else projection.projectRows(rows, 0);
    }, "physical");
    require(replaceAt == 0, "replacement hook not reached");
    if (replacementFailure) std::rethrow_exception(replacementFailure);
    require(auditing || sawUncommittedAlarm, "pre-commit hook not reached");
    replaceFile = {};
    Sql history(options.historyPath);
    require(history.scalar("SELECT last_contiguous_journal_id FROM alarm_projection_meta;") == 0,
        "replacement advanced history watermark");
    require(history.scalar("SELECT COUNT(*) FROM alarm_events;") == 0, "replacement committed history rows");
}
}

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string(argv[1]) == "--boundaries-only") {
            for (bool wal : {false, true}) {
                replacementDuringPage(false, false, wal);
                replacementDuringPage(true, false, wal);
                replacementDuringPage(true, true, wal);
            }
            std::cout << "PASS physical boundaries (6 cases)\n";
            return 0;
        }
        const bool enforce = argc > 1 && std::string(argv[1]) == "--check";
        std::cout << "source=wal/full history=wal/full CPU=process-clock filesystem=/tmp\n";
        for (bool enabled : {false, true}) {
            Temp temp;
            auto options = temp.options(Outbox::StorageProfile::WalFull);
            options.historyBatchSize = 32;
            options.historyPollIntervalMs = 200;
            if (!enabled) options.historyPath.clear();
            else prepareHistoryWal(options.historyPath);
            EventStoreRuntime runtime(options);
            runtime.start();
            registerProducer(options);
            auto queryPaths = pathChecks.load(); auto queryWall = Clock::now(); auto queryCpu = cpuSeconds();
            for (int i = 0; i < 100; ++i) call(options, "Hello");
            report(enabled ? "ipc-hello-history-100" : "ipc-hello-no-history-100", queryCpu, queryWall, queryPaths);
            auto paths = pathChecks.load(); auto wall = Clock::now(); auto cpu = cpuSeconds();
            std::this_thread::sleep_for(std::chrono::seconds(1));
            report(enabled ? "idle-history" : "idle-no-history", cpu, wall, paths);
            paths = pathChecks.load(); wall = Clock::now(); cpu = cpuSeconds();
            for (int i = 1; i <= 100; ++i) append(options, i, "change");
            if (enabled) waitThrough(runtime, 100);
            report(enabled ? "ipc-change-history-100" : "ipc-change-no-history-100", cpu, wall, paths);
            require(runtime.historyStatus().failures == 0, "unexpected projection failure");
            runtime.stop();
        }
        Temp temp;
        auto options = temp.options(Outbox::StorageProfile::WalFull);
        Outbox box(options.databasePath, "", 12, 24, 100, 0, options.profile);
        const auto settings = box.storageSettings();
        require(settings.journalMode == "wal" && settings.synchronous == 2, "source WAL/FULL");
        std::cout << "sqlite=" << settings.sqliteVersion << '\n';
        EventStoreDatabase source(box, options.identity, options.producers);
        prepareHistoryWal(options.historyPath);
        EventHistoryProjection projection(source, options.historyPath);
        auto queryPaths = pathChecks.load(); auto queryWall = Clock::now(); auto queryCpu = cpuSeconds();
        for (int i = 0; i < 8192; ++i) (void)source.projectionCursor();
        report("source-cursor-query-8192", queryCpu, queryWall, queryPaths);
        queryPaths = pathChecks.load(); queryWall = Clock::now(); queryCpu = cpuSeconds();
        for (int i = 0; i < 8192; ++i) (void)box.storageSettings();
        report("storage-settings-query-8192", queryCpu, queryWall, queryPaths);
        queryPaths = pathChecks.load(); queryWall = Clock::now(); queryCpu = cpuSeconds();
        for (int i = 0; i < 8192; ++i) (void)projection.watermark();
        report("history-watermark-query-8192", queryCpu, queryWall, queryPaths);
        auto paths = pathChecks.load(); auto wall = Clock::now(); auto cpu = cpuSeconds();
        std::int64_t cursor = 0;
        for (int page = 0; page < 128; ++page) {
            std::vector<EventStoreJournalRow> rows(64);
            for (std::size_t i = 0; i < rows.size(); ++i) {
                rows[i].id = cursor + i + 1;
                rows[i].local.kind = "change";
            }
            cursor = projection.projectRows(rows, cursor);
        }
        report("projection-change-8192", cpu, wall, paths);
        require(projection.watermark() == 8192, "projection watermark");
        if (enforce) require(pathChecks.load() - paths <= 128 * 8 + 4,
            "physical path checks scale per row instead of per bounded page");
        paths = pathChecks.load(); wall = Clock::now(); cpu = cpuSeconds();
        for (int page = 0; page < 128; ++page) {
            std::vector<EventStoreJournalRow> rows(64);
            for (std::size_t i = 0; i < rows.size(); ++i) {
                rows[i].id = page * 64 + i + 1;
                rows[i].local.kind = "change";
            }
            require(projection.auditRows(rows, page * 64) == (page + 1) * 64, "audit cursor");
        }
        report("audit-change-8192", cpu, wall, paths);
        if (enforce) {
            require(pathChecks.load() - paths <= 128 * 8, "audit physical checks scale per row");
            for (bool wal : {false, true}) {
                replacementDuringPage(false, false, wal);
                replacementDuringPage(true, false, wal);
                replacementDuringPage(true, true, wal);
            }
            std::cout << "PASS replacement during project/audit/missing-prefix\n";
        }
        std::cout << "PASS CPU fixture\n";
        return 0;
    } catch (const std::exception& ex) { std::cerr << "FAIL: " << ex.what() << '\n'; return 1; }
}
