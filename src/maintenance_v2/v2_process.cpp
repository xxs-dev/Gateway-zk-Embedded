#include "v2_process.hpp"

#include "v2_files.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace edge_gateway {
namespace maintenance_v2 {

#ifndef _WIN32

namespace {

std::vector<char*> makeArgv(const std::vector<std::string>& args) {
    std::vector<char*> out;
    out.reserve(args.size() + 1);
    for (const auto& a : args) out.push_back(const_cast<char*>(a.c_str()));
    out.push_back(nullptr);
    return out;
}

// After fork in a multi-threaded process: only async-signal-safe calls.
void closeInheritedDescriptors(int keep) {
    long limit = ::sysconf(_SC_OPEN_MAX);
    if (limit < 0 || limit > 65536) limit = 65536;
    for (int fd = 3; fd < limit; ++fd) {
        if (fd != keep) ::close(fd);
    }
}

}  // namespace

CommandResult runCommand(const std::vector<std::string>& argv, int timeoutMs, std::size_t maxOutput, const std::function<bool()>& cancel) {
    CommandResult result;
    if (argv.empty()) return result;
    int pipeFds[2];
    if (::pipe(pipeFds) != 0) return result;
    ::fcntl(pipeFds[0], F_SETFD, FD_CLOEXEC);
    auto args = makeArgv(argv);
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        return result;
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        const int devNull = ::open("/dev/null", O_RDONLY);
        if (devNull >= 0) ::dup2(devNull, 0);
        ::dup2(pipeFds[1], 1);
        ::dup2(pipeFds[1], 2);
        closeInheritedDescriptors(-1);
        ::execv(args[0], args.data());
        ::_exit(127);
    }
    ::close(pipeFds[1]);
    result.started = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    bool open = true;
    char buffer[4096];
    while (open) {
        if (cancel && cancel()) {
            result.cancelled = true;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timedOut = true;
            break;
        }
        pollfd p {};
        p.fd = pipeFds[0];
        p.events = POLLIN;
        const int rc = ::poll(&p, 1, 200);
        if (rc < 0 && errno != EINTR) break;
        if (rc > 0) {
            const auto n = ::read(pipeFds[0], buffer, sizeof(buffer));
            if (n <= 0) {
                open = false;
            } else if (result.output.size() < maxOutput) {
                result.output.append(buffer, static_cast<std::size_t>(std::min<std::size_t>(n, maxOutput - result.output.size())));
            }
        }
    }
    ::close(pipeFds[0]);
    if (result.timedOut || result.cancelled) {
        ::kill(-pid, SIGKILL);
        ::kill(pid, SIGKILL);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!result.timedOut && !result.cancelled && WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
    return result;
}

bool spawnDetached(const std::vector<std::string>& argv, std::string* error) {
    if (argv.empty()) {
        *error = "empty command";
        return false;
    }
    int report[2];
    if (::pipe(report) != 0) {
        *error = std::strerror(errno);
        return false;
    }
    ::fcntl(report[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(report[1], F_SETFD, FD_CLOEXEC);
    auto args = makeArgv(argv);
    const pid_t child = ::fork();
    if (child < 0) {
        *error = std::strerror(errno);
        ::close(report[0]);
        ::close(report[1]);
        return false;
    }
    if (child == 0) {
        ::close(report[0]);
        ::setsid();
        const pid_t grandchild = ::fork();
        if (grandchild != 0) ::_exit(grandchild < 0 ? 1 : 0);
        const int devNull = ::open("/dev/null", O_RDWR);
        if (devNull >= 0) {
            ::dup2(devNull, 0);
            ::dup2(devNull, 1);
            ::dup2(devNull, 2);
        }
        closeInheritedDescriptors(report[1]);
        ::execv(args[0], args.data());
        const int err = errno;
        if (::write(report[1], &err, sizeof(err)) < 0) {
        }
        ::_exit(127);
    }
    ::close(report[1]);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    int err = 0;
    ssize_t n = 0;
    do {
        n = ::read(report[0], &err, sizeof(err));
    } while (n < 0 && errno == EINTR);
    ::close(report[0]);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        *error = "fork failed";
        return false;
    }
    if (n == static_cast<ssize_t>(sizeof(err))) {
        *error = std::string("exec ") + argv[0] + ": " + std::strerror(err);
        return false;
    }
    return true;
}

bool systemdRunning() {
    return isDirectory("/run/systemd/system");
}

bool findExecutable(const std::string& name, std::string* path) {
    if (!name.empty() && name[0] == '/') {
        if (::access(name.c_str(), X_OK) == 0) {
            *path = name;
            return true;
        }
        return false;
    }
    const char* dirs[] = {"/usr/local/sbin", "/usr/local/bin", "/usr/sbin", "/usr/bin", "/sbin", "/bin"};
    for (const auto* dir : dirs) {
        const auto candidate = std::string(dir) + "/" + name;
        if (::access(candidate.c_str(), X_OK) == 0) {
            *path = candidate;
            return true;
        }
    }
    return false;
}

std::string selfExecutable() {
    char buffer[4096];
    const auto n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (n <= 0) return std::string();
    buffer[n] = '\0';
    return buffer;
}

#else
CommandResult runCommand(const std::vector<std::string>&, int, std::size_t, const std::function<bool()>&) { return CommandResult(); }
bool spawnDetached(const std::vector<std::string>&, std::string* error) {
    *error = "unsupported";
    return false;
}
bool systemdRunning() { return false; }
bool findExecutable(const std::string&, std::string*) { return false; }
std::string selfExecutable() { return std::string(); }
#endif

}  // namespace maintenance_v2
}  // namespace edge_gateway
