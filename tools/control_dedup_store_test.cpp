#include "edge_gateway/control_dedup_store.hpp"
#include "edge_gateway/writeback_service.hpp"
#include "control_dedup_test_support.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <sys/wait.h>
#include <thread>

using namespace edge_gateway;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
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
        require(!repeat.owner && !repeat.result.success && repeat.result.stage == "writeback-timeout",
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
        require(store.bind("machine", "meter", command)->stage == "writeback-timeout", "timestamps must not conflict");
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
            require(!failed.success && failed.stage == "writeback-timeout" && calls == 1, "finalize failure must be unknown");
            db.sql("DROP TRIGGER fail_finalize;");
        }
        require(store.dispatch("machine", "meter", command, 8, execute).stage == "writeback-timeout" && calls == 1,
                "finalize failure pending must never redispatch");

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
            require(reopened.dispatch("machine", "meter", command, 11, execute).stage == "writeback-timeout" && calls == before,
                    "both crash windows must retain unknown without redispatch");
        }
        std::cout << "control_dedup_store_test passed\n";
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
