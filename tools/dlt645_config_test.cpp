// DL/T 645 shipped templates/configs and load-time validation: corrected DIs and signed types,
// settlement data off by default, writes off by default and no default password, whitelist /
// DI / address / offset / write checks, and a --mock acceptance run over every shipped config.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <unistd.h>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "dlt645_test_support.hpp"
#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/dlt645_client.hpp"
#include "edge_gateway/dlt645_collector.hpp"
#include "edge_gateway/dlt645_mock_serial_port.hpp"
#include "edge_gateway/dlt645_point_rules.hpp"
#include "edge_gateway/memory_point_store.hpp"

using dlt645_test::require;
using namespace edge_gateway;

namespace {

std::string g_root;

const char* const kStandard = "config/templates/dlt645_2007_standard_points.json";
const char* const kBreaker = "config/templates/dlt645_residual_current_breaker_20231101_points.json";
const char* const kBreakerComm104 = "config/templates/dlt645_residual_current_breaker_20231101_comm104_verified_points.json";

std::vector<PointDefinition> loadTemplate(const std::string& relative) {
    return ConfigLoader::loadDlt645StandardPointsFromFile(g_root + "/" + relative);
}

const PointDefinition& byCode(const std::vector<PointDefinition>& points, const std::string& code) {
    for (const auto& point : points) {
        if (point.pointCode == code) return point;
    }
    throw std::runtime_error("point not found: " + code);
}

bool throwsWith(const std::function<void()>& action, const std::string& text) {
    try {
        action();
    } catch (const std::exception& ex) {
        if (std::string(ex.what()).find(text) != std::string::npos) return true;
        std::cerr << "unexpected error text: " << ex.what() << std::endl;
        return false;
    }
    return false;
}

void standardTemplate() {
    const auto points = loadTemplate(kStandard);
    require(points.size() == 279, "standard template: 276 points + 3 appended phase angles");
    for (const auto& code : {"KY02100110", "KY02100107", "KY02100108", "KY02100109"}) {
        const auto& p = byCode(points, code);
        require(p.read.dlt645Di.compare(0, 4, "0205") == 0 && p.read.unit == "kVA" &&
                p.read.dataType == "dlt645_bcd_signed" && p.read.dlt645ByteCount == 3,
                std::string("apparent power must read 0205xx00: ") + code);
    }
    for (const auto& code : {"KY02100710", "KY02100112", "KY02100113", "KY02100114"}) {
        const auto& p = byCode(points, code);
        require(p.read.dlt645Di.compare(0, 4, "0206") == 0 && p.read.dataType == "dlt645_bcd_signed" &&
                p.read.dlt645ByteCount == 2 && std::abs(p.read.scale - 0.001) < 1e-12,
                std::string("power factor must read 0206xx00: ") + code);
    }
    // Auto index = template position: existing points keep theirs, phase angles are appended.
    require(points[26].pointCode == "KY02100110" && points[30].pointCode == "KY02100710", "point order unchanged");
    for (std::size_t i = 276; i < 279; ++i) {
        require(points[i].read.dlt645Di.compare(0, 4, "0207") == 0 && std::abs(points[i].read.scale - 0.1) < 1e-12 &&
                !points[i].enabled, "phase angle XXX.X appended, disabled by default");
    }
    for (const auto& p : points) {
        const auto& di = p.read.dlt645Di;
        if (di.compare(0, 4, "0202") == 0) require(p.read.dataType == "dlt645_bcd_signed", "current is signed: " + di);
        if (di.compare(0, 4, "0203") == 0 || di.compare(0, 4, "0204") == 0 || di.compare(0, 4, "0205") == 0) {
            require(p.read.dataType == "dlt645_bcd_signed", "power is signed: " + di);
        }
        if (di.compare(0, 4, "0003") == 0 || di.compare(0, 4, "0004") == 0) {
            require(p.read.dataType == "dlt645_bcd_signed", "combined reactive energy is signed: " + di);
        }
        if (di.compare(0, 2, "00") == 0 && di.compare(6, 2, "00") != 0) {
            require(!p.enabled && p.read.intervalMs >= 3600000 && p.collectPriority > 0,
                    "settlement-day DI must be disabled, >=1 h, background: " + di);
        }
        if (di.compare(0, 2, "00") == 0 && di.compare(6, 2, "00") == 0) {
            require(p.collectPriority > 0, "energy registers are background: " + di);
        }
        if (di.compare(0, 2, "02") == 0) require(p.collectPriority == 0, "instantaneous values are realtime: " + di);
    }
}

void breakerTemplates() {
    for (const auto* name : {kBreaker, kBreakerComm104}) {
        const auto points = loadTemplate(name);
        for (const auto& p : points) {
            if (p.read.dlt645Di.compare(0, 4, "0202") == 0) {
                require(p.read.dataType == "dlt645_bcd_signed", std::string(name) + ": current is signed");
            }
            if (!p.write.dlt645.di.empty()) {
                require(!p.enabled && !p.write.enable, std::string(name) + ": write point must ship disabled: " + p.pointCode);
            }
        }
    }
    // Enabling trip in the protocol template works: the read-back is prepared (breaker_switch_state).
    auto points = loadTemplate(kBreaker);
    for (auto& p : points) {
        if (p.pointCode == "breaker_remote_trip" || p.pointCode == "breaker_remote_close") {
            p.enabled = true;
            p.write.enable = true;
        }
    }
    validateDlt645PointList(points, "breaker template");
    require(byCode(points, "breaker_remote_trip").write.dlt645.verifyPointCode == "breaker_switch_state" &&
            *byCode(points, "breaker_remote_close").write.dlt645.verifyValue == 1.0, "read-back targets");
    // The COMM202600104 firmware has no position DI: enabling trip without a read-back must fail.
    auto comm104 = loadTemplate(kBreakerComm104);
    for (auto& p : comm104) {
        if (p.pointCode == "breaker_remote_trip") {
            p.enabled = true;
            p.write.enable = true;
        }
    }
    require(throwsWith([&] { validateDlt645PointList(comm104, "comm104"); }, "position read-back"),
            "trip without read-back must not load");
}

void templateValidation() {
    const auto path = "/tmp/dlt645_config_test_" + std::to_string(static_cast<long long>(::getpid())) + ".json";
    {
        std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
        output << R"({"points":[{"pointCode":"word","name":"w","desc":"","unit":"","di":"04000501",)"
                  R"("dataType":"dlt645_uint32","byteCount":2}]})";
    }
    const bool rejected = throwsWith([&] { ConfigLoader::loadDlt645StandardPointsFromFile(path); }, "point word");
    std::remove(path.c_str());
    require(rejected, "template with an unknown dataType must not load");
}

void shippedConfigs() {
    const auto config = ConfigLoader::loadFromFile(g_root + "/config/runtime/devices/device_dlt645_breakers_20_22.json");
    require(!config.protocol.dlt645.write.enabled, "shipped breaker config must not enable writes");
    require(config.protocol.dlt645.write.password.empty() && config.protocol.dlt645.write.operatorCode.empty(),
            "shipped breaker config must not carry a password");
}

std::string deviceJson(const std::string& points, const std::string& extra = "", const std::string& address = "000000000001") {
    return R"({"protocol":{"type":"dlt645_2007"},)" + extra +
           R"("meters":[{"meterCode":"M1","address":")" + address + R"(","points":[)" + points + "]}]}";
}

std::string readPointJson(const std::string& code, const std::string& type, const std::string& dlt645,
                          const std::string& extra = "") {
    return R"({"index":1,"pointCode":")" + code + R"(","enabled":true,"read":{"enable":true,"dataType":")" + type +
           R"(",)" + extra + R"("dlt645":)" + dlt645 + "}}";
}

void validation() {
    const auto load = [](const std::string& text) { (void)ConfigLoader::loadFromText(text); };
    load(deviceJson(readPointJson("ua", "dlt645_bcd", R"({"di":"02010100","byteCount":2})")));

    require(throwsWith([&] { load(deviceJson(readPointJson("ua", "dlt645_uint32", R"({"di":"02010100","byteCount":2})"))); },
                       "point ua"), "unknown dataType must fail at load, naming the point");
    require(throwsWith([&] { load(deviceJson(readPointJson("ua", "dlt645_status_bits", R"({"di":"04000503","byteCount":2})"))); },
                       "not a DL/T 645 type"), "dlt645_status_bits does not exist");
    require(throwsWith([&] { load(deviceJson(readPointJson("ua", "dlt645_bcd", R"({"di":"0201010","byteCount":2})"))); },
                       "read.dlt645.di"), "DI must be 8 hex digits");
    require(throwsWith([&] { load(deviceJson(readPointJson("ua", "dlt645_bcd", R"({"di":"02010100","byteCount":2,"scale":0.1})"))); },
                       "read.dlt645.scale"), "read.dlt645.scale must not be silently ignored");
    require(throwsWith([&] { load(deviceJson(readPointJson("ua", "dlt645_bcd", R"({"di":"02010100","byteCount":2,"offset":-2})"))); },
                       "offset"), "negative offset rejected");
    require(throwsWith([&] { load(deviceJson(readPointJson("ua", "dlt645_bcd", R"({"di":"0201FF00"})"))); },
                       "block DI"), "block DI needs byteCount/offset");
    const auto ua = readPointJson("ua", "dlt645_bcd", R"({"di":"0201FF00","byteCount":2,"offset":0})");
    auto ub = readPointJson("ub", "dlt645_bcd", R"({"di":"0201FF00","byteCount":2,"offset":0})");
    require(throwsWith([&] { load(deviceJson(ua + "," + ub)); }, "same bytes"), "Ub at Ua's offset must fail");
    ub = readPointJson("ub", "dlt645_bcd", R"({"di":"0201FF00","byteCount":2,"offset":2})");
    load(deviceJson(ua + "," + ub));
    require(throwsWith([&] { load(deviceJson(readPointJson("ua", "dlt645_bcd", R"({"di":"02010100"})"), "", "00000000002O")); },
                       "address"), "address with a letter must fail");
    require(throwsWith([&] { load(R"({"protocol":{"type":"dlt645_2007"},"meters":[{"meterCode":"A","address":"1"},{"meterCode":"B","address":"000000000001"}]})"); },
                       "used by another meter"), "duplicate meter address must fail");
    require(throwsWith([&] { load(R"({"protocol":{"type":"dlt645_2007","transport":{"wakeupBytes":40}},"meters":[]})"); },
                       "wakeupBytes"), "wakeupBytes bounded");

    // Writes: default password forbidden, switching commands need a read-back.
    const auto writeBlock = [](const std::string& password) {
        return R"("dlt645":{"write":{"enabled":true,"password":")" + password + R"(","operatorCode":"01020304"}},)";
    };
    require(throwsWith([&] { load(deviceJson("", writeBlock("00000000"))); }, "factory default"), "00000000 forbidden");
    require(throwsWith([&] { load(deviceJson("", writeBlock("04000000"))); }, "factory default"), "000000 secret forbidden");
    const auto password = dlt645_test::fixturePassword();
    load(deviceJson("", writeBlock(password)));
    const auto state = readPointJson("breaker_switch_state", "dlt645_bcd", R"({"di":"07020100","byteCount":1})");
    const auto trip = [](const std::string& verify) {
        return R"({"index":2,"pointCode":"trip","enabled":true,"write":{"enable":true,"min":0,"max":99,"step":1,)" + verify +
               R"("dlt645":{"di":"06010101","dataType":"dlt645_scheduled_control","byteCount":2,"unit":2,)"
               R"("verifyPointCode":"breaker_switch_state","verifyValue":0}}})";
    };
    require(throwsWith([&] { load(deviceJson(state + "," + trip(""), writeBlock(password))); }, "position read-back"),
            "trip without verifyByRead must fail");
    load(deviceJson(state + "," + trip(R"("verifyAfterWrite":true,"verifyByRead":true,)"), writeBlock(password)));
    auto missingTarget = trip(R"("verifyAfterWrite":true,"verifyByRead":true,)");
    require(throwsWith([&] { load(deviceJson(missingTarget, writeBlock(password))); }, "verifyPointCode"),
            "read-back point must exist in the same meter");
}

// --mock acceptance: every shipped DL/T 645 config decodes all its points against the simulator.
void mockAcceptance(const std::string& relativeConfig) {
    auto config = ConfigLoader::loadFromFile(g_root + "/" + relativeConfig);
    auto templatePath = config.protocol.standardPointsFile;
    const std::string installRoot = "/opt/modbus-gateway/";
    if (templatePath.compare(0, installRoot.size(), installRoot) == 0) templatePath = templatePath.substr(installRoot.size());
    const auto points = ConfigLoader::loadDlt645StandardPointsFromFile(g_root + "/" + templatePath);
    auto port = std::make_shared<Dlt645MockSerialPort>();
    SerialPortOptions options;
    options.device = "mock";
    options.timeoutMs = 20;
    options.wakeupBytes = config.protocol.transport.wakeupBytes;
    auto client = std::make_shared<Dlt645Client>(port, options);
    const std::string storeName = "dlt645_config_mock_acceptance_test";
    MemoryPointStore::cleanupOrphanedSegment(storeName);
    {
        MemoryPointStore store(storeName);
        std::size_t meterIndex = 0;
        for (const auto& meter : config.meters) {
            DeviceConfig item = config;
            item.meterCode = meter.meterCode;
            item.address = meter.address;
            item.memoryStore.sharedMemoryName = storeName;
            item.points = points;
            for (std::size_t i = 0; i < item.points.size(); ++i) {
                item.points[i].index = 200000U + static_cast<std::uint32_t>(meterIndex * 10000U + i);
            }
            item.meters.clear();
            item.collect.maxTasksPerMeterPerCycle = 1000;
            item.collect.maxBackgroundTasksPerMeterPerCycle = 1000;
            Dlt645Collector collector(item, store, client);
            const auto result = collector.collectOnce(1000);
            std::size_t expected = 0;
            for (const auto& p : item.points) {
                if (p.enabled && p.read.enable && p.read.dataType != "device_online") ++expected;
            }
            require(result.values.size() == expected, relativeConfig + ": every enabled point must be read once");
            for (const auto& value : result.values) {
                require(value.quality == 1, relativeConfig + ": " + value.pointCode + " not decoded: " + value.qualityMsg);
            }
            ++meterIndex;
        }
    }
    MemoryPointStore::cleanupOrphanedSegment(storeName);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: dlt645_config_test <repo root>" << std::endl;
        return 2;
    }
    g_root = argv[1];
    try {
        standardTemplate();
        breakerTemplates();
        templateValidation();
        shippedConfigs();
        validation();
        mockAcceptance("config/runtime/devices/device_dlt645_multi_meter_1_2.json");
        mockAcceptance("config/runtime/devices/device_dlt645_breakers_20_22.json");
        std::cout << "dlt645_config_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "dlt645_config_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
