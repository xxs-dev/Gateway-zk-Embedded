// Maintenance v2: server-sent realtime values (handoff doc 3.4) and service logs (3.7).
#include "v2_service.hpp"

#include "edge_gateway/system_monitor_direct_maintenance_bridge.hpp"
#include "v2_files.hpp"
#include "v2_json.hpp"
#include "v2_process.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <thread>

namespace edge_gateway {
namespace maintenance_v2 {

namespace bridge = system_monitor_direct_maintenance::bridge;

namespace {

const int kMaxStreams = 4;                // handoff 3.4: at most 4 stream connections
const std::size_t kMaxStreamPoints = 5000;  // per connection
const std::int64_t kHeartbeatMs = 5000;
const int kMaxLogLines = 5000;

// One of Service::streams_. The lease lives inside the stream lambda (shared_ptr), so the slot is
// returned when the response is dropped, whether or not the server ever ran the stream.
class StreamLease {
public:
    explicit StreamLease(std::atomic<int>* counter) : counter_(counter) {}
    StreamLease(const StreamLease&) = delete;
    StreamLease& operator=(const StreamLease&) = delete;
    ~StreamLease() { --*counter_; }

private:
    std::atomic<int>* counter_;
};

std::shared_ptr<StreamLease> acquireStream(std::atomic<int>& counter) {
    int current = counter.load();
    while (current < kMaxStreams) {
        if (counter.compare_exchange_weak(current, current + 1)) return std::make_shared<StreamLease>(&counter);
    }
    return nullptr;
}

bool parseUnsignedList(const std::string& text, std::vector<std::uint32_t>* out) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto comma = text.find(',', pos);
        if (comma == std::string::npos) comma = text.size();
        const auto item = text.substr(pos, comma - pos);
        if (item.empty() || item.size() > 10 || item.find_first_not_of("0123456789") != std::string::npos) return false;
        const auto value = std::stoull(item);
        if (value > 4294967295ULL) return false;
        out->push_back(static_cast<std::uint32_t>(value));
        pos = comma + 1;
    }
    return true;
}

std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto comma = text.find(',', pos);
        if (comma == std::string::npos) comma = text.size();
        if (comma > pos) out.push_back(text.substr(pos, comma - pos));
        pos = comma + 1;
    }
    return out;
}

void writeStreamPoint(Writer& w, const StoredPointValue& p) {
    const bool finite = std::isfinite(p.value);
    w.beginObject().fieldInt("index", p.index).field("meterCode", p.meterCode).field("pointCode", p.pointCode);
    if (finite) {
        w.fieldNum("value", p.value);
    } else {
        w.key("value").null();
    }
    w.fieldInt("quality", finite ? p.quality : 0).fieldInt("ts", p.ts).fieldBool("stale", p.stale).endObject();
}

struct Seen {
    double value = 0;
    bool finite = false;
    int quality = -1;
    std::int64_t ts = 0;
    bool stale = false;
    bool same(const StoredPointValue& p) const {
        const bool f = std::isfinite(p.value);
        return f == finite && (!f || std::memcmp(&value, &p.value, sizeof(double)) == 0) && quality == p.quality && ts == p.ts && stale == p.stale;
    }
};

int priorityOf(const std::string& level) {
    static const char* names[] = {"emerg", "alert", "crit", "err", "warning", "notice", "info", "debug"};
    for (int i = 0; i < 8; ++i) {
        if (level == names[i]) return i;
    }
    if (level == "error") return 3;
    if (level == "warn") return 4;
    if (level.size() == 1 && level[0] >= '0' && level[0] <= '7') return level[0] - '0';
    return -2;
}

bool parseMs(const std::string& text, std::int64_t* out) {
    if (text.empty() || text.size() > 16 || text.find_first_not_of("0123456789") != std::string::npos) return false;
    *out = std::stoll(text);
    return true;
}

// One `journalctl -o json` record -> the client's line object.
bool journalLine(const std::string& raw, Writer* w, std::int64_t* tsOut) {
    if (raw.empty() || raw[0] != '{') return false;
    JsonValue v;
    try {
        v = parseJson(raw, 4096);  // byte-array MESSAGE fields and many-field records are still one record
    } catch (...) {
        return false;
    }
    std::int64_t tsUs = 0;
    const auto realtime = str(v, "__REALTIME_TIMESTAMP");
    if (!realtime.empty() && realtime.find_first_not_of("0123456789") == std::string::npos) tsUs = std::stoll(realtime);
    std::string message;
    if (const auto* m = v.find("MESSAGE")) {
        if (m->isString()) {
            message = m->asString();
        } else if (m->isArray()) {  // journald encodes non-UTF-8 messages as a byte array
            for (const auto& b : m->asArray().values) {
                if (b->isNumber()) message += static_cast<char>(static_cast<int>(b->asNumber()) & 0xff);
            }
            message = "[binary " + std::to_string(message.size()) + " bytes]";
        }
    }
    std::string unit = str(v, "_SYSTEMD_UNIT");
    if (unit.empty()) unit = str(v, "UNIT");
    const auto priorityText = str(v, "PRIORITY");
    const int priority = priorityText.size() == 1 && priorityText[0] >= '0' && priorityText[0] <= '7' ? priorityText[0] - '0' : 6;
    *tsOut = tsUs / 1000;
    w->beginObject().fieldInt("ts", tsUs / 1000).field("unit", unit).fieldInt("priority", priority).field("message", message).endObject();
    return true;
}

}  // namespace

std::string sseEvent(const std::string& event, const std::string& data) {
    return "event: " + event + "\ndata: " + data + "\n\n";
}

// ---------------------------------------------------------------- GET /realtime/stream

Response Service::realtimeStream(const Request& r) {
    std::vector<std::uint32_t> indexes;
    if (!parseUnsignedList(r.queryValue("indexes"), &indexes)) return errorResponse(400, "validation_failed", "indexes must be a comma list of unsigned integers");
    const auto meters = splitList(r.queryValue("meters"));
    if (indexes.empty() && meters.empty()) return errorResponse(400, "validation_failed", "indexes or meters is required");
    if (indexes.size() > kMaxStreamPoints) return errorResponse(400, "validation_failed", "at most 5000 points per stream");
    std::int64_t interval = 250;
    const auto intervalText = r.queryValue("minIntervalMs");
    if (!intervalText.empty() && (!parseMs(intervalText, &interval) || interval < 100 || interval > 60000)) {
        return errorResponse(400, "validation_failed", "minIntervalMs must be 100..60000");
    }
    std::sort(indexes.begin(), indexes.end());
    indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
    auto lease = acquireStream(streams_);
    if (!lease) return errorResponse(429, "too_many_streams", "at most 4 realtime streams");
    Response response;
    const DirectConfig config = config_;
    response.stream = [config, indexes, meters, interval, lease](const StreamWriter& write) {
        bridge::Realtime rt(config);
        const auto machine = rt.machineCode();
        std::map<std::uint32_t, Seen> seen;
        bool first = true;
        std::int64_t lastSent = nowMs();
        while (true) {
            const auto now = nowMs();
            std::vector<StoredPointValue> values;
            std::set<std::uint32_t> taken;
            if (!indexes.empty()) {
                for (auto& v : rt.router().getLatestByIndexes(indexes, now)) {
                    if (taken.insert(v.index).second) values.push_back(v);
                }
            }
            for (const auto& meter : meters) {
                for (auto& v : rt.router().getLatestByMeter(machine, meter, now)) {
                    if (values.size() >= kMaxStreamPoints) break;
                    if (taken.insert(v.index).second) values.push_back(v);
                }
            }
            std::sort(values.begin(), values.end(), [](const StoredPointValue& a, const StoredPointValue& b) { return a.index < b.index; });
            Writer w;
            w.beginObject().fieldInt("ts", now).key("points").beginArray();
            int changed = 0;
            for (const auto& v : values) {
                auto& s = seen[v.index];
                if (!first && s.same(v)) continue;
                s.finite = std::isfinite(v.value);
                s.value = v.value;
                s.quality = v.quality;
                s.ts = v.ts;
                s.stale = v.stale;
                writeStreamPoint(w, v);
                ++changed;
            }
            w.endArray().endObject();
            if (first || changed > 0) {
                if (!write(sseEvent("points", w.text()))) return;
                lastSent = now;
                first = false;
            } else if (now - lastSent >= kHeartbeatMs) {
                Writer h;
                h.beginObject().fieldInt("ts", now).endObject();
                if (!write(sseEvent("heartbeat", h.text()))) return;
                lastSent = now;
            }
            const auto until = now + interval;
            while (nowMs() < until) {
                if (!write(std::string())) return;  // server stopping
                std::this_thread::sleep_for(std::chrono::milliseconds(std::min<std::int64_t>(100, std::max<std::int64_t>(1, until - nowMs()))));
            }
        }
    };
    return response;
}

// ---------------------------------------------------------------- GET /logs

std::vector<std::string> Service::logUnits() {
    auto units = expectedUnits();
    if (std::find(units.begin(), units.end(), "gateway-services.service") == units.end()) units.push_back("gateway-services.service");
    return units;
}

Response Service::logs(const Request& r) {
    const auto allowed = logUnits();
    std::vector<std::string> units;
    const auto unit = r.queryValue("unit");
    if (unit.empty()) {
        units = allowed;
    } else {
        for (const auto& u : splitList(unit)) {
            if (std::find(allowed.begin(), allowed.end(), u) == allowed.end()) return errorResponse(400, "unit_not_allowed", u + " is not a gateway service");
            units.push_back(u);
        }
    }
    int priority = -1;
    if (!r.queryValue("level").empty()) {
        priority = priorityOf(r.queryValue("level"));
        if (priority < -1) return errorResponse(400, "validation_failed", "level must be emerg..debug");
    }
    std::int64_t since = 0, until = 0, limit = 500;
    if (!r.queryValue("since").empty() && !parseMs(r.queryValue("since"), &since)) return errorResponse(400, "validation_failed", "since must be epoch ms");
    if (!r.queryValue("until").empty() && !parseMs(r.queryValue("until"), &until)) return errorResponse(400, "validation_failed", "until must be epoch ms");
    if (!r.queryValue("limit").empty() && (!parseMs(r.queryValue("limit"), &limit) || limit < 1 || limit > kMaxLogLines)) {
        return errorResponse(400, "validation_failed", "limit must be 1..5000");
    }
    const bool follow = r.queryValue("follow") == "true" || r.queryValue("follow") == "1";
    std::string journal = options_.journalCommand;
    if (journal.empty() && !findExecutable("journalctl", &journal)) return errorResponse(503, "journal_unavailable", "journalctl is not available on this device");
    std::vector<std::string> argv{journal, "--no-pager", "-o", "json", "-n", std::to_string(limit)};
    if (priority >= 0) argv.insert(argv.end(), {"-p", std::to_string(priority)});
    if (since > 0) argv.insert(argv.end(), {"--since", "@" + std::to_string(since / 1000)});
    if (until > 0 && !follow) argv.insert(argv.end(), {"--until", "@" + std::to_string(until / 1000)});
    for (const auto& u : units) argv.insert(argv.end(), {"-u", u});
    if (!follow) {
        const auto result = runCommand(argv, 20000, 16U * 1024U * 1024U);
        if (!result.started) return errorResponse(503, "journal_unavailable", "cannot run journalctl");
        Writer w;
        w.beginObject().fieldBool("success", true).fieldStrings("units", units).key("lines").beginArray();
        int count = 0;
        std::size_t pos = 0;
        while (pos < result.output.size()) {
            auto nl = result.output.find('\n', pos);
            if (nl == std::string::npos) nl = result.output.size();
            std::int64_t ts = 0;
            if (journalLine(result.output.substr(pos, nl - pos), &w, &ts)) ++count;
            pos = nl + 1;
        }
        w.endArray().fieldInt("count", count).fieldBool("truncated", count >= limit || result.timedOut).fieldInt("ts", nowMs()).endObject();
        return jsonResponse(200, w.text());
    }
    auto lease = acquireStream(streams_);
    if (!lease) return errorResponse(429, "too_many_streams", "at most 4 streams");
    argv.push_back("-f");
    Response response;
    response.stream = [argv, lease](const StreamWriter& write) {
        std::string pending;
        std::int64_t lastSent = nowMs();
        bool alive = true;
        runCommandStreaming(
            argv,
            [&](const std::string& chunk) {
                pending += chunk;
                std::size_t nl;
                while (alive && (nl = pending.find('\n')) != std::string::npos) {
                    Writer w;
                    std::int64_t ts = 0;
                    if (journalLine(pending.substr(0, nl), &w, &ts)) {
                        alive = write(sseEvent("line", w.text()));
                        lastSent = nowMs();
                    }
                    pending.erase(0, nl + 1);
                }
                if (pending.size() > 1024 * 1024) pending.clear();
                return alive;
            },
            [&]() {
                if (!alive) return false;
                if (nowMs() - lastSent >= 15000) {
                    Writer h;
                    h.beginObject().fieldInt("ts", nowMs()).endObject();
                    alive = write(sseEvent("heartbeat", h.text()));
                    lastSent = nowMs();
                }
                return alive && write(std::string());
            },
            500);
    };
    return response;
}

}  // namespace maintenance_v2
}  // namespace edge_gateway
