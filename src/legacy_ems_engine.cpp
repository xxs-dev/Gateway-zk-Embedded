#include "edge_gateway/legacy_ems_engine.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace edge_gateway {

namespace {

constexpr std::uint32_t kAverageWindowSizeIndex = 156;
constexpr std::uint32_t kTqAvgPowerUnbalance = 225;
constexpr std::uint32_t kStackChargeKwAllow = 1552;
constexpr std::uint32_t kStackDischargeKwAllow = 1553;
constexpr std::uint32_t kStackChargeCurrentAllow = 1556;
constexpr std::uint32_t kStackDischargeCurrentAllow = 1557;
constexpr std::uint32_t kStackRealVoltage = 1566;
constexpr std::uint32_t kStackChargeKwhSum = 1586;
constexpr std::uint32_t kStackDischargeKwhSum = 1587;
constexpr std::uint32_t kStackChargeKwhToday = 1615;
constexpr std::uint32_t kStackDischargeKwhToday = 1616;
constexpr std::uint32_t kStackChargeKwhZeroSave = 398;
constexpr std::uint32_t kStackDischargeKwhZeroSave = 399;
constexpr std::uint32_t kCosRunFlag = 8;
constexpr std::uint32_t kCosTarget = 514;
constexpr std::uint32_t kCosTargetQa = 505;
constexpr std::uint32_t kCosTargetQb = 506;
constexpr std::uint32_t kCosTargetQc = 507;
constexpr std::uint32_t kCosTargetQ3 = 508;
constexpr std::uint32_t kPcsPControlA = 1318;
constexpr std::uint32_t kPcsPControlB = 1319;
constexpr std::uint32_t kPcsPControlC = 1320;
constexpr std::uint32_t kPcsQControlA = 1321;
constexpr std::uint32_t kPcsQControlB = 1322;
constexpr std::uint32_t kPcsQControlC = 1323;
constexpr std::uint32_t kPcsComStatus = 1399;

const std::vector<std::pair<std::uint32_t, std::uint32_t>>& tqAverageMappings() {
    static const std::vector<std::pair<std::uint32_t, std::uint32_t>> mappings = {
        {1030, 201},
        {1031, 202},
        {1032, 203},
        {1036, 209},
        {1037, 210},
        {1038, 211},
        {1039, 212},
        {1040, 213},
        {1041, 214},
        {1042, 215},
        {1043, 216}
    };
    return mappings;
}

const std::vector<std::pair<std::uint32_t, std::uint32_t>>& cnAverageMappings() {
    static const std::vector<std::pair<std::uint32_t, std::uint32_t>> mappings = {
        {1130, 251},
        {1131, 252},
        {1132, 253},
        {1136, 259},
        {1137, 260},
        {1138, 261},
        {1139, 262},
        {1140, 263},
        {1141, 264},
        {1142, 265},
        {1143, 266}
    };
    return mappings;
}

const std::vector<std::pair<std::uint32_t, std::uint32_t>>& bwAverageMappings() {
    static const std::vector<std::pair<std::uint32_t, std::uint32_t>> mappings = {
        {4537, 401},
        {4538, 402},
        {4539, 403},
        {4536, 404},
        {4541, 405},
        {4542, 406},
        {4543, 407},
        {4540, 408}
    };
    return mappings;
}

std::string cmdId(std::uint32_t index, std::int64_t nowMs) {
    return "LEGACY_EMS_" + std::to_string(index) + "_" + std::to_string(nowMs);
}

double average(const std::deque<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

bool isFiniteNonZero(double value) {
    return std::isfinite(value) && value != 0.0;
}

double apparentPower(double p, double q) {
    return std::sqrt(p * p + q * q);
}

double powerFactor(double p, double s) {
    if (!isFiniteNonZero(s)) {
        return 0.0;
    }
    return std::abs(p) / s;
}

int localHourFromEpochMs(std::int64_t nowMs) {
    std::time_t seconds = static_cast<std::time_t>(nowMs / 1000LL);
    std::tm localTime{};
#if defined(_WIN32)
    localtime_s(&localTime, &seconds);
#else
    localtime_r(&seconds, &localTime);
#endif
    if (localTime.tm_hour < 0 || localTime.tm_hour > 23) {
        return 0;
    }
    return localTime.tm_hour;
}

bool valueMatches(double actual, double target, double tolerance = 0.5) {
    return std::abs(actual - target) <= tolerance;
}

}  // namespace

LegacyEmsEngine::LegacyEmsEngine(
    const LegacyEmsPointCatalog& catalog,
    PointStoreRouter& router,
    std::int64_t defaultTtlMs,
    std::unordered_map<std::string, std::string> profile
) : catalog_(catalog),
    router_(router),
    defaultTtlMs_(defaultTtlMs),
    profile_(std::move(profile)) {
}

double LegacyEmsEngine::get(std::uint32_t index, std::int64_t nowMs) const {
    const auto value = latestValue(index, nowMs);
    if (!value) {
        return 0.0;
    }
    return *value;
}

CommandSubmitResult LegacyEmsEngine::set(std::uint32_t index, double value, std::int64_t nowMs) {
    return putLatest(index, value, nowMs);
}

CommandSubmitResult LegacyEmsEngine::cmd(std::uint32_t index, double value, std::int64_t nowMs) {
    return submitCommand(index, value, nowMs);
}

LegacyEmsRunResult LegacyEmsEngine::runOnce(std::int64_t nowMs) {
    LegacyEmsRunResult result;

    const auto avgNumRaw = get(kAverageWindowSizeIndex, nowMs);
    const auto avgNum = static_cast<std::size_t>(std::max(1.0, avgNumRaw > 0.0 ? avgNumRaw : 10.0));
    const bool meterTq = profileEnabled("Meter_TQ", true);
    const bool meterCn = profileEnabled("Meter_CN", true);
    const bool meterBw = profileEnabled("Meter_BW", false);
    const bool meterFh = profileEnabled("Meter_FH", false);
    const int bmsModel = profileInt("BMS_MODEL", 2);

    if (meterTq) {
        updateAverageSet(tqAverageMappings(), avgNum, nowMs, result);
        updateTqPowerUnbalance(nowMs, result);
    }
    if (meterCn) {
        updateAverageSet(cnAverageMappings(), avgNum, nowMs, result);
    }
    if (meterBw) {
        updateAverageSet(bwAverageMappings(), avgNum, nowMs, result);
    }
    if (!meterFh && meterBw && meterTq) {
        updateDerivedLoadFromTqAndBw(nowMs, result);
    } else if (!meterFh && !meterBw && meterTq && meterCn) {
        updateDerivedLoadFromTqAndCn(nowMs, result);
    }
    if (bmsModel == 1 || bmsModel == 3) {
        updateBmsTodayEnergy(nowMs, result);
    }
    if (meterTq) {
        updateCosTargets(nowMs, result);
    }
    if (meterCn) {
        updateLvHvTargets(nowMs, result);
    }
    if (meterTq) {
        updateCdFdTargets(nowMs, result);
    }
    if (meterTq) {
        updateSkTargets(nowMs, result);
    }
    if (meterTq && meterCn) {
        updateDsTargets(nowMs, result);
        updatePhTargets(nowMs, result);
        updateGfTargets(nowMs, result);
    }
    updatePowerSolveOutputs(nowMs, result);

    const auto voltage = latestValue(kStackRealVoltage, nowMs);
    const auto chargeCurrent = latestValue(kStackChargeCurrentAllow, nowMs);
    const auto dischargeCurrent = latestValue(kStackDischargeCurrentAllow, nowMs);

    if (voltage && chargeCurrent && catalog_.findByIndex(kStackChargeKwAllow)) {
        const auto routed = set(kStackChargeKwAllow, *voltage * *chargeCurrent * 0.001, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
        }
    }
    if (voltage && dischargeCurrent && catalog_.findByIndex(kStackDischargeKwAllow)) {
        const auto routed = set(kStackDischargeKwAllow, *voltage * *dischargeCurrent * 0.001, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
        }
    }

    return result;
}

Optional<double> LegacyEmsEngine::latestValue(std::uint32_t index, std::int64_t nowMs) const {
    const auto latest = router_.getLatestByIndex(index, nowMs);
    if (!latest || latest->quality != 1 || latest->stale) {
        return NullOpt;
    }
    return latest->value;
}

bool LegacyEmsEngine::profileEnabled(const std::string& key, bool defaultValue) const {
    const auto it = profile_.find(key);
    if (it == profile_.end()) {
        return defaultValue;
    }
    return it->second == "1" || it->second == "true" || it->second == "TRUE";
}

int LegacyEmsEngine::profileInt(const std::string& key, int defaultValue) const {
    const auto it = profile_.find(key);
    if (it == profile_.end()) {
        return defaultValue;
    }
    try {
        return std::stoi(it->second);
    } catch (...) {
        return defaultValue;
    }
}

bool LegacyEmsEngine::hasPoint(std::uint32_t index) const {
    return static_cast<bool>(catalog_.findByIndex(index)) && static_cast<bool>(router_.routeByIndex(index));
}

bool LegacyEmsEngine::hasRoute(std::uint32_t index) const {
    return static_cast<bool>(router_.routeByIndex(index));
}

bool LegacyEmsEngine::updateAverage(
    std::uint32_t inputIndex,
    std::uint32_t outputIndex,
    std::size_t windowSize,
    std::int64_t nowMs,
    LegacyEmsRunResult& result
) {
    if (!hasPoint(outputIndex)) {
        return false;
    }
    const auto input = latestValue(inputIndex, nowMs);
    if (!input) {
        return false;
    }

    auto& values = averageWindows_[outputIndex];
    while (values.size() >= windowSize) {
        values.pop_front();
    }
    values.push_back(*input);

    const auto routed = set(outputIndex, average(values), nowMs);
    if (routed.accepted) {
        ++result.latestWrites;
    }
    return routed.accepted;
}

bool LegacyEmsEngine::updateAverageSet(
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& mappings,
    std::size_t windowSize,
    std::int64_t nowMs,
    LegacyEmsRunResult& result
) {
    bool updated = false;
    for (const auto& mapping : mappings) {
        updated = updateAverage(mapping.first, mapping.second, windowSize, nowMs, result) || updated;
    }
    return updated;
}

Optional<double> LegacyEmsEngine::updateAverageWindowOnly(
    std::uint32_t inputIndex,
    std::uint32_t windowKey,
    std::size_t windowSize,
    std::int64_t nowMs
) {
    const auto input = latestValue(inputIndex, nowMs);
    if (!input) {
        return NullOpt;
    }
    auto& values = averageWindows_[windowKey];
    while (values.size() >= windowSize) {
        values.pop_front();
    }
    values.push_back(*input);
    return average(values);
}

bool LegacyEmsEngine::updateTqPowerUnbalance(std::int64_t nowMs, LegacyEmsRunResult& result) {
    const auto pa = latestValue(209, nowMs);
    const auto pb = latestValue(210, nowMs);
    const auto pc = latestValue(211, nowMs);
    const auto p3 = latestValue(212, nowMs);

    if (pa && latestValue(213, nowMs) && hasPoint(217)) {
        const auto routed = set(217, apparentPower(*pa, *latestValue(213, nowMs)), nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
        }
    }
    if (pb && latestValue(214, nowMs) && hasPoint(218)) {
        const auto routed = set(218, apparentPower(*pb, *latestValue(214, nowMs)), nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
        }
    }
    if (pc && latestValue(215, nowMs) && hasPoint(219)) {
        const auto routed = set(219, apparentPower(*pc, *latestValue(215, nowMs)), nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
        }
    }
    if (p3 && latestValue(216, nowMs) && hasPoint(220)) {
        const auto routed = set(220, apparentPower(*p3, *latestValue(216, nowMs)), nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
        }
    }

    const auto sa = latestValue(217, nowMs);
    const auto sb = latestValue(218, nowMs);
    const auto sc = latestValue(219, nowMs);
    const auto s3 = latestValue(220, nowMs);
    if (pa && sa && hasPoint(221)) {
        const auto routed = set(221, powerFactor(*pa, *sa), nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
        }
    }
    if (pb && sb && hasPoint(222)) {
        const auto routed = set(222, powerFactor(*pb, *sb), nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
        }
    }
    if (pc && sc && hasPoint(223)) {
        const auto routed = set(223, powerFactor(*pc, *sc), nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
        }
    }
    if (p3 && s3 && hasPoint(224)) {
        const auto routed = set(224, powerFactor(*p3, *s3), nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
        }
    }

    if (!hasPoint(kTqAvgPowerUnbalance) || !pa || !pb || !pc || !p3 || !isFiniteNonZero(*p3)) {
        return false;
    }

    const auto maxPhase = std::max(*pa, std::max(*pb, *pc));
    const auto minPhase = std::min(*pa, std::min(*pb, *pc));
    const auto unbalance = std::abs((maxPhase - minPhase) / *p3 * 300.0);
    const auto routed = set(kTqAvgPowerUnbalance, unbalance, nowMs);
    if (routed.accepted) {
        ++result.latestWrites;
    }
    return routed.accepted;
}

bool LegacyEmsEngine::updateDerivedLoadFromTqAndCn(std::int64_t nowMs, LegacyEmsRunResult& result) {
    const auto tqPa = latestValue(209, nowMs);
    const auto tqPb = latestValue(210, nowMs);
    const auto tqPc = latestValue(211, nowMs);
    const auto tqP3 = latestValue(212, nowMs);
    const auto tqQa = latestValue(213, nowMs);
    const auto tqQb = latestValue(214, nowMs);
    const auto tqQc = latestValue(215, nowMs);
    const auto tqQ3 = latestValue(216, nowMs);
    const auto cnPa = latestValue(259, nowMs);
    const auto cnPb = latestValue(260, nowMs);
    const auto cnPc = latestValue(261, nowMs);
    const auto cnP3 = latestValue(262, nowMs);
    const auto cnQa = latestValue(263, nowMs);
    const auto cnQb = latestValue(264, nowMs);
    const auto cnQc = latestValue(265, nowMs);
    const auto cnQ3 = latestValue(266, nowMs);

    if (!tqPa || !tqPb || !tqPc || !tqP3 || !tqQa || !tqQb || !tqQc || !tqQ3 ||
        !cnPa || !cnPb || !cnPc || !cnP3 || !cnQa || !cnQb || !cnQc || !cnQ3) {
        return false;
    }

    const double fhPa = *tqPa - *cnPa;
    const double fhPb = *tqPb - *cnPb;
    const double fhPc = *tqPc - *cnPc;
    const double fhP3 = *tqP3 - *cnP3;
    const double fhQa = *tqQa - *cnQa;
    const double fhQb = *tqQb - *cnQb;
    const double fhQc = *tqQc - *cnQc;
    const double fhQ3 = *tqQ3 - *cnQ3;
    const double fhSa = apparentPower(fhPa, fhQa);
    const double fhSb = apparentPower(fhPb, fhQb);
    const double fhSc = apparentPower(fhPc, fhQc);
    const double fhS3 = apparentPower(fhP3, fhQ3);
    const double fhCosA = powerFactor(fhPa, fhSa);
    const double fhCosB = powerFactor(fhPb, fhSb);
    const double fhCosC = powerFactor(fhPc, fhSc);
    const double fhCos3 = powerFactor(fhP3, fhS3);

    bool updated = false;
    const std::pair<std::uint32_t, double> outputs[] = {
        {309, fhPa}, {310, fhPb}, {311, fhPc}, {312, fhP3},
        {313, fhQa}, {314, fhQb}, {315, fhQc}, {316, fhQ3},
        {317, fhSa}, {318, fhSb}, {319, fhSc}, {320, fhS3},
        {321, fhCosA}, {322, fhCosB}, {323, fhCosC}, {324, fhCos3}
    };
    for (const auto& output : outputs) {
        if (!hasRoute(output.first)) {
            continue;
        }
        const auto routed = set(output.first, output.second, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }

    if (hasPoint(325) && isFiniteNonZero(fhP3)) {
        const auto maxPhase = std::max(fhPa, std::max(fhPb, fhPc));
        const auto minPhase = std::min(fhPa, std::min(fhPb, fhPc));
        const auto routed = set(325, std::abs((maxPhase - minPhase) / fhP3 * 300.0), nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    return updated;
}

bool LegacyEmsEngine::updateBmsTodayEnergy(std::int64_t nowMs, LegacyEmsRunResult& result) {
    bool updated = false;
    const auto chargeSum = latestValue(kStackChargeKwhSum, nowMs);
    const auto chargeZero = latestValue(kStackChargeKwhZeroSave, nowMs);
    if (chargeSum && chargeZero && hasPoint(kStackChargeKwhToday)) {
        const auto routed = set(kStackChargeKwhToday, *chargeSum - *chargeZero, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }

    const auto dischargeSum = latestValue(kStackDischargeKwhSum, nowMs);
    const auto dischargeZero = latestValue(kStackDischargeKwhZeroSave, nowMs);
    if (dischargeSum && dischargeZero && hasPoint(kStackDischargeKwhToday)) {
        const auto routed = set(kStackDischargeKwhToday, *dischargeSum - *dischargeZero, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    return updated;
}

bool LegacyEmsEngine::updateDerivedLoadFromTqAndBw(std::int64_t nowMs, LegacyEmsRunResult& result) {
    const auto avgNumRaw = get(kAverageWindowSizeIndex, nowMs);
    const auto avgNum = static_cast<std::size_t>(std::max(1.0, avgNumRaw > 0.0 ? avgNumRaw : 10.0));
    const auto tqPa = latestValue(209, nowMs);
    const auto tqPb = latestValue(210, nowMs);
    const auto tqPc = latestValue(211, nowMs);
    const auto tqP3 = latestValue(212, nowMs);
    const auto tqQa = latestValue(213, nowMs);
    const auto tqQb = latestValue(214, nowMs);
    const auto tqQc = latestValue(215, nowMs);
    const auto tqQ3 = latestValue(216, nowMs);
    const auto bwPa = updateAverageWindowOnly(4537, 401, avgNum, nowMs);
    const auto bwPb = updateAverageWindowOnly(4538, 402, avgNum, nowMs);
    const auto bwPc = updateAverageWindowOnly(4539, 403, avgNum, nowMs);
    const auto bwP3 = updateAverageWindowOnly(4536, 404, avgNum, nowMs);
    const auto bwQa = updateAverageWindowOnly(4541, 405, avgNum, nowMs);
    const auto bwQb = updateAverageWindowOnly(4542, 406, avgNum, nowMs);
    const auto bwQc = updateAverageWindowOnly(4543, 407, avgNum, nowMs);
    const auto bwQ3 = updateAverageWindowOnly(4540, 408, avgNum, nowMs);

    if (!tqPa || !tqPb || !tqPc || !tqP3 || !tqQa || !tqQb || !tqQc || !tqQ3 ||
        !bwPa || !bwPb || !bwPc || !bwP3 || !bwQa || !bwQb || !bwQc || !bwQ3) {
        return false;
    }

    const double fhPa = *tqPa - *bwPa;
    const double fhPb = *tqPb - *bwPb;
    const double fhPc = *tqPc - *bwPc;
    const double fhP3 = *tqP3 - *bwP3;
    const double fhQa = *tqQa - *bwQa;
    const double fhQb = *tqQb - *bwQb;
    const double fhQc = *tqQc - *bwQc;
    const double fhQ3 = *tqQ3 - *bwQ3;
    const double fhSa = apparentPower(fhPa, fhQa);
    const double fhSb = apparentPower(fhPb, fhQb);
    const double fhSc = apparentPower(fhPc, fhQc);
    const double fhS3 = apparentPower(fhP3, fhQ3);
    const double fhCosA = powerFactor(fhPa, fhSa);
    const double fhCosB = powerFactor(fhPb, fhSb);
    const double fhCosC = powerFactor(fhPc, fhSc);
    const double fhCos3 = powerFactor(fhP3, fhS3);

    bool updated = false;
    const std::pair<std::uint32_t, double> outputs[] = {
        {309, fhPa}, {310, fhPb}, {311, fhPc}, {312, fhP3},
        {313, fhQa}, {314, fhQb}, {315, fhQc}, {316, fhQ3},
        {317, fhSa}, {318, fhSb}, {319, fhSc}, {320, fhS3},
        {321, fhCosA}, {322, fhCosB}, {323, fhCosC}, {324, fhCos3}
    };
    for (const auto& output : outputs) {
        if (!hasRoute(output.first)) {
            continue;
        }
        const auto routed = set(output.first, output.second, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    if (hasPoint(325) && isFiniteNonZero(fhP3)) {
        const auto maxPhase = std::max(fhPa, std::max(fhPb, fhPc));
        const auto minPhase = std::min(fhPa, std::min(fhPb, fhPc));
        const auto routed = set(325, std::abs((maxPhase - minPhase) / fhP3 * 300.0), nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    return updated;
}

bool LegacyEmsEngine::updateCosTargets(std::int64_t nowMs, LegacyEmsRunResult& result) {
    const auto cosTarget = latestValue(kCosTarget, nowMs);
    const auto pa = latestValue(209, nowMs);
    const auto pb = latestValue(210, nowMs);
    const auto pc = latestValue(211, nowMs);
    const auto qa = latestValue(213, nowMs);
    const auto qb = latestValue(214, nowMs);
    const auto qc = latestValue(215, nowMs);
    if (!cosTarget || !pa || !pb || !pc || !qa || !qb || !qc) {
        return false;
    }
    if (*cosTarget <= -1.0 || *cosTarget >= 1.0) {
        return false;
    }

    const double targetTan = std::tan(std::acos(*cosTarget));
    const double targetQa = std::abs(targetTan * *pa);
    const double targetQb = std::abs(targetTan * *pb);
    const double targetQc = std::abs(targetTan * *pc);
    const double targetQ3 = targetQa + targetQb + targetQc;

    double outQa = 0.0;
    if ((*qa > 0.0 && *qa > targetQa) || (*qa < 0.0 && *qa < -targetQa)) {
        outQa = *qa > 0.0 ? (*qa - targetQa) : (*qa + targetQa);
    }
    double outQb = 0.0;
    if ((*qb > 0.0 && *qb > targetQb) || (*qb < 0.0 && *qb < -targetQb)) {
        outQb = *qb > 0.0 ? (*qb - targetQb) : (*qb + targetQb);
    }
    double outQc = 0.0;
    if ((*qc > 0.0 && *qc > targetQc) || (*qc < 0.0 && *qc < -targetQc)) {
        outQc = *qc > 0.0 ? (*qc - targetQc) : (*qc + targetQc);
    }
    const double outQ3 = std::abs(outQa) + std::abs(outQb) + std::abs(outQc);

    bool updated = false;
    const std::pair<std::uint32_t, double> outputs[] = {
        {kCosTargetQa, targetQa},
        {kCosTargetQb, targetQb},
        {kCosTargetQc, targetQc},
        {kCosTargetQ3, targetQ3},
        {601, outQa},
        {602, outQb},
        {603, outQc},
        {604, outQ3},
        {kCosRunFlag, outQ3 != 0.0 ? 1.0 : 0.0}
    };
    for (const auto& output : outputs) {
        if (!hasRoute(output.first)) {
            continue;
        }
        const auto routed = set(output.first, output.second, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    return updated;
}

bool LegacyEmsEngine::updateLvHvTargets(std::int64_t nowMs, LegacyEmsRunResult& result) {
    const auto cnUa = latestValue(251, nowMs);
    const auto cnUb = latestValue(252, nowMs);
    const auto cnUc = latestValue(253, nowMs);
    const auto lvLow = latestValue(544, nowMs);
    const auto lvUp = latestValue(545, nowMs);
    const auto hvLow = latestValue(546, nowMs);
    const auto hvUp = latestValue(547, nowMs);
    const auto gradP = latestValue(533, nowMs);
    const auto pMax = latestValue(535, nowMs);
    if (!cnUa || !cnUb || !cnUc || !lvLow || !lvUp || !hvLow || !hvUp || !gradP || !pMax) {
        return false;
    }

    auto clamp = [&](double value, double lower, double upper) {
        return std::max(lower, std::min(value, upper));
    };

    double outPaLv = 0.0;
    if (*cnUa < *lvLow) {
        outPaLv -= *gradP;
    } else if (*cnUa > *lvUp) {
        outPaLv += *gradP;
    }
    outPaLv = clamp(outPaLv, -*pMax, 0.0);

    double outPbLv = 0.0;
    if (*cnUb < *lvLow) {
        outPbLv -= *gradP;
    } else if (*cnUb > *lvUp) {
        outPbLv += *gradP;
    }
    outPbLv = clamp(outPbLv, -*pMax, 0.0);

    double outPcLv = 0.0;
    if (*cnUc < *lvLow) {
        outPcLv -= *gradP;
    } else if (*cnUc > *lvUp) {
        outPcLv += *gradP;
    }
    outPcLv = clamp(outPcLv, -*pMax, 0.0);

    const double outP3Lv = std::abs(outPaLv) + std::abs(outPbLv) + std::abs(outPcLv);

    double outPaHv = 0.0;
    if (*cnUa > *hvUp) {
        outPaHv += *gradP;
    } else if (*cnUa < *hvLow) {
        outPaHv -= *gradP;
    }
    outPaHv = clamp(outPaHv, 0.0, *pMax);

    double outPbHv = 0.0;
    if (*cnUb > *hvUp) {
        outPbHv += *gradP;
    } else if (*cnUb < *hvLow) {
        outPbHv -= *gradP;
    }
    outPbHv = clamp(outPbHv, 0.0, *pMax);

    double outPcHv = 0.0;
    if (*cnUc > *hvUp) {
        outPcHv += *gradP;
    } else if (*cnUc < *hvLow) {
        outPcHv -= *gradP;
    }
    outPcHv = clamp(outPcHv, 0.0, *pMax);

    const double outP3Hv = std::abs(outPaHv) + std::abs(outPbHv) + std::abs(outPcHv);

    bool updated = false;
    const std::pair<std::uint32_t, double> outputs[] = {
        {605, outPaLv}, {606, outPbLv}, {607, outPcLv}, {608, outP3Lv},
        {609, outPaHv}, {610, outPbHv}, {611, outPcHv}, {612, outP3Hv},
        {10, outP3Lv != 0.0 ? 1.0 : 0.0},
        {12, outP3Hv != 0.0 ? 1.0 : 0.0}
    };
    for (const auto& output : outputs) {
        if (!hasRoute(output.first)) {
            continue;
        }
        const auto routed = set(output.first, output.second, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    return updated;
}

bool LegacyEmsEngine::updateCdFdTargets(std::int64_t nowMs, LegacyEmsRunResult& result) {
    const auto bmsSoc = latestValue(1570, nowMs);
    const auto cdTargetP = latestValue(451, nowMs);
    const auto cdTargetSoc = latestValue(452, nowMs);
    const auto tqPxzPosValue = latestValue(453, nowMs);
    const auto tqPxzPosEn = latestValue(454, nowMs);
    const auto fdTargetP = latestValue(455, nowMs);
    const auto fdTargetSoc = latestValue(456, nowMs);
    const auto tqPxzNegValue = latestValue(457, nowMs);
    const auto tqPxzNegEn = latestValue(458, nowMs);
    const auto fhP3 = latestValue(312, nowMs);
    if (!bmsSoc || !cdTargetP || !cdTargetSoc || !fdTargetP || !fdTargetSoc || !fhP3) {
        return false;
    }

    bool updated = false;

    double outP3Cd = 0.0;
    double cdRun = 0.0;
    if (*bmsSoc < *cdTargetSoc && *cdTargetP != 0.0) {
        cdRun = 1.0;
        if (tqPxzPosEn && *tqPxzPosEn == 1.0 && tqPxzPosValue) {
            const double pYx = *tqPxzPosValue - *fhP3;
            outP3Cd = pYx > 0.0 ? std::min(pYx, *cdTargetP) : 0.0;
        } else {
            outP3Cd = *cdTargetP;
        }
    }

    double outP3Fd = 0.0;
    double fdRun = 0.0;
    if (*bmsSoc > *fdTargetSoc && *fdTargetP != 0.0) {
        fdRun = 1.0;
        if (tqPxzNegEn && *tqPxzNegEn == 1.0 && tqPxzNegValue) {
            outP3Fd = -1.0 * std::min(std::max(*tqPxzNegValue - *fhP3, 0.0), *fdTargetP);
        } else {
            outP3Fd = -1.0 * (*fdTargetP);
        }
    }

    const std::pair<std::uint32_t, double> outputs[] = {
        {14, cdRun},
        {16, fdRun},
        {613, outP3Cd},
        {614, outP3Fd}
    };
    for (const auto& output : outputs) {
        if (!hasRoute(output.first)) {
            continue;
        }
        const auto routed = set(output.first, output.second, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    return updated;
}

bool LegacyEmsEngine::updateSkTargets(std::int64_t nowMs, LegacyEmsRunResult& result) {
    const auto skP3 = latestValue(590, nowMs);
    const auto skQ3 = latestValue(591, nowMs);
    if (!skP3 || !skQ3) {
        return false;
    }

    const std::pair<std::uint32_t, double> outputs[] = {
        {26, (*skP3 != 0.0 || *skQ3 != 0.0) ? 1.0 : 0.0}
    };

    bool updated = false;
    for (const auto& output : outputs) {
        if (!hasRoute(output.first)) {
            continue;
        }
        const auto routed = set(output.first, output.second, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    return updated;
}

bool LegacyEmsEngine::updatePhTargets(std::int64_t nowMs, LegacyEmsRunResult& result) {
    const auto tqPa = latestValue(209, nowMs);
    const auto tqPb = latestValue(210, nowMs);
    const auto tqPc = latestValue(211, nowMs);
    const auto tqP3 = latestValue(212, nowMs);
    const auto cnPa = latestValue(259, nowMs);
    const auto cnPb = latestValue(260, nowMs);
    const auto cnPc = latestValue(261, nowMs);
    const auto bphPer = latestValue(562, nowMs);
    if (!tqPa || !tqPb || !tqPc || !tqP3 || !cnPa || !cnPb || !cnPc || !bphPer || !isFiniteNonZero(*tqP3)) {
        return false;
    }

    const double tqCnPa = *tqPa + *cnPa;
    const double tqCnPb = *tqPb + *cnPb;
    const double tqCnPc = *tqPc + *cnPc;
    const double allowBph = *bphPer * *tqP3 / 300.0;
    const double maxVal = std::max(tqCnPa, std::max(tqCnPb, tqCnPc));
    const double minVal = std::min(tqCnPa, std::min(tqCnPb, tqCnPc));
    const double dVal = maxVal - minVal;
    const double tqCnBph = std::abs(dVal / *tqP3 * 300.0);

    double outPaPh = 0.0;
    double outPbPh = 0.0;
    double outPcPh = 0.0;
    if (dVal > allowBph) {
        const double setVal = (dVal - allowBph) / 2.0;
        if (tqCnPa == maxVal) {
            outPaPh = setVal;
        } else if (tqCnPb == maxVal) {
            outPbPh = setVal;
        } else if (tqCnPc == maxVal) {
            outPcPh = setVal;
        }

        if (tqCnPa == minVal) {
            outPaPh = -setVal;
        } else if (tqCnPb == minVal) {
            outPbPh = -setVal;
        } else if (tqCnPc == minVal) {
            outPcPh = -setVal;
        }
    }
    const double outP3Ph = std::abs(outPaPh) + std::abs(outPbPh) + std::abs(outPcPh);

    bool updated = false;
    const std::pair<std::uint32_t, double> outputs[] = {
        {564, tqCnBph},
        {565, tqCnPa},
        {566, tqCnPb},
        {567, tqCnPc},
        {623, outPaPh},
        {624, outPbPh},
        {625, outPcPh},
        {20, outP3Ph != 0.0 ? 1.0 : 0.0}
    };
    for (const auto& output : outputs) {
        if (!hasRoute(output.first)) {
            continue;
        }
        const auto routed = set(output.first, output.second, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    return updated;
}

bool LegacyEmsEngine::updateGfTargets(std::int64_t nowMs, LegacyEmsRunResult& result) {
    const auto fhPa = latestValue(309, nowMs);
    const auto fhPb = latestValue(310, nowMs);
    const auto fhPc = latestValue(311, nowMs);
    const auto tqPxzNegValue = latestValue(457, nowMs);
    const auto startHour = latestValue(581, nowMs);
    const auto endHour = latestValue(583, nowMs);
    if (!fhPa || !fhPb || !fhPc || !tqPxzNegValue || !startHour || !endHour) {
        return false;
    }

    const int currentHour = localHourFromEpochMs(nowMs);
    double outPaGf = 0.0;
    double outPbGf = 0.0;
    double outPcGf = 0.0;
    if (currentHour >= static_cast<int>(*startHour) && currentHour <= static_cast<int>(*endHour)) {
        outPaGf = *fhPa <= *tqPxzNegValue ? (*tqPxzNegValue - *fhPa) : 0.0;
        outPbGf = *fhPb <= *tqPxzNegValue ? (*tqPxzNegValue - *fhPb) : 0.0;
        outPcGf = *fhPc <= *tqPxzNegValue ? (*tqPxzNegValue - *fhPc) : 0.0;
    }
    const double outP3Gf = std::abs(outPaGf) + std::abs(outPbGf) + std::abs(outPcGf);

    bool updated = false;
    const std::pair<std::uint32_t, double> outputs[] = {
        {619, outPaGf},
        {620, outPbGf},
        {621, outPcGf},
        {622, outP3Gf},
        {22, outP3Gf != 0.0 ? 1.0 : 0.0}
    };
    for (const auto& output : outputs) {
        if (!hasRoute(output.first)) {
            continue;
        }
        const auto routed = set(output.first, output.second, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    return updated;
}

bool LegacyEmsEngine::updateDsTargets(std::int64_t nowMs, LegacyEmsRunResult& result) {
    const int currentHour = localHourFromEpochMs(nowMs);
    const auto power = latestValue(400 + static_cast<std::uint32_t>(currentHour), nowMs);
    const auto soc = latestValue(424 + static_cast<std::uint32_t>(currentHour), nowMs);
    const auto modeValue = latestValue(760 + static_cast<std::uint32_t>(currentHour), nowMs);
    const auto bmsSoc = latestValue(1570, nowMs);
    const auto cnUa = latestValue(251, nowMs);
    const auto cnUb = latestValue(252, nowMs);
    const auto cnUc = latestValue(253, nowMs);
    const auto gradP = latestValue(533, nowMs);
    const auto dsEnVmax = latestValue(463, nowMs);
    const auto dsEnVmin = latestValue(464, nowMs);
    if (!power || !soc || !modeValue || !bmsSoc || !cnUa || !cnUb || !cnUc || !gradP || !dsEnVmax || !dsEnVmin) {
        return false;
    }

    const int mode = static_cast<int>(*modeValue);
    double outPaDs = latestValue(615, nowMs).value_or(0.0);
    double outPbDs = latestValue(616, nowMs).value_or(0.0);
    double outPcDs = latestValue(617, nowMs).value_or(0.0);
    double outP3Ds = latestValue(618, nowMs).value_or(0.0);

    if (*bmsSoc < *soc && mode != 2) {
        if (outP3Ds < *power - 1.0) {
            if (*cnUa > *dsEnVmin) {
                outPaDs += *gradP;
            }
            if (*cnUb > *dsEnVmin) {
                outPbDs += *gradP;
            }
            if (*cnUc > *dsEnVmin) {
                outPcDs += *gradP;
            }
        }
        if ((std::abs(outPaDs) + std::abs(outPbDs) + std::abs(outPcDs)) > *power) {
            outPaDs = 0.3333 * *power;
            outPbDs = 0.3333 * *power;
            outPcDs = 0.3333 * *power;
        }
        if (*cnUa < *dsEnVmin - 1.0) {
            outPaDs -= *gradP;
        }
        if (*cnUb < *dsEnVmin - 1.0) {
            outPbDs -= *gradP;
        }
        if (*cnUc < *dsEnVmin - 1.0) {
            outPcDs -= *gradP;
        }
        outPaDs = std::max(0.0, outPaDs);
        outPbDs = std::max(0.0, outPbDs);
        outPcDs = std::max(0.0, outPcDs);
    } else if (*bmsSoc > *soc && mode != 1) {
        if (outP3Ds < *power - 1.0) {
            if (*cnUa < *dsEnVmax) {
                outPaDs -= *gradP;
            }
            if (*cnUb < *dsEnVmax) {
                outPbDs -= *gradP;
            }
            if (*cnUc < *dsEnVmax) {
                outPcDs -= *gradP;
            }
        }
        if ((std::abs(outPaDs) + std::abs(outPbDs) + std::abs(outPcDs)) > *power) {
            outPaDs = -0.3333 * *power;
            outPbDs = -0.3333 * *power;
            outPcDs = -0.3333 * *power;
        }
        if (*cnUa > *dsEnVmax + 1.0) {
            outPaDs += *gradP;
        }
        if (*cnUb > *dsEnVmax + 1.0) {
            outPbDs += *gradP;
        }
        if (*cnUc > *dsEnVmax + 1.0) {
            outPcDs += *gradP;
        }
        outPaDs = std::min(0.0, outPaDs);
        outPbDs = std::min(0.0, outPbDs);
        outPcDs = std::min(0.0, outPcDs);
    } else {
        auto decayToZero = [&](double value) {
            if (value > 0.0) {
                value -= 2.0 * *gradP;
                return std::max(0.0, value);
            }
            if (value < 0.0) {
                value += 2.0 * *gradP;
                return std::min(0.0, value);
            }
            return value;
        };
        outPaDs = decayToZero(outPaDs);
        outPbDs = decayToZero(outPbDs);
        outPcDs = decayToZero(outPcDs);
    }

    if (mode == 1) {
        outPaDs = std::max(0.0, outPaDs);
        outPbDs = std::max(0.0, outPbDs);
        outPcDs = std::max(0.0, outPcDs);
    } else if (mode == 2) {
        outPaDs = std::min(0.0, outPaDs);
        outPbDs = std::min(0.0, outPbDs);
        outPcDs = std::min(0.0, outPcDs);
    }

    outP3Ds = std::abs(outPaDs) + std::abs(outPbDs) + std::abs(outPcDs);

    bool updated = false;
    const std::pair<std::uint32_t, double> outputs[] = {
        {461, *power},
        {462, *soc},
        {615, outPaDs},
        {616, outPbDs},
        {617, outPcDs},
        {618, outP3Ds},
        {18, outP3Ds != 0.0 ? 1.0 : 0.0}
    };
    for (const auto& output : outputs) {
        if (!hasRoute(output.first)) {
            continue;
        }
        const auto routed = set(output.first, output.second, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    return updated;
}

bool LegacyEmsEngine::updatePowerSolveOutputs(std::int64_t nowMs, LegacyEmsRunResult& result) {
    const auto outQaCos = latestValue(601, nowMs);
    const auto outQbCos = latestValue(602, nowMs);
    const auto outQcCos = latestValue(603, nowMs);
    const auto skRun = latestValue(26, nowMs);
    const auto skP3 = latestValue(590, nowMs);
    const auto skQ3 = latestValue(591, nowMs);
    const auto zrEn = latestValue(23, nowMs);
    const auto zrP1 = latestValue(588, nowMs);
    const auto tqPxzPosEn = latestValue(454, nowMs);
    const auto tqPxzPosValue = latestValue(453, nowMs);
    const auto tqPxzNegEn = latestValue(458, nowMs);
    const auto tqPxzNegValue = latestValue(457, nowMs);
    const auto fhPa = latestValue(309, nowMs);
    const auto fhPb = latestValue(310, nowMs);
    const auto fhPc = latestValue(311, nowMs);
    const auto outPaDs = latestValue(615, nowMs);
    const auto outPbDs = latestValue(616, nowMs);
    const auto outPcDs = latestValue(617, nowMs);
    const auto outPaLv = latestValue(605, nowMs);
    const auto outPbLv = latestValue(606, nowMs);
    const auto outPcLv = latestValue(607, nowMs);
    const auto outPaHv = latestValue(609, nowMs);
    const auto outPbHv = latestValue(610, nowMs);
    const auto outPcHv = latestValue(611, nowMs);
    const auto outPaGf = latestValue(619, nowMs);
    const auto outPbGf = latestValue(620, nowMs);
    const auto outPcGf = latestValue(621, nowMs);
    const auto outPaPh = latestValue(623, nowMs);
    const auto outPbPh = latestValue(624, nowMs);
    const auto outPcPh = latestValue(625, nowMs);
    const auto pMax = latestValue(535, nowMs);
    const auto qMax = latestValue(504, nowMs);
    const auto s3Max = latestValue(151, nowMs);

    double pcsPaOut = outPaDs ? *outPaDs : 0.0;
    double pcsPbOut = outPbDs ? *outPbDs : 0.0;
    double pcsPcOut = outPcDs ? *outPcDs : 0.0;
    double pcsQaOut = outQaCos ? *outQaCos : 0.0;
    double pcsQbOut = outQbCos ? *outQbCos : 0.0;
    double pcsQcOut = outQcCos ? *outQcCos : 0.0;

    if (outPaLv && *outPaLv < 0.0) {
        pcsPaOut = std::min(pcsPaOut, *outPaLv);
    }
    if (outPbLv && *outPbLv < 0.0) {
        pcsPbOut = std::min(pcsPbOut, *outPbLv);
    }
    if (outPcLv && *outPcLv < 0.0) {
        pcsPcOut = std::min(pcsPcOut, *outPcLv);
    }
    if (outPaHv && *outPaHv > 0.0) {
        pcsPaOut = std::max(pcsPaOut, *outPaHv);
    }
    if (outPbHv && *outPbHv > 0.0) {
        pcsPbOut = std::max(pcsPbOut, *outPbHv);
    }
    if (outPcHv && *outPcHv > 0.0) {
        pcsPcOut = std::max(pcsPcOut, *outPcHv);
    }
    if (outPaGf && *outPaGf > 0.0) {
        pcsPaOut = std::max(pcsPaOut, *outPaGf);
    }
    if (outPbGf && *outPbGf > 0.0) {
        pcsPbOut = std::max(pcsPbOut, *outPbGf);
    }
    if (outPcGf && *outPcGf > 0.0) {
        pcsPcOut = std::max(pcsPcOut, *outPcGf);
    }

    pcsPaOut += outPaPh ? *outPaPh : 0.0;
    pcsPbOut += outPbPh ? *outPbPh : 0.0;
    pcsPcOut += outPcPh ? *outPcPh : 0.0;

    if (skRun && *skRun == 1.0 && skP3 && skQ3) {
        pcsPaOut = pcsPbOut = pcsPcOut = *skP3 / 3.0;
        pcsQaOut = pcsQbOut = pcsQcOut = *skQ3 / 3.0;
    }

    if (tqPxzPosEn && *tqPxzPosEn == 1.0 && tqPxzPosValue && fhPa && fhPb && fhPc) {
        const double powerACdSy = *tqPxzPosValue - *fhPa;
        const double powerBCdSy = *tqPxzPosValue - *fhPb;
        const double powerCCdSy = *tqPxzPosValue - *fhPc;
        if (pcsPaOut > 0.0) {
            pcsPaOut = std::min(pcsPaOut, powerACdSy);
            if (pcsPaOut < 0.0) {
                pcsPaOut = 0.0;
            }
        }
        if (pcsPbOut > 0.0) {
            pcsPbOut = std::min(pcsPbOut, powerBCdSy);
            if (pcsPbOut < 0.0) {
                pcsPbOut = 0.0;
            }
        }
        if (pcsPcOut > 0.0) {
            pcsPcOut = std::min(pcsPcOut, powerCCdSy);
            if (pcsPcOut < 0.0) {
                pcsPcOut = 0.0;
            }
        }
    }

    if (tqPxzNegEn && *tqPxzNegEn == 1.0 && tqPxzNegValue && fhPa && fhPb && fhPc) {
        const double powerAFdSy = *tqPxzNegValue - *fhPa;
        const double powerBFdSy = *tqPxzNegValue - *fhPb;
        const double powerCFdSy = *tqPxzNegValue - *fhPc;
        if (pcsPaOut < 0.0) {
            pcsPaOut = std::max(pcsPaOut, powerAFdSy);
            if (pcsPaOut > 0.0) {
                pcsPaOut = 0.0;
            }
        }
        if (pcsPbOut < 0.0) {
            pcsPbOut = std::max(pcsPbOut, powerBFdSy);
            if (pcsPbOut > 0.0) {
                pcsPbOut = 0.0;
            }
        }
        if (pcsPcOut < 0.0) {
            pcsPcOut = std::max(pcsPcOut, powerCFdSy);
            if (pcsPcOut > 0.0) {
                pcsPcOut = 0.0;
            }
        }
    }

    double zrRunValue = 0.0;
    if (zrEn && *zrEn == 1.0 && tqPxzNegValue && zrP1 && fhPa && fhPb && fhPc) {
        const double powerAZxSy = *tqPxzNegValue - *fhPa - *zrP1;
        const double powerBZxSy = *tqPxzNegValue - *fhPb - *zrP1;
        const double powerCZxSy = *tqPxzNegValue - *fhPc - *zrP1;
        if (powerAZxSy < 0.0) {
            pcsPaOut = std::min(pcsPaOut, powerAZxSy);
            zrRunValue = 1.0;
        }
        if (powerBZxSy < 0.0) {
            pcsPbOut = std::min(pcsPbOut, powerBZxSy);
            zrRunValue = 1.0;
        }
        if (powerCZxSy < 0.0) {
            pcsPcOut = std::min(pcsPcOut, powerCZxSy);
            zrRunValue = 1.0;
        }
    }

    auto clampAbs = [](double value, double limit) {
        if (!(limit > 0.0) || std::abs(value) <= limit) {
            return value;
        }
        return value > 0.0 ? limit : -limit;
    };

    if (pMax) {
        pcsPaOut = clampAbs(pcsPaOut, *pMax);
        pcsPbOut = clampAbs(pcsPbOut, *pMax);
        pcsPcOut = clampAbs(pcsPcOut, *pMax);
    }
    if (qMax) {
        pcsQaOut = clampAbs(pcsQaOut, *qMax);
        pcsQbOut = clampAbs(pcsQbOut, *qMax);
        pcsQcOut = clampAbs(pcsQcOut, *qMax);
    }

    if (s3Max && *s3Max > 0.0) {
        const double s1Max = *s3Max / 3.0;
        auto clampByApparentPower = [&](double& p, double q) {
            const double s = std::sqrt(p * p + q * q);
            if (!(s1Max > 0.0) || s <= s1Max) {
                return;
            }
            const double remain = std::max(0.0, s1Max * s1Max - q * q);
            if (p > 0.0) {
                p = std::sqrt(remain);
            } else if (p < 0.0) {
                p = -std::sqrt(remain);
            } else {
                p = 0.0;
            }
        };
        clampByApparentPower(pcsPaOut, pcsQaOut);
        clampByApparentPower(pcsPbOut, pcsQbOut);
        clampByApparentPower(pcsPcOut, pcsQcOut);
    }

    const double pcsP3Out = pcsPaOut + pcsPbOut + pcsPcOut;
    if (pcsP3Out > 0.0) {
        const auto chargeKwAllow = latestValue(1552, nowMs);
        if (chargeKwAllow && pcsP3Out > *chargeKwAllow) {
            double pcsPos = 0.0;
            double pcsNeg = 0.0;
            if (pcsPaOut > 0.0) {
                pcsPos += pcsPaOut;
            } else {
                pcsNeg += pcsPaOut;
            }
            if (pcsPbOut > 0.0) {
                pcsPos += pcsPbOut;
            } else {
                pcsNeg += pcsPbOut;
            }
            if (pcsPcOut > 0.0) {
                pcsPos += pcsPcOut;
            } else {
                pcsNeg += pcsPcOut;
            }
            if (pcsPos > 0.0) {
                const double bmsPer = (*chargeKwAllow - pcsNeg) / pcsPos;
                if (pcsPaOut > 0.0) {
                    pcsPaOut *= bmsPer;
                }
                if (pcsPbOut > 0.0) {
                    pcsPbOut *= bmsPer;
                }
                if (pcsPcOut > 0.0) {
                    pcsPcOut *= bmsPer;
                }
            }
        }
    } else if (pcsP3Out < 0.0) {
        const auto dischargeKwAllow = latestValue(1553, nowMs);
        if (dischargeKwAllow) {
            const double bmsDischargeKwAllow = -*dischargeKwAllow;
            if (pcsP3Out < bmsDischargeKwAllow) {
                double pcsPos = 0.0;
                double pcsNeg = 0.0;
                if (pcsPaOut > 0.0) {
                    pcsPos += pcsPaOut;
                } else {
                    pcsNeg += pcsPaOut;
                }
                if (pcsPbOut > 0.0) {
                    pcsPos += pcsPbOut;
                } else {
                    pcsNeg += pcsPbOut;
                }
                if (pcsPcOut > 0.0) {
                    pcsPos += pcsPcOut;
                } else {
                    pcsNeg += pcsPcOut;
                }
                if (pcsNeg < 0.0) {
                    const double bmsPer = (bmsDischargeKwAllow - pcsPos) / pcsNeg;
                    if (pcsPaOut < 0.0) {
                        pcsPaOut *= bmsPer;
                    }
                    if (pcsPbOut < 0.0) {
                        pcsPbOut *= bmsPer;
                    }
                    if (pcsPcOut < 0.0) {
                        pcsPcOut *= bmsPer;
                    }
                }
            }
        }
    }

    const auto bmsSoc = latestValue(1570, nowMs);
    const auto bmsSocMax = latestValue(161, nowMs);
    const auto bmsSocMin = latestValue(162, nowMs);
    bool bmsLowLimit = false;
    bool bmsUpLimit = false;
    if (bmsSoc && bmsSocMax && bmsSocMin) {
        if (*bmsSoc < *bmsSocMin) {
            bmsLowLimit = true;
            if (pcsPaOut < 0.0) {
                pcsPaOut = 0.0;
            }
            if (pcsPbOut < 0.0) {
                pcsPbOut = 0.0;
            }
            if (pcsPcOut < 0.0) {
                pcsPcOut = 0.0;
            }
            pcsQaOut = 0.0;
            pcsQbOut = 0.0;
            pcsQcOut = 0.0;
        }
        if (*bmsSoc > *bmsSocMax) {
            bmsUpLimit = true;
            if (pcsPaOut > 0.0) {
                pcsPaOut = 0.0;
            }
            if (pcsPbOut > 0.0) {
                pcsPbOut = 0.0;
            }
            if (pcsPcOut > 0.0) {
                pcsPcOut = 0.0;
            }
        }
    }

    bool updated = false;
    std::vector<std::pair<std::uint32_t, double>> outputs;
    if (bmsLowLimit) {
        outputs.push_back({8, 0.0});
        outputs.push_back({10, 0.0});
    }
    if (bmsUpLimit) {
        outputs.push_back({12, 0.0});
        outputs.push_back({22, 0.0});
    }
    outputs.push_back({24, zrRunValue});
    outputs.push_back({627, pcsPaOut});
    outputs.push_back({628, pcsPbOut});
    outputs.push_back({629, pcsPcOut});
    outputs.push_back({630, pcsQaOut});
    outputs.push_back({631, pcsQbOut});
    outputs.push_back({632, pcsQcOut});
    for (const auto& output : outputs) {
        if (!hasRoute(output.first)) {
            continue;
        }
        const auto routed = set(output.first, output.second, nowMs);
        if (routed.accepted) {
            ++result.latestWrites;
            updated = true;
        }
    }
    submitPcsPowerCommands(pcsPaOut, pcsPbOut, pcsPcOut, pcsQaOut, pcsQbOut, pcsQcOut, nowMs, result);
    return updated;
}

void LegacyEmsEngine::submitPcsPowerCommands(
    double pcsPaOut,
    double pcsPbOut,
    double pcsPcOut,
    double pcsQaOut,
    double pcsQbOut,
    double pcsQcOut,
    std::int64_t nowMs,
    LegacyEmsRunResult& result
) {
    const auto pcsComStatus = latestValue(kPcsComStatus, nowMs);
    if (!pcsComStatus || *pcsComStatus != 1.0) {
        return;
    }

    struct PcsCommandTarget {
        std::uint32_t index = 0;
        int targetValue = 0;
    };

    const PcsCommandTarget commands[] = {
        {kPcsPControlA, static_cast<int>(pcsPaOut)},
        {kPcsPControlB, static_cast<int>(pcsPbOut)},
        {kPcsPControlC, static_cast<int>(pcsPcOut)},
        {kPcsQControlA, static_cast<int>(pcsQaOut)},
        {kPcsQControlB, static_cast<int>(pcsQbOut)},
        {kPcsQControlC, static_cast<int>(pcsQcOut)}
    };
    const auto pendingWrites = router_.peekPendingWrites();
    for (const auto& command : commands) {
        const double targetDouble = static_cast<double>(command.targetValue);
        const double current = latestValue(command.index, nowMs).value_or(0.0);
        if (command.targetValue == 0) {
            if (current == 0.0) {
                continue;
            }
        } else if (valueMatches(current, targetDouble)) {
            continue;
        }
        bool pendingDuplicate = false;
        for (const auto& pending : pendingWrites) {
            if (pending.index == command.index && valueMatches(pending.value, targetDouble)) {
                pendingDuplicate = true;
                break;
            }
        }
        if (pendingDuplicate) {
            continue;
        }

        const auto routed = submitCommand(command.index, targetDouble, nowMs);
        if (routed.accepted) {
            ++result.deviceWrites;
        }
    }
}

CommandSubmitResult LegacyEmsEngine::putLatest(std::uint32_t index, double value, std::int64_t nowMs) {
    PointValue point;
    point.index = index;
    point.value = value;
    point.quality = 1;
    point.ts = nowMs;
    point.expireAt = nowMs + std::max<std::int64_t>(1, defaultTtlMs_);
    return router_.putLatestByIndex(point);
}

CommandSubmitResult LegacyEmsEngine::submitCommand(std::uint32_t index, double value, std::int64_t nowMs) {
    PendingWriteCommand command;
    command.cmdId = cmdId(index, nowMs);
    command.index = index;
    command.value = value;
    command.source = "legacy-ems";
    command.ts = nowMs;
    return router_.submitWriteCommand(command);
}

}  // namespace edge_gateway
