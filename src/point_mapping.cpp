// Point mapping file: parsing, consistency with the point tables, graph write targets, arithmetic.
#include "edge_gateway/point_mapping.hpp"

#include "edge_gateway/json_value.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <stdexcept>

namespace edge_gateway {

namespace {

using json::JsonValue;

std::string where(std::size_t i, const std::string& name) {
    return "entries[" + std::to_string(i) + "]" + (name.empty() ? std::string() : " (" + name + ")");
}

std::string str(const JsonValue& o, const char* key) {
    const auto* v = o.find(key);
    return v && v->isString() ? v->asString() : std::string();
}

bool number(const JsonValue& o, const char* key, double* out) {
    const auto* v = o.find(key);
    if (!v || v->isNull()) return false;
    if (!v->isNumber() || !std::isfinite(v->asNumber())) throw std::invalid_argument(std::string(key) + " must be a finite number");
    *out = v->asNumber();
    return true;
}

std::uint32_t indexOf(const JsonValue& o, const char* key) {
    double d = 0;
    if (!number(o, key, &d) || d < 1 || d > 4294967295.0 || d != std::floor(d)) throw std::invalid_argument(std::string(key) + " must be a positive integer");
    return static_cast<std::uint32_t>(d);
}

PointMappingRef ref(const JsonValue& o) {
    if (!o.isObject()) throw std::invalid_argument("source must be an object");
    PointMappingRef r;
    r.meterCode = str(o, "meterCode");
    r.pointCode = str(o, "pointCode");
    r.index = indexOf(o, "index");
    if (r.meterCode.empty() || r.pointCode.empty()) throw std::invalid_argument("meterCode and pointCode are required");
    return r;
}

bool parseNumberText(const std::string& text, double* out) {
    if (text.empty()) return false;
    char* end = nullptr;
    const double d = std::strtod(text.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(d)) return false;
    *out = d;
    return true;
}

PointMappingEntry parseEntry(const JsonValue& e) {
    if (!e.isObject()) throw std::invalid_argument("entry must be an object");
    PointMappingEntry m;
    m.virtualName = str(e, "virtual");
    if (m.virtualName.empty()) throw std::invalid_argument("virtual is required");
    m.virtualIndex = indexOf(e, "virtualIndex");
    const auto direction = str(e, "direction");
    if (direction != "in" && direction != "out") throw std::invalid_argument("direction must be in or out");
    m.output = direction == "out";
    const auto* sources = e.find("sources");
    if (!sources || !sources->isArray() || sources->asArray().values.empty()) throw std::invalid_argument("sources must be a non-empty array");
    for (const auto& s : sources->asArray().values) m.sources.push_back(ref(*s));
    m.aggregate = str(e, "aggregate");
    if (m.sources.size() > 1) {
        if (m.output) throw std::invalid_argument("an output writes exactly one device point");
        if (m.aggregate != "sum" && m.aggregate != "min" && m.aggregate != "max" && m.aggregate != "avg")
            throw std::invalid_argument("several sources need aggregate sum, min, max or avg");
    } else if (!m.aggregate.empty() && m.aggregate != "sum" && m.aggregate != "min" && m.aggregate != "max" && m.aggregate != "avg") {
        throw std::invalid_argument("aggregate must be sum, min, max or avg");
    }
    number(e, "scale", &m.scale);
    number(e, "offset", &m.offset);
    if (m.scale == 0.0) throw std::invalid_argument("scale must not be 0");
    if (const auto* inv = e.find("invert")) {
        if (!inv->isBool()) throw std::invalid_argument("invert must be true or false");
        m.invert = inv->asBool();
    }
    if (const auto* en = e.find("enum")) {
        if (!en->isObject()) throw std::invalid_argument("enum must be an object");
        std::set<double> targets;
        for (const auto& member : en->asObject().values) {
            double from = 0;
            double to = 0;
            if (!parseNumberText(member.key, &from)) throw std::invalid_argument("enum keys must be device values (numbers)");
            if (member.value->isNumber() && std::isfinite(member.value->asNumber())) {
                to = member.value->asNumber();
            } else if (!(member.value->isString() && parseNumberText(member.value->asString(), &to))) {
                throw std::invalid_argument("enum values must be numeric EMS codes (named EMS enums are not defined yet, CQ17)");
            }
            if (m.output && !targets.insert(to).second) throw std::invalid_argument("an output enum must map each EMS code once");
            m.enumMap[from] = to;
        }
    }
    if (const auto* validity = e.find("validity")) {
        if (!validity->isObject()) throw std::invalid_argument("validity must be an object");
        double t = 0;
        if (number(*validity, "timeoutMs", &t)) {
            if (t < 0) throw std::invalid_argument("validity.timeoutMs must be >= 0");
            m.validityTimeoutMs = static_cast<std::int64_t>(t);
        }
    }
    const auto* write = e.find("write");
    if (m.output) {
        if (!write || !write->isObject()) throw std::invalid_argument("an output needs write");
        m.hasMin = number(*write, "min", &m.writeMin);
        m.hasMax = number(*write, "max", &m.writeMax);
        if (m.hasMin && m.hasMax && m.writeMin > m.writeMax) throw std::invalid_argument("write.min must be <= write.max");
        number(*write, "deadband", &m.deadband);
        if (m.deadband < 0) throw std::invalid_argument("write.deadband must be >= 0");
        double refresh = 0;
        if (number(*write, "refreshMs", &refresh)) {
            if (refresh < 0) throw std::invalid_argument("write.refreshMs must be >= 0");
            m.refreshMs = static_cast<std::int64_t>(refresh);
        }
        const auto* rb = write->find("readback");
        if (!rb || !rb->isObject()) throw std::invalid_argument("an output needs write.readback");
        m.readback = ref(*rb);
        number(*rb, "tolerance", &m.readbackTolerance);
        if (m.readbackTolerance < 0) throw std::invalid_argument("write.readback.tolerance must be >= 0");
        double timeout = 0;
        if (number(*rb, "timeoutMs", &timeout)) {
            if (timeout <= 0) throw std::invalid_argument("write.readback.timeoutMs must be > 0");
            m.readbackTimeoutMs = static_cast<std::int64_t>(timeout);
        }
    } else if (write) {
        throw std::invalid_argument("write is only for outputs");
    }
    return m;
}

}  // namespace

PointMapping parsePointMapping(const std::string& text) {
    JsonValue root;
    try {
        root = json::JsonParser(text, 16, 200000).parse();
    } catch (const std::exception& ex) {
        throw std::invalid_argument(std::string("point mapping is not valid JSON: ") + ex.what());
    }
    if (!root.isObject()) throw std::invalid_argument("point mapping must be a JSON object");
    if (str(root, "schemaVersion") != "1.0" || str(root, "kind") != "point-mapping")
        throw std::invalid_argument("point mapping needs schemaVersion \"1.0\" and kind \"point-mapping\"");
    PointMapping mapping;
    mapping.machineCode = str(root, "machineCode");
    const auto* entries = root.find("entries");
    if (!entries || !entries->isArray()) throw std::invalid_argument("entries must be an array");
    std::size_t i = 0;
    for (const auto& e : entries->asArray().values) {
        try {
            mapping.entries.push_back(parseEntry(*e));
        } catch (const std::exception& ex) {
            throw std::invalid_argument(where(i, e->isObject() ? str(*e, "virtual") : std::string()) + ": " + ex.what());
        }
        ++i;
    }
    return mapping;
}

PointMappingTargets pointMappingTargets(const std::vector<DeviceConfig>& devices) {
    PointMappingTargets out;
    auto add = [&out](const DeviceConfig& d, const std::string& meterCode, const PointDefinition& p) {
        PointMappingTarget t;
        t.meterCode = meterCode;
        t.pointCode = p.pointCode;
        t.protocol = d.protocol.type;
        t.writable = p.write.enable;
        out[p.index] = t;
    };
    for (const auto& d : devices) {
        for (const auto& m : d.meters) {
            for (const auto& p : m.points) add(d, m.meterCode, p);
        }
        for (const auto& p : d.points) add(d, d.meterCode, p);
    }
    return out;
}

std::vector<PointMappingProblem> checkPointMapping(const PointMapping& mapping, const PointMappingTargets& targets,
                                                   const std::string& machineCode) {
    std::vector<PointMappingProblem> problems;
    auto problem = [&problems](const char* rule, const std::string& message) { problems.push_back(PointMappingProblem{rule, message}); };
    if (!machineCode.empty() && mapping.machineCode != machineCode)
        problem("mapping.format", "machineCode " + mapping.machineCode + " is not this device (" + machineCode + ")");
    auto checkRef = [&](const std::string& entry, const char* role, const PointMappingRef& r) -> const PointMappingTarget* {
        const auto it = targets.find(r.index);
        if (it == targets.end()) {
            problem("mapping.points", entry + ": " + role + " index " + std::to_string(r.index) + " is not in the device point tables");
            return nullptr;
        }
        if (it->second.meterCode != r.meterCode || it->second.pointCode != r.pointCode) {
            problem("mapping.points", entry + ": " + role + " index " + std::to_string(r.index) + " is " + it->second.meterCode + "/" + it->second.pointCode +
                                          ", not " + r.meterCode + "/" + r.pointCode);
            return nullptr;
        }
        if (it->second.protocol == "ems_virtual") {
            problem("mapping.points", entry + ": " + role + " index " + std::to_string(r.index) + " is an EMS point, not a device point");
            return nullptr;
        }
        return &it->second;
    };
    std::set<std::uint32_t> virtuals;
    std::set<std::uint32_t> outputTargets;
    for (std::size_t i = 0; i < mapping.entries.size(); ++i) {
        const auto& e = mapping.entries[i];
        const auto name = where(i, e.virtualName);
        if (e.virtualIndex < kPointMappingVirtualIndexMin || e.virtualIndex > kPointMappingVirtualIndexMax) {
            problem("mapping.points", name + ": virtualIndex " + std::to_string(e.virtualIndex) + " is outside 680000-689999");
        }
        const auto v = targets.find(e.virtualIndex);
        if (v == targets.end() || v->second.protocol != "ems_virtual") {
            problem("mapping.points", name + ": virtualIndex " + std::to_string(e.virtualIndex) + " is not an EMS_CORE point (declare it in device_ems_virtual.json)");
        } else if (v->second.pointCode != e.virtualName) {
            problem("mapping.points", name + ": EMS point " + std::to_string(e.virtualIndex) + " has pointCode " + v->second.pointCode + ", not " + e.virtualName);
        } else if (e.output && v->second.writable) {
            // Output commands come from the strategy graph scan by scan; a writable EMS point would be a
            // persisted parameter that anyone could set and that would survive restarts.
            problem("mapping.write", name + ": EMS output point " + std::to_string(e.virtualIndex) +
                                         " is writable; only the strategy graph writes output points (set write.enable to false)");
        }
        if (!virtuals.insert(e.virtualIndex).second) problem("mapping.points", name + ": virtual point " + std::to_string(e.virtualIndex) + " is mapped twice");
        for (const auto& s : e.sources) {
            const auto* t = checkRef(name, "source", s);
            if (t && e.output) {
                if (!t->writable) problem("mapping.write", name + ": device point " + std::to_string(s.index) + " is not writable (write.enable=false)");
                if (!outputTargets.insert(s.index).second) problem("mapping.write", name + ": device point " + std::to_string(s.index) + " is written by two entries");
            }
        }
        if (e.output) checkRef(name, "readback", e.readback);
    }
    return problems;
}

std::vector<GraphDeviceWrite> graphWriteTargets(const std::string& graphJson) {
    std::vector<GraphDeviceWrite> out;
    const auto root = json::JsonParser(graphJson, 64, 2000000).parse();
    const auto* nodes = root.find("nodes");
    if (!nodes || !nodes->isArray()) return out;
    for (const auto& n : nodes->asArray().values) {
        if (!n->isObject()) continue;
        const auto* enabled = n->find("enabled");
        if (enabled && enabled->isBool() && !enabled->asBool()) continue;
        const auto type = str(*n, "type");
        const auto id = str(*n, "id");
        const JsonValue* params = n->find("parameters");
        if (!params) params = n->find("params");
        auto param = [&](const char* key, double fallback) {
            double d = fallback;
            const auto* v = params ? params->find(key) : nullptr;
            if (v && v->isNumber()) d = v->asNumber();
            return d;
        };
        if (type == "controlWrite") {
            const auto* submit = params ? params->find("submitWrites") : nullptr;
            if (!submit || !submit->isBool() || !submit->asBool()) continue;
            const auto target = param("targetIndex", 0);
            if (target >= 1) out.push_back(GraphDeviceWrite{id, type, static_cast<std::uint32_t>(target)});
        } else if (type == "pcsWriteback") {
            // Defaults as in GraphEmsEngine::submitPcsWritebackCommands.
            const std::pair<const char*, double> keys[] = {{"pControlAIndex", 1318}, {"pControlBIndex", 1319}, {"pControlCIndex", 1320},
                                                           {"qControlAIndex", 1321}, {"qControlBIndex", 1322}, {"qControlCIndex", 1323}};
            for (const auto& k : keys) {
                const auto target = param(k.first, k.second);
                if (target >= 1) out.push_back(GraphDeviceWrite{id, type, static_cast<std::uint32_t>(target)});
            }
        }
    }
    return out;
}

bool pointMappingToEms(const PointMappingEntry& entry, double deviceValue, double* emsValue) {
    double v = deviceValue;
    if (!entry.enumMap.empty()) {
        const auto it = entry.enumMap.find(v);
        if (it == entry.enumMap.end()) return false;
        *emsValue = it->second;
        return true;
    }
    v = v * entry.scale + entry.offset;
    if (entry.invert) v = -v;
    *emsValue = v;
    return std::isfinite(v);
}

bool pointMappingToDevice(const PointMappingEntry& entry, double emsValue, double* deviceValue) {
    double v = emsValue;
    if (!entry.enumMap.empty()) {
        bool found = false;
        for (const auto& kv : entry.enumMap) {
            if (kv.second == emsValue) {
                v = kv.first;
                found = true;
                break;
            }
        }
        if (!found) return false;
    } else {
        if (entry.invert) v = -v;
        v = (v - entry.offset) / entry.scale;
    }
    if (entry.hasMin) v = std::max(v, entry.writeMin);
    if (entry.hasMax) v = std::min(v, entry.writeMax);
    *deviceValue = v;
    return std::isfinite(v);
}

double pointMappingAggregate(const std::string& aggregate, const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    if (aggregate == "min") return *std::min_element(values.begin(), values.end());
    if (aggregate == "max") return *std::max_element(values.begin(), values.end());
    double sum = 0.0;
    for (const auto v : values) sum += v;
    if (aggregate == "avg") return sum / static_cast<double>(values.size());
    return values.size() == 1 && aggregate.empty() ? values[0] : sum;
}

}  // namespace edge_gateway
