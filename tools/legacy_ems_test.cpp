#include "edge_gateway/legacy_ems_engine.hpp"
#include "edge_gateway/legacy_ems_point_catalog.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <iostream>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/compute_engine_service.hpp"
#include "edge_gateway/graph_ems_engine.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/point_store_router.hpp"

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireNear(double actual, double expected, double tolerance, const std::string& message) {
    if (std::fabs(actual - expected) > tolerance) {
        throw std::runtime_error(message);
    }
}

void requireThrowsWithMessage(const std::string& expected, const std::function<void()>& action) {
    try {
        action();
    } catch (const std::exception& ex) {
        const std::string message = ex.what();
        if (message.find(expected) == std::string::npos) {
            throw std::runtime_error("unexpected exception message: " + message);
        }
        return;
    }
    throw std::runtime_error("expected exception not thrown: " + expected);
}

edge_gateway::PointDefinition point(std::uint32_t index, const std::string& code, bool writable) {
    edge_gateway::PointDefinition result;
    result.index = index;
    result.pointCode = code;
    result.name = code;
    result.desc = code;
    result.enabled = true;
    result.read.enable = true;
    result.read.cachePolicy.ttlMs = 600000;
    result.write.enable = writable;
    return result;
}

void addRouteIfMissing(
    edge_gateway::PointStoreRouter& router,
    std::uint32_t index,
    const std::string& pointCode,
    const std::string& sharedMemoryName,
    bool writable
) {
    if (router.routeByIndex(index)) {
        return;
    }
    edge_gateway::PointStoreRoute route;
    route.index = index;
    route.machineCode = "GW_TEST";
    route.meterCode = "LEGACY";
    route.pointCode = pointCode;
    route.interfaceCode = "legacy-ems";
    route.interfaceType = "compute";
    route.sharedMemoryName = sharedMemoryName;
    route.writable = writable;
    router.addRoute(route);
}

edge_gateway::DeviceConfig buildTestDeviceConfig() {
    edge_gateway::DeviceConfig config;
    config.machineCode = "GW_TEST";
    config.meterCode = "LEGACY";
    config.deviceName = "Legacy EMS Test";
    config.protocol.type = "computed";
    config.memoryStore.sharedMemoryName = "legacy_ems_test_store";
    config.memoryStore.maxLatestPoints = 256;
    config.memoryStore.maxPendingWrites = 32;
    config.memoryStore.maxPersistentSamples = 32;
    config.points.push_back(point(201, "TQ_avg_UA", false));
    config.points.push_back(point(202, "TQ_avg_UB", false));
    config.points.push_back(point(203, "TQ_avg_UC", false));
    config.points.push_back(point(209, "TQ_avg_PA", false));
    config.points.push_back(point(210, "TQ_avg_PB", false));
    config.points.push_back(point(211, "TQ_avg_PC", false));
    config.points.push_back(point(212, "TQ_avg_P3", false));
    config.points.push_back(point(213, "TQ_avg_QA", false));
    config.points.push_back(point(214, "TQ_avg_QB", false));
    config.points.push_back(point(215, "TQ_avg_QC", false));
    config.points.push_back(point(216, "TQ_avg_Q3", false));
    config.points.push_back(point(217, "TQ_avg_SA", false));
    config.points.push_back(point(218, "TQ_avg_SB", false));
    config.points.push_back(point(219, "TQ_avg_SC", false));
    config.points.push_back(point(220, "TQ_avg_S3", false));
    config.points.push_back(point(221, "TQ_avg_COSA", false));
    config.points.push_back(point(222, "TQ_avg_COSB", false));
    config.points.push_back(point(223, "TQ_avg_COSC", false));
    config.points.push_back(point(224, "TQ_avg_COS3", false));
    config.points.push_back(point(225, "TQ_avg_P_BPH", false));
    config.points.push_back(point(251, "CN_avg_UA", false));
    config.points.push_back(point(252, "CN_avg_UB", false));
    config.points.push_back(point(253, "CN_avg_UC", false));
    config.points.push_back(point(259, "CN_avg_PA", false));
    config.points.push_back(point(260, "CN_avg_PB", false));
    config.points.push_back(point(261, "CN_avg_PC", false));
    config.points.push_back(point(262, "CN_avg_P3", false));
    config.points.push_back(point(263, "CN_avg_QA", false));
    config.points.push_back(point(264, "CN_avg_QB", false));
    config.points.push_back(point(265, "CN_avg_QC", false));
    config.points.push_back(point(266, "CN_avg_Q3", false));
    config.points.push_back(point(309, "FH_avg_PA", false));
    config.points.push_back(point(310, "FH_avg_PB", false));
    config.points.push_back(point(311, "FH_avg_PC", false));
    config.points.push_back(point(312, "FH_avg_P3", false));
    config.points.push_back(point(313, "FH_avg_QA", false));
    config.points.push_back(point(314, "FH_avg_QB", false));
    config.points.push_back(point(315, "FH_avg_QC", false));
    config.points.push_back(point(316, "FH_avg_Q3", false));
    config.points.push_back(point(317, "FH_avg_SA", false));
    config.points.push_back(point(318, "FH_avg_SB", false));
    config.points.push_back(point(319, "FH_avg_SC", false));
    config.points.push_back(point(320, "FH_avg_S3", false));
    config.points.push_back(point(321, "FH_avg_COSA", false));
    config.points.push_back(point(322, "FH_avg_COSB", false));
    config.points.push_back(point(323, "FH_avg_COSC", false));
    config.points.push_back(point(324, "FH_avg_COS3", false));
    config.points.push_back(point(325, "FH_avg_P_BPH", false));
    config.points.push_back(point(505, "cos_target_qa", false));
    config.points.push_back(point(506, "cos_target_qb", false));
    config.points.push_back(point(507, "cos_target_qc", false));
    config.points.push_back(point(508, "cos_target_q3", false));
    config.points.push_back(point(8, "cos_run_flag", false));
    config.points.push_back(point(514, "cos_target", false));
    config.points.push_back(point(601, "out_qa_cos", false));
    config.points.push_back(point(602, "out_qb_cos", false));
    config.points.push_back(point(603, "out_qc_cos", false));
    config.points.push_back(point(604, "out_q3_cos", false));
    config.points.push_back(point(10, "lv_run_flag", false));
    config.points.push_back(point(12, "hv_run_flag", false));
    config.points.push_back(point(544, "lv_low", false));
    config.points.push_back(point(545, "lv_up", false));
    config.points.push_back(point(546, "hv_low", false));
    config.points.push_back(point(547, "hv_up", false));
    config.points.push_back(point(504, "pcs_q1_max", false));
    config.points.push_back(point(533, "p_grad", false));
    config.points.push_back(point(535, "p_max", false));
    config.points.push_back(point(605, "out_pa_lv", false));
    config.points.push_back(point(606, "out_pb_lv", false));
    config.points.push_back(point(607, "out_pc_lv", false));
    config.points.push_back(point(608, "out_p3_lv", false));
    config.points.push_back(point(609, "out_pa_hv", false));
    config.points.push_back(point(610, "out_pb_hv", false));
    config.points.push_back(point(611, "out_pc_hv", false));
    config.points.push_back(point(612, "out_p3_hv", false));
    config.points.push_back(point(13, "cd_en", false));
    config.points.push_back(point(14, "cd_run", false));
    config.points.push_back(point(15, "fd_en", false));
    config.points.push_back(point(16, "fd_run", false));
    config.points.push_back(point(17, "ds_en", false));
    config.points.push_back(point(18, "ds_run", false));
    config.points.push_back(point(451, "cd_target_p", false));
    config.points.push_back(point(452, "cd_target_soc", false));
    config.points.push_back(point(455, "fd_target_p", false));
    config.points.push_back(point(456, "fd_target_soc", false));
    config.points.push_back(point(457, "tq_pxz_neg_value", false));
    config.points.push_back(point(453, "tq_pxz_pos_value", false));
    config.points.push_back(point(454, "tq_pxz_pos_en", false));
    config.points.push_back(point(458, "tq_pxz_neg_en", false));
    config.points.push_back(point(613, "out_p3_cd", false));
    config.points.push_back(point(614, "out_p3_fd", false));
    config.points.push_back(point(19, "ph_en", false));
    config.points.push_back(point(20, "ph_run", false));
    config.points.push_back(point(21, "gf_en", false));
    config.points.push_back(point(22, "gf_run", false));
    config.points.push_back(point(23, "zr_en", false));
    config.points.push_back(point(24, "zr_run", false));
    config.points.push_back(point(25, "sk_en", false));
    config.points.push_back(point(26, "sk_run", false));
    config.points.push_back(point(562, "bph_per", false));
    config.points.push_back(point(564, "tq_cn_avg_p_bph", false));
    config.points.push_back(point(565, "tq_cn_avg_pa", false));
    config.points.push_back(point(566, "tq_cn_avg_pb", false));
    config.points.push_back(point(567, "tq_cn_avg_pc", false));
    config.points.push_back(point(581, "gf_charge_start_time", false));
    config.points.push_back(point(583, "gf_charge_end_time", false));
    config.points.push_back(point(588, "zr_p1", false));
    config.points.push_back(point(590, "sk_p3", false));
    config.points.push_back(point(591, "sk_q3", false));
    config.points.push_back(point(400, "ds_power_0", false));
    config.points.push_back(point(424, "ds_soc_0", false));
    config.points.push_back(point(760, "ds_mode_0", false));
    config.points.push_back(point(461, "ds_power_now", false));
    config.points.push_back(point(462, "ds_soc_now", false));
    config.points.push_back(point(463, "ds_en_vmax", false));
    config.points.push_back(point(464, "ds_en_vmin", false));
    config.points.push_back(point(615, "out_pa_ds", false));
    config.points.push_back(point(616, "out_pb_ds", false));
    config.points.push_back(point(617, "out_pc_ds", false));
    config.points.push_back(point(618, "out_p3_ds", false));
    config.points.push_back(point(619, "out_pa_gf", false));
    config.points.push_back(point(620, "out_pb_gf", false));
    config.points.push_back(point(621, "out_pc_gf", false));
    config.points.push_back(point(622, "out_p3_gf", false));
    config.points.push_back(point(623, "out_pa_ph", false));
    config.points.push_back(point(624, "out_pb_ph", false));
    config.points.push_back(point(625, "out_pc_ph", false));
    config.points.push_back(point(151, "pcs_s3_max", false));
    config.points.push_back(point(1570, "stack_soc", false));
    config.points.push_back(point(161, "box_soc_max", false));
    config.points.push_back(point(162, "box_soc_min", false));
    config.points.push_back(point(627, "pcs_pa_out", false));
    config.points.push_back(point(628, "pcs_pb_out", false));
    config.points.push_back(point(629, "pcs_pc_out", false));
    config.points.push_back(point(630, "pcs_qa_out", false));
    config.points.push_back(point(631, "pcs_qb_out", false));
    config.points.push_back(point(632, "pcs_qc_out", false));
    config.points.push_back(point(1318, "pcs_p_ctl_a", true));
    config.points.push_back(point(1319, "pcs_p_ctl_b", true));
    config.points.push_back(point(1320, "pcs_p_ctl_c", true));
    config.points.push_back(point(1321, "pcs_q_ctl_a", true));
    config.points.push_back(point(1322, "pcs_q_ctl_b", true));
    config.points.push_back(point(1323, "pcs_q_ctl_c", true));
    config.points.push_back(point(1399, "pcs_com_num", false));
    config.points.push_back(point(984, "running_lamp", true));
    config.points.push_back(point(1030, "TQ_UA", false));
    config.points.push_back(point(1031, "TQ_UB", false));
    config.points.push_back(point(1032, "TQ_UC", false));
    config.points.push_back(point(1036, "TQ_PA", false));
    config.points.push_back(point(1037, "TQ_PB", false));
    config.points.push_back(point(1038, "TQ_PC", false));
    config.points.push_back(point(1039, "TQ_P3", false));
    config.points.push_back(point(1040, "TQ_QA", false));
    config.points.push_back(point(1041, "TQ_QB", false));
    config.points.push_back(point(1042, "TQ_QC", false));
    config.points.push_back(point(1043, "TQ_Q3", false));
    config.points.push_back(point(1130, "CN_UA", false));
    config.points.push_back(point(1131, "CN_UB", false));
    config.points.push_back(point(1132, "CN_UC", false));
    config.points.push_back(point(1136, "CN_PA", false));
    config.points.push_back(point(1137, "CN_PB", false));
    config.points.push_back(point(1138, "CN_PC", false));
    config.points.push_back(point(1139, "CN_P3", false));
    config.points.push_back(point(1140, "CN_QA", false));
    config.points.push_back(point(1141, "CN_QB", false));
    config.points.push_back(point(1142, "CN_QC", false));
    config.points.push_back(point(1143, "CN_Q3", false));
    config.points.push_back(point(4536, "BW_P3", false));
    config.points.push_back(point(4537, "BW_PA", false));
    config.points.push_back(point(4538, "BW_PB", false));
    config.points.push_back(point(4539, "BW_PC", false));
    config.points.push_back(point(4540, "BW_Q3", false));
    config.points.push_back(point(4541, "BW_QA", false));
    config.points.push_back(point(4542, "BW_QB", false));
    config.points.push_back(point(4543, "BW_QC", false));
    config.points.push_back(point(1552, "stack_charge_kw_allow", false));
    config.points.push_back(point(1553, "stack_discharge_kw_allow", false));
    config.points.push_back(point(1556, "stack_charge_current_allow", false));
    config.points.push_back(point(1557, "stack_discharge_current_allow", false));
    config.points.push_back(point(1566, "stack_real_voltage", false));
    config.points.push_back(point(1586, "stack_charge_kwh_sum", false));
    config.points.push_back(point(1587, "stack_discharge_kwh_sum", false));
    config.points.push_back(point(1615, "stack_charge_kwh_today", false));
    config.points.push_back(point(1616, "stack_discharge_kwh_today", false));
    config.points.push_back(point(398, "stack_charge_kwh_0save", false));
    config.points.push_back(point(399, "stack_discharge_kwh_0save", false));
    return config;
}

edge_gateway::DeviceConfig buildIsolatedTestDeviceConfig(const std::string& sharedMemoryName) {
    auto config = buildTestDeviceConfig();
    config.memoryStore.sharedMemoryName = sharedMemoryName;
    return config;
}

std::vector<std::string>& registeredStoreSegments() {
    static std::vector<std::string> segments;
    return segments;
}

void cleanupStoreSegment(const edge_gateway::MemoryStoreConfig& config) {
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(config.sharedMemoryName);
    registeredStoreSegments().push_back(config.sharedMemoryName);
}

struct StoreSegmentCleanupGuard {
    ~StoreSegmentCleanupGuard() {
        for (const auto& sharedMemoryName : registeredStoreSegments()) {
            try {
                edge_gateway::MemoryPointStore::cleanupOrphanedSegment(sharedMemoryName);
            } catch (...) {
            }
        }
    }
};

void writeTextFile(const std::string& path, const std::string& text) {
    std::ofstream output(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("failed to write test file: " + path);
    }
    output << text;
}

struct MeterAverageV2Spec {
    std::string id;
    std::string profileKey;
    std::string optionalProfileKey;
    std::uint32_t outputIndex = 0;

    MeterAverageV2Spec(
        std::string nodeId,
        std::string requiredProfile,
        std::string optionalProfile,
        std::uint32_t output
    ) : id(std::move(nodeId)),
        profileKey(std::move(requiredProfile)),
        optionalProfileKey(std::move(optionalProfile)),
        outputIndex(output) {
    }
};

void writeMeterAverageV2Graph(
    const std::string& path,
    const std::string& graphCode,
    const std::vector<MeterAverageV2Spec>& specs
) {
    std::ostringstream output;
    output << "{\n"
           << "  \"schemaVersion\":\"2.0.0\",\n"
           << "  \"graphCode\":\"" << graphCode << "\",\n"
           << "  \"compile\":{\"maxNodes\":64,\"maxEdges\":128,"
              "\"virtualIndexStart\":700000,\"virtualIndexEnd\":799999},\n"
           << "  \"nodes\":[\n";
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const auto& spec = specs[i];
        if (i != 0) {
            output << ",\n";
        }
        output << "    {\"id\":\"" << spec.id << "\",\"type\":\"meterAverage\","
               << "\"enabled\":true,\"order\":" << i << ",\"parameters\":{";
        bool hasProfile = false;
        if (!spec.profileKey.empty()) {
            output << "\"profileKey\":\"" << spec.profileKey << "\"";
            hasProfile = true;
        }
        if (!spec.optionalProfileKey.empty()) {
            if (hasProfile) {
                output << ',';
            }
            output << "\"optionalProfileKey\":\"" << spec.optionalProfileKey << "\"";
            hasProfile = true;
        }
        if (hasProfile) {
            output << ',';
        }
        output << "\"windowSizeIndex\":156,\"mappings\":[{\"input\":1036,\"output\":"
               << spec.outputIndex << "}]},\"ports\":["
               << "{\"id\":\"window\",\"direction\":\"input\",\"valueType\":\"number\","
                  "\"runtimePath\":\"/windowSizeIndex\","
                  "\"binding\":{\"kind\":\"point\",\"index\":156}},"
               << "{\"id\":\"input\",\"direction\":\"input\",\"valueType\":\"number\","
                  "\"runtimePath\":\"/mappings/0/input\","
                  "\"binding\":{\"kind\":\"point\",\"index\":1036}},"
               << "{\"id\":\"output\",\"direction\":\"output\",\"valueType\":\"number\","
                  "\"runtimePath\":\"/mappings/0/output\","
                  "\"binding\":{\"kind\":\"point\",\"index\":" << spec.outputIndex << "}}]}";
    }
    output << "\n  ],\n  \"links\":[]\n}\n";
    writeTextFile(path, output.str());
}

struct LegacyCatalogFixturePaths {
    std::string glListFile;
    std::string varListFile;
};

LegacyCatalogFixturePaths writeLegacyCatalogFixture() {
    LegacyCatalogFixturePaths paths{
        "graph_ems_catalog_gllist_test.xml",
        "graph_ems_catalog_varlist_test.xml"
    };
    writeTextFile(
        paths.glListFile,
        std::string("<Root>\n") +
        "<Data index=\"6\" name=\"var6\" desc=\"\xd5\xfb\xb9\xf1\xca\xd6\xd7\xd4\xb6\xaf\xc4\xa3\xca\xbd\" iolink=\"read\" />\n" +
        "<Data index=\"201\" name=\"var201\" desc=\"TQ_avg_UA\" iolink=\"read\" />\n" +
        "</Root>\n"
    );
    writeTextFile(
        paths.varListFile,
        std::string("<Root>\n") +
        "<Data index=\"984\" name=\"var984\" desc=\"\xd4\xcb\xd0\xd0\xd6\xb8\xca\xbe\xb5\xc6\" iolink=\"write\" />\n" +
        "<Data index=\"1030\" name=\"var1030\" desc=\"A\xcf\xe0\xb5\xe7\xd1\xb9\" iolink=\"read\" />\n" +
        "</Root>\n"
    );
    return paths;
}

LegacyCatalogFixturePaths writeRuntimeLegacyCatalogFixture(const edge_gateway::DeviceConfig& deviceConfig) {
    LegacyCatalogFixturePaths paths{
        "graph_ems_runtime_gllist_test.xml",
        "graph_ems_runtime_varlist_test.xml"
    };
    std::string varList = "<Root>\n";
    for (const auto& pointDefinition : deviceConfig.points) {
        varList += "<Data index=\"" + std::to_string(pointDefinition.index) +
            "\" name=\"" + pointDefinition.pointCode +
            "\" desc=\"" + pointDefinition.desc +
            "\" iolink=\"" + (pointDefinition.write.enable ? "write" : "read") + "\" />\n";
    }
    varList += "</Root>\n";
    writeTextFile(paths.glListFile, "<Root>\n</Root>\n");
    writeTextFile(paths.varListFile, varList);
    return paths;
}

edge_gateway::LegacyEmsPointCatalog buildRuntimeTestCatalog(
    const edge_gateway::LegacyEmsPointCatalog& fixtureCatalog,
    const edge_gateway::DeviceConfig& deviceConfig
) {
    auto catalog = fixtureCatalog;
    for (const auto& pointDefinition : deviceConfig.points) {
        edge_gateway::LegacyEmsPoint point;
        point.source = edge_gateway::LegacyEmsPointSource::Variable;
        point.index = pointDefinition.index;
        point.name = pointDefinition.pointCode;
        point.desc = pointDefinition.desc.empty() ? pointDefinition.pointCode : pointDefinition.desc;
        point.iolink = pointDefinition.write.enable ? "write" : "read";
        point.readable = pointDefinition.read.enable;
        point.writable = pointDefinition.write.enable;
        catalog.addPoint(point);
    }
    return catalog;
}

void setTestTimezone(const char* timezone) {
#if defined(_WIN32)
    _putenv_s("TZ", timezone);
    _tzset();
#else
    setenv("TZ", timezone, 1);
    tzset();
#endif
}

void removeFileIfExists(const std::string& path) {
    std::remove(path.c_str());
}

void removeEmptyDirectoryIfExists(const std::string& path) {
#ifdef _WIN32
    _rmdir(path.c_str());
#else
    rmdir(path.c_str());
#endif
}

}  // namespace

int main() {
    StoreSegmentCleanupGuard storeSegmentCleanup;
    try {
        setTestTimezone("UTC");
        const auto catalogFixture = writeLegacyCatalogFixture();

        const auto catalog = edge_gateway::LegacyEmsPointCatalog::loadFromFiles(
            catalogFixture.glListFile,
            catalogFixture.varListFile,
            "gbk"
        );

        require(catalog.size() == 4, "legacy catalog fixture should include GLList and VarList points");

        const auto gl6 = catalog.findByIndex(6);
        require(static_cast<bool>(gl6), "GLList index 6 missing");
        require(gl6->source == edge_gateway::LegacyEmsPointSource::Global, "index 6 should come from GLList");
        require(gl6->name == "var6", "index 6 name not parsed");
        require(gl6->desc == "整柜手自动模式", "GBK desc for index 6 not converted to UTF-8");

        const auto avg201 = catalog.findByIndex(201);
        require(static_cast<bool>(avg201), "GLList index 201 missing");
        require(avg201->desc == "TQ_avg_UA", "index 201 desc not parsed");

        const auto var984 = catalog.findByIndex(984);
        require(static_cast<bool>(var984), "VarList index 984 missing");
        require(var984->source == edge_gateway::LegacyEmsPointSource::Variable, "index 984 should come from VarList");
        require(var984->desc == "运行指示灯", "GBK desc for index 984 not converted to UTF-8");
        require(var984->writable, "index 984 should be detected as writable");

        const auto var1030 = catalog.findByIndex(1030);
        require(static_cast<bool>(var1030), "VarList index 1030 missing");
        require(var1030->desc == "A相电压", "GBK desc for index 1030 not converted to UTF-8");
        require(!var1030->writable, "index 1030 should be read-only");

        auto deviceConfig = buildTestDeviceConfig();
        const auto runtimeCatalog = buildRuntimeTestCatalog(catalog, deviceConfig);
        const auto runtimeCatalogFixture = writeRuntimeLegacyCatalogFixture(deviceConfig);
        cleanupStoreSegment(deviceConfig.memoryStore);
        edge_gateway::MemoryPointStore store(deviceConfig.memoryStore);
        edge_gateway::PointStoreRouter router;
        router.addStore(deviceConfig.memoryStore.sharedMemoryName, store);
        router.addRoutesFromDeviceConfigs({deviceConfig}, deviceConfig.memoryStore.sharedMemoryName);

        edge_gateway::LegacyEmsEngine engine(runtimeCatalog, router);
        engine.set(201, 223.5, 1000);
        const auto latest = router.getLatestByIndex(201, 1000);
        require(static_cast<bool>(latest), "legacy set should write latest value");
        requireNear(latest->value, 223.5, 0.0001, "legacy set latest value mismatch");

        const auto writeResult = engine.cmd(984, 1.0, 1100);
        require(writeResult.accepted, "legacy cmd should submit writable point command: " + writeResult.message);
        const auto pending = router.peekPendingWrites(1);
        require(pending.size() == 1, "legacy cmd should create one pending write");
        require(pending[0].index == 984, "pending write index mismatch");
        requireNear(pending[0].value, 1.0, 0.0001, "pending write value mismatch");
        require(pending[0].source == "legacy-ems", "pending write source mismatch");

        auto fullQueueConfig = buildTestDeviceConfig();
        fullQueueConfig.memoryStore.sharedMemoryName = "legacy_ems_test_full_write_queue";
        fullQueueConfig.memoryStore.maxPendingWrites = 1;
        cleanupStoreSegment(fullQueueConfig.memoryStore);
        edge_gateway::MemoryPointStore fullQueueStore(fullQueueConfig.memoryStore);
        edge_gateway::PointStoreRouter fullQueueRouter;
        fullQueueRouter.addStore(fullQueueConfig.memoryStore.sharedMemoryName, fullQueueStore);
        fullQueueRouter.addRoutesFromDeviceConfigs({fullQueueConfig}, fullQueueConfig.memoryStore.sharedMemoryName);
        edge_gateway::PendingWriteCommand queued;
        queued.cmdId = "queued";
        queued.index = 984;
        queued.value = 1.0;
        queued.source = "test";
        queued.ts = 1200;
        auto acceptedFullQueue = fullQueueRouter.submitWriteCommand(queued);
        require(acceptedFullQueue.accepted, "first write should fill bounded queue");
        queued.cmdId = "overflow";
        auto rejectedFullQueue = fullQueueRouter.submitWriteCommand(queued);
        require(!rejectedFullQueue.accepted, "full write queue should reject without throwing");
        require(
            rejectedFullQueue.message.find("shared pending write queue is full") != std::string::npos,
            "full write queue rejection message mismatch"
        );

        writeTextFile(
            "graph_ems_app_config_test.json",
            R"json({
  "computeEngine": {
    "enabled": true,
    "rules": [
      {
        "ruleCode": "graph_ems_parse",
        "name": "Graph EMS Parse",
        "enabled": true,
        "script": {
          "type": "graphEms",
          "graphFile": "runtime/logic/shuntong_ems_graph.json",
          "graphProfile": {
            "PCS_MODEL": "3",
            "Meter_TQ": "1"
          }
        }
      }
    ]
  }
})json"
        );
        const auto graphAppConfig = edge_gateway::ConfigLoader::loadAppConfigFromFile(
            "graph_ems_app_config_test.json"
        );
        require(graphAppConfig.runtimeMode == "gateway", "missing runtimeMode should default to gateway");
        require(graphAppConfig.computeEngine.rules.size() == 1, "graphEms rule not parsed");
        const auto& graphRule = graphAppConfig.computeEngine.rules[0];
        require(graphRule.script.type == "graphEms", "graphEms script type not parsed");
        require(
            graphRule.script.graphFile.find("shuntong_ems_graph.json") != std::string::npos,
            "graphFile not parsed"
        );
        require(graphRule.script.graphProfile.at("PCS_MODEL") == "3", "graphProfile not parsed");

        writeTextFile(
            "graph_ems_runtime_mode_app_config_test.json",
            R"json({
  "runtimeMode": "ems",
  "computeEngine": {
    "enabled": false,
    "rules": []
  }
})json"
        );
        const auto emsModeAppConfig = edge_gateway::ConfigLoader::loadAppConfigFromFile(
            "graph_ems_runtime_mode_app_config_test.json"
        );
        require(emsModeAppConfig.runtimeMode == "ems", "runtimeMode ems not parsed");

        writeTextFile(
            "agc_avc_runtime_mode_app_config_test.json",
            R"json({
  "runtimeMode": "agc_avc",
  "agcAvc": {
    "enabled": true,
    "shadowMode": true,
    "submitWrites": false
  }
})json"
        );
        const auto agcAvcModeAppConfig = edge_gateway::ConfigLoader::loadAppConfigFromFile(
            "agc_avc_runtime_mode_app_config_test.json"
        );
        require(agcAvcModeAppConfig.runtimeMode == "agc_avc", "runtimeMode agc_avc not parsed");

        writeTextFile(
            "graph_ems_invalid_runtime_mode_app_config_test.json",
            R"json({
  "runtimeMode": "invalid"
})json"
        );
        requireThrowsWithMessage("runtimeMode must be gateway, ems or agc_avc", []() {
            edge_gateway::ConfigLoader::loadAppConfigFromFile("graph_ems_invalid_runtime_mode_app_config_test.json");
        });

        writeTextFile(
            "graph_ems_minimal_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "minimal",
  "nodes": [
    { "id": "ds", "type": "timedChargeDischarge", "enabled": true }
  ],
  "edges": []
})json"
        );
        const auto graphConfig = edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_minimal_test.json");
        require(graphConfig.nodes.size() == 1, "graph node not parsed");
        require(graphConfig.nodes[0].id == "ds", "graph node id mismatch");
        require(graphConfig.nodes[0].type == "timedChargeDischarge", "graph node type mismatch");

        writeTextFile(
            "graph_ems_duplicate_node_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "duplicate",
  "nodes": [
    { "id": "ds", "type": "timedChargeDischarge", "enabled": true },
    { "id": "ds", "type": "phaseBalance", "enabled": true }
  ],
  "edges": []
})json"
        );
        requireThrowsWithMessage("duplicate graph node id", []() {
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_duplicate_node_test.json");
        });

        writeTextFile(
            "graph_ems_unknown_node_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "unknown",
  "nodes": [
    { "id": "custom", "type": "customScript", "enabled": true }
  ],
  "edges": []
})json"
        );
        requireThrowsWithMessage("unknown graph node type", []() {
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_unknown_node_test.json");
        });

        writeTextFile(
            "graph_ems_unsupported_schema_test.json",
            R"json({
  "schemaVersion": "2.0.0",
  "graphCode": "unsupported_schema",
  "nodes": [],
  "edges": []
})json"
        );
        requireThrowsWithMessage("expected major version 1", []() {
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_unsupported_schema_test.json");
        });

        writeTextFile(
            "graph_ems_limits_test.json",
            R"json({
  "schemaVersion": "1.3.0",
  "graphCode": "limits",
  "limits": { "maxNodes": 1, "maxEdges": 1 },
  "nodes": [
    { "id": "one", "type": "pointInput" },
    { "id": "two", "type": "pointInput" }
  ],
  "edges": []
})json"
        );
        requireThrowsWithMessage("node count exceeds maxNodes", []() {
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_limits_test.json");
        });

        writeTextFile(
            "graph_ems_invalid_formula_test.json",
            R"json({
  "schemaVersion": "1.1.0",
  "graphCode": "invalid_formula",
  "nodes": [
    {
      "id": "unsafe_formula",
      "type": "formula",
      "params": {
        "operation": "runScript",
        "inputs": [{ "value": 1 }],
        "outputIndex": 700150
      }
    }
  ],
  "edges": []
})json"
        );
        requireThrowsWithMessage("formula node has unsupported operation", []() {
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_invalid_formula_test.json");
        });

        writeTextFile(
            "graph_ems_invalid_switch_index_test.json",
            R"json({
  "schemaVersion": "1.1.0",
  "graphCode": "invalid_switch_index",
  "nodes": [
    {
      "id": "invalid_switch",
      "type": "switch",
      "params": {
        "conditionIndex": 0,
        "trueValue": 1,
        "falseValue": 0,
        "outputIndex": 700021
      }
    }
  ],
  "edges": []
})json"
        );
        requireThrowsWithMessage("conditionIndex must be a positive uint32 index", []() {
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_invalid_switch_index_test.json");
        });

        writeTextFile(
            "graph_ems_cycle_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "cycle",
  "nodes": [
    { "id": "ds", "type": "timedChargeDischarge", "enabled": true },
    { "id": "power_solve", "type": "pcsPowerSolve", "enabled": true }
  ],
  "edges": [
    { "from": "ds", "to": "power_solve" },
    { "from": "power_solve", "to": "ds" }
  ]
})json"
        );
        requireThrowsWithMessage("graph contains cycle", []() {
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_cycle_test.json");
        });

        writeTextFile(
            "graph_ems_meter_average_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "meter_average",
  "nodes": [
    {
      "id": "meter_average",
      "type": "meterAverage",
      "enabled": true,
      "params": {
        "windowSizeIndex": 156,
        "mappings": [
          { "input": 1036, "output": 209 }
        ]
      }
    }
  ],
  "edges": []
})json"
        );
        const auto graphAverageConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_average");
        edge_gateway::PointStoreRouter graphAverageRouter;
        cleanupStoreSegment(graphAverageConfig.memoryStore);
        edge_gateway::MemoryPointStore graphAverageStore(graphAverageConfig.memoryStore);
        graphAverageRouter.addStore(graphAverageConfig.memoryStore.sharedMemoryName, graphAverageStore);
        graphAverageRouter.addRoutesFromDeviceConfigs(
            {graphAverageConfig},
            graphAverageConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine graphSeedEngine(runtimeCatalog, graphAverageRouter);
        graphSeedEngine.set(156, 2.0, 1150);
        graphSeedEngine.set(1036, 30.0, 1150);
        edge_gateway::GraphEmsEngine graphAverageEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_meter_average_test.json"),
            graphAverageRouter,
            600000
        );
        const auto graphAverageResult = graphAverageEngine.runOnce(1150);
        require(graphAverageResult.latestWrites == 1, "graph meterAverage should count one latest write");
        const auto graphAvgPa = graphAverageRouter.getLatestByIndex(209, 1150);
        require(static_cast<bool>(graphAvgPa), "graph meterAverage output missing");
        requireNear(graphAvgPa->value, 30.0, 0.0001, "graph meterAverage output mismatch");

        writeTextFile(
            "graph_ems_generic_nodes_test.json",
            R"json({
  "schemaVersion": "1.1.0",
  "graphCode": "generic_nodes",
  "nodes": [
    {
      "id": "net_power",
      "type": "formula",
      "enabled": true,
      "params": {
        "operation": "subtract",
        "inputs": [
          { "index": 700001 },
          { "index": 700002 }
        ],
        "outputIndex": 700010
      }
    },
    {
      "id": "bounded_power",
      "type": "formula",
      "enabled": true,
      "params": {
        "operation": "clamp",
        "inputs": [
          { "index": 700010 }
        ],
        "lower": -50,
        "upper": 50,
        "outputIndex": 700011
      }
    },
    {
      "id": "power_direction",
      "type": "switch",
      "enabled": true,
      "params": {
        "leftIndex": 700011,
        "operator": "gt",
        "rightValue": 0,
        "trueValue": 1,
        "falseValue": -1,
        "outputIndex": 700012
      }
    }
  ],
  "edges": [
    { "from": "net_power", "to": "bounded_power" },
    { "from": "bounded_power", "to": "power_direction" }
  ]
})json"
        );
        const auto genericConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_generic_nodes");
        edge_gateway::PointStoreRouter genericRouter;
        cleanupStoreSegment(genericConfig.memoryStore);
        edge_gateway::MemoryPointStore genericStore(genericConfig.memoryStore);
        genericRouter.addStore(genericConfig.memoryStore.sharedMemoryName, genericStore);
        genericRouter.addRoutesFromDeviceConfigs({genericConfig}, genericConfig.memoryStore.sharedMemoryName);
        addRouteIfMissing(genericRouter, 700001, "GENERIC_GRID_POWER", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700002, "GENERIC_STORAGE_POWER", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700010, "GENERIC_NET_POWER", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700011, "GENERIC_BOUNDED_POWER", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700012, "GENERIC_POWER_DIRECTION", genericConfig.memoryStore.sharedMemoryName, false);
        edge_gateway::LegacyEmsEngine genericSeedEngine(runtimeCatalog, genericRouter);
        genericSeedEngine.set(700001, 120.0, 1155);
        genericSeedEngine.set(700002, 20.0, 1155);
        edge_gateway::GraphEmsEngine genericEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_generic_nodes_test.json"),
            genericRouter,
            600000
        );
        const auto genericResult = genericEngine.runOnce(1155);
        require(genericResult.errors.empty(), "generic graph nodes should execute without errors");
        require(genericResult.latestWrites == 3, "generic graph nodes should write three outputs");
        const auto genericNetPower = genericRouter.getLatestByIndex(700010, 1155);
        const auto genericBoundedPower = genericRouter.getLatestByIndex(700011, 1155);
        const auto genericDirection = genericRouter.getLatestByIndex(700012, 1155);
        require(static_cast<bool>(genericNetPower), "formula subtract output missing");
        require(static_cast<bool>(genericBoundedPower), "formula clamp output missing");
        require(static_cast<bool>(genericDirection), "switch output missing");
        requireNear(genericNetPower->value, 100.0, 0.0001, "formula subtract output mismatch");
        requireNear(genericBoundedPower->value, 50.0, 0.0001, "formula clamp output mismatch");
        requireNear(genericDirection->value, 1.0, 0.0001, "switch output mismatch");

        writeTextFile(
            "graph_ems_modular_math_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "modular_math",
  "nodes": [
    {
      "id": "square",
      "type": "formula",
      "params": {
        "operation": "square",
        "inputs": [{ "index": 700001 }],
        "outputIndex": 700013
      }
    },
    {
      "id": "sqrt",
      "type": "formula",
      "params": {
        "operation": "sqrt",
        "inputs": [{ "index": 700013 }],
        "outputIndex": 700014
      }
    },
    {
      "id": "missing_default",
      "type": "formula",
      "params": {
        "operation": "add",
        "inputs": [{ "index": 799999, "defaultValue": 5 }, { "value": 2 }],
        "outputIndex": 700016
      }
    },
    {
      "id": "safe_divide_zero",
      "type": "formula",
      "params": {
        "operation": "safeDivide",
        "inputs": [{ "value": 10 }, { "value": 0 }],
        "zeroDivisorValue": -1,
        "outputIndex": 700021
      }
    },
    {
      "id": "window",
      "type": "windowAggregate",
      "params": {
        "operation": "average",
        "inputIndex": 700001,
        "outputIndex": 700015,
        "windowSize": 3
      }
    }
  ],
  "edges": [
    { "from": "square", "to": "sqrt" }
  ]
})json"
        );
        addRouteIfMissing(genericRouter, 700013, "GENERIC_SQUARE", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700014, "GENERIC_SQRT", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700015, "GENERIC_WINDOW", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700016, "GENERIC_DEFAULT", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700021, "GENERIC_SAFE_DIVIDE", genericConfig.memoryStore.sharedMemoryName, false);
        genericSeedEngine.set(700001, 10.0, 1160);
        edge_gateway::GraphEmsEngine modularMathEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_modular_math_test.json"),
            genericRouter,
            600000
        );
        modularMathEngine.runOnce(1160);
        requireNear(genericRouter.getLatestByIndex(700014, 1160)->value, 10.0, 0.0001, "sqrt(square(x)) mismatch");
        requireNear(genericRouter.getLatestByIndex(700016, 1160)->value, 7.0, 0.0001, "formula default input mismatch");
        requireNear(genericRouter.getLatestByIndex(700021, 1160)->value, -1.0, 0.0001, "safeDivide zero fallback mismatch");
        requireNear(genericRouter.getLatestByIndex(700015, 1160)->value, 10.0, 0.0001, "window first sample mismatch");
        genericSeedEngine.set(700001, 20.0, 1161);
        modularMathEngine.runOnce(1161);
        requireNear(genericRouter.getLatestByIndex(700015, 1161)->value, 15.0, 0.0001, "window second sample mismatch");
        genericSeedEngine.set(700001, 40.0, 1162);
        modularMathEngine.runOnce(1162);
        requireNear(genericRouter.getLatestByIndex(700015, 1162)->value, 70.0 / 3.0, 0.0001, "window third sample mismatch");
        genericSeedEngine.set(700001, 80.0, 1163);
        modularMathEngine.runOnce(1163);
        requireNear(genericRouter.getLatestByIndex(700015, 1163)->value, 140.0 / 3.0, 0.0001, "window rolling sample mismatch");

        writeTextFile(
            "graph_ems_schedule_select_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "schedule_select",
  "nodes": [
    {
      "id": "clock_hour",
      "type": "timeSource",
      "params": {
        "component": "hour",
        "outputIndex": 700022
      }
    },
    {
      "id": "clock_minute",
      "type": "timeSource",
      "params": { "component": "minute", "outputIndex": 700023 }
    },
    {
      "id": "clock_second",
      "type": "timeSource",
      "params": { "component": "second", "outputIndex": 700024 }
    },
    {
      "id": "clock_minute_of_day",
      "type": "timeSource",
      "params": { "component": "minuteOfDay", "outputIndex": 700025 }
    },
    {
      "id": "clock_weekday",
      "type": "timeSource",
      "params": { "component": "weekday", "outputIndex": 700026 }
    },
    {
      "id": "clock_day_of_month",
      "type": "timeSource",
      "params": { "component": "dayOfMonth", "outputIndex": 700021 }
    },
    {
      "id": "day_schedule",
      "type": "scheduleSelect",
      "params": {
        "scheduleCurve": [
          { "hour": 0, "power": 0, "targetSoc": 70, "mode": 0 },
          { "hour": 5, "power": 45, "targetSoc": 80, "mode": 1 },
          { "hour": 6, "powerIndex": 700027, "targetSocIndex": 700028, "modeIndex": 700029 }
        ],
        "powerOutputIndex": 700017,
        "socOutputIndex": 700018,
        "modeOutputIndex": 700019,
        "enableMaskIndexes": [700030, 700031],
        "enableOutputIndex": 700020
      }
    }
  ],
  "edges": []
})json"
        );
        addRouteIfMissing(genericRouter, 700017, "SCHEDULE_POWER", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700018, "SCHEDULE_SOC", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700019, "SCHEDULE_MODE", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700020, "SCHEDULE_ENABLE", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700021, "CLOCK_DAY_OF_MONTH", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700022, "CLOCK_HOUR", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700023, "CLOCK_MINUTE", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700024, "CLOCK_SECOND", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700025, "CLOCK_MINUTE_OF_DAY", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700026, "CLOCK_WEEKDAY", genericConfig.memoryStore.sharedMemoryName, false);
        for (std::uint32_t index = 700027; index <= 700031; ++index) {
            addRouteIfMissing(genericRouter, index, "SCHEDULE_SOURCE_" + std::to_string(index), genericConfig.memoryStore.sharedMemoryName, false);
        }
        edge_gateway::GraphEmsEngine scheduleSelectEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_schedule_select_test.json"),
            genericRouter,
            600000
        );
        setTestTimezone("UTC");
        const auto timeSourceTestMs = 5LL * 3600000LL + 6LL * 60000LL + 7LL * 1000LL;
        const auto scheduleSelectResult = scheduleSelectEngine.runOnce(timeSourceTestMs);
        require(scheduleSelectResult.errors.empty(), "scheduleSelect should execute without errors");
        requireNear(genericRouter.getLatestByIndex(700017, timeSourceTestMs)->value, 45.0, 0.0001, "schedule power mismatch");
        requireNear(genericRouter.getLatestByIndex(700018, timeSourceTestMs)->value, 80.0, 0.0001, "schedule SOC mismatch");
        requireNear(genericRouter.getLatestByIndex(700019, timeSourceTestMs)->value, 1.0, 0.0001, "schedule mode mismatch");
        requireNear(genericRouter.getLatestByIndex(700022, timeSourceTestMs)->value, 5.0, 0.0001, "timeSource hour mismatch");
        requireNear(genericRouter.getLatestByIndex(700023, timeSourceTestMs)->value, 6.0, 0.0001, "timeSource minute mismatch");
        requireNear(genericRouter.getLatestByIndex(700024, timeSourceTestMs)->value, 7.0, 0.0001, "timeSource second mismatch");
        requireNear(genericRouter.getLatestByIndex(700025, timeSourceTestMs)->value, 306.0, 0.0001, "timeSource minuteOfDay mismatch");
        requireNear(genericRouter.getLatestByIndex(700026, timeSourceTestMs)->value, 4.0, 0.0001, "timeSource weekday mismatch");
        requireNear(genericRouter.getLatestByIndex(700021, timeSourceTestMs)->value, 1.0, 0.0001, "timeSource day-of-month mismatch");

        const auto indexedScheduleMs = 6LL * 3600000LL;
        genericSeedEngine.set(700027, 123.0, indexedScheduleMs);
        genericSeedEngine.set(700028, 88.0, indexedScheduleMs);
        genericSeedEngine.set(700029, 2.0, indexedScheduleMs);
        genericSeedEngine.set(700030, 1U << 6U, indexedScheduleMs);
        genericSeedEngine.set(700031, 0.0, indexedScheduleMs);
        const auto indexedScheduleResult = scheduleSelectEngine.runOnce(indexedScheduleMs);
        require(indexedScheduleResult.errors.empty(), "indexed scheduleSelect should execute without errors");
        requireNear(genericRouter.getLatestByIndex(700017, indexedScheduleMs)->value, 123.0, 0.0001, "indexed schedule power mismatch");
        requireNear(genericRouter.getLatestByIndex(700018, indexedScheduleMs)->value, 88.0, 0.0001, "indexed schedule SOC mismatch");
        requireNear(genericRouter.getLatestByIndex(700019, indexedScheduleMs)->value, 2.0, 0.0001, "indexed schedule mode mismatch");
        requireNear(genericRouter.getLatestByIndex(700020, indexedScheduleMs)->value, 1.0, 0.0001, "indexed schedule enable mask mismatch");
        genericSeedEngine.set(700030, 0.0, indexedScheduleMs + 1000);
        const auto disabledScheduleResult = scheduleSelectEngine.runOnce(indexedScheduleMs + 1000);
        require(disabledScheduleResult.errors.empty(), "disabled indexed scheduleSelect should execute without errors");
        requireNear(genericRouter.getLatestByIndex(700020, indexedScheduleMs + 1000)->value, 0.0, 0.0001, "indexed schedule enable should clear");

        writeTextFile(
            "graph_ems_modular_power_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "modular_power",
  "nodes": [
    {
      "id": "arbiter",
      "type": "phaseArbiter",
      "params": {
        "activeBaseIndexes": [700101, 700102, 700103],
        "reactiveBaseIndexes": [700104, 700105, 700106],
        "activeOutputIndexes": [700121, 700122, 700123],
        "reactiveOutputIndexes": [700124, 700125, 700126],
        "candidates": [
          { "name": "charge", "target": "active", "merge": "stronger", "direction": "positive", "totalIndex": 700107, "runOutputIndex": 700111 },
          { "name": "balance", "target": "active", "merge": "add", "direction": "any", "indexes": [700108, 700109, 700110], "runOutputIndex": 700112 }
        ]
      }
    },
    {
      "id": "constraints",
      "type": "powerConstraint",
      "params": {
        "activeInputIndexes": [700121, 700122, 700123],
        "reactiveInputIndexes": [700124, 700125, 700126],
        "activeOutputIndexes": [700141, 700142, 700143],
        "reactiveOutputIndexes": [700144, 700145, 700146],
        "activeAbsLimitIndex": 700130,
        "reactiveAbsLimitIndex": 700131,
        "apparentTotalLimitIndex": 700132,
        "positiveTotalLimitIndex": 700133,
        "negativeTotalLimitIndex": 700134,
        "stateIndex": 700135,
        "stateUpperIndex": 700136,
        "stateLowerIndex": 700137
      }
    }
  ],
  "edges": [{ "from": "arbiter", "to": "constraints" }]
})json"
        );
        for (std::uint32_t index = 700101; index <= 700146; ++index) {
            addRouteIfMissing(genericRouter, index, "MODULAR_POWER_" + std::to_string(index), genericConfig.memoryStore.sharedMemoryName, false);
        }
        genericSeedEngine.set(700101, 10.0, 19000000);
        genericSeedEngine.set(700102, 5.0, 19000000);
        genericSeedEngine.set(700103, 15.0, 19000000);
        genericSeedEngine.set(700104, 2.0, 19000000);
        genericSeedEngine.set(700105, 3.0, 19000000);
        genericSeedEngine.set(700106, 4.0, 19000000);
        genericSeedEngine.set(700107, 60.0, 19000000);
        genericSeedEngine.set(700108, -2.0, 19000000);
        genericSeedEngine.set(700109, 3.0, 19000000);
        genericSeedEngine.set(700110, 0.0, 19000000);
        genericSeedEngine.set(700130, 20.0, 19000000);
        genericSeedEngine.set(700131, 10.0, 19000000);
        genericSeedEngine.set(700132, 300.0, 19000000);
        genericSeedEngine.set(700133, 30.0, 19000000);
        genericSeedEngine.set(700134, 30.0, 19000000);
        genericSeedEngine.set(700135, 50.0, 19000000);
        genericSeedEngine.set(700136, 95.0, 19000000);
        genericSeedEngine.set(700137, 10.0, 19000000);
        edge_gateway::GraphEmsEngine modularPowerEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_modular_power_test.json"),
            genericRouter,
            600000
        );
        const auto modularPowerResult = modularPowerEngine.runOnce(19000000);
        require(modularPowerResult.errors.empty(), "modular power chain should execute without errors");
        requireNear(genericRouter.getLatestByIndex(700141, 19000000)->value, 18.0 * 30.0 / 58.0, 0.0001, "modular PA limit mismatch");
        requireNear(genericRouter.getLatestByIndex(700142, 19000000)->value, 20.0 * 30.0 / 58.0, 0.0001, "modular PB limit mismatch");
        requireNear(genericRouter.getLatestByIndex(700143, 19000000)->value, 20.0 * 30.0 / 58.0, 0.0001, "modular PC limit mismatch");
        requireNear(genericRouter.getLatestByIndex(700144, 19000000)->value, 2.0, 0.0001, "modular QA mismatch");
        requireNear(genericRouter.getLatestByIndex(700111, 19000000)->value, 1.0, 0.0001, "charge candidate run feedback mismatch");
        requireNear(genericRouter.getLatestByIndex(700112, 19000000)->value, 1.0, 0.0001, "balance candidate run feedback mismatch");
        genericSeedEngine.set(700107, 0.0, 19001000);
        genericSeedEngine.set(700108, 0.0, 19001000);
        genericSeedEngine.set(700109, 0.0, 19001000);
        genericSeedEngine.set(700110, 0.0, 19001000);
        const auto idleModularPowerResult = modularPowerEngine.runOnce(19001000);
        require(idleModularPowerResult.errors.empty(), "idle modular power chain should execute without errors");
        requireNear(genericRouter.getLatestByIndex(700111, 19001000)->value, 0.0, 0.0001, "charge candidate run feedback should clear");
        requireNear(genericRouter.getLatestByIndex(700112, 19001000)->value, 0.0, 0.0001, "balance candidate run feedback should clear");

        writeTextFile(
            "graph_ems_division_by_zero_test.json",
            R"json({
  "schemaVersion": "1.1.0",
  "graphCode": "division_by_zero",
  "nodes": [
    {
      "id": "unsafe_divide",
      "type": "formula",
      "params": {
        "operation": "divide",
        "inputs": [
          { "value": 10 },
          { "value": 0 }
        ],
        "outputIndex": 700020
      }
    }
  ],
  "edges": []
})json"
        );
        addRouteIfMissing(genericRouter, 700150, "GENERIC_DIVIDE_OUTPUT", genericConfig.memoryStore.sharedMemoryName, false);
        edge_gateway::GraphEmsEngine divisionByZeroEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_division_by_zero_test.json"),
            genericRouter,
            600000
        );
        const auto divisionByZeroResult = divisionByZeroEngine.runOnce(1156);
        require(divisionByZeroResult.errors.size() == 1, "formula division by zero should report one node error");
        require(
            divisionByZeroResult.errors[0].find("formula division by zero") != std::string::npos,
            "formula division by zero error message mismatch"
        );
        require(!genericRouter.getLatestByIndex(700150, 1156), "formula division by zero must not produce output");

        writeTextFile(
            "graph_ems_feedback_verify_test.json",
            R"json({
  "schemaVersion": "1.2.0",
  "graphCode": "feedback_verify",
  "nodes": [
    {
      "id": "pcs_active_power_feedback",
      "type": "feedbackVerify",
      "params": {
        "targetIndex": 700030,
        "feedbackIndex": 700031,
        "tolerance": 0.5,
        "targetChangeTolerance": 0.01,
        "timeoutMs": 1000,
        "outputIndex": 700032
      }
    }
  ],
  "edges": []
})json"
        );
        addRouteIfMissing(genericRouter, 700030, "PCS_POWER_TARGET", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700031, "PCS_POWER_FEEDBACK", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700032, "PCS_POWER_VERIFY_STATUS", genericConfig.memoryStore.sharedMemoryName, false);
        const std::string feedbackStateFile = "graph_ems_feedback_verify_state_test.json";
        removeFileIfExists(feedbackStateFile);
        genericSeedEngine.set(700030, 10.0, 1200);
        genericSeedEngine.set(700031, 8.0, 1200);
        edge_gateway::GraphEmsEngine feedbackVerifyEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_feedback_verify_test.json"),
            genericRouter,
            600000,
            feedbackStateFile,
            {{"stateSaveIntervalMs", "0"}}
        );
        const auto feedbackWaitingResult = feedbackVerifyEngine.runOnce(1200);
        require(feedbackWaitingResult.errors.empty(), "feedback verify waiting run should not report errors");
        requireNear(
            genericRouter.getLatestByIndex(700032, 1200)->value,
            0.0,
            0.0001,
            "feedback verify should wait before timeout"
        );

        genericSeedEngine.set(700031, 9.6, 1300);
        feedbackVerifyEngine.runOnce(1300);
        requireNear(
            genericRouter.getLatestByIndex(700032, 1300)->value,
            1.0,
            0.0001,
            "feedback verify should pass within tolerance"
        );

        genericSeedEngine.set(700030, 12.0, 1400);
        feedbackVerifyEngine.runOnce(1400);
        requireNear(
            genericRouter.getLatestByIndex(700032, 1400)->value,
            0.0,
            0.0001,
            "feedback verify target change should reset timeout"
        );
        feedbackVerifyEngine.runOnce(1800);
        edge_gateway::GraphEmsEngine restartedFeedbackVerifyEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_feedback_verify_test.json"),
            genericRouter,
            600000,
            feedbackStateFile,
            {{"stateSaveIntervalMs", "0"}}
        );
        restartedFeedbackVerifyEngine.runOnce(5000);
        requireNear(
            genericRouter.getLatestByIndex(700032, 5000)->value,
            0.0,
            0.0001,
            "feedback verify restart should preserve elapsed waiting time"
        );
        restartedFeedbackVerifyEngine.runOnce(5599);
        requireNear(
            genericRouter.getLatestByIndex(700032, 5599)->value,
            0.0,
            0.0001,
            "feedback verify restart should retain the remaining timeout"
        );
        restartedFeedbackVerifyEngine.runOnce(5600);
        requireNear(
            genericRouter.getLatestByIndex(700032, 5600)->value,
            -1.0,
            0.0001,
            "feedback verify restart should report timeout after the remaining duration"
        );

        genericSeedEngine.set(700031, 12.0, 5601);
        restartedFeedbackVerifyEngine.runOnce(5601);
        requireNear(
            genericRouter.getLatestByIndex(700032, 5601)->value,
            1.0,
            0.0001,
            "feedback verify should recover after feedback reaches target"
        );
        restartedFeedbackVerifyEngine.runOnce(605602);
        requireNear(
            genericRouter.getLatestByIndex(700032, 605602)->value,
            -2.0,
            0.0001,
            "feedback verify stale inputs should report invalid status"
        );

        writeTextFile(
            "graph_ems_control_write_test.json",
            R"json({
  "schemaVersion": "1.2.0",
  "graphCode": "control_write",
  "nodes": [
    {
      "id": "generic_control_write",
      "type": "controlWrite",
      "params": {
        "submitWrites": true,
        "inputIndex": 700034,
        "targetIndex": 700035,
        "minValue": -50,
        "maxValue": 50,
        "deadband": 0.1,
        "permitIndex": 700033,
        "permitValue": 1,
        "valueMode": "truncate",
        "highPriority": true
      }
    }
  ],
  "edges": []
})json"
        );
        addRouteIfMissing(genericRouter, 700033, "GENERIC_CONTROL_PERMIT", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700034, "GENERIC_CONTROL_INPUT", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700035, "GENERIC_CONTROL_TARGET", genericConfig.memoryStore.sharedMemoryName, true);
        genericSeedEngine.set(700033, 0.0, 602500);
        genericSeedEngine.set(700034, 25.8, 602500);
        edge_gateway::GraphEmsEngine controlWriteEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_control_write_test.json"),
            genericRouter,
            600000
        );
        const auto deniedGenericWrite = controlWriteEngine.runOnce(602500);
        require(deniedGenericWrite.errors.empty(), "denied generic control write should not report errors");
        require(deniedGenericWrite.deviceWrites == 0, "permit must block generic control write");
        require(genericRouter.peekPendingWrites(8).empty(), "denied generic control write must not enter queue");

        genericSeedEngine.set(700033, 1.0, 602501);
        const auto acceptedGenericWrite = controlWriteEngine.runOnce(602501);
        const auto genericPendingWrites = genericRouter.peekPendingWrites(8);
        require(acceptedGenericWrite.errors.empty(), "accepted generic control write should not report errors");
        require(acceptedGenericWrite.deviceWrites == 1, "generic control write should submit one command");
        require(genericPendingWrites.size() == 1, "generic control write queue size mismatch");
        require(genericPendingWrites[0].index == 700035, "generic control write target index mismatch");
        requireNear(genericPendingWrites[0].value, 25.0, 0.0001, "generic control write target truncation mismatch");
        require(genericPendingWrites[0].highPriority, "generic control write high priority flag missing");

        const auto duplicateGenericWrite = controlWriteEngine.runOnce(602502);
        require(duplicateGenericWrite.deviceWrites == 0, "duplicate generic control write must be skipped");
        require(genericRouter.peekPendingWrites(8).size() == 1, "duplicate generic write must not grow queue");

        genericSeedEngine.set(700034, 60.0, 602503);
        const auto outOfRangeGenericWrite = controlWriteEngine.runOnce(602503);
        require(outOfRangeGenericWrite.errors.size() == 1, "out-of-range generic control write should report error");
        require(
            outOfRangeGenericWrite.errors[0].find("outside configured bounds") != std::string::npos,
            "out-of-range generic control write error mismatch"
        );
        require(genericRouter.peekPendingWrites(8).size() == 1, "out-of-range generic write must not enter queue");

        writeTextFile(
            "graph_ems_rate_limit_test.json",
            R"json({
  "schemaVersion": "1.2.0",
  "graphCode": "rate_limit",
  "nodes": [
    {
      "id": "pcs_power_rate_limit",
      "type": "rateLimit",
      "params": {
        "inputIndex": 700036,
        "outputIndex": 700037,
        "risePerSecond": 10,
        "fallPerSecond": 20,
        "minValue": -50,
        "maxValue": 50,
        "initialValue": 0
      }
    }
  ],
  "edges": []
})json"
        );
        addRouteIfMissing(genericRouter, 700036, "RATE_LIMIT_INPUT", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700037, "RATE_LIMIT_OUTPUT", genericConfig.memoryStore.sharedMemoryName, false);
        genericSeedEngine.set(700036, 30.0, 603000);
        edge_gateway::GraphEmsEngine rateLimitEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_rate_limit_test.json"),
            genericRouter,
            600000
        );
        rateLimitEngine.runOnce(603000);
        requireNear(
            genericRouter.getLatestByIndex(700037, 603000)->value,
            0.0,
            0.0001,
            "rate limit first run should use initial value"
        );
        rateLimitEngine.runOnce(603500);
        requireNear(
            genericRouter.getLatestByIndex(700037, 603500)->value,
            5.0,
            0.0001,
            "rate limit rise after half second mismatch"
        );
        rateLimitEngine.runOnce(604500);
        requireNear(
            genericRouter.getLatestByIndex(700037, 604500)->value,
            15.0,
            0.0001,
            "rate limit rise after one second mismatch"
        );

        genericSeedEngine.set(700036, -30.0, 604500);
        rateLimitEngine.runOnce(604500);
        requireNear(
            genericRouter.getLatestByIndex(700037, 604500)->value,
            15.0,
            0.0001,
            "rate limit must not move when no time elapsed"
        );
        rateLimitEngine.runOnce(605000);
        requireNear(
            genericRouter.getLatestByIndex(700037, 605000)->value,
            5.0,
            0.0001,
            "rate limit fall after half second mismatch"
        );
        rateLimitEngine.runOnce(607000);
        requireNear(
            genericRouter.getLatestByIndex(700037, 607000)->value,
            -30.0,
            0.0001,
            "rate limit fall should stop at target"
        );

        genericSeedEngine.set(700036, 60.0, 607001);
        const auto outOfRangeRateLimit = rateLimitEngine.runOnce(607001);
        require(outOfRangeRateLimit.errors.size() == 1, "out-of-range rate limit input should report error");
        require(
            outOfRangeRateLimit.errors[0].find("outside configured bounds") != std::string::npos,
            "out-of-range rate limit error mismatch"
        );

        genericSeedEngine.set(700036, 30.0, 607100);
        edge_gateway::GraphEmsEngine restartedRateLimitEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_rate_limit_test.json"),
            genericRouter,
            600000
        );
        restartedRateLimitEngine.runOnce(607100);
        requireNear(
            genericRouter.getLatestByIndex(700037, 607100)->value,
            -30.0,
            0.0001,
            "restarted rate limit should resume existing output"
        );
        restartedRateLimitEngine.runOnce(607600);
        requireNear(
            genericRouter.getLatestByIndex(700037, 607600)->value,
            -25.0,
            0.0001,
            "restarted rate limit rise mismatch"
        );

        writeTextFile(
            "graph_ems_boolean_filters_test.json",
            R"json({
  "schemaVersion": "1.2.0",
  "graphCode": "boolean_filters",
  "nodes": [
    {
      "id": "soc_hysteresis",
      "type": "hysteresis",
      "params": {
        "inputIndex": 700038,
        "outputIndex": 700039,
        "lowThreshold": 20,
        "highThreshold": 30,
        "initialState": false
      }
    },
    {
      "id": "remote_mode_debounce",
      "type": "debounce",
      "params": {
        "inputIndex": 700043,
        "outputIndex": 700044,
        "onDelayMs": 100,
        "offDelayMs": 200,
        "initialState": false
      }
    },
    {
      "id": "inverted_hysteresis_fail_safe",
      "type": "hysteresis",
      "params": {
        "inputIndex": 700045,
        "outputIndex": 700046,
        "lowThreshold": 20,
        "highThreshold": 30,
        "initialState": false,
        "invert": true
      }
    },
    {
      "id": "inverted_debounce_fail_safe",
      "type": "debounce",
      "params": {
        "inputIndex": 700047,
        "outputIndex": 700048,
        "onDelayMs": 0,
        "offDelayMs": 0,
        "initialState": false,
        "invert": true
      }
    }
  ],
  "edges": []
})json"
        );
        addRouteIfMissing(genericRouter, 700038, "HYSTERESIS_INPUT", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700039, "HYSTERESIS_OUTPUT", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700043, "DEBOUNCE_INPUT", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700044, "DEBOUNCE_OUTPUT", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700045, "INVERTED_HYSTERESIS_INPUT", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700046, "INVERTED_HYSTERESIS_OUTPUT", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700047, "INVERTED_DEBOUNCE_INPUT", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700048, "INVERTED_DEBOUNCE_OUTPUT", genericConfig.memoryStore.sharedMemoryName, false);
        const std::string booleanFilterStateFile = "graph_ems_boolean_filters_state_test.json";
        removeFileIfExists(booleanFilterStateFile);
        genericSeedEngine.set(700038, 25.0, 608000);
        genericSeedEngine.set(700043, 0.0, 608000);
        edge_gateway::GraphEmsEngine booleanFilterEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_boolean_filters_test.json"),
            genericRouter,
            600000,
            booleanFilterStateFile,
            {{"stateSaveIntervalMs", "0"}}
        );
        booleanFilterEngine.runOnce(608000);
        requireNear(genericRouter.getLatestByIndex(700039, 608000)->value, 0.0, 0.0001, "hysteresis initial state mismatch");
        requireNear(
            genericRouter.getLatestByIndex(700046, 608000)->value,
            0.0,
            0.0001,
            "inverted hysteresis must fail safe to zero when input is missing"
        );
        requireNear(
            genericRouter.getLatestByIndex(700048, 608000)->value,
            0.0,
            0.0001,
            "inverted debounce must fail safe to zero when input is missing"
        );
        genericSeedEngine.set(700038, 35.0, 608001);
        booleanFilterEngine.runOnce(608001);
        requireNear(genericRouter.getLatestByIndex(700039, 608001)->value, 1.0, 0.0001, "hysteresis high threshold mismatch");
        genericSeedEngine.set(700038, 25.0, 608002);
        booleanFilterEngine.runOnce(608002);
        requireNear(genericRouter.getLatestByIndex(700039, 608002)->value, 1.0, 0.0001, "hysteresis band should retain state");
        genericSeedEngine.set(700038, 15.0, 608003);
        booleanFilterEngine.runOnce(608003);
        requireNear(genericRouter.getLatestByIndex(700039, 608003)->value, 0.0, 0.0001, "hysteresis low threshold mismatch");

        genericSeedEngine.set(700043, 1.0, 608010);
        booleanFilterEngine.runOnce(608010);
        booleanFilterEngine.runOnce(608060);
        edge_gateway::GraphEmsEngine restartedBooleanFilterEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_boolean_filters_test.json"),
            genericRouter,
            600000,
            booleanFilterStateFile,
            {{"stateSaveIntervalMs", "0"}}
        );
        restartedBooleanFilterEngine.runOnce(700000);
        restartedBooleanFilterEngine.runOnce(700049);
        requireNear(genericRouter.getLatestByIndex(700044, 700049)->value, 0.0, 0.0001, "debounce on delay should hold false");
        restartedBooleanFilterEngine.runOnce(700050);
        requireNear(genericRouter.getLatestByIndex(700044, 700050)->value, 1.0, 0.0001, "debounce restart should retain on-delay progress");
        genericSeedEngine.set(700043, 0.0, 700060);
        restartedBooleanFilterEngine.runOnce(700060);
        restartedBooleanFilterEngine.runOnce(700160);
        edge_gateway::GraphEmsEngine secondRestartedBooleanFilterEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_boolean_filters_test.json"),
            genericRouter,
            600000,
            booleanFilterStateFile,
            {{"stateSaveIntervalMs", "0"}}
        );
        secondRestartedBooleanFilterEngine.runOnce(800000);
        secondRestartedBooleanFilterEngine.runOnce(800099);
        requireNear(genericRouter.getLatestByIndex(700044, 800099)->value, 1.0, 0.0001, "debounce restart should retain off-delay progress");
        secondRestartedBooleanFilterEngine.runOnce(800100);
        requireNear(genericRouter.getLatestByIndex(700044, 800100)->value, 0.0, 0.0001, "debounce restart off-delay mismatch");

        writeTextFile(
            "graph_ems_sequence_test.json",
            R"json({
  "schemaVersion": "1.3.0",
  "graphCode": "sequence",
  "nodes": [
    {
      "id": "charge_discharge_sequence",
      "type": "sequence",
      "params": {
        "initialState": 0,
        "stateOutputIndex": 700047,
        "states": [
          { "id": 0, "name": "待机" },
          { "id": 1, "name": "放电" },
          { "id": 2, "name": "充电" },
          { "id": 3, "name": "完成" }
        ],
        "transitions": [
          {
            "from": 0,
            "to": 1,
            "name": "启动",
            "minDurationMs": 100,
            "conditions": [{ "index": 700045, "operator": "eq", "value": 1 }]
          },
          {
            "from": 1,
            "to": 2,
            "name": "放电到下限",
            "minDurationMs": 200,
            "conditions": [{ "index": 700046, "operator": "lte", "value": 20 }]
          },
          {
            "from": 2,
            "to": 3,
            "name": "充电到上限",
            "conditions": [{ "index": 700046, "operator": "gte", "value": 95 }]
          }
        ]
      }
    }
  ],
  "edges": []
})json"
        );
        addRouteIfMissing(genericRouter, 700045, "SEQUENCE_START", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700046, "SEQUENCE_SOC", genericConfig.memoryStore.sharedMemoryName, false);
        addRouteIfMissing(genericRouter, 700047, "SEQUENCE_STATE", genericConfig.memoryStore.sharedMemoryName, false);
        const std::string sequenceStateFile = "graph_ems_sequence_state_test.json";
        removeFileIfExists(sequenceStateFile);
        genericSeedEngine.set(700045, 1.0, 609000);
        genericSeedEngine.set(700046, 50.0, 609000);
        edge_gateway::GraphEmsEngine sequenceEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_sequence_test.json"),
            genericRouter,
            600000,
            sequenceStateFile,
            {{"stateSaveIntervalMs", "0"}}
        );
        sequenceEngine.runOnce(609000);
        requireNear(genericRouter.getLatestByIndex(700047, 609000)->value, 0.0, 0.0001, "sequence initial state mismatch");
        sequenceEngine.runOnce(609099);
        requireNear(genericRouter.getLatestByIndex(700047, 609099)->value, 0.0, 0.0001, "sequence minimum duration should hold idle");
        sequenceEngine.runOnce(609100);
        requireNear(genericRouter.getLatestByIndex(700047, 609100)->value, 1.0, 0.0001, "sequence start transition mismatch");

        genericSeedEngine.set(700046, 20.0, 609200);
        sequenceEngine.runOnce(609250);
        edge_gateway::GraphEmsEngine restartedSequenceEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_sequence_test.json"),
            genericRouter,
            600000,
            sequenceStateFile,
            {{"stateSaveIntervalMs", "0"}}
        );
        restartedSequenceEngine.runOnce(710049);
        requireNear(genericRouter.getLatestByIndex(700047, 710049)->value, 1.0, 0.0001, "sequence restart should retain minimum-duration progress");
        restartedSequenceEngine.runOnce(710099);
        requireNear(genericRouter.getLatestByIndex(700047, 710099)->value, 2.0, 0.0001, "sequence restart transition mismatch");
        genericSeedEngine.set(700046, 95.0, 710100);
        restartedSequenceEngine.runOnce(710100);
        requireNear(genericRouter.getLatestByIndex(700047, 710100)->value, 3.0, 0.0001, "sequence completion transition mismatch");

        const auto graphServiceConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_service");
        edge_gateway::PointStoreRouter graphServiceRouter;
        cleanupStoreSegment(graphServiceConfig.memoryStore);
        edge_gateway::MemoryPointStore graphServiceStore(graphServiceConfig.memoryStore);
        graphServiceRouter.addStore(graphServiceConfig.memoryStore.sharedMemoryName, graphServiceStore);
        graphServiceRouter.addRoutesFromDeviceConfigs(
            {graphServiceConfig},
            graphServiceConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine graphServiceSeedEngine(runtimeCatalog, graphServiceRouter);
        graphServiceSeedEngine.set(156, 2.0, 1160);
        graphServiceSeedEngine.set(1036, 42.0, 1160);

        const std::string graphServiceV2File = "graph_ems_meter_average_v2_test.logic.json";
        writeMeterAverageV2Graph(
            graphServiceV2File,
            "meter-average-v2-service",
            {MeterAverageV2Spec("meter_average", std::string(), std::string(), 209)}
        );

        edge_gateway::ComputeRuleConfig graphServiceRule;
        graphServiceRule.ruleCode = "graph_ems_service_rule";
        graphServiceRule.enabled = true;
        graphServiceRule.trigger.type = "interval";
        graphServiceRule.trigger.intervalMs = 1;
        graphServiceRule.script.type = "graphEms";
        graphServiceRule.script.graphFile = graphServiceV2File;
        graphServiceRule.script.graphStateFile = "graph_ems_service_state_test.json";
        removeFileIfExists(graphServiceRule.script.graphStateFile);

        edge_gateway::ComputeEngineConfig graphServiceComputeConfig;
        graphServiceComputeConfig.enabled = true;
        graphServiceComputeConfig.scanIntervalMs = 1;
        graphServiceComputeConfig.defaultOutputTtlMs = 600000;
        graphServiceComputeConfig.rules.push_back(graphServiceRule);

        edge_gateway::ComputeEngineService graphService(graphServiceComputeConfig, graphServiceRouter);
        graphService.runOnce(1160);
        const auto graphServiceAvgPa = graphServiceRouter.getLatestByIndex(209, 1160);
        require(static_cast<bool>(graphServiceAvgPa), "ComputeEngine graphEms should write TQ_avg_PA");
        requireNear(graphServiceAvgPa->value, 42.0, 0.0001, "ComputeEngine graphEms output mismatch");
        removeFileIfExists(graphServiceV2File);

        const std::string graphProfileV2File = "graph_ems_profile_v2_test.logic.json";
        writeMeterAverageV2Graph(
            graphProfileV2File,
            "profile-v2",
            {MeterAverageV2Spec("tq_average", "Meter_TQ", std::string(), 209)}
        );
        const auto graphProfileConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_profile");
        edge_gateway::PointStoreRouter graphProfileRouter;
        cleanupStoreSegment(graphProfileConfig.memoryStore);
        edge_gateway::MemoryPointStore graphProfileStore(graphProfileConfig.memoryStore);
        graphProfileRouter.addStore(graphProfileConfig.memoryStore.sharedMemoryName, graphProfileStore);
        graphProfileRouter.addRoutesFromDeviceConfigs(
            {graphProfileConfig},
            graphProfileConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine graphProfileSeedEngine(runtimeCatalog, graphProfileRouter);
        graphProfileSeedEngine.set(156, 2.0, 1165);
        graphProfileSeedEngine.set(1036, 42.0, 1165);

        edge_gateway::ComputeRuleConfig graphProfileRule;
        graphProfileRule.ruleCode = "graph_ems_profile_rule";
        graphProfileRule.enabled = true;
        graphProfileRule.trigger.type = "interval";
        graphProfileRule.trigger.intervalMs = 1;
        graphProfileRule.script.type = "graphEms";
        graphProfileRule.script.graphFile = graphProfileV2File;
        graphProfileRule.script.graphStateFile = "graph_ems_profile_state_test.json";
        removeFileIfExists(graphProfileRule.script.graphStateFile);
        graphProfileRule.script.graphProfile = {{"Meter_TQ", "0"}};

        edge_gateway::ComputeEngineConfig graphProfileComputeConfig;
        graphProfileComputeConfig.enabled = true;
        graphProfileComputeConfig.scanIntervalMs = 1;
        graphProfileComputeConfig.defaultOutputTtlMs = 600000;
        graphProfileComputeConfig.rules.push_back(graphProfileRule);

        edge_gateway::ComputeEngineService graphProfileService(graphProfileComputeConfig, graphProfileRouter);
        graphProfileService.runOnce(1165);
        const auto graphProfileAvgPa = graphProfileRouter.getLatestByIndex(209, 1165);
        require(
            !static_cast<bool>(graphProfileAvgPa) || graphProfileAvgPa->ts != 1165,
            "ComputeEngine graphProfile Meter_TQ=0 should skip TQ average"
        );
        removeFileIfExists(graphProfileV2File);

        const std::string graphOptionalV2File = "graph_ems_optional_profile_v2_test.logic.json";
        writeMeterAverageV2Graph(
            graphOptionalV2File,
            "optional-profile-v2",
            {
                MeterAverageV2Spec("required_model_node", "PCS_MODEL", std::string(), 201),
                MeterAverageV2Spec("optional_ups_node", std::string(), "UPS_MODEL", 209),
                MeterAverageV2Spec(
                    "optional_dehumidifier_node",
                    std::string(),
                    "DEHUMIDIFIER_MODEL",
                    210
                )
            }
        );
        const auto graphOptionalConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_optional");
        edge_gateway::PointStoreRouter graphOptionalRouter;
        cleanupStoreSegment(graphOptionalConfig.memoryStore);
        edge_gateway::MemoryPointStore graphOptionalStore(graphOptionalConfig.memoryStore);
        graphOptionalRouter.addStore(graphOptionalConfig.memoryStore.sharedMemoryName, graphOptionalStore);
        graphOptionalRouter.addRoutesFromDeviceConfigs(
            {graphOptionalConfig},
            graphOptionalConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine graphOptionalSeedEngine(runtimeCatalog, graphOptionalRouter);
        graphOptionalSeedEngine.set(156, 2.0, 1167);
        graphOptionalSeedEngine.set(1036, 42.0, 1167);

        edge_gateway::ComputeRuleConfig graphOptionalRule;
        graphOptionalRule.ruleCode = "graph_ems_optional_rule";
        graphOptionalRule.enabled = true;
        graphOptionalRule.trigger.type = "interval";
        graphOptionalRule.trigger.intervalMs = 1;
        graphOptionalRule.script.type = "graphEms";
        graphOptionalRule.script.graphFile = graphOptionalV2File;
        graphOptionalRule.script.graphStateFile = "graph_ems_optional_state_test.json";
        removeFileIfExists(graphOptionalRule.script.graphStateFile);
        graphOptionalRule.script.graphProfile = {{"PCS_MODEL", "3"}};

        edge_gateway::ComputeEngineConfig graphOptionalComputeConfig;
        graphOptionalComputeConfig.enabled = true;
        graphOptionalComputeConfig.scanIntervalMs = 1;
        graphOptionalComputeConfig.defaultOutputTtlMs = 600000;
        graphOptionalComputeConfig.rules.push_back(graphOptionalRule);

        edge_gateway::ComputeEngineService graphOptionalService(graphOptionalComputeConfig, graphOptionalRouter);
        graphOptionalService.runOnce(1167);
        const auto graphRequiredModelAvgPa = graphOptionalRouter.getLatestByIndex(201, 1167);
        const auto graphOptionalAvgPa = graphOptionalRouter.getLatestByIndex(209, 1167);
        const auto graphOptionalDehumidifierAvgPa = graphOptionalRouter.getLatestByIndex(210, 1167);
        require(static_cast<bool>(graphRequiredModelAvgPa), "numeric profileKey PCS_MODEL=3 should run node");
        requireNear(graphRequiredModelAvgPa->value, 42.0, 0.0001, "numeric profileKey output mismatch");
        require(
            !static_cast<bool>(graphOptionalAvgPa) || graphOptionalAvgPa->ts != 1167,
            "optionalProfileKey should skip node when UPS_MODEL is missing"
        );
        require(
            !static_cast<bool>(graphOptionalDehumidifierAvgPa) || graphOptionalDehumidifierAvgPa->ts != 1167,
            "optionalProfileKey should skip node when DEHUMIDIFIER_MODEL is missing"
        );

        const auto graphOptionalEnabledConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_optional_on");
        edge_gateway::PointStoreRouter graphOptionalEnabledRouter;
        cleanupStoreSegment(graphOptionalEnabledConfig.memoryStore);
        edge_gateway::MemoryPointStore graphOptionalEnabledStore(graphOptionalEnabledConfig.memoryStore);
        graphOptionalEnabledRouter.addStore(
            graphOptionalEnabledConfig.memoryStore.sharedMemoryName,
            graphOptionalEnabledStore
        );
        graphOptionalEnabledRouter.addRoutesFromDeviceConfigs(
            {graphOptionalEnabledConfig},
            graphOptionalEnabledConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine graphOptionalEnabledSeedEngine(runtimeCatalog, graphOptionalEnabledRouter);
        graphOptionalEnabledSeedEngine.set(156, 2.0, 1168);
        graphOptionalEnabledSeedEngine.set(1036, 55.0, 1168);

        edge_gateway::ComputeRuleConfig graphOptionalEnabledRule = graphOptionalRule;
        graphOptionalEnabledRule.ruleCode = "graph_ems_optional_enabled_rule";
        graphOptionalEnabledRule.script.graphProfile = {{"UPS_MODEL", "1"}};
        graphOptionalEnabledRule.script.graphStateFile = "graph_ems_optional_enabled_state_test.json";
        removeFileIfExists(graphOptionalEnabledRule.script.graphStateFile);

        edge_gateway::ComputeEngineConfig graphOptionalEnabledComputeConfig;
        graphOptionalEnabledComputeConfig.enabled = true;
        graphOptionalEnabledComputeConfig.scanIntervalMs = 1;
        graphOptionalEnabledComputeConfig.defaultOutputTtlMs = 600000;
        graphOptionalEnabledComputeConfig.rules.push_back(graphOptionalEnabledRule);

        edge_gateway::ComputeEngineService graphOptionalEnabledService(
            graphOptionalEnabledComputeConfig,
            graphOptionalEnabledRouter
        );
        graphOptionalEnabledService.runOnce(1168);
        const auto graphOptionalEnabledAvgPa = graphOptionalEnabledRouter.getLatestByIndex(209, 1168);
        const auto graphOptionalDehumidifierDisabledAvgPa =
            graphOptionalEnabledRouter.getLatestByIndex(210, 1168);
        require(static_cast<bool>(graphOptionalEnabledAvgPa), "optionalProfileKey should run when UPS_MODEL=1");
        requireNear(graphOptionalEnabledAvgPa->value, 55.0, 0.0001, "optionalProfileKey enabled output mismatch");
        require(
            !static_cast<bool>(graphOptionalDehumidifierDisabledAvgPa) ||
                graphOptionalDehumidifierDisabledAvgPa->ts != 1168,
            "enabling UPS_MODEL should not enable other optional device nodes"
        );
        removeFileIfExists(graphOptionalV2File);

        writeTextFile(
            "graph_ems_tq_metrics_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "tq_metrics",
  "nodes": [
    {
      "id": "tq_average",
      "type": "meterAverage",
      "enabled": true,
      "params": {
        "windowSizeIndex": 156,
        "deriveTqMetrics": true,
        "mappings": [
          { "input": 1036, "output": 209 },
          { "input": 1037, "output": 210 },
          { "input": 1038, "output": 211 },
          { "input": 1039, "output": 212 },
          { "input": 1040, "output": 213 },
          { "input": 1041, "output": 214 },
          { "input": 1042, "output": 215 },
          { "input": 1043, "output": 216 }
        ]
      }
    }
  ],
  "edges": []
})json"
        );
        const auto graphTqMetricsConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_tq_metrics");
        edge_gateway::PointStoreRouter graphTqMetricsRouter;
        cleanupStoreSegment(graphTqMetricsConfig.memoryStore);
        edge_gateway::MemoryPointStore graphTqMetricsStore(graphTqMetricsConfig.memoryStore);
        graphTqMetricsRouter.addStore(graphTqMetricsConfig.memoryStore.sharedMemoryName, graphTqMetricsStore);
        graphTqMetricsRouter.addRoutesFromDeviceConfigs(
            {graphTqMetricsConfig},
            graphTqMetricsConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine graphTqMetricsSeedEngine(runtimeCatalog, graphTqMetricsRouter);
        graphTqMetricsSeedEngine.set(156, 2.0, 1166);
        graphTqMetricsSeedEngine.set(1036, 30.0, 1166);
        graphTqMetricsSeedEngine.set(1037, 20.0, 1166);
        graphTqMetricsSeedEngine.set(1038, 10.0, 1166);
        graphTqMetricsSeedEngine.set(1039, 60.0, 1166);
        graphTqMetricsSeedEngine.set(1040, 4.0, 1166);
        graphTqMetricsSeedEngine.set(1041, 3.0, 1166);
        graphTqMetricsSeedEngine.set(1042, 0.0, 1166);
        graphTqMetricsSeedEngine.set(1043, 7.0, 1166);
        edge_gateway::GraphEmsEngine graphTqMetricsEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_tq_metrics_test.json"),
            graphTqMetricsRouter,
            600000
        );
        graphTqMetricsEngine.runOnce(1166);
        const auto graphTqSa = graphTqMetricsRouter.getLatestByIndex(217, 1166);
        const auto graphTqCos3 = graphTqMetricsRouter.getLatestByIndex(224, 1166);
        const auto graphTqBalance = graphTqMetricsRouter.getLatestByIndex(225, 1166);
        require(static_cast<bool>(graphTqSa), "graph TQ SA metric missing");
        require(static_cast<bool>(graphTqCos3), "graph TQ COS3 metric missing");
        require(static_cast<bool>(graphTqBalance), "graph TQ balance metric missing");
        requireNear(graphTqSa->value, std::sqrt(916.0), 0.0001, "graph TQ SA metric mismatch");
        requireNear(graphTqCos3->value, 60.0 / std::sqrt(3649.0), 0.0001, "graph TQ COS3 metric mismatch");
        requireNear(graphTqBalance->value, 100.0, 0.0001, "graph TQ balance metric mismatch");

        writeTextFile(
            "graph_ems_fh_direct_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "fh_direct",
  "nodes": [
    {
      "id": "fh_direct",
      "type": "derivedLoad",
      "enabled": true,
      "params": {
        "source": "fh",
        "fhPaIndex": 309,
        "fhPbIndex": 310,
        "fhPcIndex": 311,
        "fhP3Index": 312,
        "fhQaIndex": 313,
        "fhQbIndex": 314,
        "fhQcIndex": 315,
        "fhQ3Index": 316
      }
    }
  ],
  "edges": []
})json"
        );
        const auto graphFhDirectConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_fh_direct");
        edge_gateway::PointStoreRouter graphFhDirectRouter;
        cleanupStoreSegment(graphFhDirectConfig.memoryStore);
        edge_gateway::MemoryPointStore graphFhDirectStore(graphFhDirectConfig.memoryStore);
        graphFhDirectRouter.addStore(graphFhDirectConfig.memoryStore.sharedMemoryName, graphFhDirectStore);
        graphFhDirectRouter.addRoutesFromDeviceConfigs(
            {graphFhDirectConfig},
            graphFhDirectConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine graphFhDirectSeedEngine(runtimeCatalog, graphFhDirectRouter);
        graphFhDirectSeedEngine.set(309, 20.0, 1167);
        graphFhDirectSeedEngine.set(310, 15.0, 1167);
        graphFhDirectSeedEngine.set(311, 25.0, 1167);
        graphFhDirectSeedEngine.set(312, 60.0, 1167);
        graphFhDirectSeedEngine.set(313, 4.0, 1167);
        graphFhDirectSeedEngine.set(314, 3.0, 1167);
        graphFhDirectSeedEngine.set(315, 0.0, 1167);
        graphFhDirectSeedEngine.set(316, 7.0, 1167);
        edge_gateway::GraphEmsEngine graphFhDirectEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_fh_direct_test.json"),
            graphFhDirectRouter,
            600000
        );
        graphFhDirectEngine.runOnce(1167);
        const auto graphFhDirectPa = graphFhDirectRouter.getLatestByIndex(309, 1167);
        const auto graphFhDirectSa = graphFhDirectRouter.getLatestByIndex(317, 1167);
        const auto graphFhDirectCos3 = graphFhDirectRouter.getLatestByIndex(324, 1167);
        const auto graphFhDirectBalance = graphFhDirectRouter.getLatestByIndex(325, 1167);
        require(static_cast<bool>(graphFhDirectPa), "graph direct FH PA missing");
        require(static_cast<bool>(graphFhDirectSa), "graph direct FH SA missing");
        require(static_cast<bool>(graphFhDirectCos3), "graph direct FH COS3 missing");
        require(static_cast<bool>(graphFhDirectBalance), "graph direct FH balance missing");
        requireNear(graphFhDirectPa->value, 20.0, 0.0001, "graph direct FH PA should preserve source value");
        requireNear(graphFhDirectSa->value, std::sqrt(416.0), 0.0001, "graph direct FH SA mismatch");
        requireNear(graphFhDirectCos3->value, 60.0 / std::sqrt(3649.0), 0.0001, "graph direct FH COS3 mismatch");
        requireNear(graphFhDirectBalance->value, 50.0, 0.0001, "graph direct FH balance mismatch");

        writeTextFile(
            "graph_ems_bms_model_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "bms_model",
  "nodes": [
    {
      "id": "bms",
      "type": "bmsDerived",
      "enabled": true,
      "params": {
        "bmsModel": 2,
        "chargeCurrentAllowIndex": 1556,
        "dischargeCurrentAllowIndex": 1557,
        "voltageIndex": 1566,
        "chargeKwhSumIndex": 1586,
        "dischargeKwhSumIndex": 1587,
        "chargeKwhZeroIndex": 398,
        "dischargeKwhZeroIndex": 399,
        "chargeKwAllowOutput": 1552,
        "dischargeKwAllowOutput": 1553,
        "chargeKwhTodayOutput": 1615,
        "dischargeKwhTodayOutput": 1616
      }
    }
  ],
  "edges": []
})json"
        );
        const auto graphBmsModelConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_bms_model");
        edge_gateway::PointStoreRouter graphBmsModelRouter;
        cleanupStoreSegment(graphBmsModelConfig.memoryStore);
        edge_gateway::MemoryPointStore graphBmsModelStore(graphBmsModelConfig.memoryStore);
        graphBmsModelRouter.addStore(graphBmsModelConfig.memoryStore.sharedMemoryName, graphBmsModelStore);
        graphBmsModelRouter.addRoutesFromDeviceConfigs(
            {graphBmsModelConfig},
            graphBmsModelConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine graphBmsModelSeedEngine(runtimeCatalog, graphBmsModelRouter);
        graphBmsModelSeedEngine.set(1556, 100.0, 1167);
        graphBmsModelSeedEngine.set(1557, 80.0, 1167);
        graphBmsModelSeedEngine.set(1566, 500.0, 1167);
        graphBmsModelSeedEngine.set(1586, 1200.0, 1167);
        graphBmsModelSeedEngine.set(1587, 900.0, 1167);
        graphBmsModelSeedEngine.set(398, 1000.0, 1167);
        graphBmsModelSeedEngine.set(399, 850.0, 1167);
        edge_gateway::GraphEmsEngine graphBmsModelEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_bms_model_test.json"),
            graphBmsModelRouter,
            600000
        );
        graphBmsModelEngine.runOnce(1167);
        const auto graphBmsChargeKw = graphBmsModelRouter.getLatestByIndex(1552, 1167);
        const auto graphBmsToday = graphBmsModelRouter.getLatestByIndex(1615, 1167);
        require(static_cast<bool>(graphBmsChargeKw), "graph BMS charge kW allow missing");
        requireNear(graphBmsChargeKw->value, 50.0, 0.0001, "graph BMS charge kW allow mismatch");
        require(
            !static_cast<bool>(graphBmsToday) || graphBmsToday->ts != 1167,
            "graph BMS_MODEL=2 should skip today energy outputs"
        );

        writeTextFile(
            "graph_ems_topological_order_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "topological_order",
  "nodes": [
    {
      "id": "power_solve",
      "type": "pcsPowerSolve",
      "enabled": true,
      "params": {
        "outPaDsIndex": 615,
        "outPbDsIndex": 616,
        "outPcDsIndex": 617,
        "paOutput": 627,
        "pbOutput": 628,
        "pcOutput": 629
      }
    },
    {
      "id": "ds",
      "type": "timedChargeDischarge",
      "enabled": true,
      "params": {
        "scheduleCurve": [
          { "hour": 0, "power": 30, "targetSoc": 80 }
        ],
        "bmsSocIndex": 1570,
        "cnUaIndex": 251,
        "cnUbIndex": 252,
        "cnUcIndex": 253,
        "gradPIndex": 533,
        "vMaxIndex": 463,
        "vMinIndex": 464,
        "paOutput": 615,
        "pbOutput": 616,
        "pcOutput": 617,
        "p3Output": 618,
        "runOutput": 18
      }
    }
  ],
  "edges": [
    { "from": "ds", "to": "power_solve" }
  ]
})json"
        );
        const auto graphTopoConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_topological_order");
        edge_gateway::PointStoreRouter graphTopoRouter;
        cleanupStoreSegment(graphTopoConfig.memoryStore);
        edge_gateway::MemoryPointStore graphTopoStore(graphTopoConfig.memoryStore);
        graphTopoRouter.addStore(graphTopoConfig.memoryStore.sharedMemoryName, graphTopoStore);
        graphTopoRouter.addRoutesFromDeviceConfigs(
            {graphTopoConfig},
            graphTopoConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine graphTopoSeedEngine(runtimeCatalog, graphTopoRouter);
        graphTopoSeedEngine.set(251, 230.0, 0);
        graphTopoSeedEngine.set(252, 230.0, 0);
        graphTopoSeedEngine.set(253, 230.0, 0);
        graphTopoSeedEngine.set(1570, 20.0, 0);
        graphTopoSeedEngine.set(463, 250.0, 0);
        graphTopoSeedEngine.set(464, 220.0, 0);
        graphTopoSeedEngine.set(533, 5.0, 0);
        edge_gateway::GraphEmsEngine graphTopoEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_topological_order_test.json"),
            graphTopoRouter,
            600000
        );
        graphTopoEngine.runOnce(0);
        const auto graphTopoPa = graphTopoRouter.getLatestByIndex(627, 0);
        require(static_cast<bool>(graphTopoPa), "graph topological PCS_PA_OUT missing");
        requireNear(graphTopoPa->value, 5.0, 0.0001, "graph edges should drive topological execution order");

        writeTextFile(
            "graph_ems_ds_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "ds",
  "nodes": [
    {
      "id": "ds",
      "type": "timedChargeDischarge",
      "enabled": true,
      "params": {
        "powerScheduleStartIndex": 400,
        "socScheduleStartIndex": 424,
        "modeScheduleStartIndex": 760,
        "bmsSocIndex": 1570,
        "cnUaIndex": 251,
        "cnUbIndex": 252,
        "cnUcIndex": 253,
        "gradPIndex": 533,
        "vMaxIndex": 463,
        "vMinIndex": 464,
        "powerNowOutput": 461,
        "socNowOutput": 462,
        "paOutput": 615,
        "pbOutput": 616,
        "pcOutput": 617,
        "p3Output": 618,
        "runOutput": 18
      }
    }
  ],
  "edges": []
})json"
        );
        const auto legacyDsCompareConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_legacy_ds_compare");
        edge_gateway::PointStoreRouter legacyDsCompareRouter;
        cleanupStoreSegment(legacyDsCompareConfig.memoryStore);
        edge_gateway::MemoryPointStore legacyDsCompareStore(legacyDsCompareConfig.memoryStore);
        legacyDsCompareRouter.addStore(
            legacyDsCompareConfig.memoryStore.sharedMemoryName,
            legacyDsCompareStore
        );
        legacyDsCompareRouter.addRoutesFromDeviceConfigs(
            {legacyDsCompareConfig},
            legacyDsCompareConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine legacyDsCompareEngine(
            runtimeCatalog,
            legacyDsCompareRouter,
            600000,
            {{"Meter_TQ", "1"}, {"Meter_CN", "1"}, {"Meter_BW", "0"}, {"Meter_FH", "0"}, {"BMS_MODEL", "2"}}
        );

        const auto graphDsCompareConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_ds_compare");
        edge_gateway::PointStoreRouter graphDsCompareRouter;
        cleanupStoreSegment(graphDsCompareConfig.memoryStore);
        edge_gateway::MemoryPointStore graphDsCompareStore(graphDsCompareConfig.memoryStore);
        graphDsCompareRouter.addStore(graphDsCompareConfig.memoryStore.sharedMemoryName, graphDsCompareStore);
        graphDsCompareRouter.addRoutesFromDeviceConfigs(
            {graphDsCompareConfig},
            graphDsCompareConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine graphDsSeedEngine(runtimeCatalog, graphDsCompareRouter);
        const auto seedDsInputs = [](auto& engine, std::int64_t ts) {
            engine.set(251, 230.0, ts);
            engine.set(252, 230.0, ts);
            engine.set(253, 230.0, ts);
            engine.set(1570, 20.0, ts);
            engine.set(400, 30.0, ts);
            engine.set(424, 80.0, ts);
            engine.set(760, 0.0, ts);
            engine.set(463, 250.0, ts);
            engine.set(464, 220.0, ts);
            engine.set(533, 5.0, ts);
        };
        seedDsInputs(legacyDsCompareEngine, 1170);
        seedDsInputs(graphDsSeedEngine, 1170);
        legacyDsCompareEngine.runOnce(1170);
        edge_gateway::GraphEmsEngine graphDsCompareEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_ds_test.json"),
            graphDsCompareRouter,
            600000
        );
        graphDsCompareEngine.runOnce(1170);
        const std::uint32_t dsCompareIndexes[] = {461, 615, 618, 18};
        for (const auto index : dsCompareIndexes) {
            const auto legacyValue = legacyDsCompareRouter.getLatestByIndex(index, 1170);
            const auto graphValue = graphDsCompareRouter.getLatestByIndex(index, 1170);
            require(static_cast<bool>(legacyValue), "legacy DS compare output missing");
            require(static_cast<bool>(graphValue), "graph DS compare output missing");
            requireNear(graphValue->value, legacyValue->value, 0.0001, "graph DS output mismatch");
        }

        writeTextFile(
            "graph_ems_ds_curve_writeback_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "ds_curve_writeback",
  "nodes": [
    {
      "id": "ds",
      "type": "timedChargeDischarge",
      "enabled": true,
        "params": {
        "scheduleCurve": [
          { "hour": 0, "power": -25, "targetSoc": 80 },
          { "hour": 5, "power": 45, "targetSoc": 90 }
        ],
        "bmsSocIndex": 1570,
        "cnUaIndex": 251,
        "cnUbIndex": 252,
        "cnUcIndex": 253,
        "gradPIndex": 533,
        "vMaxIndex": 463,
        "vMinIndex": 464,
        "powerNowOutput": 461,
        "socNowOutput": 462,
        "paOutput": 615,
        "pbOutput": 616,
        "pcOutput": 617,
        "p3Output": 618,
        "runOutput": 18
      }
    },
    {
      "id": "power_solve",
      "type": "pcsPowerSolve",
      "enabled": true,
      "params": {
        "paOutput": 627,
        "pbOutput": 628,
        "pcOutput": 629,
        "qaOutput": 630,
        "qbOutput": 631,
        "qcOutput": 632
      }
    },
    {
      "id": "pcs_writeback",
      "type": "pcsWriteback",
      "enabled": true,
      "params": {
        "submitWrites": true,
        "paInput": 627,
        "pbInput": 628,
        "pcInput": 629,
        "qaInput": 630,
        "qbInput": 631,
        "qcInput": 632,
        "comStatusIndex": 1399,
        "pControlAIndex": 1318,
        "pControlBIndex": 1319,
        "pControlCIndex": 1320,
        "qControlAIndex": 1321,
        "qControlBIndex": 1322,
        "qControlCIndex": 1323
      }
    }
  ],
  "edges": [
    { "from": "ds", "to": "power_solve" },
    { "from": "power_solve", "to": "pcs_writeback" }
  ]
})json"
        );
        const auto graphDsCurveConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_ds_curve");
        edge_gateway::PointStoreRouter graphDsCurveRouter;
        cleanupStoreSegment(graphDsCurveConfig.memoryStore);
        edge_gateway::MemoryPointStore graphDsCurveStore(graphDsCurveConfig.memoryStore);
        graphDsCurveRouter.addStore(graphDsCurveConfig.memoryStore.sharedMemoryName, graphDsCurveStore);
        graphDsCurveRouter.addRoutesFromDeviceConfigs(
            {graphDsCurveConfig},
            graphDsCurveConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine graphDsCurveSeedEngine(runtimeCatalog, graphDsCurveRouter);
        const std::int64_t graphCurveHour5Ms = 5LL * 60LL * 60LL * 1000LL;
        graphDsCurveSeedEngine.set(251, 230.0, graphCurveHour5Ms);
        graphDsCurveSeedEngine.set(252, 230.0, graphCurveHour5Ms);
        graphDsCurveSeedEngine.set(253, 230.0, graphCurveHour5Ms);
        graphDsCurveSeedEngine.set(1570, 20.0, graphCurveHour5Ms);
        graphDsCurveSeedEngine.set(463, 250.0, graphCurveHour5Ms);
        graphDsCurveSeedEngine.set(464, 220.0, graphCurveHour5Ms);
        graphDsCurveSeedEngine.set(533, 5.0, graphCurveHour5Ms);
        graphDsCurveSeedEngine.set(1399, 1.0, graphCurveHour5Ms);
        edge_gateway::GraphEmsEngine graphDsCurveEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_ds_curve_writeback_test.json"),
            graphDsCurveRouter,
            600000
        );
        const auto graphDsCurveResult = graphDsCurveEngine.runOnce(graphCurveHour5Ms);
        const auto curvePowerNow = graphDsCurveRouter.getLatestByIndex(461, graphCurveHour5Ms);
        const auto curveSocNow = graphDsCurveRouter.getLatestByIndex(462, graphCurveHour5Ms);
        require(static_cast<bool>(curvePowerNow), "DS curve power now missing");
        require(static_cast<bool>(curveSocNow), "DS curve SOC now missing");
        requireNear(curvePowerNow->value, 45.0, 0.0001, "DS curve should use configured signed power");
        requireNear(curveSocNow->value, 90.0, 0.0001, "DS curve should use configured hour target SOC");
        require(graphDsCurveResult.deviceWrites == 3, "DS curve should submit three PCS write commands");
        const auto curveWrites = graphDsCurveRouter.peekPendingWrites(8);
        auto findCurveWrite = [&](std::uint32_t index) -> const edge_gateway::PendingWriteCommand* {
            for (const auto& command : curveWrites) {
                if (command.index == index) {
                    return &command;
                }
            }
            return nullptr;
        };
        require(curveWrites.size() == 3, "DS curve should enqueue PCS write commands");
        const auto* curveWriteA = findCurveWrite(1318);
        const auto* curveWriteB = findCurveWrite(1319);
        const auto* curveWriteC = findCurveWrite(1320);
        require(curveWriteA != nullptr, "DS curve write A missing");
        require(curveWriteB != nullptr, "DS curve write B missing");
        require(curveWriteC != nullptr, "DS curve write C missing");
        requireNear(curveWriteA->value, 5.0, 0.0001, "DS curve write A value mismatch");

        const auto graphCurveHour0Ms = 0LL;
        graphDsCurveSeedEngine.set(251, 230.0, graphCurveHour0Ms);
        graphDsCurveSeedEngine.set(252, 230.0, graphCurveHour0Ms);
        graphDsCurveSeedEngine.set(253, 230.0, graphCurveHour0Ms);
        graphDsCurveSeedEngine.set(1570, 95.0, graphCurveHour0Ms);
        graphDsCurveSeedEngine.set(463, 250.0, graphCurveHour0Ms);
        graphDsCurveSeedEngine.set(464, 220.0, graphCurveHour0Ms);
        graphDsCurveSeedEngine.set(533, 10.0, graphCurveHour0Ms);
        graphDsCurveSeedEngine.set(1399, 1.0, graphCurveHour0Ms);
        graphDsCurveEngine.runOnce(graphCurveHour0Ms);
        const auto dischargePowerNow = graphDsCurveRouter.getLatestByIndex(461, graphCurveHour0Ms);
        const auto dischargePa = graphDsCurveRouter.getLatestByIndex(615, graphCurveHour0Ms);
        require(static_cast<bool>(dischargePowerNow), "DS curve discharge power now missing");
        require(static_cast<bool>(dischargePa), "DS curve discharge PA missing");
        requireNear(dischargePowerNow->value, -25.0, 0.0001, "DS curve should use configured signed discharge power");
        require(dischargePa->value < 0.0, "DS curve should discharge when BMS SOC is above target SOC");

        writeTextFile(
            "graph_ems_gf_ph_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "gf_ph",
  "nodes": [
    {
      "id": "gf",
      "type": "photovoltaicCharge",
      "enabled": true,
      "params": {
        "fhPaIndex": 309,
        "fhPbIndex": 310,
        "fhPcIndex": 311,
        "negativeLimitIndex": 457,
        "startHourIndex": 581,
        "endHourIndex": 583,
        "paOutput": 619,
        "pbOutput": 620,
        "pcOutput": 621,
        "p3Output": 622,
        "runOutput": 22
      }
    },
    {
      "id": "ph",
      "type": "phaseBalance",
      "enabled": true,
      "params": {
        "tqPaIndex": 209,
        "tqPbIndex": 210,
        "tqPcIndex": 211,
        "tqP3Index": 212,
        "cnPaIndex": 259,
        "cnPbIndex": 260,
        "cnPcIndex": 261,
        "balancePercentIndex": 562,
        "balanceOutput": 564,
        "tqCnPaOutput": 565,
        "tqCnPbOutput": 566,
        "tqCnPcOutput": 567,
        "paOutput": 623,
        "pbOutput": 624,
        "pcOutput": 625,
        "runOutput": 20
      }
    }
  ],
  "edges": []
})json"
        );
        const auto legacyGfPhConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_legacy_gf_ph");
        edge_gateway::PointStoreRouter legacyGfPhRouter;
        cleanupStoreSegment(legacyGfPhConfig.memoryStore);
        edge_gateway::MemoryPointStore legacyGfPhStore(legacyGfPhConfig.memoryStore);
        legacyGfPhRouter.addStore(legacyGfPhConfig.memoryStore.sharedMemoryName, legacyGfPhStore);
        legacyGfPhRouter.addRoutesFromDeviceConfigs(
            {legacyGfPhConfig},
            legacyGfPhConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine legacyGfPhEngine(
            runtimeCatalog,
            legacyGfPhRouter,
            600000,
            {{"Meter_TQ", "1"}, {"Meter_CN", "1"}, {"Meter_FH", "1"}, {"BMS_MODEL", "2"}}
        );

        const auto graphGfPhConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_gf_ph");
        edge_gateway::PointStoreRouter graphGfPhRouter;
        cleanupStoreSegment(graphGfPhConfig.memoryStore);
        edge_gateway::MemoryPointStore graphGfPhStore(graphGfPhConfig.memoryStore);
        graphGfPhRouter.addStore(graphGfPhConfig.memoryStore.sharedMemoryName, graphGfPhStore);
        graphGfPhRouter.addRoutesFromDeviceConfigs(
            {graphGfPhConfig},
            graphGfPhConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine graphGfPhSeedEngine(runtimeCatalog, graphGfPhRouter);
        const auto seedGfPhInputs = [](auto& engine, std::int64_t ts) {
            engine.set(209, 30.0, ts);
            engine.set(210, 20.0, ts);
            engine.set(211, 10.0, ts);
            engine.set(212, 60.0, ts);
            engine.set(259, 10.0, ts);
            engine.set(260, 5.0, ts);
            engine.set(261, 15.0, ts);
            engine.set(309, 20.0, ts);
            engine.set(310, 40.0, ts);
            engine.set(311, 30.0, ts);
            engine.set(562, 10.0, ts);
            engine.set(457, 30.0, ts);
            engine.set(581, 0.0, ts);
            engine.set(583, 23.0, ts);
        };
        seedGfPhInputs(legacyGfPhEngine, 1180);
        seedGfPhInputs(graphGfPhSeedEngine, 1180);
        legacyGfPhEngine.runOnce(1180);
        edge_gateway::GraphEmsEngine graphGfPhEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_gf_ph_test.json"),
            graphGfPhRouter,
            600000
        );
        graphGfPhEngine.runOnce(1180);
        const std::uint32_t gfPhCompareIndexes[] = {
            619, 620, 621, 622, 22,
            564, 565, 566, 567, 623, 624, 625, 20
        };
        for (const auto index : gfPhCompareIndexes) {
            const auto legacyValue = legacyGfPhRouter.getLatestByIndex(index, 1180);
            const auto graphValue = graphGfPhRouter.getLatestByIndex(index, 1180);
            require(static_cast<bool>(legacyValue), "legacy GF/PH compare output missing");
            require(static_cast<bool>(graphValue), "graph GF/PH compare output missing");
            requireNear(graphValue->value, legacyValue->value, 0.0001, "graph GF/PH output mismatch");
        }

        writeTextFile(
            "graph_ems_pcs_solve_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "pcs_solve",
  "nodes": [
    {
      "id": "power_solve",
      "type": "pcsPowerSolve",
      "enabled": true,
      "params": {
        "paOutput": 627,
        "pbOutput": 628,
        "pcOutput": 629,
        "qaOutput": 630,
        "qbOutput": 631,
        "qcOutput": 632,
        "zrRunOutput": 24,
        "cosRunOutput": 8,
        "lvRunOutput": 10,
        "hvRunOutput": 12,
        "gfRunOutput": 22
      }
    }
  ],
  "edges": []
})json"
        );
        writeTextFile(
            "graph_ems_pcs_writeback_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "pcs_writeback",
  "nodes": [
    {
      "id": "power_solve",
      "type": "pcsPowerSolve",
      "enabled": true,
      "params": {
        "paOutput": 627,
        "pbOutput": 628,
        "pcOutput": 629,
        "qaOutput": 630,
        "qbOutput": 631,
        "qcOutput": 632,
        "zrRunOutput": 24,
        "cosRunOutput": 8,
        "lvRunOutput": 10,
        "hvRunOutput": 12,
        "gfRunOutput": 22,
        "submitWrites": false
      }
    },
    {
      "id": "pcs_writeback",
      "type": "pcsWriteback",
      "enabled": true,
      "params": {
        "submitWrites": true,
        "paInput": 627,
        "pbInput": 628,
        "pcInput": 629,
        "qaInput": 630,
        "qbInput": 631,
        "qcInput": 632,
        "comStatusIndex": 1399,
        "pControlAIndex": 1318,
        "pControlBIndex": 1319,
        "pControlCIndex": 1320,
        "qControlAIndex": 1321,
        "qControlBIndex": 1322,
        "qControlCIndex": 1323
      }
    }
  ],
  "edges": [
    { "from": "power_solve", "to": "pcs_writeback" }
  ]
})json"
        );
        const auto seedPcsSolveInputs = [](auto& engine, std::int64_t ts) {
            engine.set(601, 7.5, ts);
            engine.set(602, 0.0, ts);
            engine.set(603, 0.0, ts);
            engine.set(26, 0.0, ts);
            engine.set(590, 0.0, ts);
            engine.set(591, 0.0, ts);
            engine.set(23, 0.0, ts);
            engine.set(588, 0.0, ts);
            engine.set(454, 0.0, ts);
            engine.set(453, 0.0, ts);
            engine.set(458, 0.0, ts);
            engine.set(457, 0.0, ts);
            engine.set(309, 0.0, ts);
            engine.set(310, 0.0, ts);
            engine.set(311, 0.0, ts);
            engine.set(613, 0.0, ts);
            engine.set(614, 0.0, ts);
            engine.set(615, 0.0, ts);
            engine.set(616, 0.0, ts);
            engine.set(617, 0.0, ts);
            engine.set(605, -5.0, ts);
            engine.set(606, 0.0, ts);
            engine.set(607, 0.0, ts);
            engine.set(609, 0.0, ts);
            engine.set(610, 0.0, ts);
            engine.set(611, 5.0, ts);
            engine.set(619, 0.0, ts);
            engine.set(620, 0.0, ts);
            engine.set(621, 0.0, ts);
            engine.set(623, 0.0, ts);
            engine.set(624, 0.0, ts);
            engine.set(625, 0.0, ts);
            engine.set(535, 30.0, ts);
            engine.set(504, 20.0, ts);
            engine.set(151, 300.0, ts);
        };
        const auto legacyPcsSolveConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_legacy_pcs_solve");
        edge_gateway::PointStoreRouter legacyPcsSolveRouter;
        cleanupStoreSegment(legacyPcsSolveConfig.memoryStore);
        edge_gateway::MemoryPointStore legacyPcsSolveStore(legacyPcsSolveConfig.memoryStore);
        legacyPcsSolveRouter.addStore(legacyPcsSolveConfig.memoryStore.sharedMemoryName, legacyPcsSolveStore);
        legacyPcsSolveRouter.addRoutesFromDeviceConfigs(
            {legacyPcsSolveConfig},
            legacyPcsSolveConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine legacyPcsSolveEngine(runtimeCatalog, legacyPcsSolveRouter);
        const auto graphPcsSolveConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_pcs_solve");
        edge_gateway::PointStoreRouter graphPcsSolveRouter;
        cleanupStoreSegment(graphPcsSolveConfig.memoryStore);
        edge_gateway::MemoryPointStore graphPcsSolveStore(graphPcsSolveConfig.memoryStore);
        graphPcsSolveRouter.addStore(graphPcsSolveConfig.memoryStore.sharedMemoryName, graphPcsSolveStore);
        graphPcsSolveRouter.addRoutesFromDeviceConfigs(
            {graphPcsSolveConfig},
            graphPcsSolveConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine graphPcsSolveSeedEngine(runtimeCatalog, graphPcsSolveRouter);
        seedPcsSolveInputs(legacyPcsSolveEngine, 1190);
        seedPcsSolveInputs(graphPcsSolveSeedEngine, 1190);
        legacyPcsSolveEngine.runOnce(1190);
        edge_gateway::GraphEmsEngine graphPcsSolveEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_pcs_solve_test.json"),
            graphPcsSolveRouter,
            600000
        );
        graphPcsSolveEngine.runOnce(1190);
        const std::uint32_t pcsSolveCompareIndexes[] = {24, 627, 628, 629, 630, 631, 632};
        for (const auto index : pcsSolveCompareIndexes) {
            const auto legacyValue = legacyPcsSolveRouter.getLatestByIndex(index, 1190);
            const auto graphValue = graphPcsSolveRouter.getLatestByIndex(index, 1190);
            require(static_cast<bool>(legacyValue), "legacy PCS solve compare output missing");
            require(static_cast<bool>(graphValue), "graph PCS solve compare output missing");
            requireNear(graphValue->value, legacyValue->value, 0.001, "graph PCS solve output mismatch");
        }

        const auto seedPcsWritebackInputs = [](auto& engine, std::int64_t ts) {
            engine.set(601, 7.5, ts);
            engine.set(602, 0.0, ts);
            engine.set(603, 0.0, ts);
            engine.set(26, 0.0, ts);
            engine.set(590, 0.0, ts);
            engine.set(591, 0.0, ts);
            engine.set(23, 0.0, ts);
            engine.set(588, 0.0, ts);
            engine.set(454, 0.0, ts);
            engine.set(453, 0.0, ts);
            engine.set(458, 0.0, ts);
            engine.set(457, 0.0, ts);
            engine.set(309, 0.0, ts);
            engine.set(310, 0.0, ts);
            engine.set(311, 0.0, ts);
            engine.set(613, 0.0, ts);
            engine.set(614, 0.0, ts);
            engine.set(615, 0.0, ts);
            engine.set(616, 0.0, ts);
            engine.set(617, 0.0, ts);
            engine.set(605, -5.0, ts);
            engine.set(606, 0.0, ts);
            engine.set(607, 0.0, ts);
            engine.set(609, 0.0, ts);
            engine.set(610, 0.0, ts);
            engine.set(611, 5.0, ts);
            engine.set(619, 0.0, ts);
            engine.set(620, 0.0, ts);
            engine.set(621, 0.0, ts);
            engine.set(623, 0.0, ts);
            engine.set(624, 0.0, ts);
            engine.set(625, 0.0, ts);
            engine.set(535, 30.0, ts);
            engine.set(504, 20.0, ts);
            engine.set(151, 300.0, ts);
            engine.set(1399, 1.0, ts);
        };
        const auto legacyPcsWriteConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_legacy_pcs_writeback");
        edge_gateway::PointStoreRouter legacyPcsWriteRouter;
        cleanupStoreSegment(legacyPcsWriteConfig.memoryStore);
        edge_gateway::MemoryPointStore legacyPcsWriteStore(legacyPcsWriteConfig.memoryStore);
        legacyPcsWriteRouter.addStore(legacyPcsWriteConfig.memoryStore.sharedMemoryName, legacyPcsWriteStore);
        legacyPcsWriteRouter.addRoutesFromDeviceConfigs(
            {legacyPcsWriteConfig},
            legacyPcsWriteConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine legacyPcsWriteEngine(runtimeCatalog, legacyPcsWriteRouter);
        const auto graphPcsWriteConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_pcs_writeback");
        edge_gateway::PointStoreRouter graphPcsWriteRouter;
        cleanupStoreSegment(graphPcsWriteConfig.memoryStore);
        edge_gateway::MemoryPointStore graphPcsWriteStore(graphPcsWriteConfig.memoryStore);
        graphPcsWriteRouter.addStore(graphPcsWriteConfig.memoryStore.sharedMemoryName, graphPcsWriteStore);
        graphPcsWriteRouter.addRoutesFromDeviceConfigs(
            {graphPcsWriteConfig},
            graphPcsWriteConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine graphPcsWriteSeedEngine(runtimeCatalog, graphPcsWriteRouter);
        seedPcsWritebackInputs(legacyPcsWriteEngine, 1195);
        seedPcsWritebackInputs(graphPcsWriteSeedEngine, 1195);
        const auto legacyPcsWriteResult = legacyPcsWriteEngine.runOnce(1195);
        edge_gateway::GraphEmsEngine graphPcsWriteEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_pcs_writeback_test.json"),
            graphPcsWriteRouter,
            600000
        );
        const auto graphPcsWriteResult = graphPcsWriteEngine.runOnce(1195);
        const auto legacyPcsWrites = legacyPcsWriteRouter.peekPendingWrites(16);
        const auto graphPcsWrites = graphPcsWriteRouter.peekPendingWrites(16);
        require(graphPcsWrites.size() == legacyPcsWrites.size(), "graph PCS write count mismatch");
        require(graphPcsWriteResult.deviceWrites == legacyPcsWriteResult.deviceWrites, "graph PCS result write count mismatch");
        for (std::size_t i = 0; i < legacyPcsWrites.size(); ++i) {
            require(graphPcsWrites[i].index == legacyPcsWrites[i].index, "graph PCS write index mismatch");
            requireNear(graphPcsWrites[i].value, legacyPcsWrites[i].value, 0.0001, "graph PCS write value mismatch");
            require(graphPcsWrites[i].source == "graph-ems", "graph PCS write source mismatch");
        }

        writeTextFile(
            "graph_ems_control_gate_writeback_test.json",
            R"json({
  "schemaVersion": "1.2.0",
  "graphCode": "control_gate_writeback",
  "nodes": [
    {
      "id": "pcs_control_gate",
      "type": "controlGate",
      "enabled": true,
      "params": {
        "combine": "all",
        "conditions": [
          { "index": 1399, "operator": "eq", "value": 1 },
          { "index": 700040, "operator": "eq", "value": 1 }
        ],
        "outputIndex": 700041
      }
    },
    {
      "id": "pcs_writeback",
      "type": "pcsWriteback",
      "enabled": true,
      "params": {
        "submitWrites": true,
        "permitIndex": 700041,
        "permitValue": 1,
        "paInput": 627,
        "pbInput": 628,
        "pcInput": 629,
        "qaInput": 630,
        "qbInput": 631,
        "qcInput": 632,
        "comStatusIndex": 1399,
        "pControlAIndex": 1318,
        "pControlBIndex": 1319,
        "pControlCIndex": 1320,
        "qControlAIndex": 1321,
        "qControlBIndex": 1322,
        "qControlCIndex": 1323
      }
    }
  ],
  "edges": [
    { "from": "pcs_control_gate", "to": "pcs_writeback" }
  ]
})json"
        );
        const auto controlGateConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_control_gate");
        edge_gateway::PointStoreRouter controlGateRouter;
        cleanupStoreSegment(controlGateConfig.memoryStore);
        edge_gateway::MemoryPointStore controlGateStore(controlGateConfig.memoryStore);
        controlGateRouter.addStore(controlGateConfig.memoryStore.sharedMemoryName, controlGateStore);
        controlGateRouter.addRoutesFromDeviceConfigs(
            {controlGateConfig},
            controlGateConfig.memoryStore.sharedMemoryName
        );
        addRouteIfMissing(
            controlGateRouter,
            700040,
            "PCS_REMOTE_MODE",
            controlGateConfig.memoryStore.sharedMemoryName,
            false
        );
        addRouteIfMissing(
            controlGateRouter,
            700041,
            "PCS_CONTROL_PERMIT",
            controlGateConfig.memoryStore.sharedMemoryName,
            false
        );
        edge_gateway::LegacyEmsEngine controlGateSeedEngine(runtimeCatalog, controlGateRouter);
        controlGateSeedEngine.set(1399, 1.0, 1196);
        controlGateSeedEngine.set(700040, 0.0, 1196);
        controlGateSeedEngine.set(627, 12.0, 1196);
        controlGateSeedEngine.set(628, 0.0, 1196);
        controlGateSeedEngine.set(629, 0.0, 1196);
        controlGateSeedEngine.set(630, 0.0, 1196);
        controlGateSeedEngine.set(631, 0.0, 1196);
        controlGateSeedEngine.set(632, 0.0, 1196);
        edge_gateway::GraphEmsEngine controlGateEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_control_gate_writeback_test.json"),
            controlGateRouter,
            600000
        );
        const auto deniedControlResult = controlGateEngine.runOnce(1196);
        const auto deniedPermit = controlGateRouter.getLatestByIndex(700041, 1196);
        require(deniedControlResult.errors.empty(), "control gate denied run should not report errors");
        require(static_cast<bool>(deniedPermit), "control gate denied output missing");
        requireNear(deniedPermit->value, 0.0, 0.0001, "control gate should deny when remote mode is off");
        require(controlGateRouter.peekPendingWrites(8).empty(), "denied control gate must block PCS writes");

        controlGateSeedEngine.set(700040, 1.0, 1197);
        const auto permittedControlResult = controlGateEngine.runOnce(1197);
        const auto permittedValue = controlGateRouter.getLatestByIndex(700041, 1197);
        const auto permittedWrites = controlGateRouter.peekPendingWrites(8);
        require(permittedControlResult.errors.empty(), "control gate permitted run should not report errors");
        require(static_cast<bool>(permittedValue), "control gate permitted output missing");
        requireNear(permittedValue->value, 1.0, 0.0001, "control gate should permit when all conditions pass");
        require(permittedWrites.size() == 1, "permitted control gate should submit one non-zero PCS write");
        require(permittedWrites[0].index == 1318, "permitted control gate PCS write index mismatch");
        requireNear(permittedWrites[0].value, 12.0, 0.0001, "permitted control gate PCS write value mismatch");

        const auto pendingBeforeStale = controlGateRouter.peekPendingWrites(8).size();
        const auto staleControlResult = controlGateEngine.runOnce(701198);
        const auto stalePermit = controlGateRouter.getLatestByIndex(700041, 701198);
        require(staleControlResult.errors.empty(), "stale control gate run should fail closed without node errors");
        require(static_cast<bool>(stalePermit), "stale control gate output missing");
        requireNear(stalePermit->value, 0.0, 0.0001, "stale control gate inputs must refresh permit to zero");
        require(
            controlGateRouter.peekPendingWrites(8).size() == pendingBeforeStale,
            "stale control gate inputs must not submit additional PCS writes"
        );

        writeTextFile(
            "graph_ems_pcs_solve_submit_writes_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "pcs_solve_submit_writes",
  "nodes": [
    {
      "id": "power_solve",
      "type": "pcsPowerSolve",
      "enabled": true,
      "params": {
        "paOutput": 627,
        "pbOutput": 628,
        "pcOutput": 629,
        "qaOutput": 630,
        "qbOutput": 631,
        "qcOutput": 632,
        "zrRunOutput": 24,
        "cosRunOutput": 8,
        "lvRunOutput": 10,
        "hvRunOutput": 12,
        "gfRunOutput": 22,
        "submitWrites": true,
        "comStatusIndex": 1399,
        "pControlAIndex": 1318,
        "pControlBIndex": 1319,
        "pControlCIndex": 1320,
        "qControlAIndex": 1321,
        "qControlBIndex": 1322,
        "qControlCIndex": 1323
      }
    }
  ],
  "edges": []
})json"
        );
        const auto graphPcsSolveSubmitConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_pcs_solve_submit");
        edge_gateway::PointStoreRouter graphPcsSolveSubmitRouter;
        cleanupStoreSegment(graphPcsSolveSubmitConfig.memoryStore);
        edge_gateway::MemoryPointStore graphPcsSolveSubmitStore(graphPcsSolveSubmitConfig.memoryStore);
        graphPcsSolveSubmitRouter.addStore(graphPcsSolveSubmitConfig.memoryStore.sharedMemoryName, graphPcsSolveSubmitStore);
        graphPcsSolveSubmitRouter.addRoutesFromDeviceConfigs(
            {graphPcsSolveSubmitConfig},
            graphPcsSolveSubmitConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine graphPcsSolveSubmitSeedEngine(runtimeCatalog, graphPcsSolveSubmitRouter);
        seedPcsWritebackInputs(graphPcsSolveSubmitSeedEngine, 1197);
        edge_gateway::GraphEmsEngine graphPcsSolveSubmitEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_pcs_solve_submit_writes_test.json"),
            graphPcsSolveSubmitRouter,
            600000
        );
        const auto graphPcsSolveSubmitResult = graphPcsSolveSubmitEngine.runOnce(1197);
        const auto graphPcsSolveSubmitWrites = graphPcsSolveSubmitRouter.peekPendingWrites(16);
        require(graphPcsSolveSubmitResult.deviceWrites == 6, "pcsPowerSolve submitWrites should submit six commands");
        require(graphPcsSolveSubmitWrites.size() == 6, "pcsPowerSolve submitWrites pending count mismatch");
        require(graphPcsSolveSubmitWrites[0].index == 1318, "pcsPowerSolve submitWrites first index mismatch");
        requireNear(graphPcsSolveSubmitWrites[0].value, -5.0, 0.0001, "pcsPowerSolve submitWrites first value mismatch");
        require(graphPcsSolveSubmitWrites[0].source == "graph-ems", "pcsPowerSolve submitWrites source mismatch");

        writeTextFile(
            "graph_ems_cdfd_power_solve_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "cdfd_power_solve",
  "nodes": [
    {
      "id": "cd_fd",
      "type": "chargeDischarge",
      "enabled": true,
      "params": {
        "bmsSocIndex": 1570,
        "cdTargetPowerIndex": 451,
        "cdTargetSocIndex": 452,
        "fdTargetPowerIndex": 455,
        "fdTargetSocIndex": 456,
        "positiveLimitEnableIndex": 454,
        "negativeLimitEnableIndex": 458,
        "fhP3Index": 312,
        "cdP3Output": 613,
        "fdP3Output": 614
      }
    },
    {
      "id": "power_solve",
      "type": "pcsPowerSolve",
      "enabled": true,
      "params": {
        "outP3CdIndex": 613,
        "outP3FdIndex": 614,
        "pMaxIndex": 535,
        "qMaxIndex": 504,
        "s3MaxIndex": 151,
        "bmsSocIndex": 1570,
        "bmsSocMaxIndex": 161,
        "bmsSocMinIndex": 162,
        "paOutput": 627,
        "pbOutput": 628,
        "pcOutput": 629,
        "qaOutput": 630,
        "qbOutput": 631,
        "qcOutput": 632,
        "zrRunOutput": 24
      }
    }
  ],
  "edges": [
    { "from": "cd_fd", "to": "power_solve" }
  ]
})json"
        );
        const auto graphCdfdSolveConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_cdfd_solve");
        edge_gateway::PointStoreRouter graphCdfdSolveRouter;
        cleanupStoreSegment(graphCdfdSolveConfig.memoryStore);
        edge_gateway::MemoryPointStore graphCdfdSolveStore(graphCdfdSolveConfig.memoryStore);
        graphCdfdSolveRouter.addStore(graphCdfdSolveConfig.memoryStore.sharedMemoryName, graphCdfdSolveStore);
        graphCdfdSolveRouter.addRoutesFromDeviceConfigs(
            {graphCdfdSolveConfig},
            graphCdfdSolveConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine graphCdfdSolveSeedEngine(runtimeCatalog, graphCdfdSolveRouter);
        graphCdfdSolveSeedEngine.set(1570, 97.0, 1198);
        graphCdfdSolveSeedEngine.set(451, 30.0, 1198);
        graphCdfdSolveSeedEngine.set(452, 95.0, 1198);
        graphCdfdSolveSeedEngine.set(455, 30.0, 1198);
        graphCdfdSolveSeedEngine.set(456, 20.0, 1198);
        graphCdfdSolveSeedEngine.set(454, 0.0, 1198);
        graphCdfdSolveSeedEngine.set(458, 0.0, 1198);
        graphCdfdSolveSeedEngine.set(312, 0.0, 1198);
        graphCdfdSolveSeedEngine.set(151, 30.0, 1198);
        graphCdfdSolveSeedEngine.set(535, 10.0, 1198);
        graphCdfdSolveSeedEngine.set(504, 10.0, 1198);
        graphCdfdSolveSeedEngine.set(161, 100.0, 1198);
        graphCdfdSolveSeedEngine.set(162, 5.0, 1198);
        edge_gateway::GraphEmsEngine graphCdfdSolveEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_cdfd_power_solve_test.json"),
            graphCdfdSolveRouter,
            600000
        );
        graphCdfdSolveEngine.runOnce(1198);
        const auto graphCdfdOutP3Fd = graphCdfdSolveRouter.getLatestByIndex(614, 1198);
        require(static_cast<bool>(graphCdfdOutP3Fd), "graph CD/FD output missing");
        requireNear(graphCdfdOutP3Fd->value, -30.0, 0.0001, "graph CD/FD discharge output mismatch");
        for (const auto index : {627U, 628U, 629U}) {
            const auto graphCdfdPhase = graphCdfdSolveRouter.getLatestByIndex(index, 1198);
            require(static_cast<bool>(graphCdfdPhase), "graph CD/FD PCS phase output missing");
            requireNear(graphCdfdPhase->value, -10.0, 0.0001, "graph CD/FD PCS phase output mismatch");
        }

        writeTextFile(
            "graph_ems_sequential_cdfd_power_solve_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "sequential_cdfd_power_solve",
  "nodes": [
    {
      "id": "cd_fd",
      "type": "chargeDischarge",
      "enabled": true,
      "params": {
        "mode": "dischargeThenCharge",
        "bmsSocIndex": 1570,
        "cdTargetPowerIndex": 451,
        "cdTargetSocIndex": 452,
        "fdTargetPowerIndex": 455,
        "fdTargetSocIndex": 456,
        "cdRunOutput": 14,
        "fdRunOutput": 16,
        "cdP3Output": 613,
        "fdP3Output": 614,
        "phaseStateOutput": 17
      }
    },
    {
      "id": "power_solve",
      "type": "pcsPowerSolve",
      "enabled": true,
      "params": {
        "outP3CdIndex": 613,
        "outP3FdIndex": 614,
        "pMaxIndex": 535,
        "qMaxIndex": 504,
        "s3MaxIndex": 151,
        "bmsSocIndex": 1570,
        "bmsSocMaxIndex": 161,
        "bmsSocMinIndex": 162,
        "paOutput": 627,
        "pbOutput": 628,
        "pcOutput": 629,
        "qaOutput": 630,
        "qbOutput": 631,
        "qcOutput": 632,
        "zrRunOutput": 24
      }
    }
  ],
  "edges": [
    { "from": "cd_fd", "to": "power_solve" }
  ]
})json"
        );
        const auto graphSeqCdfdConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_seq_cdfd");
        edge_gateway::PointStoreRouter graphSeqCdfdRouter;
        cleanupStoreSegment(graphSeqCdfdConfig.memoryStore);
        edge_gateway::MemoryPointStore graphSeqCdfdStore(graphSeqCdfdConfig.memoryStore);
        graphSeqCdfdRouter.addStore(graphSeqCdfdConfig.memoryStore.sharedMemoryName, graphSeqCdfdStore);
        graphSeqCdfdRouter.addRoutesFromDeviceConfigs(
            {graphSeqCdfdConfig},
            graphSeqCdfdConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine graphSeqCdfdSeedEngine(runtimeCatalog, graphSeqCdfdRouter);
        graphSeqCdfdSeedEngine.set(1570, 97.0, 1199);
        graphSeqCdfdSeedEngine.set(451, 30.0, 1199);
        graphSeqCdfdSeedEngine.set(452, 95.0, 1199);
        graphSeqCdfdSeedEngine.set(455, 30.0, 1199);
        graphSeqCdfdSeedEngine.set(456, 20.0, 1199);
        graphSeqCdfdSeedEngine.set(151, 30.0, 1199);
        graphSeqCdfdSeedEngine.set(535, 10.0, 1199);
        graphSeqCdfdSeedEngine.set(504, 10.0, 1199);
        graphSeqCdfdSeedEngine.set(161, 100.0, 1199);
        graphSeqCdfdSeedEngine.set(162, 5.0, 1199);
        edge_gateway::GraphEmsEngine graphSeqCdfdEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_sequential_cdfd_power_solve_test.json"),
            graphSeqCdfdRouter,
            600000
        );
        graphSeqCdfdEngine.runOnce(1199);
        const auto graphSeqPhase = graphSeqCdfdRouter.getLatestByIndex(17, 1199);
        require(static_cast<bool>(graphSeqPhase), "graph sequential CD/FD phase state missing");
        requireNear(graphSeqPhase->value, 1.0, 0.0001, "graph sequential CD/FD should start discharging");
        const auto graphSeqFdRun = graphSeqCdfdRouter.getLatestByIndex(16, 1199);
        require(static_cast<bool>(graphSeqFdRun), "graph sequential FD run missing");
        requireNear(graphSeqFdRun->value, 1.0, 0.0001, "graph sequential FD run mismatch");
        for (const auto index : {627U, 628U, 629U}) {
            const auto graphSeqPhaseOut = graphSeqCdfdRouter.getLatestByIndex(index, 1199);
            require(static_cast<bool>(graphSeqPhaseOut), "graph sequential PCS phase output missing");
            requireNear(graphSeqPhaseOut->value, -10.0, 0.0001, "graph sequential PCS phase output mismatch");
        }

        writeTextFile(
            "graph_ems_charge_discharge_cycle_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "charge_discharge_cycle_test",
  "nodes": [
    {
      "id": "charge_discharge_test",
      "type": "chargeDischargeCycleTest",
      "enabled": true,
      "params": {
        "bmsSocIndex": 1570,
        "phasePowerA": 10.0,
        "phasePowerB": 10.0,
        "phasePowerC": 10.0,
        "dischargeDepth": 20.0,
        "chargeDepth": 95.0,
        "paOutput": 615,
        "pbOutput": 616,
        "pcOutput": 617,
        "p3Output": 618,
        "runOutput": 18,
        "phaseStateOutput": 17
      }
    },
    {
      "id": "power_solve",
      "type": "pcsPowerSolve",
      "enabled": true,
      "params": {
        "outPaDsIndex": 615,
        "outPbDsIndex": 616,
        "outPcDsIndex": 617,
        "pMaxIndex": 535,
        "qMaxIndex": 504,
        "s3MaxIndex": 151,
        "bmsSocIndex": 1570,
        "bmsSocMaxIndex": 161,
        "bmsSocMinIndex": 162,
        "paOutput": 627,
        "pbOutput": 628,
        "pcOutput": 629,
        "qaOutput": 630,
        "qbOutput": 631,
        "qcOutput": 632,
        "zrRunOutput": 24
      }
    }
  ],
  "edges": [
    { "from": "charge_discharge_test", "to": "power_solve" }
  ]
})json"
        );
        const auto cycleConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_cycle");
        edge_gateway::PointStoreRouter cycleRouter;
        cleanupStoreSegment(cycleConfig.memoryStore);
        edge_gateway::MemoryPointStore cycleStore(cycleConfig.memoryStore);
        cycleRouter.addStore(cycleConfig.memoryStore.sharedMemoryName, cycleStore);
        cycleRouter.addRoutesFromDeviceConfigs({cycleConfig}, cycleConfig.memoryStore.sharedMemoryName);
        edge_gateway::LegacyEmsEngine cycleSeedEngine(runtimeCatalog, cycleRouter);
        cycleSeedEngine.set(151, 30.0, 1200);
        cycleSeedEngine.set(535, 10.0, 1200);
        cycleSeedEngine.set(504, 10.0, 1200);
        cycleSeedEngine.set(161, 100.0, 1200);
        cycleSeedEngine.set(162, 5.0, 1200);
        cycleSeedEngine.set(1570, 97.0, 1200);
        edge_gateway::GraphEmsEngine cycleEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_charge_discharge_cycle_test.json"),
            cycleRouter,
            600000
        );

        cycleEngine.runOnce(1200);
        auto cyclePhase = cycleRouter.getLatestByIndex(17, 1200);
        require(static_cast<bool>(cyclePhase), "cycle phase state missing after initial discharge");
        requireNear(cyclePhase->value, 1.0, 0.0001, "cycle should start in discharge phase");
        for (const auto index : {627U, 628U, 629U}) {
            const auto phaseOut = cycleRouter.getLatestByIndex(index, 1200);
            require(static_cast<bool>(phaseOut), "cycle initial PCS phase output missing");
            requireNear(phaseOut->value, -10.0, 0.0001, "cycle initial discharge output mismatch");
        }

        cycleSeedEngine.set(1570, 20.0, 1210);
        cycleEngine.runOnce(1210);
        cyclePhase = cycleRouter.getLatestByIndex(17, 1210);
        require(static_cast<bool>(cyclePhase), "cycle phase state missing after discharge depth");
        requireNear(cyclePhase->value, 2.0, 0.0001, "cycle should switch to charge at discharge depth");
        for (const auto index : {627U, 628U, 629U}) {
            const auto phaseOut = cycleRouter.getLatestByIndex(index, 1210);
            require(static_cast<bool>(phaseOut), "cycle charge PCS phase output missing");
            requireNear(phaseOut->value, 10.0, 0.0001, "cycle charge output mismatch");
        }

        cycleSeedEngine.set(1570, 50.0, 1220);
        cycleEngine.runOnce(1220);
        cyclePhase = cycleRouter.getLatestByIndex(17, 1220);
        require(static_cast<bool>(cyclePhase), "cycle phase state missing while charging");
        requireNear(cyclePhase->value, 2.0, 0.0001, "cycle should keep charging between depth limits");
        for (const auto index : {627U, 628U, 629U}) {
            const auto phaseOut = cycleRouter.getLatestByIndex(index, 1220);
            require(static_cast<bool>(phaseOut), "cycle mid-charge PCS phase output missing");
            requireNear(phaseOut->value, 10.0, 0.0001, "cycle mid-charge output mismatch");
        }

        cycleSeedEngine.set(1570, 95.0, 1230);
        cycleEngine.runOnce(1230);
        cyclePhase = cycleRouter.getLatestByIndex(17, 1230);
        require(static_cast<bool>(cyclePhase), "cycle phase state missing after charge depth");
        requireNear(cyclePhase->value, 1.0, 0.0001, "cycle should loop back to discharge at charge depth");
        for (const auto index : {627U, 628U, 629U}) {
            const auto phaseOut = cycleRouter.getLatestByIndex(index, 1230);
            require(static_cast<bool>(phaseOut), "cycle next discharge PCS phase output missing");
            requireNear(phaseOut->value, -10.0, 0.0001, "cycle next discharge output mismatch");
        }

        writeTextFile(
            "graph_ems_legacy_nodes_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "legacy_nodes",
  "nodes": [
    {
      "id": "derived_load",
      "type": "derivedLoad",
      "enabled": true,
      "params": {
        "source": "tqCn",
        "tqPaIndex": 209,
        "tqPbIndex": 210,
        "tqPcIndex": 211,
        "tqP3Index": 212,
        "tqQaIndex": 213,
        "tqQbIndex": 214,
        "tqQcIndex": 215,
        "tqQ3Index": 216,
        "cnPaIndex": 259,
        "cnPbIndex": 260,
        "cnPcIndex": 261,
        "cnP3Index": 262,
        "cnQaIndex": 263,
        "cnQbIndex": 264,
        "cnQcIndex": 265,
        "cnQ3Index": 266
      }
    },
    {
      "id": "bms",
      "type": "bmsDerived",
      "enabled": true,
      "params": {
        "bmsModel": 1,
        "chargeCurrentAllowIndex": 1556,
        "dischargeCurrentAllowIndex": 1557,
        "voltageIndex": 1566,
        "chargeKwhSumIndex": 1586,
        "dischargeKwhSumIndex": 1587,
        "chargeKwhZeroIndex": 398,
        "dischargeKwhZeroIndex": 399,
        "chargeKwAllowOutput": 1552,
        "dischargeKwAllowOutput": 1553,
        "chargeKwhTodayOutput": 1615,
        "dischargeKwhTodayOutput": 1616
      }
    },
    {
      "id": "cos",
      "type": "cosCompensation",
      "enabled": true,
      "params": {
        "targetCosIndex": 514,
        "tqPaIndex": 209,
        "tqPbIndex": 210,
        "tqPcIndex": 211,
        "tqQaIndex": 213,
        "tqQbIndex": 214,
        "tqQcIndex": 215
      }
    },
    {
      "id": "lv_hv",
      "type": "voltageCompensation",
      "enabled": true,
      "params": {
        "cnUaIndex": 251,
        "cnUbIndex": 252,
        "cnUcIndex": 253,
        "lvLowIndex": 544,
        "lvUpIndex": 545,
        "hvLowIndex": 546,
        "hvUpIndex": 547,
        "gradPIndex": 533,
        "pMaxIndex": 535
      }
    },
    {
      "id": "cd_fd",
      "type": "chargeDischarge",
      "enabled": true,
      "params": {
        "bmsSocIndex": 1570,
        "cdTargetPowerIndex": 451,
        "cdTargetSocIndex": 452,
        "fdTargetPowerIndex": 455,
        "fdTargetSocIndex": 456,
        "positiveLimitIndex": 453,
        "positiveLimitEnableIndex": 454,
        "negativeLimitIndex": 457,
        "negativeLimitEnableIndex": 458,
        "fhP3Index": 312
      }
    },
    {
      "id": "sk",
      "type": "skOverride",
      "enabled": true,
      "params": {
        "skP3Index": 590,
        "skQ3Index": 591
      }
    }
  ],
  "edges": [
    { "from": "derived_load", "to": "cd_fd" }
  ]
})json"
        );
        const auto legacyNodesConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_legacy_nodes");
        edge_gateway::PointStoreRouter legacyNodesRouter;
        cleanupStoreSegment(legacyNodesConfig.memoryStore);
        edge_gateway::MemoryPointStore legacyNodesStore(legacyNodesConfig.memoryStore);
        legacyNodesRouter.addStore(legacyNodesConfig.memoryStore.sharedMemoryName, legacyNodesStore);
        legacyNodesRouter.addRoutesFromDeviceConfigs(
            {legacyNodesConfig},
            legacyNodesConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine legacyNodesEngine(
            runtimeCatalog,
            legacyNodesRouter,
            600000,
            {{"Meter_TQ", "1"}, {"Meter_CN", "1"}, {"Meter_BW", "0"}, {"Meter_FH", "0"}, {"BMS_MODEL", "1"}}
        );

        const auto graphNodesConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_nodes");
        edge_gateway::PointStoreRouter graphNodesRouter;
        cleanupStoreSegment(graphNodesConfig.memoryStore);
        edge_gateway::MemoryPointStore graphNodesStore(graphNodesConfig.memoryStore);
        graphNodesRouter.addStore(graphNodesConfig.memoryStore.sharedMemoryName, graphNodesStore);
        graphNodesRouter.addRoutesFromDeviceConfigs(
            {graphNodesConfig},
            graphNodesConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine graphNodesSeedEngine(runtimeCatalog, graphNodesRouter);
        const auto seedLegacyNodeInputs = [](auto& seed, std::int64_t ts) {
            seed.set(209, 30.0, ts);
            seed.set(210, 20.0, ts);
            seed.set(211, 10.0, ts);
            seed.set(212, 60.0, ts);
            seed.set(213, 30.0, ts);
            seed.set(214, 10.0, ts);
            seed.set(215, -20.0, ts);
            seed.set(216, 20.0, ts);
            seed.set(251, 220.0, ts);
            seed.set(252, 235.0, ts);
            seed.set(253, 250.0, ts);
            seed.set(259, 10.0, ts);
            seed.set(260, 5.0, ts);
            seed.set(261, 15.0, ts);
            seed.set(262, 30.0, ts);
            seed.set(263, 1.0, ts);
            seed.set(264, 2.0, ts);
            seed.set(265, 3.0, ts);
            seed.set(266, 6.0, ts);
            seed.set(514, 0.8, ts);
            seed.set(544, 225.0, ts);
            seed.set(545, 230.0, ts);
            seed.set(546, 240.0, ts);
            seed.set(547, 245.0, ts);
            seed.set(533, 5.0, ts);
            seed.set(535, 30.0, ts);
            seed.set(1570, 40.0, ts);
            seed.set(451, 25.0, ts);
            seed.set(452, 80.0, ts);
            seed.set(453, 50.0, ts);
            seed.set(454, 1.0, ts);
            seed.set(455, 18.0, ts);
            seed.set(456, 20.0, ts);
            seed.set(457, 60.0, ts);
            seed.set(458, 1.0, ts);
            seed.set(590, 90.0, ts);
            seed.set(591, 30.0, ts);
            seed.set(1556, 100.0, ts);
            seed.set(1557, 80.0, ts);
            seed.set(1566, 500.0, ts);
            seed.set(1586, 1200.0, ts);
            seed.set(1587, 900.0, ts);
            seed.set(398, 1000.0, ts);
            seed.set(399, 850.0, ts);
        };
        seedLegacyNodeInputs(legacyNodesEngine, 1210);
        seedLegacyNodeInputs(graphNodesSeedEngine, 1210);
        legacyNodesEngine.runOnce(1210);
        edge_gateway::GraphEmsEngine graphNodesEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_legacy_nodes_test.json"),
            graphNodesRouter,
            600000
        );
        graphNodesEngine.runOnce(1210);
        const std::uint32_t legacyNodeCompareIndexes[] = {
            309, 310, 311, 312, 313, 314, 315, 316,
            317, 318, 319, 320, 321, 322, 323, 324, 325,
            1552, 1553, 1615, 1616,
            505, 506, 507, 508, 601, 602, 603, 604, 8,
            605, 606, 607, 608, 609, 610, 611, 612, 10, 12,
            14, 16, 613, 614, 26
        };
        for (const auto index : legacyNodeCompareIndexes) {
            const auto legacyValue = legacyNodesRouter.getLatestByIndex(index, 1210);
            const auto graphValue = graphNodesRouter.getLatestByIndex(index, 1210);
            require(static_cast<bool>(legacyValue), "legacy node compare output missing");
            require(static_cast<bool>(graphValue), "graph legacy node output missing at index " + std::to_string(index));
            requireNear(graphValue->value, legacyValue->value, 0.001, "graph legacy node output mismatch at index " + std::to_string(index));
        }

        writeTextFile(
            "graph_ems_state_restore_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "state_restore",
  "nodes": [
    { "id": "ds", "type": "timedChargeDischarge", "enabled": true },
    { "id": "power_solve", "type": "pcsPowerSolve", "enabled": true },
    { "id": "cos", "type": "cosCompensation", "enabled": true },
    { "id": "lv_hv", "type": "voltageCompensation", "enabled": true },
    { "id": "cd_fd", "type": "chargeDischarge", "enabled": true },
    { "id": "gf", "type": "photovoltaicCharge", "enabled": true },
    { "id": "ph", "type": "phaseBalance", "enabled": true }
  ],
  "edges": []
})json"
        );
        const std::string graphStateFile = "graph_ems_state_restore_runtime.json";
        removeFileIfExists(graphStateFile);
        const auto graphStateConfig = edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_state_restore_test.json");
        const auto graphStateWriterConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_state_writer");
        edge_gateway::PointStoreRouter graphStateWriterRouter;
        cleanupStoreSegment(graphStateWriterConfig.memoryStore);
        edge_gateway::MemoryPointStore graphStateWriterStore(graphStateWriterConfig.memoryStore);
        graphStateWriterRouter.addStore(graphStateWriterConfig.memoryStore.sharedMemoryName, graphStateWriterStore);
        graphStateWriterRouter.addRoutesFromDeviceConfigs(
            {graphStateWriterConfig},
            graphStateWriterConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine graphStateWriterSeed(runtimeCatalog, graphStateWriterRouter);
        graphStateWriterSeed.set(615, 5.0, 1220);
        graphStateWriterSeed.set(616, 4.0, 1220);
        graphStateWriterSeed.set(617, 3.0, 1220);
        graphStateWriterSeed.set(618, 12.0, 1220);
        graphStateWriterSeed.set(601, 1.0, 1220);
        graphStateWriterSeed.set(602, 2.0, 1220);
        graphStateWriterSeed.set(603, 3.0, 1220);
        graphStateWriterSeed.set(604, 6.0, 1220);
        graphStateWriterSeed.set(627, 5.0, 1220);
        graphStateWriterSeed.set(628, 4.0, 1220);
        graphStateWriterSeed.set(629, 3.0, 1220);
        graphStateWriterSeed.set(630, 1.0, 1220);
        graphStateWriterSeed.set(631, 2.0, 1220);
        graphStateWriterSeed.set(632, 3.0, 1220);
        graphStateWriterSeed.set(8, 1.0, 1220);
        graphStateWriterSeed.set(10, 1.0, 1220);
        graphStateWriterSeed.set(12, 0.0, 1220);
        graphStateWriterSeed.set(18, 1.0, 1220);
        graphStateWriterSeed.set(20, 1.0, 1220);
        graphStateWriterSeed.set(22, 0.0, 1220);
        graphStateWriterSeed.set(24, 1.0, 1220);
        edge_gateway::GraphEmsEngine graphStateWriterEngine(
            graphStateConfig,
            graphStateWriterRouter,
            600000,
            graphStateFile
        );
        graphStateWriterEngine.runOnce(1220);

        const auto graphStateReaderConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_state_reader");
        edge_gateway::PointStoreRouter graphStateReaderRouter;
        cleanupStoreSegment(graphStateReaderConfig.memoryStore);
        edge_gateway::MemoryPointStore graphStateReaderStore(graphStateReaderConfig.memoryStore);
        graphStateReaderRouter.addStore(graphStateReaderConfig.memoryStore.sharedMemoryName, graphStateReaderStore);
        graphStateReaderRouter.addRoutesFromDeviceConfigs(
            {graphStateReaderConfig},
            graphStateReaderConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::GraphEmsEngine graphStateReaderEngine(
            graphStateConfig,
            graphStateReaderRouter,
            600000,
            graphStateFile
        );
        graphStateReaderEngine.runOnce(1230);
        const std::uint32_t restoredIndexes[] = {615, 616, 617, 618, 627, 628, 629, 630, 631, 632, 8, 10, 18, 20, 24};
        for (const auto index : restoredIndexes) {
            const auto restored = graphStateReaderRouter.getLatestByIndex(index, 1230);
            require(static_cast<bool>(restored), "graph EMS state restore missing index " + std::to_string(index));
        }
        requireNear(graphStateReaderRouter.getLatestByIndex(615, 1230)->value, 5.0, 0.0001, "restored OUT_PA_DS mismatch");
        requireNear(graphStateReaderRouter.getLatestByIndex(627, 1230)->value, 5.0, 0.0001, "restored PCS_PA_OUT mismatch");
        require(graphStateReaderRouter.peekPendingWrites(1).empty(), "graph EMS state restore should not submit device writes");

        const std::string corruptStateFile = "graph_ems_corrupt_state_test.json";
        writeTextFile(corruptStateFile, "{invalid-json");
        edge_gateway::GraphEmsEngine corruptStateEngine(
            graphStateConfig,
            graphStateReaderRouter,
            600000,
            corruptStateFile,
            {{"stateSaveIntervalMs", "0"}}
        );
        const auto corruptStateResult = corruptStateEngine.runOnce(1235);
        bool reportedRestoreError = false;
        for (const auto& error : corruptStateResult.errors) {
            if (error.find("restoreState:") != std::string::npos) {
                reportedRestoreError = true;
            }
        }
        require(reportedRestoreError, "corrupt graph state should report a restoreState error");
        std::ifstream repairedStateInput(corruptStateFile.c_str(), std::ios::in | std::ios::binary);
        std::stringstream repairedStateBuffer;
        repairedStateBuffer << repairedStateInput.rdbuf();
        require(
            repairedStateBuffer.str().find("\"schemaVersion\": \"1.1.0\"") != std::string::npos,
            "corrupt graph state should be replaced with a valid state document"
        );
        removeFileIfExists(corruptStateFile);

        const std::string graphStateDir = "tmp/graph_ems_state_dir";
        const std::string graphStateFileInDir = graphStateDir + "/graph_ems_state.json";
        removeFileIfExists(graphStateFileInDir);
        removeEmptyDirectoryIfExists(graphStateDir);
        edge_gateway::GraphEmsEngine graphStateDirEngine(
            graphStateConfig,
            graphStateWriterRouter,
            600000,
            graphStateFileInDir
        );
        const auto graphStateDirResult = graphStateDirEngine.runOnce(1240);
        for (const auto& error : graphStateDirResult.errors) {
            require(
                error.find("saveState:") == std::string::npos,
                "graph EMS state save should create parent directory without saveState error"
            );
        }
        std::ifstream graphStateDirInput(graphStateFileInDir.c_str(), std::ios::in | std::ios::binary);
        require(graphStateDirInput.is_open(), "graph EMS state file should be created in subdirectory");
        std::stringstream firstStateBuffer;
        firstStateBuffer << graphStateDirInput.rdbuf();
        graphStateDirInput.close();
        require(firstStateBuffer.str().find("\"savedAt\": 1240") != std::string::npos, "graph EMS first state timestamp mismatch");
        graphStateDirEngine.runOnce(1241);
        std::ifstream throttledStateInput(graphStateFileInDir.c_str(), std::ios::in | std::ios::binary);
        std::stringstream throttledStateBuffer;
        throttledStateBuffer << throttledStateInput.rdbuf();
        require(throttledStateBuffer.str().find("\"savedAt\": 1240") != std::string::npos, "graph EMS state writes should be throttled");
        graphStateDirEngine.runOnce(6240);
        std::ifstream refreshedStateInput(graphStateFileInDir.c_str(), std::ios::in | std::ios::binary);
        std::stringstream refreshedStateBuffer;
        refreshedStateBuffer << refreshedStateInput.rdbuf();
        require(refreshedStateBuffer.str().find("\"savedAt\": 6240") != std::string::npos, "graph EMS state should refresh after throttle interval");
        std::ifstream temporaryStateInput((graphStateFileInDir + ".tmp").c_str(), std::ios::in | std::ios::binary);
        require(!temporaryStateInput.is_open(), "graph EMS atomic state replacement must not leave a temporary file");

        std::ifstream shuntongSourceInput(
            "tools/testdata/shuntong_ems_generator_seed.json",
            std::ios::in | std::ios::binary
        );
        require(shuntongSourceInput.is_open(), "failed to open shuntong migration source graph");
        std::stringstream shuntongSourceBuffer;
        shuntongSourceBuffer << shuntongSourceInput.rdbuf();
        const auto shuntongSource = shuntongSourceBuffer.str();
        const auto hasSourceNode = [&](const std::string& id) {
            return shuntongSource.find("\"id\": \"" + id + "\"") != std::string::npos;
        };
        require(hasSourceNode("ds"), "shuntong migration source missing ds");
        require(hasSourceNode("gf"), "shuntong migration source missing gf");
        require(hasSourceNode("ph"), "shuntong migration source missing ph");
        require(hasSourceNode("force_full_charge"), "shuntong migration source missing force_full_charge");
        require(hasSourceNode("station_limit_v2"), "shuntong migration source missing station_limit_v2");
        require(hasSourceNode("energy_saving"), "shuntong migration source missing energy_saving");
        require(hasSourceNode("power_solve"), "shuntong migration source missing power_solve");
        require(hasSourceNode("pcs_writeback"), "shuntong migration source missing pcs_writeback");

        const auto modularGraphTemplate = edge_gateway::GraphEmsConfig::loadFromFile(
            "config/examples/shuntong_ems_modular_graph.json"
        );
        const std::set<std::string> modularNodeTypes = {
            "formula", "timeSource", "windowAggregate", "scheduleSelect", "phaseArbiter",
            "powerConstraint", "switch", "controlGate", "controlWrite", "sequence"
        };
        require(modularGraphTemplate.nodes.size() > 300, "modular shuntong graph should contain expanded algorithm nodes");
        require(modularGraphTemplate.nodes.size() <= 512, "modular shuntong graph exceeds runtime node limit");
        const auto findModularNode = [&](const std::string& id) -> const edge_gateway::GraphEmsNodeConfig* {
            for (const auto& node : modularGraphTemplate.nodes) {
                if (node.id == id) {
                    return &node;
                }
            }
            return nullptr;
        };
        const auto modularParam = [&](const std::string& nodeId, const std::string& key) -> std::string {
            const auto* node = findModularNode(nodeId);
            require(node != nullptr, "modular shuntong graph missing node: " + nodeId);
            const auto value = node->params.find(key);
            require(value != node->params.end(), "modular node " + nodeId + " missing param: " + key);
            return value->second;
        };
        for (const auto& node : modularGraphTemplate.nodes) {
            require(
                modularNodeTypes.find(node.type) != modularNodeTypes.end(),
                "modular shuntong graph contains a legacy or unsupported node: " + node.type
            );
            if (node.type == "controlWrite") {
                const auto submit = node.params.find("submitWrites");
                if (submit != node.params.end() && submit->second == "true") {
                    require(
                        node.params.find("optionalProfileKey") != node.params.end(),
                        "device write node must require an explicit optional profile: " + node.id
                    );
                }
            }
        }
        require(modularParam("schedule_select", "scheduleCurve.0.powerIndex") == "400", "schedule power index mapping mismatch");
        require(modularParam("schedule_select", "scheduleCurve.0.targetSocIndex") == "424", "schedule SOC index mapping mismatch");
        require(modularParam("schedule_select", "scheduleCurve.0.modeIndex") == "760", "schedule mode index mapping mismatch");
        require(modularParam("station_limit_positive_schedule", "scheduleCurve.0.powerIndex") == "700", "station positive hourly index mismatch");
        require(modularParam("station_limit_negative_schedule", "scheduleCurve.0.powerIndex") == "724", "station negative hourly index mismatch");
        require(modularParam("solar_tracking_per_phase", "inputs.0.index") == "582", "PV tracking index mapping mismatch");
        require(modularParam("power_constraints", "reserveMarginIndex") == "595", "reserve capacity index mapping mismatch");
        require(modularParam("lv_phase0_low_target", "inputs.0.index") == "1136", "LV realtime power index mismatch");
        require(modularParam("balance_load_average", "inputs.0.index") == "309", "phase balance load index mismatch");
        require(modularParam("force_full_day_of_month", "component") == "dayOfMonth", "force-full day source mismatch");
        require(modularParam("force_full_run", "outputIndex") == "30", "force-full run feedback mismatch");

        std::vector<std::uint32_t> cycleSafeOutputIndexes;
        for (const auto* id : {"cycle_safe_p0", "cycle_safe_p1", "cycle_safe_p2", "cycle_safe_q0", "cycle_safe_q1", "cycle_safe_q2"}) {
            cycleSafeOutputIndexes.push_back(static_cast<std::uint32_t>(std::stoul(modularParam(id, "outputIndex"))));
        }
        edge_gateway::DeviceIdentity modularIdentity;
        const auto modularVirtualConfig = edge_gateway::ConfigLoader::loadFromFile(
            "config/examples/device_ems_modular_virtual.json",
            modularIdentity
        );
        require(
            modularVirtualConfig.memoryStore.sharedMemoryName == "gateway_point_store_ems_virtual",
            "modular virtual point config should use the EMS virtual shared memory"
        );

        auto modularCycleConfig = modularVirtualConfig;
        modularCycleConfig.memoryStore.sharedMemoryName = "legacy_ems_test_store_modular_cycle";
        cleanupStoreSegment(modularCycleConfig.memoryStore);
        edge_gateway::MemoryPointStore modularCycleStore(modularCycleConfig.memoryStore);
        edge_gateway::PointStoreRouter modularCycleRouter;
        modularCycleRouter.addStore(modularCycleConfig.memoryStore.sharedMemoryName, modularCycleStore);
        modularCycleRouter.addRoutesFromDeviceConfigs(
            {modularCycleConfig},
            modularCycleConfig.memoryStore.sharedMemoryName
        );
        addRouteIfMissing(
            modularCycleRouter,
            1570,
            "stack_soc",
            modularCycleConfig.memoryStore.sharedMemoryName,
            false
        );
        edge_gateway::LegacyEmsEngine modularCycleSeedEngine(runtimeCatalog, modularCycleRouter);
        edge_gateway::GraphEmsEngine modularCycleEngine(
            modularGraphTemplate,
            modularCycleRouter,
            600000,
            std::string(),
            {
                {"Meter_TQ", "1"}, {"Meter_CN", "1"}, {"Meter_BW", "0"}, {"Meter_FH", "0"},
                {"BMS_MODEL", "2"}, {"CHARGE_DISCHARGE_TEST", "1"}
            }
        );
        const auto verifyCycle = [&](double soc, double expectedPhase, double expectedPower, std::int64_t ts) {
            modularCycleSeedEngine.set(1570, soc, ts);
            const auto result = modularCycleEngine.runOnce(ts);
            require(
                result.errors.empty(),
                "full modular graph cycle run should not report node errors" +
                    (result.errors.empty() ? std::string() : ": " + result.errors.front())
            );
            const auto phase = modularCycleRouter.getLatestByIndex(17, ts);
            const auto phasePower = modularCycleRouter.getLatestByIndex(615, ts);
            const auto totalPower = modularCycleRouter.getLatestByIndex(618, ts);
            require(static_cast<bool>(phase), "full modular graph cycle phase output missing");
            require(static_cast<bool>(phasePower), "full modular graph cycle phase-A output missing");
            require(static_cast<bool>(totalPower), "full modular graph cycle total output missing");
            requireNear(
                phase->value,
                expectedPhase,
                0.0001,
                "full modular graph cycle phase mismatch soc=" + std::to_string(soc) +
                    " expected=" + std::to_string(expectedPhase) +
                    " actual=" + std::to_string(phase->value)
            );
            requireNear(
                phasePower->value,
                expectedPower,
                0.0001,
                "full modular graph cycle phase-A power mismatch actual=" + std::to_string(phasePower->value)
            );
            requireNear(
                totalPower->value,
                std::abs(expectedPower) * 3.0,
                0.0001,
                "full modular graph cycle total power mismatch actual=" + std::to_string(totalPower->value)
            );
            require(result.deviceWrites == 0, "failed cycle safety conditions must block all device writes");
            for (const auto safeOutputIndex : cycleSafeOutputIndexes) {
                const auto safeOutput = modularCycleRouter.getLatestByIndex(safeOutputIndex, ts);
                require(static_cast<bool>(safeOutput), "cycle safety output missing");
                requireNear(safeOutput->value, 0.0, 0.0001, "failed cycle safety condition must clear output");
            }
        };
        verifyCycle(80.0, 1.0, -10.0, 1200);
        verifyCycle(20.0, 2.0, 10.0, 1210);
        verifyCycle(50.0, 2.0, 10.0, 1220);
        verifyCycle(95.0, 1.0, -10.0, 1230);

        const auto runModularProfileScenario = [&] (
            const std::string& name,
            const std::unordered_map<std::string, std::string>& profile,
            const std::function<void(
                edge_gateway::LegacyEmsEngine&,
                edge_gateway::GraphEmsEngine&,
                edge_gateway::PointStoreRouter&
            )>& scenario
        ) {
            auto physicalConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_modular_" + name);
            auto virtualConfig = modularVirtualConfig;
            virtualConfig.memoryStore.sharedMemoryName = physicalConfig.memoryStore.sharedMemoryName;
            cleanupStoreSegment(physicalConfig.memoryStore);
            edge_gateway::MemoryPointStore store(physicalConfig.memoryStore);
            edge_gateway::PointStoreRouter scenarioRouter;
            scenarioRouter.addStore(physicalConfig.memoryStore.sharedMemoryName, store);
            scenarioRouter.addRoutesFromDeviceConfigs(
                {physicalConfig},
                physicalConfig.memoryStore.sharedMemoryName
            );
            addRouteIfMissing(
                scenarioRouter,
                156,
                "avg_window",
                physicalConfig.memoryStore.sharedMemoryName,
                false
            );
            for (const auto& meter : virtualConfig.meters) {
                for (const auto& point : meter.points) {
                    addRouteIfMissing(
                        scenarioRouter,
                        point.index,
                        point.pointCode,
                        physicalConfig.memoryStore.sharedMemoryName,
                        false
                    );
                }
            }
            edge_gateway::LegacyEmsEngine seedEngine(runtimeCatalog, scenarioRouter);
            edge_gateway::GraphEmsEngine graphEngine(
                modularGraphTemplate,
                scenarioRouter,
                600000,
                std::string(),
                profile
            );
            scenario(seedEngine, graphEngine, scenarioRouter);
        };

        runModularProfileScenario(
            "tq_off",
            {{"Meter_TQ", "0"}, {"Meter_CN", "1"}, {"Meter_BW", "0"}, {"Meter_FH", "0"}, {"BMS_MODEL", "2"}},
            [](edge_gateway::LegacyEmsEngine& seed, edge_gateway::GraphEmsEngine& engine,
               edge_gateway::PointStoreRouter& scenarioRouter) {
                seed.set(156, 1.0, 2000);
                seed.set(1036, 30.0, 2000);
                const auto result = engine.runOnce(2000);
                require(
                    result.errors.empty(),
                    "modular Meter_TQ=0 run reported errors" +
                        (result.errors.empty() ? std::string() : ": " + result.errors.front())
                );
                const auto value = scenarioRouter.getLatestByIndex(209, 2000);
                require(!value || value->ts != 2000, "modular Meter_TQ=0 should skip TQ average outputs");
            }
        );
        runModularProfileScenario(
            "cn_off",
            {{"Meter_TQ", "1"}, {"Meter_CN", "0"}, {"Meter_BW", "0"}, {"Meter_FH", "0"}, {"BMS_MODEL", "2"}},
            [](edge_gateway::LegacyEmsEngine& seed, edge_gateway::GraphEmsEngine& engine,
               edge_gateway::PointStoreRouter& scenarioRouter) {
                seed.set(156, 1.0, 2100);
                seed.set(1136, 18.0, 2100);
                const auto result = engine.runOnce(2100);
                require(result.errors.empty(), "modular Meter_CN=0 run reported errors");
                const auto value = scenarioRouter.getLatestByIndex(259, 2100);
                require(!value || value->ts != 2100, "modular Meter_CN=0 should skip CN average outputs");
            }
        );
        runModularProfileScenario(
            "bw",
            {{"Meter_TQ", "1"}, {"Meter_CN", "0"}, {"Meter_BW", "1"}, {"Meter_FH", "0"}, {"BMS_MODEL", "2"}},
            [](edge_gateway::LegacyEmsEngine& seed, edge_gateway::GraphEmsEngine& engine,
               edge_gateway::PointStoreRouter& scenarioRouter) {
                seed.set(156, 1.0, 2200);
                seed.set(1036, 30.0, 2200);
                seed.set(4537, 8.0, 2200);
                const auto result = engine.runOnce(2200);
                require(result.errors.empty(), "modular Meter_BW=1 run reported errors");
                const auto tqValue = scenarioRouter.getLatestByIndex(209, 2200);
                const auto bwValue = scenarioRouter.getLatestByIndex(700034, 2200);
                require(static_cast<bool>(tqValue), "modular Meter_BW=1 TQ average prerequisite missing");
                require(static_cast<bool>(bwValue), "modular Meter_BW=1 BW average prerequisite missing");
                const auto value = scenarioRouter.getLatestByIndex(309, 2200);
                require(static_cast<bool>(value), "modular Meter_BW=1 FH output missing");
                requireNear(value->value, 22.0, 0.0001, "modular Meter_BW=1 FH output mismatch");
            }
        );
        runModularProfileScenario(
            "fh_direct",
            {{"Meter_TQ", "1"}, {"Meter_CN", "1"}, {"Meter_BW", "0"}, {"Meter_FH", "1"}, {"BMS_MODEL", "2"}},
            [](edge_gateway::LegacyEmsEngine& seed, edge_gateway::GraphEmsEngine& engine,
               edge_gateway::PointStoreRouter& scenarioRouter) {
                seed.set(309, 42.0, 2300);
                seed.set(1036, 30.0, 2300);
                seed.set(1136, 8.0, 2300);
                const auto result = engine.runOnce(2400);
                require(result.errors.empty(), "modular Meter_FH=1 run reported errors");
                const auto value = scenarioRouter.getLatestByIndex(309, 2400);
                require(static_cast<bool>(value), "modular Meter_FH=1 physical FH output missing");
                requireNear(value->value, 42.0, 0.0001, "modular Meter_FH=1 must not overwrite physical FH data");
                require(value->ts == 2300, "modular Meter_FH=1 should preserve physical FH timestamp");
            }
        );
        runModularProfileScenario(
            "bms3",
            {{"Meter_TQ", "0"}, {"Meter_CN", "0"}, {"Meter_BW", "0"}, {"Meter_FH", "0"}, {"BMS_MODEL", "3"}},
            [](edge_gateway::LegacyEmsEngine& seed, edge_gateway::GraphEmsEngine& engine,
               edge_gateway::PointStoreRouter& scenarioRouter) {
                seed.set(1586, 1200.0, 2500);
                seed.set(1587, 900.0, 2500);
                seed.set(398, 1000.0, 2500);
                seed.set(399, 850.0, 2500);
                const auto result = engine.runOnce(2500);
                require(result.errors.empty(), "modular BMS_MODEL=3 run reported errors");
                requireNear(scenarioRouter.getLatestByIndex(1615, 2500)->value, 200.0, 0.0001,
                            "modular BMS_MODEL=3 charge energy mismatch");
                requireNear(scenarioRouter.getLatestByIndex(1616, 2500)->value, 50.0, 0.0001,
                            "modular BMS_MODEL=3 discharge energy mismatch");
            }
        );
        runModularProfileScenario(
            "lv_hv_realtime",
            { {"Meter_TQ", "0"}, {"Meter_CN", "1"}, {"Meter_BW", "0"}, {"Meter_FH", "0"}, {"BMS_MODEL", "2"} },
            [](edge_gateway::LegacyEmsEngine& seed, edge_gateway::GraphEmsEngine& engine,
               edge_gateway::PointStoreRouter& scenarioRouter) {
                const std::int64_t ts = 2600;
                seed.set(156, 1.0, ts);
                seed.set(1130, 210.0, ts);
                seed.set(1131, 260.0, ts);
                seed.set(1132, 235.0, ts);
                seed.set(1136, -20.0, ts);
                seed.set(1137, 20.0, ts);
                seed.set(1138, 0.0, ts);
                seed.set(544, 225.0, ts);
                seed.set(545, 230.0, ts);
                seed.set(546, 240.0, ts);
                seed.set(547, 245.0, ts);
                seed.set(533, 5.0, ts);
                seed.set(535, 30.0, ts);
                seed.set(504, 30.0, ts);
                seed.set(151, 90.0, ts);
                seed.set(1552, 90.0, ts);
                seed.set(1553, 90.0, ts);
                seed.set(1570, 50.0, ts);
                seed.set(161, 95.0, ts);
                seed.set(162, 20.0, ts);
                const auto result = engine.runOnce(ts);
                require(result.errors.empty(), "modular LV/HV realtime run reported errors");
                requireNear(scenarioRouter.getLatestByIndex(605, ts)->value, -30.0, 0.0001,
                            "LV severe deviation should scale realtime power and clamp to -Pmax");
                requireNear(scenarioRouter.getLatestByIndex(610, ts)->value, 30.0, 0.0001,
                            "HV severe deviation should scale realtime power and clamp to Pmax");
                requireNear(scenarioRouter.getLatestByIndex(10, ts)->value, 1.0, 0.0001,
                            "LV intervention feedback should be set");
                requireNear(scenarioRouter.getLatestByIndex(12, ts)->value, 1.0, 0.0001,
                            "HV intervention feedback should be set");
                require(result.deviceWrites == 0, "modular LV/HV shadow run must not submit device writes");
            }
        );
        runModularProfileScenario(
            "phase_balance",
            { {"Meter_TQ", "1"}, {"Meter_CN", "1"}, {"Meter_BW", "0"}, {"Meter_FH", "1"}, {"BMS_MODEL", "2"} },
            [](edge_gateway::LegacyEmsEngine& seed, edge_gateway::GraphEmsEngine& engine,
               edge_gateway::PointStoreRouter& scenarioRouter) {
                const auto seedBalance = [&](std::int64_t ts, double a, double b, double c) {
                    seed.set(209, a, ts);
                    seed.set(210, b, ts);
                    seed.set(211, c, ts);
                    seed.set(309, a, ts);
                    seed.set(310, b, ts);
                    seed.set(311, c, ts);
                    seed.set(562, 10.0, ts);
                };
                seedBalance(2700, 0.0, 0.0, 0.0);
                auto result = engine.runOnce(2700);
                require(result.errors.empty(), "zero-load phase-balance run reported errors");
                requireNear(scenarioRouter.getLatestByIndex(564, 2700)->value, 0.0, 0.0001,
                            "zero-load phase-balance ratio must be zero");
                requireNear(scenarioRouter.getLatestByIndex(623, 2700)->value, 0.0, 0.0001,
                            "zero-load phase-balance output must be zero");

                seedBalance(2800, 30.0, 10.0, 20.0);
                result = engine.runOnce(2800);
                require(result.errors.empty(), "normal phase-balance run reported errors");
                requireNear(scenarioRouter.getLatestByIndex(564, 2800)->value, 200.0 / 3.0, 0.0001,
                            "phase-balance percentage mismatch");
                requireNear(scenarioRouter.getLatestByIndex(623, 2800)->value, -10.0, 0.0001,
                            "phase-balance A correction mismatch");
                requireNear(scenarioRouter.getLatestByIndex(624, 2800)->value, 10.0, 0.0001,
                            "phase-balance B correction mismatch");
                requireNear(scenarioRouter.getLatestByIndex(625, 2800)->value, 0.0, 0.0001,
                            "phase-balance C correction mismatch");
                require(result.deviceWrites == 0, "phase-balance shadow run must not submit device writes");
            }
        );
        runModularProfileScenario(
            "solar_tracking",
            { {"Meter_TQ", "1"}, {"Meter_CN", "1"}, {"Meter_BW", "0"}, {"Meter_FH", "1"}, {"BMS_MODEL", "2"} },
            [](edge_gateway::LegacyEmsEngine& seed, edge_gateway::GraphEmsEngine& engine,
               edge_gateway::PointStoreRouter& scenarioRouter) {
                setTestTimezone("UTC");
                const std::int64_t ts = 12LL * 3600000LL;
                seed.set(309, 10.0, ts);
                seed.set(310, 20.0, ts);
                seed.set(311, 40.0, ts);
                seed.set(581, 8.0, ts);
                seed.set(582, 90.0, ts);
                seed.set(583, 18.0, ts);
                const auto result = engine.runOnce(ts);
                require(result.errors.empty(), "solar tracking run reported errors");
                requireNear(scenarioRouter.getLatestByIndex(619, ts)->value, 20.0, 0.0001,
                            "solar phase-A output must use trackingPower/3");
                requireNear(scenarioRouter.getLatestByIndex(620, ts)->value, 10.0, 0.0001,
                            "solar phase-B output must use trackingPower/3");
                requireNear(scenarioRouter.getLatestByIndex(621, ts)->value, 0.0, 0.0001,
                            "solar phase-C output must stop when load exceeds per-phase tracking power");
                require(result.deviceWrites == 0, "solar shadow run must not submit device writes");
            }
        );
        runModularProfileScenario(
            "station_limit_v2",
            {
                {"Meter_TQ", "0"}, {"Meter_CN", "0"}, {"Meter_BW", "0"}, {"Meter_FH", "0"},
                {"BMS_MODEL", "2"}, {"STATION_LIMIT_V2", "1"}
            },
            [](edge_gateway::LegacyEmsEngine& seed, edge_gateway::GraphEmsEngine& engine,
               edge_gateway::PointStoreRouter& scenarioRouter) {
                const auto seedHourlyValues = [&](std::int64_t ts, double positive, double negative) {
                    for (std::uint32_t index = 700; index <= 723; ++index) {
                        seed.set(index, positive, ts);
                    }
                    for (std::uint32_t index = 724; index <= 747; ++index) {
                        seed.set(index, negative, ts);
                    }
                    seed.set(475, 65535.0, ts);
                    seed.set(476, 255.0, ts);
                    seed.set(477, 65535.0, ts);
                    seed.set(478, 255.0, ts);
                };

                seedHourlyValues(3000, 88.0, -12.0);
                seed.set(27, 1.0, 3000);
                seed.set(28, 0.0, 3000);
                seed.set(471, 1.0, 3000);
                seed.set(472, 123.0, 3000);
                seed.set(473, 1.0, 3000);
                seed.set(474, -45.0, 3000);
                auto result = engine.runOnce(3000);
                require(
                    result.errors.empty(),
                    "station fixed limit run reported errors" +
                        (result.errors.empty() ? std::string() : ": " + result.errors.front())
                );
                requireNear(scenarioRouter.getLatestByIndex(453, 3000)->value, 123.0, 0.0001, "station fixed positive value mismatch");
                requireNear(scenarioRouter.getLatestByIndex(454, 3000)->value, 1.0, 0.0001, "station fixed positive enable mismatch");
                requireNear(scenarioRouter.getLatestByIndex(457, 3000)->value, -45.0, 0.0001, "station fixed negative value mismatch");
                requireNear(scenarioRouter.getLatestByIndex(458, 3000)->value, 1.0, 0.0001, "station fixed negative enable mismatch");

                seedHourlyValues(4000, 88.0, -12.0);
                seed.set(28, 1.0, 4000);
                result = engine.runOnce(4000);
                require(result.errors.empty(), "station hourly limit run reported errors");
                requireNear(scenarioRouter.getLatestByIndex(453, 4000)->value, 88.0, 0.0001, "station hourly positive value mismatch");
                requireNear(scenarioRouter.getLatestByIndex(454, 4000)->value, 1.0, 0.0001, "station hourly positive enable mismatch");
                requireNear(scenarioRouter.getLatestByIndex(457, 4000)->value, -12.0, 0.0001, "station hourly negative value mismatch");
                requireNear(scenarioRouter.getLatestByIndex(458, 4000)->value, 1.0, 0.0001, "station hourly negative enable mismatch");

                seed.set(27, 0.0, 5000);
                result = engine.runOnce(5000);
                require(result.errors.empty(), "disabled station limit run reported errors");
                requireNear(scenarioRouter.getLatestByIndex(453, 5000)->value, 0.0, 0.0001, "disabled station positive value must clear");
                requireNear(scenarioRouter.getLatestByIndex(454, 5000)->value, 0.0, 0.0001, "disabled station positive enable must clear");
                requireNear(scenarioRouter.getLatestByIndex(457, 5000)->value, 0.0, 0.0001, "disabled station negative value must clear");
                requireNear(scenarioRouter.getLatestByIndex(458, 5000)->value, 0.0, 0.0001, "disabled station negative enable must clear");
            }
        );
        runModularProfileScenario(
            "force_full_charge",
            {
                {"Meter_TQ", "0"}, {"Meter_CN", "0"}, {"Meter_BW", "0"}, {"Meter_FH", "0"},
                {"BMS_MODEL", "2"}, {"CHARGE_DISCHARGE_TEST", "0"}, {"PCS_ENERGY_SAVING", "0"},
                {"LIQUID_COOLING_ENERGY_SAVING", "0"}, {"PCS_AUTO_RESET", "0"}
            },
            [](edge_gateway::LegacyEmsEngine& seed, edge_gateway::GraphEmsEngine& engine,
               edge_gateway::PointStoreRouter& scenarioRouter) {
                setTestTimezone("UTC");
                const auto seedForceInputs = [&](std::int64_t ts, double soc) {
                    seed.set(6, 1.0, ts);
                    seed.set(29, 1.0, ts);
                    seed.set(168, 15.0, ts);
                    seed.set(169, 17.0, ts);
                    seed.set(1570, soc, ts);
                    seed.set(535, 30.0, ts);
                    seed.set(504, 30.0, ts);
                    seed.set(151, 90.0, ts);
                    seed.set(1552, 100.0, ts);
                    seed.set(1553, 100.0, ts);
                    seed.set(161, 95.0, ts);
                    seed.set(162, 20.0, ts);
                    for (const auto index : {8U, 10U, 12U, 18U, 20U, 22U}) {
                        seed.set(index, 0.0, ts);
                    }
                };
                const auto runAndRequireNoWrites = [&](std::int64_t ts, const std::string& stage) {
                    const auto result = engine.runOnce(ts);
                    require(
                        result.errors.empty(),
                        "force-full " + stage + " reported errors" +
                            (result.errors.empty() ? std::string() : ": " + result.errors.front())
                    );
                    require(result.deviceWrites == 0, "force-full shadow graph must not submit device writes: " + stage);
                };

                const auto day15 = 14LL * 86400000LL + 3600000LL;
                seedForceInputs(day15, 80.0);
                runAndRequireNoWrites(day15, "initial trigger");
                requireNear(scenarioRouter.getLatestByIndex(30, day15)->value, 1.0, 0.0001, "force-full run feedback should start");
                requireNear(scenarioRouter.getLatestByIndex(627, day15)->value, 20.0, 0.0001, "force-full phase-A power mismatch");
                requireNear(scenarioRouter.getLatestByIndex(628, day15)->value, 20.0, 0.0001, "force-full phase-B power mismatch");
                requireNear(scenarioRouter.getLatestByIndex(629, day15)->value, 20.0, 0.0001, "force-full phase-C power mismatch");

                seedForceInputs(day15 + 1000, 100.0);
                runAndRequireNoWrites(day15 + 1000, "SOC completion");
                requireNear(scenarioRouter.getLatestByIndex(30, day15 + 1000)->value, 0.0, 0.0001, "force-full should stop at 100 percent SOC");

                seedForceInputs(day15 + 2000, 80.0);
                runAndRequireNoWrites(day15 + 2000, "same-day latch");
                requireNear(scenarioRouter.getLatestByIndex(30, day15 + 2000)->value, 0.0, 0.0001, "force-full must not restart on the same date");

                const auto day16 = day15 + 86400000LL;
                seedForceInputs(day16, 80.0);
                runAndRequireNoWrites(day16, "off-date reset");
                requireNear(scenarioRouter.getLatestByIndex(30, day16)->value, 0.0, 0.0001, "force-full off-date reset must remain idle");

                const auto day17 = day16 + 86400000LL;
                seedForceInputs(day17, 80.0);
                runAndRequireNoWrites(day17, "second configured date");
                requireNear(scenarioRouter.getLatestByIndex(30, day17)->value, 1.0, 0.0001, "force-full should trigger on the second configured date");
            }
        );

        edge_gateway::ComputeRuleConfig rule;
        rule.ruleCode = "legacy_ems_test_rule";
        rule.enabled = true;
        rule.trigger.type = "interval";
        rule.trigger.intervalMs = 1;
        rule.script.type = "legacyEms";
        rule.script.legacyGlListFile = runtimeCatalogFixture.glListFile;
        rule.script.legacyVarListFile = runtimeCatalogFixture.varListFile;
        rule.script.legacyEncoding = "gbk";

        edge_gateway::ComputeEngineConfig computeConfig;
        computeConfig.enabled = true;
        computeConfig.scanIntervalMs = 1;
        computeConfig.defaultOutputTtlMs = 600000;
        computeConfig.rules.push_back(rule);

        bool productionRejectedLegacy = false;
        try {
            edge_gateway::ComputeEngineService service(computeConfig, router);
        } catch (const std::invalid_argument& ex) {
            const std::string message = ex.what();
            productionRejectedLegacy =
                message.find("legacyEms") != std::string::npos &&
                message.find("schemaVersion 2.x") != std::string::npos;
        }
        require(
            productionRejectedLegacy,
            "production ComputeEngineService must reject legacyEms and direct migration to schemaVersion 2.x"
        );

        const auto tqOffConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_tq_off");
        edge_gateway::PointStoreRouter tqOffRouter;
        cleanupStoreSegment(tqOffConfig.memoryStore);
        edge_gateway::MemoryPointStore tqOffStore(tqOffConfig.memoryStore);
        tqOffRouter.addStore(tqOffConfig.memoryStore.sharedMemoryName, tqOffStore);
        tqOffRouter.addRoutesFromDeviceConfigs({tqOffConfig}, tqOffConfig.memoryStore.sharedMemoryName);
        edge_gateway::LegacyEmsEngine tqOffEngine(
            runtimeCatalog,
            tqOffRouter,
            600000,
            {{"Meter_TQ", "0"}, {"Meter_CN", "1"}, {"Meter_BW", "0"}, {"Meter_FH", "0"}, {"BMS_MODEL", "2"}}
        );
        tqOffEngine.set(156, 2.0, 2000);
        tqOffEngine.set(1030, 220.0, 2000);
        tqOffEngine.set(1136, 10.0, 2000);
        tqOffEngine.runOnce(2000);
        const auto tqOffAvg = tqOffRouter.getLatestByIndex(201, 2000);
        require(!static_cast<bool>(tqOffAvg) || tqOffAvg->ts != 2000, "Meter_TQ=0 should skip TQ averages");
        const auto tqOffFh = tqOffRouter.getLatestByIndex(309, 2000);
        require(!static_cast<bool>(tqOffFh) || tqOffFh->ts != 2000, "Meter_TQ=0 should skip FH derived values");

        const auto cnOffConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_cn_off");
        edge_gateway::PointStoreRouter cnOffRouter;
        cleanupStoreSegment(cnOffConfig.memoryStore);
        edge_gateway::MemoryPointStore cnOffStore(cnOffConfig.memoryStore);
        cnOffRouter.addStore(cnOffConfig.memoryStore.sharedMemoryName, cnOffStore);
        cnOffRouter.addRoutesFromDeviceConfigs({cnOffConfig}, cnOffConfig.memoryStore.sharedMemoryName);
        edge_gateway::LegacyEmsEngine cnOffEngine(
            runtimeCatalog,
            cnOffRouter,
            600000,
            {{"Meter_TQ", "1"}, {"Meter_CN", "0"}, {"Meter_BW", "0"}, {"Meter_FH", "0"}, {"BMS_MODEL", "2"}}
        );
        cnOffEngine.set(156, 2.0, 2100);
        cnOffEngine.set(1030, 220.0, 2100);
        cnOffEngine.set(1036, 30.0, 2100);
        cnOffEngine.set(1037, 20.0, 2100);
        cnOffEngine.set(1038, 10.0, 2100);
        cnOffEngine.set(1039, 60.0, 2100);
        cnOffEngine.set(1040, 4.0, 2100);
        cnOffEngine.set(1041, 3.0, 2100);
        cnOffEngine.set(1042, 0.0, 2100);
        cnOffEngine.set(1043, 5.0, 2100);
        cnOffEngine.set(1136, 10.0, 2100);
        cnOffEngine.runOnce(2100);
        const auto cnOffAvg = cnOffRouter.getLatestByIndex(259, 2100);
        require(!static_cast<bool>(cnOffAvg) || cnOffAvg->ts != 2100, "Meter_CN=0 should skip CN averages");
        const auto cnOffFh = cnOffRouter.getLatestByIndex(309, 2100);
        require(!static_cast<bool>(cnOffFh) || cnOffFh->ts != 2100, "Meter_CN=0 should skip FH derived values");

        const auto bmsConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_bms");
        edge_gateway::PointStoreRouter bmsRouter;
        cleanupStoreSegment(bmsConfig.memoryStore);
        edge_gateway::MemoryPointStore bmsStore(bmsConfig.memoryStore);
        bmsRouter.addStore(bmsConfig.memoryStore.sharedMemoryName, bmsStore);
        bmsRouter.addRoutesFromDeviceConfigs({bmsConfig}, bmsConfig.memoryStore.sharedMemoryName);
        edge_gateway::LegacyEmsEngine bmsEngine(
            runtimeCatalog,
            bmsRouter,
            600000,
            {{"Meter_TQ", "1"}, {"Meter_CN", "1"}, {"Meter_BW", "0"}, {"Meter_FH", "0"}, {"BMS_MODEL", "1"}}
        );
        bmsEngine.set(1586, 1200.0, 2200);
        bmsEngine.set(1587, 900.0, 2200);
        bmsEngine.set(398, 1000.0, 2200);
        bmsEngine.set(399, 850.0, 2200);
        bmsEngine.runOnce(2200);
        const auto chargeToday = bmsRouter.getLatestByIndex(1615, 2200);
        require(static_cast<bool>(chargeToday), "BMS_MODEL=1 should write stack charge today");
        requireNear(chargeToday->value, 200.0, 0.0001, "stack charge today mismatch");
        const auto dischargeToday = bmsRouter.getLatestByIndex(1616, 2200);
        require(static_cast<bool>(dischargeToday), "BMS_MODEL=1 should write stack discharge today");
        requireNear(dischargeToday->value, 50.0, 0.0001, "stack discharge today mismatch");

        const auto bwConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_bw");
        edge_gateway::PointStoreRouter bwRouter;
        cleanupStoreSegment(bwConfig.memoryStore);
        edge_gateway::MemoryPointStore bwStore(bwConfig.memoryStore);
        bwRouter.addStore(bwConfig.memoryStore.sharedMemoryName, bwStore);
        bwRouter.addRoutesFromDeviceConfigs({bwConfig}, bwConfig.memoryStore.sharedMemoryName);
        edge_gateway::LegacyEmsEngine bwEngine(
            runtimeCatalog,
            bwRouter,
            600000,
            {{"Meter_TQ", "1"}, {"Meter_CN", "0"}, {"Meter_BW", "1"}, {"Meter_FH", "0"}, {"BMS_MODEL", "2"}}
        );
        bwEngine.set(156, 2.0, 2300);
        bwEngine.set(1036, 30.0, 2300);
        bwEngine.set(1037, 20.0, 2300);
        bwEngine.set(1038, 10.0, 2300);
        bwEngine.set(1039, 60.0, 2300);
        bwEngine.set(1040, 4.0, 2300);
        bwEngine.set(1041, 3.0, 2300);
        bwEngine.set(1042, 0.0, 2300);
        bwEngine.set(1043, 5.0, 2300);
        bwEngine.set(4537, 8.0, 2300);
        bwEngine.set(4538, 6.0, 2300);
        bwEngine.set(4539, 4.0, 2300);
        bwEngine.set(4536, 18.0, 2300);
        bwEngine.set(4541, 1.0, 2300);
        bwEngine.set(4542, 1.0, 2300);
        bwEngine.set(4543, 1.0, 2300);
        bwEngine.set(4540, 3.0, 2300);
        bwEngine.runOnce(2300);
        const auto bwTqPa = bwRouter.getLatestByIndex(209, 2300);
        require(static_cast<bool>(bwTqPa), "Meter_BW=1 test setup missing TQ_avg_PA");
        const auto bwFhPa = bwRouter.getLatestByIndex(309, 2300);
        require(
            static_cast<bool>(bwFhPa),
            "Meter_BW=1 should derive FH_avg_PA after TQ_avg_PA=" +
            std::to_string(bwTqPa ? bwTqPa->value : -9999.0)
        );
        requireNear(bwFhPa->value, 22.0, 0.0001, "FH_avg_PA should equal TQ_avg_PA - BW_avg_PA");

        const auto cosConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_cos");
        edge_gateway::PointStoreRouter cosRouter;
        cleanupStoreSegment(cosConfig.memoryStore);
        edge_gateway::MemoryPointStore cosStore(cosConfig.memoryStore);
        cosRouter.addStore(cosConfig.memoryStore.sharedMemoryName, cosStore);
        cosRouter.addRoutesFromDeviceConfigs({cosConfig}, cosConfig.memoryStore.sharedMemoryName);
        edge_gateway::LegacyEmsEngine cosEngine(
            runtimeCatalog,
            cosRouter,
            600000,
            {{"Meter_TQ", "1"}, {"Meter_CN", "1"}, {"Meter_BW", "0"}, {"Meter_FH", "0"}, {"BMS_MODEL", "2"}}
        );
        cosEngine.set(156, 2.0, 2400);
        cosEngine.set(1036, 30.0, 2400);
        cosEngine.set(1037, 20.0, 2400);
        cosEngine.set(1038, 10.0, 2400);
        cosEngine.set(1039, 60.0, 2400);
        cosEngine.set(1040, 4.0, 2400);
        cosEngine.set(1041, 3.0, 2400);
        cosEngine.set(1042, 2.0, 2400);
        cosEngine.set(1043, 9.0, 2400);
        cosEngine.set(514, 0.8, 2400);
        cosEngine.runOnce(2400);
        const auto cosQa = cosRouter.getLatestByIndex(505, 2400);
        require(static_cast<bool>(cosQa), "COS target should write target QA");
        requireNear(cosQa->value, 22.5, 0.001, "target QA mismatch");
        const auto cosQ3 = cosRouter.getLatestByIndex(508, 2400);
        require(static_cast<bool>(cosQ3), "COS target should write target Q3");
        requireNear(cosQ3->value, 45.0, 0.001, "target Q3 mismatch");
        const auto cosRun = cosRouter.getLatestByIndex(8, 2400);
        require(static_cast<bool>(cosRun), "COS target should write run flag");
        requireNear(cosRun->value, 0.0, 0.0001, "COS run flag should be 0 when no compensation is needed");

        const auto cosQaOut = cosRouter.getLatestByIndex(601, 2400);
        require(static_cast<bool>(cosQaOut), "COS target should write OUT_QA_COS");
        requireNear(cosQaOut->value, 0.0, 0.0001, "OUT_QA_COS should be 0 when no compensation is needed");

        const auto lvhvConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_lvhv");
        edge_gateway::PointStoreRouter lvhvRouter;
        cleanupStoreSegment(lvhvConfig.memoryStore);
        edge_gateway::MemoryPointStore lvhvStore(lvhvConfig.memoryStore);
        lvhvRouter.addStore(lvhvConfig.memoryStore.sharedMemoryName, lvhvStore);
        lvhvRouter.addRoutesFromDeviceConfigs({lvhvConfig}, lvhvConfig.memoryStore.sharedMemoryName);
        edge_gateway::LegacyEmsEngine lvhvEngine(
            runtimeCatalog,
            lvhvRouter,
            600000,
            {{"Meter_TQ", "1"}, {"Meter_CN", "1"}, {"Meter_BW", "0"}, {"Meter_FH", "0"}, {"BMS_MODEL", "2"}}
        );
        lvhvEngine.set(156, 2.0, 2500);
        lvhvEngine.set(1130, 220.0, 2500);
        lvhvEngine.set(1131, 235.0, 2500);
        lvhvEngine.set(1132, 250.0, 2500);
        lvhvEngine.set(544, 225.0, 2500);
        lvhvEngine.set(545, 230.0, 2500);
        lvhvEngine.set(546, 240.0, 2500);
        lvhvEngine.set(547, 245.0, 2500);
        lvhvEngine.set(533, 5.0, 2500);
        lvhvEngine.set(535, 30.0, 2500);
        lvhvEngine.runOnce(2500);
        const auto lvhvCnUa = lvhvRouter.getLatestByIndex(251, 2500);
        require(static_cast<bool>(lvhvCnUa), "LV/HV setup missing CN_avg_UA");
        const auto lvRun = lvhvRouter.getLatestByIndex(10, 2500);
        require(
            static_cast<bool>(lvRun),
            "LV run flag missing after CN_avg_UA=" + std::to_string(lvhvCnUa ? lvhvCnUa->value : -9999.0)
        );
        requireNear(lvRun->value, 1.0, 0.0001, "LV run flag should be 1");
        const auto hvRun = lvhvRouter.getLatestByIndex(12, 2500);
        require(static_cast<bool>(hvRun), "HV run flag missing");
        requireNear(hvRun->value, 1.0, 0.0001, "HV run flag should be 1");
        const auto outPaLv = lvhvRouter.getLatestByIndex(605, 2500);
        require(static_cast<bool>(outPaLv), "OUT_PA_LV missing");
        requireNear(outPaLv->value, -5.0, 0.0001, "OUT_PA_LV mismatch");
        const auto outPcHv = lvhvRouter.getLatestByIndex(611, 2500);
        require(static_cast<bool>(outPcHv), "OUT_PC_HV missing");
        requireNear(outPcHv->value, 5.0, 0.0001, "OUT_PC_HV mismatch");

        const auto cdfdConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_cdfd");
        edge_gateway::PointStoreRouter cdfdRouter;
        cleanupStoreSegment(cdfdConfig.memoryStore);
        edge_gateway::MemoryPointStore cdfdStore(cdfdConfig.memoryStore);
        cdfdRouter.addStore(cdfdConfig.memoryStore.sharedMemoryName, cdfdStore);
        cdfdRouter.addRoutesFromDeviceConfigs({cdfdConfig}, cdfdConfig.memoryStore.sharedMemoryName);
        edge_gateway::LegacyEmsEngine cdfdEngine(
            runtimeCatalog,
            cdfdRouter,
            600000,
            {{"Meter_TQ", "1"}, {"Meter_CN", "1"}, {"Meter_BW", "0"}, {"Meter_FH", "0"}, {"BMS_MODEL", "2"}}
        );
        cdfdEngine.set(156, 2.0, 2600);
        cdfdEngine.set(1036, 30.0, 2600);
        cdfdEngine.set(1037, 20.0, 2600);
        cdfdEngine.set(1038, 10.0, 2600);
        cdfdEngine.set(1039, 60.0, 2600);
        cdfdEngine.set(1040, 4.0, 2600);
        cdfdEngine.set(1041, 3.0, 2600);
        cdfdEngine.set(1042, 0.0, 2600);
        cdfdEngine.set(1043, 5.0, 2600);
        cdfdEngine.set(1136, 10.0, 2600);
        cdfdEngine.set(1137, 5.0, 2600);
        cdfdEngine.set(1138, 15.0, 2600);
        cdfdEngine.set(1139, 30.0, 2600);
        cdfdEngine.set(1140, 1.0, 2600);
        cdfdEngine.set(1141, 1.0, 2600);
        cdfdEngine.set(1142, 1.0, 2600);
        cdfdEngine.set(1143, 3.0, 2600);
        cdfdEngine.set(1570, 40.0, 2600);
        cdfdEngine.set(451, 25.0, 2600);
        cdfdEngine.set(452, 80.0, 2600);
        cdfdEngine.set(453, 50.0, 2600);
        cdfdEngine.set(454, 1.0, 2600);
        cdfdEngine.set(455, 18.0, 2600);
        cdfdEngine.set(456, 20.0, 2600);
        cdfdEngine.set(458, 1.0, 2600);
        cdfdEngine.runOnce(2600);
        const auto cdfdFhP3 = cdfdRouter.getLatestByIndex(312, 2600);
        require(static_cast<bool>(cdfdFhP3), "CD/FD setup missing FH_avg_P3");
        const auto cdRun = cdfdRouter.getLatestByIndex(14, 2600);
        require(
            static_cast<bool>(cdRun),
            "CD run flag missing after FH_avg_P3=" + std::to_string(cdfdFhP3 ? cdfdFhP3->value : -9999.0)
        );
        requireNear(cdRun->value, 1.0, 0.0001, "CD run flag should be 1");
        const auto outP3Cd = cdfdRouter.getLatestByIndex(613, 2600);
        require(static_cast<bool>(outP3Cd), "OUT_P3_CD missing");
        requireNear(outP3Cd->value, 20.0, 0.0001, "OUT_P3_CD mismatch");
        const auto fdRun = cdfdRouter.getLatestByIndex(16, 2600);
        require(static_cast<bool>(fdRun), "FD run flag missing");
        requireNear(fdRun->value, 1.0, 0.0001, "FD run flag should be 1");
        const auto outP3Fd = cdfdRouter.getLatestByIndex(614, 2600);
        require(static_cast<bool>(outP3Fd), "OUT_P3_FD missing");
        requireNear(outP3Fd->value, -18.0, 0.0001, "OUT_P3_FD mismatch");

        const auto solveConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_solve");
        edge_gateway::PointStoreRouter solveRouter;
        cleanupStoreSegment(solveConfig.memoryStore);
        edge_gateway::MemoryPointStore solveStore(solveConfig.memoryStore);
        solveRouter.addStore(solveConfig.memoryStore.sharedMemoryName, solveStore);
        solveRouter.addRoutesFromDeviceConfigs({solveConfig}, solveConfig.memoryStore.sharedMemoryName);
        edge_gateway::LegacyEmsEngine solveEngine(
            runtimeCatalog,
            solveRouter,
            600000,
            {{"Meter_TQ", "1"}, {"Meter_CN", "1"}, {"Meter_BW", "0"}, {"Meter_FH", "0"}, {"BMS_MODEL", "2"}}
        );
        solveEngine.set(156, 2.0, 2700);
        solveEngine.set(1036, 30.0, 2700);
        solveEngine.set(1037, 20.0, 2700);
        solveEngine.set(1038, 10.0, 2700);
        solveEngine.set(1039, 60.0, 2700);
        solveEngine.set(1040, 30.0, 2700);
        solveEngine.set(1041, 3.0, 2700);
        solveEngine.set(1042, 2.0, 2700);
        solveEngine.set(1043, 35.0, 2700);
        solveEngine.set(1130, 220.0, 2700);
        solveEngine.set(1131, 235.0, 2700);
        solveEngine.set(1132, 250.0, 2700);
        solveEngine.set(1136, 10.0, 2700);
        solveEngine.set(1137, 5.0, 2700);
        solveEngine.set(1138, 15.0, 2700);
        solveEngine.set(1139, 30.0, 2700);
        solveEngine.set(1140, 1.0, 2700);
        solveEngine.set(1141, 1.0, 2700);
        solveEngine.set(1142, 1.0, 2700);
        solveEngine.set(1143, 3.0, 2700);
        solveEngine.set(514, 0.8, 2700);
        solveEngine.set(544, 225.0, 2700);
        solveEngine.set(545, 230.0, 2700);
        solveEngine.set(546, 240.0, 2700);
        solveEngine.set(547, 245.0, 2700);
        solveEngine.set(533, 5.0, 2700);
        solveEngine.set(535, 30.0, 2700);
        solveEngine.runOnce(2700);
        const auto pcsPaOut = solveRouter.getLatestByIndex(627, 2700);
        require(static_cast<bool>(pcsPaOut), "PCS_PA_OUT missing");
        requireNear(pcsPaOut->value, -5.0, 0.0001, "PCS_PA_OUT should prefer LV output");
        const auto pcsPcOut = solveRouter.getLatestByIndex(629, 2700);
        require(static_cast<bool>(pcsPcOut), "PCS_PC_OUT missing");
        requireNear(pcsPcOut->value, 5.0, 0.0001, "PCS_PC_OUT should prefer HV output");
        const auto pcsQaOut = solveRouter.getLatestByIndex(630, 2700);
        require(static_cast<bool>(pcsQaOut), "PCS_QA_OUT missing");
        requireNear(pcsQaOut->value, 7.5, 0.001, "PCS_QA_OUT should equal OUT_QA_COS");

        const auto solveAdvancedConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_solve_advanced");
        edge_gateway::PointStoreRouter solveAdvancedRouter;
        cleanupStoreSegment(solveAdvancedConfig.memoryStore);
        edge_gateway::MemoryPointStore solveAdvancedStore(solveAdvancedConfig.memoryStore);
        solveAdvancedRouter.addStore(solveAdvancedConfig.memoryStore.sharedMemoryName, solveAdvancedStore);
        solveAdvancedRouter.addRoutesFromDeviceConfigs(
            {solveAdvancedConfig},
            solveAdvancedConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine solveAdvancedEngine(
            runtimeCatalog,
            solveAdvancedRouter,
            600000,
            {{"Meter_TQ", "1"}, {"Meter_CN", "1"}, {"Meter_BW", "0"}, {"Meter_FH", "0"}, {"BMS_MODEL", "2"}}
        );
        solveAdvancedEngine.set(156, 2.0, 2800);
        solveAdvancedEngine.set(1036, 30.0, 2800);
        solveAdvancedEngine.set(1037, 20.0, 2800);
        solveAdvancedEngine.set(1038, 10.0, 2800);
        solveAdvancedEngine.set(1039, 60.0, 2800);
        solveAdvancedEngine.set(1040, 4.0, 2800);
        solveAdvancedEngine.set(1041, 3.0, 2800);
        solveAdvancedEngine.set(1042, 0.0, 2800);
        solveAdvancedEngine.set(1043, 5.0, 2800);
        solveAdvancedEngine.set(1130, 230.0, 2800);
        solveAdvancedEngine.set(1131, 230.0, 2800);
        solveAdvancedEngine.set(1132, 230.0, 2800);
        solveAdvancedEngine.set(1136, 10.0, 2800);
        solveAdvancedEngine.set(1137, 5.0, 2800);
        solveAdvancedEngine.set(1138, 15.0, 2800);
        solveAdvancedEngine.set(1139, 30.0, 2800);
        solveAdvancedEngine.set(1140, 1.0, 2800);
        solveAdvancedEngine.set(1141, 1.0, 2800);
        solveAdvancedEngine.set(1142, 1.0, 2800);
        solveAdvancedEngine.set(1143, 3.0, 2800);
        solveAdvancedEngine.set(581, 0.0, 2800);
        solveAdvancedEngine.set(583, 23.0, 2800);
        solveAdvancedEngine.set(562, 10.0, 2800);
        solveAdvancedEngine.set(457, 30.0, 2800);
        solveAdvancedEngine.set(590, 90.0, 2800);
        solveAdvancedEngine.set(591, 30.0, 2800);
        solveAdvancedEngine.runOnce(2800);

        const auto phRun = solveAdvancedRouter.getLatestByIndex(20, 2800);
        require(static_cast<bool>(phRun), "PH run flag missing");
        requireNear(phRun->value, 1.0, 0.0001, "PH run flag should be 1");
        const auto outPaPh = solveAdvancedRouter.getLatestByIndex(623, 2800);
        require(static_cast<bool>(outPaPh), "OUT_PA_PH missing");
        requireNear(outPaPh->value, 6.5, 0.0001, "OUT_PA_PH mismatch");
        const auto outPbPh = solveAdvancedRouter.getLatestByIndex(624, 2800);
        require(static_cast<bool>(outPbPh), "OUT_PB_PH missing");
        requireNear(outPbPh->value, -6.5, 0.0001, "OUT_PB_PH mismatch");
        const auto outPcPh = solveAdvancedRouter.getLatestByIndex(625, 2800);
        require(static_cast<bool>(outPcPh), "OUT_PC_PH missing");
        requireNear(outPcPh->value, 0.0, 0.0001, "OUT_PC_PH mismatch");

        const auto gfRun = solveAdvancedRouter.getLatestByIndex(22, 2800);
        require(static_cast<bool>(gfRun), "GF run flag missing");
        requireNear(gfRun->value, 1.0, 0.0001, "GF run flag should be 1");
        const auto outPaGf = solveAdvancedRouter.getLatestByIndex(619, 2800);
        require(static_cast<bool>(outPaGf), "OUT_PA_GF missing");
        requireNear(outPaGf->value, 10.0, 0.0001, "OUT_PA_GF mismatch");

        const auto skRun = solveAdvancedRouter.getLatestByIndex(26, 2800);
        require(static_cast<bool>(skRun), "SK run flag missing");
        requireNear(skRun->value, 1.0, 0.0001, "SK run flag should be 1");

        const auto solveAdvancedPa = solveAdvancedRouter.getLatestByIndex(627, 2800);
        require(static_cast<bool>(solveAdvancedPa), "advanced PCS_PA_OUT missing");
        requireNear(solveAdvancedPa->value, 30.0, 0.0001, "SK should override PCS_PA_OUT");
        const auto solveAdvancedPb = solveAdvancedRouter.getLatestByIndex(628, 2800);
        require(static_cast<bool>(solveAdvancedPb), "advanced PCS_PB_OUT missing");
        requireNear(solveAdvancedPb->value, 30.0, 0.0001, "SK should override PCS_PB_OUT");
        const auto solveAdvancedPc = solveAdvancedRouter.getLatestByIndex(629, 2800);
        require(static_cast<bool>(solveAdvancedPc), "advanced PCS_PC_OUT missing");
        requireNear(solveAdvancedPc->value, 30.0, 0.0001, "SK should override PCS_PC_OUT");
        const auto solveAdvancedQa = solveAdvancedRouter.getLatestByIndex(630, 2800);
        require(static_cast<bool>(solveAdvancedQa), "advanced PCS_QA_OUT missing");
        requireNear(solveAdvancedQa->value, 10.0, 0.0001, "SK should override PCS_QA_OUT");
        const auto solveAdvancedQb = solveAdvancedRouter.getLatestByIndex(631, 2800);
        require(static_cast<bool>(solveAdvancedQb), "advanced PCS_QB_OUT missing");
        requireNear(solveAdvancedQb->value, 10.0, 0.0001, "SK should override PCS_QB_OUT");
        const auto solveAdvancedQc = solveAdvancedRouter.getLatestByIndex(632, 2800);
        require(static_cast<bool>(solveAdvancedQc), "advanced PCS_QC_OUT missing");
        requireNear(solveAdvancedQc->value, 10.0, 0.0001, "SK should override PCS_QC_OUT");

        const auto dsConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_ds");
        edge_gateway::PointStoreRouter dsRouter;
        cleanupStoreSegment(dsConfig.memoryStore);
        edge_gateway::MemoryPointStore dsStore(dsConfig.memoryStore);
        dsRouter.addStore(dsConfig.memoryStore.sharedMemoryName, dsStore);
        dsRouter.addRoutesFromDeviceConfigs({dsConfig}, dsConfig.memoryStore.sharedMemoryName);
        edge_gateway::LegacyEmsEngine dsEngine(
            runtimeCatalog,
            dsRouter,
            600000,
            {{"Meter_TQ", "1"}, {"Meter_CN", "1"}, {"Meter_BW", "0"}, {"Meter_FH", "0"}, {"BMS_MODEL", "2"}}
        );
        dsEngine.set(156, 2.0, 0);
        dsEngine.set(1130, 230.0, 0);
        dsEngine.set(1131, 230.0, 0);
        dsEngine.set(1132, 230.0, 0);
        dsEngine.set(1136, 10.0, 0);
        dsEngine.set(1137, 5.0, 0);
        dsEngine.set(1138, 15.0, 0);
        dsEngine.set(1139, 30.0, 0);
        dsEngine.set(1140, 1.0, 0);
        dsEngine.set(1141, 1.0, 0);
        dsEngine.set(1142, 1.0, 0);
        dsEngine.set(1143, 3.0, 0);
        dsEngine.set(1570, 20.0, 0);
        dsEngine.set(400, 30.0, 0);
        dsEngine.set(424, 80.0, 0);
        dsEngine.set(760, 0.0, 0);
        dsEngine.set(463, 250.0, 0);
        dsEngine.set(464, 220.0, 0);
        dsEngine.set(533, 5.0, 0);
        dsEngine.runOnce(0);

        const auto dsPowerNow = dsRouter.getLatestByIndex(461, 0);
        require(static_cast<bool>(dsPowerNow), "DS power now missing");
        requireNear(dsPowerNow->value, 30.0, 0.0001, "DS power now mismatch");
        const auto dsSocNow = dsRouter.getLatestByIndex(462, 0);
        require(static_cast<bool>(dsSocNow), "DS soc now missing");
        requireNear(dsSocNow->value, 80.0, 0.0001, "DS soc now mismatch");
        const auto dsRun = dsRouter.getLatestByIndex(18, 0);
        require(static_cast<bool>(dsRun), "DS run flag missing");
        requireNear(dsRun->value, 1.0, 0.0001, "DS run flag should be 1");
        const auto outPaDs = dsRouter.getLatestByIndex(615, 0);
        require(static_cast<bool>(outPaDs), "OUT_PA_DS missing");
        requireNear(outPaDs->value, 5.0, 0.0001, "OUT_PA_DS mismatch");
        const auto outP3Ds = dsRouter.getLatestByIndex(618, 0);
        require(static_cast<bool>(outP3Ds), "OUT_P3_DS missing");
        requireNear(outP3Ds->value, 15.0, 0.0001, "OUT_P3_DS mismatch");
        const auto dsPcsPa = dsRouter.getLatestByIndex(627, 0);
        require(static_cast<bool>(dsPcsPa), "DS PCS_PA_OUT missing");
        requireNear(dsPcsPa->value, 5.0, 0.0001, "DS should seed PCS_PA_OUT");

        const auto dsLocalHourConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_ds_local_hour");
        edge_gateway::PointStoreRouter dsLocalHourRouter;
        cleanupStoreSegment(dsLocalHourConfig.memoryStore);
        edge_gateway::MemoryPointStore dsLocalHourStore(dsLocalHourConfig.memoryStore);
        dsLocalHourRouter.addStore(dsLocalHourConfig.memoryStore.sharedMemoryName, dsLocalHourStore);
        dsLocalHourRouter.addRoutesFromDeviceConfigs(
            {dsLocalHourConfig},
            dsLocalHourConfig.memoryStore.sharedMemoryName
        );
        addRouteIfMissing(
            dsLocalHourRouter,
            405,
            "ds_power_5",
            dsLocalHourConfig.memoryStore.sharedMemoryName,
            false
        );
        addRouteIfMissing(
            dsLocalHourRouter,
            429,
            "ds_soc_5",
            dsLocalHourConfig.memoryStore.sharedMemoryName,
            false
        );
        addRouteIfMissing(
            dsLocalHourRouter,
            765,
            "ds_mode_5",
            dsLocalHourConfig.memoryStore.sharedMemoryName,
            false
        );
        edge_gateway::LegacyEmsEngine dsLocalHourEngine(
            runtimeCatalog,
            dsLocalHourRouter,
            600000,
            {{"Meter_TQ", "1"}, {"Meter_CN", "1"}, {"Meter_BW", "0"}, {"Meter_FH", "0"}, {"BMS_MODEL", "2"}}
        );
        setTestTimezone("CST-8");
        const std::int64_t localHour5Ms = 21LL * 3600000LL;
        dsLocalHourEngine.set(1130, 230.0, localHour5Ms);
        dsLocalHourEngine.set(1131, 230.0, localHour5Ms);
        dsLocalHourEngine.set(1132, 230.0, localHour5Ms);
        dsLocalHourEngine.set(1136, 10.0, localHour5Ms);
        dsLocalHourEngine.set(1137, 5.0, localHour5Ms);
        dsLocalHourEngine.set(1138, 15.0, localHour5Ms);
        dsLocalHourEngine.set(1139, 30.0, localHour5Ms);
        dsLocalHourEngine.set(1140, 1.0, localHour5Ms);
        dsLocalHourEngine.set(1141, 1.0, localHour5Ms);
        dsLocalHourEngine.set(1142, 1.0, localHour5Ms);
        dsLocalHourEngine.set(1143, 3.0, localHour5Ms);
        dsLocalHourEngine.set(1570, 20.0, localHour5Ms);
        dsLocalHourEngine.set(405, 45.0, localHour5Ms);
        dsLocalHourEngine.set(429, 80.0, localHour5Ms);
        dsLocalHourEngine.set(765, 0.0, localHour5Ms);
        dsLocalHourEngine.set(463, 250.0, localHour5Ms);
        dsLocalHourEngine.set(464, 220.0, localHour5Ms);
        dsLocalHourEngine.set(533, 5.0, localHour5Ms);
        dsLocalHourEngine.runOnce(localHour5Ms);

        const auto dsLocalHourPowerNow = dsLocalHourRouter.getLatestByIndex(461, localHour5Ms);
        require(static_cast<bool>(dsLocalHourPowerNow), "DS local-hour power now missing");
        requireNear(dsLocalHourPowerNow->value, 45.0, 0.0001, "DS should use local hour schedule");
        const auto dsLocalHourRun = dsLocalHourRouter.getLatestByIndex(18, localHour5Ms);
        require(static_cast<bool>(dsLocalHourRun), "DS local-hour run flag missing");
        requireNear(dsLocalHourRun->value, 1.0, 0.0001, "DS local-hour run flag should be 1");
        setTestTimezone("UTC");

        const auto clampConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_clamp");
        edge_gateway::PointStoreRouter clampRouter;
        cleanupStoreSegment(clampConfig.memoryStore);
        edge_gateway::MemoryPointStore clampStore(clampConfig.memoryStore);
        clampRouter.addStore(clampConfig.memoryStore.sharedMemoryName, clampStore);
        clampRouter.addRoutesFromDeviceConfigs({clampConfig}, clampConfig.memoryStore.sharedMemoryName);
        edge_gateway::LegacyEmsEngine clampEngine(
            runtimeCatalog,
            clampRouter,
            600000,
            {{"Meter_TQ", "1"}, {"Meter_CN", "1"}, {"Meter_BW", "0"}, {"Meter_FH", "0"}, {"BMS_MODEL", "2"}}
        );
        clampEngine.set(156, 2.0, 1000);
        clampEngine.set(1036, 47.0, 1000);
        clampEngine.set(1037, 39.0, 1000);
        clampEngine.set(1038, 37.0, 1000);
        clampEngine.set(1039, 123.0, 1000);
        clampEngine.set(1040, 20.0, 1000);
        clampEngine.set(1041, 20.0, 1000);
        clampEngine.set(1042, 20.0, 1000);
        clampEngine.set(1043, 60.0, 1000);
        clampEngine.set(1130, 230.0, 1000);
        clampEngine.set(1131, 230.0, 1000);
        clampEngine.set(1132, 230.0, 1000);
        clampEngine.set(1136, 10.0, 1000);
        clampEngine.set(1137, 5.0, 1000);
        clampEngine.set(1138, 15.0, 1000);
        clampEngine.set(1139, 30.0, 1000);
        clampEngine.set(1140, 1.0, 1000);
        clampEngine.set(1141, 1.0, 1000);
        clampEngine.set(1142, 1.0, 1000);
        clampEngine.set(1143, 3.0, 1000);
        clampEngine.set(457, 30.0, 1000);
        clampEngine.set(458, 1.0, 1000);
        clampEngine.set(454, 1.0, 1000);
        clampEngine.set(451, 50.0, 1000);
        clampEngine.set(452, 80.0, 1000);
        clampEngine.set(455, 50.0, 1000);
        clampEngine.set(456, 10.0, 1000);
        clampEngine.set(1570, 40.0, 1000);
        clampEngine.set(23, 1.0, 1000);
        clampEngine.set(588, 5.0, 1000);
        clampEngine.set(504, 20.0, 1000);
        clampEngine.set(535, 50.0, 1000);
        clampEngine.set(514, 0.2, 1000);
        clampEngine.set(151, 300.0, 1000);
        clampEngine.set(1552, 12.0, 1000);
        clampEngine.set(1553, 9.0, 1000);
        clampEngine.runOnce(1000);

        const auto zrRun = clampRouter.getLatestByIndex(24, 1000);
        require(static_cast<bool>(zrRun), "ZR run flag missing");
        requireNear(zrRun->value, 1.0, 0.0001, "ZR run flag should be 1");
        const auto clampPa = clampRouter.getLatestByIndex(627, 1000);
        require(static_cast<bool>(clampPa), "clamp PCS_PA_OUT missing");
        requireNear(clampPa->value, -5.14286, 0.001, "PCS_PA_OUT should be clamped by BMS discharge limit");
        const auto clampPb = clampRouter.getLatestByIndex(628, 1000);
        require(static_cast<bool>(clampPb), "clamp PCS_PB_OUT missing");
        requireNear(clampPb->value, -3.85714, 0.001, "PCS_PB_OUT should be clamped by BMS discharge limit");
        const auto clampPc = clampRouter.getLatestByIndex(629, 1000);
        require(static_cast<bool>(clampPc), "clamp PCS_PC_OUT missing");
        requireNear(clampPc->value, 0.0, 0.001, "PCS_PC_OUT should be clamped by BMS discharge limit");

        const auto lowSocConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_low_soc");
        edge_gateway::PointStoreRouter lowSocRouter;
        cleanupStoreSegment(lowSocConfig.memoryStore);
        edge_gateway::MemoryPointStore lowSocStore(lowSocConfig.memoryStore);
        lowSocRouter.addStore(lowSocConfig.memoryStore.sharedMemoryName, lowSocStore);
        lowSocRouter.addRoutesFromDeviceConfigs({lowSocConfig}, lowSocConfig.memoryStore.sharedMemoryName);
        edge_gateway::LegacyEmsEngine lowSocEngine(
            runtimeCatalog,
            lowSocRouter,
            600000,
            {{"Meter_TQ", "1"}, {"Meter_CN", "1"}, {"Meter_BW", "0"}, {"Meter_FH", "0"}, {"BMS_MODEL", "2"}}
        );
        lowSocEngine.set(156, 2.0, 2000);
        lowSocEngine.set(1036, 30.0, 2000);
        lowSocEngine.set(1037, 20.0, 2000);
        lowSocEngine.set(1038, 10.0, 2000);
        lowSocEngine.set(1039, 60.0, 2000);
        lowSocEngine.set(1040, 20.0, 2000);
        lowSocEngine.set(1041, 20.0, 2000);
        lowSocEngine.set(1042, 20.0, 2000);
        lowSocEngine.set(1043, 60.0, 2000);
        lowSocEngine.set(1130, 230.0, 2000);
        lowSocEngine.set(1131, 230.0, 2000);
        lowSocEngine.set(1132, 230.0, 2000);
        lowSocEngine.set(1136, 10.0, 2000);
        lowSocEngine.set(1137, 5.0, 2000);
        lowSocEngine.set(1138, 15.0, 2000);
        lowSocEngine.set(1139, 30.0, 2000);
        lowSocEngine.set(1140, 1.0, 2000);
        lowSocEngine.set(1141, 1.0, 2000);
        lowSocEngine.set(1142, 1.0, 2000);
        lowSocEngine.set(1143, 3.0, 2000);
        lowSocEngine.set(457, 30.0, 2000);
        lowSocEngine.set(458, 1.0, 2000);
        lowSocEngine.set(504, 20.0, 2000);
        lowSocEngine.set(535, 50.0, 2000);
        lowSocEngine.set(151, 60.0, 2000);
        lowSocEngine.set(1570, 5.0, 2000);
        lowSocEngine.set(161, 95.0, 2000);
        lowSocEngine.set(162, 10.0, 2000);
        lowSocEngine.set(8, 1.0, 2000);
        lowSocEngine.set(10, 1.0, 2000);
        lowSocEngine.runOnce(2000);

        const auto lowSocPa = lowSocRouter.getLatestByIndex(627, 2000);
        require(static_cast<bool>(lowSocPa), "low SOC PCS_PA_OUT missing");
        requireNear(lowSocPa->value, 0.0, 0.0001, "low SOC should block discharge");
        const auto lowSocQa = lowSocRouter.getLatestByIndex(630, 2000);
        require(static_cast<bool>(lowSocQa), "low SOC PCS_QA_OUT missing");
        requireNear(lowSocQa->value, 0.0, 0.0001, "low SOC should clear reactive output");
        const auto lowSocCosRun = lowSocRouter.getLatestByIndex(8, 2000);
        require(static_cast<bool>(lowSocCosRun), "low SOC COS run flag missing");
        requireNear(lowSocCosRun->value, 0.0, 0.0001, "low SOC should clear COS run flag");
        const auto lowSocLvRun = lowSocRouter.getLatestByIndex(10, 2000);
        require(static_cast<bool>(lowSocLvRun), "low SOC LV run flag missing");
        requireNear(lowSocLvRun->value, 0.0, 0.0001, "low SOC should clear LV run flag");

        const auto highSocConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_high_soc");
        edge_gateway::PointStoreRouter highSocRouter;
        cleanupStoreSegment(highSocConfig.memoryStore);
        edge_gateway::MemoryPointStore highSocStore(highSocConfig.memoryStore);
        highSocRouter.addStore(highSocConfig.memoryStore.sharedMemoryName, highSocStore);
        highSocRouter.addRoutesFromDeviceConfigs({highSocConfig}, highSocConfig.memoryStore.sharedMemoryName);
        edge_gateway::LegacyEmsEngine highSocEngine(
            runtimeCatalog,
            highSocRouter,
            600000,
            {{"Meter_TQ", "1"}, {"Meter_CN", "1"}, {"Meter_BW", "0"}, {"Meter_FH", "0"}, {"BMS_MODEL", "2"}}
        );
        highSocEngine.set(156, 2.0, 3000);
        highSocEngine.set(1036, 30.0, 3000);
        highSocEngine.set(1037, 20.0, 3000);
        highSocEngine.set(1038, 10.0, 3000);
        highSocEngine.set(1039, 60.0, 3000);
        highSocEngine.set(1040, 4.0, 3000);
        highSocEngine.set(1041, 3.0, 3000);
        highSocEngine.set(1042, 2.0, 3000);
        highSocEngine.set(1043, 9.0, 3000);
        highSocEngine.set(1130, 220.0, 3000);
        highSocEngine.set(1131, 235.0, 3000);
        highSocEngine.set(1132, 250.0, 3000);
        highSocEngine.set(1136, 10.0, 3000);
        highSocEngine.set(1137, 5.0, 3000);
        highSocEngine.set(1138, 15.0, 3000);
        highSocEngine.set(1139, 30.0, 3000);
        highSocEngine.set(1140, 1.0, 3000);
        highSocEngine.set(1141, 1.0, 3000);
        highSocEngine.set(1142, 1.0, 3000);
        highSocEngine.set(1143, 3.0, 3000);
        highSocEngine.set(544, 225.0, 3000);
        highSocEngine.set(545, 230.0, 3000);
        highSocEngine.set(546, 240.0, 3000);
        highSocEngine.set(547, 245.0, 3000);
        highSocEngine.set(533, 5.0, 3000);
        highSocEngine.set(535, 50.0, 3000);
        highSocEngine.set(504, 20.0, 3000);
        highSocEngine.set(151, 60.0, 3000);
        highSocEngine.set(1570, 98.0, 3000);
        highSocEngine.set(161, 95.0, 3000);
        highSocEngine.set(162, 10.0, 3000);
        highSocEngine.set(12, 1.0, 3000);
        highSocEngine.set(22, 1.0, 3000);
        highSocEngine.runOnce(3000);

        const auto highSocPc = highSocRouter.getLatestByIndex(629, 3000);
        require(static_cast<bool>(highSocPc), "high SOC PCS_PC_OUT missing");
        requireNear(highSocPc->value, 0.0, 0.0001, "high SOC should block charge");
        const auto highSocHvRun = highSocRouter.getLatestByIndex(12, 3000);
        require(static_cast<bool>(highSocHvRun), "high SOC HV run flag missing");
        requireNear(highSocHvRun->value, 0.0, 0.0001, "high SOC should clear HV run flag");
        const auto highSocGfRun = highSocRouter.getLatestByIndex(22, 3000);
        require(static_cast<bool>(highSocGfRun), "high SOC GF run flag missing");
        requireNear(highSocGfRun->value, 0.0, 0.0001, "high SOC should clear GF run flag");

        const auto seedPcsWriteInputs = [](auto& engine, std::int64_t ts) {
            engine.set(156, 2.0, ts);
            engine.set(1036, 30.0, ts);
            engine.set(1037, 20.0, ts);
            engine.set(1038, 10.0, ts);
            engine.set(1039, 60.0, ts);
            engine.set(1040, 30.0, ts);
            engine.set(1041, 3.0, ts);
            engine.set(1042, 2.0, ts);
            engine.set(1043, 35.0, ts);
            engine.set(1130, 220.0, ts);
            engine.set(1131, 235.0, ts);
            engine.set(1132, 250.0, ts);
            engine.set(1136, 10.0, ts);
            engine.set(1137, 5.0, ts);
            engine.set(1138, 15.0, ts);
            engine.set(1139, 30.0, ts);
            engine.set(1140, 1.0, ts);
            engine.set(1141, 1.0, ts);
            engine.set(1142, 1.0, ts);
            engine.set(1143, 3.0, ts);
            engine.set(514, 0.8, ts);
            engine.set(544, 225.0, ts);
            engine.set(545, 230.0, ts);
            engine.set(546, 240.0, ts);
            engine.set(547, 245.0, ts);
            engine.set(533, 5.0, ts);
            engine.set(535, 30.0, ts);
            engine.set(504, 20.0, ts);
            engine.set(151, 300.0, ts);
            engine.set(1399, 1.0, ts);
        };

        const auto pcsWriteConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_pcs_write");
        edge_gateway::PointStoreRouter pcsWriteRouter;
        cleanupStoreSegment(pcsWriteConfig.memoryStore);
        edge_gateway::MemoryPointStore pcsWriteStore(pcsWriteConfig.memoryStore);
        pcsWriteRouter.addStore(pcsWriteConfig.memoryStore.sharedMemoryName, pcsWriteStore);
        pcsWriteRouter.addRoutesFromDeviceConfigs({pcsWriteConfig}, pcsWriteConfig.memoryStore.sharedMemoryName);
        edge_gateway::LegacyEmsEngine pcsWriteEngine(
            runtimeCatalog,
            pcsWriteRouter,
            600000,
            {{"Meter_TQ", "1"}, {"Meter_CN", "1"}, {"Meter_BW", "0"}, {"Meter_FH", "0"}, {"BMS_MODEL", "2"}}
        );
        seedPcsWriteInputs(pcsWriteEngine, 4000);
        const auto pcsWriteResult = pcsWriteEngine.runOnce(4000);
        const auto pcsWrites = pcsWriteRouter.peekPendingWrites(8);
        require(pcsWriteResult.deviceWrites == 3, "PCS writeback should submit three commands");
        require(pcsWrites.size() == 3, "PCS writeback pending command count mismatch");
        require(pcsWrites[0].index == 1318, "PCS PA write index mismatch");
        requireNear(pcsWrites[0].value, -5.0, 0.0001, "PCS PA write value mismatch");
        require(pcsWrites[0].source == "legacy-ems", "PCS PA write source mismatch");
        require(pcsWrites[1].index == 1320, "PCS PC write index mismatch");
        requireNear(pcsWrites[1].value, 5.0, 0.0001, "PCS PC write value mismatch");
        require(pcsWrites[1].source == "legacy-ems", "PCS PC write source mismatch");
        require(pcsWrites[2].index == 1321, "PCS QA write index mismatch");
        requireNear(pcsWrites[2].value, 7.0, 0.0001, "PCS QA write value mismatch");
        require(pcsWrites[2].source == "legacy-ems", "PCS QA write source mismatch");
        const auto pcsWriteSecondResult = pcsWriteEngine.runOnce(4001);
        const auto pcsWritesAfterSecondRun = pcsWriteRouter.peekPendingWrites(8);
        require(pcsWriteSecondResult.deviceWrites == 0, "PCS writeback should not duplicate pending same-value commands");
        require(pcsWritesAfterSecondRun.size() == 3, "PCS writeback duplicate pending count mismatch");

        writeTextFile(
            "graph_ems_pcs_writeback_only_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "pcs_writeback_only",
  "nodes": [
    {
      "id": "pcs_writeback",
      "type": "pcsWriteback",
      "enabled": true,
      "params": {
        "submitWrites": true,
        "paInput": 627,
        "pbInput": 628,
        "pcInput": 629,
        "qaInput": 630,
        "qbInput": 631,
        "qcInput": 632,
        "comStatusIndex": 1399,
        "pControlAIndex": 1318,
        "pControlBIndex": 1319,
        "pControlCIndex": 1320,
        "qControlAIndex": 1321,
        "qControlBIndex": 1322,
        "qControlCIndex": 1323
      }
    }
  ],
  "edges": []
})json"
        );

        const auto pcsReadbackConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_pcs_readback");
        edge_gateway::PointStoreRouter pcsReadbackRouter;
        cleanupStoreSegment(pcsReadbackConfig.memoryStore);
        edge_gateway::MemoryPointStore pcsReadbackStore(pcsReadbackConfig.memoryStore);
        pcsReadbackRouter.addStore(pcsReadbackConfig.memoryStore.sharedMemoryName, pcsReadbackStore);
        pcsReadbackRouter.addRoutesFromDeviceConfigs({pcsReadbackConfig}, pcsReadbackConfig.memoryStore.sharedMemoryName);
        edge_gateway::LegacyEmsEngine pcsReadbackEngine(
            runtimeCatalog,
            pcsReadbackRouter,
            600000,
            {{"Meter_TQ", "1"}, {"Meter_CN", "1"}, {"Meter_BW", "0"}, {"Meter_FH", "0"}, {"BMS_MODEL", "2"}}
        );
        seedPcsWriteInputs(pcsReadbackEngine, 5000);
        pcsReadbackEngine.set(1318, -5.0, 5000);
        pcsReadbackEngine.set(1320, 5.0, 5000);
        pcsReadbackEngine.set(1321, 7.0, 5000);
        pcsReadbackEngine.runOnce(5000);
        require(pcsReadbackRouter.peekPendingWrites(8).empty(), "PCS writeback should respect signed readback values");

        const auto graphPcsReadbackConfig = buildIsolatedTestDeviceConfig("legacy_ems_test_store_graph_pcs_readback");
        edge_gateway::PointStoreRouter graphPcsReadbackRouter;
        cleanupStoreSegment(graphPcsReadbackConfig.memoryStore);
        edge_gateway::MemoryPointStore graphPcsReadbackStore(graphPcsReadbackConfig.memoryStore);
        graphPcsReadbackRouter.addStore(
            graphPcsReadbackConfig.memoryStore.sharedMemoryName,
            graphPcsReadbackStore
        );
        graphPcsReadbackRouter.addRoutesFromDeviceConfigs(
            {graphPcsReadbackConfig},
            graphPcsReadbackConfig.memoryStore.sharedMemoryName
        );
        edge_gateway::LegacyEmsEngine graphPcsReadbackSeedEngine(runtimeCatalog, graphPcsReadbackRouter);
        graphPcsReadbackSeedEngine.set(1399, 1.0, 5010);
        graphPcsReadbackSeedEngine.set(627, -5.0, 5010);
        graphPcsReadbackSeedEngine.set(628, 0.0, 5010);
        graphPcsReadbackSeedEngine.set(629, 5.0, 5010);
        graphPcsReadbackSeedEngine.set(630, 7.0, 5010);
        graphPcsReadbackSeedEngine.set(631, 0.0, 5010);
        graphPcsReadbackSeedEngine.set(632, 0.0, 5010);
        graphPcsReadbackSeedEngine.set(1318, -5.0, 5010);
        graphPcsReadbackSeedEngine.set(1320, 5.0, 5010);
        graphPcsReadbackSeedEngine.set(1321, 7.0, 5010);
        edge_gateway::GraphEmsEngine graphPcsReadbackEngine(
            edge_gateway::GraphEmsConfig::loadLegacyV1ForMigration("graph_ems_pcs_writeback_only_test.json"),
            graphPcsReadbackRouter,
            600000
        );
        graphPcsReadbackEngine.runOnce(5010);
        require(graphPcsReadbackRouter.peekPendingWrites(8).empty(), "graph PCS writeback should respect signed readback values");

        std::cout << "legacy_ems_test passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "legacy_ems_test failed: " << ex.what() << "\n";
        return 1;
    }
}
