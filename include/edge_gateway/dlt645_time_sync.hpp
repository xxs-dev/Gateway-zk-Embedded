#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

class Dlt645Client;

// Kernel view of the gateway clock (adjtimex). trusted only when the kernel reports the clock
// synchronized (STA_UNSYNC cleared: a time daemon such as chrony with rtcsync, systemd-timesyncd or
// ntpd disciplines it; a plain settimeofday - date -s, hwclock -s, ntpdate - sets STA_UNSYNC) and the
// date is plausible (year >= 2024). The raw kernel fields are kept for field diagnosis.
struct Dlt645ClockState {
    bool trusted = false;
    std::string reason;   // "" | clock_unsynchronized | clock_before_2024 | clock_state_unavailable
    std::string message;  // operator text, "未发送：系统时钟未同步（...）" when not trusted
    bool kernelRead = false;
    int kernelState = 0;   // adjtimex() return value (TIME_ERROR = 5 when unsynchronized)
    long kernelStatus = 0; // timex.status
    long maxErrorUs = 0;   // timex.maxerror
    long estErrorUs = 0;   // timex.esterror
};

Dlt645ClockState dlt645GatewayClockState(std::int64_t wallNowMs);

// Outcome of the last broadcast time sync attempt: logged (rate limited) and written to the status
// file read by `Dlt645Driver --time-sync-status`. Only "sent" means a frame went out, and even then
// the meters do not answer, so acceptance is never reported.
struct Dlt645TimeSyncStatus {
    bool enabled = false;
    int intervalMs = 0;
    std::string result;   // disabled | pending | sent | skipped | failed
    std::string reason;   // clock_* (see Dlt645ClockState), midnight_window, send_failed, send_uncertain
    std::string message;  // operator text: "未发送：..." / "发送结果未知：..." / "已发送..."
    std::int64_t attemptWallMs = 0;
    std::int64_t nextAttemptInMs = 0;
    std::int64_t lastSentWallMs = 0;     // 0: nothing sent since the driver started
    std::int64_t notSentSinceLastSent = 0;
    Dlt645ClockState clock;
};

std::string dlt645TimeSyncStatusJson(const Dlt645TimeSyncStatus& status);

// /run/modbus-gateway/dlt645-time-sync-<device config file name without .json>.json
std::string dlt645TimeSyncStatusPath(const std::string& deviceConfigPath);

// DL/T 645 broadcast time sync (C=08H, dlt645.timeSync). Sends at most once per intervalMs (steady
// clock), only while the gateway clock is trusted and never between 23:55 and 00:05 local time
// (meters freeze/settle at midnight); a skipped or failed attempt is retried after a minute. The
// broadcast goes through the bus client, so it waits for the running transaction and quiet time.
// Every attempt updates status() and the status file; the log repeats an unchanged outcome at most
// once an hour.
class Dlt645TimeSync {
public:
    using ClockCheck = std::function<Dlt645ClockState(std::int64_t wallNowMs)>;
    using WallClock = std::function<std::int64_t()>;
    using Log = std::function<void(const std::string&)>;

    Dlt645TimeSync(
        Dlt645TimeSyncConfig config,
        std::shared_ptr<Dlt645Client> client,
        std::string statusFile = std::string(),
        ClockCheck clockCheck = dlt645GatewayClockState,
        WallClock wallClock = WallClock(),
        Log log = Log()
    );

    // Returns true when a broadcast frame was sent.
    bool tick(std::int64_t steadyNowMs);
    const Dlt645TimeSyncStatus& status() const { return status_; }

private:
    void finish(std::int64_t steadyNowMs, const std::string& result, const std::string& reason,
                const std::string& message);
    void writeStatusFile(std::int64_t steadyNowMs);
    void rateLimitedLog(const std::string& key, const std::string& line, std::int64_t steadyNowMs,
                        std::string& lastKey, std::int64_t& lastLogMs);

    Dlt645TimeSyncConfig config_;
    std::shared_ptr<Dlt645Client> client_;
    std::string statusFile_;
    ClockCheck clockCheck_;
    WallClock wallClock_;
    Log log_;
    Dlt645TimeSyncStatus status_;
    std::int64_t nextAttemptSteadyMs_ = 0;
    bool attempted_ = false;
    std::string lastLogKey_;
    std::int64_t lastLogMs_ = 0;
    std::string lastFileErrorKey_;
    std::int64_t lastFileErrorLogMs_ = 0;
};

}  // namespace edge_gateway
