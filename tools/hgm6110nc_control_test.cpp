#include "edge_gateway/command_executor.hpp"
#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/modbus_rtu_client.hpp"
#include "edge_gateway/scada_project_loader.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unistd.h>

namespace {
void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

std::uint16_t crc(const std::vector<std::uint8_t>& frame) {
    std::uint16_t value = 0xffff;
    for (auto b : frame) {
        value ^= b;
        for (int bit = 0; bit < 8; ++bit)
            value = (value & 1) ? (value >> 1) ^ 0xa001 : value >> 1;
    }
    return value;
}

class Serial final : public edge_gateway::ISerialPort {
public:
    void open() override { opened = true; }
    void close() override { opened = false; }
    bool isOpen() const override { return opened; }
    void write(const std::vector<std::uint8_t>& bytes) override {
        frames.push_back(bytes);
        if (drop) return;
        reply = bytes;
        if (wrongEcho) {
            reply[3] ^= 1;
            reply.resize(6);
            auto sum = crc(reply);
            reply.push_back(sum & 0xff);
            reply.push_back(sum >> 8);
        }
    }
    std::vector<std::uint8_t> read(std::size_t maxBytes, int) override {
        auto n = std::min(maxBytes, reply.size());
        std::vector<std::uint8_t> result(reply.begin(), reply.begin() + n);
        reply.erase(reply.begin(), reply.begin() + n);
        return result;
    }
    bool opened = false, drop = false, wrongEcho = false;
    std::vector<std::vector<std::uint8_t>> frames;
    std::vector<std::uint8_t> reply;
};
}

int main(int argc, char** argv) {
    using namespace edge_gateway;
    require(argc == 3, "usage: hgm6110nc_control_test device.json project");
    auto config = ConfigLoader::loadFromFile(argv[1]);
    const auto project = ScadaProjectLoader::loadFromDirectory(argv[2]);
    require(project.screens.size() == 55, "project load");
    require(config.meters.size() == 1, "one generator");
    config.machineCode = "HGM_ISOLATED_TEST";
    config.meterCode = config.meters.front().meterCode;
    config.points = config.meters.front().points;
    config.protocol.slave = 1;
    auto serial = std::make_shared<Serial>();
    SerialPortOptions options;
    options.timeoutMs = 10;
    options.frameIntervalMs = 0;
    options.readRetryCount = 3;
    auto client = std::make_shared<ModbusRtuClient>(serial, options);
    MemoryPointStore store("hgm_control_test_" + std::to_string(getpid()), MemoryStoreOpenMode::CreateExclusive);
    CommandExecutor executor(config, store, client, nullptr);
    require(serial->frames.empty(), "construction must not write");
    std::size_t count = 0;
    for (const auto& point : config.points) {
        if (!point.write.enable) continue;
        ++count;
        require(point.pointCode.find("gen_cmd_") == 0 && point.address >= 0 && point.address <= 6, "command scope");
        require(!point.read.enable && !point.write.verifyAfterWrite && !point.write.verifyByRead, "no same-address readback");
        require(!point.write.startupValue, "no startup command");
        const auto before = serial->frames.size();
        for (double value : {0.0, -1.0, 2.0, std::numeric_limits<double>::quiet_NaN()}) {
            const auto result = executor.executeByIndex("bad", point.index, value, 1000);
            require(!result.success && serial->frames.size() == before, "invalid command must not hit wire");
        }
        const auto ok = executor.executeByIndex("valid", point.index, 1, 1000);
        require(ok.success && !ok.verifyAttempted, "protocol ack only");
        require(serial->frames.size() == before + 1, "exactly one request");
        std::vector<std::uint8_t> expected{1, 5, 0, static_cast<std::uint8_t>(point.address), 0xff, 0};
        const auto sum = crc(expected);
        expected.push_back(sum & 0xff); expected.push_back(sum >> 8);
        require(serial->frames.back() == expected, "FC05 true frame");
        if (point.address == 3) require(expected == std::vector<std::uint8_t>({1,5,0,3,0xff,0,0x7c,0x3a}), "vendor auto example");
        serial->drop = true;
        auto result = executor.executeByIndex("timeout", point.index, 1, 1000);
        require(!result.success && serial->frames.size() == before + 2, "no retry on lost acknowledgement");
        require(result.outcomeUnknown && !result.feedbackMismatch, "lost acknowledgement is unknown, not definite failure");
        serial->drop = false; serial->wrongEcho = true;
        result = executor.executeByIndex("wrong-echo", point.index, 1, 1000);
        require(!result.success && serial->frames.size() == before + 3, "wrong echo rejected without retry");
        require(result.outcomeUnknown && !result.feedbackMismatch, "wrong echo cannot prove the command was not executed");
        serial->wrongEcho = false;
    }
    require(count == 7, "exactly seven write-only controls");
    std::cout << "7 controls: exact FF00 frames, no startup/reset/retry, invalid values and wrong echo rejected; no hardware accessed\n";
}
