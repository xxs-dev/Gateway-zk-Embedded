// Private directed ownership experiments; cleanup by this harness is NOT product cleanup.
#include "edge_gateway/diagnostic_process.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
using edge_gateway::DiagnosticProcess;
static void need(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
#ifndef _WIN32
static void tickUntil(DiagnosticProcess& job, bool terminal) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < end) {
        const bool done = job.poll();
        if (terminal ? done : job.result().output.find("COMMAND=") != std::string::npos) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("directed experiment deadline");
}
static pid_t commandPid(const DiagnosticProcess& job) {
    const auto at = job.result().output.find("COMMAND="); need(at != std::string::npos, "missing command identity");
    const auto pid = std::stoi(job.result().output.substr(at + 8)); need(pid > 1, "unsafe command pid"); return pid;
}
static void reaped(pid_t pid) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int status = 0;
    while (std::chrono::steady_clock::now() < end) {
        const auto got = waitpid(pid, &status, WNOHANG);
        if (got == pid) return;
        need(got >= 0, "lost private owner wait identity");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("private owner did not exit");
}
#endif
int main(int argc, char** argv) {
#ifdef _WIN32
    (void)argc; (void)argv; return 77;
#else
    try {
        need(argc == 2, "mode required"); const std::string mode = argv[1];
        need(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0, "private experiment subreaper");
        if (mode == "fd") {
            const int original = open("/dev/null", O_RDONLY); need(original >= 0, "private inherited fd");
            const int held = fcntl(original, F_DUPFD, 100); close(original); need(held == 100, "private fd100 occupied");
            DiagnosticProcess job;
            need(job.start("test ! -e /proc/$$/fd/100 && test ! -e /proc/$$/fd/3 && test ! -e /proc/$$/fd/4", 256), "fd probe start");
            tickUntil(job, true);
            need(job.result().exitCode == 0 && !job.result().cleanupFailed && !job.active(), "inherited fd escaped closure");
            need(fcntl(held, F_GETFD) >= 0, "parent descriptor changed"); close(held);
            std::cout << "{\"case\":\"fd\",\"passed\":true,\"parentFdPreserved\":true}\n"; return 0;
        }
        if (mode == "parent-death") {
            int pipefd[2]; need(pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) == 0, "private identity pipe");
            const pid_t owner = fork(); need(owner >= 0, "private parent fork");
            if (!owner) {
                close(pipefd[0]); DiagnosticProcess job;
                if (!job.start("printf 'COMMAND=%s\\n' $$; sleep 30", 256)) _exit(2);
                tickUntil(job, false);
                const pid_t ids[2] = {job.supervisorForTest(), commandPid(job)};
                if (write(pipefd[1], ids, sizeof(ids)) != sizeof(ids)) _exit(3);
                _exit(0); // Actual owning parent death, deliberately no destructor.
            }
            close(pipefd[1]); pid_t ids[2]{};
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            ssize_t count = -1;
            while (std::chrono::steady_clock::now() < end) {
                count = read(pipefd[0], ids, sizeof(ids));
                if (count >= 0) break;
                need(errno == EAGAIN || errno == EINTR, "private pipe error");
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            close(pipefd[0]); need(count == sizeof(ids) && ids[0] > 1 && ids[1] > 1, "parent death identity missing");
            reaped(owner); reaped(ids[0]);
            need(kill(ids[1], 0) < 0 && errno == ESRCH, "product retained command after parent death");
            std::cout << "{\"case\":\"parent-death\",\"passed\":true,\"productCommandAbsent\":true}\n"; return 0;
        }
        need(mode == "supervisor-death", "invalid directed mode");
        DiagnosticProcess job; need(job.start("printf 'COMMAND=%s\\n' $$; sleep 30", 256), "supervisor death start");
        tickUntil(job, false); const auto command = commandPid(job);
        need(kill(job.supervisorForTest(), SIGKILL) == 0, "private supervisor signal"); tickUntil(job, true);
        const auto cleanupDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (kill(command, 0) == 0 && std::chrono::steady_clock::now() < cleanupDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const bool commandAlive = kill(command, 0) == 0;
        need(job.result().cleanupFailed && job.unavailable() && !job.start("exit 0", 256), "dead owner did not poison admission");
        // If orphaned, it is now our direct child, still unreaped. No PID reuse.
        bool outerRescued = false;
        if (commandAlive) {
            siginfo_t info{};
            need(waitid(P_PID, command, &info, WEXITED | WNOHANG | WNOWAIT) == 0, "orphan not owned by private harness");
            need(getpgid(command) == command, "command group identity changed");
            need(kill(-command, SIGKILL) == 0, "outer rescue of proven command group"); reaped(command);
            outerRescued = true;
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (std::chrono::steady_clock::now() < end) {
                int status; const auto got = waitpid(-1, &status, WNOHANG);
                if (got < 0 && errno == ECHILD) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        std::cout << "{\"case\":\"supervisor-death\",\"admissionPoisoned\":true,\"productCommandAbsent\":"
                  << (commandAlive ? "false" : "true") << ",\"outerHarnessRescued\":" << (outerRescued ? "true" : "false") << "}\n";
        return commandAlive ? 1 : 0; // Never count outer rescue as product PASS.
    } catch (const std::exception& error) { std::cerr << error.what() << "\n"; return 2; }
#endif
}
