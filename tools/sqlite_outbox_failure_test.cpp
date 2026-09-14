#include "edge_gateway/mqtt_event_outbox.hpp"
#include "edge_gateway/sqlite_error.hpp"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>

#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using Outbox = edge_gateway::MqttEventOutbox;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Database {
    std::string path;
    Database() {
        char name[] = "/tmp/outbox_failure_XXXXXX";
        const int fd = mkstemp(name);
        require(fd >= 0, "mkstemp failed");
        close(fd);
        path = name;
    }
    ~Database() {
        for (const char* suffix : {"", "-wal", "-shm", "-journal"}) {
            std::remove((path + suffix).c_str());
        }
    }
};

struct Faults {
    void* handle;
    void (*reset)();
    void (*event)(int, int, int);
    int (*eventAttempts)();
    int (*rollbackToAttempts)();
    int (*beginAttempts)();
    void (*restoreFailure)();
    void (*rollbackFailure)();
    void (*commitFailure)(int, int);
    int (*openHandles)();
    explicit Faults(const std::string& path) {
        handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        require(handle != nullptr, "load failure fixture failed");
        reset = load<void (*)()>("outbox_fault_reset");
        event = load<void (*)(int, int, int)>("outbox_fault_event");
        eventAttempts = load<int (*)()>("outbox_fault_event_attempts");
        rollbackToAttempts = load<int (*)()>("outbox_fault_rollback_to_attempts");
        beginAttempts = load<int (*)()>("outbox_fault_begin_attempts");
        restoreFailure = load<void (*)()>("outbox_fault_restore_failure");
        rollbackFailure = load<void (*)()>("outbox_fault_rollback_failure");
        commitFailure = load<void (*)(int, int)>("outbox_fault_commit");
        openHandles = load<int (*)()>("outbox_fault_open_handles");
        reset();
    }
    ~Faults() { dlclose(handle); }
    template <typename Function> Function load(const char* name) {
        void* symbol = dlsym(handle, name);
        require(symbol != nullptr, std::string("missing fixture symbol ") + name);
        return reinterpret_cast<Function>(symbol);
    }
};

Outbox::EventMessage event(const std::string& target) {
    Outbox::EventMessage result;
    result.eventId = "stable-event-1";
    result.eventType = "alarm";
    result.targetId = target;
    result.topic = "test/alarm";
    result.payload = "{\"active\":true}";
    result.eventTs = 1720000000000LL;
    return result;
}

Outbox::EventState state() {
    Outbox::EventState result;
    result.stateKey = "alarm-state";
    result.eventType = "alarm";
    result.index = 1;
    result.active = true;
    result.lifecycle = "raised:1";
    return result;
}

void missingLibraryMustFail() {
    Database db;
    bool failed = false;
    try { Outbox box(db.path, db.path + ".missing.so", 12, 24, 8); }
    catch (const std::exception&) { failed = true; }
    require(failed, "explicit missing SQLite library silently fell back to system library");
    Outbox valid(db.path, "", 12, 24, 8);
    require(valid.enqueueBatch({event("main")}).size() == 1, "failed load poisoned later initialization");
}

void injectedFailure(const std::string& library, int code, bool rollback, const char* text) {
    Database db;
    Faults faults(library);
    Outbox box(db.path, library, 12, 24, 8);
    faults.event(code, rollback ? 1 : 0, 1);
    std::string error;
    try { box.enqueueFanoutWithStates({event("optional"), event("main")}, {state()}); }
    catch (const edge_gateway::SqliteError& ex) {
        require(ex.primaryCode() == (code & 255) && ex.extendedCode() == code, "SQLite error codes changed");
        require(ex.transactionActive() == !rollback, "transaction state was not captured at failure");
        require(ex.operation() == "enqueue.event.insert", "error operation is missing");
        error = ex.what();
    }
    require(error.find(text) != std::string::npos, "original storage failure was lost: " + error);
    require(box.pendingCount() == 0 && box.loadStates().empty(), "global failure committed partial data");
    if (rollback) require(faults.rollbackToAttempts() == 0, "savepoint cleanup attempted after automatic rollback");
    faults.reset();
    const auto retry = box.enqueueFanoutWithStates({event("main")}, {state()});
    require(retry.ids.size() == 1 && box.pendingCount() == 1 && box.loadStates().size() == 1,
        "same connection did not recover after storage failure");
}

void lockedIsNotBusy(const std::string& library) {
    Database db;
    Faults faults(library);
    Outbox box(db.path, library, 12, 24, 8);
    faults.event(6, 0, -1);
    bool failed = false;
    try { box.enqueueBatch({event("main")}); }
    catch (const std::exception&) { failed = true; }
    require(failed && faults.eventAttempts() == 1, "SQLITE_LOCKED retried as ordinary writer contention");
    require(box.pendingCount() == 0, "LOCKED committed an event");
}

void optionalConstraintRemainsIsolated(const std::string& library) {
    Database db;
    Faults faults(library);
    Outbox box(db.path, library, 12, 24, 8);
    faults.event(1811, 0, 1);
    const auto result = box.enqueueFanoutWithStates({event("optional"), event("main")}, {state()});
    require(result.failedTargetIds == std::vector<std::string>{"optional"}, "optional target isolation changed");
    require(box.pendingCount() == 1 && box.loadStates().size() == 1, "primary/state did not commit");
}

void snapshotRestartsTransaction(const std::string& library) {
    Database db;
    Faults faults(library);
    Outbox box(db.path, library, 12, 24, 8);
    faults.reset();
    faults.event(517, 0, 1);
    require(box.enqueueBatch({event("main")}).size() == 1, "snapshot retry failed");
    require(faults.eventAttempts() == 2 && faults.beginAttempts() == 2,
        "BUSY_SNAPSHOT did not restart whole transaction");
}

void libraryPinning(const std::string& library) {
    Database db;
    Database other;
    Outbox box(db.path, library, 12, 24, 8);
    require(!box.storageSettings().sqliteLibraryPath.empty(), "actual SQLite library is not reported");
    bool rejected = false;
    try { Outbox mixed(other.path, "libsqlite3.so.0", 12, 24, 8); }
    catch (const std::exception& ex) { rejected = std::string(ex.what()).find("mix SQLite") != std::string::npos; }
    require(rejected, "different library replaced global API table");
    {
        Outbox same(other.path, library, 12, 24, 8);
        require(same.pendingCount() == 0, "same library could not be shared");
    }
    require(box.enqueueBatch({event("main")}).size() == 1, "failed load/destruction corrupted live instance");
    Outbox inherited(other.path, "", 12, 24, 8);
    require(inherited.storageSettings().sqliteLibraryPath == box.storageSettings().sqliteLibraryPath,
        "default library did not reuse pinned API");
}

void failedSymbolsDoNotPoisonApi(const std::string& incompleteLibrary) {
    Database db;
    bool rejected = false;
    try { Outbox bad(db.path, incompleteLibrary, 12, 24, 8); }
    catch (const std::exception& ex) { rejected = std::string(ex.what()).find("symbol") != std::string::npos; }
    require(rejected, "incomplete SQLite library was accepted");
    Outbox valid(db.path, "", 12, 24, 8);
    require(valid.enqueueBatch({event("main")}).size() == 1, "partial API resolution poisoned global functions");
}

void postCommitRestoreDoesNotUndoSuccess(const std::string& library) {
    Database db;
    Faults faults(library);
    {
        Outbox box(db.path, library, 12, 24, 8);
        faults.restoreFailure();
        require(box.enqueueBatch({event("main")}).size() == 1,
            "post-COMMIT timeout restoration reported a committed event as failed");
        bool quarantined = false;
        try { box.pendingCount(); }
        catch (const std::exception& ex) { quarantined = std::string(ex.what()).find("quarantined") != std::string::npos; }
        require(quarantined, "failed connection restoration allowed more SQL");
    }
    Outbox reopened(db.path, library, 12, 24, 8);
    require(reopened.pendingCount() == 1, "committed event vanished on reopen");
}

void failedRollbackQuarantines(const std::string& library) {
    Database db;
    Faults faults(library);
    {
        Outbox box(db.path, library, 12, 24, 8);
        faults.event(266, 0, 1);
        faults.rollbackFailure();
        bool original = false;
        try { box.enqueueBatch({event("main")}); }
        catch (const edge_gateway::SqliteError& ex) { original = ex.extendedCode() == 266; }
        require(original, "rollback failure hid original error");
        bool quarantined = false;
        try { box.enqueueBatch({event("main")}); }
        catch (const std::exception& ex) { quarantined = std::string(ex.what()).find("quarantined") != std::string::npos; }
        require(quarantined, "unresolved transaction allowed another write");
    }
    Outbox reopened(db.path, library, 12, 24, 8);
    require(reopened.pendingCount() == 0, "failed transaction leaked on reopen");
}

void failedCommitIsAtomic(const std::string& library, int code, bool automaticRollback) {
    Database db;
    Faults faults(library);
    Outbox box(db.path, library, 12, 24, 8);
    faults.commitFailure(code, automaticRollback ? 1 : 0);
    bool original = false;
    try { box.enqueueFanoutWithStates({event("main"), event("optional")}, {state()}); }
    catch (const edge_gateway::SqliteError& ex) {
        original = ex.extendedCode() == code && ex.operation() == "commit" &&
            ex.transactionActive() == !automaticRollback;
    }
    require(original, "COMMIT failure lost code/operation/transaction state");
    require(box.pendingCount() == 0 && box.pendingCount("optional") == 0 && box.loadStates().empty(),
        "COMMIT failure left partial target/state data");
    require(box.enqueueBatchWithStates({event("main")}, {state()}).size() == 1,
        "connection did not recover after failed COMMIT");
}

void concurrentLibraryInitialization(const std::string& library) {
    std::promise<void> start;
    auto ready = start.get_future().share();
    std::vector<std::future<std::string>> workers;
    for (int i = 0; i < 8; ++i) {
        workers.push_back(std::async(std::launch::async, [&, ready] {
            Database db;
            ready.wait();
            Outbox box(db.path, library, 12, 24, 8);
            require(box.enqueueBatch({event("main")}).size() == 1, "concurrent initialization failed");
            return box.storageSettings().sqliteLibraryPath;
        }));
    }
    start.set_value();
    const auto path = workers.front().get();
    require(!path.empty(), "missing resolved library path");
    for (std::size_t i = 1; i < workers.size(); ++i) {
        require(workers[i].get() == path, "concurrent initialization mixed libraries");
    }
}

void failedOpenClosesHandle(const std::string& library) {
    Database db;
    Faults faults(library);
    require(faults.openHandles() == 0, "unexpected initial handles");
    bool original = false;
    try { Outbox box(db.path + "/not-a-directory.db", library, 12, 24, 8); }
    catch (const edge_gateway::SqliteError& ex) {
        original = ex.primaryCode() == 14 && ex.operation() == "open" && !ex.transactionActive();
    }
    require(original, "open failure lost its diagnostic");
    require(faults.openHandles() == 0, "failed open leaked a SQLite handle");
    {
        Outbox valid(db.path, library, 12, 24, 8);
        require(faults.openHandles() == 1 && valid.pendingCount() == 0, "recovery after failed open failed");
    }
    require(faults.openHandles() == 0, "destruction leaked a SQLite handle");
}

bool run(const char* name, const std::function<void()>& action) {
    const pid_t pid = fork();
    require(pid >= 0, "fork failed");
    if (pid == 0) {
        try { action(); std::cout << "PASS " << name << std::endl; _exit(0); }
        catch (const std::exception& ex) {
            std::cerr << "FAIL " << name << ": " << ex.what() << std::endl;
            _exit(1);
        }
    }
    int status = 0;
    require(waitpid(pid, &status, 0) == pid, "waitpid failed");
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
}

int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "usage: sqlite_outbox_failure_test FIXTURE INCOMPLETE_LIBRARY" << std::endl; return 2; }
    const std::string library = argv[1];
    int failed = 0;
    failed += !run("missing-private-library", missingLibraryMustFail);
    failed += !run("full-automatic-rollback", [&] { injectedFailure(library, 13, true, "injected full"); });
    failed += !run("optional-ioerr-is-global", [&] { injectedFailure(library, 266, false, "injected ioerr"); });
    failed += !run("locked-is-not-busy", [&] { lockedIsNotBusy(library); });
    failed += !run("optional-constraint", [&] { optionalConstraintRemainsIsolated(library); });
    failed += !run("snapshot-whole-transaction", [&] { snapshotRestartsTransaction(library); });
    failed += !run("library-pinning", [&] { libraryPinning(library); });
    failed += !run("incomplete-library", [&] { failedSymbolsDoNotPoisonApi(argv[2]); });
    failed += !run("post-commit-restore", [&] { postCommitRestoreDoesNotUndoSuccess(library); });
    failed += !run("failed-rollback-quarantine", [&] { failedRollbackQuarantines(library); });
    failed += !run("commit-full-automatic-rollback", [&] { failedCommitIsAtomic(library, 13, true); });
    failed += !run("commit-ioerr-active-transaction", [&] { failedCommitIsAtomic(library, 266, false); });
    failed += !run("concurrent-library-initialization", [&] { concurrentLibraryInitialization(library); });
    failed += !run("failed-open-handle-release", [&] { failedOpenClosesHandle(library); });
    return failed == 0 ? 0 : 1;
}
