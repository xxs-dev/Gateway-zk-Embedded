#include "edge_gateway/control_dedup_store.hpp"
#include "edge_gateway/writeback_outcome.hpp"
#include "edge_gateway/writeback_service.hpp"
#include "control_dedup_test_support.hpp"
#include "edge_gateway/filesystem_compat.hpp"
#include <dlfcn.h>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <sys/wait.h>
#include <thread>

using namespace edge_gateway;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

// Rewrites ledger results the way a release without the three-state classification stored them
// (no trailing format marker).
void stripLedgerFormatMarker(const std::string& path) {
    void* library = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!library) library = dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
    require(library != nullptr, "sqlite library");
    using Open = int (*)(const char*, void**);
    using Exec = int (*)(void*, const char*, void*, void*, char**);
    using Close = int (*)(void*);
    const auto open = reinterpret_cast<Open>(dlsym(library, "sqlite3_open"));
    const auto exec = reinterpret_cast<Exec>(dlsym(library, "sqlite3_exec"));
    const auto close = reinterpret_cast<Close>(dlsym(library, "sqlite3_close"));
    require(open && exec && close, "sqlite symbols");
    void* db = nullptr;
    require(open(path.c_str(), &db) == 0, "open ledger");
    char* error = nullptr;
    const int rc = exec(db, "UPDATE control_dedup_v1 SET result=substr(result,1,length(result)-2) "
                            "WHERE cmd LIKE 'legacy-%' AND result LIKE '% 3';", nullptr, nullptr, &error);
    if (rc != 0) std::cerr << "strip format marker: " << (error ? error : "?") << '\n';
    close(db);
    dlclose(library);
    require(rc == 0, "strip format marker");
}

// A writeback-failed recorded by an older release may have taken effect (its Modbus timeouts and
// read-back mismatches were stored that way): replayed as unknown. Never-dispatched stages stay failed.
void legacyLedgerResultsAreConservative(const std::string& path) {
    {
        ControlDedupStore store(path);
        const auto record = [&](const std::string& id, const std::string& stage) {
            PendingWriteCommand command{id, 2001, 5, "mqtt", 1, 1};
            require(!store.bind("machine", "meter", command), "legacy command queues");  // ingress records it
            (void)store.dispatch("machine", "meter", command, 2, [&] {
                auto receipt = beginWritebackResult(command, 2);
                completeWritebackResult(receipt, false, "old release result", stage, 3);
                return receipt;
            });
        };
        record("legacy-failed", "writeback-failed");
        record("legacy-expired", "expired");
        record("current-failed", "writeback-failed");
    }
    stripLedgerFormatMarker(path);
    ControlDedupStore reopened(path);
    const auto replay = [&](const std::string& id) {
        PendingWriteCommand command{id, 2001, 5, "mqtt", 1, 1};
        const auto result = reopened.bind("machine", "meter", command);
        require(static_cast<bool>(result), "legacy replay present");
        return *result;
    };
    const auto legacyFailed = replay("legacy-failed");
    require(std::string(writebackOutcome(legacyFailed)) == "unknown" && legacyFailed.stage == "writeback-failed" &&
            legacyFailed.message == "old release result",
            "a legacy writeback-failed may have taken effect: replay as unknown, stage and message kept");
    require(std::string(writebackOutcome(replay("legacy-expired"))) == "failed", "legacy expired stays failed");
    require(std::string(writebackOutcome(replay("current-failed"))) == "failed",
            "a classified writeback-failed stays a definite failure");
}
int main() {
    dedup_test::Directory directory;
    const auto path = directory.file("ledger.db");
    try {
        ControlDedupStore store(path);
        PendingWriteCommand command{"cmd-1", 1001, 1, "mqtt", 1, 1};
        require(!store.bind("machine", "meter", command), "new command must queue");
        auto claim = store.claim("machine", "meter", command, 2);
        require(claim.owner, "first claim must own dispatch");
        auto repeat = store.claim("machine", "meter", command, 3);
        require(!repeat.owner && !repeat.result.success && repeat.result.stage == "writeback-unconfirmed" &&
                repeat.result.outcomeUnknown,
                "pending repeat must be unknown");
        command.value = 2;
        bool conflict = false;
        try { store.bind("machine", "meter", command); } catch (...) { conflict = true; }
        require(conflict, "changed value must conflict");
        command.value = 1;
        for (int change = 0; change < 5; ++change) {
            auto altered = command;
            if (change == 0) altered.index++;
            if (change == 1) altered.highPriority = true;
            if (change == 2) altered.source += "-other";
            if (change == 3) altered.controlGeneration++;
            bool rejected = false;
            try { store.bind("machine", change == 4 ? "other-meter" : "meter", altered); }
            catch (...) { rejected = true; }
            require(rejected, "material changes must conflict, including another meter");
        }
        command.ts = 99; command.acceptedAt = 100;
        require(store.bind("machine", "meter", command)->stage == "writeback-unconfirmed", "timestamps must not conflict");
        ControlDedupStore reopened(path);
        require(!reopened.claim("machine", "meter", command, 4).owner, "reconstruction must retain pending");
        auto receipt = beginWritebackResult(command, 2);
        completeWritebackResult(receipt, true, "written", "writeback-succeeded", 3, true, true);
        store.finish("machine", "meter", receipt);
        const auto replay = reopened.bind("machine", "meter", command);
        require(replay && replay->success && replay->verifyPassed && replay->completedAt == 3, "replay saved result");

        command.cmdId = "long-source";
        command.source = "scada-windows:" + std::string(100, 's');
        store.bind("machine", "meter", command);
        auto wireCommand = command; wireCommand.source.resize(31);
        require(store.claim("machine", "meter", wireCommand, 5).owner, "wire source canonicalization");
        command.source.back() = 'x';
        bool fullSourceConflict = false;
        try { store.bind("machine", "meter", command); } catch (...) { fullSourceConflict = true; }
        require(fullSourceConflict, "full ingress source must remain immutable");

        command.cmdId = "busy"; command.source = "mqtt";
        store.bind("machine", "meter", command);
        int calls = 0;
        const auto execute = [&] {
            ++calls; auto r = beginWritebackResult(command, 5);
            completeWritebackResult(r, true, "ok", "writeback-succeeded", 6); return r;
        };
        {
            dedup_test::Database locked(path); locked.sql("BEGIN IMMEDIATE;");
            const auto busy = store.dispatch("machine", "meter", command, 5, execute);
            require(!busy.success && calls == 0, "busy before reserve cannot execute");
            require(store.result("machine", "meter", "cmd-1")->success, "existing receipt readable under write lock");
            locked.sql("ROLLBACK;");
        }
        {
            dedup_test::Database db(path); db.failFinalize();
            const auto failed = store.dispatch("machine", "meter", command, 5, execute);
            // The device result is not recorded, so the receipt cannot claim it (a replay could not
            // repeat it); the executor's own result stays in the message.
            require(!failed.success && failed.stage == "writeback-unconfirmed" && failed.outcomeUnknown && calls == 1 &&
                    failed.message.find("executor reported writeback-succeeded: ok") != std::string::npos,
                    "finalize failure must be unconfirmed and keep the executor result");
            db.sql("DROP TRIGGER fail_finalize;");
        }
        require(store.dispatch("machine", "meter", command, 8, execute).stage == "writeback-unconfirmed" && calls == 1,
                "finalize failure pending must never redispatch");

        // A feedback mismatch keeps its cause and message in the ledger (outcome unknown, not failed).
        {
            auto attempt = command;
            attempt.cmdId = "outcome-mismatch";
            store.bind("machine", "meter", attempt);
            const auto result = store.dispatch("machine", "meter", attempt, 9, [&] {
                auto r = beginWritebackResult(attempt, 9);
                completeWritebackResult(r, false, "verify failed: wrote 9, read back 7", "writeback-mismatch", 10, true, false);
                r.outcomeUnknown = true;
                r.feedbackMismatch = true;
                return r;
            });
            ControlDedupStore restarted(path);
            const auto replay = restarted.bind("machine", "meter", attempt);
            require(!result.success && result.feedbackMismatch && result.stage == "writeback-mismatch" && replay &&
                    replay->feedbackMismatch && replay->outcomeUnknown && replay->stage == "writeback-mismatch" &&
                    replay->message == "verify failed: wrote 9, read back 7" && replay->verifyAttempted &&
                    !replay->verifyPassed && std::string(writebackOutcome(*replay)) == "unknown",
                    "feedback mismatch must replay with its cause and message");
        }
        // CommandResult.outcomeUnknown stays unknown in the ledger; a conclusive failure is final.
        for (const bool outcomeUnknown : {true, false}) {
            auto attempt = command;
            attempt.cmdId = outcomeUnknown ? "outcome-unknown" : "outcome-failed";
            store.bind("machine", "meter", attempt);
            const auto result = store.dispatch("machine", "meter", attempt, 9, [&] {
                auto r = beginWritebackResult(attempt, 9);
                completeWritebackResult(r, false, outcomeUnknown ? "no reply" : "device exception 02",
                                        "writeback-failed", 10);
                r.outcomeUnknown = outcomeUnknown;
                return r;
            });
            const auto replay = store.bind("machine", "meter", attempt);
            require(!result.success && result.outcomeUnknown == outcomeUnknown &&
                    result.stage == (outcomeUnknown ? "writeback-timeout" : "writeback-failed") &&
                    replay && replay->outcomeUnknown == outcomeUnknown && replay->stage == result.stage,
                    "ledger must keep unknown and conclusive failures apart");
        }

        ControlDedupStore capped(path, 3);
        auto newCommand = command; newCommand.cmdId = "full";
        bool full = false;
        try { capped.bind("machine", "meter", newCommand); } catch (...) { full = true; }
        require(full && capped.result("machine", "meter", "cmd-1")->success, "capacity blocks only new IDs");

        for (const auto id : {std::string(), std::string(64, 'a'), std::string("a\0b", 3)}) {
            newCommand.cmdId = id; bool rejected = false;
            try { store.bind("machine", "meter", newCommand); } catch (...) { rejected = true; }
            require(rejected, "invalid ID must fail before storage/queue truncation");
        }
        newCommand.cmdId = std::string(63, 'a'); store.bind("machine", "meter", newCommand);

        command.cmdId = "race"; store.bind("machine", "meter", command);
        int markers[2]; require(pipe(markers) == 0, "pipe");
        pid_t children[2];
        for (int i = 0; i < 2; ++i) {
            children[i] = fork(); require(children[i] >= 0, "fork");
            if (!children[i]) {
                close(markers[0]);
                ControlDedupStore child(path);
                child.dispatch("machine", "meter", command, 9, [&] {
                    require(write(markers[1], "W", 1) == 1, "write marker");
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    return execute();
                });
                _exit(0);
            }
        }
        close(markers[1]);
        for (auto child : children) { int status; waitpid(child, &status, 0); require(status == 0, "child failed"); }
        char bytes[8]; require(read(markers[0], bytes, sizeof(bytes)) == 1, "two processes must have one dispatch owner");
        close(markers[0]);
        for (int afterWrite = 0; afterWrite < 2; ++afterWrite) {
            command.cmdId = afterWrite ? "crash-after-write" : "crash-before-write";
            store.bind("machine", "meter", command);
            const auto child = fork(); require(child >= 0, "crash fork");
            if (!child) {
                ControlDedupStore childStore(path);
                if (!childStore.claim("machine", "meter", command, 10).owner) _exit(3);
                if (afterWrite) execute();
                _exit(0); // No finalize, destructor or orderly shutdown.
            }
            int status; waitpid(child, &status, 0); require(status == 0, "crash setup failed");
            const auto before = calls;
            require(reopened.dispatch("machine", "meter", command, 11, execute).stage == "writeback-unconfirmed" && calls == before,
                    "both crash windows must retain unknown without redispatch");
        }
        {
            // Finalized receipts expire so a busy site cannot exhaust capacity forever.
            const auto retainedPath = directory.file("retention.db");
            ControlDedupStore shortLived(retainedPath, 2, 1, 1);
            PendingWriteCommand rotating{"rot-0", 1001, 1, "mqtt", 1, 1};
            for (int i = 0; i < 4; ++i) {
                rotating.cmdId = "rot-" + std::to_string(i);
                require(!shortLived.bind("machine", "meter", rotating), "expired receipts must free capacity");
                const auto done = shortLived.dispatch("machine", "meter", rotating, 20, [&] {
                    auto r = beginWritebackResult(rotating, 20);
                    completeWritebackResult(r, true, "ok", "writeback-succeeded", 21); return r;
                });
                require(done.success, "rotating dispatch must finalize");
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            rotating.cmdId = "pending-a"; shortLived.bind("machine", "meter", rotating);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            rotating.cmdId = "pending-b"; shortLived.bind("machine", "meter", rotating);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            rotating.cmdId = "pending-c"; bool full = false;
            try { shortLived.bind("machine", "meter", rotating); } catch (...) { full = true; }
            require(full, "unfinished reservations must never be pruned");
        }
        legacyLedgerResultsAreConservative(directory.file("legacy-ledger.db"));
        std::cout << "control_dedup_store_test passed\n";
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
