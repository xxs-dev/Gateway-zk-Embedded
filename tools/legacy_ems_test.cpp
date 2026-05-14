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
#include <stdexcept>
#include <string>

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

void cleanupStoreSegment(const edge_gateway::MemoryStoreConfig& config) {
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(config.sharedMemoryName);
}

void writeTextFile(const std::string& path, const std::string& text) {
    std::ofstream output(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("failed to write test file: " + path);
    }
    output << text;
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
            "graph_ems_invalid_runtime_mode_app_config_test.json",
            R"json({
  "runtimeMode": "invalid"
})json"
        );
        requireThrowsWithMessage("runtimeMode must be gateway or ems", []() {
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
        const auto graphConfig = edge_gateway::GraphEmsConfig::loadFromFile("graph_ems_minimal_test.json");
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
            edge_gateway::GraphEmsConfig::loadFromFile("graph_ems_duplicate_node_test.json");
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
            edge_gateway::GraphEmsConfig::loadFromFile("graph_ems_unknown_node_test.json");
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
            edge_gateway::GraphEmsConfig::loadFromFile("graph_ems_cycle_test.json");
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
            edge_gateway::GraphEmsConfig::loadFromFile("graph_ems_meter_average_test.json"),
            graphAverageRouter,
            600000
        );
        const auto graphAverageResult = graphAverageEngine.runOnce(1150);
        require(graphAverageResult.latestWrites == 1, "graph meterAverage should count one latest write");
        const auto graphAvgPa = graphAverageRouter.getLatestByIndex(209, 1150);
        require(static_cast<bool>(graphAvgPa), "graph meterAverage output missing");
        requireNear(graphAvgPa->value, 30.0, 0.0001, "graph meterAverage output mismatch");

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

        edge_gateway::ComputeRuleConfig graphServiceRule;
        graphServiceRule.ruleCode = "graph_ems_service_rule";
        graphServiceRule.enabled = true;
        graphServiceRule.trigger.type = "interval";
        graphServiceRule.trigger.intervalMs = 1;
        graphServiceRule.script.type = "graphEms";
        graphServiceRule.script.graphFile = "graph_ems_meter_average_test.json";

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

        writeTextFile(
            "graph_ems_profile_test.json",
            R"json({
  "schemaVersion": "1.0.0",
  "graphCode": "profile",
  "nodes": [
    {
      "id": "tq_average",
      "type": "meterAverage",
      "enabled": true,
      "params": {
        "profileKey": "Meter_TQ",
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
        graphProfileRule.script.graphFile = "graph_ems_profile_test.json";
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
            edge_gateway::GraphEmsConfig::loadFromFile("graph_ems_tq_metrics_test.json"),
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
            edge_gateway::GraphEmsConfig::loadFromFile("graph_ems_fh_direct_test.json"),
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
            edge_gateway::GraphEmsConfig::loadFromFile("graph_ems_bms_model_test.json"),
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
            edge_gateway::GraphEmsConfig::loadFromFile("graph_ems_topological_order_test.json"),
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
            edge_gateway::GraphEmsConfig::loadFromFile("graph_ems_ds_test.json"),
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
            edge_gateway::GraphEmsConfig::loadFromFile("graph_ems_ds_curve_writeback_test.json"),
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
            edge_gateway::GraphEmsConfig::loadFromFile("graph_ems_gf_ph_test.json"),
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
            edge_gateway::GraphEmsConfig::loadFromFile("graph_ems_pcs_solve_test.json"),
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
            edge_gateway::GraphEmsConfig::loadFromFile("graph_ems_pcs_writeback_test.json"),
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
            edge_gateway::GraphEmsConfig::loadFromFile("graph_ems_pcs_solve_submit_writes_test.json"),
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
            edge_gateway::GraphEmsConfig::loadFromFile("graph_ems_legacy_nodes_test.json"),
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
        const auto graphStateConfig = edge_gateway::GraphEmsConfig::loadFromFile("graph_ems_state_restore_test.json");
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

        const auto shuntongGraphTemplate = edge_gateway::GraphEmsConfig::loadFromFile(
            "config/examples/shuntong_ems_graph.json"
        );
        const auto hasGraphNode = [&](const std::string& id) {
            for (const auto& node : shuntongGraphTemplate.nodes) {
                if (node.id == id) {
                    return true;
                }
            }
            return false;
        };
        require(hasGraphNode("ds"), "shuntong graph missing ds");
        require(hasGraphNode("gf"), "shuntong graph missing gf");
        require(hasGraphNode("ph"), "shuntong graph missing ph");
        require(hasGraphNode("power_solve"), "shuntong graph missing power_solve");
        require(hasGraphNode("pcs_writeback"), "shuntong graph missing pcs_writeback");

        addRouteIfMissing(router, 156, "avg_window", deviceConfig.memoryStore.sharedMemoryName, false);
        engine.set(156, 2.0, 1200);
        engine.set(1030, 220.0, 1200);
        engine.set(1031, 221.0, 1200);
        engine.set(1032, 222.0, 1200);

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

        edge_gateway::ComputeEngineService service(computeConfig, router);
        service.runOnce(1300);
        auto avgUa = router.getLatestByIndex(201, 1300);
        require(static_cast<bool>(avgUa), "ComputeEngine legacyEms should write TQ_avg_UA");
        requireNear(avgUa->value, 220.0, 0.0001, "first TQ_avg_UA should equal first sample");

        engine.set(1030, 224.0, 1400);
        service.runOnce(1400);
        avgUa = router.getLatestByIndex(201, 1400);
        require(static_cast<bool>(avgUa), "second TQ_avg_UA missing");
        requireNear(avgUa->value, 222.0, 0.0001, "TQ_avg_UA should use configured moving average window");

        engine.set(1036, 30.0, 1500);
        engine.set(1037, 20.0, 1500);
        engine.set(1038, 10.0, 1500);
        engine.set(1039, 60.0, 1500);
        engine.set(1040, 4.0, 1500);
        engine.set(1041, 3.0, 1500);
        engine.set(1042, 0.0, 1500);
        engine.set(1043, 5.0, 1500);
        engine.set(1130, 230.0, 1500);
        engine.set(1131, 231.0, 1500);
        engine.set(1132, 232.0, 1500);
        engine.set(1136, 10.0, 1500);
        engine.set(1137, 5.0, 1500);
        engine.set(1138, 15.0, 1500);
        engine.set(1139, 30.0, 1500);
        engine.set(1140, 1.0, 1500);
        engine.set(1141, 1.0, 1500);
        engine.set(1142, 1.0, 1500);
        engine.set(1143, 3.0, 1500);
        service.runOnce(1500);

        const auto cnAvgPa = router.getLatestByIndex(259, 1500);
        require(static_cast<bool>(cnAvgPa), "CN_avg_PA missing");
        requireNear(cnAvgPa->value, 10.0, 0.0001, "CN_avg_PA should equal first CN sample");

        const auto fhAvgPa = router.getLatestByIndex(309, 1500);
        require(static_cast<bool>(fhAvgPa), "FH_avg_PA missing");
        requireNear(fhAvgPa->value, 20.0, 0.0001, "FH_avg_PA should equal TQ_avg_PA - CN_avg_PA");

        const auto fhAvgSa = router.getLatestByIndex(317, 1500);
        require(static_cast<bool>(fhAvgSa), "FH_avg_SA missing");
        requireNear(fhAvgSa->value, std::sqrt(409.0), 0.0001, "FH_avg_SA mismatch");

        const auto fhAvgCos3 = router.getLatestByIndex(324, 1500);
        require(static_cast<bool>(fhAvgCos3), "FH_avg_COS3 missing");
        requireNear(fhAvgCos3->value, 30.0 / std::sqrt(904.0), 0.0001, "FH_avg_COS3 mismatch");

        const auto fhAvgBph = router.getLatestByIndex(325, 1500);
        require(static_cast<bool>(fhAvgBph), "FH_avg_P_BPH missing");
        requireNear(fhAvgBph->value, 250.0, 0.0001, "FH_avg_P_BPH mismatch");

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
        pcsWriteEngine.set(156, 2.0, 4000);
        pcsWriteEngine.set(1036, 30.0, 4000);
        pcsWriteEngine.set(1037, 20.0, 4000);
        pcsWriteEngine.set(1038, 10.0, 4000);
        pcsWriteEngine.set(1039, 60.0, 4000);
        pcsWriteEngine.set(1040, 30.0, 4000);
        pcsWriteEngine.set(1041, 3.0, 4000);
        pcsWriteEngine.set(1042, 2.0, 4000);
        pcsWriteEngine.set(1043, 35.0, 4000);
        pcsWriteEngine.set(1130, 220.0, 4000);
        pcsWriteEngine.set(1131, 235.0, 4000);
        pcsWriteEngine.set(1132, 250.0, 4000);
        pcsWriteEngine.set(1136, 10.0, 4000);
        pcsWriteEngine.set(1137, 5.0, 4000);
        pcsWriteEngine.set(1138, 15.0, 4000);
        pcsWriteEngine.set(1139, 30.0, 4000);
        pcsWriteEngine.set(1140, 1.0, 4000);
        pcsWriteEngine.set(1141, 1.0, 4000);
        pcsWriteEngine.set(1142, 1.0, 4000);
        pcsWriteEngine.set(1143, 3.0, 4000);
        pcsWriteEngine.set(514, 0.8, 4000);
        pcsWriteEngine.set(544, 225.0, 4000);
        pcsWriteEngine.set(545, 230.0, 4000);
        pcsWriteEngine.set(546, 240.0, 4000);
        pcsWriteEngine.set(547, 245.0, 4000);
        pcsWriteEngine.set(533, 5.0, 4000);
        pcsWriteEngine.set(535, 30.0, 4000);
        pcsWriteEngine.set(504, 20.0, 4000);
        pcsWriteEngine.set(151, 300.0, 4000);
        pcsWriteEngine.set(1399, 1.0, 4000);
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

        std::cout << "legacy_ems_test passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "legacy_ems_test failed: " << ex.what() << "\n";
        return 1;
    }
}
