// Point mapping stage inside ComputeEngine (see point_mapping_stage.hpp).
#include "edge_gateway/point_mapping_stage.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <iostream>
#include <iomanip>
#include <sstream>

namespace edge_gateway {

namespace {

std::string jsonEscape(const std::string& text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string num(double v) {
    if (!std::isfinite(v)) return "null";
    std::ostringstream s;
    s << std::setprecision(10) << v;
    return s.str();
}

}  // namespace

PointMappingStage::PointMappingStage(PointMapping mapping, PointStoreRouter& router, std::int64_t outputTtlMs, std::int64_t maxFutureSkewMs)
    : mapping_(std::move(mapping)),
      router_(router),
      outputTtlMs_(std::max<std::int64_t>(1000, outputTtlMs)),
      maxFutureSkewMs_(maxFutureSkewMs),
      states_(mapping_.entries.size()) {}

bool PointMappingStage::sourceValid(const Optional<StoredPointValue>& v, std::int64_t nowMs, std::int64_t maxAgeMs, std::string* why) const {
    if (!v) {
        *why = "no value";
        return false;
    }
    if (v->quality != 1) {
        *why = "bad quality";
        return false;
    }
    if (v->stale) {
        *why = "stale";
        return false;
    }
    if (!std::isfinite(v->value)) {
        *why = "not a number";
        return false;
    }
    if (maxFutureSkewMs_ > 0 && v->ts > nowMs + maxFutureSkewMs_) {
        *why = "timestamp in the future";
        return false;
    }
    if (maxAgeMs > 0 && v->ts > 0 && nowMs - v->ts > maxAgeMs) {
        *why = "older than validity.timeoutMs";
        return false;
    }
    return true;
}

void PointMappingStage::setState(EntryState& s, const PointMappingEntry& e, const std::string& state, const std::string& message) {
    if (s.state != state && (state == "readback-timeout" || state == "writeback-failed" || state == "enum-miss" || state == "rejected")) {
        std::cerr << "point mapping " << e.virtualName << " (" << e.virtualIndex << "): " << state << (message.empty() ? "" : ": ") << message << std::endl;
    }
    s.state = state;
    s.message = message;
}

PointMappingRunResult PointMappingStage::runInputs(std::int64_t nowMs) {
    PointMappingRunResult result;
    std::vector<std::uint32_t> indexes;
    for (const auto& e : mapping_.entries) {
        if (e.output) continue;
        for (const auto& s : e.sources) indexes.push_back(s.index);
    }
    if (indexes.empty()) return result;
    std::unordered_map<std::uint32_t, StoredPointValue> latest;
    for (const auto& v : router_.getLatestByIndexes(indexes, nowMs)) latest[v.index] = v;
    const auto refreshMs = std::max<std::int64_t>(500, outputTtlMs_ / 3);
    for (std::size_t i = 0; i < mapping_.entries.size(); ++i) {
        const auto& e = mapping_.entries[i];
        if (e.output) continue;
        auto& s = states_[i];
        std::vector<double> values;
        std::string why;
        std::int64_t newest = 0;
        bool valid = true;
        for (const auto& src : e.sources) {
            const auto it = latest.find(src.index);
            Optional<StoredPointValue> v;
            if (it != latest.end()) v = it->second;
            std::string reason;
            if (!sourceValid(v, nowMs, e.validityTimeoutMs, &reason)) {
                valid = false;
                why = src.meterCode + "/" + src.pointCode + ": " + reason;
                break;
            }
            values.push_back(v->value);
            newest = std::max(newest, v->ts);
        }
        double ems = s.lastValue;
        if (valid && !pointMappingToEms(e, pointMappingAggregate(e.aggregate, values), &ems)) {
            valid = false;
            why = "enum has no entry for device value " + num(pointMappingAggregate(e.aggregate, values));
        }
        const bool changed = !s.published || valid != s.lastValid || (valid && ems != s.lastValue);
        if (!changed && nowMs - s.lastPublishAt < refreshMs) continue;
        PointValue out;
        out.index = e.virtualIndex;
        out.value = valid ? ems : s.lastValue;
        out.quality = valid ? 1 : 0;
        out.qualityMsg = valid ? "ok" : "mapping: " + why;
        out.ts = valid && newest > 0 && newest <= nowMs ? newest : nowMs;
        out.expireAt = nowMs + outputTtlMs_;
        const auto routed = router_.putLatestByIndex(out);
        if (!routed.accepted) {
            setState(s, e, "invalid", "cannot publish the virtual point: " + routed.message);
            result.errors.push_back(e.virtualName + ": " + routed.message);
            continue;
        }
        ++result.latestWrites;
        s.published = true;
        s.lastValid = valid;
        if (valid) s.lastValue = ems;
        s.lastPublishAt = nowMs;
        setState(s, e, valid ? "valid" : "invalid", valid ? std::string() : why);
    }
    return result;
}

PointMappingRunResult PointMappingStage::runOutputs(std::int64_t nowMs, std::size_t maxDeviceWrites) {
    PointMappingRunResult result;
    std::vector<PendingWriteCommand> pending;
    bool pendingLoaded = false;
    for (std::size_t i = 0; i < mapping_.entries.size(); ++i) {
        const auto& e = mapping_.entries[i];
        if (!e.output) continue;
        auto& s = states_[i];
        const auto& target = e.sources.front();
        // Verify the last command: driver write result, then the readback point within tolerance.
        if (!s.cmdId.empty()) {
            const auto route = router_.routeByIndex(target.index);
            const auto wb = route ? router_.getWritebackResult(*route, s.cmdId) : Optional<WritebackResultRecord>();
            std::string reason;
            const auto rb = router_.getLatestByIndex(e.readback.index, nowMs);
            if (wb && !wb->success) {
                ++s.verifyFailures;
                setState(s, e, "writeback-failed", wb->message);
                s.cmdId.clear();
            } else if (sourceValid(rb, nowMs, 0, &reason) && std::fabs(rb->value - s.sentValue) <= e.readbackTolerance) {
                setState(s, e, "ok", std::string());
                s.cmdId.clear();
            } else if (nowMs > s.verifyDeadline) {
                ++s.verifyFailures;
                setState(s, e, "readback-timeout",
                         "readback " + e.readback.pointCode + (rb ? " = " + num(rb->value) : std::string(" has no valid value")) + ", expected " + num(s.sentValue) +
                             " +/- " + num(e.readbackTolerance));
                s.cmdId.clear();
            }
        }
        const auto command = router_.getLatestByIndex(e.virtualIndex, nowMs);
        std::string why;
        if (!sourceValid(command, nowMs, 0, &why)) {
            if (s.cmdId.empty() && s.state != "readback-timeout" && s.state != "writeback-failed") setState(s, e, "no-command", why);
            continue;  // hold the device at its last value
        }
        double device = 0.0;
        if (!pointMappingToDevice(e, command->value, &device)) {
            setState(s, e, "enum-miss", "no device value for EMS code " + num(command->value));
            continue;
        }
        const bool due = !s.sent || std::fabs(device - s.sentValue) > e.deadband || (e.refreshMs > 0 && nowMs - s.sentAt >= e.refreshMs);
        if (!due) continue;
        if (!pendingLoaded) {
            pending = router_.peekPendingWrites();
            pendingLoaded = true;
        }
        const auto tolerance = std::max(e.deadband, 1e-9);
        const bool queued = std::any_of(pending.begin(), pending.end(), [&](const PendingWriteCommand& p) {
            return p.index == target.index && std::fabs(p.value - device) <= tolerance;
        });
        if (queued) continue;
        if (result.deviceWrites >= maxDeviceWrites) {
            ++result.deviceWritesSkipped;
            continue;
        }
        PendingWriteCommand cmd;
        cmd.index = target.index;
        cmd.value = device;
        cmd.source = "point-mapping";
        cmd.ts = nowMs;
        cmd.cmdId = "map-" + std::to_string(e.virtualIndex) + "-" + std::to_string(nowMs) + "-" + std::to_string(++sequence_);
        const auto routed = router_.submitWriteCommand(cmd);
        if (!routed.accepted) {
            setState(s, e, "rejected", routed.message);
            result.errors.push_back(e.virtualName + " -> " + std::to_string(target.index) + ": " + routed.message);
            continue;
        }
        ++result.deviceWrites;
        s.sent = true;
        s.sentValue = device;
        s.sentAt = nowMs;
        s.commandValue = command->value;
        s.cmdId = cmd.cmdId;
        s.verifyDeadline = nowMs + e.readbackTimeoutMs;
        setState(s, e, "verifying", std::string());
    }
    return result;
}

std::string PointMappingStage::statusJson(std::int64_t nowMs) const {
    std::size_t valid = 0, invalid = 0, ok = 0, verifying = 0, failed = 0;
    std::ostringstream items;
    for (std::size_t i = 0; i < mapping_.entries.size(); ++i) {
        const auto& e = mapping_.entries[i];
        const auto& s = states_[i];
        if (!e.output) {
            (s.state == "valid" ? valid : invalid) += 1;
        } else if (s.state == "ok") {
            ++ok;
        } else if (s.state == "verifying") {
            ++verifying;
        } else if (s.state != "no-command" && s.state != "idle") {
            ++failed;
        }
        if (i > 0) items << ',';
        items << "{\"virtual\":\"" << jsonEscape(e.virtualName) << "\",\"virtualIndex\":" << e.virtualIndex << ",\"direction\":\""
              << (e.output ? "out" : "in") << "\",\"state\":\"" << s.state << "\",\"message\":\"" << jsonEscape(s.message) << "\"";
        if (e.output) {
            items << ",\"deviceIndex\":" << e.sources.front().index << ",\"deviceValue\":" << (s.sent ? num(s.sentValue) : "null")
                  << ",\"lastWriteAt\":" << s.sentAt << ",\"verifyFailures\":" << s.verifyFailures;
        } else {
            items << ",\"value\":" << (s.published && s.lastValid ? num(s.lastValue) : "null");
        }
        items << '}';
    }
    std::ostringstream out;
    out << "{\"schemaVersion\":\"1.0\",\"ts\":" << nowMs << ",\"machineCode\":\"" << jsonEscape(mapping_.machineCode) << "\",\"entries\":"
        << mapping_.entries.size() << ",\"inputsValid\":" << valid << ",\"inputsInvalid\":" << invalid << ",\"outputsOk\":" << ok
        << ",\"outputsVerifying\":" << verifying << ",\"outputsFailed\":" << failed << ",\"items\":[" << items.str() << "]}";
    return out.str();
}

}  // namespace edge_gateway
