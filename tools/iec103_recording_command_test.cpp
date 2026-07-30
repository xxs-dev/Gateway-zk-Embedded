#include "edge_gateway/iec103_recording_command.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

void requireTrue(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

#ifndef _WIN32

class FakeRecordingClient final : public edge_gateway::IecClient {
public:
    std::vector<edge_gateway::IecDataValue> poll() override {
        return {};
    }

    std::vector<edge_gateway::Iec103DisturbanceRecord> listDisturbanceRecords(int) override {
        edge_gateway::Iec103DisturbanceRecord record;
        record.fan = 15;
        record.state = 1;
        record.timestampMs = 1785315600000LL;
        record.rawTimeHex = "010201011A";
        return {record};
    }

    edge_gateway::Iec103ComtradeFiles pullComtradeRecording(
        int fan,
        const std::string& outputDirectory,
        int
    ) override {
        if (progress_) {
            progress_("pulling_cfg");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (progress_) {
            progress_("pulling_dat");
        }
        const auto cfg = outputDirectory + "/FAN00015.CFG";
        const auto dat = outputDirectory + "/FAN00015.DAT";
        std::ofstream(cfg, std::ios::binary) << "CFG";
        std::ofstream(dat, std::ios::binary) << "DAT";
        edge_gateway::Iec103ComtradeFiles files;
        files.fan = fan;
        files.cfgPath = cfg;
        files.datPath = dat;
        files.cfgBytes = 3;
        files.datBytes = 3;
        return files;
    }

    void setRecordingProgressCallback(
        std::function<void(const std::string&)> callback
    ) override {
        progress_ = std::move(callback);
    }

private:
    std::function<void(const std::string&)> progress_;
};

edge_gateway::Iec103RecordingJobStatus waitForTerminal(
    const std::string& socketPath,
    const std::string& requestId
) {
    for (int i = 0; i < 100; ++i) {
        const auto status = edge_gateway::Iec103RecordingCommandClient::status(socketPath, requestId);
        if (status.terminal()) {
            return status;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("recording command did not reach a terminal state");
}

#endif

}  // namespace

int main() {
#ifdef _WIN32
    std::cout << "IEC103 recording command test skipped on Windows" << std::endl;
    return EXIT_SUCCESS;
#else
    using namespace edge_gateway;

    const auto base = std::string("/tmp/iec103-recording-command-") + std::to_string(getpid());
    mkdir(base.c_str(), 0750);
    const auto socketPath = base + "/driver.sock";
    const auto client = std::make_shared<FakeRecordingClient>();
    Iec103RecordingCommandServer server(client, socketPath, base, 1000);
    server.start();

    const auto acceptedList = Iec103RecordingCommandClient::submitList(socketPath, "LIST_001");
    requireTrue(acceptedList.stage == "accepted", "list command must be accepted asynchronously");
    const auto list = waitForTerminal(socketPath, "LIST_001");
    requireTrue(list.stage == "completed", "list command terminal stage");
    requireTrue(list.records.size() == 1 && list.records.front().fan == 15,
        "list command recording result");

    const auto duplicate = Iec103RecordingCommandClient::submitList(socketPath, "LIST_001");
    requireTrue(duplicate.stage == "completed" && duplicate.records.size() == 1,
        "duplicate list request must be idempotent");

    const auto acceptedPull = Iec103RecordingCommandClient::submitPull(socketPath, "PULL_001", 15);
    requireTrue(acceptedPull.stage == "accepted", "pull command must be accepted asynchronously");
    const auto pull = waitForTerminal(socketPath, "PULL_001");
    requireTrue(pull.stage == "local_ready", "pull command terminal stage");
    requireTrue(pull.files.cfgBytes == 3 && pull.files.datBytes == 3,
        "pull command COMTRADE sizes");

    const auto conflict = Iec103RecordingCommandClient::submitPull(socketPath, "LIST_001", 15);
    requireTrue(conflict.stage == "failed" && conflict.errorCode == "REQUEST_ID_CONFLICT",
        "requestId conflict must be rejected");
    const auto missing = Iec103RecordingCommandClient::status(socketPath, "MISSING");
    requireTrue(missing.stage == "failed" && missing.errorCode == "REQUEST_NOT_FOUND",
        "missing status must be explicit");

    server.stop();
    requireTrue(access(socketPath.c_str(), F_OK) != 0, "command socket must be removed on stop");
    std::remove((base + "/FAN00015.CFG").c_str());
    std::remove((base + "/FAN00015.DAT").c_str());
    rmdir(base.c_str());

    requireTrue(
        defaultIec103RecordingSocketPath("/opt/modbus-gateway/config/runtime/devices/device_am5se_t.json") ==
            "/run/modbus-gateway/iec103/device_am5se_t.sock",
        "default command socket path");
    std::cout << "IEC103 recording command test passed" << std::endl;
    return EXIT_SUCCESS;
#endif
}
