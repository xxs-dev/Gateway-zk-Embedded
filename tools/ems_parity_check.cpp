#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/graph_ems_engine.hpp"
#include "edge_gateway/legacy_ems_engine.hpp"
#include "edge_gateway/legacy_ems_point_catalog.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace {

struct Options {
    std::string appConfig = "config/runtime/apps/mqtt-service.json";
    std::string legacyRuleCode;
    std::string graphRuleCode;
    std::string baselineGraphFile;
    std::string candidateGraphFile;
    std::string reportFile;
    double tolerance = 0.001;
    std::vector<std::uint32_t> indexes;
    std::vector<std::pair<std::uint32_t, double>> pointValues;
    std::vector<std::pair<std::string, std::string>> profileValues;
    std::unordered_map<std::uint32_t, std::uint32_t> indexMap;
    bool requireRefreshed = false;
    std::size_t iterations = 1;
    std::int64_t stepMs = 200;
};

struct Difference {
    std::uint32_t index = 0;
    std::uint32_t candidateIndex = 0;
    std::string reason;
    double baselineValue = 0.0;
    double candidateValue = 0.0;
    bool hasBaseline = false;
    bool hasCandidate = false;
};

struct SegmentCleanup {
    explicit SegmentCleanup(std::string value) : name(std::move(value)) {}
    ~SegmentCleanup() {
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(name);
    }
    std::string name;
};

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

int processId() {
#ifdef _WIN32
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream out;
    for (const auto ch : value) {
        switch (ch) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default: out << ch; break;
        }
    }
    return out.str();
}

std::string fileFingerprint(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open graph file for fingerprint: " + path);
    }
    std::uint64_t hash = 14695981039346656037ULL;
    char buffer[4096];
    while (input.good()) {
        input.read(buffer, sizeof(buffer));
        const auto count = input.gcount();
        for (std::streamsize i = 0; i < count; ++i) {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= 1099511628211ULL;
        }
    }
    std::ostringstream out;
    out << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

std::vector<std::uint32_t> parseIndexes(const std::string& value) {
    std::vector<std::uint32_t> result;
    std::stringstream input(value);
    std::string token;
    while (std::getline(input, token, ',')) {
        if (token.empty()) {
            continue;
        }
        const auto index = static_cast<std::uint32_t>(std::stoul(token));
        if (index == 0) {
            throw std::invalid_argument("comparison index must be positive");
        }
        result.push_back(index);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::pair<std::uint32_t, double> parsePointValue(const std::string& value) {
    const auto separator = value.find('=');
    if (separator == std::string::npos) {
        throw std::invalid_argument("point value must use INDEX=VALUE");
    }
    const auto index = static_cast<std::uint32_t>(std::stoul(value.substr(0, separator)));
    const auto number = std::stod(value.substr(separator + 1));
    if (index == 0 || !std::isfinite(number)) {
        throw std::invalid_argument("point value requires a positive index and finite value");
    }
    return {index, number};
}

std::pair<std::string, std::string> parseProfileValue(const std::string& value) {
    const auto separator = value.find('=');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= value.size()) {
        throw std::invalid_argument("profile value must use KEY=VALUE");
    }
    return {value.substr(0, separator), value.substr(separator + 1)};
}

std::pair<std::uint32_t, std::uint32_t> parseIndexMap(const std::string& value) {
    const auto separator = value.find('=');
    if (separator == std::string::npos) {
        throw std::invalid_argument("index map must use BASELINE=CANDIDATE");
    }
    const auto baseline = static_cast<std::uint32_t>(std::stoul(value.substr(0, separator)));
    const auto candidate = static_cast<std::uint32_t>(std::stoul(value.substr(separator + 1)));
    if (baseline == 0 || candidate == 0) {
        throw std::invalid_argument("index map requires positive indexes");
    }
    return {baseline, candidate};
}

Options parseOptions(int argc, char* argv[]) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto requireValue = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument("missing value for " + arg);
            }
            return argv[++i];
        };
        if (arg == "--app-config") {
            options.appConfig = requireValue();
        } else if (arg == "--legacy-rule") {
            options.legacyRuleCode = requireValue();
        } else if (arg == "--graph-rule") {
            options.graphRuleCode = requireValue();
        } else if (arg == "--baseline-graph") {
            options.baselineGraphFile = requireValue();
        } else if (arg == "--candidate-graph") {
            options.candidateGraphFile = requireValue();
        } else if (arg == "--report") {
            options.reportFile = requireValue();
        } else if (arg == "--tolerance") {
            options.tolerance = std::stod(requireValue());
        } else if (arg == "--indexes") {
            options.indexes = parseIndexes(requireValue());
        } else if (arg == "--point") {
            options.pointValues.push_back(parsePointValue(requireValue()));
        } else if (arg == "--profile") {
            options.profileValues.push_back(parseProfileValue(requireValue()));
        } else if (arg == "--index-map") {
            const auto mapping = parseIndexMap(requireValue());
            options.indexMap[mapping.first] = mapping.second;
        } else if (arg == "--require-refreshed") {
            options.requireRefreshed = true;
        } else if (arg == "--iterations") {
            options.iterations = static_cast<std::size_t>(std::stoul(requireValue()));
        } else if (arg == "--step-ms") {
            options.stepMs = std::stoll(requireValue());
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage: EmsParityCheck [options]\n"
                << "  --app-config FILE   App config containing legacyEms and graphEms rules\n"
                << "  --legacy-rule CODE  Select a legacyEms rule by ruleCode\n"
                << "  --graph-rule CODE   Select a graphEms rule by ruleCode\n"
                << "  --baseline-graph FILE Compare this existing graph against --candidate-graph\n"
                << "  --candidate-graph FILE Candidate modular graph for graph-to-graph comparison\n"
                << "  --indexes CSV       Output indexes to compare\n"
                << "  --tolerance VALUE   Absolute numeric tolerance, default 0.001\n"
                << "  --point INDEX=VALUE Add or override one input snapshot value; repeatable\n"
                << "  --profile KEY=VALUE Set a graph profile value for both graphs; repeatable\n"
                << "  --index-map A=B    Compare baseline index A with candidate index B; repeatable\n"
                << "  --require-refreshed Fail when a compared output was not written in this run\n"
                << "  --iterations COUNT Run both engines repeatedly, default 1\n"
                << "  --step-ms VALUE    Simulated interval between iterations, default 200\n"
                << "  --report FILE       Write JSON report to this file\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown option: " + arg);
        }
    }
    if (!std::isfinite(options.tolerance) || options.tolerance < 0.0) {
        throw std::invalid_argument("tolerance must be a non-negative finite number");
    }
    if (options.baselineGraphFile.empty() != options.candidateGraphFile.empty()) {
        throw std::invalid_argument("--baseline-graph and --candidate-graph must be used together");
    }
    if (options.iterations == 0 || options.iterations > 10000 || options.stepMs <= 0) {
        throw std::invalid_argument("iterations must be 1..10000 and step-ms must be positive");
    }
    if (options.indexes.empty()) {
        options.indexes = {461, 462};
        for (std::uint32_t index = 601; index <= 632; ++index) {
            options.indexes.push_back(index);
        }
    }
    return options;
}

const edge_gateway::ComputeRuleConfig& selectRule(
    const edge_gateway::ComputeEngineConfig& config,
    const std::string& type,
    const std::string& ruleCode
) {
    for (const auto& rule : config.rules) {
        if (rule.script.type == type && (ruleCode.empty() || rule.ruleCode == ruleCode)) {
            return rule;
        }
    }
    throw std::runtime_error("cannot find " + type + " rule" + (ruleCode.empty() ? std::string() : ": " + ruleCode));
}

void addUnique(std::vector<std::string>& values, std::unordered_set<std::string>& seen, const std::string& value) {
    if (!value.empty() && seen.insert(value).second) {
        values.push_back(value);
    }
}

std::string legacyPointCode(const edge_gateway::LegacyEmsPoint& point) {
    return !point.name.empty() ? point.name : !point.desc.empty() ? point.desc : "legacy_var_" + std::to_string(point.index);
}

void addLegacyRoutes(
    edge_gateway::PointStoreRouter& router,
    const edge_gateway::LegacyEmsPointCatalog& catalog,
    const std::string& sharedMemoryName
) {
    for (const auto& point : catalog.points()) {
        if (point.index == 0 || router.routeByIndex(point.index)) {
            continue;
        }
        edge_gateway::PointStoreRoute route;
        route.index = point.index;
        route.machineCode = "EMS_PARITY";
        route.meterCode = point.source == edge_gateway::LegacyEmsPointSource::Global ? "LEGACY_GL" : "LEGACY_VAR";
        route.pointCode = legacyPointCode(point);
        route.interfaceCode = "ems-parity";
        route.interfaceType = "compute";
        route.sharedMemoryName = sharedMemoryName;
        route.reportOnChange = true;
        router.addRoute(route);
    }
}

bool looksLikeIndexParameter(const std::string& key) {
    std::string normalized;
    normalized.reserve(key.size());
    for (const auto ch : key) {
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return normalized.find("index") != std::string::npos ||
        normalized.size() >= 5 && normalized.compare(normalized.size() - 5, 5, "input") == 0 ||
        normalized.size() >= 6 && normalized.compare(normalized.size() - 6, 6, "output") == 0;
}

bool writableGraphParameter(const std::string& nodeType, const std::string& key) {
    if (nodeType == "controlWrite" && key == "targetIndex") {
        return true;
    }
    if (nodeType != "pcsWriteback" && nodeType != "pcsPowerSolve") {
        return false;
    }
    return key.find("Control") != std::string::npos || key.find("control") != std::string::npos;
}

void addExplicitGraphRoutes(
    edge_gateway::PointStoreRouter& router,
    const edge_gateway::GraphEmsConfig& graph,
    const std::string& sharedMemoryName
) {
    for (const auto& node : graph.nodes) {
        for (const auto& parameter : node.params) {
            if (!looksLikeIndexParameter(parameter.first) || parameter.second.empty()) {
                continue;
            }
            std::size_t consumed = 0;
            std::uint32_t index = 0;
            try {
                index = static_cast<std::uint32_t>(std::stoul(parameter.second, &consumed));
            } catch (...) {
                continue;
            }
            if (index == 0 || consumed != parameter.second.size() || router.routeByIndex(index)) {
                continue;
            }
            edge_gateway::PointStoreRoute route;
            route.index = index;
            route.machineCode = "EMS_PARITY";
            route.meterCode = "GRAPH";
            route.pointCode = node.id + "." + parameter.first;
            route.interfaceCode = "ems-parity";
            route.interfaceType = "compute";
            route.sharedMemoryName = sharedMemoryName;
            route.writable = writableGraphParameter(node.type, parameter.first);
            router.addRoute(route);
        }
    }
}

void cloneRoutes(
    const edge_gateway::PointStoreRouter& source,
    edge_gateway::PointStoreRouter& target,
    const std::string& sharedMemoryName
) {
    for (const auto& entry : source.routes()) {
        auto route = entry.second;
        route.sharedMemoryName = sharedMemoryName;
        target.addRoute(route);
    }
}

void seedValues(
    const std::vector<edge_gateway::StoredPointValue>& values,
    edge_gateway::PointStoreRouter& target
) {
    for (const auto& value : values) {
        edge_gateway::PointValue point;
        point.index = value.index;
        point.value = value.value;
        point.quality = value.quality;
        point.ts = value.ts;
        point.expireAt = value.expireAt;
        point.stale = value.stale;
        const auto result = target.putLatestByIndex(point);
        if (!result.accepted) {
            throw std::runtime_error("failed to clone input index " + std::to_string(value.index) + ": " + result.message);
        }
    }
}

std::string buildReport(
    const Options& options,
    const std::string& comparisonMode,
    const std::string& baselineCode,
    const std::string& baselineSource,
    const std::string& baselineFingerprint,
    const std::string& candidateCode,
    const std::string& candidateSource,
    const std::string& candidateFingerprint,
    std::int64_t checkedAt,
    std::size_t sampleCount,
    std::size_t comparisonCount,
    const std::vector<std::uint32_t>& missingInBoth,
    const std::vector<Difference>& differences
) {
    std::ostringstream out;
    out << std::setprecision(15);
    out << "{\n"
        << "  \"schemaVersion\": \"1.0.0\",\n"
        << "  \"passed\": " << (differences.empty() && comparisonCount > 0 ? "true" : "false") << ",\n"
        << "  \"comparisonMode\": \"" << jsonEscape(comparisonMode) << "\",\n"
        << "  \"baseline\": {\"code\": \"" << jsonEscape(baselineCode)
        << "\", \"source\": \"" << jsonEscape(baselineSource)
        << "\", \"fingerprint\": \"" << jsonEscape(baselineFingerprint) << "\"},\n"
        << "  \"candidate\": {\"code\": \"" << jsonEscape(candidateCode)
        << "\", \"source\": \"" << jsonEscape(candidateSource)
        << "\", \"fingerprint\": \"" << jsonEscape(candidateFingerprint) << "\"},\n"
        << "  \"checkedAt\": " << checkedAt << ",\n"
        << "  \"sampleCount\": " << sampleCount << ",\n"
        << "  \"comparisonCount\": " << comparisonCount << ",\n"
        << "  \"missingInBothCount\": " << missingInBoth.size() << ",\n"
        << "  \"tolerance\": " << options.tolerance << ",\n"
        << "  \"requireRefreshed\": " << (options.requireRefreshed ? "true" : "false") << ",\n"
        << "  \"iterations\": " << options.iterations << ",\n"
        << "  \"stepMs\": " << options.stepMs << ",\n"
        << "  \"mismatchCount\": " << differences.size() << ",\n"
        << "  \"indexes\": [";
    for (std::size_t i = 0; i < options.indexes.size(); ++i) {
        if (i > 0) out << ", ";
        out << options.indexes[i];
    }
    out << "],\n  \"indexMappings\": [";
    bool firstMapping = true;
    for (const auto& mapping : options.indexMap) {
        out << (firstMapping ? "" : ", ")
            << "{\"baseline\": " << mapping.first << ", \"candidate\": " << mapping.second << "}";
        firstMapping = false;
    }
    out << "],\n  \"missingInBoth\": [";
    for (std::size_t i = 0; i < missingInBoth.size(); ++i) {
        if (i > 0) out << ", ";
        out << missingInBoth[i];
    }
    out << "],\n  \"mismatches\": [";
    for (std::size_t i = 0; i < differences.size(); ++i) {
        const auto& difference = differences[i];
        out << (i == 0 ? "\n" : ",\n")
            << "    {\"index\": " << difference.index
            << ", \"reason\": \"" << jsonEscape(difference.reason) << "\"";
        if (difference.candidateIndex != 0 && difference.candidateIndex != difference.index) {
            out << ", \"candidateIndex\": " << difference.candidateIndex;
        }
        if (difference.hasBaseline) out << ", \"baselineValue\": " << difference.baselineValue;
        if (difference.hasCandidate) out << ", \"candidateValue\": " << difference.candidateValue;
        out << "}";
    }
    out << (differences.empty() ? "]\n" : "\n  ]\n") << "}\n";
    return out.str();
}

}  // namespace

int main(int argc, char* argv[]) {
    using namespace edge_gateway;
    try {
        const auto options = parseOptions(argc, argv);
        const auto graphToGraph = !options.baselineGraphFile.empty();
        auto app = ConfigLoader::loadAppConfigFromFile(options.appConfig);
        DeviceIdentity identity;
        if (!app.identityConfigFile.empty()) {
            identity = ConfigLoader::loadDeviceIdentityFromFile(app.identityConfigFile);
        }
        const auto devices = ConfigLoader::loadMany(app.deviceConfigFiles, identity);

        const ComputeRuleConfig* legacyRule = nullptr;
        const ComputeRuleConfig* graphRule = nullptr;
        std::unique_ptr<LegacyEmsPointCatalog> catalog;
        GraphEmsConfig baselineGraph;
        GraphEmsConfig candidateGraph;
        std::string baselineSource;
        std::string candidateSource;
        std::string baselineFingerprint;
        std::string candidateFingerprint;
        std::unordered_map<std::string, std::string> baselineProfile;
        std::unordered_map<std::string, std::string> candidateProfile;

        if (graphToGraph) {
            baselineSource = options.baselineGraphFile;
            candidateSource = options.candidateGraphFile;
            baselineGraph = GraphEmsConfig::loadFromFile(baselineSource);
            candidateGraph = GraphEmsConfig::loadFromFile(candidateSource);
            baselineFingerprint = fileFingerprint(baselineSource);
            candidateFingerprint = fileFingerprint(candidateSource);
        } else {
            legacyRule = &selectRule(app.computeEngine, "legacyEms", options.legacyRuleCode);
            graphRule = &selectRule(app.computeEngine, "graphEms", options.graphRuleCode);
            catalog.reset(new LegacyEmsPointCatalog(LegacyEmsPointCatalog::loadFromFiles(
                legacyRule->script.legacyGlListFile,
                legacyRule->script.legacyVarListFile,
                legacyRule->script.legacyEncoding.empty() ? std::string("gbk") : legacyRule->script.legacyEncoding
            )));
            candidateSource = graphRule->script.graphFile;
            candidateGraph = GraphEmsConfig::loadFromFile(candidateSource);
            candidateFingerprint = fileFingerprint(candidateSource);
            candidateProfile = graphRule->script.graphProfile;
        }
        for (const auto& value : options.profileValues) {
            baselineProfile[value.first] = value.second;
            candidateProfile[value.first] = value.second;
        }

        std::vector<std::string> sharedMemoryNames;
        std::unordered_set<std::string> seen;
        for (const auto& name : app.computeEngine.sharedMemoryNames) addUnique(sharedMemoryNames, seen, name);
        for (const auto& device : devices) addUnique(sharedMemoryNames, seen, device.memoryStore.sharedMemoryName);
        addUnique(sharedMemoryNames, seen, app.computeEngine.outputDefaultSharedMemoryName);
        if (sharedMemoryNames.empty()) sharedMemoryNames.push_back("gateway_point_store");

        PointStoreRouter liveRouter;
        std::vector<std::unique_ptr<MemoryPointStore>> liveStores;
        for (const auto& name : sharedMemoryNames) {
            liveStores.emplace_back(new MemoryPointStore(name));
            liveRouter.addStore(name, *liveStores.back());
        }
        liveRouter.addRoutesFromDeviceConfigs(devices, sharedMemoryNames.front());
        const auto outputStore = app.computeEngine.outputDefaultSharedMemoryName.empty()
            ? sharedMemoryNames.front()
            : app.computeEngine.outputDefaultSharedMemoryName;
        if (graphToGraph) {
            addExplicitGraphRoutes(liveRouter, baselineGraph, outputStore);
        } else {
            addLegacyRoutes(liveRouter, *catalog, outputStore);
        }
        addExplicitGraphRoutes(liveRouter, candidateGraph, outputStore);

        const auto checkedAt = currentTimeMs();
        auto liveValues = liveRouter.getAllLatest(checkedAt);
        for (const auto& configured : options.pointValues) {
            auto existing = std::find_if(liveValues.begin(), liveValues.end(), [&](const StoredPointValue& value) {
                return value.index == configured.first;
            });
            StoredPointValue point;
            point.index = configured.first;
            point.value = configured.second;
            point.quality = 1;
            point.ts = checkedAt;
            point.expireAt = checkedAt + app.computeEngine.defaultOutputTtlMs;
            if (existing == liveValues.end()) {
                liveValues.push_back(point);
            } else {
                *existing = point;
            }
        }
        const auto suffix = std::to_string(processId()) + "_" + std::to_string(checkedAt);
        const auto baselineStoreName = "ems_parity_baseline_" + suffix;
        const auto candidateStoreName = "ems_parity_candidate_" + suffix;
        MemoryPointStore::cleanupOrphanedSegment(baselineStoreName);
        MemoryPointStore::cleanupOrphanedSegment(candidateStoreName);
        SegmentCleanup baselineCleanup(baselineStoreName);
        SegmentCleanup candidateCleanup(candidateStoreName);

        std::vector<Difference> differences;
        std::vector<std::uint32_t> missingInBoth;
        std::size_t comparisonCount = 0;
        const auto evaluatedAt = checkedAt + static_cast<std::int64_t>(options.iterations - 1) * options.stepMs;
        {
            MemoryPointStore baselineStore(baselineStoreName);
            MemoryPointStore candidateStore(candidateStoreName);
            PointStoreRouter baselineRouter;
            PointStoreRouter candidateRouter;
            baselineRouter.addStore(baselineStoreName, baselineStore);
            candidateRouter.addStore(candidateStoreName, candidateStore);
            cloneRoutes(liveRouter, baselineRouter, baselineStoreName);
            cloneRoutes(liveRouter, candidateRouter, candidateStoreName);
            seedValues(liveValues, baselineRouter);
            seedValues(liveValues, candidateRouter);

            if (graphToGraph) {
                GraphEmsEngine baselineEngine(
                    baselineGraph,
                    baselineRouter,
                    app.computeEngine.defaultOutputTtlMs,
                    std::string(),
                    baselineProfile
                );
                for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
                    const auto result = baselineEngine.runOnce(
                        checkedAt + static_cast<std::int64_t>(iteration) * options.stepMs
                    );
                    if (!result.errors.empty()) {
                        throw std::runtime_error("baseline graph execution failed: " + result.errors.front());
                    }
                }
            } else {
                LegacyEmsEngine baselineEngine(
                    *catalog,
                    baselineRouter,
                    app.computeEngine.defaultOutputTtlMs,
                    legacyRule->script.legacyProfile
                );
                for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
                    (void)baselineEngine.runOnce(checkedAt + static_cast<std::int64_t>(iteration) * options.stepMs);
                }
            }

            GraphEmsEngine candidateEngine(
                candidateGraph,
                candidateRouter,
                app.computeEngine.defaultOutputTtlMs,
                std::string(),
                candidateProfile
            );
            for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
                const auto result = candidateEngine.runOnce(
                    checkedAt + static_cast<std::int64_t>(iteration) * options.stepMs
                );
                if (!result.errors.empty()) {
                    throw std::runtime_error("candidate graph execution failed: " + result.errors.front());
                }
            }

            for (const auto index : options.indexes) {
                const auto mapped = options.indexMap.find(index);
                const auto candidateIndex = mapped == options.indexMap.end() ? index : mapped->second;
                const auto baselineValue = baselineRouter.getLatestByIndex(index, evaluatedAt);
                const auto candidateValue = candidateRouter.getLatestByIndex(candidateIndex, evaluatedAt);
                Difference difference;
                difference.index = index;
                difference.candidateIndex = candidateIndex;
                difference.hasBaseline = static_cast<bool>(baselineValue);
                difference.hasCandidate = static_cast<bool>(candidateValue);
                if (baselineValue) difference.baselineValue = baselineValue->value;
                if (candidateValue) difference.candidateValue = candidateValue->value;
                if (!baselineValue && !candidateValue) {
                    missingInBoth.push_back(index);
                } else if (!baselineValue || !candidateValue) {
                    ++comparisonCount;
                    difference.reason = !baselineValue ? "baseline output missing" : "candidate output missing";
                    differences.push_back(difference);
                } else if (options.requireRefreshed &&
                           (baselineValue->ts != evaluatedAt || candidateValue->ts != evaluatedAt)) {
                    ++comparisonCount;
                    difference.reason = baselineValue->ts != evaluatedAt && candidateValue->ts != evaluatedAt
                        ? "both outputs not refreshed"
                        : baselineValue->ts != evaluatedAt ? "baseline output not refreshed" : "candidate output not refreshed";
                    differences.push_back(difference);
                } else if (baselineValue->quality != candidateValue->quality) {
                    ++comparisonCount;
                    difference.reason = "quality mismatch";
                    differences.push_back(difference);
                } else if (std::abs(baselineValue->value - candidateValue->value) > options.tolerance) {
                    ++comparisonCount;
                    difference.reason = "value mismatch";
                    differences.push_back(difference);
                } else {
                    ++comparisonCount;
                }
            }
        }

        const auto report = buildReport(
            options,
            graphToGraph ? "graph-to-graph" : "legacy-to-graph",
            graphToGraph ? baselineGraph.graphCode : legacyRule->ruleCode,
            graphToGraph ? baselineSource : "legacyEms:" + legacyRule->ruleCode,
            graphToGraph ? baselineFingerprint : std::string(),
            candidateGraph.graphCode,
            candidateSource,
            candidateFingerprint,
            evaluatedAt,
            liveValues.size(),
            comparisonCount,
            missingInBoth,
            differences
        );
        if (!options.reportFile.empty()) {
            std::ofstream output(options.reportFile.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
            if (!output.is_open()) {
                throw std::runtime_error("failed to open parity report: " + options.reportFile);
            }
            output << report;
        }
        std::cout << report;
        return differences.empty() && comparisonCount > 0 ? 0 : 2;
    } catch (const std::exception& ex) {
        std::cerr << "EMS parity check failed: " << ex.what() << "\n";
        return 1;
    }
}
