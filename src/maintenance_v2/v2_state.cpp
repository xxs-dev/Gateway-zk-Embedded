#include "v2_state.hpp"

#include "v2_crypto.hpp"
#include "v2_files.hpp"
#include "v2_json.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>

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

bool SessionStore::valid(const std::string& token, std::int64_t now) {
    if (token.empty()) return false;
    const auto hash = Crypto::instance().sha256Hex(token);
    std::lock_guard<std::mutex> lock(mutex_);
    load();
    for (const auto& e : entries_) {
        if (constantTimeEquals(e.hash, hash)) return now - e.lastUsedAt <= ttlMs_ && now - e.createdAt <= maxMs_;
    }
    return false;
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
    if (!e.clientIp.empty()) w.field("clientIp", e.clientIp);
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

// Lines of the file's tail (up to 1 MB), newest first; a torn or foreign line is skipped by the caller.
std::vector<std::string> tailLines(const std::string& file) {
    std::vector<std::string> lines;
    std::ifstream in(file.c_str(), std::ios::binary);
    if (!in.is_open()) return lines;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    const std::streamoff window = std::min<std::streamoff>(size, 1 << 20);
    in.seekg(size - window);
    std::string tail(static_cast<std::size_t>(window), '\0');
    in.read(&tail[0], window);
    std::size_t end = tail.size();
    while (end > 0) {
        const auto nl = tail.rfind('\n', end - 1);
        const auto begin = nl == std::string::npos ? 0 : nl + 1;
        auto line = tail.substr(begin, end - begin);
        while (!line.empty() && line.back() == '\r') line.pop_back();
        // The first line of a cut window is partial unless the window starts the file.
        if (!line.empty() && (begin > 0 || window == size)) lines.push_back(line);
        if (nl == std::string::npos) break;
        end = nl;
    }
    return lines;
}

// Valid UTF-8 (the strict parser refuses anything else) and bounded: one entry always fits the tail window.
std::string auditText(const std::string& in, std::size_t maxBytes) {
    std::string out;
    std::size_t i = 0;
    while (i < in.size()) {
        const auto c = static_cast<unsigned char>(in[i]);
        std::size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        bool valid = n > 0 && i + n <= in.size();
        for (std::size_t k = 1; valid && k < n; ++k) valid = (static_cast<unsigned char>(in[i + k]) & 0xC0) == 0x80;
        if (valid && n == 2) valid = c >= 0xC2;
        if (valid && n == 3) {
            const auto c1 = static_cast<unsigned char>(in[i + 1]);
            valid = !(c == 0xE0 && c1 < 0xA0) && !(c == 0xED && c1 >= 0xA0);  // overlong, surrogates
        }
        if (valid && n == 4) {
            const auto c1 = static_cast<unsigned char>(in[i + 1]);
            valid = c <= 0xF4 && !(c == 0xF0 && c1 < 0x90) && !(c == 0xF4 && c1 >= 0x90);
        }
        const std::string piece = valid ? in.substr(i, n) : std::string("\xEF\xBF\xBD");  // U+FFFD
        if (out.size() + piece.size() > maxBytes) break;  // cut on a character boundary
        out += piece;
        i += valid ? n : 1;
    }
    return out;
}

}  // namespace

namespace {

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
        e->clientIp = str(v, "clientIp");
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
    entry.operatorName = auditText(entry.operatorName, 200);
    entry.ticketId = auditText(entry.ticketId, 200);
    entry.action = auditText(entry.action, 100);
    entry.target = auditText(entry.target, 2000);
    entry.result = auditText(entry.result, 300);
    entry.transactionId = auditText(entry.transactionId, 100);
    entry.clientIp = auditText(entry.clientIp, 64);
    // Chain onto the newest valid entry. A torn last line (power loss mid-append) is skipped, never a reason to
    // start the chain over: restarting at seq 1 would hide every later entry from readers that track sinceSeq.
    AuditEntry last;
    bool found = false;
    bool skipped = false;
    for (const auto& line : tailLines(file_)) {
        if (parseEntry(line, &last)) {
            found = true;
            break;
        }
        skipped = true;
    }
    if (found) {
        entry.seq = last.seq + 1;
        entry.prevHash = last.hash;
    } else {
        entry.seq = 1;
        entry.prevHash = std::string(64, '0');
    }
    if (skipped) std::cerr << "maintenance v2 audit: skipped an unreadable line at the end of " << file_ << std::endl;
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

DebugMode DebugModeStore::readLocked() const {
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

void DebugModeStore::writeLocked(const DebugMode& m) const {
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

DebugMode DebugModeStore::get() {
    std::lock_guard<std::mutex> lock(mutex_);
    return readLocked();
}

void DebugModeStore::set(const DebugMode& m) {
    std::lock_guard<std::mutex> lock(mutex_);
    writeLocked(m);
}

bool DebugModeStore::exit(DebugMode* ended) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto m = readLocked();
    if (!m.active) return false;
    if (ended) *ended = m;
    writeLocked(DebugMode());
    return true;
}

bool DebugModeStore::touch(std::int64_t now, std::int64_t idleMs, DebugMode* ended, bool activity) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto m = readLocked();
    if (!m.active) return false;
    const auto last = std::max(m.lastActivityAt, m.since);
    if (now - last > idleMs) {
        if (ended) *ended = m;
        writeLocked(DebugMode());
        return true;
    }
    if (activity && now - m.lastActivityAt > 60000) {
        m.lastActivityAt = now;
        writeLocked(m);
    }
    return false;
}

}  // namespace maintenance_v2
}  // namespace edge_gateway
