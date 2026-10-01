#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace edge_gateway {

struct DiagnosticProcessResult {
    int exitCode = -1;
    int signal = 0;
    bool timedOut = false;
    bool cancelled = false;
    bool cleanupFailed = false;
    bool truncated = false;
    std::string output;
};

// Single owned job, nonblocking main-loop polls, no worker thread/callback/this.
// The caller must supply only its existing fixed diagnostic command vocabulary.
class DiagnosticProcess {
public:
    DiagnosticProcess() = default;
    ~DiagnosticProcess();
    DiagnosticProcess(const DiagnosticProcess&) = delete;
    DiagnosticProcess& operator=(const DiagnosticProcess&) = delete;

    bool start(const std::string& command, std::size_t outputLimit, int timeoutMs = 10000);
    bool poll();
    bool active() const { return pid_ > 0; }
    bool unavailable() const { return poisoned_; }
    const DiagnosticProcessResult& result() const { return result_; }
    bool cancelAndWait();
#ifdef EDGE_DIAGNOSTIC_PROCESS_TEST_HOOKS
    // Compiled only into the isolated test target, never edge_gateway.
    bool skipFirstStatusRead = false;
    int readsPerPoll = 16;
    bool delayCleanupFailure = false;
    bool shortStatus = false;
    bool dieBeforeCommand = false;
    bool holdOutputWriter = false;
    int heldWriterForTest = -1;
    int supervisorForTest() const { return pid_; }
#endif

private:
    int pid_ = -1;
    int outputFd_ = -1;
    int statusFd_ = -1;
    int cancelFd_ = -1;
    std::size_t outputLimit_ = 0;
    unsigned char statusBytes_[32]{};
    std::size_t statusSize_ = 0;
    bool reported_ = false;
    bool outputEnded_ = false;
    bool channelFailed_ = false;
    bool poisoned_ = false;
    std::int64_t exitObservedMs_ = 0;
    DiagnosticProcessResult result_;
};

}  // namespace edge_gateway
