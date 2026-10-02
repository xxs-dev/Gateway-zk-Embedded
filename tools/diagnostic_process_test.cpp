#include "edge_gateway/diagnostic_process.hpp"
#include <chrono>
#include <cerrno>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#ifndef _WIN32
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

using edge_gateway::DiagnosticProcess;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
#ifndef _WIN32
int ticks = 0;
void finish(DiagnosticProcess& job, int timeoutMs = 3500) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < until) {
        const auto before = std::chrono::steady_clock::now();
        const bool ready = job.poll();
        require(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(100),
                "poll blocked the independent tick");
        ++ticks;
        if (ready) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("bounded test wait expired");
}
std::string quoted(const std::string& path) {
    require(path.find('\'') == std::string::npos, "unsafe test executable path");
    return "'" + path + "'";
}
pid_t descendant(const DiagnosticProcess& job) {
    const auto at = job.result().output.find("CHILD=");
    require(at != std::string::npos, "fixture descendant was not reported");
    const auto pid = std::stoi(job.result().output.substr(at + 6));
    require(pid > 1, "invalid private descendant");
    return pid;
}
void requireGone(pid_t pid) {
    require(kill(pid, 0) < 0 && errno == ESRCH, "owned descendant was not reaped");
}
void waitGone(pid_t pid) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < until) {
        if (kill(pid, 0) < 0 && errno == ESRCH) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    requireGone(pid);
}
void runCases(const std::string& self) {
    {
        DiagnosticProcess job;
        require(job.start("sleep 30", 256, 120), "start silent job");
        require(!job.start("exit 0", 256), "one owned job only");
        finish(job);
        require(job.result().timedOut && job.result().exitCode == 124 &&
                !job.result().cleanupFailed && !job.active(), "silent timeout and cleanup");
        require(ticks >= 3, "independent ticks did not advance");
        std::cout << "PASS silent-deadline/nonblocking-ticks\n";
    }
    {
        DiagnosticProcess job;
        require(job.start("while :; do printf '01234567890123456789\\n'; done", 256, 120), "start output job");
        finish(job);
        require(job.result().timedOut && job.result().truncated && job.result().output.size() <= 256 &&
                job.result().output.find("[truncated]") != std::string::npos, "bounded continuous output");
        std::cout << "PASS continuous-output-cap\n";
    }
    for (auto command : {"exit 1", "kill -TERM $$"}) {
        DiagnosticProcess job; require(job.start(command, 256), "start exit job"); finish(job);
        require(job.result().exitCode == (command == std::string("exit 1") ? 1 : 143), "normalized status");
        require(job.result().signal == (command == std::string("exit 1") ? 0 : SIGTERM), "signal category");
        std::cout << "PASS " << command << "\n";
    }
    {
        DiagnosticProcess job; job.readsPerPoll = 1;
        require(job.start(quoted(self) + " --burst", 256 * 1024), "start final-drain job");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        finish(job);
        require(job.result().exitCode == 0 && !job.result().cleanupFailed &&
                job.result().output == std::string(128 * 1024, 'x') + "END",
                "bounded final drain dropped output");
        std::cout << "PASS bounded-final-drain\n";
    }
    {
        DiagnosticProcess job; require(job.start("printf done", 256), "start status race");
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        siginfo_t info{};
        while (std::chrono::steady_clock::now() < until) {
            require(waitid(P_PID, job.supervisorForTest(), &info, WEXITED | WNOHANG | WNOWAIT) == 0, "observe private owner");
            if (info.si_pid) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(info.si_pid != 0, "owner did not exit before forced EAGAIN");
        job.skipFirstStatusRead = true;
        finish(job);
        require(job.result().exitCode == 0 && !job.result().cleanupFailed && job.result().output == "done",
                "EAGAIN -> exited owner must drain status again");
        std::cout << "PASS deterministic-status-read-exit-race\n";
    }
    {
        DiagnosticProcess job; job.delayCleanupFailure = true;
        require(job.start("exit 0", 256), "start modeled cleanup uncertainty"); finish(job);
        require(job.active() && job.result().cleanupFailed && job.unavailable(), "live owner unknown not notified");
        require(!job.start("exit 0", 256), "uncertain owner reopened admission");
        clockid_t ownerClock;
        timespec cpuBefore{}, cpuAfter{};
        require(clock_getcpuclockid(job.supervisorForTest(), &ownerClock) == 0 &&
                clock_gettime(ownerClock, &cpuBefore) == 0, "private owner CPU clock");
        const auto waitedFrom = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        require(clock_gettime(ownerClock, &cpuAfter) == 0 && job.active(), "modeled owner wait missing");
        const auto cpuNs = (cpuAfter.tv_sec - cpuBefore.tv_sec) * 1000000000LL +
            cpuAfter.tv_nsec - cpuBefore.tv_nsec;
        require(std::chrono::steady_clock::now() - waitedFrom >= std::chrono::milliseconds(100) &&
                cpuNs < 50000000LL, "retained owner spun on readable cancellation/HUP");
        require(!job.cancelAndWait() && !job.active() && job.unavailable(), "owner close hid prior uncertainty");
        require(!job.start("exit 0", 256), "poison latch lost after owner exit");
        std::cout << "PASS modeled-early-unknown/live-owner/backoff/closed-admission\n";
    }
    for (int fault = 0; fault < 3; ++fault) {
        DiagnosticProcess job;
        job.shortStatus = fault == 0; job.dieBeforeCommand = fault == 1; job.holdOutputWriter = fault == 2;
        require(job.start("printf done", 256), "start private channel fault"); finish(job);
        if (fault == 2) require(job.heldWriterForTest >= 0, "private EOF holder was not created");
        require(job.result().cleanupFailed && job.result().exitCode == -1 && job.unavailable(),
                "channel/owner fault was accepted or reopened admission");
        std::cout << "PASS private-channel-owner-fault-" << fault << "\n";
    }
    for (bool orphan : {false, true}) {
        DiagnosticProcess job;
        job.forceProcFallback = true;
        require(job.start(quoted(self) + (orphan ? " --orphan" : " --descendant"), 256, 200), "start descendant");
        finish(job);
        requireGone(descendant(job));
        require(!job.result().cleanupFailed && job.result().exitCode == (orphan ? 0 : 124), "descendant lifecycle");
        std::cout << "PASS " << (orphan ? "normal-exit-escaped-descendant" : "deadline-escaped-descendant") << "\n";
    }
    {
        DiagnosticProcess job; job.forceProcFallback = true;
        require(job.start("exit 0", 256), "start proc/pidfd fallback"); finish(job);
        require(job.result().exitCode == 0 && !job.result().cleanupFailed && !job.active(),
                "proc/pidfd fallback normal cleanup");
        std::cout << "PASS proc-pidfd-fallback-normal\n";
    }
    {
        DiagnosticProcess job; job.forceProcFallback = true;
        require(job.start(quoted(self) + " --descendant", 256), "start supervisor-death descendant");
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (job.result().output.find("CHILD=") == std::string::npos &&
                std::chrono::steady_clock::now() < until) {
            job.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const auto child = descendant(job);
        require(kill(job.supervisorForTest(), SIGKILL) == 0, "kill actual outer supervisor");
        finish(job); waitGone(child);
        require(job.result().cleanupFailed && job.unavailable() && !job.start("exit 0", 256),
                "outer-supervisor death reopened admission");
        std::cout << "PASS outer-supervisor-death-escaped-descendant\n";
    }
    {
        DiagnosticProcess job; job.forceProcFallback = true; job.pauseGuardianBeforeCommand = true;
        require(job.start(quoted(self) + " --descendant", 256), "start guardian-death descendant");
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (job.result().output.find("CHILD=") == std::string::npos &&
                std::chrono::steady_clock::now() < until) {
            job.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const auto child = descendant(job);
        const auto at = job.result().output.find("PRESTART_READY=");
        require(at != std::string::npos, "guardian identity missing");
        const auto guardian = std::stoi(job.result().output.substr(at + 15));
        require(guardian > 1 && kill(guardian, SIGKILL) == 0, "kill exact inner guardian");
        finish(job); waitGone(child);
        require(job.result().cleanupFailed && job.unavailable() && !job.start("exit 0", 256),
                "inner-guardian death reopened admission");
        std::cout << "PASS inner-guardian-death-escaped-descendant\n";
    }
    {
        DiagnosticProcess job; job.forceProcFallback = true; job.pauseGuardianBeforeCommand = true;
        require(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0, "private test subreaper");
        char marker[] = "/tmp/gw-diag-prestart-XXXXXX";
        const int markerFd = mkstemp(marker);
        require(markerFd >= 0, "private start marker"); close(markerFd);
        require(unlink(marker) == 0, "remove empty marker before command");
        require(job.start("printf STARTED > " + quoted(marker), 256), "start pre-command parent-death probe");
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (job.result().output.find("PRESTART_READY=") == std::string::npos &&
                std::chrono::steady_clock::now() < until) {
            job.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const auto at = job.result().output.find("PRESTART_READY=");
        require(at != std::string::npos && job.result().output.find('\n', at) != std::string::npos,
                "guardian startup identity not observed");
        const auto guardian = std::stoi(job.result().output.substr(at + 15));
        require(guardian > 1, "invalid guardian identity");
        const auto supervisor = job.supervisorForTest();
        require(kill(supervisor, SIGKILL) == 0, "kill pre-command outer supervisor");
        siginfo_t supervisorInfo{};
        const auto supervisorDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < supervisorDeadline) {
            require(waitid(P_PID, supervisor, &supervisorInfo, WEXITED | WNOHANG | WNOWAIT) == 0,
                    "observe exact unreaped supervisor");
            if (supervisorInfo.si_pid == supervisor) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(supervisorInfo.si_pid == supervisor, "outer supervisor death unconfirmed");
        int status = 0; bool guardianReaped = false;
        const auto guardianDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < guardianDeadline) {
            const auto got = waitpid(guardian, &status, WNOHANG);
            require(got >= 0, "lost adopted guardian identity");
            if (got == guardian) { guardianReaped = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        finish(job);
        const bool commandStarted = access(marker, F_OK) == 0;
        if (commandStarted) require(unlink(marker) == 0, "remove private start marker");
        require(prctl(PR_SET_CHILD_SUBREAPER, 0) == 0, "restore private test subreaper");
        require(guardianReaped && !commandStarted && job.result().cleanupFailed && job.unavailable(),
                "pre-command reparenting launched command");
        std::cout << "PASS pre-command-outer-death-no-launch\n";
    }
    {
        pid_t child = 0, owner = 0;
        {
            DiagnosticProcess job;
            require(job.start(quoted(self) + " --descendant", 256), "start destructor job");
            owner = job.supervisorForTest();
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (job.result().output.find("CHILD=") == std::string::npos && std::chrono::steady_clock::now() < until) {
                job.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            child = descendant(job);
        }
        requireGone(child); requireGone(owner);
        std::cout << "PASS destructor-cancel-owned-reaper\n";
    }
    {
        DiagnosticProcess job; require(job.start("sleep 30", 256), "start stop job");
        require(job.cancelAndWait(), "bounded stop cleanup");
        require(!job.active() && job.result().cancelled && job.result().exitCode == 125, "cancel category");
        std::cout << "PASS explicit-stop\n";
    }
}
#endif
}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    (void)argc; (void)argv;
    DiagnosticProcess job;
    require(!job.start("exit 0", 256) && !job.active(), "Windows must not fall back to blocking popen");
    std::cout << "SKIP POSIX process/group/reaper cases; Windows refusal checked, not POSIX PASS\n";
    return 77;
#else
    try {
        if (argc == 2 && std::string(argv[1]) == "--burst") {
            const std::string output = std::string(128 * 1024, 'x') + "END";
            std::cout << output; return 0;
        }
        if (argc == 2 && (std::string(argv[1]) == "--descendant" || std::string(argv[1]) == "--orphan")) {
            int ready[2]; require(pipe(ready) == 0, "private ready pipe");
            const pid_t child = fork();
            require(child >= 0, "private descendant fork");
            if (child == 0) {
                close(ready[0]);
                require(setsid() > 0, "private descendant session");
                close(0); close(1); close(2);
                require(write(ready[1], "x", 1) == 1, "private ready write"); close(ready[1]);
                for (;;) pause();
            }
            close(ready[1]); char signal;
            require(read(ready[0], &signal, 1) == 1, "private descendant did not enter session"); close(ready[0]);
            std::cout << "CHILD=" << child << "\n" << std::flush;
            if (std::string(argv[1]) == "--orphan") return 0;
            for (;;) pause();
        }
        runCases(argv[0]);
        std::cout << "diagnostic_process_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n"; return 1;
    }
#endif
}
