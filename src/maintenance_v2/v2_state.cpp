#include "v2_state.hpp"

#include "v2_crypto.hpp"
#include "v2_files.hpp"
#include "v2_json.hpp"

#include <algorithm>
#include <fstream>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace edge_gateway {
namespace maintenance_v2 {

namespace {

std::string base64Url(const std::string& bytes) {
    auto out = base64Encode(bytes);
    while (!out.empty() && out.back() == '=') out.pop_back();
    for (auto& c : out) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    return out;
}

bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

}  // namespace

// ---------------------------------------------------------------- sessions

SessionStore::SessionStore(std::string file, std::int64_t ttlMs, std::int64_t maxMs)
    : file_(std::move(file)), ttlMs_(ttlMs), maxMs_(maxMs) {}

void SessionStore::load() {
    if (loaded_) return;
    loaded_ = true;
    std::string text;
    if (!readFile(file_, &text)) return;
    try {
        const auto root = parseJson(text);
        const auto* list = root.find("sessions");
        if (list == nullptr || !list->isArray()) return;
        for (const auto& item : list->asArray().values) {
            Entry e;
            e.hash = str(*item, "tokenSha256");
            e.client = str(*item, "client");
            integer(*item, "createdAt", &e.createdAt);
            integer(*item, "lastUsedAt", &e.lastUsedAt);
            if (e.hash.size() == 64) entries_.push_back(e);
        }
    } catch (...) {
    }
}

void SessionStore::save() {
    Writer w;
    w.beginObject().key("sessions").beginArray();
    for (const auto& e : entries_) {
        w.beginObject().field("tokenSha256", e.hash).field("client", e.client).fieldInt("createdAt", e.createdAt).fieldInt("lastUsedAt", e.lastUsedAt).endObject();
    }
    w.endArray().endObject();
    try {
        writeFileAtomic(file_, w.text());
        savedAt_ = nowMs();
    } catch (...) {
        // Sessions still work in memory; a restart then asks for the password again.
    }
}

std::string SessionStore::create(const std::string& client, std::int64_t now, std::int64_t* expiresAt) {
    std::string raw;
    if (!Crypto::instance().randomBytes(32, &raw)) return std::string();
    const auto token = base64Url(raw);
    std::lock_guard<std::mutex> lock(mutex_);
    load();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& e) {
                       return now - e.lastUsedAt > ttlMs_ || now - e.createdAt > maxMs_;
                   }),
                   entries_.end());
    while (entries_.size() >= 64) entries_.erase(entries_.begin());
    Entry e;
    e.hash = Crypto::instance().sha256Hex(token);
    e.client = client;
    e.createdAt = now;
    e.lastUsedAt = now;
    entries_.push_back(e);
    save();
    *expiresAt = std::min(now + ttlMs_, now + maxMs_);
    return token;
}

SessionStore::Check SessionStore::touch(const std::string& token, std::int64_t now, std::int64_t* expiresAt) {
    if (token.empty()) return Check::Missing;
    const auto hash = Crypto::instance().sha256Hex(token);
    std::lock_guard<std::mutex> lock(mutex_);
    load();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (!constantTimeEquals(it->hash, hash)) continue;
        if (now - it->lastUsedAt > ttlMs_ || now - it->createdAt > maxMs_) {
            entries_.erase(it);
            save();
            return Check::Expired;
        }
        it->lastUsedAt = now;
        if (expiresAt) *expiresAt = std::min(now + ttlMs_, it->createdAt + maxMs_);
        if (now - savedAt_ > 60000) save();
        return Check::Ok;
    }
    return Check::Expired;
}

void SessionStore::revoke(const std::string& token) {
    const auto hash = Crypto::instance().sha256Hex(token);
    std::lock_guard<std::mutex> lock(mutex_);
    load();
    const auto before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return constantTimeEquals(e.hash, hash); }),
                   entries_.end());
    if (entries_.size() != before) save();
}

// ---------------------------------------------------------------- audit

std::string AuditLog::toJson(const AuditEntry& e, bool withHash) {
    // Keys in RFC 8785 order (all ASCII): this string is the JCS form when withHash == false.
    Writer w;
    w.beginObject()
        .field("action", e.action);
    if (withHash) w.field("hash", e.hash);
    w.field("operator", e.operatorName)
        .field("prevHash", e.prevHash)
        .field("result", e.result)
        .fieldInt("seq", e.seq)
        .field("target", e.target)
        .field("ticketId", e.ticketId)
        .field("transactionId", e.transactionId)
        .fieldInt("ts", e.ts)
        .endObject();
    return w.text();
}

std::string AuditLog::hashOf(const AuditEntry& e) {
    return Crypto::instance().sha256Hex(e.prevHash + toJson(e, false));
}

namespace {

bool lastLine(const std::string& file, std::string* line) {
    std::ifstream in(file.c_str(), std::ios::binary);
    if (!in.is_open()) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    const std::streamoff window = std::min<std::streamoff>(size, 65536);
    in.seekg(size - window);
    std::string tail(static_cast<std::size_t>(window), '\0');
    in.read(&tail[0], window);
    while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r')) tail.pop_back();
    if (tail.empty()) return false;
    const auto pos = tail.find_last_of('\n');
    *line = pos == std::string::npos ? tail : tail.substr(pos + 1);
    return true;
}

bool parseEntry(const std::string& line, AuditEntry* e) {
    try {
        const auto v = parseJson(line, 1000);
        integer(v, "seq", &e->seq);
        integer(v, "ts", &e->ts);
        e->operatorName = str(v, "operator");
        e->ticketId = str(v, "ticketId");
        e->action = str(v, "action");
        e->target = str(v, "target");
        e->result = str(v, "result");
        e->transactionId = str(v, "transactionId");
        e->prevHash = str(v, "prevHash");
        e->hash = str(v, "hash");
        return e->seq > 0 && e->hash.size() == 64;
    } catch (...) {
        return false;
    }
}

}  // namespace

bool AuditLog::append(AuditEntry entry, std::string* error) {
    FileLock lock(file_ + ".lock", true);
    if (!lock.locked()) {
        if (error) *error = "cannot lock audit log";
        return false;
    }
    AuditEntry last;
    std::string line;
    if (lastLine(file_, &line) && parseEntry(line, &last)) {
        entry.seq = last.seq + 1;
        entry.prevHash = last.hash;
    } else {
        entry.seq = 1;
        entry.prevHash = std::string(64, '0');
    }
    if (entry.ts == 0) entry.ts = nowMs();
    entry.hash = hashOf(entry);
    if (entry.hash.empty() || !appendLineDurable(file_, toJson(entry, true))) {
        if (error) *error = "cannot append audit log";
        return false;
    }
    return true;
}

std::vector<AuditEntry> AuditLog::since(std::int64_t sinceSeq, std::size_t limit) const {
    std::vector<AuditEntry> out;
    std::ifstream in(file_.c_str(), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        AuditEntry e;
        if (!parseEntry(line, &e) || e.seq <= sinceSeq) continue;
        out.push_back(e);
        if (out.size() >= limit) break;
    }
    return out;
}

// ---------------------------------------------------------------- debug mode

DebugMode DebugModeStore::get() {
    std::lock_guard<std::mutex> lock(mutex_);
    DebugMode m;
    std::string text;
    if (!readFile(file_, &text)) return m;
    try {
        const auto v = parseJson(text);
        m.active = boolean(v, "active", false);
        integer(v, "since", &m.since);
        m.operatorName = str(v, "operator");
        m.operatorId = str(v, "operatorId");
        m.reason = str(v, "reason");
        m.ticketId = str(v, "ticketId");
        integer(v, "lastActivityAt", &m.lastActivityAt);
    } catch (...) {
    }
    return m;
}

void DebugModeStore::set(const DebugMode& m) {
    std::lock_guard<std::mutex> lock(mutex_);
    Writer w;
    w.beginObject()
        .fieldBool("active", m.active)
        .fieldInt("since", m.since)
        .field("operator", m.operatorName)
        .field("operatorId", m.operatorId)
        .field("reason", m.reason)
        .field("ticketId", m.ticketId)
        .fieldInt("lastActivityAt", m.lastActivityAt)
        .endObject();
    writeFileAtomic(file_, w.text(), 0644);  // readable by the local display (banner)
}

bool DebugModeStore::touch(std::int64_t now, std::int64_t idleMs, DebugMode* ended) {
    auto m = get();
    if (!m.active) return false;
    const auto last = std::max(m.lastActivityAt, m.since);
    if (now - last > idleMs) {
        if (ended) *ended = m;
        set(DebugMode());
        return true;
    }
    if (now - m.lastActivityAt > 60000) {
        m.lastActivityAt = now;
        set(m);
    }
    return false;
}

}  // namespace maintenance_v2
}  // namespace edge_gateway
