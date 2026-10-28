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
    require(points.size() == 282, "standard template: 276 points + phase angles + relay state + remote trip/close");
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
    // C=1CH remote trip/close ship disabled with the relay bit (04000503 bit 4) as read-back.
    const auto& relay = byCode(points, "dlt645_relay_open");
    require(!relay.enabled && relay.read.dlt645Di == "04000503" && relay.read.bit == 4, "relay state point");
    for (const auto* code : {"dlt645_remote_trip", "dlt645_remote_close"}) {
        const auto& p = byCode(points, code);
        require(!p.enabled && !p.write.enable && p.write.dlt645.dataType == "dlt645_remote_control" &&
                p.write.dlt645.verifyPointCode == "dlt645_relay_open", std::string(code) + " ships disabled with read-back");
    }
    auto enabled = points;
    for (auto& p : enabled) {
        if (p.pointCode == "dlt645_relay_open" || p.pointCode == "dlt645_remote_trip" || p.pointCode == "dlt645_remote_close") {
            p.enabled = true;
            p.read.enable = p.write.dlt645.dataType != "dlt645_remote_control";
            p.write.enable = p.write.dlt645.dataType == "dlt645_remote_control";
        }
    }
    validateDlt645PointList(enabled, "standard template", Dlt645Version::V2007);
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
    validateDlt645PointList(points, "breaker template", Dlt645Version::V2007);
    require(byCode(points, "breaker_remote_trip").write.dlt645.verifyPointCode == "breaker_switch_state" &&
            *byCode(points, "breaker_remote_close").write.dlt645.verifyValue == 1.0, "read-back targets");
    // The COMM202600104 firmware has no position DI: the read-back target is the meter's DIO
    // auxiliary contact (meters[].positionReadBackIndex). The template loads without a target...
    auto comm104 = loadTemplate(kBreakerComm104);
    for (auto& p : comm104) {
        if (p.pointCode == "breaker_remote_trip" || p.pointCode == "breaker_remote_close") {
            p.enabled = true;
            p.write.enable = true;
        }
    }
    validateDlt645PointList(comm104, "comm104 template", Dlt645Version::V2007, true);
    // ...but a meter without positionReadBackIndex cannot run an enabled trip point.
    DeviceConfig bus;
    bus.protocol.type = "dlt645_2007";
    bus.meters.resize(3);
    auto& meter = bus.meters[2];
    meter.meterCode = "LOAD_BREAKER";
    require(throwsWith([&] {
                validateDlt645PointList(expandDlt645MeterPoints(bus, 2, comm104), "meter", Dlt645Version::V2007);
            }, "position read-back"), "trip without read-back must not run");
    meter.positionReadBackIndex = 999;
    const auto expanded = expandDlt645MeterPoints(bus, 2, comm104);
    validateDlt645PointList(expanded, "meter", Dlt645Version::V2007);
    require(byCode(expanded, "breaker_remote_trip").write.dlt645.verifyIndex == 999 &&
            byCode(expanded, "breaker_remote_close").write.dlt645.verifyIndex == 999 &&
            *byCode(expanded, "breaker_remote_close").write.dlt645.verifyValue == 1.0 &&
            byCode(expanded, "breaker_remote_trip").index == 220000U + 11U,
            "meter 22 trip/close read back from DIO index 999 (trip point index 220011)");
    require(byCode(expanded, "breaker_cancel_trip").write.dlt645.verifyIndex == 0, "cancel commands have no read-back");
}

void template1997() {
    const auto points = loadTemplate("config/templates/dlt645_1997_standard_points.json");
    require(points.size() == 31, "1997 template: energy, voltage, current, power, reactive, PF, online");
    const auto check = [&points](const char* code, const char* di, int bytes, double scale) {
        const auto& p = byCode(points, code);
        require(p.read.dlt645Di == di && p.read.dlt645ByteCount == bytes && std::abs(p.read.scale - scale) < 1e-12 &&
                p.read.dataType == "dlt645_bcd", std::string("1997 format of ") + code);
    };
    check("KY02100891", "9010", 4, 0.01);    // XXXXXX.XX kWh
    check("KY02100018", "B611", 2, 1.0);     // XXX V
    check("KY02100024", "B621", 2, 0.01);    // XX.XX A
    check("KY02100029", "B630", 3, 0.0001);  // XX.XXXX kW
    check("KY02100030", "B640", 2, 0.01);    // XX.XX kvar
    check("KY02100710", "B650", 2, 0.001);   // X.XXX
    // A 1997 template under a 2007 config (or the other way round) is refused when expanding.
    DeviceConfig config;
    config.protocol.type = "dlt645_2007";
    config.meters.resize(1);
    require(throwsWith([&] { expandDlt645MeterPoints(config, 0, points); }, "does not match protocol.type"),
            "1997 template under dlt645_2007 must be refused");
    config.protocol.type = "dlt645_1997";
    require(throwsWith([&] { expandDlt645MeterPoints(config, 0, loadTemplate(kStandard)); }, "does not match protocol.type"),
            "2007 template under dlt645_1997 must be refused");
    require(expandDlt645MeterPoints(config, 0, points).size() == 31, "matching edition expands");
}

void autoIndexBase() {
    const auto load = [](const std::string& text) { return ConfigLoader::loadFromText(text); };
    const auto config = load(R"({"protocol":{"type":"dlt645_2007"},"dlt645":{"autoIndexBase":300000},)"
                             R"("meters":[{"meterCode":"A","address":"1"},{"meterCode":"B","address":"2"}]})");
    require(config.protocol.dlt645.autoIndexBase == 300000, "autoIndexBase parsed");
    const auto points = loadTemplate(kStandard);
    require(expandDlt645MeterPoints(config, 0, points).front().index == 300000 &&
            expandDlt645MeterPoints(config, 1, points).back().index == 310000 + points.size() - 1,
            "auto indexes start at autoIndexBase, one 10000 block per meter");
    require(load(R"({"protocol":{"type":"dlt645_2007"},"meters":[{"meterCode":"A","address":"1"}]})")
                .protocol.dlt645.autoIndexBase == 200000, "default 200000");
    // An explicit index inside an auto-expanded meter's block is refused.
    require(throwsWith([&] {
                load(R"({"protocol":{"type":"dlt645_2007"},"dlt645":{"autoIndexBase":300000},"meters":[)"
                     R"({"meterCode":"A","address":"1"},)"
                     R"({"meterCode":"B","address":"2","points":[{"index":300005,"pointCode":"ua","enabled":true,)"
                     R"("read":{"enable":true,"dataType":"dlt645_bcd","dlt645":{"di":"02010100","byteCount":2}}}]}]})");
            }, "auto-index block of meter A"), "explicit index overlapping an auto block");
    require(throwsWith([&] {
                load(R"({"protocol":{"type":"dlt645_2007"},"dlt645":{"autoIndexBase":4294960000},)"
                     R"("meters":[{"meterCode":"A","address":"1"}]})");
            }, "beyond 4294967295"), "auto block must fit uint32");
    require(throwsWith([&] { load(R"({"protocol":{"type":"dlt645_2007"},"dlt645":{"autoIndexBase":0},"meters":[]})"); },
                       "autoIndexBase"), "autoIndexBase must be positive");
}

void editionAndCommandValidation() {
    const auto load = [](const std::string& text) { (void)ConfigLoader::loadFromText(text); };
    const auto meter = [](const std::string& type, const std::string& points, const std::string& extra = "") {
        return R"({"protocol":{"type":")" + type + R"("},)" + extra +
               R"("meters":[{"meterCode":"M1","address":"000000000001","points":[)" + points + "]}]}";
    };
    const auto read = [](const std::string& di) {
        return R"({"index":1,"pointCode":"p","enabled":true,"read":{"enable":true,"dataType":"dlt645_bcd",)"
               R"("dlt645":{"di":")" + di + R"(","byteCount":2}}})";
    };
    load(meter("dlt645_1997", read("B611")));
    require(throwsWith([&] { load(meter("dlt645_1997", read("02010100"))); }, "4 hex digits (DI1 DI0)"),
            "2007 DI in a 1997 config");
    require(throwsWith([&] { load(meter("dlt645_2007", read("B611"))); }, "8 hex digits"), "1997 DI in a 2007 config");
    require(throwsWith([&] {
                load(meter("dlt645_1997", R"({"index":1,"pointCode":"u","enabled":true,"read":{"enable":true,)"
                                          R"("dataType":"dlt645_bcd","dlt645":{"di":"B61F"}}})"));
            }, "block DI"), "1997 block DI (trailing F) needs byteCount/offset");

    const auto password = dlt645_test::fixturePassword();
    const auto write = R"("dlt645":{"write":{"enabled":true,"password":")" + password + R"(","operatorCode":"01020304"}},)";
    // 1997 writes carry no operator code.
    load(meter("dlt645_1997", "", R"("dlt645":{"write":{"enabled":true,"password":")" + password + R"("}},)"));
    const auto remote = [](const std::string& type, const std::string& extra) {
        return R"({"index":5,"pointCode":"rc","enabled":true,"write":{"enable":true,"allowedValues":[1],)" + extra +
               R"("dlt645":{"dataType":"dlt645_remote_control","controlType":")" + type + R"("}}})";
    };
    load(meter("dlt645_2007", remote("keep_power", ""), write));
    require(throwsWith([&] { load(meter("dlt645_1997", remote("keep_power", ""), write)); }, "DL/T 645-2007 only"),
            "C=1CH in a 1997 config must not load");
    require(throwsWith([&] { load(meter("dlt645_2007", remote("open", ""), write)); }, "controlType"),
            "unknown remote control type");
    require(throwsWith([&] { load(meter("dlt645_2007", remote("trip", ""), write)); }, "position read-back"),
            "remote trip needs a position read-back");
    const auto relay = R"({"index":4,"pointCode":"relay","enabled":true,"read":{"enable":true,"dataType":"dlt645_bitfield_le",)"
                       R"("bit":4,"dlt645":{"di":"04000503","byteCount":2}}})";
    load(meter("dlt645_2007", std::string(relay) + "," + R"({"index":5,"pointCode":"rc","enabled":true,"write":{"enable":true,)"
               R"("verifyAfterWrite":true,"verifyByRead":true,"dlt645":{"dataType":"dlt645_remote_control","controlType":"trip",)"
               R"("verifyPointCode":"relay","verifyValue":1}}})", write));
    require(throwsWith([&] { load(meter("dlt645_2007", R"({"index":5,"pointCode":"rc","enabled":true,"write":{"enable":true,)"
               R"("dlt645":{"di":"06010101","dataType":"dlt645_remote_control","controlType":"alarm"}}})", write)); },
                       "uses no write.dlt645.di"), "C=1CH has no DI");

    // Broadcast time sync: off by default, interval bounded.
    const auto sync = [](const std::string& body) { return R"("dlt645":{"timeSync":)" + body + "},"; };
    require(!ConfigLoader::loadFromText(meter("dlt645_2007", "")).protocol.dlt645.timeSync.enabled, "time sync off by default");
    load(meter("dlt645_1997", "", sync(R"({"enabled":true,"intervalMs":86400000})")));
    require(throwsWith([&] { load(meter("dlt645_2007", "", sync(R"({"enabled":true,"intervalMs":60000})"))); }, "timeSync.intervalMs"),
            "time sync more often than hourly refused");
}

void templateValidation() {
    const auto path = "/tmp/dlt645_config_test_" + std::to_string(static_cast<long long>(::getpid())) + ".json";
    {
        std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
        output << R"({"protocol":"dlt645_2007","points":[{"pointCode":"word","name":"w","desc":"","unit":"","di":"04000501",)"
                  R"("dataType":"dlt645_uint32","byteCount":2}]})";
    }
    const bool rejected = throwsWith([&] { ConfigLoader::loadDlt645StandardPointsFromFile(path); }, "point word");
    std::remove(path.c_str());
    require(rejected, "template with an unknown dataType must not load");
}

void shippedConfigs() {
    const auto config = ConfigLoader::loadFromFile(g_root + "/config/runtime/devices/device_dlt645_breakers_20_22.json");
    require(!config.protocol.dlt645.write.enabled, "shipped breaker config must not enable writes");
    // Field-verified on breaker 22: level-0 factory code (PA P0 P1 P2) and operator code 00000000.
    require(config.protocol.dlt645.write.password == "00000000" && config.protocol.dlt645.write.operatorCode == "00000000",
            "shipped breaker config keeps the verified password and operator code");
    require(config.meters.size() == 3 && config.meters[0].positionReadBackIndex == 998 &&
            config.meters[1].positionReadBackIndex == 997 && config.meters[2].positionReadBackIndex == 999,
            "breakers 20/21/22 read their position from DIO auxiliary contacts 998/997/999");
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

    // Writes: 8-hex-digit password/operator code (the vendor's level-0 code 00000000 is valid),
    // switching commands need a read-back.
    const auto writeBlock = [](const std::string& password) {
        return R"("dlt645":{"write":{"enabled":true,"password":")" + password + R"(","operatorCode":"01020304"}},)";
    };
    load(deviceJson("", writeBlock("00000000")));
    require(throwsWith([&] { load(deviceJson("", writeBlock("0000000"))); }, "8 hex"), "short password rejected");
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

    // Read-back from another device's point (DIO auxiliary contact) by its gateway index.
    const auto auxTrip = [](const std::string& verify) {
        return R"({"index":2,"pointCode":"trip","enabled":true,"write":{"enable":true,"min":0,"max":99,"step":1,)"
               R"("verifyAfterWrite":true,"verifyByRead":true,)"
               R"("dlt645":{"di":"06010101","dataType":"dlt645_scheduled_control","byteCount":2,"unit":2,)" + verify + "}}}";
    };
    load(deviceJson(auxTrip(R"("verifyIndex":999,"verifyValue":0)"), writeBlock(password)));
    require(throwsWith([&] { load(deviceJson(auxTrip(R"("verifyIndex":999)"), writeBlock(password))); }, "verifyValue"),
            "verifyIndex needs the expected value");
    require(throwsWith([&] { load(deviceJson(state + "," + auxTrip(
                R"("verifyIndex":999,"verifyPointCode":"breaker_switch_state","verifyValue":0)"), writeBlock(password))); },
                       "exactly one"), "verifyIndex and verifyPointCode are exclusive");
    require(throwsWith([&] { load(deviceJson(auxTrip(R"("verifyIndex":2,"verifyValue":0)"), writeBlock(password))); },
                       "another point"), "verifyIndex must not name the command point");
}

// --mock acceptance: every shipped DL/T 645 config decodes all its points against the simulator.
void mockAcceptance(const std::string& relativeConfig) {
    auto config = ConfigLoader::loadFromFile(g_root + "/" + relativeConfig);
    auto templatePath = config.protocol.standardPointsFile;
    const std::string installRoot = "/opt/modbus-gateway/";
    if (templatePath.compare(0, installRoot.size(), installRoot) == 0) templatePath = templatePath.substr(installRoot.size());
    const auto points = ConfigLoader::loadDlt645StandardPointsFromFile(
        templatePath.compare(0, 1, "/") == 0 ? templatePath : g_root + "/" + templatePath);
    std::vector<std::string> addresses;
    for (const auto& meter : config.meters) addresses.push_back(meter.address);
    auto port = std::make_shared<Dlt645MockSerialPort>(false, addresses);
    SerialPortOptions options;
    options.device = "mock";
    options.timeoutMs = 20;
    options.wakeupBytes = config.protocol.transport.wakeupBytes;
    auto client = std::make_shared<Dlt645Client>(port, options, dlt645VersionOf(config.protocol.type));
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
            item.points = expandDlt645MeterPoints(config, meterIndex, points);
            require(item.points.front().index == config.protocol.dlt645.autoIndexBase + meterIndex * 10000U,
                    relativeConfig + ": auto index block");
            item.meters.clear();
            item.collect.maxTasksPerMeterPerCycle = 1000;
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
        template1997();
        autoIndexBase();
        editionAndCommandValidation();
        templateValidation();
        shippedConfigs();
        validation();
        mockAcceptance("config/runtime/devices/device_dlt645_multi_meter_1_2.json");
        mockAcceptance("config/runtime/devices/device_dlt645_breakers_20_22.json");
        mockAcceptance("config/examples/device_dlt645_1997_meters.json");
        std::cout << "dlt645_config_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "dlt645_config_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
