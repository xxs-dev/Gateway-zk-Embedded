#pragma once

// Child processes without a shell (argv only): bounded output, timeout,
// optional cancel check; detached helpers survive a SystemMonitor restart.

#include <functional>
#include <string>
#include <vector>

namespace edge_gateway {
namespace maintenance_v2 {

struct CommandResult {
    bool started = false;
    bool timedOut = false;
    bool cancelled = false;
    int exitCode = -1;
    std::string output;  // stdout + stderr, truncated to maxOutput
};

CommandResult runCommand(
    const std::vector<std::string>& argv,
    int timeoutMs,
    std::size_t maxOutput = 65536,
    const std::function<bool()>& cancel = std::function<bool()>()
);

// Like runCommand without a deadline: onOutput gets every chunk (stdout + stderr) as it arrives,
// keepGoing is asked about every pollMs; either returning false kills the child.
CommandResult runCommandStreaming(
    const std::vector<std::string>& argv,
    const std::function<bool(const std::string& chunk)>& onOutput,
    const std::function<bool()>& keepGoing,
    int pollMs = 500
);

// Double fork + setsid; stdin/stdout/stderr -> /dev/null. Returns once the
// grandchild has been exec'd (or failed to).
bool spawnDetached(const std::vector<std::string>& argv, std::string* error);

bool systemdRunning();
bool findExecutable(const std::string& name, std::string* path);
std::string selfExecutable();

}  // namespace maintenance_v2
}  // namespace edge_gateway
