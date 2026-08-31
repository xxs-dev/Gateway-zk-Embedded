#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

#include <dlfcn.h>
#include <unistd.h>

#include "edge_gateway/sqlite_alarm_writer.hpp"
#include "edge_gateway/sqlite_sample_writer.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class FixtureApi {
public:
    explicit FixtureApi(const char* libraryPath) {
        handle_ = dlopen(libraryPath, RTLD_NOW | RTLD_LOCAL);
        require(handle_ != nullptr, "failed to load sqlite failure fixture");
        reset = load<void (*)()>("fake_sqlite_reset");
        setCommitFailure = load<void (*)(int)>("fake_sqlite_set_commit_failure");
        finalizeCount = load<int (*)()>("fake_sqlite_finalize_count");
        doubleFinalizeCount = load<int (*)()>("fake_sqlite_double_finalize_count");
        rollbackCount = load<int (*)()>("fake_sqlite_rollback_count");
    }

    ~FixtureApi() {
        if (handle_ != nullptr) {
            dlclose(handle_);
        }
    }

    FixtureApi(const FixtureApi&) = delete;
    FixtureApi& operator=(const FixtureApi&) = delete;

    void (*reset)() = nullptr;
    void (*setCommitFailure)(int) = nullptr;
    int (*finalizeCount)() = nullptr;
    int (*doubleFinalizeCount)() = nullptr;
    int (*rollbackCount)() = nullptr;

private:
    template <typename Function>
    Function load(const char* name) {
        auto* symbol = dlsym(handle_, name);
        require(symbol != nullptr, std::string("failed to load fixture symbol: ") + name);
        return reinterpret_cast<Function>(symbol);
    }

    void* handle_ = nullptr;
};

template <typename WriteOperation>
void verifyCommitFailure(FixtureApi& fixture, WriteOperation write, const std::string& writerName) {
    fixture.reset();
    bool commitFailed = false;
    try {
        write();
    } catch (const std::exception& ex) {
        commitFailed = std::string(ex.what()) == "injected commit failure";
    }

    require(commitFailed, writerName + " should surface the injected COMMIT failure");
    require(fixture.finalizeCount() == 1, writerName + " should finalize its statement exactly once");
    require(fixture.doubleFinalizeCount() == 0, writerName + " double-finalized its statement");
    require(fixture.rollbackCount() == 1, writerName + " should roll back after COMMIT failure");
}

void verifySampleWriter(FixtureApi& fixture, const std::string& libraryPath) {
    edge_gateway::SqliteSampleWriter writer("fake-samples.db", libraryPath);
    verifyCommitFailure(
        fixture,
        [&writer]() {
            writer.writeSamples({edge_gateway::PersistentPointSample{1001, 1.0, 1000}});
        },
        "sample writer"
    );

    fixture.setCommitFailure(0);
    writer.writeSamples({edge_gateway::PersistentPointSample{1002, 2.0, 2000}});
    require(fixture.doubleFinalizeCount() == 0, "sample writer should remain usable after rollback");
}

void verifyAlarmWriter(FixtureApi& fixture, const std::string& libraryPath) {
    edge_gateway::SqliteAlarmWriter writer("fake-alarms.db", libraryPath);
    edge_gateway::AlarmEvent event;
    event.index = 2001;
    event.machineCode = "GW_TEST";
    event.meterCode = "METER_TEST";
    event.pointCode = "ALARM_TEST";
    event.alarmType = "high";
    event.active = true;
    event.ts = 1000;

    verifyCommitFailure(fixture, [&writer, &event]() { writer.writeEvents({event}); }, "alarm writer");

    fixture.setCommitFailure(0);
    writer.writeEvents({event});
    require(fixture.doubleFinalizeCount() == 0, "alarm writer should remain usable after rollback");
}

void removeDatabaseFiles(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

void verifyRealSqliteSmoke() {
    const auto prefix = std::string("/tmp/gateway_sqlite_writer_smoke_") + std::to_string(getpid());
    const auto samplePath = prefix + "_samples.db";
    const auto alarmPath = prefix + "_alarms.db";
    removeDatabaseFiles(samplePath);
    removeDatabaseFiles(alarmPath);

    {
        edge_gateway::SqliteSampleWriter writer(samplePath);
        writer.writeSamples({edge_gateway::PersistentPointSample{3001, 1.0, 1000}});
        writer.writeSamples({edge_gateway::PersistentPointSample{3001, 2.0, 1000}});
    }
    {
        edge_gateway::SqliteAlarmWriter writer(alarmPath);
        edge_gateway::AlarmEvent event;
        event.index = 4001;
        event.machineCode = "GW_TEST";
        event.meterCode = "METER_TEST";
        event.pointCode = "ALARM_TEST";
        event.alarmType = "high";
        event.ts = 1000;
        writer.writeEvents({event});
    }

    removeDatabaseFiles(samplePath);
    removeDatabaseFiles(alarmPath);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: sqlite_writer_failure_test <fixture-library>");
        const std::string libraryPath = argv[1];
        FixtureApi fixture(libraryPath.c_str());
        verifySampleWriter(fixture, libraryPath);
        verifyAlarmWriter(fixture, libraryPath);
        verifyRealSqliteSmoke();
        std::cout << "sqlite_writer_failure_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "sqlite_writer_failure_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
