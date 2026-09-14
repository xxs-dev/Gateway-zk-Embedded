#include "edge_gateway/sqlite_sample_writer.hpp"

#include <algorithm>
#include <cstdio>
#include <dlfcn.h>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
using namespace edge_gateway;
using Clock = SqliteSampleWriter::MaintenanceClock;
using namespace std::chrono_literals;
constexpr std::int64_t kDay = 86400000;
constexpr std::int64_t kNow = 1800000000000;

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

class Database {
public:
    Database() {
        static unsigned sequence = 0;
        path = "retention_" + std::to_string(getpid()) + "_" + std::to_string(++sequence) + ".db";
        library_ = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
        require(library_ != nullptr, "SQLite unavailable");
        const auto open = reinterpret_cast<int (*)(const char*, void**)>(dlsym(library_, "sqlite3_open"));
        exec_ = reinterpret_cast<Exec>(dlsym(library_, "sqlite3_exec"));
        close_ = reinterpret_cast<int (*)(void*)>(dlsym(library_, "sqlite3_close"));
        require(open && exec_ && close_ && open(path.c_str(), &db_) == 0, "SQLite test open failed");
    }
    ~Database() {
        close_(db_);
        dlclose(library_);
        for (const auto* suffix : {"", "-wal", "-shm"}) std::remove((path + suffix).c_str());
    }
    std::string query(const std::string& sql) const {
        std::string result;
        require(exec_(db_, sql.c_str(), [](void* out, int count, char** values, char**) {
            for (int i = 0; i < count; ++i) {
                *static_cast<std::string*>(out) += (values[i] ? values[i] : "NULL");
                *static_cast<std::string*>(out) += '|';
            }
            return 0;
        }, &result, nullptr) == 0, "test SQL failed: " + sql);
        return result;
    }
    int rows() const { return std::stoi(query("SELECT count(*) FROM point_samples;")); }
    std::string path;
private:
    using Exec = int (*)(void*, const char*, int (*)(void*, int, char**, char**), void*, char**);
    void* library_ = nullptr;
    void* db_ = nullptr;
    Exec exec_ = nullptr;
    int (*close_)(void*) = nullptr;
};

PersistentPointSample sample(std::uint32_t index, std::int64_t ts, double value = 1) {
    PersistentPointSample result{};
    result.index = index;
    result.ts = ts;
    result.value = value;
    return result;
}

void expectFailure(const std::function<void()>& operation) {
    bool failed = false;
    try { operation(); } catch (const std::exception&) { failed = true; }
    require(failed, "expected SQLite failure was not exercised");
}

bool cleanupUsesTimestampSeek(const std::string& plan) {
    const auto modernSeek = "SEARCH point_samples USING COVERING INDEX idx_point_samples_ts (ts<?)";
    const auto legacySeek = "SEARCH TABLE point_samples USING COVERING INDEX idx_point_samples_ts (ts<?)";
    return (plan.find(modernSeek) != std::string::npos || plan.find(legacySeek) != std::string::npos) &&
        plan.find("SCAN point_samples") == std::string::npos &&
        plan.find("SCAN TABLE point_samples") == std::string::npos &&
        plan.find("TEMP B-TREE") == std::string::npos;
}

void verifyQueryPlanAssertions() {
    const std::string modernSeek = "SEARCH point_samples USING COVERING INDEX idx_point_samples_ts (ts<?)";
    const std::string legacySeek = "SEARCH TABLE point_samples USING COVERING INDEX idx_point_samples_ts (ts<?)";
    require(cleanupUsesTimestampSeek("2|0|0|SEARCH point_samples USING INTEGER PRIMARY KEY (rowid=?)|"
        "7|0|0|LIST SUBQUERY 1|11|7|0|" + modernSeek + "|"), "modern query-plan fixture rejected");
    require(cleanupUsesTimestampSeek("2|0|0|SEARCH TABLE point_samples USING INTEGER PRIMARY KEY (rowid=?)|"
        "7|0|0|LIST SUBQUERY 1|11|7|0|" + legacySeek + "|"), "SQLite 3.31 query-plan fixture rejected");
    for (const auto* plan : {
        "SCAN point_samples USING COVERING INDEX idx_point_samples_ts",
        "SCAN TABLE point_samples USING COVERING INDEX idx_point_samples_ts",
        "SEARCH point_samples USING COVERING INDEX wrong_index (ts<?)",
        "SEARCH TABLE point_samples USING COVERING INDEX wrong_index (ts<?)",
        "SEARCH point_samples USING COVERING INDEX idx_point_samples_ts",
        "SEARCH TABLE point_samples USING COVERING INDEX idx_point_samples_ts (ts<=?)",
        "SEARCH point_samples USING COVERING INDEX idx_point_samples_ts (ts>?)",
        "SEARCH point_samples USING INDEX idx_point_samples_ts (ts<?)"}) {
        require(!cleanupUsesTimestampSeek(plan), "unsafe query-plan fixture accepted: " + std::string(plan));
    }
    for (const auto& seek : {modernSeek, legacySeek}) {
        for (const auto* extra : {"|SCAN point_samples|", "|SCAN TABLE point_samples|", "|USE TEMP B-TREE FOR ORDER BY|"}) {
            require(!cleanupUsesTimestampSeek(seek + extra), "seek hid a scan/sort query-plan fixture");
        }
    }
}

void verifyOldSchemaAndQueryPlan() {
    Database db;
    db.query("CREATE TABLE point_samples(point_index INTEGER NOT NULL, ts INTEGER NOT NULL, "
             "value REAL NOT NULL, PRIMARY KEY(point_index, ts)); INSERT INTO point_samples VALUES(1,1,1);");
    {
        SqliteSampleWriter writer(db.path);
        require(db.rows() == 1, "constructor migration must not delete history");
        require(db.query("SELECT count(*) FROM pragma_index_info('idx_point_samples_ts') WHERE name='ts';") == "1|",
                "old schema did not gain ts index");
        const auto plan = db.query("EXPLAIN QUERY PLAN DELETE FROM point_samples WHERE rowid IN ("
            "SELECT rowid FROM point_samples INDEXED BY idx_point_samples_ts WHERE ts < 2 ORDER BY ts LIMIT 512);");
        std::cout << "cleanup query plan: " << plan << std::endl;
        require(cleanupUsesTimestampSeek(plan), "cleanup must seek the timestamp index without a full scan or sorting pass");
    }
    SqliteSampleWriter reopened(db.path);
    require(db.rows() == 1, "reopening old/new schema changed data");
}

void verifyBoundaryAndIdempotence() {
    Database db;
    SqliteSampleWriter writer(db.path);
    const auto cutoff = kNow - 30 * kDay;
    const auto base = Clock::now() + 1h;
    std::vector<PersistentPointSample> values{
        sample(1, cutoff - 1), sample(2, cutoff), sample(3, cutoff + 1), sample(4, kNow + kDay),
        sample(5, 0), sample(6, -1)};
    writer.writeSamples(values);
    writer.writeSamples(values);
    require(db.rows() == 6, "write must remain idempotent and must not perform retention inline");
    writer.cleanupExpiredSamples(kNow, base);
    require(db.rows() == 3, "strict cutoff must retain boundary, recent and future samples");
    require(db.query("SELECT point_index FROM point_samples ORDER BY point_index;") == "2|3|4|",
            "retention selected the wrong timestamps");
    writer.writeSamples({sample(2, cutoff, 9)});
    require(db.rows() == 3 && db.query("SELECT value FROM point_samples WHERE point_index=2;") == "9.0|",
            "duplicate write lost replace semantics");
    writer.writeSamples(values);
    writer.cleanupExpiredSamples(kNow, base + 60s);
    require(db.rows() == 3, "replayed expired history must be reclaimed again");
}

void verifyCustomAndClampedRetention() {
    for (const auto days : {45, 0, -1, 3650, 99999}) {
        const auto effective = std::max(1, std::min(days, 3650));
        Database db;
        SqliteSampleWriter writer(db.path, "", days);
        const auto cutoff = kNow - effective * kDay;
        writer.writeSamples({sample(1, cutoff - 1), sample(2, cutoff)});
        writer.cleanupExpiredSamples(kNow);
        require(db.rows() == 1 && db.query("SELECT point_index FROM point_samples;") == "2|",
                "writer retention days not clamped/applied");
    }
    Database db;
    SqliteSampleWriter writer(db.path);
    writer.writeSamples({sample(1, std::numeric_limits<std::int64_t>::min())});
    writer.cleanupExpiredSamples(std::numeric_limits<std::int64_t>::min());
    require(db.rows() == 1, "cutoff underflow deleted boundary data");
    SqliteSampleWriter disabled("");
    disabled.cleanupExpiredSamples(kNow);
}

void verifyBatchesAndIntervals() {
    Database db;
    SqliteSampleWriter writer(db.path);
    std::vector<PersistentPointSample> values;
    for (int i = 0; i < 1200; ++i) values.push_back(sample(i + 1, 1));
    writer.writeSamples(values);
    const auto base = Clock::now() + 1h;
    writer.cleanupExpiredSamples(kNow, base);
    require(db.rows() == 688, "first cleanup exceeded or missed 512-row budget");
    writer.cleanupExpiredSamples(kNow, base + 999ms);
    require(db.rows() == 688, "full batch repeated before 1 second");
    writer.cleanupExpiredSamples(kNow, base + 1s);
    require(db.rows() == 176, "full batch did not continue after 1 second");
    writer.cleanupExpiredSamples(kNow, base + 2s);
    require(db.rows() == 0, "third batch did not reclaim remainder");
    writer.writeSamples({sample(1, 1)});
    writer.cleanupExpiredSamples(kNow, base + 61999ms);
    require(db.rows() == 1, "idle interval shorter than 60 seconds");
    writer.cleanupExpiredSamples(kNow, base + 62s);
    require(db.rows() == 0, "idle cleanup did not resume after 60 seconds");
}

void verifyCleanupFailureAndWriteRecovery() {
    Database db;
    SqliteSampleWriter writer(db.path);
    writer.writeSamples({sample(1, 1), sample(2, 2)});
    db.query("CREATE TRIGGER fail_cleanup BEFORE DELETE ON point_samples WHEN OLD.point_index=2 "
             "BEGIN SELECT RAISE(ABORT,'injected cleanup failure'); END;");
    const auto base = Clock::now() + 1h;
    expectFailure([&] { writer.cleanupExpiredSamples(kNow, base); });
    require(db.rows() == 2, "failed DELETE must atomically retain the entire batch");
    writer.writeSamples({sample(3, kNow)});
    require(db.rows() == 3, "cleanup failure poisoned subsequent write");
    db.query("DROP TRIGGER fail_cleanup;");
    writer.cleanupExpiredSamples(kNow, base + 4999ms);
    require(db.rows() == 3, "cleanup failure bypassed 5-second backoff");
    writer.cleanupExpiredSamples(kNow, base + 5s);
    require(db.rows() == 1, "cleanup failed to recover after backoff");
    db.query("CREATE TRIGGER fail_write BEFORE INSERT ON point_samples WHEN NEW.point_index=5 "
             "BEGIN SELECT RAISE(ABORT,'injected write failure'); END;");
    expectFailure([&] { writer.writeSamples({sample(4, kNow), sample(5, kNow)}); });
    require(db.rows() == 1, "failed write must roll back partial batch");
    db.query("DROP TRIGGER fail_write;");
    writer.writeSamples({sample(4, kNow), sample(5, kNow)});
    require(db.rows() == 3, "writer unusable after rollback");
}

void verifyBusyFailureBackoffFromCompletion() {
    Database db;
    SqliteSampleWriter writer(db.path);
    writer.writeSamples({sample(1, 1)});
    db.query("BEGIN IMMEDIATE;");
    const auto started = Clock::now();
    expectFailure([&] { writer.cleanupExpiredSamples(kNow); });
    const auto elapsed = Clock::now() - started;
    db.query("ROLLBACK;");
    std::cout << "real SQLite cleanup busy failure elapsed_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << std::endl;
    require(elapsed >= 4500ms, "original SQLite busy timeout was not retained");
    writer.cleanupExpiredSamples(kNow);
    require(db.rows() == 1, "slow failure must back off from completion, not attempt start");
    writer.cleanupExpiredSamples(kNow, Clock::now() + 5s);
    require(db.rows() == 0, "busy failure did not recover");
}
}  // namespace

int main() {
    try {
        const auto run = [](const char* name, const std::function<void()>& test) {
            test();
            std::cout << name << " passed" << std::endl;
        };
        run("query-plan assertion fixtures", verifyQueryPlanAssertions);
        run("old schema and indexed query", verifyOldSchemaAndQueryPlan);
        run("boundary future and idempotence", verifyBoundaryAndIdempotence);
        run("custom clamp and overflow", verifyCustomAndClampedRetention);
        run("batches and intervals", verifyBatchesAndIntervals);
        run("cleanup failure atomicity and write rollback/reuse", verifyCleanupFailureAndWriteRecovery);
        run("busy failure completion backoff", verifyBusyFailureBackoffFromCompletion);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "sqlite_sample_writer_retention_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
