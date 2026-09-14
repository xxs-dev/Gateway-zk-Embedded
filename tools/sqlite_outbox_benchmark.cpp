#include "edge_gateway/mqtt_event_outbox.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/resource.h>
#include <unistd.h>

using Outbox = edge_gateway::MqttEventOutbox;
using Clock = std::chrono::steady_clock;

namespace {
long long ioBytes(const char* key) {
    std::ifstream input("/proc/self/io");
    std::string field;
    long long value;
    while (input >> field >> value) if (field == key) return value;
    return -1;
}
long long memoryHighWaterKiB() {
    std::ifstream input("/proc/self/status");
    std::string line;
    while (std::getline(input, line))
        if (line.compare(0, 6, "VmHWM:") == 0) return std::stoll(line.substr(6));
    return -1;
}
double seconds(const timeval& value) { return value.tv_sec + value.tv_usec / 1e6; }
double milliseconds(Clock::time_point since) {
    return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
}
Outbox::StorageProfile profile(const std::string& value) {
    if (value == "delete-normal") return Outbox::StorageProfile::DeleteNormal;
    if (value == "delete-full") return Outbox::StorageProfile::DeleteFull;
    if (value == "wal-normal") return Outbox::StorageProfile::WalNormal;
    if (value == "wal-full") return Outbox::StorageProfile::WalFull;
    throw std::runtime_error("invalid profile");
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 9) throw std::runtime_error(
            "usage: benchmark LIB DB PROFILE ROLE OUTPUT SECONDS GAP_MS TAG");
        const std::string db = argv[2], role = argv[4], output = argv[5], tag = argv[8];
        const auto duration = std::stod(argv[6]);
        const auto gap = std::chrono::milliseconds(std::stoi(argv[7]));
        const char* replayPausePath = std::getenv("SQLITE_BENCHMARK_REPLAY_PAUSE");
        const auto initialization = Clock::now();
        std::unique_ptr<Outbox> box(new Outbox(db, argv[1], 12, 24, 64, 0, profile(argv[3])));
        const auto settings = box->storageSettings();
        const double initMs = milliseconds(initialization);
        std::ofstream(output + ".ready") << "ready\n";
        const auto barrierDeadline = Clock::now() + std::chrono::seconds(60);
        while (role != "init" && role != "drain" && access((db + ".go").c_str(), F_OK) != 0) {
            if (Clock::now() > barrierDeadline) throw std::runtime_error("start barrier timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        rusage before{};
        getrusage(RUSAGE_SELF, &before);
        const auto ioBefore = ioBytes("write_bytes:");
        const auto readBefore = ioBytes("read_bytes:");
        const auto begin = Clock::now();
        const auto deadline = begin + std::chrono::milliseconds(static_cast<int>(duration * 1000));
        auto next = begin;
        std::vector<double> latency;
        std::size_t operations = 0, events = 0, errors = 0, sends = 0, sequence = 0;
        std::vector<double> errorLatency;
        double pauseWaitMs = 0;
        while (role != "init" && Clock::now() < deadline) {
            if (role == "replay" && replayPausePath && access(replayPausePath, F_OK) == 0) {
                const auto pausedAt = Clock::now();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                pauseWaitMs += milliseconds(pausedAt);
                continue;
            }
            const auto opStart = Clock::now();
            try {
                if (role == "producer") {
                    std::vector<Outbox::EventMessage> batch;
                    std::vector<Outbox::EventState> states;
                    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    for (int i = 0; i < 16; ++i) {
                        Outbox::EventMessage event;
                        event.eventId = tag + ":" + std::to_string(sequence++);
                        event.eventType = i % 8 == 0 ? "alarm" : "change";
                        event.topic = "benchmark/" + tag;
                        event.payload = std::string(1024, 'x');
                        event.eventTs = stamp;
                        batch.push_back(event);
                        Outbox::EventState state;
                        state.stateKey = tag + ":" + std::to_string(i);
                        state.eventType = event.eventType;
                        state.index = i + 1;
                        state.value = static_cast<double>(sequence);
                        state.sourceTs = stamp;
                        states.push_back(state);
                    }
                    const auto ids = box->enqueueBatchWithStates(batch, states);
                    if (ids.size() != batch.size()) throw std::runtime_error("incomplete enqueue");
                    events += ids.size();
                } else if (role == "replay" || role == "drain") {
                    const auto result = box->replayBatchWithStats("main", {}, 0, 64,
                        [&](const std::vector<Outbox::ReplayMessage>& messages) {
                            if (role == "replay")
                                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                            sends += messages.size();
                        });
                    events += result.count;
                    if (role == "drain" && result.count == 0 && box->pendingCount() == 0) break;
                    if (result.count == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                } else if (role == "reader") {
                    (void)box->pendingCount();
                } else throw std::runtime_error("invalid role");
                ++operations;
                latency.push_back(milliseconds(opStart));
            } catch (const std::exception& ex) {
                ++errors;
                errorLatency.push_back(milliseconds(opStart));
                if (errors <= 5) std::cerr << role << ": " << ex.what() << '\n';
            }
            if (gap.count() > 0) {
                // No catch-up burst: overdue batches are reported through actual event throughput.
                next = std::max(next + gap, Clock::now());
                std::this_thread::sleep_until(next);
            }
        }
        const double elapsed = milliseconds(begin);
        const auto closeStart = Clock::now();
        box.reset();
        const double closeMs = milliseconds(closeStart);
        rusage after{};
        getrusage(RUSAGE_SELF, &after);
        std::sort(latency.begin(), latency.end());
        const auto pct = [&](double fraction) {
            return latency.empty() ? 0.0 : latency[std::min(latency.size() - 1,
                static_cast<std::size_t>(fraction * (latency.size() - 1)))];
        };
        std::ofstream out(output);
        out << "{\"role\":\"" << role << "\",\"version\":\"" << settings.sqliteVersion
            << "\",\"journal\":\"" << settings.journalMode << "\",\"synchronous\":" << settings.synchronous
            << ",\"busyTimeoutMs\":" << settings.busyTimeoutMs
            << ",\"walAutoCheckpointPages\":" << settings.walAutoCheckpointPages
            << ",\"initMs\":" << initMs << ",\"closeMs\":" << closeMs
            << ",\"elapsedMs\":" << elapsed << ",\"operations\":" << operations
            << ",\"pauseWaitMs\":" << pauseWaitMs
            << ",\"events\":" << events << ",\"sends\":" << sends << ",\"errors\":" << errors
            << ",\"p50Ms\":" << pct(.50) << ",\"p95Ms\":" << pct(.95)
            << ",\"p99Ms\":" << pct(.99) << ",\"maxMs\":" << pct(1.0)
            << ",\"cpuSeconds\":" << seconds(after.ru_utime) + seconds(after.ru_stime)
                - seconds(before.ru_utime) - seconds(before.ru_stime)
            << ",\"rssMaxKiB\":" << memoryHighWaterKiB()
            << ",\"rssMeasurement\":\"proc-status-VmHWM\""
            << ",\"ioAccountingAvailable\":" << (ioBefore >= 0 ? "true" : "false")
            << ",\"writeBytes\":" << (ioBefore >= 0 ? ioBytes("write_bytes:") - ioBefore : -1)
            << ",\"readBytes\":" << (readBefore >= 0 ? ioBytes("read_bytes:") - readBefore : -1)
            << ",\"latencyMs\":[";
        for (std::size_t i = 0; i < latency.size(); ++i) out << (i ? "," : "") << latency[i];
        out << "],\"errorLatencyMs\":[";
        for (std::size_t i = 0; i < errorLatency.size(); ++i) out << (i ? "," : "") << errorLatency[i];
        out << "]}\n";
        return errors == 0 ? 0 : 2;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
