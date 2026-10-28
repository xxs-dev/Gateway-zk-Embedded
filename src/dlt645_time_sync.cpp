#include "edge_gateway/dlt645_time_sync.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <utility>

#ifndef _WIN32
#include <sys/timex.h>
#endif

#include "edge_gateway/dlt645_client.hpp"
#include "edge_gateway/dlt645_codec.hpp"

namespace edge_gateway {

namespace {

constexpr std::int64_t kRetryMs = 60000;

std::int64_t systemWallClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// 23:55:00 .. 00:04:59 local time: meters freeze and settle at midnight.
bool nearMidnight(const std::tm& local) {
    const auto minutes = local.tm_hour * 60 + local.tm_min;
    return minutes >= 23 * 60 + 55 || minutes < 5;
}

}  // namespace

bool dlt645GatewayClockTrusted(std::int64_t wallNowMs, std::string* reason) {
    const auto setReason = [reason](const char* text) {
        if (reason != nullptr) *reason = text;
        return false;
    };
    if (Dlt645Codec::localTime(wallNowMs).tm_year + 1900 < 2024) {
        return setReason("gateway clock is before 2024 (RTC not set)");
    }
#ifdef _WIN32
    return setReason("clock synchronization state is not available on this platform");
#else
    struct timex state {};
    if (adjtimex(&state) == -1) {
        return setReason("adjtimex failed");
    }
    if ((state.status & STA_UNSYNC) != 0) {
        return setReason("gateway clock is not NTP-synchronized");
    }
    return true;
#endif
}

Dlt645TimeSync::Dlt645TimeSync(
    Dlt645TimeSyncConfig config,
    std::shared_ptr<Dlt645Client> client,
    ClockCheck clockTrusted,
    WallClock wallClock
) : config_(std::move(config)),
    client_(std::move(client)),
    clockTrusted_(std::move(clockTrusted)),
    wallClock_(wallClock ? std::move(wallClock) : WallClock(systemWallClockMs)) {
    if (config_.enabled && !client_) {
        throw std::invalid_argument("DLT645 time sync needs a client");
    }
}

bool Dlt645TimeSync::tick(std::int64_t steadyNowMs) {
    if (!config_.enabled || (attempted_ && steadyNowMs < nextAttemptSteadyMs_)) {
        return false;
    }
    attempted_ = true;
    nextAttemptSteadyMs_ = steadyNowMs + kRetryMs;
    const auto wallNowMs = wallClock_();
    std::string reason;
    if (!clockTrusted_(wallNowMs, &reason)) {
        note("dlt645 time sync skipped: " + reason);
        return false;
    }
    if (nearMidnight(Dlt645Codec::localTime(wallNowMs))) {
        note("dlt645 time sync deferred: no broadcast between 23:55 and 00:05");
        return false;
    }
    try {
        client_->broadcastTime(wallClock_);
    } catch (const std::exception& ex) {
        note(std::string("dlt645 time sync failed: ") + ex.what());
        return false;
    }
    nextAttemptSteadyMs_ = steadyNowMs + config_.intervalMs;
    lastNote_.clear();
    std::cerr << "dlt645 time sync broadcast sent" << std::endl;
    return true;
}

void Dlt645TimeSync::note(const std::string& message) {
    if (message != lastNote_) {
        std::cerr << message << std::endl;
        lastNote_ = message;
    }
}

}  // namespace edge_gateway
