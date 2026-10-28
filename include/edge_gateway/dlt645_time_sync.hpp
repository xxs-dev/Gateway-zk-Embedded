#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

class Dlt645Client;

// The gateway clock may be broadcast to meters only when the kernel reports it synchronized
// (adjtimex STA_UNSYNC cleared by chrony/ntpd/systemd-timesyncd, the source of timedatectl
// "System clock synchronized") and the date is plausible (year >= 2024). reason says why not.
bool dlt645GatewayClockTrusted(std::int64_t wallNowMs, std::string* reason);

// DL/T 645 broadcast time sync (C=08H, dlt645.timeSync). Sends at most once per intervalMs (steady
// clock), only while the gateway clock is trusted and never between 23:55 and 00:05 local time
// (meters freeze/settle at midnight); a skipped or failed attempt is retried after a minute. The
// broadcast goes through the bus client, so it waits for the running transaction and quiet time.
class Dlt645TimeSync {
public:
    using ClockCheck = std::function<bool(std::int64_t wallNowMs, std::string* reason)>;
    using WallClock = std::function<std::int64_t()>;

    Dlt645TimeSync(
        Dlt645TimeSyncConfig config,
        std::shared_ptr<Dlt645Client> client,
        ClockCheck clockTrusted = dlt645GatewayClockTrusted,
        WallClock wallClock = WallClock()
    );

    // Returns true when a broadcast was sent.
    bool tick(std::int64_t steadyNowMs);

private:
    void note(const std::string& message);

    Dlt645TimeSyncConfig config_;
    std::shared_ptr<Dlt645Client> client_;
    ClockCheck clockTrusted_;
    WallClock wallClock_;
    std::int64_t nextAttemptSteadyMs_ = 0;
    bool attempted_ = false;
    std::string lastNote_;
};

}  // namespace edge_gateway
