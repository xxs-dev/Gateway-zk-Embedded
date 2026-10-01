#include "edge_gateway/diagnostic_process.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

#ifndef _WIN32
#include <cerrno>
#include <climits>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

namespace edge_gateway {
namespace {
#ifndef _WIN32
struct WireResult {
    int magic;
    int exitCode;
    int signal;
    int timedOut;
    int cancelled;
    int cleanupFailed;
};
constexpr int kWireMagic = 0x44494147;
static_assert(sizeof(WireResult) <= 32, "diagnostic wire bound");

long long monotonicMs() {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return static_cast<long long>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

void sendResult(const WireResult& result) {
    const char* bytes = reinterpret_cast<const char*>(&result);
    std::size_t sent = 0;
    while (sent < sizeof(result)) {
        const auto n = write(3, bytes + sent, sizeof(result) - sent);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        sent += static_cast<std::size_t>(n);
    }
}

// Only direct, unreaped children of this private subreaper may be signalled.
// This also collects a diagnostic descendant which changed process group/session.
bool killOwnedChildren() {
    const int fd = open("/proc/thread-self/children", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char bytes[4096];
    ssize_t count = 0;
    bool ended = false;
    for (int reads = 0; reads < 4 && count < static_cast<ssize_t>(sizeof(bytes)); ++reads) {
        const auto n = read(fd, bytes + count, sizeof(bytes) - static_cast<std::size_t>(count));
        if (n == 0) { ended = true; break; }
        if (n < 0) { if (errno == EINTR) continue; break; }
        count += n;
    }
    close(fd);
    // Never signal a truncated PID token or trust an incomplete children list.
    if (!ended || count >= static_cast<ssize_t>(sizeof(bytes))) return false;
    bool valid = true;
    int child = 0;
    for (ssize_t i = 0; i <= count; ++i) {
        const char c = i == count ? ' ' : bytes[i];
        if (c >= '0' && c <= '9') {
            if (child > (INT_MAX - 9) / 10) return false;
            child = child * 10 + c - '0';
        } else if (c == ' ' || c == '\n') {
            if (child > 0 && kill(child, SIGKILL) != 0 && errno != ESRCH) valid = false;
            child = 0;
        } else {
            return false;
        }
    }
    return valid;
}

// After fork: only syscall/libc async-signal-safe operations and stack storage;
// never allocate, use C++ streams/mutexes, or call back into the multithreaded parent.
void supervise(const char* command, int timeoutMs, long maxFd, pid_t owner,
               bool testDelay, bool testShort, bool testDie) {
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, nullptr);
    for (int sig = 1; sig < NSIG; ++sig) signal(sig, SIG_DFL);
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    WireResult result{kWireMagic, -1, 0, 0, 0, 0};
    const long long started = monotonicMs();
    bool closed = false;
#ifdef SYS_close_range
    closed = syscall(SYS_close_range, 5U, ~0U, 0U) == 0;
#endif
    if (!closed) for (long fd = 5; fd < maxFd; ++fd) close(static_cast<int>(fd));
    const int devnull = open("/dev/null", O_RDONLY);
    if (devnull < 0 || dup2(devnull, STDIN_FILENO) < 0 ||
            prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) {
        sendResult(result); _exit(127);
    }
    if (devnull > 4) close(devnull);
    if (started < 0) { sendResult(result); _exit(127); }
    const int childrenProof = open("/proc/thread-self/children", O_RDONLY | O_CLOEXEC);
    if (childrenProof < 0) { sendResult(result); _exit(127); }
    close(childrenProof);
#ifdef EDGE_DIAGNOSTIC_PROCESS_TEST_HOOKS
    if (testDie) _exit(1); // No command/descendant has been created.
#else
    (void)testDelay; (void)testShort; (void)testDie;
#endif
    const pid_t child = fork();
    if (child < 0) { sendResult(result); _exit(127); }
    if (child == 0) {
        close(3); close(4);
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL);
        signal(SIGHUP, SIG_DFL); signal(SIGPIPE, SIG_DFL);
        if (setpgid(0, 0) != 0) _exit(126);
        char* argv[] = {const_cast<char*>("sh"), const_cast<char*>("-c"), const_cast<char*>(command), nullptr};
        execv("/bin/sh", argv);
        _exit(127);
    }
    // Both sides set the group, avoiding the fork/exec signal race.
    if (setpgid(child, child) != 0 && errno != EACCES && errno != ESRCH) {
        result.cleanupFailed = 1;
    }
    bool stopping = result.cleanupFailed != 0;
    bool sentFailure = false;
    bool leaderReaped = false;
    long long stoppedAt = started;
    for (;;) {
        pollfd cancel{4, POLLIN, 0};
        // Cancellation stays readable/HUP. A retained reaper must not spin
        // on that socket while waiting for kernel-confirmed child death.
        const int polled = stopping ? ::poll(nullptr, 0, 10) : ::poll(&cancel, 1, 10);
        const auto now = monotonicMs();
        if (!stopping) {
            siginfo_t info{};
            const int seen = waitid(P_PID, child, &info, WEXITED | WNOHANG | WNOWAIT);
            if (seen < 0 || polled < 0 || now < 0) {
                if (errno == EINTR && seen >= 0 && now >= 0) continue;
                result.cleanupFailed = 1; stopping = true;
            } else if (getppid() != owner || cancel.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
                result.cancelled = 1; stopping = true;
            } else if (now - started >= timeoutMs) {
                result.timedOut = 1; stopping = true;
            } else if (info.si_pid == child) {
                stopping = true;
            }
            if (stopping) {
                stoppedAt = now;
                // The leader has NOT been reaped: its PID/PGID cannot be reused.
                if (kill(-child, SIGKILL) != 0 && errno != ESRCH) result.cleanupFailed = 1;
                if (kill(child, SIGKILL) != 0 && errno != ESRCH) result.cleanupFailed = 1;
            }
        }
        if (!stopping) continue;
        // Reparented descendants are owned by this supervisor, not SystemMonitor.
        if (!killOwnedChildren()) result.cleanupFailed = 1;
        int status = 0;
        pid_t reaped;
        while ((reaped = waitpid(-1, &status, WNOHANG)) > 0) {
            if (reaped == child) {
                leaderReaped = true;
                if (WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
                else if (WIFSIGNALED(status)) {
                    result.signal = WTERMSIG(status);
                    result.exitCode = 128 + result.signal;
                }
            }
        }
        if (reaped < 0 && errno == ECHILD) {
#ifdef EDGE_DIAGNOSTIC_PROCESS_TEST_HOOKS
            if (testDelay && now >= 0 && now - stoppedAt < 300) {
                // All real children are reaped; exercise the actual retained
                // owner's stopping loop without creating a D-state process.
                if (!sentFailure) {
                    result.cleanupFailed = 1; result.exitCode = -1;
                    sendResult(result); sentFailure = true;
                }
                continue;
            }
#endif
            break;
        }
        if (!sentFailure && (now < 0 || now - stoppedAt >= 2000)) {
            result.cleanupFailed = 1;
            sendResult(result);
            sentFailure = true;
            // Never abandon owned children. Retain this isolated reaper until
            // the kernel acknowledges death; parent admission remains closed.
        }
    }
    if (!leaderReaped) result.cleanupFailed = 1;
    if (result.cleanupFailed) result.exitCode = -1;
    else if (result.cancelled) result.exitCode = 125;
    else if (result.timedOut) result.exitCode = 124;
#ifdef EDGE_DIAGNOSTIC_PROCESS_TEST_HOOKS
    if (testShort && !sentFailure) {
        write(3, &result, 7); sentFailure = true;
    }
#endif
    if (!sentFailure) sendResult(result);
    _exit(0);
}

void closeDescriptor(int& fd) {
    if (fd >= 0) close(fd);
    fd = -1;
}
#endif
}  // namespace

DiagnosticProcess::~DiagnosticProcess() {
    if (!cancelAndWait()) std::cerr << "diagnostic cleanup unresolved; private supervisor retains ownership\n";
#ifndef _WIN32
    closeDescriptor(outputFd_); closeDescriptor(statusFd_); closeDescriptor(cancelFd_);
#ifdef EDGE_DIAGNOSTIC_PROCESS_TEST_HOOKS
    closeDescriptor(heldWriterForTest);
#endif
#endif
}

bool DiagnosticProcess::start(const std::string& command, std::size_t outputLimit, int timeoutMs) {
    if (active() || poisoned_ || command.empty() || command.size() > 4096 || timeoutMs <= 0 || timeoutMs > 10000) return false;
    result_ = {}; statusSize_ = 0;
    reported_ = false; outputEnded_ = false; channelFailed_ = false; exitObservedMs_ = 0;
    outputLimit_ = std::max<std::size_t>(256, std::min<std::size_t>(1024 * 1024, outputLimit));
#ifdef _WIN32
    result_.output = "asynchronous diagnostics require Linux process supervision";
    return false;
#else
    int output[2]{-1, -1}, status[2]{-1, -1}, cancel[2]{-1, -1};
    int safe[3]{-1, -1, -1};
    const auto cleanup = [&] {
        for (auto pair : {output, status, cancel}) for (int i = 0; i < 2; ++i) if (pair[i] >= 0) close(pair[i]);
        for (auto fd : safe) if (fd >= 0) close(fd);
    };
    struct sigaction childHandler {};
    if (sigaction(SIGCHLD, nullptr, &childHandler) != 0 || childHandler.sa_handler == SIG_IGN ||
            (childHandler.sa_flags & SA_NOCLDWAIT)) return false;
    const long maxFd = sysconf(_SC_OPEN_MAX);
    if (maxFd <= 0 || maxFd > 1048576 || pipe2(output, O_CLOEXEC) != 0 || pipe2(status, O_CLOEXEC) != 0 ||
            socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, cancel) != 0) { cleanup(); return false; }
    safe[0] = fcntl(output[1], F_DUPFD_CLOEXEC, 10);
    safe[1] = fcntl(status[1], F_DUPFD_CLOEXEC, 10);
    safe[2] = fcntl(cancel[1], F_DUPFD_CLOEXEC, 10);
    if (safe[0] < 0 || safe[1] < 0 || safe[2] < 0 ||
            fcntl(output[0], F_SETFL, O_NONBLOCK) < 0 || fcntl(status[0], F_SETFL, O_NONBLOCK) < 0) {
        cleanup(); return false;
    }
    const char* rawCommand = command.c_str();
    const pid_t owner = getpid();
    bool testDelay = false, testShort = false, testDie = false;
#ifdef EDGE_DIAGNOSTIC_PROCESS_TEST_HOOKS
    testDelay = delayCleanupFailure; testShort = shortStatus; testDie = dieBeforeCommand;
#endif
    const pid_t supervisor = fork();
    if (supervisor < 0) { cleanup(); return false; }
    if (supervisor == 0) {
        if (dup2(safe[0], 1) < 0 || dup2(safe[1], 3) < 0 || dup2(safe[2], 4) < 0 || dup2(1, 2) < 0) _exit(127);
        supervise(rawCommand, timeoutMs, maxFd, owner, testDelay, testShort, testDie);
    }
    pid_ = supervisor;
    outputFd_ = output[0]; output[0] = -1;
    statusFd_ = status[0]; status[0] = -1;
    cancelFd_ = cancel[0]; cancel[0] = -1;
#ifdef EDGE_DIAGNOSTIC_PROCESS_TEST_HOOKS
    if (holdOutputWriter) heldWriterForTest = fcntl(output[1], F_DUPFD_CLOEXEC, 10);
#endif
    cleanup();
    return true;
#endif
}

bool DiagnosticProcess::poll() {
#ifdef _WIN32
    return false;
#else
    if (!active()) return false;
    // Even a continuously writing command gets only 64KiB of reads per tick.
    char buffer[4096];
    int readBudget = 16;
#ifdef EDGE_DIAGNOSTIC_PROCESS_TEST_HOOKS
    readBudget = std::max(1, std::min(16, readsPerPoll));
#endif
    for (int i = 0; i < readBudget; ++i) {
        const auto n = read(outputFd_, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (n == 0) outputEnded_ = true;
            else if (errno != EAGAIN && errno != EWOULDBLOCK) channelFailed_ = true;
            break;
        }
        if (reported_) continue;
        const auto keep = std::min<std::size_t>(static_cast<std::size_t>(n), outputLimit_ - result_.output.size());
        result_.output.append(buffer, keep);
        if (keep < static_cast<std::size_t>(n)) result_.truncated = true;
    }
    const auto drainStatus = [&] {
        for (int tries = 0; tries < 4 && statusSize_ < sizeof(WireResult); ++tries) {
            const auto n = read(statusFd_, statusBytes_ + statusSize_, sizeof(WireResult) - statusSize_);
            if (n > 0) statusSize_ += static_cast<std::size_t>(n);
            else if (n < 0 && errno == EINTR) continue;
            else {
                if ((n == 0 && statusSize_ != sizeof(WireResult)) ||
                        (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) channelFailed_ = true;
                break;
            }
        }
    };
#ifdef EDGE_DIAGNOSTIC_PROCESS_TEST_HOOKS
    if (skipFirstStatusRead) skipFirstStatusRead = false; else
#endif
    drainStatus();
    siginfo_t info{};
    const auto observed = waitid(P_PID, pid_, &info, WEXITED | WNOHANG | WNOWAIT);
    const bool exited = observed == 0 && info.si_pid == pid_;
    if (exited) drainStatus(); // Covers EAGAIN -> write+exit between read and wait.
    if (observed < 0 && errno != EINTR) channelFailed_ = true;
    const auto now = monotonicMs();
    if (exited && !exitObservedMs_) exitObservedMs_ = now;
    if (exited && !outputEnded_ && (now < 0 || exitObservedMs_ < 0 || now - exitObservedMs_ >= 2000)) channelFailed_ = true;
    WireResult wire{};
    if (statusSize_ == sizeof(wire)) {
        std::memcpy(&wire, statusBytes_, sizeof(wire));
        if (wire.magic != kWireMagic) channelFailed_ = true;
        if (!reported_) {
            result_.exitCode = wire.exitCode; result_.signal = wire.signal;
            result_.timedOut = wire.timedOut != 0; result_.cancelled = wire.cancelled != 0;
            result_.cleanupFailed = wire.cleanupFailed != 0;
        }
    }
    if (channelFailed_) {
        result_.cleanupFailed = true;
        if (cancelFd_ >= 0) send(cancelFd_, "x", 1, MSG_DONTWAIT | MSG_NOSIGNAL);
        closeDescriptor(cancelFd_);
    }
    // A cleanup failure is reportable while the isolated reaper still owns
    // children. Result notification never implies admission/ownership release.
    const bool notify = !reported_ && (result_.cleanupFailed || (exited && outputEnded_));
    if (exited && (outputEnded_ || channelFailed_)) {
        int status = 0;
        const auto waited = waitpid(pid_, &status, WNOHANG);
        if (waited == pid_) {
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || statusSize_ != sizeof(wire)) result_.cleanupFailed = true;
            pid_ = -1;
            closeDescriptor(outputFd_); closeDescriptor(statusFd_); closeDescriptor(cancelFd_);
        }
    } else if (observed < 0 && errno == ECHILD) {
        result_.cleanupFailed = true; pid_ = -1;
        closeDescriptor(outputFd_); closeDescriptor(statusFd_); closeDescriptor(cancelFd_);
    }
    if (result_.cleanupFailed) poisoned_ = true;
    if (!notify && (reported_ || !result_.cleanupFailed)) return false;
    if (result_.cleanupFailed) result_.exitCode = -1;
    if (result_.truncated) {
        const std::string marker = "\n[truncated]";
        if (result_.output.size() > outputLimit_ - marker.size()) result_.output.resize(outputLimit_ - marker.size());
        result_.output += marker;
    }
    reported_ = true;
    return true;
#endif
}

bool DiagnosticProcess::cancelAndWait() {
#ifndef _WIN32
    if (cancelFd_ >= 0) send(cancelFd_, "x", 1, MSG_DONTWAIT | MSG_NOSIGNAL);
    closeDescriptor(cancelFd_);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (active() && std::chrono::steady_clock::now() < until) {
        poll();
        if (active()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
#endif
    return !active() && !result_.cleanupFailed;
}
}  // namespace edge_gateway
