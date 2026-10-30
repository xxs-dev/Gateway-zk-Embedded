#include "edge_gateway/dlt645_time_sync.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <sys/timex.h>
#endif

#include "edge_gateway/dlt645_client.hpp"
#include "edge_gateway/dlt645_codec.hpp"
#include "edge_gateway/filesystem_compat.hpp"

namespace edge_gateway {

namespace {

constexpr std::int64_t kRetryMs = 60000;
constexpr std::int64_t kRepeatLogMs = 3600000;

std::int64_t systemWallClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void stderrLog(const std::string& line) {
    std::cerr << line << std::endl;
}

// 23:55:00 .. 00:04:59 local time: meters freeze and settle at midnight.
bool nearMidnight(const std::tm& local) {
    const auto minutes = local.tm_hour * 60 + local.tm_min;
    return minutes >= 23 * 60 + 55 || minutes < 5;
}

std::string localText(std::int64_t wallMs) {
    if (wallMs <= 0) return std::string();
    const auto local = Dlt645Codec::localTime(wallMs);
    char text[32] = {0};
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    return text;
}

std::string jsonText(const std::string& value) {
    std::string out = "\"";
    for (const auto ch : value) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20U) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(ch)));
                out += escaped;
            } else {
                out += ch;
            }
        }
    }
    return out + "\"";
}

}  // namespace

Dlt645ClockState dlt645GatewayClockState(std::int64_t wallNowMs) {
    Dlt645ClockState state;
#ifdef _WIN32
    state.reason = "clock_state_unavailable";
    state.message = "未发送：无法判断系统时钟是否同步（本平台不提供内核同步状态）";
    (void)wallNowMs;
    return state;
#else
    struct timex kernel {};
    const auto clockState = adjtimex(&kernel);
    if (clockState == -1) {
        state.reason = "clock_state_unavailable";
        state.message = std::string("未发送：无法判断系统时钟是否同步（adjtimex 失败：") + std::strerror(errno) + "）";
        return state;
    }
    state.kernelRead = true;
    state.kernelState = clockState;
    state.kernelStatus = static_cast<long>(kernel.status);
    state.maxErrorUs = static_cast<long>(kernel.maxerror);
    state.estErrorUs = static_cast<long>(kernel.esterror);
    if (Dlt645Codec::localTime(wallNowMs).tm_year + 1900 < 2024) {
        state.reason = "clock_before_2024";
        state.message = "未发送：系统时钟未同步（系统时间早于 2024 年，RTC 未设置）";
        return state;
    }
    if ((kernel.status & STA_UNSYNC) != 0) {
        state.reason = "clock_unsynchronized";
        state.message = "未发送：系统时钟未同步（内核 STA_UNSYNC 置位，maxerror=" +
            std::to_string(state.maxErrorUs) + " us）";
        return state;
    }
    state.trusted = true;
    state.message = "系统时钟已同步（内核 STA_UNSYNC 已清除，maxerror=" + std::to_string(state.maxErrorUs) + " us）";
    return state;
#endif
}

std::string dlt645TimeSyncStatusJson(const Dlt645TimeSyncStatus& status) {
    std::ostringstream out;
    char kernelStatus[16] = {0};
    std::snprintf(kernelStatus, sizeof(kernelStatus), "0x%04lx", static_cast<unsigned long>(status.clock.kernelStatus));
    out << "{\"enabled\":" << (status.enabled ? "true" : "false")
        << ",\"intervalMs\":" << status.intervalMs
        << ",\"result\":" << jsonText(status.result)
        << ",\"reason\":" << jsonText(status.reason)
        << ",\"message\":" << jsonText(status.message)
        << ",\"attemptTs\":" << status.attemptWallMs
        << ",\"attemptTime\":" << jsonText(localText(status.attemptWallMs))
        << ",\"nextAttemptInMs\":" << status.nextAttemptInMs
        << ",\"lastSentTs\":" << status.lastSentWallMs
        << ",\"lastSentTime\":" << jsonText(localText(status.lastSentWallMs))
        << ",\"notSentSinceLastSent\":" << status.notSentSinceLastSent
        << ",\"clock\":{\"trusted\":" << (status.clock.trusted ? "true" : "false")
        << ",\"reason\":" << jsonText(status.clock.reason)
        << ",\"kernelRead\":" << (status.clock.kernelRead ? "true" : "false")
        << ",\"kernelState\":" << status.clock.kernelState
        << ",\"kernelStatus\":" << jsonText(kernelStatus)
#ifndef _WIN32
        << ",\"staUnsync\":" << ((status.clock.kernelStatus & STA_UNSYNC) != 0 ? "true" : "false")
#endif
        << ",\"maxErrorUs\":" << status.clock.maxErrorUs
        << ",\"estErrorUs\":" << status.clock.estErrorUs
        << "}}";
    return out.str();
}

std::string dlt645TimeSyncStatusPath(const std::string& deviceConfigPath) {
    auto name = deviceConfigPath;
    const auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0) name.resize(name.size() - 5);
    return "/run/modbus-gateway/dlt645-time-sync-" + name + ".json";
}

Dlt645TimeSync::Dlt645TimeSync(
    Dlt645TimeSyncConfig config,
    std::shared_ptr<Dlt645Client> client,
    std::string statusFile,
    ClockCheck clockCheck,
    WallClock wallClock,
    Log log
) : config_(std::move(config)),
    client_(std::move(client)),
    statusFile_(std::move(statusFile)),
    clockCheck_(clockCheck ? std::move(clockCheck) : ClockCheck(dlt645GatewayClockState)),
    wallClock_(wallClock ? std::move(wallClock) : WallClock(systemWallClockMs)),
    log_(log ? std::move(log) : Log(stderrLog)) {
    if (config_.enabled && !client_) {
        throw std::invalid_argument("DLT645 time sync needs a client");
    }
    status_.enabled = config_.enabled;
    status_.intervalMs = config_.intervalMs;
    status_.result = config_.enabled ? "pending" : "disabled";
    status_.message = config_.enabled
        ? "已启用，尚未尝试"
        : "未启用（dlt645.timeSync.enabled=false），不发送广播校时";
    // Replaces what an earlier run left behind (another configuration).
    writeStatusFile(0);
}

bool Dlt645TimeSync::tick(std::int64_t steadyNowMs) {
    if (!config_.enabled || (attempted_ && steadyNowMs < nextAttemptSteadyMs_)) {
        return false;
    }
    attempted_ = true;
    const auto wallNowMs = wallClock_();
    status_.attemptWallMs = wallNowMs;
    status_.clock = clockCheck_(wallNowMs);
    if (!status_.clock.trusted) {
        finish(steadyNowMs, "skipped", status_.clock.reason, status_.clock.message);
        return false;
    }
    if (nearMidnight(Dlt645Codec::localTime(wallNowMs))) {
        finish(steadyNowMs, "skipped", "midnight_window", "未发送：23:55～00:05 不校时，窗口结束后发送");
        return false;
    }
    std::int64_t sentWallMs = 0;
    try {
        // The client reads the clock once the bus is free, right before sending.
        client_->broadcastTime([this, &sentWallMs] {
            sentWallMs = wallClock_();
            return sentWallMs;
        });
    } catch (const Dlt645OutcomeUnknownError& ex) {
        finish(steadyNowMs, "failed", "send_uncertain",
               std::string("发送结果未知：校时帧可能已发出（") + ex.what() + "），表不应答，无法确认");
        return false;
    } catch (const std::exception& ex) {
        finish(steadyNowMs, "failed", "send_failed", std::string("未发送：串口发送失败（") + ex.what() + "）");
        return false;
    }
    status_.lastSentWallMs = sentWallMs;
    finish(steadyNowMs, "sent", std::string(),
           "已发送广播校时 " + localText(sentWallMs) + "（表不应答，是否采纳无法确认）");
    return true;
}

void Dlt645TimeSync::finish(
    std::int64_t steadyNowMs,
    const std::string& result,
    const std::string& reason,
    const std::string& message
) {
    const bool sent = result == "sent";
    status_.result = result;
    status_.reason = reason;
    status_.message = message;
    status_.nextAttemptInMs = sent ? config_.intervalMs : kRetryMs;
    status_.notSentSinceLastSent = sent ? 0 : status_.notSentSinceLastSent + 1;
    nextAttemptSteadyMs_ = steadyNowMs + status_.nextAttemptInMs;

    std::ostringstream line;
    line << "dlt645 time sync result=" << result;
    if (!reason.empty()) line << " reason=" << reason;
    line << ": " << message;
    if (!sent) {
        line << "; next attempt in " << status_.nextAttemptInMs / 1000 << " s, "
             << status_.notSentSinceLastSent << " attempt(s) not sent since "
             << (status_.lastSentWallMs > 0 ? "the last broadcast at " + localText(status_.lastSentWallMs)
                                            : std::string("the driver started"));
    }
    if (sent) {
        log_(line.str());
        lastLogKey_ = "sent";
        lastLogMs_ = steadyNowMs;
    } else {
        rateLimitedLog(result + "|" + reason, line.str(), steadyNowMs, lastLogKey_, lastLogMs_);
    }
    writeStatusFile(steadyNowMs);
}

void Dlt645TimeSync::rateLimitedLog(
    const std::string& key,
    const std::string& line,
    std::int64_t steadyNowMs,
    std::string& lastKey,
    std::int64_t& lastLogMs
) {
    if (key == lastKey && steadyNowMs - lastLogMs < kRepeatLogMs) {
        return;
    }
    log_(line);
    lastKey = key;
    lastLogMs = steadyNowMs;
}

void Dlt645TimeSync::writeStatusFile(std::int64_t steadyNowMs) {
    if (statusFile_.empty()) {
        return;
    }
    std::string error;
    const auto temporary = statusFile_ + ".tmp";
    {
        std::error_code ignored;
        const auto parent = filesystem::path(statusFile_).parent_path();
        if (!parent.empty()) filesystem::create_directories(parent, ignored);
        std::ofstream out(temporary.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
        out << dlt645TimeSyncStatusJson(status_) << '\n';
        out.close();
        if (!out) error = "cannot write " + temporary;
    }
#ifdef _WIN32
    if (error.empty()) std::remove(statusFile_.c_str());
#endif
    if (error.empty() && std::rename(temporary.c_str(), statusFile_.c_str()) != 0) {
        error = std::string("cannot replace the file: ") + std::strerror(errno);
    }
    if (error.empty()) {
        lastFileErrorKey_.clear();
        return;
    }
    rateLimitedLog(error, "dlt645 time sync status file " + statusFile_ + " not written: " + error,
                   steadyNowMs, lastFileErrorKey_, lastFileErrorLogMs_);
}

}  // namespace edge_gateway
