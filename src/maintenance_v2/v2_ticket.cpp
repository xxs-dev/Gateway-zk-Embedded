#include "v2_ticket.hpp"

#include "v2_crypto.hpp"
#include "v2_files.hpp"
#include "v2_json.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace edge_gateway {
namespace maintenance_v2 {

namespace {

const char* kSignPrefix = "GZ-FIELD-TICKET-V1\n";

// Firmware-embedded platform keys. Empty until the platform's production key is
// issued (C5); test devices get a lab key through trusted-ticket-keys.json.
const TrustedKey* builtInKeys(std::size_t* count) {
    *count = 0;
    return nullptr;
}

TicketVerdict reject(const std::string& code, const std::string& message) {
    TicketVerdict v;
    v.code = code;
    v.message = message;
    return v;
}

bool parsePayload(const JsonValue& root, Ticket* t, std::string* why) {
    if (!root.isObject()) {
        *why = "payload is not an object";
        return false;
    }
    std::int64_t version = 0;
    if (!integer(root, "ticketVersion", &version) || version != 1) {
        *why = "ticketVersion must be 1";
        return false;
    }
    t->ticketId = str(root, "ticketId");
    t->issuer = str(root, "issuer");
    t->keyId = str(root, "keyId");
    t->machineCode = str(root, "machineCode");
    t->reason = str(root, "reason");
    if (t->ticketId.empty() || t->keyId.empty() || t->machineCode.empty()) {
        *why = "ticketId, keyId and machineCode are required";
        return false;
    }
    const auto* actions = root.find("actions");
    if (actions == nullptr || !actions->isArray()) {
        *why = "actions must be an array";
        return false;
    }
    for (const auto& a : actions->asArray().values) {
        if (!a->isString()) {
            *why = "actions must be strings";
            return false;
        }
        t->actions.push_back(a->asString());
    }
    std::int64_t level = 0;
    if (!integer(root, "level", &level) || level < 1 || level > 3) {
        *why = "level must be 1..3";
        return false;
    }
    t->level = static_cast<int>(level);
    if (!integer(root, "issuedAt", &t->issuedAt) || !integer(root, "notBefore", &t->notBefore) ||
        !integer(root, "expiresAt", &t->expiresAt) || t->expiresAt < t->notBefore) {
        *why = "issuedAt / notBefore / expiresAt are required";
        return false;
    }
    const auto* oneTime = root.find("oneTime");
    if (oneTime != nullptr) {
        if (!oneTime->isBool()) {
            *why = "oneTime must be bool";
            return false;
        }
        t->oneTime = oneTime->asBool();
    }
    const auto* op = root.find("operator");
    if (op != nullptr) {
        if (!op->isObject()) {
            *why = "operator must be an object";
            return false;
        }
        t->operatorId = str(*op, "userId");
        t->operatorName = str(*op, "name");
    }
    const auto* constraints = root.find("constraints");
    if (constraints != nullptr) {
        if (!constraints->isObject()) {
            *why = "constraints must be an object";
            return false;
        }
        const auto* paths = constraints->find("configPaths");
        if (paths != nullptr) {
            if (!paths->isArray()) {
                *why = "configPaths must be an array";
                return false;
            }
            for (const auto& p : paths->asArray().values) {
                if (!p->isString()) {
                    *why = "configPaths must be strings";
                    return false;
                }
                t->configPaths.push_back(p->asString());
            }
        }
        const auto* points = constraints->find("pointIndexes");
        if (points != nullptr) {
            if (!points->isArray()) {
                *why = "pointIndexes must be an array";
                return false;
            }
            for (const auto& p : points->asArray().values) {
                const double d = p->isNumber() ? p->asNumber() : -1;
                if (!(d >= 0 && d <= 4294967295.0) || std::floor(d) != d) {
                    *why = "pointIndexes must be unsigned integers";
                    return false;
                }
                t->pointIndexes.push_back(static_cast<std::uint32_t>(d));
            }
        }
    }
    return true;
}

}  // namespace

bool isSafetyParameter(std::uint32_t index) {
    static const std::uint32_t kSafety[] = {161, 162, 151, 535, 504, 453, 454, 457, 458};
    return std::find(std::begin(kSafety), std::end(kSafety), index) != std::end(kSafety);
}

int requiredLevel(const std::string& action, const std::vector<std::uint32_t>& points) {
    if (action == "control.batch") {
        for (const auto p : points) {
            if (isSafetyParameter(p)) return 3;
        }
        return 2;
    }
    if (action == "debug.enter") return 2;
    return 3;  // config.apply / config.restore / config.delete / ota.job
}

TicketVerdict verifyTicket(const std::string& wire, const std::vector<TrustedKey>& keys, const TicketRequirement& req) {
    if (wire.empty()) return reject("ticket_required", "no X-GZ-Field-Ticket header");
    const auto dot = wire.find('.');
    if (dot == std::string::npos || wire.find('.', dot + 1) != std::string::npos || wire.size() > 16384) {
        return reject("malformed", "ticket is not payload.signature");
    }
    std::string payload;
    std::string signature;
    if (!base64UrlDecode(wire.substr(0, dot), &payload) || !base64UrlDecode(wire.substr(dot + 1), &signature) || payload.empty()) {
        return reject("malformed", "ticket is not base64url");
    }
    Ticket ticket;
    try {
        const auto root = parseJson(payload, 4096);
        if (canonicalJson(root) != payload) return reject("malformed", "ticket payload is not RFC 8785 canonical JSON");
        std::string why;
        if (!parsePayload(root, &ticket, &why)) return reject("malformed", why);
    } catch (const std::exception& ex) {
        return reject("malformed", ex.what());
    }
    const TrustedKey* key = nullptr;
    for (const auto& k : keys) {
        if (k.keyId == ticket.keyId) {
            key = &k;
            break;
        }
    }
    if (key == nullptr) return reject("unknown_key", "key " + ticket.keyId + " is not trusted on this device");
    if (signature.size() != 64 || !Crypto::instance().verifyEd25519(key->publicKey, kSignPrefix + payload, signature)) {
        return reject("bad_signature", "signature does not verify with " + ticket.keyId);
    }
    if (ticket.machineCode != req.machineCode) return reject("machine_mismatch", "ticket is for " + ticket.machineCode);
    if (req.nowMs < ticket.notBefore - kTicketClockSkewMs) return reject("not_yet_valid", "ticket is not valid yet");
    if (req.nowMs > ticket.expiresAt + kTicketClockSkewMs) return reject("expired", "ticket expired");
    if (std::find(ticket.actions.begin(), ticket.actions.end(), req.action) == ticket.actions.end()) {
        return reject("action_not_allowed", "ticket does not allow " + req.action);
    }
    if (ticket.level < req.level) return reject("level_too_low", "action needs L" + std::to_string(req.level));
    if (!ticket.configPaths.empty()) {
        for (const auto& path : req.paths) {
            if (std::find(ticket.configPaths.begin(), ticket.configPaths.end(), path) == ticket.configPaths.end()) {
                return reject("path_not_allowed", path + " is outside the ticket");
            }
        }
    }
    if (!ticket.pointIndexes.empty()) {
        for (const auto point : req.points) {
            if (std::find(ticket.pointIndexes.begin(), ticket.pointIndexes.end(), point) == ticket.pointIndexes.end()) {
                return reject("point_not_allowed", "point " + std::to_string(point) + " is outside the ticket");
            }
        }
    }
    TicketVerdict ok;
    ok.ok = true;
    ok.ticket = ticket;
    return ok;
}

std::vector<TrustedKey> loadTrustedKeys(const std::string& file, std::string* warning) {
    std::vector<TrustedKey> keys;
    std::size_t count = 0;
    const TrustedKey* builtIn = builtInKeys(&count);
    for (std::size_t i = 0; i < count; ++i) keys.push_back(builtIn[i]);
    std::string text;
    if (file.empty() || !readFile(file, &text)) return keys;
    try {
        const auto root = parseJson(text, 20000);
        const auto* list = root.find("keys");
        if (list == nullptr || !list->isArray()) throw std::runtime_error("keys array is required");
        for (const auto& item : list->asArray().values) {
            TrustedKey key;
            key.keyId = str(*item, "keyId");
            const auto algorithm = str(*item, "algorithm", "Ed25519");
            std::string raw;
            if (key.keyId.empty() || algorithm != "Ed25519" || !base64Decode(str(*item, "publicKey"), &raw) || raw.size() != 32) {
                if (warning) *warning += "skipped invalid key entry in " + file + "; ";
                continue;
            }
            key.publicKey = raw;
            key.source = file;
            keys.push_back(key);
        }
    } catch (const std::exception& ex) {
        if (warning) *warning += file + ": " + ex.what() + "; ";
    }
    return keys;
}

namespace {

std::map<std::string, std::int64_t> loadReplay(const std::string& file) {
    std::map<std::string, std::int64_t> used;
    std::string text;
    if (!readFile(file, &text)) return used;
    try {
        const auto root = parseJson(text);
        const auto* list = root.find("used");
        if (list == nullptr || !list->isArray()) return used;
        for (const auto& item : list->asArray().values) {
            std::int64_t until = 0;
            const auto id = str(*item, "ticketId");
            if (!id.empty() && integer(*item, "until", &until)) used[id] = until;
        }
    } catch (...) {
    }
    return used;
}

}  // namespace

bool ReplayStore::used(const std::string& ticketId) {
    std::lock_guard<std::mutex> lock(mutex_);
    FileLock fileLock(file_ + ".lock", true);
    const auto list = loadReplay(file_);
    return list.find(ticketId) != list.end();
}

bool ReplayStore::markUsed(const Ticket& ticket, std::int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    FileLock fileLock(file_ + ".lock", true);
    auto list = loadReplay(file_);
    if (list.find(ticket.ticketId) != list.end()) return false;
    for (auto it = list.begin(); it != list.end();) {
        it = it->second < now ? list.erase(it) : std::next(it);
    }
    list[ticket.ticketId] = ticket.expiresAt + kTicketClockSkewMs;
    Writer w;
    w.beginObject().key("used").beginArray();
    for (const auto& entry : list) {
        w.beginObject().field("ticketId", entry.first).fieldInt("until", entry.second).endObject();
    }
    w.endArray().endObject();
    writeFileAtomic(file_, w.text());
    return true;
}

}  // namespace maintenance_v2
}  // namespace edge_gateway
